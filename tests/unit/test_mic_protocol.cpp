/**
 * @file tests/unit/test_mic_protocol.cpp
 * @brief Test src/mic_protocol.* against the shared vectors in tests/fixtures/mic_vectors.json.
 */
#include <algorithm>
#include <fstream>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <src/mic_protocol.h>

namespace {
  using namespace mic::protocol;

  std::string from_hex(std::string_view hex) {
    std::string out;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
      out.push_back(static_cast<char>(std::stoi(std::string {hex.substr(i, 2)}, nullptr, 16)));
    }
    return out;
  }

  template<class T>
  std::string to_hex(const T &container) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    for (auto value : container) {
      auto byte = static_cast<unsigned char>(value);
      out.push_back(digits[byte >> 4]);
      out.push_back(digits[byte & 0x0F]);
    }
    return out;
  }

  const nlohmann::json &vectors() {
    static const nlohmann::json tree = []() {
      std::ifstream in {std::string {SUNSHINE_SOURCE_DIR} + "/tests/fixtures/mic_vectors.json"};
      return nlohmann::json::parse(in);
    }();
    return tree;
  }

  crypto::cipher::gcm_t vector_cipher() {
    auto key_bytes = from_hex(vectors()["key"].get<std::string>());
    return crypto::cipher::gcm_t {crypto::aes_t {key_bytes.begin(), key_bytes.end()}, false};
  }

  header_t header_of(const nlohmann::json &packet) {
    return {
      static_cast<packet_type_e>(packet["type"].get<int>()),
      vectors()["session_id"].get<std::uint32_t>(),
      packet["sequence"].get<std::uint32_t>()
    };
  }

  direction_e direction_of(const nlohmann::json &packet) {
    return static_cast<direction_e>(packet["direction"].get<int>());
  }

  std::string audio_datagram() {
    return from_hex(vectors()["packets"][0]["datagram"].get<std::string>());
  }
}  // namespace

TEST(MicProtocol, VectorNonces) {
  ASSERT_EQ(vectors()["packets"].size(), 3u);
  for (const auto &packet : vectors()["packets"]) {
    EXPECT_EQ(to_hex(build_nonce(direction_of(packet), header_of(packet))), packet["nonce"].get<std::string>()) << packet["name"];
  }
}

TEST(MicProtocol, VectorsDecrypt) {
  auto cipher = vector_cipher();
  for (const auto &packet : vectors()["packets"]) {
    auto result = decrypt_packet(cipher, direction_of(packet), from_hex(packet["datagram"].get<std::string>()));
    ASSERT_TRUE(result.has_value()) << packet["name"];
    EXPECT_EQ(result->header.type, header_of(packet).type) << packet["name"];
    EXPECT_EQ(result->header.session_id, header_of(packet).session_id) << packet["name"];
    EXPECT_EQ(result->header.sequence, header_of(packet).sequence) << packet["name"];
    EXPECT_EQ(to_hex(result->payload), packet["payload"].get<std::string>()) << packet["name"];
  }
}

TEST(MicProtocol, VectorsEncrypt) {
  auto cipher = vector_cipher();
  for (const auto &packet : vectors()["packets"]) {
    auto datagram = encrypt_packet(cipher, direction_of(packet), header_of(packet), from_hex(packet["payload"].get<std::string>()));
    EXPECT_EQ(to_hex(datagram), packet["datagram"].get<std::string>()) << packet["name"];
  }
}

TEST(MicProtocol, RejectsTamperedSequence) {
  auto cipher = vector_cipher();
  auto datagram = audio_datagram();
  datagram[8] ^= 0x01;  // last byte of the sequence field
  EXPECT_FALSE(decrypt_packet(cipher, direction_e::to_host, datagram).has_value());
}

TEST(MicProtocol, RejectsWrongDirection) {
  auto cipher = vector_cipher();
  EXPECT_FALSE(decrypt_packet(cipher, direction_e::to_client, audio_datagram()).has_value());
}

TEST(MicProtocol, RejectsShortDatagram) {
  EXPECT_FALSE(parse_header(std::string(24, '\0')).has_value());
}

TEST(MicProtocol, RejectsUnknownType) {
  std::string datagram(25, '\0');
  datagram[0] = 3;
  EXPECT_FALSE(parse_header(datagram).has_value());
}

TEST(MicProtocol, RejectsOversizedDatagram) {
  EXPECT_FALSE(parse_header(std::string(9 + 16 + 1401, '\0')).has_value());
}

TEST(MicProtocol, VectorPinProof) {
  const auto &vector = vectors()["pin_proof"];
  auto fingerprint_bytes = from_hex(vector["fingerprint"].get<std::string>());
  crypto::sha256_t fingerprint;
  std::copy(fingerprint_bytes.begin(), fingerprint_bytes.end(), fingerprint.begin());

  auto proof = pin_proof(vector["pin"].get<std::string>(), fingerprint, from_hex(vector["nonce"].get<std::string>()));
  EXPECT_EQ(to_hex(proof), vector["proof"].get<std::string>());
}

TEST(MicProtocol, PinProofChangesWithPin) {
  auto fingerprint = crypto::hash("test-certificate-der");
  std::string nonce(16, 'n');
  EXPECT_NE(pin_proof("4821", fingerprint, nonce), pin_proof("4822", fingerprint, nonce));
}

TEST(MicProtocol, CertFingerprintIsStableAndDistinct) {
  auto first = crypto::gen_creds("mic-test-one", 2048);
  auto second = crypto::gen_creds("mic-test-two", 2048);
  auto a = cert_fingerprint(first.x509);
  auto b = cert_fingerprint(first.x509);
  auto c = cert_fingerprint(second.x509);
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(c.has_value());
  EXPECT_EQ(*a, *b);
  EXPECT_NE(*a, *c);
}

TEST(MicProtocol, CertFingerprintRejectsGarbage) {
  EXPECT_FALSE(cert_fingerprint("not a certificate").has_value());
}
