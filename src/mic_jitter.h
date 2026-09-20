/**
 * @file src/mic_jitter.h
 * @brief Orders remote microphone frames by sequence and reports lost frames.
 */
#pragma once

// standard includes
#include <cstdint>
#include <map>
#include <vector>

namespace mic {
  class jitter_buffer_t {
  public:
    enum class pop_e {
      wait,  ///< Nothing to play: prebuffering, or the buffer is empty
      frame,  ///< payload holds the next Opus frame
      lost  ///< The next sequence is missing. Run loss concealment.
    };

    struct pop_result_t {
      pop_e kind;
      std::vector<std::uint8_t> payload;
    };

    static constexpr std::size_t PREBUFFER_FRAMES = 2;  // 40 ms
    static constexpr std::size_t MAX_FRAMES = 10;  // 200 ms

    /**
     * @brief Queue one frame.
     * @return false for a duplicate, or for a frame older than the playout point.
     */
    bool push(std::uint32_t sequence, std::vector<std::uint8_t> payload);

    /**
     * @brief Take the next frame. Call once per 20 ms.
     */
    pop_result_t pop();

    std::size_t size() const {
      return frames.size();
    }

  private:
    std::map<std::uint32_t, std::vector<std::uint8_t>> frames;
    bool started = false;
    std::uint32_t next_sequence = 0;
  };
}  // namespace mic
