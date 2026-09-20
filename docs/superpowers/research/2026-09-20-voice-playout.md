# Voice playout research: UDP to jitter buffer to Opus decode to WASAPI

Scope: validate the draft Calliope host receiver design against established practice and
against three reference implementations of the same feature. Subject of review:

- `/Users/craigmdennis/Sites/Apollo/.superpowers/sdd/2026-09-20-calliope-host-receiver/task-2-brief.md` (jitter buffer)
- `/Users/craigmdennis/Sites/Apollo/.superpowers/sdd/2026-09-20-calliope-host-receiver/task-5-brief.md` (Windows writer)
- `/Users/craigmdennis/Sites/Apollo/.superpowers/sdd/2026-09-20-calliope-host-receiver/task-6-brief.md` (orchestrator)

Signal parameters assumed throughout: 48 kHz mono, 20 ms Opus frames, about 32 kbps,
one frame per encrypted UDP datagram, 50 packets per second, home Wi-Fi LAN only.

---

## Verified from source

### Established practice is pull: the audio device clock is the playout clock

WebRTC NetEq, the most widely deployed voice jitter buffer, is pulled by the audio
sink. Its design document states that `GetAudio` "pulls 10 ms of audio from NetEq for
playout" and that the decision to decode the next packet, to accelerate, to decelerate
or to conceal is taken inside that pull
(https://webrtc.googlesource.com/src/+/main/modules/audio_coding/neteq/g3doc/index.md,
mirrored at
https://raw.githubusercontent.com/webrtc-mirror/webrtc/main/modules/audio_coding/neteq/g3doc/index.md).
The same document is explicit that the arrival statistics are measured against that
pull: "The interarrival time is measured in the number of GetAudio 'ticks' and thus
clock drift between the sender and receiver can be accounted for." There is no timer in
the design; the sink's cadence is the reference.

On Windows the equivalent tick is the WASAPI render event. Microsoft documents
`AUDCLNT_STREAMFLAGS_EVENTCALLBACK` as a flag "that enables an application's
buffer-servicing thread to schedule its execution to occur when a new buffer becomes
available from the audio device", and states that "If the audio adapter is controlled by
a WaveRT driver, the signaling of the event handle is tied to the DMA-transfer
notifications from the audio hardware"
(https://learn.microsoft.com/en-us/windows/win32/coreaudio/exclusive-mode-streams). The
event therefore carries the device clock.

Apollo's own outbound audio path already follows this rule. `src/audio.cpp` has no
timer; the loop blocks on the capture device and is paced by it:

```cpp
      auto status = mic->sample(sample_buffer);
      switch (status) {
        case platf::capture_e::ok:
          break;
        case platf::capture_e::timeout:
          continue;
```

(`/Users/craigmdennis/Sites/Apollo/src/audio.cpp:226`)

All three reference implementations of remote microphone receive are device-clocked or
packet-clocked, and none introduces a timer.

- Apollo pull request #1428 (`logabell/apollo-microphone`) decodes inside the WASAPI
  render thread. `render_loop` waits on the render event, queries
  `GetCurrentPadding`, and only then calls `decode_next_packet()` until it has enough
  PCM: "`if (queued_frames >= target_prebuffer_frames || queued_packets == 0) break;`
  ... `if (!decode_next_packet()) break;`"
  (local copy at
  `/private/tmp/claude-501/-Users-craigmdennis-Sites-Apollo/eb1b5035-4b2e-4ccf-bcec-e4c161cbc278/scratchpad/pr/mic_write.cpp:826-841`).
  `write_data` does no decoding at all; it inserts into `pending_packets` and signals the
  render event: "`SetEvent(render_event.get());`" (same file, line 645).
- Vibepollo decodes on packet arrival and drains the decoded PCM from the render thread.
  The render loop contains only a PCM prebuffer and the WASAPI write:
  "`static constexpr std::size_t kPrebufFrames = 960 * 2;  // 2 Opus packets — matches
  mic_buffer_packets default`"
  (https://raw.githubusercontent.com/xenstalker02/Vibepollo/master/src/platform/windows/mic_write.cpp:444).
  The jitter buffer and Opus decode live in the packet handler
  (https://raw.githubusercontent.com/xenstalker02/Vibepollo/master/src/stream.cpp:1565-1600).
- `moonlight-common-c` does not buffer for playout at all on the client side. Its
  reassembly window is `AudioPacketDuration * RTPA_DATA_SHARDS + RTPQ_OOS_WAIT_TIME_MS`
  (https://raw.githubusercontent.com/moonlight-stream/moonlight-common-c/master/src/RtpAudioQueue.c:547),
  with `RTPA_DATA_SHARDS 4` and `RTPQ_OOS_WAIT_TIME_MS 10`
  (https://raw.githubusercontent.com/moonlight-stream/moonlight-common-c/master/src/RtpAudioQueue.h),
  and it hands decoded samples straight to the platform renderer.

No source found in this review describes a production voice receiver that pops one frame
per fixed software timer tick.

### A 20 ms `boost::asio::steady_timer` on Windows does not have 20 ms granularity

The default Windows timer interrupt interval is 15.625 ms — "the _default_ interval has
been 15.625 ms (1,000 ms divided by 64)"
(https://randomascii.wordpress.com/2020/10/04/windows-timer-resolution-the-great-rule-change/,
and https://randomascii.wordpress.com/2013/07/08/windows-timer-resolution-megawatts-wasted/).
Asio does not opt into the high-resolution waitable timer: issue #1328,
"Support high resolution waitable timers on Windows", is open and requests
`CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` support, which asio currently lacks
(https://github.com/chriskohlhoff/asio/issues/1328).

Apollo does raise the system timer resolution, but only for the duration of a game
stream. `nt_set_timer_resolution_max()` is called from `streaming_will_start()` and
reverted in `streaming_will_stop()`:

```cpp
    // Reduce timer period to 0.5ms
    if (nt_set_timer_resolution_max()) {
```

(`/Users/craigmdennis/Sites/Apollo/src/platform/windows/misc.cpp:1141`, inside
`streaming_will_start()` at line 1108; reverted at line 1227 inside
`streaming_will_stop()`.)

The mic session in the draft is deliberately independent of streaming
(task-6-brief.md: "This module never touches streaming code"), so it can and will run
with the system timer at its 15.625 ms default.

Apollo already owns a platform abstraction for this exact problem,
`platf::create_high_precision_timer()` returning `win32_high_precision_timer`, which does
use `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`
(`/Users/craigmdennis/Sites/Apollo/src/platform/windows/misc.cpp:1809-1856`). The draft
does not use it.

### Clock drift magnitudes for consumer hardware

Consumer crystal parts are specified in the tens of ppm. A 24.576 MHz audio crystal is
sold with ±30 ppm frequency tolerance plus ±50 ppm temperature stability
(https://www.microscale.net/products/24-576mhz-crystal); the Abracon ASE oscillator
series offers overall stability options including ±50 ppm
(https://abracon.com/Oscillators/ASEseries.pdf). Two independent devices can therefore
differ by roughly 100 ppm worst case, and the quantity is conventionally expressed in ppm
(https://en.wikipedia.org/wiki/Crystal_oscillator).

Arithmetic at these figures, for 20 ms frames and 48 kHz:

| Relative offset | Time to accumulate one 20 ms frame | Time to accumulate 40 ms (the prebuffer) | Time to accumulate 200 ms (the cap) |
|---|---|---|---|
| 20 ppm | 16.7 min | 33 min | 2.8 h |
| 50 ppm | 6.7 min | 13.3 min | 1.1 h |
| 100 ppm | 3.3 min | 6.7 min | 33 min |

Two clock pairs matter.

- **Sender capture clock against host render device clock.** Unavoidable in any design.
- **Host `steady_timer` (system/QPC time base) against host render device clock.** Present
  only in the push design, and self-inflicted. These are separate oscillators on the same
  machine; the offset is of the same order as the first pair.

### Opus packet loss concealment contract

`opus.h` states: "Lost packets can be replaced with loss concealment by calling the
decoder with a null pointer and zero length for the missing packet"
(https://raw.githubusercontent.com/xiph/opus/main/include/opus.h:420-423), and for
`opus_decode_float`: "In the case of PLC (data==NULL) or FEC (decode_fec=1), then
frame_size needs to be exactly the duration of audio that is missing, otherwise the
decoder will not be in the optimal state to decode the next incoming packet. For the PLC
and FEC cases, frame_size **must** be a multiple of 2.5 ms" (same file, lines 556-561).

Verified empirically against libopus 1.6.1 (probe compiled and run locally; throwaway
source in the session scratchpad):

```
PLC(NULL,0,fs=960)  -> 960
PLC(NULL,0,fs=5760) -> 5760
PLC(NULL,0,fs=100 not 2.5ms mult) -> -1 (invalid argument)
```

So the draft's call `opus_decode_float(decoder, nullptr, 0, pcm, 960, 0)` is exactly
correct for a lost 20 ms frame.

The same probe measured PLC decay after ten voiced frames at 32 kbps:

```
last good frame peak=0.3022
PLC frame  1: peak=0.30155
PLC frame  2: peak=0.20523
PLC frame  3: peak=0.07230
PLC frame  4: peak=0.00913
PLC frame  5: peak=0.00034
PLC frame  6: peak=0.00006
```

Opus PLC therefore fades to effective silence within about five frames (100 ms).
Concealing more than about five consecutive frames produces nothing but silence.

### The fixed 960-sample decode buffer rejects any packet longer than 20 ms

Verified with the same probe against libopus 1.6.1:

```
20ms voip 32k packet len=113 toc=0x78
decode 20ms into fs=960 -> 960
40ms packet (len=151) into fs=960 -> -2 (buffer too small)
```

`-2` is `OPUS_BUFFER_TOO_SMALL`. The draft's `play_one_frame` passes
`FRAME_SAMPLES` (960) as the output capacity for the normal decode path
(task-6-brief.md, `opus_decode_float(session->decoder, next.payload.data(), ...,
FRAME_SAMPLES, 0)`). Both reference implementations size the normal-decode output buffer
to the Opus maximum instead: pull request #1428 uses
`constexpr std::uint32_t max_packet_duration_samples = 5760;` and decodes into
`decoded_pcm.size()` (scratchpad copy, lines 45 and 753-760); Vibepollo derives the size
from `opus_packet_get_nb_samples` and clamps to 5760
(https://raw.githubusercontent.com/xenstalker02/Vibepollo/master/src/stream.cpp:1507-1522).

### `0xF8 0xFF 0xFE` is a valid 20 ms Opus silence frame

Discord documents the convention: "send five frames of silence (`0xF8, 0xFF, 0xFE`)
before stopping to avoid unintended Opus interpolation"
(https://docs.discord.com/developers/topics/voice-connections).

RFC 6716 section 3.1 defines the TOC byte as five `config` bits, one stereo bit `s`, and
a two-bit frame count code `c` (https://www.rfc-editor.org/rfc/rfc6716.html). `0xF8` is
`11111 0 00`: config 31, mono, code 0 (one frame). Config 31 is CELT-only, fullband,
20 ms. The remaining two bytes `0xFF 0xFE` are that single frame's compressed payload.

Verified against libopus 1.6.1:

```
silence: nb_frames=1 nb_samples@48k=960 bw=0x451 ch=1
decode silence -> 960 samples, peak=2.03459e-34
```

`0x451` is `OPUS_BANDWIDTH_FULLBAND`. The packet is a single mono 20 ms CELT-only
fullband frame that decodes to 960 samples of effective silence. Note for contrast that
Opus DTX produces one-byte packets, not this three-byte one (probe: `dtx silence packet
len=1`), so `0xF8 0xFF 0xFE` is a hand-written convention rather than an encoder output.

### Jitter buffer sizing in the reference implementations

| Implementation | Prebuffer | Packet cap | PCM cap |
|---|---|---|---|
| Draft under review | 2 frames (40 ms) | 10 frames (200 ms) | 200 ms deque |
| Apollo PR #1428 | `target_prebuffer_packets = 4` (80 ms) | `max_queued_packets = 64` (1280 ms) | `max_queued_frames = decoded_sample_rate` (1000 ms) |
| Vibepollo | `mic_buffer_packets` default 2 (40 ms), range 2–16 | `mic_max_queued_packets = 32` (640 ms) | 48000 samples (1000 ms) |
| `moonlight-common-c` | none | 30-entry decode queue; 30 ms reassembly window | platform renderer |
| WebRTC NetEq | adaptive, 95th percentile of arrival delay | `max_packets_in_buffer = 200` | n/a |

Sources: scratchpad copy of `mic_write.cpp` lines 46-49; Vibepollo `src/config.cpp:841`
("`2,   // mic_buffer_packets (default 2 = 40ms prebuffer; smooths packet delivery
jitter)`") and `src/config.cpp:1709` (`int_between_f(vars, "mic_buffer_packets", ...,
{2, 16})`) and `src/stream.h:510` (`static constexpr std::size_t
mic_max_queued_packets = 32;`); `moonlight-common-c` `src/AudioStream.c:69`
(`LbqInitializeLinkedBlockingQueue(&packetQueue, 30);`); WebRTC
`api/neteq/neteq.h:134` (`size_t max_packets_in_buffer = 200;`) and
`modules/audio_coding/neteq/delay_manager.h:33` (`double quantile = 0.95;`).

Vibepollo's README states the shipped behaviour directly: "`mic_buffer_packets` | `2` |
Jitter buffer prebuffer depth in Opus packets (1 packet = 20ms). Default 2 = 40ms
prebuffer. Range 1–16" and "Jitter buffer (40ms prebuffer, 2 packets default) → Opus
decode with PLC on packet loss"
(https://raw.githubusercontent.com/xenstalker02/Vibepollo/master/README.md).

### Delay budget guidance

ITU-T G.114, as summarised by Cisco, bands one-way delay: 0-150 ms "Acceptable for most
user applications", 150-400 ms "Acceptable provided that administrators are aware of the
transmission time", above 400 ms "Unacceptable for general network planning purposes"
(https://www.cisco.com/c/en/us/support/docs/voice/voice-quality/5125-delay-details.html;
the recommendation itself is at https://www.itu.int/rec/T-REC-G.114-200305-I/en).

The same document gives a sizing rule for de-jitter buffers: "The initial playout delay
is configurable. The maximum depth of the buffer before it overflows is normally set to
1.5 or 2.0 times this value."

### Neither reference implementation re-prebuffers after an underrun

In pull request #1428, `has_playout_cursor` is set once in `decode_next_packet` and is
never cleared there, and `playout_started` is set once in `render_loop` and never reset
(scratchpad copy, lines 703-711 and 852-869). Vibepollo likewise sets
`mic.has_playout_cursor = true` once
(https://raw.githubusercontent.com/xenstalker02/Vibepollo/master/src/stream.cpp:1568-1572).
An empty buffer in both simply produces no output that tick; playout resumes from the
next packet without a fresh prebuffer.

Both also decline to conceal when the buffer is empty. PR #1428:

```cpp
  bool mic_write_wasapi_t::should_conceal_missing_packet_locked() const {
    if (pending_packets.empty()) {
      return false;
    }
```

(scratchpad copy, line 658.) Vibepollo's equivalent is the `else break;` arm of its drain
loop. So the draft's rule "conceal only when a later frame is queued" matches both; it is
only the prebuffer reset that diverges.

### In-band FEC behaviour

`opus_defines.h` documents `OPUS_SET_INBAND_FEC`: "Inband FEC disabled (default)" for 0,
and for 1, "Inband FEC enabled. If the packet loss rate is sufficiently high, Opus will
automatically switch to SILK even at high rates to enable use of that FEC"
(https://raw.githubusercontent.com/xiph/opus/main/include/opus_defines.h:516-526).

Measured with libopus 1.6.1 at 32 kbps mono, 20 ms frames:

```
FEC on:  avg packet 73 bytes, bandwidth=0x451 toc=0x78   (config 15, hybrid)
FEC off: avg packet 80 bytes, bandwidth=0x451 toc=0xf8   (config 31, CELT-only)
```

Enabling FEC with `OPUS_SET_PACKET_LOSS_PERC(10)` changed the operating mode from
CELT-only to hybrid, as documented. Note the TOC of the FEC-off case, `0xf8`, is the same
configuration as the Discord silence frame.

Decoding FEC also requires the *next* packet to be in hand, which is how both references
use it: PR #1428 looks up `expected_sequence_number + 1` and calls with `decode_fec = 1`
(scratchpad copy, lines 727-751); Vibepollo does the same
(https://raw.githubusercontent.com/xenstalker02/Vibepollo/master/src/stream.cpp:1585-1588).

### `OPUS_RESET_STATE`

"Resets the codec state to be equivalent to a freshly initialized state. This should be
called when switching streams in order to prevent the back to back decoding from giving
different results from one at a time decoding"
(https://raw.githubusercontent.com/xiph/opus/main/include/opus_defines.h:705-710).

None of the three reference implementations resets the decoder on a gap.

---

## Inferred

These follow from the verified material but were not observed running.

### Concrete failure of the push design: uncontrolled, unbounded playout latency

The draft has three rate sources: the sender's capture clock feeding the jitter buffer,
the host 20 ms timer moving frames from the jitter buffer into the deque, and the device
clock draining the deque. Neither intermediate buffer has a level controller; each has
only a hard cap and a drop-oldest rule.

- If the timer runs faster than the device (well within 100 ppm), the deque grows until it
  pins at its 200 ms cap and stays there. `write()` then discards the excess on every
  call, so the design self-corrects the rate but at the cost of a permanent 200 ms of
  added latency that was never designed in.
- Independently, if the sender runs faster than the timer, the jitter buffer pins at its
  own 200 ms cap.
- Both can be positive at once (sender fastest, timer in the middle, device slowest), in
  which case steady-state added latency settles near 400 ms and crosses G.114's
  "unacceptable" boundary, with no diagnostic and no way back down.
- With the opposite signs, the deque empties and the render thread writes nothing, or the
  jitter buffer empties and the draft's `started = false` forces a fresh 40 ms prebuffer.
  At 100 ppm the 40 ms prebuffer drains in about 6.7 minutes, so a timer-faster-than-sender
  machine stalls for 40 ms roughly every seven minutes for no network reason at all.

The pull design removes the middle rate source entirely. The only remaining pair is
sender against device, which no design can remove, and its correction cost is one frame
every 3.3 to 16.7 minutes at 20-100 ppm.

### The Windows timer granularity makes the push design bursty, not merely imprecise

`timer->expires_at(timer->expiry() + TICK)` keeps the average rate exact against
`steady_clock`, so quantisation does not cause rate drift. It causes phase bursts: with a
15.625 ms interrupt period, successive 20 ms deadlines land on interrupt ticks at
31.25 ms, 46.875 ms, 62.5 ms and so on, so the handler runs zero times in some intervals
and twice in others. With only a 2-frame prebuffer, a zero-then-double pattern can empty
the jitter buffer, and the draft's `started = false` then triggers a fresh 40 ms
prebuffer. The plausible outcome on a Windows host with no game stream running is
repeated prebuffer restarts under no packet loss at all. This is a prediction and needs a
Windows run to confirm.

### Smallest correct design

Replace the timer with the render event, and delete the PCM deque.

Ownership and locking:

- `mic::jitter_buffer_t` stays exactly as specified in task-2-brief.md: a pure class with
  `push(sequence, payload)` / `pop()` / `size()`, no threads, no time, no Opus. Its unit
  tests need no change. This is the right boundary and the draft got it right.
- The io_context thread keeps the socket and calls `push()` only.
- The WASAPI render thread calls `pop()`, then `opus_decode_float`, then writes into the
  render buffer. It is the only thread that touches the decoder.
- One `std::mutex` guards the jitter buffer alone. `push()` under the lock costs a map
  insert; `pop()` under the lock costs a map erase. The Opus decode happens after the
  lock is released, because only the render thread ever decodes.
- One residual buffer of at most 960 floats lives in the render thread, because
  `GetCurrentPadding` yields an arbitrary frame count that is not a multiple of 960.
- Shutdown ordering is already correct in the draft: `end_session` does
  `session->vmic.reset()` (which joins the render thread) before
  `opus_decoder_destroy(session->decoder)`. That ordering becomes load-bearing under pull
  and should carry a comment saying so.

Interface change in `src/platform/common.h`: `virtual bool write(const float *, std::size_t)`
becomes a fill callback supplied at construction, for example
`std::unique_ptr<virtual_mic_t> virtual_mic(std::function<std::size_t(float *out, std::size_t frames)> fill, virtual_mic_error_e &error)`,
where the render thread calls `fill` for the frames the device has room for and
zero-fills any shortfall. The Linux and macOS stubs are unaffected. `previous_default_capture()`
is unaffected.

What this deletes from the draft: `TICK`, the `steady_timer`'s 20 ms cadence, `on_tick`'s
call to `play_one_frame`, `play_one_frame` itself in its current form, `std::deque<float> queue`,
`queue_mutex`, `MAX_QUEUED_SAMPLES`, and the whole second cap. A `steady_timer` at 1 s is
still wanted for the `replaced` expiry and the 5 s session timeout; those are housekeeping
and 15.625 ms granularity is irrelevant to them.

### Wi-Fi jitter and the 40 ms prebuffer

ITU-T guidance treats jitter under 20 ms as good for VoIP and 20-50 ms as acceptable. A
40 ms prebuffer therefore covers the normal case and the lower half of the acceptable
band. 802.11 adds structured tail latency that a fixed 40 ms will not cover: power-save
deferral can add a beacon interval or more, commonly 100 ms
(https://dot11zen.blogspot.com/2018/02/80211-power-management-with-packet.html), and
reported measurements of home station-to-AP links put a large share of packets above
20 ms and a tail above 100 ms
(https://arxiv.org/pdf/2006.15514). The tail source is weakly attributed and should be
treated as indicative only.

The practical reading: 40 ms is the right default for a quiet LAN and matches Vibepollo's
shipped default, but a user on a congested channel or with a phone that dozes will need
more, and the cheapest substitute for adaptivity is the configuration knob Vibepollo
already ships.

The 40 ms / 200 ms pairing is the part that is not supported. Cisco's rule of thumb from
G.114 puts the overflow depth at 1.5 to 2.0 times the initial playout delay, which for a
40 ms prebuffer means 60-80 ms, not 200 ms. The references cap much higher (640 ms,
1280 ms) but they also cap a queue that only ever holds packets not yet needed, and they
do not stack a second 200 ms PCM cap behind it. Under pull, with the PCM deque gone, a
cap of 5 frames (100 ms) is both closer to the rule and closer to what the buffer can
usefully hold before the delay is worse than the dropout.

### Drift compensation: drop-oldest and re-prebuffer are acceptable, with one change

At 20-100 ppm, sender-against-device drift requires a single 20 ms correction every 3.3 to
16.7 minutes. One dropped 20 ms frame, or one concealed 20 ms frame, at that rate is
inaudible in conversation. NetEq's accelerate/expand time-stretching
(https://webrtc.googlesource.com/src/+/main/modules/audio_coding/neteq/g3doc/index.md)
is the quality-preserving alternative, and it is a large amount of machinery for an event
that happens four times an hour. Fixed sizing with frame-granular correction is the right
call for version 1.

Two qualifications.

- Correction must be one frame at a time, not a burst. The draft's `push()` already does
  this (`while (frames.size() > MAX_FRAMES) frames.erase(frames.begin());` removes exactly
  the overflow, which under steady drift is one frame). Good as written.
- The draft's `started = false` on empty converts every momentary underrun into a 40 ms
  stall. Under pull, with no timer bursts, this is rare; it is still worth bounding.
  Because Opus PLC decays to silence in about five frames (measured above), the natural
  rule is: conceal while the buffer is empty for up to about five frames, then declare the
  talkspurt over and re-prebuffer. Concealing beyond that produces silence anyway, so
  there is nothing to lose and a single late packet no longer costs 40 ms.

### In-band FEC is not worth it on this LAN for version 1

- The encoder lives in Calliope, a separate codebase, so `decode_fec` on the host is inert
  until the sender opts in. Version 1 cannot buy anything by enabling the decoder flag
  alone.
- Enabling it changes the Opus operating mode from CELT-only to hybrid at 32 kbps
  (measured above), which changes the codec's characteristics for a loss rate a home LAN
  largely does not have, since 802.11 already performs link-layer retransmission.
- PLC at 20 ms frames is a good substitute for isolated losses, and the loss pattern that
  FEC does not help with — a burst long enough to matter — is also the pattern PLC cannot
  help with.

Keep `decode_fec = 0`. Revisit only if measurement on the target network shows non-trivial
isolated loss.

### Decoder state across a mute gap

Calliope sends nothing while muted, so the host makes no decode calls at all during the
gap and the decoder state is frozen at the last voiced frame. On unmute, the first decode
runs against that stale state and Opus's overlap-add blends across a gap of arbitrary
length. This is precisely the artefact the Discord silence-frame convention avoids on the
sender side.

The minimal host-side equivalent is one line at the point where the jitter buffer leaves
the prebuffering state after having been empty:
`opus_decoder_ctl(session->decoder, OPUS_RESET_STATE);`. This is consistent with the
documented purpose ("should be called when switching streams in order to prevent the back
to back decoding from giving different results from one at a time decoding"), it is free,
and it is a no-op in the case where the host had already concealed the gap down to
silence. No reference implementation does this, so it is an addition rather than a
correction; it has no downside but has not been A/B tested for audibility.

The alternative, which is a sender change, is for Calliope to send five `0xF8 0xFF 0xFE`
frames before going quiet, as Discord specifies. That is strictly better but is out of
scope for the host receiver.

---

## Recommended changes to the draft

1. **Replace the push timer with pull from the WASAPI render thread.** Delete `TICK`'s
   20 ms cadence, `play_one_frame`'s call from `on_tick`, `std::deque<float> queue`,
   `queue_mutex` and `MAX_QUEUED_SAMPLES` from task-5-brief.md and task-6-brief.md.
   Evidence: NetEq is pulled by `GetAudio`
   (https://webrtc.googlesource.com/src/+/main/modules/audio_coding/neteq/g3doc/index.md);
   the WASAPI render event is tied to hardware DMA notifications
   (https://learn.microsoft.com/en-us/windows/win32/coreaudio/exclusive-mode-streams);
   PR #1428 decodes inside `render_loop` (scratchpad `mic_write.cpp:786-841`); Apollo's
   own `src/audio.cpp:226` is device-clocked; and a 20 ms asio timer on Windows has
   15.625 ms granularity outside a game stream
   (https://randomascii.wordpress.com/2020/10/04/windows-timer-resolution-the-great-rule-change/,
   https://github.com/chriskohlhoff/asio/issues/1328,
   `/Users/craigmdennis/Sites/Apollo/src/platform/windows/misc.cpp:1141`).
2. **Keep `jitter_buffer_t` exactly as specified.** The `push`/`pop`/`size` interface, the
   `pop_e` enum and all nine unit tests carry over unchanged. Keep as is.
3. **Change `platf::virtual_mic_t` from `write(const float *, std::size_t)` to a fill
   callback supplied at construction**, and have the render thread zero-fill any shortfall.
   Consequence of change 1.
4. **Guard the jitter buffer with one `std::mutex`; leave the decoder unguarded and owned
   by the render thread.** Add a comment to `end_session` recording that
   `session->vmic.reset()` must precede `opus_decoder_destroy` because the reset joins the
   render thread. The draft's existing ordering is already correct (task-6-brief.md,
   `end_session`).
5. **Size the normal decode output buffer to 5760 floats, not 960.** Verified locally: a
   40 ms packet decoded into a 960-sample buffer returns `-2` (`OPUS_BUFFER_TOO_SMALL`),
   which after `MAX_DECODE_FAILURES` would kill the session. Both references use 5760
   (scratchpad `mic_write.cpp:45`; Vibepollo `src/stream.cpp:1507-1522`).
6. **Keep the PLC call exactly as written.** `opus_decode_float(decoder, nullptr, 0, pcm,
   960, 0)` matches the documented contract and returns 960 in practice
   (https://raw.githubusercontent.com/xiph/opus/main/include/opus.h:556-561). Keep as is.
7. **Lower `MAX_FRAMES` from 10 (200 ms) to 5 (100 ms)**, now that the second 200 ms PCM
   cap is gone. Evidence: the G.114 de-jitter rule of thumb puts overflow depth at 1.5-2.0
   times the 40 ms playout delay
   (https://www.cisco.com/c/en/us/support/docs/voice/voice-quality/5125-delay-details.html),
   and Opus PLC is silent past five frames anyway.
8. **Bound the re-prebuffer: conceal up to about five consecutive empty frames before
   returning `wait` and resetting `started`.** Neither reference resets the prebuffer at
   all (scratchpad `mic_write.cpp:703-711`; Vibepollo `src/stream.cpp:1568-1572`), and
   measured PLC decays to silence by frame five. One new `pop()` behaviour, one new unit
   test.
9. **Call `opus_decoder_ctl(decoder, OPUS_RESET_STATE)` when playout restarts after the
   buffer has been empty** (the mute-gap case).
   (https://raw.githubusercontent.com/xiph/opus/main/include/opus_defines.h:705-710).
10. **Expose the prebuffer depth as a config integer, default 2, range 2-16**, mirroring
    Vibepollo's `mic_buffer_packets`
    (https://raw.githubusercontent.com/xenstalker02/Vibepollo/master/src/config.cpp:1709).
    This is the cheap substitute for an adaptive buffer; NetEq's adaptive machinery
    (https://webrtc.googlesource.com/src/+/main/modules/audio_coding/neteq/g3doc/index.md)
    is out of proportion for version 1.
11. **Keep `decode_fec = 0` and do not ask Calliope to enable `OPUS_SET_INBAND_FEC`.**
    Measured: enabling it forces a CELT-to-hybrid mode change at 32 kbps, and the decoder
    flag is inert without a sender change
    (https://raw.githubusercontent.com/xiph/opus/main/include/opus_defines.h:516-526).
    Keep as is.
12. **Keep the `steady_timer`, retuned to 1 s, for the `replaced` expiry and the 5 s
    session timeout only.** Timer granularity is immaterial at that period.
13. **Keep the drop-oldest-on-overflow rule.** It removes exactly one frame per excess
    push, and at 20-100 ppm that is one 20 ms correction every 3.3 to 16.7 minutes. Keep
    as is.

## Still unverified

- No Windows run was performed. The prediction that 15.625 ms timer granularity causes
  repeated prebuffer restarts in the push design is reasoning from the documented
  interrupt period, not an observation.
- The audibility of a single dropped or concealed 20 ms frame every few minutes is argued
  from the PLC decay measurement and from G.114 delay bands, not from a listening test.
- The claim that a large share of home Wi-Fi station-to-AP packets exceed 20 ms, with a
  tail above 100 ms, came through a search summary attributed to
  https://arxiv.org/pdf/2006.15514; the primary text was not read. No measurement of
  jitter for 50 pps small UDP packets on the specific target network was found or made.
- ITU-T G.114's own text was not read; the delay bands and the 1.5-2.0x de-jitter sizing
  rule are quoted from Cisco's summary
  (https://www.cisco.com/c/en/us/support/docs/voice/voice-quality/5125-delay-details.html).
- Actual ppm offsets between the Calliope device and the target Windows PC's audio clock
  were not measured; the 20-100 ppm range comes from component datasheets, not from these
  two devices.
- The exact mechanism by which asio's Windows backend waits (a millisecond timeout on
  `GetQueuedCompletionStatus` derived from the timer queue) was not read in the asio
  source. Issue #1328 confirms asio does not use `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`,
  which is sufficient for the conclusion but not for the mechanism.
- `moonlight-mic` (`JimothySnicket/moonlight-mic`) carries its host code in a git
  submodule that was not fetched; only `ARCHITECTURE.md` was read
  (https://raw.githubusercontent.com/JimothySnicket/moonlight-mic/main/ARCHITECTURE.md).
  It confirms the same 20 ms / 960-sample / mono / VOIP parameters but its jitter buffer,
  if any, was not inspected.
- Whether Calliope keeps sending ping packets while muted (which the 5 s session timeout
  depends on) was not confirmed from the sender's design.
