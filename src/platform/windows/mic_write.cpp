/**
 * @file src/platform/windows/mic_write.cpp
 * @brief Plays audio into the Steam Streaming Microphone and manages the default capture device.
 *
 * Device lookup, endpoint format normalisation, and the event-driven render loop are adapted
 * from Apollo pull request #1428 (logabell/apollo-microphone), licensed GPL-3.0.
 * Design evidence: docs/superpowers/research/2026-09-20-windows-virtual-mic.md and
 * docs/superpowers/research/2026-09-20-voice-playout.md.
 */
// standard includes
#include <algorithm>
#include <atomic>
#include <cwctype>
#include <optional>
#include <thread>
#include <vector>

// platform includes
#include <Audioclient.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <newdev.h>
#include <synchapi.h>

// local includes
#include "misc.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"

// Must be the last included file
// clang-format off
#include "PolicyConfig.h"
// clang-format on

using namespace std::literals;

namespace platf {
  namespace {
    constexpr PROPERTYKEY MIC_PKEY_DEVICE_DESC {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 2};
    constexpr PROPERTYKEY MIC_PKEY_FRIENDLY_NAME {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
    constexpr PROPERTYKEY MIC_PKEY_INTERFACE_NAME {{0x026e516e, 0xb814, 0x414b, {0x83, 0xcd, 0x85, 0x6d, 0x6f, 0xef, 0x48, 0x22}}, 2};

    constexpr DWORD SAMPLE_RATE = 48000;
    // 100 ms underruns at a 20 ms packet cadence (moonlight-mic raised its buffer to 200 ms for this).
    constexpr REFERENCE_TIME BUFFER_DURATION_100NS = 2000000;
    // Request packets only while the device holds less than 40 ms, so the jitter buffer holds the delay.
    constexpr UINT32 TARGET_QUEUED_FRAMES = SAMPLE_RATE / 25;
    // The device description and the interface name come from the driver INF and are not localised.
    constexpr auto DEVICE_NAME_PATTERN = L"steam streaming microphone";

#if defined(__x86_64) || defined(__x86_64__) || defined(__amd64) || defined(__amd64__) || defined(_M_AMD64)
    constexpr auto STEAM_MIC_DRIVER_PATH = L"%CommonProgramFiles(x86)%\\Steam\\drivers\\Windows10\\x64\\SteamStreamingMicrophone.inf";
#else
    constexpr auto STEAM_MIC_DRIVER_PATH = L"";
#endif

    template<class T>
    void release_com(T *pointer) {
      if (pointer) {
        pointer->Release();
      }
    }

    template<class T>
    void co_task_free(T *pointer) {
      if (pointer) {
        CoTaskMemFree(pointer);
      }
    }

    using device_enum_t = util::safe_ptr<IMMDeviceEnumerator, release_com<IMMDeviceEnumerator>>;
    using device_t = util::safe_ptr<IMMDevice, release_com<IMMDevice>>;
    using collection_t = util::safe_ptr<IMMDeviceCollection, release_com<IMMDeviceCollection>>;
    using prop_t = util::safe_ptr<IPropertyStore, release_com<IPropertyStore>>;
    using policy_t = util::safe_ptr<IPolicyConfig, release_com<IPolicyConfig>>;
    using audio_client_t = util::safe_ptr<IAudioClient, release_com<IAudioClient>>;
    using render_client_t = util::safe_ptr<IAudioRenderClient, release_com<IAudioRenderClient>>;
    using wstring_t = util::safe_ptr<WCHAR, co_task_free<WCHAR>>;
    using wave_format_t = util::safe_ptr<WAVEFORMATEX, co_task_free<WAVEFORMATEX>>;

    std::wstring lower(std::wstring text) {
      std::transform(text.begin(), text.end(), text.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
      });
      return text;
    }

    std::wstring prop_string(IPropertyStore *store, const PROPERTYKEY &key) {
      PROPVARIANT value;
      PropVariantInit(&value);
      std::wstring text;
      if (SUCCEEDED(store->GetValue(key, &value)) && value.vt == VT_LPWSTR && value.pwszVal) {
        text = value.pwszVal;
      }
      PropVariantClear(&value);
      return text;
    }

    device_enum_t create_enumerator() {
      device_enum_t device_enum;
      CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, IID_IMMDeviceEnumerator, (void **) &device_enum);
      return device_enum;
    }

    policy_t create_policy() {
      policy_t policy;
      CoCreateInstance(CLSID_CPolicyConfigClient, nullptr, CLSCTX_ALL, IID_IPolicyConfig, (void **) &policy);
      return policy;
    }

    /**
     * @brief Find the active Steam Streaming Microphone endpoint for one data flow.
     *
     * Render and capture are looked up separately. A match on the name alone once made
     * another implementation write to the wrong endpoint.
     */
    std::optional<std::wstring> find_steam_endpoint(IMMDeviceEnumerator *device_enum, EDataFlow flow) {
      collection_t collection;
      if (FAILED(device_enum->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection)) || !collection) {
        return std::nullopt;
      }

      UINT count = 0;
      collection->GetCount(&count);
      for (UINT index = 0; index < count; ++index) {
        device_t device;
        wstring_t id;
        prop_t props;
        if (FAILED(collection->Item(index, &device)) || !device ||
            FAILED(device->GetId(&id)) || !id ||
            FAILED(device->OpenPropertyStore(STGM_READ, &props)) || !props) {
          continue;
        }

        for (const auto &key : {MIC_PKEY_FRIENDLY_NAME, MIC_PKEY_INTERFACE_NAME, MIC_PKEY_DEVICE_DESC}) {
          if (lower(prop_string(props.get(), key)).find(DEVICE_NAME_PATTERN) != std::wstring::npos) {
            return std::wstring {id.get()};
          }
        }
      }
      return std::nullopt;
    }

    /**
     * @brief Install the Steam Streaming Microphone driver from the Steam folder.
     */
    bool install_steam_mic_driver() {
      if (!config::audio.install_steam_drivers) {
        return false;
      }

      // MinGW's libnewdev.a is missing DiInstallDriverW(), so it is loaded at runtime.
      auto newdev = LoadLibraryExW(L"newdev.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
      if (!newdev) {
        return false;
      }
      auto fg = util::fail_guard([newdev]() {
        FreeLibrary(newdev);
      });

      auto fn_DiInstallDriverW = (decltype(DiInstallDriverW) *) GetProcAddress(newdev, "DiInstallDriverW");
      if (!fn_DiInstallDriverW) {
        return false;
      }

      WCHAR driver_path[MAX_PATH] = {};
      ExpandEnvironmentStringsW(STEAM_MIC_DRIVER_PATH, driver_path, ARRAYSIZE(driver_path));
      if (!fn_DiInstallDriverW(nullptr, driver_path, 0, nullptr)) {
        auto code = GetLastError();
        switch (code) {
          case ERROR_ACCESS_DENIED:
            BOOST_LOG(warning) << "Remote microphone: administrator privileges are required to install the Steam Streaming Microphone"sv;
            break;
          case ERROR_FILE_NOT_FOUND:
          case ERROR_PATH_NOT_FOUND:
            BOOST_LOG(info) << "Remote microphone: the Steam audio drivers were not found. Steam is not installed on this PC."sv;
            break;
          default:
            BOOST_LOG(warning) << "Remote microphone: could not install the Steam Streaming Microphone driver: "sv << code;
            break;
        }
        return false;
      }

      BOOST_LOG(info) << "Remote microphone: installed the Steam Streaming Microphone driver"sv;
      // The audio subsystem needs time to publish the new endpoints.
      Sleep(5000);
      return true;
    }

    WAVEFORMATEXTENSIBLE make_format(bool ieee_float) {
      WAVEFORMATEXTENSIBLE format {};
      format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
      format.Format.nChannels = 2;
      format.Format.nSamplesPerSec = SAMPLE_RATE;
      format.Format.wBitsPerSample = 32;
      format.Format.nBlockAlign = static_cast<WORD>(format.Format.nChannels * (format.Format.wBitsPerSample / 8));
      format.Format.nAvgBytesPerSec = format.Format.nSamplesPerSec * format.Format.nBlockAlign;
      format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
      format.Samples.wValidBitsPerSample = 32;
      format.SubFormat = ieee_float ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;
      format.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
      return format;
    }

    bool is_normalised(const WAVEFORMATEX *format) {
      return format && format->nChannels == 2 && format->nSamplesPerSec == SAMPLE_RATE && format->wBitsPerSample == 32;
    }

    /**
     * @brief Set one endpoint to 2 channels, 32-bit PCM, 48000 Hz.
     *
     * The Steam driver copies render to capture with no conversion, so both endpoints need the
     * same format. The device format must be PCM: the float subtype makes Initialize fail with
     * 0x88890008.
     */
    void normalise_endpoint_format(IPolicyConfig *policy, const std::wstring &device_id) {
      wave_format_t current;
      if (SUCCEEDED(policy->GetDeviceFormat(device_id.c_str(), FALSE, &current)) && is_normalised(current.get())) {
        return;
      }

      auto wanted = make_format(false);
      WAVEFORMATEXTENSIBLE previous {};
      auto status = policy->SetDeviceFormat(device_id.c_str(), &wanted.Format, &previous.Format);
      if (FAILED(status)) {
        BOOST_LOG(warning) << "Remote microphone: could not set the Steam Streaming Microphone format: 0x"sv << util::hex(status).to_string_view();
      }
    }

    std::wstring default_capture_id(IMMDeviceEnumerator *device_enum) {
      device_t device;
      wstring_t id;
      if (FAILED(device_enum->GetDefaultAudioEndpoint(eCapture, eConsole, &device)) || !device ||
          FAILED(device->GetId(&id)) || !id) {
        return {};
      }
      return id.get();
    }

    void set_default_capture(const std::wstring &device_id) {
      auto policy = create_policy();
      if (!policy || device_id.empty()) {
        return;
      }
      for (int role = 0; role < (int) ERole_enum_count; ++role) {
        policy->SetDefaultEndpoint(device_id.c_str(), (ERole) role);
      }
    }

    bool is_recoverable(HRESULT status) {
      return status == AUDCLNT_E_DEVICE_INVALIDATED ||
             status == AUDCLNT_E_RESOURCES_INVALIDATED ||
             status == AUDCLNT_E_SERVICE_NOT_RUNNING;
    }

    class wasapi_virtual_mic_t: public virtual_mic_t {
    public:
      explicit wasapi_virtual_mic_t(virtual_mic_fill_t fill):
          fill {std::move(fill)} {
      }

      ~wasapi_virtual_mic_t() override {
        stop = true;
        if (render_thread.joinable()) {
          render_thread.join();
        }
        close_client();
        if (!previous_default.empty()) {
          set_default_capture(previous_default);
        }
        if (render_event) {
          CloseHandle(render_event);
        }
        device_enum.reset();
        if (com_initialized) {
          CoUninitialize();
        }
      }

      bool init(virtual_mic_error_e &error_out) {
        com_initialized = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_SPEED_OVER_MEMORY));

        device_enum = create_enumerator();
        if (!device_enum) {
          error_out = virtual_mic_error_e::device_open_failed;
          return false;
        }

        auto found_render = find_steam_endpoint(device_enum.get(), eRender);
        if (!found_render && install_steam_mic_driver()) {
          found_render = find_steam_endpoint(device_enum.get(), eRender);
        }
        auto found_capture = find_steam_endpoint(device_enum.get(), eCapture);
        if (!found_render || !found_capture) {
          BOOST_LOG(warning) << "Remote microphone: the Steam Streaming Microphone was not found. Install Steam on this PC."sv;
          error_out = virtual_mic_error_e::device_missing;
          return false;
        }
        render_id = *found_render;

        if (auto policy = create_policy()) {
          normalise_endpoint_format(policy.get(), render_id);
          normalise_endpoint_format(policy.get(), *found_capture);
        }

        render_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!render_event || !open_client()) {
          error_out = virtual_mic_error_e::device_open_failed;
          return false;
        }

        previous_default = default_capture_id(device_enum.get());
        if (previous_default == *found_capture) {
          // A stale switch is already in place. Keeping it as "previous" would make the restore a no-op forever.
          previous_default.clear();
        }
        set_default_capture(*found_capture);

        render_thread = std::thread {[this]() {
          render_loop();
        }};
        return true;
      }

      bool healthy() const override {
        return !failed;
      }

      std::string previous_default_capture() const override {
        return to_utf8(previous_default);
      }

    private:
      /**
       * @brief Activate, initialise, prime, and start the render client. Runs on init and on recovery.
       */
      bool open_client() {
        device_t device;
        if (FAILED(device_enum->GetDevice(render_id.c_str(), &device)) || !device ||
            FAILED(device->Activate(IID_IAudioClient, CLSCTX_ALL, nullptr, (void **) &audio_client)) || !audio_client) {
          return false;
        }

        auto format = make_format(true);
        auto status = audio_client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, BUFFER_DURATION_100NS, 0, &format.Format, nullptr);
        if (FAILED(status)) {
          BOOST_LOG(error) << "Remote microphone: could not initialize the Steam Streaming Microphone: 0x"sv << util::hex(status).to_string_view();
          return false;
        }

        if (FAILED(audio_client->GetBufferSize(&buffer_frames)) ||
            FAILED(audio_client->GetService(IID_IAudioRenderClient, (void **) &render_client)) || !render_client ||
            FAILED(audio_client->SetEventHandle(render_event))) {
          return false;
        }

        // A cold buffer glitches at the start of the stream, so half of it is primed with silence.
        BYTE *buffer = nullptr;
        if (SUCCEEDED(render_client->GetBuffer(buffer_frames / 2, &buffer)) && buffer) {
          render_client->ReleaseBuffer(buffer_frames / 2, AUDCLNT_BUFFERFLAGS_SILENT);
        }

        return SUCCEEDED(audio_client->Start());
      }

      void close_client() {
        if (audio_client) {
          audio_client->Stop();
        }
        render_client.reset();
        audio_client.reset();
      }

      /**
       * @brief Reopen the client after a recoverable error. Latches failed for every other error.
       */
      bool recover(HRESULT status) {
        if (!is_recoverable(status)) {
          failed = true;
          return false;
        }

        BOOST_LOG(warning) << "Remote microphone: reopening the Steam Streaming Microphone after 0x"sv << util::hex(status).to_string_view();
        close_client();
        Sleep(500);
        if (!open_client()) {
          failed = true;
          return false;
        }
        return true;
      }

      void render_loop() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_SPEED_OVER_MEMORY);
        platf::adjust_thread_priority(platf::thread_priority_e::high);

        std::vector<float> packet(VIRTUAL_MIC_MAX_PACKET_SAMPLES);
        std::vector<float> pending;  // decoded samples that did not fit in the device yet

        while (!stop) {
          WaitForSingleObject(render_event, 20);
          if (stop) {
            break;
          }

          UINT32 padding = 0;
          auto status = audio_client->GetCurrentPadding(&padding);
          if (FAILED(status)) {
            if (!recover(status)) {
              break;
            }
            pending.clear();  // stale audio must not replay into the reopened client
            continue;
          }

          while (padding + pending.size() < TARGET_QUEUED_FRAMES) {
            auto samples = fill(packet.data(), packet.size());
            if (samples == 0) {
              break;
            }
            for (std::size_t index = 0; index < samples; ++index) {
              pending.push_back(std::clamp(packet[index], -1.0f, 1.0f));
            }
          }

          auto frames = std::min<UINT32>(buffer_frames - padding, static_cast<UINT32>(pending.size()));
          if (frames == 0) {
            continue;
          }

          BYTE *buffer = nullptr;
          status = render_client->GetBuffer(frames, &buffer);
          if (FAILED(status) || !buffer) {
            if (!recover(status)) {
              break;
            }
            pending.clear();
            continue;
          }

          auto *out = reinterpret_cast<float *>(buffer);
          for (UINT32 frame = 0; frame < frames; ++frame) {
            out[frame * 2] = pending[frame];
            out[frame * 2 + 1] = pending[frame];
          }
          pending.erase(pending.begin(), pending.begin() + frames);

          status = render_client->ReleaseBuffer(frames, 0);
          if (FAILED(status) && !recover(status)) {
            break;
          }
        }

        CoUninitialize();
      }

      virtual_mic_fill_t fill;
      device_enum_t device_enum;
      audio_client_t audio_client;
      render_client_t render_client;
      HANDLE render_event = nullptr;
      UINT32 buffer_frames = 0;
      std::wstring render_id;
      std::wstring previous_default;
      bool com_initialized = false;

      std::thread render_thread;
      std::atomic<bool> stop {false};
      std::atomic<bool> failed {false};
    };
  }  // namespace

  std::unique_ptr<virtual_mic_t> virtual_mic(virtual_mic_fill_t fill, virtual_mic_error_e &error_out) {
    error_out = virtual_mic_error_e::none;
    auto mic = std::make_unique<wasapi_virtual_mic_t>(std::move(fill));
    if (!mic->init(error_out)) {
      return nullptr;
    }
    return mic;
  }

  void restore_default_capture(const std::string &device_id) {
    bool com_initialized = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_SPEED_OVER_MEMORY));
    set_default_capture(from_utf8(device_id));
    if (com_initialized) {
      CoUninitialize();
    }
  }
}  // namespace platf
