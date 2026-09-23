# Remote microphone diagnostics

Apollo and Calliope report where audio stops and what it sounds like at each stage. The Calliope repository holds a copy of this file. The Apollo copy is the source.

## Problem

A connected session can produce silence or poor audio on the PC with no evidence at either end. Calliope shows Mic live when the ping loop is answered, and nothing more. Apollo logs the session start and end. Neither side counts packets, frames, or clipped samples, and Calliope has no log a person can read from a phone.

## Scope

Five additions, in order of build:

1. Capture counters in Calliope.
2. Peak and clip detection in Calliope.
3. Structured logging and a copied diagnostics report in Calliope.
4. Session recording to WAV files in Calliope.
5. Host counters in the Apollo pong, shown in Calliope.

Out of scope: host-side log export, automatic upload of any file, and encoder setting changes. The encoder stays at 32 kbps constant bit rate, mono, 20 ms, until the counters show that stream is received cleanly.

## Host counters in the pong

The pong payload grows from 1 byte to 21 bytes:

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | Error code, unchanged |
| 1 | 4 | Audio packets accepted by the jitter buffer |
| 5 | 4 | Audio packets rejected as late or duplicate |
| 9 | 4 | Frames concealed for loss |
| 13 | 4 | Decode failures |
| 17 | 4 | Frames written to the virtual microphone |

Each counter is an unsigned 32-bit big-endian integer, counted since the session began, and wraps. Calliope reads byte 0 alone when the payload is shorter than 21 bytes, so an old host and an old client stay compatible with no version field.

Counters are incremented on the thread that observes the event: the network thread for accepted and rejected packets, the render thread for concealed frames, decode failures, and frames written. Each is an atomic read when the pong is built. The reference sender prints the counters with each pong.

## Capture counters

`CaptureStats` is a value type in MicCore with these fields for the last second: tap buffers, Opus packets, Opus bytes, peak sample, and clipped samples. It also carries the input device name, sample rate, and channel count. The tap accumulates the counts on the audio thread. The one-second stats tick that already delivers `StreamStats` drains and resets them, and the controller publishes both.

Every tap start and restart logs one line with the device name, sample rate, and channel count. Session totals of packets, bytes, and clipped samples accumulate for the diagnostics report.

Expected values while speaking: 50 packets a second at about 80 bytes each.

## Peak and clip

The tap computes the peak absolute sample and the count of samples at or above 0.99 in the same pass that computes the RMS windows for the meter. The shared meter shows a clip mark that holds for one second after any clipped sample. The clip threshold is a constant in `LevelMath`.

## Structured logging and the diagnostics report

MicCore replaces its print statements with one `os.Logger` subsystem, `com.craigmdennis.calliope`, with the categories `capture`, `stream`, `host`, and `pairing`. Log lines state device names, formats, session identifiers, error codes, and HTTP status codes. Log lines never contain a session key or a PIN.

"Copy diagnostics" appears in the Mac settings menu and the iOS microphone screen menu. It builds one plain-text report and places it on the general pasteboard. On iOS, Universal Clipboard carries the report to a Mac. The report contains, in order:

1. App version and build, OS version, and device model.
2. The selected input, its sample rate, and channel count.
3. The paired PC name and the current view state.
4. The latest `StreamStats`, `CaptureStats`, and host counters.
5. Session totals: duration, packets, bytes, clipped samples.
6. The last 15 minutes of log lines for this process from `OSLogStore`, oldest first.

The report is plain text with one heading per part, sized to paste into a chat.

## Session recording

A "Record next session" toggle appears in the Mac settings menu and the iOS microphone screen menu. While the toggle is on, the next session writes two WAV files in the app container's Documents folder:

- `<date>-input.wav`: the raw PCM from the tap, in the input format.
- `<date>-sent.wav`: the Opus packets decoded back to 48 kHz mono PCM through `AVAudioConverter`.

Both files play in QuickTime Player and the Files app. The toggle turns itself off when the session ends, so a forgotten switch cannot fill the device. The Mac settings menu gets "Show recordings", which opens the folder in Finder. The iOS Info.plist gains `UIFileSharingEnabled` and `LSSupportsOpeningDocumentsInPlace` so the Files app lists the folder.

Writing happens on a serial queue fed from the tap, never on the audio thread. A write failure logs one line and stops recording for that session without ending the session.

## Testing

- MicCoreTests: `CaptureStats` accumulation and reset, clip counting in `LevelMath`, pong parsing at 1 byte and 21 bytes, and the diagnostics report layout from fixed inputs.
- Apollo unit tests: the pong layout and the counter increments through the jitter buffer and playout.
- Manual: a Mac session with recording on produces two playable files. Copy diagnostics on both platforms yields a report containing all six parts.
