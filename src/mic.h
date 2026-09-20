/**
 * @file src/mic.h
 * @brief Remote microphone: pairing, mic sessions, and playout into the virtual microphone.
 */
#pragma once

// standard includes
#include <cstdint>
#include <optional>
#include <string>
#include <variant>

// lib includes
#include <nlohmann/json.hpp>

// local includes
#include "crypto.h"
#include "mic_pairing.h"
#include "mic_store.h"

namespace mic {
  enum class session_error_e {
    unsupported,  ///< This host platform has no virtual microphone
    busy,  ///< The mic thread did not answer in time. The caller retries.
    failed
  };

  struct session_info_t {
    std::uint32_t session_id;
    std::string key;  ///< 16 raw bytes
    std::uint16_t port;
  };

  /**
   * @brief Load the store and repair a default capture device left switched by a crash.
   *
   * Starts the mic thread only when a mic device is already paired.
   */
  void start();

  /**
   * @brief End the mic session, restore the default capture device, and join the mic thread.
   */
  void stop();

  std::optional<std::string> pair_request(const std::string &address, const std::string &name, const std::string &nonce, const crypto::sha256_t &proof);
  pairing_t::status_t pair_status(const std::string &request_id);
  std::optional<std::string> submit_pin(const std::string &pin, const std::string &name);

  nlohmann::json list();
  bool remove(const std::string &uuid);
  std::optional<device_t> authorize(const std::string &token);

  std::variant<session_info_t, session_error_e> session_start(const device_t &device);
  void session_end(const std::string &uuid);
}  // namespace mic
