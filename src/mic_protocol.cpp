/**
 * @file src/mic_protocol.cpp
 * @brief Definitions for the remote microphone wire protocol.
 */
// standard includes
#include <string>

// lib includes
#include <openssl/hmac.h>
#include <openssl/x509.h>

// local includes
#include "mic_protocol.h"

namespace mic::protocol {
  namespace {
    void put_be32(std::uint8_t *out, std::uint32_t value) {
      out[0] = static_cast<std::uint8_t>(value >> 24);
      out[1] = static_cast<std::uint8_t>(value >> 16);
      out[2] = static_cast<std::uint8_t>(value >> 8);
      out[3] = static_cast<std::uint8_t>(value);
    }

    std::uint32_t get_be32(const char *in) {
      auto byte = [in](int index) {
        return static_cast<std::uint32_t>(static_cast<std::uint8_t>(in[index]));
      };
      return (byte(0) << 24) | (byte(1) << 16) | (byte(2) << 8) | byte(3);
    }
  }  // namespace

  crypto::aes_t build_nonce(direction_e direction, const header_t &header) {
    crypto::aes_t nonce(12, 0);
    nonce[0] = static_cast<std::uint8_t>(direction);
    nonce[1] = static_cast<std::uint8_t>(header.type);
    put_be32(nonce.data() + 4, header.session_id);
    put_be32(nonce.data() + 8, header.sequence);
    return nonce;
  }

  std::optional<header_t> parse_header(std::string_view datagram) {
    if (datagram.size() < HEADER_SIZE + TAG_SIZE || datagram.size() > HEADER_SIZE + TAG_SIZE + MAX_PAYLOAD) {
      return std::nullopt;
    }

    auto type = static_cast<std::uint8_t>(datagram[0]);
    if (type > static_cast<std::uint8_t>(packet_type_e::pong)) {
      return std::nullopt;
    }

    return header_t {
      static_cast<packet_type_e>(type),
      get_be32(datagram.data() + 1),
      get_be32(datagram.data() + 5)
    };
  }

  std::optional<packet_t> decrypt_packet(crypto::cipher::gcm_t &cipher, direction_e direction, std::string_view datagram) {
    auto header = parse_header(datagram);
    if (!header) {
      return std::nullopt;
    }

    auto nonce = build_nonce(direction, *header);
    packet_t packet {*header, {}};
    // gcm_t reads [tag][ciphertext], which is the layout that follows the header.
    if (cipher.decrypt(datagram.substr(HEADER_SIZE), packet.payload, &nonce) != 0) {
      return std::nullopt;
    }
    return packet;
  }

  std::vector<std::uint8_t> encrypt_packet(crypto::cipher::gcm_t &cipher, direction_e direction, const header_t &header, std::string_view payload) {
    auto nonce = build_nonce(direction, header);
    std::vector<std::uint8_t> out(HEADER_SIZE + TAG_SIZE + crypto::cipher::round_to_pkcs7_padded(payload.size()));
    out[0] = static_cast<std::uint8_t>(header.type);
    put_be32(out.data() + 1, header.session_id);
    put_be32(out.data() + 5, header.sequence);

    // gcm_t writes [tag][ciphertext] at the given address.
    auto length = cipher.encrypt(payload, out.data() + HEADER_SIZE, &nonce);
    if (length < 0) {
      return {};
    }
    out.resize(HEADER_SIZE + TAG_SIZE + static_cast<std::size_t>(length));
    return out;
  }

  crypto::sha256_t pin_proof(std::string_view pin, const crypto::sha256_t &fingerprint, std::string_view nonce) {
    std::string message {reinterpret_cast<const char *>(fingerprint.data()), fingerprint.size()};
    message.append(nonce);

    crypto::sha256_t proof {};
    unsigned int length = 0;
    if (!HMAC(EVP_sha256(), pin.data(), static_cast<int>(pin.size()), reinterpret_cast<const unsigned char *>(message.data()), message.size(), proof.data(), &length)) {
      // An all-zero proof must never be a valid answer.
      RAND_bytes(proof.data(), static_cast<int>(proof.size()));
    }
    return proof;
  }

  std::optional<crypto::sha256_t> cert_fingerprint(std::string_view pem) {
    auto cert = crypto::x509(pem);
    if (!cert) {
      return std::nullopt;
    }

    crypto::sha256_t fingerprint {};
    unsigned int length = 0;
    if (X509_digest(cert.get(), EVP_sha256(), fingerprint.data(), &length) != 1) {
      return std::nullopt;
    }
    return fingerprint;
  }
}  // namespace mic::protocol
