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
      wait,  ///< Nothing to play: prebuffering, or the talkspurt ended
      frame,  ///< payload holds the next Opus frame
      lost  ///< The next sequence is missing or the buffer ran dry. Run loss concealment.
    };

    struct pop_result_t {
      pop_e kind;
      std::vector<std::uint8_t> payload;
    };

    static constexpr std::size_t PREBUFFER_FRAMES = 2;  // 40 ms
    static constexpr std::size_t MAX_FRAMES = 5;  // 100 ms
    static constexpr int MAX_EMPTY_CONCEAL = 5;  // Opus concealment is silent by the fifth frame

    /**
     * @brief Queue one frame.
     * @return false for a duplicate, or for a frame older than the playout point.
     */
    bool push(std::uint32_t sequence, std::vector<std::uint8_t> payload);

    /**
     * @brief Take the next frame. The caller sets the pace: the render thread calls this when the device has room.
     */
    pop_result_t pop();

    std::size_t size() const {
      return frames.size();
    }

  private:
    std::map<std::uint32_t, std::vector<std::uint8_t>> frames;
    bool started = false;
    std::uint32_t next_sequence = 0;
    int empty_pops = 0;
  };
}  // namespace mic
