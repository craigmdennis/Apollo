/**
 * @file src/updater.cpp
 * @brief Definitions for updating Apollo from a published release.
 */
#include "updater.h"

// local includes
#include "logging.h"

#ifdef _WIN32

  // standard includes
  #include <filesystem>
  #include <format>
  #include <string>
  #include <string_view>
  #include <system_error>

  // lib includes
  #include <boost/filesystem.hpp>
  #include <boost/process/v1/child.hpp>
  #include <boost/process/v1/environment.hpp>
  #include <Windows.h>

  // local includes
  #include "platform/common.h"
  #include "platform/windows/misc.h"

namespace updater {
  using namespace std::literals;

  namespace {
    /**
     * @brief Name of the updater script, installed in the `scripts` directory beside Apollo.
     */
    constexpr std::string_view UPDATE_SCRIPT_NAME = "update-apollo.ps1"sv;

    /**
     * @brief GitHub repository that Apollo releases are published to.
     */
    constexpr std::string_view UPDATE_REPO = "craigmdennis/Apollo"sv;

    /**
     * @brief Get the directory containing the running Apollo binary.
     * @return The directory, or an empty path if it could not be determined.
     */
    std::filesystem::path executable_dir() {
      std::wstring path(MAX_PATH, L'\0');

      for (;;) {
        const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) {
          return {};
        }

        if (length < path.size()) {
          path.resize(length);
          return std::filesystem::path {path}.parent_path();
        }

        // The buffer was too small, so try again with more room.
        path.resize(path.size() * 2);
      }
    }

    /**
     * @brief Get the value of an environment variable as a string.
     * @param name The name of the variable.
     * @return The value, or an empty string if the variable is not set.
     */
    std::wstring environment_value(const wchar_t *name) {
      std::wstring value(MAX_PATH, L'\0');
      const auto length = GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size()));
      if (length == 0 || length >= value.size()) {
        return {};
      }

      value.resize(length);
      return value;
    }

    /**
     * @brief Get the path to Windows PowerShell.
     * @return The path, or an empty path if the system directory is unknown.
     */
    std::filesystem::path powershell_path() {
      const auto system_root = environment_value(L"SystemRoot");
      if (system_root.empty()) {
        return {};
      }

      return std::filesystem::path {system_root} / "System32" / "WindowsPowerShell" / "v1.0" / "powershell.exe";
    }
  }  // namespace

  bool check_for_updates() {
    const auto install_dir = executable_dir();
    if (install_dir.empty()) {
      BOOST_LOG(error) << "Cannot update Apollo: the install directory could not be determined"sv;
      return false;
    }

    const auto script = install_dir / "scripts" / UPDATE_SCRIPT_NAME;
    std::error_code ec;
    if (!std::filesystem::exists(script, ec)) {
      BOOST_LOG(error) << "Cannot update Apollo: "sv << platf::to_utf8(script.wstring()) << " is missing"sv;
      return false;
    }

    const auto powershell = powershell_path();
    if (powershell.empty()) {
      BOOST_LOG(error) << "Cannot update Apollo: Windows PowerShell was not found"sv;
      return false;
    }

    // Stage a copy of the script outside the install directory. The installer replaces the
    // install directory while this script is running, so it must not execute from there.
    const auto program_data = environment_value(L"ProgramData");
    if (program_data.empty()) {
      BOOST_LOG(error) << "Cannot update Apollo: the ProgramData directory is unknown"sv;
      return false;
    }

    const auto work_dir = std::filesystem::path {program_data} / "Apollo" / "updater";
    const auto staged_script = work_dir / UPDATE_SCRIPT_NAME;

    std::filesystem::create_directories(work_dir, ec);
    if (ec) {
      BOOST_LOG(error) << "Cannot update Apollo: failed to create "sv << platf::to_utf8(work_dir.wstring()) << ": "sv << ec.message();
      return false;
    }

    std::filesystem::copy_file(script, staged_script, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
      BOOST_LOG(error) << "Cannot update Apollo: failed to stage the updater script: "sv << ec.message();
      return false;
    }

    const auto command = std::format(
      "\"{}\" -NoProfile -NonInteractive -ExecutionPolicy Bypass -WindowStyle Hidden -File \"{}\" -Repo \"{}\" -CurrentVersion \"{}\"",
      platf::to_utf8(powershell.wstring()),
      platf::to_utf8(staged_script.wstring()),
      UPDATE_REPO,
      PROJECT_VERSION
    );

    // Launch elevated so the installer can replace program files without a UAC prompt.
    // The child is created with CREATE_BREAKAWAY_FROM_JOB, so it survives the service stop
    // that installs the update.
    boost::filesystem::path working_dir {platf::to_utf8(work_dir.wstring())};
    const auto env = boost::this_process::environment();
    std::error_code launch_ec;
    auto child = platf::run_command(true, false, command, working_dir, env, nullptr, launch_ec, nullptr);

    if (launch_ec) {
      BOOST_LOG(error) << "Cannot update Apollo: failed to start the updater: "sv << launch_ec.message();
      return false;
    }

    // The updater outlives Apollo, so it must not be waited for or terminated on scope exit.
    child.detach();

    BOOST_LOG(info) << "Started the Apollo updater. Apollo restarts if an update is installed."sv;
    return true;
  }
}  // namespace updater

#else

namespace updater {
  bool check_for_updates() {
    BOOST_LOG(warning) << "Updating Apollo from a release is only supported on Windows"sv;
    return false;
  }
}  // namespace updater

#endif
