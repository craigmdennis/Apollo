# Calliope host receiver: rulings made during execution

Plan: `docs/superpowers/plans/2026-09-20-calliope-host-receiver.md`.
Spec: `docs/superpowers/specs/2026-09-20-calliope-mic-sidecar-design.md`.

The plan ran without a pause for questions. Each conflict, ambiguity, or plan defect was decided during the run. Each entry below states the decision, the reason, and the cost when the decision is wrong. Any entry can be overruled. The entries marked "Overrule candidate" change product behaviour.

## Product behaviour

- **Ruling 8. Overrule candidate.** Apollo switches the Windows default capture device to the Steam Streaming Microphone during a mic session, and restores it afterwards. The research report recommended removing the switch. The approved spec promises the switch, and the stored `previous_default_capture` value covers a crash. Cost when wrong: every program on the PC records the streamed microphone during a mic session.
- **Ruling 9.** The prebuffer depth stays a constant of 2 frames (40 ms), with no config option. A config option needs `config.cpp`, the docs, the web UI, the locale file, and the config consistency test. Cost when wrong: a noisy Wi-Fi network needs a rebuild to raise the prebuffer.
- **Ruling 11.** The render loop keeps 40 ms of audio queued in the device (`TARGET_QUEUED_FRAMES`). The research report said to fill all free space, which drains the jitter buffer into a 200 ms device buffer. Cost when wrong: underruns on the device side. The constant is the single place to raise the value.
- **Ruling 18.** The host drops a ping whose sequence does not increase. Any LAN host can reach the UDP port, and a captured ping sent again forever keeps the virtual microphone as the default capture device. Cost when wrong: a client that restarts its ping counter inside a mic session is ignored until the 5 second timeout ends the mic session. The spec now states the rule.
- **Ruling 22.** `POST /api/mic/session` waits 10 seconds instead of 30, then answers HTTP 503, and the client sends the request again. The config server has one thread, so a 30 second wait stops the web UI for 30 seconds. Cost when wrong: a slow driver install needs more than three client retries, and the first connection fails once.
- **Ruling 23.** `store_t::save()` removes the destination and renames again when a rename over an existing file fails. `src/logging.cpp` shows that this toolchain needs the fallback. Cost when wrong: a crash between the remove and the rename loses `mic_state.json`, and every mic device pairs again.
- **Ruling 24.** A pairing request raises the tray notice at most once in 30 seconds. Any LAN host can call the pairing route. Cost when wrong: a second real device that pairs within 30 seconds gets no tray notice. The PIN page still lists the request.
- **Ruling 25.** The Windows toasts for pong codes 1 to 3 use wording for a reader at the PC, different from the Calliope text. Cost when wrong: a copy edit.
- **Ruling 27.** `mic::stop()` joins the mic thread with no time limit. A handler that is running when Apollo exits, such as a driver install waiting behind a prompt, delays the exit until the handler returns. A limit needs a detached thread or a forced process end, and Apollo's existing audio driver install has the same exposure. Cost when wrong: Apollo's exit hangs until the install returns, and the process is ended by hand.
- **Ruling 21.** The remove control in the Microphones list is a `<button>` with an `aria-label`, and failed register and remove requests show a message. The plan text specified neither. Cost when wrong: two `en.json` keys and about ten lines.

## Code structure

- **Ruling 10.** The Windows writer holds a private copy of the Steam driver install routine. Refactoring `install_steam_audio_drivers()` touches the file that drives stream audio, and product rule 1 forbids that risk. Cost when wrong: two install routines to keep in step.
- **Ruling 13.** `mic_write.cpp` defines `INITGUID` and omits `<ksmedia.h>`, matching `src/platform/windows/audio.cpp`. The MinGW-w64 headers were read to confirm both points. Cost when wrong: a duplicate-definition link error on Windows, which `DECLSPEC_SELECTANY` rules out.
- **Ruling 14.** The virtual microphone is created and destroyed on one thread by documented contract. `init()` refuses `RPC_E_CHANGED_MODE`, and a destroy on a foreign thread is logged. Cost when wrong: a later caller on another thread gets an error log and no compile-time guarantee.
- **Ruling 15.** A local variable named `error` in `micSessionStart` is named `failure`, because `error` is also a Boost.Log logger name. Cost when wrong: none.
- **Ruling 16.** The six lifecycle defects found in `mic.cpp` were fixed through a new plan task (Task 11) with complete, scratch-verified code. Cost when wrong: one extra plan task.
- **Ruling 17.** The fill logic moved into a tested unit, `mic_playout`, during the Task 11 rewrite. Cost when wrong: one more small file.
- **Ruling 19.** `print_req` in `confighttp.cpp` redacts `Authorization` and `Cookie` without regard to letter case. The defect predates this branch, and the "never in a log line" rule cannot hold without the fix. Cost when wrong: none.
- **Ruling 4.** The protocol test file keeps its own lowercase hex helpers. `util::hex_vec` prints uppercase, and the shared fixture is lowercase. Cost when wrong: about 20 duplicate test-only lines.

## Verification

- **Ruling 1.** `mic_write.cpp` ships with no unit test. WASAPI and the Steam driver exist only on Windows. Cost when wrong: a defect in that file appears first at the Windows check.
- **Ruling 2.** Steps labelled "Windows check" were not run on the macOS machine. The Windows checklist in plan Task 9 covers them. Cost when wrong: a task marked complete holds a compile error that only the Windows build shows.
- **Ruling 26.** Four deferred items stay unfixed, as the whole-branch review advised: a comment on a timed-out session start, a playout test for frame, wait, frame, and two raw error messages in the reference sender. Cost when wrong: a raw traceback in the reference sender, and no regression test for `OPUS_RESET_STATE`.

## Process

- **Ruling 3.** The `.gitignore` edit belongs to Task 9 although the Files list omitted it. Cost when wrong: none.
- **Ruling 5.** Every dispatch from Task 2 on carried the ponytail rules. Cost when wrong: none.
- **Ruling 6.** Tasks 5 and 6 waited for the research reports, and Tasks 2 to 4 proceeded. Cost when wrong: a follow-up edit to jitter constants, which did happen (100 ms cap).
- **Ruling 7 and Ruling 12.** A read-only review ran at the same time as the next implementer where the two shared no file. Cost when wrong: a fix round rebases over one unrelated commit.
- **Ruling 20.** Model policy: the cheapest model for transcribing complete code, the middle model for placing code in large files and for reviews, the strongest model for uncompiled threading code and the whole-branch review. Cost when wrong: a cheaper reviewer misses a defect, and the whole-branch review or the Windows checklist has to find it.
