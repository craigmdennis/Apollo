/**
 * @file src/mic_store.h
 * @brief Persistent list of paired remote microphone devices (mic_state.json).
 */
#pragma once

// standard includes
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace mic {
  struct device_t {
    std::string name;
    std::string uuid;
    std::string token_hash;  ///< Unsalted SHA-256 of the mic token, in hex
  };

  /**
   * @brief Stores mic devices in their own file. Never touches sunshine_state.json.
   */
  class store_t {
  public:
    explicit store_t(std::filesystem::path file);

    bool load();
    bool save() const;

    const std::vector<device_t> &devices() const {
      return _devices;
    }

    const device_t &add(std::string name, std::string uuid, const std::string &token);
    bool remove(const std::string &uuid);
    std::optional<device_t> authorize(const std::string &token) const;

    static std::string hash_token(const std::string &token);

    /// Default capture device id saved before a switch. Non-empty after a crash during a mic session.
    std::string previous_default_capture;

  private:
    std::filesystem::path _file;
    std::vector<device_t> _devices;
  };
}  // namespace mic
