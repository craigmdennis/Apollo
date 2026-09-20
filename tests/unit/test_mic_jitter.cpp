/**
 * @file tests/unit/test_mic_jitter.cpp
 * @brief Test src/mic_jitter.*.
 */
#include <gtest/gtest.h>

#include <src/mic_jitter.h>

namespace {
  using mic::jitter_buffer_t;
  using pop_e = mic::jitter_buffer_t::pop_e;

  std::vector<std::uint8_t> frame(std::uint8_t marker) {
    return {marker};
  }
}  // namespace

TEST(MicJitter, WaitsUntilPrebufferIsFull) {
  jitter_buffer_t buffer;
  EXPECT_EQ(buffer.pop().kind, pop_e::wait);
  buffer.push(0, frame(0));
  EXPECT_EQ(buffer.pop().kind, pop_e::wait);
  buffer.push(1, frame(1));
  EXPECT_EQ(buffer.pop().kind, pop_e::frame);
}

TEST(MicJitter, PlaysInSequenceOrder) {
  jitter_buffer_t buffer;
  buffer.push(1, frame(1));
  buffer.push(0, frame(0));
  auto first = buffer.pop();
  auto second = buffer.pop();
  ASSERT_EQ(first.kind, pop_e::frame);
  ASSERT_EQ(second.kind, pop_e::frame);
  EXPECT_EQ(first.payload, frame(0));
  EXPECT_EQ(second.payload, frame(1));
}

TEST(MicJitter, StartsAtFirstQueuedSequence) {
  jitter_buffer_t buffer;
  buffer.push(500, frame(5));
  buffer.push(501, frame(6));
  auto first = buffer.pop();
  ASSERT_EQ(first.kind, pop_e::frame);
  EXPECT_EQ(first.payload, frame(5));
}

TEST(MicJitter, ReportsMissingFrameAsLost) {
  jitter_buffer_t buffer;
  buffer.push(0, frame(0));
  buffer.push(2, frame(2));
  EXPECT_EQ(buffer.pop().kind, pop_e::frame);
  EXPECT_EQ(buffer.pop().kind, pop_e::lost);
  auto third = buffer.pop();
  ASSERT_EQ(third.kind, pop_e::frame);
  EXPECT_EQ(third.payload, frame(2));
}

TEST(MicJitter, RejectsDuplicate) {
  jitter_buffer_t buffer;
  EXPECT_TRUE(buffer.push(0, frame(0)));
  EXPECT_FALSE(buffer.push(0, frame(9)));
  EXPECT_EQ(buffer.size(), 1u);
}

TEST(MicJitter, RejectsLateFrame) {
  jitter_buffer_t buffer;
  buffer.push(0, frame(0));
  buffer.push(1, frame(1));
  buffer.push(2, frame(2));
  buffer.pop();
  buffer.pop();
  EXPECT_FALSE(buffer.push(0, frame(0)));
}

TEST(MicJitter, DropsOldestAboveMaximum) {
  jitter_buffer_t buffer;
  for (std::uint32_t sequence = 0; sequence < 7; ++sequence) {
    buffer.push(sequence, frame(static_cast<std::uint8_t>(sequence)));
  }
  EXPECT_EQ(buffer.size(), jitter_buffer_t::MAX_FRAMES);
  auto first = buffer.pop();
  ASSERT_EQ(first.kind, pop_e::frame);
  EXPECT_EQ(first.payload, frame(2));
}

TEST(MicJitter, EmptyBufferConcealsThenRestartsPrebuffer) {
  jitter_buffer_t buffer;
  buffer.push(0, frame(0));
  buffer.push(1, frame(1));
  buffer.pop();
  buffer.pop();
  for (int call = 0; call < jitter_buffer_t::MAX_EMPTY_CONCEAL; ++call) {
    EXPECT_EQ(buffer.pop().kind, pop_e::lost) << "call " << call;
  }
  EXPECT_EQ(buffer.pop().kind, pop_e::wait);
  buffer.push(40, frame(4));
  EXPECT_EQ(buffer.pop().kind, pop_e::wait);
  buffer.push(41, frame(5));
  auto next = buffer.pop();
  ASSERT_EQ(next.kind, pop_e::frame);
  EXPECT_EQ(next.payload, frame(4));
}

TEST(MicJitter, ResumesWithoutPrebufferAfterAShortGap) {
  jitter_buffer_t buffer;
  buffer.push(0, frame(0));
  buffer.push(1, frame(1));
  buffer.pop();
  buffer.pop();
  EXPECT_EQ(buffer.pop().kind, pop_e::lost);  // sequence 2 concealed
  EXPECT_FALSE(buffer.push(2, frame(2)));  // arrives after its concealment: late
  EXPECT_TRUE(buffer.push(3, frame(3)));
  auto next = buffer.pop();
  ASSERT_EQ(next.kind, pop_e::frame);
  EXPECT_EQ(next.payload, frame(3));
}

TEST(MicJitter, SkipsALargeGap) {
  jitter_buffer_t buffer;
  buffer.push(0, frame(0));
  buffer.push(1, frame(1));
  buffer.pop();
  buffer.push(100, frame(7));
  buffer.pop();
  auto next = buffer.pop();
  ASSERT_EQ(next.kind, pop_e::frame);
  EXPECT_EQ(next.payload, frame(7));
}
