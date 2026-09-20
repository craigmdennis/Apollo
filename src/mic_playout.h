/**
 * @file src/mic_playout.h
 * @brief Jitter buffer plus Opus decoder for one mic session.
 */
#pragma once

// standard includes
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

// local includes
#include "mic_jitter.h"

struct OpusDecoder;

namespace mic {
  /**
   * @brief Turns queued Opus frames into 48 kHz mono float audio.
   *
   * push() runs on the network thread. fill() runs on the audio render thread and is the
   * only user of the decoder. One mutex guards the jitter buffer between the two.
   */
  class playout_t {
  public:
    static constexpr int FRAME_SAMPLES = 960;  // 20 ms at 48 kHz, the duration concealed for one lost frame
    static constexpr int MAX_DECODE_FAILURES = 25;  // 500 ms of consecutive failures

    playout_t();
    ~playout_t();

    playout_t(const playout_t &) = delete;
    playout_t &operator=(const playout_t &) = delete;

    /**
     * @return false when the Opus decoder could not be created.
     */
    bool ready() const {
      return decoder != nullptr;
    }

    /**
     * @brief Queue one Opus frame.
     * @return false for a duplicate or late frame.
     */
    bool push(std::uint32_t sequence, std::vector<std::uint8_t> payload);

    /**
     * @brief Decode the next frame into out.
     * @param capacity Samples available in out. Use platf::VIRTUAL_MIC_MAX_PACKET_SAMPLES.
     * @return The number of samples written, or 0 when nothing is ready to play.
     */
    std::size_t fill(float *out, std::size_t capacity);

    /**
     * @return true after MAX_DECODE_FAILURES consecutive decode failures. Safe from any thread.
     */
    bool decode_failed() const {
      return failed;
    }

  private:
    std::mutex jitter_mutex;
    jitter_buffer_t jitter;

    // Touched only by the thread that calls fill().
    OpusDecoder *decoder = nullptr;
    bool waiting = true;
    int decode_failures = 0;

    std::atomic<bool> failed {false};
  };
}  // namespace mic
