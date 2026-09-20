/**
 * @file src/mic_pairing.h
 * @brief Pending remote microphone pairing requests, their limits, and the PIN check.
 */
#pragma once

// standard includes
#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <string_view>

// local includes
#include "crypto.h"

namespace mic {
  class pairing_t {
  public:
    using clock_t = std::chrono::steady_clock;

    static constexpr std::chrono::seconds EXPIRY {120};
    static constexpr std::size_t MAX_PENDING = 4;
    static constexpr int MAX_WRONG_PINS = 3;

    enum class state_e {
      pending,
      paired,
      expired  ///< Also returned for an unknown request id
    };

    struct status_t {
      state_e state;
      std::string uuid;
      std::string token;
      std::string name;
    };

    struct match_t {
      std::string request_id;
      std::string name;
    };

    std::optional<std::string> request(const std::string &address, std::string name, std::string nonce, const crypto::sha256_t &proof, clock_t::time_point now);
    std::optional<match_t> submit_pin(std::string_view pin, const crypto::sha256_t &fingerprint, clock_t::time_point now);
    void complete(const std::string &request_id, std::string name, std::string uuid, std::string token, clock_t::time_point now);
    status_t status(const std::string &request_id, clock_t::time_point now);
    std::size_t pending_count() const;

  private:
    struct request_t {
      std::string address;
      std::string name;
      std::string nonce;
      crypto::sha256_t proof;
      clock_t::time_point created;
      bool paired = false;
      std::string uuid;
      std::string token;
    };

    void expire(clock_t::time_point now);

    std::map<std::string, request_t> requests;
    int wrong_pins = 0;
  };
}  // namespace mic
