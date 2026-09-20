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
| Windows build | Passes, 2026-09-20. `src/platform/windows/mic_write.cpp` compiled first time with no errors and no warnings. The stream-path diff against `master` is empty |
| Tests on Windows | 46 tests from 5 suites pass, the same count as macOS |
| Windows verification | Task 9 Steps 3 and 4 pass. Step 5: 9 of 13 items pass, 2 are degenerate on this PC, 1 is pending, 1 is not reproducible. Step 6: 1 of 4 passes, 1 is unreachable, 2 are pending. The detail is below |
| Calliope app | Not started. The private repository `craigmdennis/Calliope` holds copies of the spec, the Apple research report, the reference sender, and the test vectors |

### What the Windows verification established

Run on 2026-09-20 against a build launched from `cmake-build-mic` with its own config directory, so
the installed Apollo was untouched. The reference sender ran both over loopback and from a Mac on
the LAN, and both paths behaved the same.

- **Step 3, build and unit tests.** Pass.
- **Step 4, inert until paired.** Pass. UDP 48002 was unbound with no `mic_state.json`, and a
  Moonlight stream was unaffected. Once a device is paired the socket stays open for Apollo's life,
  which is the gate at `src/mic.cpp:466` working as written, not a contradiction of this step.
- **Step 5.** Items 1, 2, 3, 4, 6, 10, 11, 12 and 13 pass. Item 3 returned `pong error code: 0`
  throughout, item 11 showed `replaced by` on the host and code 4 at the replaced client, and item
  13 was read from the endpoints directly: both are 2 channels, 32 bit, 48000 Hz. Items 5 and 8 are
  degenerate here, see finding 4. Item 7 is pending. Item 9 is not reproducible, see finding 5.
- **Step 6.** Item 1 passes: the session ended 5.4 seconds after the last packet with the reason
  `connection lost`. Item 2 is unreachable on this PC, see finding 4. Items 3 and 4 are pending;
  for item 3 the device was removed and re-paired, but the HTTP 401 at the session request was not
  confirmed.

Three things outside the checklist were also established. The `device_missing` path was exercised
end to end by accident, reaching the client as pong code 1 with the tray notice raised. The
read-only API key is correctly scoped: `GET /api/mic/list` answers 200 while `POST /api/mic/pin`,
`POST /api/mic/remove`, `POST /api/mic/session` and `DELETE /api/mic/session` all answer 401. And
`store_t::save()` replaced its file repeatedly under the Windows toolchain across pairings and
restarts.

### Still unverified

1. `store_t::save()`'s remove-then-rename fallback in `src/mic_store.cpp` is still untaken. The
   ordinary rename succeeded every time on Windows, so the fallback never ran.
2. No audio has been heard. The reference sender transmits Opus silence, which proves pairing, the
   session, the tray, the device format and the write into the endpoint. Audible audio needs
   Calliope.
3. The default capture switch and restore, and the recovery from an unclean exit, for the reason in
   finding 4.
4. The Remote Desktop message added for finding 2 has not been seen. It cannot be reached from the
   console session, so it needs one deliberate run from RDP.

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

## Findings from the Windows verification

1. **The tray notices repeat their own title in the body.** `update_tray_mic_connected` and
   `update_tray_mic_disconnected` in `src/system_tray.cpp:444` build a body that starts with the
   title, so a toast reads "Microphone connected" above "Microphone connected: Local test". The
   other two mic notices do not do this, and neither does the rest of the file:
   `update_tray_playing` at `src/system_tray.cpp:238` pairs the title "App launched" with the body
   "[name] launched." The body should carry only the detail, so "Local test" for a connection and
   "Local test (connection lost)" for a disconnection, with the reason kept in the parentheses it
   already uses. Seen on Windows on 2026-09-20. **Fixed**: the body now carries only the detail.

2. **A host whose only login is a Remote Desktop session cannot serve a remote microphone.**
   Windows replaces the audio endpoints of an RDP session with redirected ones, so
   `find_steam_endpoint` at `src/platform/windows/mic_write.cpp:112` sees one render endpoint named
   "Remote Audio" and no capture endpoints at all. The host reports `device_missing` and the client
   receives pong code 1. An enumeration probe run inside the RDP session found exactly that; the
   same probe, after the session was moved to the physical console, found both
   "Speakers (Steam Streaming Microphone)" and "Microphone (Steam Streaming Microphone)". This is
   not only a testing note: `sunshinesvc` launches Sunshine into the active console session
   (`tools/sunshinesvc.cpp:245`), and when a user connects over RDP as the same user their session
   becomes that console session, so the shipped service is blind in exactly the same way. The
   Windows verification must therefore run with a session attached to the physical console, and the
   user guide should say that the remote microphone needs one too. **Fixed** for the messaging: the
   platform layer now reports `device_hidden_in_session`, the driver install is skipped in a remote
   session so it no longer complains about privileges, and the tray names the session as the reason
   instead of telling the user to install Steam. The wire code stays 1, because the pong codes are
   part of the Calliope contract. `docs/remote_microphone.md` still needs the note.

3. **The UCRT64 shell must be started with `-use-full-path` or CMake cannot find npm.**
   `docs/building.md` requires `node.exe` on `PATH` before `cmake`, but `msys2_shell.cmd` strips the
   Windows `PATH` by default, so `find_program(NPM npm)` at `cmake/targets/common.cmake:55` fails
   with "Could not find NPM using the following names: npm" even when the official Node is
   installed. Configuring from `C:\msys64\msys2_shell.cmd -defterm -here -no-start -ucrt64
   -use-full-path` succeeds. `docs/building.md` should carry the flag.

4. **The default capture switch and restore cannot be exercised on a PC whose only capture device
   is the Steam microphone.** `src/platform/windows/mic_write.cpp:332` clears `previous_default`
   when it already equals the Steam capture endpoint, which is correct and carries its own comment,
   but it means `previous_default_capture` stays empty for the whole session. Read mid-session on
   2026-09-20, `mic_state.json` held `""`. Task 9 Step 5 items 5 and 8 then pass trivially, and the
   recovery branch at `src/mic.cpp:459` that Step 6 item 2 exists to prove never runs. Both need a
   second capture device that is the default before the mic session starts.

5. **Task 9 Step 5 item 9 cannot be reproduced on a PC that already has the Steam driver.** The
   Steam Streaming Microphone was installed and active, so the first-session install, the
   multi-second session request, and the HTTP 503 that follows it were never seen. Calliope is
   required to retry that request up to three times, so the path should be verified on a PC without
   the driver before the client rule is relied on.

6. **A failed pairing left no trace in the log.** `src/mic_pairing.cpp` held no log statement at
   all, and `submit_pin` at `src/mic.cpp:528` returned `std::nullopt` in silence, so a wrong or
   expired PIN was indistinguishable from a request that never arrived. This was found by being
   unable to answer "it gave me a PIN and pairing failed" from the log on 2026-09-20. **Fixed**:
   an arriving request, a request refused because four are already pending, and a PIN matching no
   pending request are each logged. Verified live afterwards, a request from a Mac appearing 11
   seconds before the pairing it completed.

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
