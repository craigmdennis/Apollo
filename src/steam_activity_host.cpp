/**
 * @file src/steam_activity_host.cpp
 * @brief Finds Steam and the signed-in user on this host, and caches the activity response.
 *
 * Apollo's main process runs as SYSTEM, so HKEY_CURRENT_USER is not the signed-in user's hive.
 * Every per-user registry read runs while impersonating the console user's token.
 */
#include "steam_activity.h"

// standard includes
#include <chrono>
#include <fstream>
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

    struct user_t {
      bool signed_in = false;
      uint32_t account_id = 0;
      uint32_t running_app_id = 0;
      fs::path steam_path;
      fs::path wallpaper;
      fs::path transcoded_wallpaper;
    };

    user_t read_user() {
      user_t user;
      HANDLE token = platf::retrieve_users_token(false);
      if (!token) {
        return user;
      }
      user.signed_in = true;

      platf::impersonate_current_user(token, [&] {
        HKEY hive = nullptr;
        if (RegOpenCurrentUser(KEY_READ, &hive) != ERROR_SUCCESS) {
          return;
        }
        user.account_id = reg_dword(hive, L"Software\\Valve\\Steam\\ActiveProcess", L"ActiveUser").value_or(0);
        user.running_app_id = reg_dword(hive, L"Software\\Valve\\Steam", L"RunningAppID").value_or(0);
        if (auto s = reg_string(hive, L"Software\\Valve\\Steam", L"SteamPath")) {
          user.steam_path = *s;
        }
        if (auto s = reg_string(hive, L"Control Panel\\Desktop", L"WallPaper")) {
          user.wallpaper = *s;
        }
        RegCloseKey(hive);
      });

      PWSTR roaming = nullptr;
      if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, token, &roaming))) {
        user.transcoded_wallpaper = fs::path(roaming) / L"Microsoft" / L"Windows" / L"Themes" / L"TranscodedWallpaper";
        CoTaskMemFree(roaming);
      }
      CloseHandle(token);
      return user;
    }
  }  // namespace

  std::optional<fs::path> locate_root() {
    std::error_code ec;
    if (auto s = reg_string(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath")) {
      if (fs::is_directory(fs::path(*s), ec)) {
        return fs::path(*s);
      }
    }
    auto user = read_user();
    if (!user.steam_path.empty() && fs::is_directory(user.steam_path, ec)) {
      return user.steam_path;
    }
    return std::nullopt;
  }

  std::optional<fs::path> current_wallpaper() {
    auto user = read_user();
    if (!user.signed_in) {
      return std::nullopt;
    }
    auto file = pick_wallpaper(user.wallpaper, user.transcoded_wallpaper);
    if (!file || wallpaper_type(*file).empty()) {
      return std::nullopt;
    }
    return file;
  }

  namespace {
    nlohmann::json build_activity() {
      auto root = locate_root();
      if (!root) {
        return json_no_steam();
      }
      auto user = read_user();
      uint32_t account = user.account_id;
      if (account == 0) {
        // No Steam session in the user's hive, so use the last account that signed in.
        std::ifstream in(*root / "config" / "loginusers.vdf", std::ios::binary);
        std::string text(std::istreambuf_iterator<char>(in), {});
        account = most_recent_account(parse_kv(text)).value_or(0);
      }
      return to_json(read_activity(*root, account, user.running_app_id, std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count()));
    }
  }  // namespace
#else
  std::optional<fs::path> locate_root() {
    return std::nullopt;
  }

  std::optional<fs::path> current_wallpaper() {
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
