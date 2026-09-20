/**
 * @file src/mic_jitter.cpp
 * @brief Definitions for the remote microphone jitter buffer.
 */
#include "mic_jitter.h"

namespace mic {
  bool jitter_buffer_t::push(std::uint32_t sequence, std::vector<std::uint8_t> payload) {
    if (started && sequence < next_sequence) {
      return false;
    }
    if (!frames.emplace(sequence, std::move(payload)).second) {
      return false;
    }

    while (frames.size() > MAX_FRAMES) {
      frames.erase(frames.begin());
    }
    return true;
  }

  jitter_buffer_t::pop_result_t jitter_buffer_t::pop() {
    if (!started) {
      if (frames.size() < PREBUFFER_FRAMES) {
        return {pop_e::wait, {}};
      }
      started = true;
      next_sequence = frames.begin()->first;
    }

    if (frames.empty()) {
      // A momentary underrun is concealed. Restarting the prebuffer for it would add a 40 ms stall.
      if (++empty_pops > MAX_EMPTY_CONCEAL) {
        started = false;
        empty_pops = 0;
        return {pop_e::wait, {}};
      }
      ++next_sequence;
      return {pop_e::lost, {}};
    }
    empty_pops = 0;

    auto first = frames.begin()->first;
    if (first > next_sequence && (first - next_sequence > MAX_FRAMES || frames.size() == MAX_FRAMES)) {
      // Either the sender moved far ahead, or dropping the oldest frames moved the queue past
      // the playout point. Concealing every missing frame would add delay, so skip to the queue.
      next_sequence = first;
    }

    auto it = frames.find(next_sequence);
    ++next_sequence;
    if (it == frames.end()) {
      return {pop_e::lost, {}};
    }

    pop_result_t result {pop_e::frame, std::move(it->second)};
    frames.erase(it);
    return result;
  }

}  // namespace mic
