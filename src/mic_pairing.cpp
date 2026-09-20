/**
 * @file src/mic_pairing.cpp
 * @brief Definitions for remote microphone pairing state.
 */
// lib includes
#include <openssl/crypto.h>

// local includes
#include "mic_pairing.h"
#include "mic_protocol.h"

namespace mic {
  void pairing_t::expire(clock_t::time_point now) {
    std::erase_if(requests, [&](const auto &entry) {
      return now - entry.second.created > EXPIRY;
    });
  }

  std::size_t pairing_t::pending_count() const {
    std::size_t count = 0;
    for (const auto &[id, request] : requests) {
      if (!request.paired) {
        ++count;
      }
    }
    return count;
  }

  std::optional<std::string> pairing_t::request(const std::string &address, std::string name, std::string nonce, const crypto::sha256_t &proof, clock_t::time_point now) {
    expire(now);

    // "Get a new PIN" sends a second request from the same address. The new one replaces the old one.
    std::erase_if(requests, [&](const auto &entry) {
      return !entry.second.paired && entry.second.address == address;
    });

    if (pending_count() >= MAX_PENDING) {
      return std::nullopt;
    }

    auto request_id = util::hex_vec(crypto::rand(16));
    requests.emplace(request_id, request_t {address, std::move(name), std::move(nonce), proof, now});
    return request_id;
  }

  std::optional<pairing_t::match_t> pairing_t::submit_pin(std::string_view pin, const crypto::sha256_t &fingerprint, clock_t::time_point now) {
    expire(now);

    for (const auto &[id, request] : requests) {
      if (request.paired) {
        continue;
      }
      auto expected = protocol::pin_proof(pin, fingerprint, request.nonce);
      if (CRYPTO_memcmp(expected.data(), request.proof.data(), expected.size()) == 0) {
        wrong_pins = 0;
        return match_t {id, request.name};
      }
    }

    if (++wrong_pins >= MAX_WRONG_PINS) {
      wrong_pins = 0;
      std::erase_if(requests, [](const auto &entry) {
        return !entry.second.paired;
      });
    }
    return std::nullopt;
  }

  void pairing_t::complete(const std::string &request_id, std::string name, std::string uuid, std::string token, clock_t::time_point now) {
    auto it = requests.find(request_id);
    if (it == requests.end()) {
      return;
    }
    it->second.paired = true;
    it->second.name = std::move(name);
    it->second.uuid = std::move(uuid);
    it->second.token = std::move(token);
    // Calliope polls every 2 seconds. A fresh period stops a late PIN entry from losing the token.
    it->second.created = now;
  }

  pairing_t::status_t pairing_t::status(const std::string &request_id, clock_t::time_point now) {
    expire(now);

    auto it = requests.find(request_id);
    if (it == requests.end()) {
      return {state_e::expired, {}, {}, {}};
    }
    if (!it->second.paired) {
      return {state_e::pending, {}, {}, {}};
    }

    status_t result {state_e::paired, it->second.uuid, it->second.token, it->second.name};
    requests.erase(it);
    return result;
  }
}  // namespace mic
