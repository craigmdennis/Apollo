/**
 * @file src/mic_playout.cpp
 * @brief Definitions for remote microphone playout.
 */
// lib includes
#include <opus/opus.h>

// local includes
#include "mic_playout.h"

namespace mic {
  playout_t::playout_t() {
    int status = OPUS_OK;
    decoder = opus_decoder_create(48000, 1, &status);
    if (status != OPUS_OK) {
      decoder = nullptr;
    }
  }

  playout_t::~playout_t() {
    if (decoder) {
      opus_decoder_destroy(decoder);
    }
  }

  bool playout_t::push(std::uint32_t sequence, std::vector<std::uint8_t> payload) {
    std::lock_guard lock {jitter_mutex};
    return jitter.push(sequence, std::move(payload));
  }

  std::size_t playout_t::fill(float *out, std::size_t capacity) {
    if (!decoder || capacity < static_cast<std::size_t>(FRAME_SAMPLES)) {
      return 0;
    }

    jitter_buffer_t::pop_result_t next;
    {
      std::lock_guard lock {jitter_mutex};
      next = jitter.pop();
    }

    if (next.kind == jitter_buffer_t::pop_e::wait) {
      waiting = true;
      return 0;
    }
    if (waiting) {
      // The sender was muted or silent. Decoding against the state of the last talkspurt
      // blends two unrelated signals, so the decoder starts clean.
      opus_decoder_ctl(decoder, OPUS_RESET_STATE);
      waiting = false;
    }

    int samples;
    if (next.kind == jitter_buffer_t::pop_e::frame && !next.payload.empty()) {
      samples = opus_decode_float(decoder, next.payload.data(), static_cast<opus_int32>(next.payload.size()), out, static_cast<int>(capacity), 0);
    } else {
      // A null payload asks Opus to conceal one lost frame of FRAME_SAMPLES.
      samples = opus_decode_float(decoder, nullptr, 0, out, FRAME_SAMPLES, 0);
    }

    if (samples < 0) {
      if (++decode_failures >= MAX_DECODE_FAILURES) {
        failed = true;
      }
      return 0;
    }

    decode_failures = 0;
    return static_cast<std::size_t>(samples);
  }
}  // namespace mic
