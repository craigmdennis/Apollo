/**
 * @file src/steam_activity_host.cpp
 * @brief Finds Steam and the signed-in user on this host, and caches the activity response.
 *
 * Apollo's main process runs as SYSTEM, so HKEY_CURRENT_USER is not the signed-in user's hive, and any
 * path a user can influence would be opened with SYSTEM's rights. Every per-user registry read and every
 * read of a file under a user-controlled path therefore runs while impersonating the console user.
 */
#include "steam_activity.h"

// standard includes
#include <chrono>
#include <functional>
#include <mutex>

// local includes
#include "logging.h"

#ifdef _WIN32
  // platform includes
  #include <Windows.h>
  #include <ShlObj.h>

  // local includes
  #include "platform/windows/misc.h"
#endif

using namespace std::literals;
namespace fs = std::filesystem;

namespace steam_activity {
#ifdef _WIN32
  namespace {
    std::optional<DWORD> reg_dword(HKEY hive, const wchar_t *sub, const wchar_t *name) {
      DWORD value = 0;
      DWORD size = sizeof(value);
      if (RegGetValueW(hive, sub, name, RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS) {
        return std::nullopt;
      }
      return value;
    }

    std::optional<std::wstring> reg_string(HKEY hive, const wchar_t *sub, const wchar_t *name) {
      DWORD size = 0;
      if (RegGetValueW(hive, sub, name, RRF_RT_REG_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS || size < sizeof(wchar_t)) {
        return std::nullopt;
      }
      std::wstring text(size / sizeof(wchar_t), L'\0');
      if (RegGetValueW(hive, sub, name, RRF_RT_REG_SZ, nullptr, text.data(), &size) != ERROR_SUCCESS) {
        return std::nullopt;
      }
      text.resize(wcslen(text.c_str()));
      return text;
    }

    /**
     * @brief The signed-in console user, if any.
     */
    class console_user_t {
    public:
      console_user_t():
          token_(platf::retrieve_users_token(false)) {
      }

      ~console_user_t() {
        if (token_) {
          CloseHandle(token_);
        }
      }

      console_user_t(const console_user_t &) = delete;
      console_user_t &operator=(const console_user_t &) = delete;

      explicit operator bool() const {
        return token_ != nullptr;
      }

      /**
       * @brief Run `fn` as the user. `fn` must not throw, because an exception would skip the revert.
       */
      void run(std::function<void()> fn) const {
        platf::impersonate_current_user(token_, std::move(fn));
      }

      fs::path transcoded_wallpaper() const {
        PWSTR roaming = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, token_, &roaming))) {
          return {};
        }
        fs::path path = fs::path(roaming) / L"Microsoft" / L"Windows" / L"Themes" / L"TranscodedWallpaper";
        CoTaskMemFree(roaming);
        return path;
      }

    private:
      HANDLE token_;
    };

    struct user_values_t {
      uint32_t account_id = 0;
      uint32_t running_app_id = 0;
      fs::path wallpaper;
    };

    // Valid only while impersonating the user.
    user_values_t read_user_values() {
      user_values_t values;
      HKEY hive = nullptr;
      if (RegOpenCurrentUser(KEY_READ, &hive) != ERROR_SUCCESS) {
        return values;
      }
      values.account_id = reg_dword(hive, L"Software\\Valve\\Steam\\ActiveProcess", L"ActiveUser").value_or(0);
      values.running_app_id = reg_dword(hive, L"Software\\Valve\\Steam", L"RunningAppID").value_or(0);
      if (auto s = reg_string(hive, L"Control Panel\\Desktop", L"WallPaper")) {
        values.wallpaper = *s;
      }
      RegCloseKey(hive);
      return values;
    }
  }  // namespace

  std::optional<fs::path> locate_root() {
    // Only the machine-wide key: administrators write it. A key in the user's hive is user-controlled.
    std::error_code ec;
    if (auto s = reg_string(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath")) {
      if (fs::is_directory(fs::path(*s), ec)) {
        return fs::path(*s);
      }
    }
    return std::nullopt;
  }

  std::optional<image_t> current_wallpaper() {
    console_user_t user;
    if (!user) {
      return std::nullopt;
    }
    auto transcoded = user.transcoded_wallpaper();
    std::optional<image_t> image;
    user.run([&] {
      try {
        auto values = read_user_values();
        if (auto file = pick_wallpaper(values.wallpaper, transcoded)) {
          image = load_wallpaper(*file);
        }
      } catch (...) {
      }
    });
    return image;
  }

  std::optional<image_t> current_art(uint32_t app_id, art_kind kind) {
    auto root = locate_root();
    console_user_t user;
    if (!root || !user) {
      return std::nullopt;
    }
    std::optional<image_t> image;
    user.run([&] {
      try {
        if (auto file = find_art(*root, app_id, kind)) {
          image = load_art(*file);
        }
      } catch (...) {
      }
    });
    return image;
  }

  namespace {
    // Called only from current_activity(), which holds its mutex.
    nlohmann::json build_activity() {
      // The last result built while a user was signed in. While nobody is, Apollo serves it and reads
      // nothing: the Steam folders can be writable by a standard user, and SYSTEM must not open
      // paths that user chose.
      static nlohmann::json last_good;

      auto root = locate_root();
      if (!root) {
        return json_no_steam();
      }

      console_user_t user;
      if (!user) {
        return last_good.is_null() ? json_no_steam() : last_good;
      }

      auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
      nlohmann::json out = json_no_steam();
      bool built = false;

      user.run([&] {
        try {
          auto values = read_user_values();
          uint32_t account = values.account_id;
          if (account == 0) {
            // No Steam session in the user's hive, so use the last account that signed in.
            if (auto doc = read_kv_file(*root / "config" / "loginusers.vdf")) {
              account = most_recent_account(*doc).value_or(0);
            }
          }
          out = to_json(read_activity(*root, account, values.running_app_id, now));
          built = true;
        } catch (...) {
        }
      });

      if (built) {
        last_good = out;
      }
      return out;
    }
  }  // namespace
#else
  std::optional<fs::path> locate_root() {
    return std::nullopt;
  }

  std::optional<image_t> current_wallpaper() {
    return std::nullopt;
  }

  std::optional<image_t> current_art(uint32_t, art_kind) {
    return std::nullopt;
  }

  namespace {
    nlohmann::json build_activity() {
      return json_no_steam();
    }
  }  // namespace
#endif

  nlohmann::json current_activity() {
    using clock = std::chrono::steady_clock;
    static std::mutex mutex;
    static nlohmann::json cached;
    static clock::time_point expires;

    std::lock_guard lock(mutex);
    auto now = clock::now();
    if (!cached.is_null() && now < expires) {
      return cached;
    }
    try {
      cached = build_activity();
    } catch (const std::exception &e) {
      BOOST_LOG(debug) << "Steam activity unavailable: "sv << e.what();
      cached = json_no_steam();
    }
    expires = now + std::chrono::seconds(30);
    return cached;
  }
}  // namespace steam_activity
