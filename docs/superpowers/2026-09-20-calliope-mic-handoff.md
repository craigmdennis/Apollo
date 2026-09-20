# Remote microphone: handoff for the Windows build

Branch: `feature/calliope-mic`, forked from `master` at `1a59548c`.
Remote: `origin` (`craigmdennis/Apollo`). Never push to `upstream` (`ClassicOldSong/Apollo`). Confirm the destination before every push.

## What this branch adds

Apollo gains a remote microphone receiver. A companion app named Calliope (iOS and macOS, not yet written) pairs with Apollo over HTTPS, then sends AES-128-GCM encrypted Opus frames over UDP port 48002. Apollo decodes the frames into the Steam Streaming Microphone device, so Steam voice chat and any game hears the remote microphone. Stock Moonlight clients are unchanged, and the mic path shares no code with the stream path.

## Status

| Part | State |
|---|---|
| Design spec | Approved and amended after research and review |
| Host plan, tasks 1 to 11 | Complete. Every task passed a task review |
| Whole-branch review | 0 Critical, 7 Important, 15 Minor. All Important findings fixed in `ab81fd5a`, and a scoped re-review confirmed each fix |
| Tests on macOS | 46 tests from 5 suites pass in the standalone target. `src/mic.cpp` and `src/confighttp.cpp` pass a syntax-only compile |
| Windows build | Not run. This is the next step |
| Calliope app | Not started. The private repository `craigmdennis/Calliope` holds copies of the spec, the Apple research report, the reference sender, and the test vectors |

### Unverified until the Windows build

1. `src/platform/windows/mic_write.cpp` has never been compiled. The file was read twice against `src/platform/windows/audio.cpp` and the MinGW-w64 headers.
2. `store_t::save()` in `src/mic_store.cpp` falls back to remove-then-rename when a rename over an existing file fails. macOS never enters that branch.
3. The Microphone tab and the Microphones list on the PIN page have never been seen in a browser.
4. No audio has been heard. The reference sender transmits Opus silence, which proves pairing, session, tray, and device switching. Audible audio needs Calliope.

## Get the branch on Windows

```bash
git fetch origin
git checkout feature/calliope-mic
git submodule update --init --recursive
```

Prerequisites: MSYS2 with the UCRT64 packages from `docs/building.md`, the official `node.exe` on `PATH`, and Steam installed. Apollo installs the Steam Streaming Microphone driver from the Steam folder at the first mic session.

## Verification steps

The full checklist is Task 9, Steps 3 to 6, in `docs/superpowers/plans/2026-09-20-calliope-host-receiver.md`. Run every item there. The summary below gives the order.

1. **Build and unit tests.** Prefix each command with `C:\msys64\msys2_shell.cmd -defterm -here -no-start -ucrt64 -c`.

   ```bash
   cmake -B cmake-build-mic -G Ninja -S . -DBUILD_TESTS=ON && ninja -C cmake-build-mic
   ./cmake-build-mic/tests/test_sunshine --gtest_filter='Mic*'
   ```

   Expected: the build completes, and 46 tests pass.
2. **Inert until paired.** With no `mic_state.json` present, start the new build. `Get-NetUDPEndpoint -LocalPort 48002 -ErrorAction SilentlyContinue` prints nothing, and a Moonlight stream behaves as before.
3. **Pairing, session, and restore.** From any machine on the LAN, run the reference sender. The PIN it prints is entered on the Microphone tab of the Apollo PIN page.

   ```bash
   pip3 install cryptography requests
   python3 tests/mic_standalone/send_test_tone.py <PC address>
   ```

   Step 5 of the plan lists 13 checks: tray notices, the default input switch and restore, `/api/mic/list`, the first-session driver install with a possible HTTP 503, a second paired microphone, pong code 4, a restart, and the device format (2 channels, 32 bit, 48000 Hz).
4. **Failure paths.** Step 6 of the plan lists 4 checks: a lost connection, an unclean Apollo exit, a removed device (HTTP 401), and three wrong PINs.

## When the Windows build fails

- A compile error in `mic_write.cpp` is the expected first failure. Fix it in that file. `src/platform/windows/audio.cpp` holds the working pattern for every WASAPI and COM call the file uses.
- `mic_write.cpp` defines `INITGUID` and does not include `<ksmedia.h>`. MinGW's `Audioclient.h` already includes it. Keep both points.
- A local variable named `error`, `warning`, `info`, `debug`, `verbose`, or `fatal` breaks `BOOST_LOG`, because those are logger names.
- C++ follows `.clang-format` exactly. Format only the mic files. The files that existed before this branch carry format drift on `master`, and that drift stays.
- The mic work must not change the stream path. This command prints nothing on a correct branch:

  ```bash
  git diff --stat master -- src/stream.cpp src/rtsp.cpp src/nvhttp.cpp src/audio.cpp src/video.cpp
  ```
- Every fix gets a manual check. `.github/CONTRIBUTING.md` states that AI-generated tests are not trusted on their own.
- Commit each fix on `feature/calliope-mic`, and push to `origin` only.

## Where the code is

| Path | Content |
|---|---|
| `src/mic.{h,cpp}` | Mic thread, UDP receive, session lifecycle, pairing entry points |
| `src/mic_protocol.{h,cpp}` | Packet format, AES-GCM, PIN proof |
| `src/mic_jitter.{h,cpp}` | Jitter buffer: 40 ms prebuffer, 100 ms cap |
| `src/mic_playout.{h,cpp}` | Opus decode, concealment, talkspurt reset |
| `src/mic_store.{h,cpp}` | Paired devices in `mic_state.json` |
| `src/mic_pairing.{h,cpp}` | Pending pairing requests, PIN attempts, expiry |
| `src/platform/windows/mic_write.cpp` | WASAPI render into the Steam Streaming Microphone, default device switch |
| `src/confighttp.cpp` | Seven `/api/mic/*` routes |
| `src/system_tray.{h,cpp}` | Four mic tray notices |
| `src_assets/common/assets/web/pin.html` | Microphone tab and Microphones list |
| `tests/unit/test_mic_*.cpp`, `tests/fixtures/mic_vectors.json` | Unit tests and known-answer vectors |
| `tests/mic_standalone/` | macOS test target, syntax check, reference sender |

## Documents

| Path | Content |
|---|---|
| `docs/superpowers/specs/2026-09-20-calliope-mic-sidecar-design.md` | The design: product rules, wire protocol, pairing, errors, Calliope screens |
| `docs/superpowers/plans/2026-09-20-calliope-host-receiver.md` | The host plan, synced to the committed code |
| `docs/superpowers/plans/2026-09-20-calliope-host-receiver-rulings.md` | 27 decisions made during execution, each with its cost when wrong |
| `docs/superpowers/research/2026-09-20-windows-virtual-mic.md` | WASAPI and Steam driver findings |
| `docs/superpowers/research/2026-09-20-voice-playout.md` | Jitter and playout findings |
| `docs/superpowers/research/2026-09-20-calliope-apple-frameworks.md` | Apple capture, Opus encode, signing, and distribution findings |
| `docs/remote_microphone.md` | The user guide |
| `docs/api.md` | The `/api/mic/*` routes |

## What comes next

1. Run the Windows verification above, and fix what it finds.
2. Read the "Product behaviour" section of the rulings file. Ruling 8, the default capture switch, is the one marked as an overrule candidate.
3. After the checklist passes, merge `feature/calliope-mic` into `master` on `origin`.
4. Write the Calliope plan as a separate plan, in the `craigmdennis/Calliope` repository. The plan starts from the spec and the Apple research report. The Apollo copies of those files are the source, so a protocol change lands in Apollo first.
5. Build Calliope with a free Apple ID first. The paid Apple Developer Program and TestFlight are an optional last milestone.

### Rules the Calliope client must follow

- Both sequence counters start at 0 and only increase for the life of a mic session. A counter never restarts after an audio interruption.
- The audio sequence does not advance while muted. One ping is sent each second.
- `POST /api/mic/session` uses an HTTP timeout of at least 15 seconds, and the request is sent again on HTTP 503, up to three times.
- The device name holds 1 to 64 bytes. HTTP 429 on pairing means four requests are pending.
- Capture: `AVAudioConverter` to Opus, 48 kHz mono, 960 frames per packet, constant 32000 bit/s, `downmix = true`.
- Voice processing stays off, because it forces a session type that captures from AirPods.
- The iOS audio session uses `.record` with empty options and a preferred built-in microphone.
- `tests/fixtures/mic_vectors.json` is copied into the Calliope tests, so both sides check the same known-answer vectors.

## What does not travel with this branch

- The execution ledger, task briefs, and review diffs sit in `.superpowers/`, which git ignores. The rulings file above holds the decisions from that ledger.
- `blog/` is ignored by design.
- Agent memory is local to the macOS machine. The remote rule at the top of this file is the one memory entry that matters here.
