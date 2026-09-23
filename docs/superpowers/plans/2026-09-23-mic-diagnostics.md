# Microphone diagnostics implementation plan (Apollo)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Every pong carries five counters that state what the host received, concealed, failed to decode, and handed to the virtual microphone.

**Architecture:** `playout_t` owns the counters as atomics, incremented on the network thread in `push` and on the render thread in `fill`. `send_pong` reads them and appends 20 big-endian bytes after the error code. A pong for a replaced session carries zeros.

**Tech Stack:** C++20, GoogleTest, the standalone macOS test target under `tests/mic_standalone/`, MSYS2 UCRT64 on Windows for the full build.

**Spec:** `docs/superpowers/specs/2026-09-23-mic-diagnostics-design.md`

## Global constraints

- Follow `.clang-format`: 2-space indent, no column limit, right-aligned pointers.
- The stream path diff against `master` stays empty. Only `src/mic*.cpp`, `src/mic*.h`, the mic tests, and the reference senders change.
- The wire error codes are unchanged. The payload grows only after byte 0.
- macOS check: `cmake -S tests/mic_standalone -B build-mic-standalone && cmake --build build-mic-standalone && ./build-mic-standalone/test_mic_standalone`.
- Windows build: prefix commands with `C:\msys64\msys2_shell.cmd -defterm -here -no-start -ucrt64 -use-full-path -c`, build directory `cmake-build-mic`, tests with `-DBUILD_TESTS=ON`.
- Never push to `upstream`.

---

### Task 1: Counters in playout

**Files:**
- Modify: `src/mic_playout.h`
- Modify: `src/mic_playout.cpp`
- Modify: `tests/unit/test_mic_playout.cpp`

**Interfaces:**
- Produces `mic::playout_t::counters_t { std::uint32_t accepted, rejected, concealed, decode_failures, frames_out; }` and `counters_t playout_t::counters() const`.

- [ ] **Step 1: Write the failing test**

```cpp
TEST(MicPlayout, CountsWhatItReceivesAndPlays) {
  playout_t playout;
  EXPECT_TRUE(playout.push(0, silence()));
  EXPECT_TRUE(playout.push(2, silence()));
  EXPECT_FALSE(playout.push(0, silence()));  // duplicate
  EXPECT_EQ(playout.fill(buffer.data(), buffer.size()), 960u);  // sequence 0
  EXPECT_EQ(playout.fill(buffer.data(), buffer.size()), 960u);  // sequence 1, concealed
  EXPECT_EQ(playout.fill(buffer.data(), buffer.size()), 960u);  // sequence 2
  auto counters = playout.counters();
  EXPECT_EQ(counters.accepted, 2u);
  EXPECT_EQ(counters.rejected, 1u);
  EXPECT_EQ(counters.concealed, 1u);
  EXPECT_EQ(counters.decode_failures, 0u);
  EXPECT_EQ(counters.frames_out, 3u);
}

TEST(MicPlayout, CountsADecodeFailure) {
  playout_t playout;
  playout.push(0, invalid());
  playout.push(1, silence());
  playout.fill(buffer.data(), buffer.size());
  EXPECT_EQ(playout.counters().decode_failures, 1u);
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run the macOS check from the global constraints.
Expected: compile failure, `counters` undefined.

- [ ] **Step 3: Implement**

In `mic_playout.h`, add `#include <atomic>` (already present for `failed`) and:

```cpp
    struct counters_t {
      std::uint32_t accepted;  ///< Frames the jitter buffer accepted
      std::uint32_t rejected;  ///< Frames rejected as late or duplicate
      std::uint32_t concealed;  ///< Frames Opus concealed for loss
      std::uint32_t decode_failures;
      std::uint32_t frames_out;  ///< Frames handed to the virtual microphone
    };

    /**
     * @brief The counters since construction. Safe from any thread.
     */
    counters_t counters() const {
      return {accepted.load(), rejected.load(), concealed.load(), failures_total.load(), frames_out.load()};
    }
```

Private members:

```cpp
    std::atomic<std::uint32_t> accepted {0};
    std::atomic<std::uint32_t> rejected {0};
    std::atomic<std::uint32_t> concealed {0};
    std::atomic<std::uint32_t> failures_total {0};
    std::atomic<std::uint32_t> frames_out {0};
```

In `mic_playout.cpp`: `push` increments `accepted` or `rejected` from the jitter result. In `fill`, the `lost` branch increments `concealed` before decoding. `samples < 0` increments `failures_total`. The success return increments `frames_out`. Each uses `fetch_add(1, std::memory_order_relaxed)`.

- [ ] **Step 4: Run the tests to verify they pass**

Run the macOS check. Expected: all suites pass, the two new tests included.

- [ ] **Step 5: Commit**

```bash
git add src/mic_playout.h src/mic_playout.cpp tests/unit/test_mic_playout.cpp
git commit -m "feat(mic): count accepted, rejected, concealed, failed, and played frames"
```

---

### Task 2: Pong payload

**Files:**
- Modify: `src/mic_protocol.h`
- Modify: `src/mic_protocol.cpp`
- Modify: `src/mic.cpp:265-273` (`send_pong`) and its two callers at lines 299 and 306
- Modify: `tests/unit/test_mic_protocol.cpp`

**Interfaces:**
- Consumes `playout_t::counters_t` (Task 1).
- Produces `std::string mic::protocol::pong_payload(error_e code, const pong_counters_t &counters)` and `struct pong_counters_t { std::uint32_t accepted, rejected, concealed, decode_failures, frames_out; }`.

- [ ] **Step 1: Write the failing test**

```cpp
TEST(MicProtocol, PongPayloadIsCodeThenFiveBigEndianCounters) {
  auto payload = pong_payload(error_e::decode_failed, {2480, 3, 12, 0, 2468});
  ASSERT_EQ(payload.size(), 21u);
  EXPECT_EQ(to_hex(payload), "03" "000009b0" "00000003" "0000000c" "00000000" "000009a4");
}
```

- [ ] **Step 2: Run it to verify it fails**

Run the macOS check. Expected: compile failure.

- [ ] **Step 3: Implement**

`mic_protocol.h`, after `error_e`:

```cpp
  /**
   * @brief Counters carried in every pong after the error code. Each is since the session began.
   */
  struct pong_counters_t {
    std::uint32_t accepted;
    std::uint32_t rejected;
    std::uint32_t concealed;
    std::uint32_t decode_failures;
    std::uint32_t frames_out;
  };

  constexpr std::size_t PONG_PAYLOAD_SIZE = 21;

  /**
   * @brief Build the pong payload: the error code, then the five counters as big-endian uint32.
   */
  std::string pong_payload(error_e code, const pong_counters_t &counters);
```

Update the `pong` enumerator comment to "carries one error_e byte and five counters".

`mic_protocol.cpp`:

```cpp
  std::string pong_payload(error_e code, const pong_counters_t &counters) {
    std::string payload;
    payload.reserve(PONG_PAYLOAD_SIZE);
    payload.push_back(static_cast<char>(code));
    for (auto value : {counters.accepted, counters.rejected, counters.concealed, counters.decode_failures, counters.frames_out}) {
      for (int shift = 24; shift >= 0; shift -= 8) {
        payload.push_back(static_cast<char>((value >> shift) & 0xFF));
      }
    }
    return payload;
  }
```

`mic.cpp` `send_pong` takes `const protocol::pong_counters_t &counters` as a fifth parameter and passes `protocol::pong_payload(error_code, counters)` as the payload. The session caller passes `to_pong(session->playout.counters())`, where

```cpp
    static protocol::pong_counters_t to_pong(const playout_t::counters_t &c) {
      return {c.accepted, c.rejected, c.concealed, c.decode_failures, c.frames_out};
    }
```

The replaced caller passes `{}`.

- [ ] **Step 4: Run the tests to verify they pass, then syntax-check mic.cpp**

Run the macOS check, then `python3 tests/mic_standalone/syntax_check.py src/mic.cpp`.
Expected: pass.

- [ ] **Step 5: Commit**

```bash
git add src/mic_protocol.h src/mic_protocol.cpp src/mic.cpp tests/unit/test_mic_protocol.cpp
git commit -m "feat(mic): carry the playout counters in every pong"
```

---

### Task 3: Reference senders and docs

**Files:**
- Modify: `tests/mic_standalone/send_test_tone.py`
- Modify: `docs/superpowers/specs/2026-09-20-calliope-mic-sidecar-design.md:225-226`
- Modify: `docs/remote_microphone.md`
- Modify: `docs/superpowers/2026-09-20-calliope-mic-handoff.md`

- [ ] **Step 1: Print the counters in the reference sender**

Where the sender prints `pong error code`, decode the rest:

```python
            if len(code) >= 21:
                accepted, rejected, concealed, failures, played = struct.unpack(">IIIII", code[1:21])
                print(f"pong error code: {code[0]}  accepted {accepted} rejected {rejected} concealed {concealed} decode failures {failures} played {played}")
            else:
                print("pong error code:", code[0])
```

Add `import struct`. Apply the same change to the Calliope copy at `docs/design/reference_sender.py` in the Calliope repository.

- [ ] **Step 2: Update the docs**

Sidecar design lines 225 to 226: "A pong payload is 1 byte holding the error code" becomes "A pong payload is 21 bytes: the error code, then five big-endian uint32 counters, accepted, rejected, concealed, decode failures, and frames played, since the session began. A client reads byte 0 alone from a shorter payload." Copy the changed file to `../Calliope/docs/design/`.

`docs/remote_microphone.md` Troubleshooting: add a row "Calliope shows Mic live and the PC hears nothing" with the action "Choose Copy diagnostics in Calliope. The Host counters line states whether the PC received and played the audio."

Handoff status table: add a row "Diagnostics: counters in the pong. Built and unit tested on macOS. Windows build and a live check against Calliope pending."

- [ ] **Step 3: Commit**

```bash
git add tests/mic_standalone/send_test_tone.py docs
git commit -m "docs(mic): describe the pong counters and print them in the reference sender"
```

---

### Task 4: Windows build and live check

Run on the PC.

- [ ] **Step 1: Build and run the mic tests**

```bash
cmake -B cmake-build-mic -G Ninja -S . -DBUILD_TESTS=ON
ninja -C cmake-build-mic
./cmake-build-mic/tests/test_sunshine --gtest_filter='MicProtocol.*:MicPlayout.*'
```

Expected: pass, no new warnings.

- [ ] **Step 2: Live check**

Run the built Apollo with its own config directory. Connect Calliope with the Task 5 client change. Expected: the PC line under the stats row shows received and played counts rising at about 50 a second while speaking, and 0 concealed on a wired LAN.

- [ ] **Step 3: Record the result in the handoff and commit**

```bash
git add docs/superpowers/2026-09-20-calliope-mic-handoff.md
git commit -m "docs(mic): record the Windows check of the pong counters"
```
