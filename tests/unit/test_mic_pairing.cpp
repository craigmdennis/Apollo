/**
 * @file tests/unit/test_mic_pairing.cpp
 * @brief Test src/mic_pairing.*.
 */
#include <gtest/gtest.h>
#include <src/mic_pairing.h>
#include <src/mic_protocol.h>

namespace {
  using namespace std::chrono_literals;
  using mic::pairing_t;
  using state_e = mic::pairing_t::state_e;

  const crypto::sha256_t FINGERPRINT = crypto::hash("host-certificate");
  const std::string NONCE(16, 'n');

  crypto::sha256_t proof_for(std::string_view pin) {
    return mic::protocol::pin_proof(pin, FINGERPRINT, NONCE);
  }

  pairing_t::clock_t::time_point t0() {
    return pairing_t::clock_t::time_point {} + 1h;
  }
}  // namespace

TEST(MicPairing, CorrectPinMatchesAndCompletes) {
  pairing_t pairing;
  auto id = pairing.request("192.168.1.20", "iPhone", NONCE, proof_for("4821"), t0());
  ASSERT_TRUE(id.has_value());
  EXPECT_EQ(pairing.status(*id, t0()).state, state_e::pending);

  auto match = pairing.submit_pin("4821", FINGERPRINT, t0() + 5s);
  ASSERT_TRUE(match.has_value());
  EXPECT_EQ(match->request_id, *id);
  EXPECT_EQ(match->name, "iPhone");

  pairing.complete(*id, "Living room iPhone", "uuid-1", "token-1", t0() + 5s);
  auto status = pairing.status(*id, t0() + 6s);
  EXPECT_EQ(status.state, state_e::paired);
  EXPECT_EQ(status.uuid, "uuid-1");
  EXPECT_EQ(status.token, "token-1");
  EXPECT_EQ(status.name, "Living room iPhone");
}

TEST(MicPairing, TokenIsReturnedOnce) {
  pairing_t pairing;
  auto id = pairing.request("192.168.1.20", "iPhone", NONCE, proof_for("4821"), t0());
  pairing.submit_pin("4821", FINGERPRINT, t0());
  pairing.complete(*id, "iPhone", "uuid-1", "token-1", t0());
  EXPECT_EQ(pairing.status(*id, t0()).state, state_e::paired);
  EXPECT_EQ(pairing.status(*id, t0()).state, state_e::expired);
}

TEST(MicPairing, WrongFingerprintFails) {
  pairing_t pairing;
  pairing.request("192.168.1.20", "iPhone", NONCE, proof_for("4821"), t0());
  EXPECT_FALSE(pairing.submit_pin("4821", crypto::hash("other-certificate"), t0()).has_value());
}

TEST(MicPairing, ThreeWrongPinsCancelEveryPendingRequest) {
  pairing_t pairing;
  auto id = pairing.request("192.168.1.20", "iPhone", NONCE, proof_for("4821"), t0());
  EXPECT_FALSE(pairing.submit_pin("0000", FINGERPRINT, t0()).has_value());
  EXPECT_FALSE(pairing.submit_pin("0001", FINGERPRINT, t0()).has_value());
  EXPECT_EQ(pairing.pending_count(), 1u);
  EXPECT_FALSE(pairing.submit_pin("0002", FINGERPRINT, t0()).has_value());
  EXPECT_EQ(pairing.pending_count(), 0u);
  EXPECT_EQ(pairing.status(*id, t0()).state, state_e::expired);
  EXPECT_FALSE(pairing.submit_pin("4821", FINGERPRINT, t0()).has_value());
}

TEST(MicPairing, RequestExpiresAfter120Seconds) {
  pairing_t pairing;
  auto id = pairing.request("192.168.1.20", "iPhone", NONCE, proof_for("4821"), t0());
  EXPECT_EQ(pairing.status(*id, t0() + 120s).state, state_e::pending);
  EXPECT_EQ(pairing.status(*id, t0() + 121s).state, state_e::expired);
  EXPECT_FALSE(pairing.submit_pin("4821", FINGERPRINT, t0() + 121s).has_value());
}

TEST(MicPairing, CompletionRestartsTheExpiryPeriod) {
  pairing_t pairing;
  auto id = pairing.request("192.168.1.20", "iPhone", NONCE, proof_for("4821"), t0());
  pairing.submit_pin("4821", FINGERPRINT, t0() + 119s);
  pairing.complete(*id, "iPhone", "uuid-1", "token-1", t0() + 119s);
  EXPECT_EQ(pairing.status(*id, t0() + 125s).state, state_e::paired);
}

TEST(MicPairing, NewRequestFromSameAddressReplacesTheOldOne) {
  pairing_t pairing;
  auto first = pairing.request("192.168.1.20", "iPhone", NONCE, proof_for("1111"), t0());
  auto second = pairing.request("192.168.1.20", "iPhone", NONCE, proof_for("2222"), t0() + 1s);
  EXPECT_EQ(pairing.pending_count(), 1u);
  EXPECT_EQ(pairing.status(*first, t0() + 1s).state, state_e::expired);
  EXPECT_EQ(pairing.status(*second, t0() + 1s).state, state_e::pending);
}

TEST(MicPairing, FifthAddressIsRejected) {
  pairing_t pairing;
  for (int index = 0; index < 4; ++index) {
    EXPECT_TRUE(pairing.request("192.168.1." + std::to_string(20 + index), "device", NONCE, proof_for("4821"), t0()).has_value());
  }
  EXPECT_FALSE(pairing.request("192.168.1.99", "device", NONCE, proof_for("4821"), t0()).has_value());
}

TEST(MicPairing, UnknownRequestIdIsExpired) {
  pairing_t pairing;
  EXPECT_EQ(pairing.status("missing", t0()).state, state_e::expired);
}
