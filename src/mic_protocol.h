/**
 * @file src/mic_protocol.h
 * @brief Wire protocol for the remote microphone: packets, nonces, and the PIN proof.
 */
#pragma once

// standard includes
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

// local includes
#include "crypto.h"

namespace mic::protocol {
  enum class packet_type_e : std::uint8_t {
    audio = 0,  ///< One Opus frame
    ping = 1,  ///< Keepalive from Calliope
    pong = 2  ///< Reply from the host, carries one error_e byte
  };

  enum class direction_e : std::uint8_t {
    to_host = 0,
    to_client = 1
  };

  enum class error_e : std::uint8_t {
    none = 0,
    device_missing = 1,
    device_open_failed = 2,
    decode_failed = 3,
    replaced = 4
  };

  constexpr std::size_t HEADER_SIZE = 9;
  constexpr std::size_t TAG_SIZE = crypto::cipher::tag_size;
  constexpr std::size_t MAX_PAYLOAD = 1400;

  struct header_t {
    packet_type_e type;
    std::uint32_t session_id;
    std::uint32_t sequence;
  };

  struct packet_t {
    header_t header;
    std::vector<std::uint8_t> payload;
  };

  /**
   * @brief Build the 12-byte GCM nonce: direction, type, two zero bytes, session id, sequence.
   */
  crypto::aes_t build_nonce(direction_e direction, const header_t &header);

  /**
   * @brief Read the clear header. Returns nothing for a short, oversized, or unknown-type datagram.
   */
  std::optional<header_t> parse_header(std::string_view datagram);

  /**
   * @brief Authenticate and decrypt a datagram laid out as [header][tag][ciphertext].
   */
  std::optional<packet_t> decrypt_packet(crypto::cipher::gcm_t &cipher, direction_e direction, std::string_view datagram);

  /**
   * @brief Encrypt a payload. Returns an empty vector when encryption fails.
   */
  std::vector<std::uint8_t> encrypt_packet(crypto::cipher::gcm_t &cipher, direction_e direction, const header_t &header, std::string_view payload);

  /**
   * @brief HMAC-SHA256 with the PIN as key over fingerprint || nonce.
   */
  crypto::sha256_t pin_proof(std::string_view pin, const crypto::sha256_t &fingerprint, std::string_view nonce);

  /**
   * @brief SHA-256 of the DER form of a PEM certificate.
   */
  std::optional<crypto::sha256_t> cert_fingerprint(std::string_view pem);
}  // namespace mic::protocol
