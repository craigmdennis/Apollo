/**
 * @file src/mic.cpp
 * @brief Definitions for the remote microphone module.
 *
 * This module never touches streaming code. Every failure ends the mic session only.
 *
 * Playout is pull-based: the virtual microphone's render thread calls fill_from_session()
 * when the device has room, so the audio device clock is the only playout clock.
 * Design evidence: docs/superpowers/research/2026-09-20-voice-playout.md.
 */
// standard includes
#include <array>
#include <atomic>
#include <cstring>
#include <future>
#include <mutex>
#include <thread>

// lib includes
#include <boost/asio.hpp>
#include <opus/opus.h>

// local includes
#include "config.h"
#include "file_handler.h"
#include "logging.h"
#include "mic.h"
#include "mic_jitter.h"
#include "mic_protocol.h"
#include "network.h"
#include "platform/common.h"
#include "system_tray.h"
#include "uuid.h"

using namespace std::literals;

namespace mic {
  namespace {
    namespace asio = boost::asio;
    using asio::ip::udp;
    using steady = std::chrono::steady_clock;

    constexpr int MIC_PORT_OFFSET = 13;
    constexpr int FRAME_SAMPLES = 960;  // 20 ms at 48 kHz, the duration concealed for one lost frame
    constexpr auto HOUSEKEEPING = 1s;
    constexpr auto SESSION_TIMEOUT = 5s;
    constexpr auto REPLACED_LIFETIME = 5s;
    constexpr int MAX_DECODE_FAILURES = 25;  // 500 ms of consecutive failures

    struct session_t {
      std::uint32_t id;
      crypto::cipher::gcm_t cipher;
      device_t device;
      steady::time_point last_valid;
      std::atomic<protocol::error_e> error {protocol::error_e::none};

      // Shared by the mic thread (push) and the render thread (pop).
      std::mutex jitter_mutex;
      jitter_buffer_t jitter;

      // Touched only by the render thread, through fill_from_session().
      OpusDecoder *decoder = nullptr;
      bool waiting = true;
      int decode_failures = 0;

      std::unique_ptr<platf::virtual_mic_t> vmic;
    };

    struct replaced_t {
      std::uint32_t id;
      crypto::cipher::gcm_t cipher;
      steady::time_point expires;
    };

    // Guarded by state_mutex. Read by config server handlers.
    std::mutex state_mutex;
    std::unique_ptr<store_t> store;
    pairing_t pairing;
    crypto::sha256_t fingerprint {};
    std::string connected_uuid;

    // Owned by the mic thread.
    std::unique_ptr<asio::io_context> io;
    std::unique_ptr<udp::socket> socket;
    std::unique_ptr<asio::steady_timer> timer;
    std::thread thread;
    std::atomic<bool> running {false};
    std::unique_ptr<session_t> session;
    std::optional<replaced_t> replaced;
    std::array<char, 2048> receive_buffer;
    udp::endpoint sender;

    void notify_connected(const std::string &name) {
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
      system_tray::update_tray_mic_connected(name);
#endif
    }

    void notify_disconnected(const std::string &name, const std::string &reason) {
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
      system_tray::update_tray_mic_disconnected(name, reason);
#endif
    }

    void notify_error(protocol::error_e error_code) {
      std::string text;
      switch (error_code) {
        case protocol::error_e::device_missing:
          text = "Apollo cannot find the Steam Streaming Microphone. Install Steam on the PC, then tap Connect.";
          break;
        case protocol::error_e::device_open_failed:
          text = "Another program on the PC is blocking the Steam Streaming Microphone. Close it, then tap Connect.";
          break;
        case protocol::error_e::decode_failed:
          text = "Apollo cannot decode the audio from this device. Tap Disconnect, then tap Connect.";
          break;
        default:
          return;
      }
      BOOST_LOG(warning) << "Remote microphone: "sv << text;
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
      system_tray::update_tray_mic_error(text);
#endif
    }

    /**
     * @brief Decode the next packet for the virtual microphone. Runs on the render thread.
     * @return The number of samples written to out, or 0 when nothing is ready to play.
     */
    std::size_t fill_from_session(session_t &target, float *out, std::size_t capacity) {
      jitter_buffer_t::pop_result_t next;
      {
        std::lock_guard lock {target.jitter_mutex};
        next = target.jitter.pop();
      }

      if (next.kind == jitter_buffer_t::pop_e::wait) {
        target.waiting = true;
        return 0;
      }
      if (target.waiting) {
        // The sender was muted or silent. Decoding against the state of the last talkspurt
        // blends two unrelated signals, so the decoder starts clean.
        opus_decoder_ctl(target.decoder, OPUS_RESET_STATE);
        target.waiting = false;
      }

      int samples;
      if (next.kind == jitter_buffer_t::pop_e::frame) {
        // capacity covers a 120 ms packet. A 960-sample buffer rejects anything longer than 20 ms.
        samples = opus_decode_float(target.decoder, next.payload.data(), static_cast<opus_int32>(next.payload.size()), out, static_cast<int>(capacity), 0);
      } else {
        // A null payload asks Opus to conceal one lost frame of FRAME_SAMPLES.
        samples = opus_decode_float(target.decoder, nullptr, 0, out, FRAME_SAMPLES, 0);
      }

      if (samples < 0) {
        if (++target.decode_failures == MAX_DECODE_FAILURES) {
          target.error = protocol::error_e::decode_failed;
          notify_error(protocol::error_e::decode_failed);
        }
        return 0;
      }

      target.decode_failures = 0;
      return static_cast<std::size_t>(samples);
    }

    /**
     * @brief End the mic session on the mic thread.
     */
    void end_session(const std::string &reason, bool notify) {
      if (!session) {
        return;
      }

      auto name = session->device.name;
      // Order matters. Destroying the virtual microphone joins the render thread, which is the
      // only user of the decoder. It also restores the default capture device.
      session->vmic.reset();
      if (session->decoder) {
        opus_decoder_destroy(session->decoder);
      }
      session.reset();

      {
        std::lock_guard lock {state_mutex};
        connected_uuid.clear();
        store->previous_default_capture.clear();
        store->save();
      }

      BOOST_LOG(info) << "Remote microphone: session ended for ["sv << name << "] "sv << reason;
      if (notify) {
        notify_disconnected(name, reason);
      }
    }

    std::variant<session_info_t, session_error_e> begin_session(const device_t &device) {
      if (session) {
        // Keep the old key for a short time so the replaced device receives pong code 4.
        replaced = replaced_t {session->id, std::move(session->cipher), steady::now() + REPLACED_LIFETIME};
        // End the old session first. Its destructor restores the default capture device,
        // so the new virtual microphone records the real previous default.
        end_session("replaced by "s + device.name, false);
      }

      auto key_bytes = crypto::rand(16);
      crypto::aes_t key {key_bytes.begin(), key_bytes.end()};
      std::uint32_t id = 0;
      auto id_bytes = crypto::rand(4);
      std::memcpy(&id, id_bytes.data(), sizeof(id));

      auto next = std::make_unique<session_t>();
      next->id = id;
      next->cipher = crypto::cipher::gcm_t {key, false};
      next->device = device;
      next->last_valid = steady::now();

      int opus_error = OPUS_OK;
      next->decoder = opus_decoder_create(48000, 1, &opus_error);
      if (opus_error != OPUS_OK) {
        next->decoder = nullptr;
        next->error = protocol::error_e::decode_failed;
      }

      if (next->decoder) {
        // The callback runs on the render thread until vmic is destroyed, and end_session()
        // destroys vmic before the session, so the raw pointer stays valid.
        auto *target = next.get();
        platf::virtual_mic_error_e vmic_error = platf::virtual_mic_error_e::none;
        next->vmic = platf::virtual_mic(
          [target](float *out, std::size_t capacity) {
            return fill_from_session(*target, out, capacity);
          },
          vmic_error
        );

        if (vmic_error == platf::virtual_mic_error_e::unsupported) {
          opus_decoder_destroy(next->decoder);
          return session_error_e::unsupported;
        }
        if (vmic_error == platf::virtual_mic_error_e::device_missing) {
          next->error = protocol::error_e::device_missing;
        } else if (vmic_error == platf::virtual_mic_error_e::device_open_failed) {
          next->error = protocol::error_e::device_open_failed;
        }
      }

      session = std::move(next);

      {
        std::lock_guard lock {state_mutex};
        connected_uuid = device.uuid;
        if (session->vmic) {
          store->previous_default_capture = session->vmic->previous_default_capture();
          store->save();
        }
      }

      BOOST_LOG(info) << "Remote microphone: session started for ["sv << device.name << ']';
      if (session->error == protocol::error_e::none) {
        notify_connected(device.name);
      } else {
        notify_error(session->error);
      }

      return session_info_t {id, key_bytes, net::map_port(MIC_PORT_OFFSET)};
    }

    void send_pong(crypto::cipher::gcm_t &cipher, std::uint32_t session_id, std::uint32_t sequence, protocol::error_e error_code) {
      const char code = static_cast<char>(error_code);
      auto datagram = protocol::encrypt_packet(cipher, protocol::direction_e::to_client, {protocol::packet_type_e::pong, session_id, sequence}, std::string_view {&code, 1});
      if (datagram.empty()) {
        return;
      }
      boost::system::error_code ec;
      socket->send_to(asio::buffer(datagram), sender, 0, ec);
    }

    void handle_datagram(std::string_view datagram) {
      auto header = protocol::parse_header(datagram);
      if (!header) {
        return;
      }

      if (session && header->session_id == session->id) {
        auto packet = protocol::decrypt_packet(session->cipher, protocol::direction_e::to_host, datagram);
        if (!packet) {
          return;
        }
        session->last_valid = steady::now();

        if (packet->header.type == protocol::packet_type_e::audio) {
          if (session->error == protocol::error_e::none) {
            std::lock_guard lock {session->jitter_mutex};
            session->jitter.push(packet->header.sequence, std::move(packet->payload));
          }
        } else if (packet->header.type == protocol::packet_type_e::ping) {
          send_pong(session->cipher, session->id, packet->header.sequence, session->error);
        }
        return;
      }

      if (replaced && header->session_id == replaced->id && header->type == protocol::packet_type_e::ping) {
        if (protocol::decrypt_packet(replaced->cipher, protocol::direction_e::to_host, datagram)) {
          send_pong(replaced->cipher, replaced->id, header->sequence, protocol::error_e::replaced);
        }
      }
    }

    /**
     * @brief Expire the replaced key, time out a silent session, and check the virtual microphone.
     *
     * One second is enough for these. Playout does not depend on this timer.
     */
    void on_housekeeping(const boost::system::error_code &ec) {
      if (ec) {
        return;
      }

      auto now = steady::now();
      if (replaced && now > replaced->expires) {
        replaced.reset();
      }

      if (session) {
        if (now - session->last_valid > SESSION_TIMEOUT) {
          end_session("connection lost", true);
        } else if (session->vmic && !session->vmic->healthy()) {
          session->error = protocol::error_e::device_open_failed;
          notify_error(protocol::error_e::device_open_failed);
          session->vmic.reset();
        }
      }

      timer->expires_after(HOUSEKEEPING);
      timer->async_wait(on_housekeeping);
    }

    void receive_next() {
      socket->async_receive_from(asio::buffer(receive_buffer), sender, [](const boost::system::error_code &ec, std::size_t bytes) {
        if (ec == asio::error::operation_aborted) {
          return;
        }
        if (!ec) {
          try {
            handle_datagram(std::string_view {receive_buffer.data(), bytes});
          } catch (const std::exception &e) {
            BOOST_LOG(warning) << "Remote microphone: dropped a packet: "sv << e.what();
          }
        }
        receive_next();
      });
    }

    void run(std::promise<bool> ready) {
      try {
        auto address_family = net::af_from_enum_string(config::sunshine.address_family);
        auto protocol_family = address_family == net::IPV4 ? udp::v4() : udp::v6();
        auto port = net::map_port(MIC_PORT_OFFSET);

        socket = std::make_unique<udp::socket>(*io);
        socket->open(protocol_family);
        socket->bind(udp::endpoint(protocol_family, port));

        timer = std::make_unique<asio::steady_timer>(*io);
        timer->expires_after(HOUSEKEEPING);
        timer->async_wait(on_housekeeping);
        receive_next();

        BOOST_LOG(info) << "Remote microphone: listening on UDP port "sv << port;
        running = true;
        ready.set_value(true);
      } catch (const std::exception &e) {
        BOOST_LOG(error) << "Remote microphone: could not open the UDP port: "sv << e.what();
        ready.set_value(false);
        return;
      }

      try {
        io->run();
      } catch (const std::exception &e) {
        BOOST_LOG(error) << "Remote microphone: the mic thread stopped: "sv << e.what();
      }
      running = false;
    }

    /**
     * @brief Start the mic thread when it is not running. Call with state_mutex held.
     */
    bool ensure_thread() {
      if (running) {
        return true;
      }
      if (thread.joinable()) {
        thread.join();
      }

      io = std::make_unique<asio::io_context>();
      std::promise<bool> ready;
      auto started = ready.get_future();
      thread = std::thread {run, std::move(ready)};
      return started.get();
    }
  }  // namespace

  void start() {
    std::lock_guard lock {state_mutex};

    auto file = std::filesystem::path {config::nvhttp.file_state}.parent_path() / "mic_state.json";
    store = std::make_unique<store_t>(file);
    if (!store->load()) {
      BOOST_LOG(error) << "Remote microphone: could not read "sv << file.string() << ". Paired microphones are unavailable until the file is repaired or deleted."sv;
    }

    if (auto value = protocol::cert_fingerprint(file_handler::read_file(config::nvhttp.cert.c_str()))) {
      fingerprint = *value;
    }

    if (!store->previous_default_capture.empty()) {
      BOOST_LOG(info) << "Remote microphone: restoring the default capture device after an unclean exit"sv;
      platf::restore_default_capture(store->previous_default_capture);
      store->previous_default_capture.clear();
      store->save();
    }

    // Inert until a mic device is paired: no thread and no socket.
    if (!store->devices().empty()) {
      ensure_thread();
    }
  }

  void stop() {
    if (!thread.joinable()) {
      return;
    }
    if (running) {
      asio::post(*io, []() {
        end_session("Apollo is closing", false);
        io->stop();
      });
    }
    thread.join();
  }

  std::optional<std::string> pair_request(const std::string &address, const std::string &name, const std::string &nonce, const crypto::sha256_t &proof) {
    std::optional<std::string> request_id;
    {
      std::lock_guard lock {state_mutex};
      request_id = pairing.request(address, name, nonce, proof, steady::now());
    }
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
    if (request_id) {
      system_tray::update_tray_mic_pair_request();
    }
#endif
    return request_id;
  }

  pairing_t::status_t pair_status(const std::string &request_id) {
    std::lock_guard lock {state_mutex};
    return pairing.status(request_id, steady::now());
  }

  std::optional<std::string> submit_pin(const std::string &pin, const std::string &name) {
    std::lock_guard lock {state_mutex};
    auto now = steady::now();
    auto match = pairing.submit_pin(pin, fingerprint, now);
    if (!match) {
      return std::nullopt;
    }

    auto device_name = name.empty() ? match->name : name;
    auto uuid = uuid_util::uuid_t::generate().string();
    auto token = crypto::rand_alphabet(48, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789");

    store->add(device_name, uuid, token);
    if (!store->save()) {
      store->remove(uuid);
      return std::nullopt;
    }

    pairing.complete(match->request_id, device_name, uuid, token, now);
    ensure_thread();
    BOOST_LOG(info) << "Remote microphone: paired ["sv << device_name << ']';
    return device_name;
  }

  nlohmann::json list() {
    std::lock_guard lock {state_mutex};
    auto devices = nlohmann::json::array();
    for (const auto &device : store->devices()) {
      devices.push_back({{"name", device.name}, {"uuid", device.uuid}, {"connected", device.uuid == connected_uuid}});
    }
    return devices;
  }

  bool remove(const std::string &uuid) {
    bool was_connected;
    {
      std::lock_guard lock {state_mutex};
      if (!store->remove(uuid)) {
        return false;
      }
      store->save();
      was_connected = connected_uuid == uuid;
    }
    if (was_connected) {
      session_end(uuid);
    }
    return true;
  }

  std::optional<device_t> authorize(const std::string &token) {
    std::lock_guard lock {state_mutex};
    return store->authorize(token);
  }

  std::variant<session_info_t, session_error_e> session_start(const device_t &device) {
    {
      std::lock_guard lock {state_mutex};
      if (!ensure_thread()) {
        return session_error_e::failed;
      }
    }

    std::promise<std::variant<session_info_t, session_error_e>> promise;
    auto result = promise.get_future();
    asio::post(*io, [&]() {
      try {
        promise.set_value(begin_session(device));
      } catch (const std::exception &e) {
        BOOST_LOG(error) << "Remote microphone: could not start a session: "sv << e.what();
        promise.set_value(session_error_e::failed);
      }
    });
    return result.get();
  }

  void session_end(const std::string &uuid) {
    if (!running) {
      return;
    }
    asio::post(*io, [uuid]() {
      if (session && session->device.uuid == uuid) {
        end_session("", true);
      }
    });
  }
}  // namespace mic
