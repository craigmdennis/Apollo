/**
 * @file tests/unit/test_mic_playout.cpp
 * @brief Test src/mic_playout.* with the real Opus decoder.
 */
#include <array>
#include <cmath>
#include <gtest/gtest.h>
#include <src/mic_playout.h>

namespace {
  using mic::playout_t;

  // A 20 ms mono Opus frame of silence (CELT, fullband).
  std::vector<std::uint8_t> silence() {
    return {0xF8, 0xFF, 0xFE};
  }

  // Code 3 in the TOC byte requires a frame count byte, so a 1-byte packet is invalid.
  std::vector<std::uint8_t> invalid() {
    return {0x03};
  }

  std::array<float, 5760> buffer;
}  // namespace

TEST(MicPlayout, DecoderIsCreated) {
  playout_t playout;
  EXPECT_TRUE(playout.ready());
  EXPECT_FALSE(playout.decode_failed());
}

TEST(MicPlayout, ReturnsNothingWhileWaiting) {
  playout_t playout;
  EXPECT_EQ(playout.fill(buffer.data(), buffer.size()), 0u);
  playout.push(0, silence());
  EXPECT_EQ(playout.fill(buffer.data(), buffer.size()), 0u);
}

TEST(MicPlayout, DecodesAFrameAfterThePrebuffer) {
  playout_t playout;
  playout.push(0, silence());
  playout.push(1, silence());
  buffer.fill(1.0f);
  ASSERT_EQ(playout.fill(buffer.data(), buffer.size()), 960u);
  for (std::size_t index = 0; index < 960; ++index) {
    ASSERT_LT(std::fabs(buffer[index]), 0.001f) << "sample " << index;
  }
}

TEST(MicPlayout, ConcealsALostFrame) {
  playout_t playout;
  playout.push(0, silence());
  playout.push(2, silence());
  EXPECT_EQ(playout.fill(buffer.data(), buffer.size()), 960u);  // sequence 0
  EXPECT_EQ(playout.fill(buffer.data(), buffer.size()), 960u);  // sequence 1, concealed
  EXPECT_EQ(playout.fill(buffer.data(), buffer.size()), 960u);  // sequence 2
}

TEST(MicPlayout, RejectsADuplicateFrame) {
  playout_t playout;
  EXPECT_TRUE(playout.push(0, silence()));
  EXPECT_FALSE(playout.push(0, silence()));
}

TEST(MicPlayout, SmallOutputBufferReturnsNothing) {
  playout_t playout;
  playout.push(0, silence());
  playout.push(1, silence());
  EXPECT_EQ(playout.fill(buffer.data(), 959), 0u);
}

TEST(MicPlayout, LatchesAfterRepeatedDecodeFailures) {
  playout_t playout;
  std::uint32_t sequence = 0;
  playout.push(sequence++, invalid());
  for (int call = 0; call < playout_t::MAX_DECODE_FAILURES; ++call) {
    playout.push(sequence++, invalid());
    EXPECT_FALSE(playout.decode_failed()) << "call " << call;
    EXPECT_EQ(playout.fill(buffer.data(), buffer.size()), 0u);
  }
  EXPECT_TRUE(playout.decode_failed());
}

TEST(MicPlayout, AGoodFrameClearsTheFailureCount) {
  playout_t playout;
  std::uint32_t sequence = 0;
  // Each call queues one frame and decodes the frame queued before it.
  auto feed = [&](std::vector<std::uint8_t> payload) {
    playout.push(sequence++, std::move(payload));
    return playout.fill(buffer.data(), buffer.size());
  };

  playout.push(sequence++, invalid());
  for (int call = 0; call < playout_t::MAX_DECODE_FAILURES - 2; ++call) {
    feed(invalid());
  }
  feed(silence());  // decodes the last invalid frame: one failure short of the limit
  EXPECT_EQ(feed(silence()), 960u);  // a good frame
  for (int call = 0; call < playout_t::MAX_DECODE_FAILURES - 1; ++call) {
    feed(invalid());
  }
  EXPECT_FALSE(playout.decode_failed());
}
