# Calliope: remote microphone sidecar for Apollo

**Status:** Approved (design)

## Problem

A stock Moonlight client sends no microphone audio to the host. The GameStream
protocol has no client-to-host audio channel, so voice chat in Steam and in games
is unavailable during a stream.

No upstream project has merged a fix:

- **Apollo.** Pull request #1428 adds a Windows microphone path. It is open, it
  conflicts with `master`, and it requires a forked Moonlight client.
- **Sunshine.** Pull request #4078 was closed without a merge. It had no
  encryption and one global microphone stream.
- **Vibepollo.** Pull request #247 was closed because it requires forked clients
  on every platform.

Every existing implementation requires a forked Moonlight client. No fork exists
for tvOS, and tvOS runs one foreground app, so a second app cannot run beside
Moonlight on an Apple TV.

A Bluetooth headset does not solve the problem. Bluetooth Classic carries stereo
playback in one profile and microphone audio in another. Opening the headset
microphone switches the link to the hands-free profile, which lowers playback to
16 kHz mono. An Apple TV accepts Bluetooth headsets only.

## Goals

- Voice from an iPhone or a Mac reaches Steam voice chat and in-game chat on the
  host, with stock Moonlight unchanged on every client device.
- A stream without a microphone behaves exactly as it does today.
- Microphone devices are stored and managed apart from streaming clients.
- Pairing a microphone device uses the same page and the same PIN interaction as
  pairing a streaming client.
- Every screen states what a control does and what action comes next.
- The Apollo tray reports microphone connection and disconnection.
- The config server API lists every paired microphone device and marks the
  connected one.

## Non-goals

- A text notice inside the stream on a stock client. Moonlight draws its own
  "Slow connection" overlay from its own packet loss figures. Apollo sends rumble,
  HDR, motion, LED, trigger, and termination messages, and none of them displays
  text. The design substitutes an alert in Calliope and a Windows toast.
- A controller rumble pulse as an error signal. It requires edits to the stream
  control path.
- Use outside the home network. No VPN support, no manual host address field, no
  NAT traversal.
- Format negotiation, forward error correction, and more than one simultaneous
  microphone session.
- QR code pairing, a lock screen mute control, and App Store distribution.
- A microphone receiver on Linux or macOS hosts.

## Terms

| Term | Meaning |
|---|---|
| Calliope | The iOS and macOS app that captures and sends microphone audio |
| Host | Apollo running on Windows |
| Mic device | One iPhone or Mac paired with the host through Calliope |
| Mic session | One period during which a mic device sends audio to the host |
| Virtual microphone | The Steam Streaming Microphone device that Steam installs on Windows |
| Mic module | The new host code in `src/mic.cpp` and `src/platform/windows/mic_write.cpp` |

## Architecture

Three parts exist, and the wire protocol is the only interface between the host
and Calliope.

1. **Wire protocol.** HTTPS requests on the existing config server set up pairing
   and mic sessions. Encrypted UDP packets carry audio.
2. **Mic module.** A new thread in Apollo receives packets, decodes Opus, and
   writes PCM to the virtual microphone. Windows applications read the virtual
   microphone as an ordinary capture device.
3. **Calliope.** One Swift codebase for iOS 17 and macOS 14. It captures the
   selected microphone, encodes Opus, and sends packets.

A mic session is independent of a stream. A mic session can start with no stream
running, and a stream can run with no mic session. For an Apple TV stream, the
stream comes from the Apple TV and the mic session comes from an iPhone, so the
two have different addresses. The host therefore authorizes a mic device by its
own token and never by the address of a stream.

### Isolation from streaming

- The mic module lives in new files and runs on its own thread with its own UDP
  socket.
- `src/stream.cpp`, `src/rtsp.cpp`, `src/nvhttp.cpp`, `src/audio.cpp`, and
  `src/video.cpp` receive no edits.
- Until a mic device is paired, Apollo opens no mic socket, installs no driver,
  and changes no audio device.
- Apollo stream audio capture reacts to a change of the default render device
  only. `OnDefaultDeviceChanged` in `src/platform/windows/audio.cpp` tests
  `flow == eRender`. A change of the default capture device does not restart
  stream audio.
- The mic module catches every mic failure. A mic failure ends the mic session
  and never ends or alters a stream.
- The mic module restores the previous default capture device when a mic session
  ends, when Apollo exits, and at the next Apollo start after a crash.

## Wire protocol

### Origin rules on the config server

The config server rejects requests from outside `origin_web_ui_allowed`, which
defaults to localhost. A mic device is on the LAN, so the mic endpoints that
Calliope calls use their own rule:

- The two pairing endpoints accept localhost and LAN addresses and reject WAN
  addresses, whatever `origin_web_ui_allowed` holds.
- The two session endpoints authenticate by mic token and skip the origin check,
  which matches the existing read-only API key path.
- The admin endpoints keep the existing cookie authentication and origin check.

### Endpoints

| Endpoint | Authentication | Purpose |
|---|---|---|
| `POST /api/mic/pair` | None, LAN only | Calliope starts pairing |
| `POST /api/mic/pair/status` | None, LAN only | Calliope polls for the pairing result |
| `POST /api/mic/pin` | Admin cookie | The web UI submits the PIN and device name |
| `GET /api/mic/list` | Admin cookie or read-only API key | Lists paired mic devices |
| `POST /api/mic/remove` | Admin cookie | Removes one mic device |
| `POST /api/mic/session` | Mic token | Starts a mic session |
| `DELETE /api/mic/session` | Mic token | Ends the mic session |

`/api/mic/list` is added to `TOKEN_ALLOWED_PATHS` in `src/confighttp.cpp`, beside
`/api/clients/list`.

### Pairing

1. Calliope browses Bonjour for `_nvstream._tcp` and lists each host.
2. When a host is selected, Calliope opens a TLS connection to the config port
   and records the SHA-256 fingerprint of the host certificate.
3. Calliope creates a random 4-digit PIN and a random 16-byte nonce. It computes
   `proof = HMAC-SHA256(key = PIN, message = fingerprint || nonce)`.
4. Calliope sends `POST /api/mic/pair` with `name`, `nonce`, and `proof`. The
   host stores a pending request, raises the tray notice, and returns a random
   128-bit `request_id`.
5. Calliope displays the PIN and polls `POST /api/mic/pair/status` with the
   `request_id` every 2 seconds.
6. The PIN and an optional device name are entered on the Microphone tab of the
   PIN page. The web UI sends `POST /api/mic/pin`.
7. The host recomputes the proof for each pending request from the submitted PIN,
   the fingerprint of its own certificate, and the stored nonce. On a match, the
   host creates the mic device and a random 32-byte token.
8. The next status poll returns `paired`, the `uuid`, and the token. The host
   returns the token once and then deletes the pending request.
9. Calliope stores the token, the fingerprint, the host name, and the host address
   in the Keychain. Calliope pins the fingerprint for every later HTTPS request.

The proof binds the PIN to the certificate that Calliope saw. A host that presents
a different certificate computes a different proof, and pairing fails.

Limits on the unauthenticated endpoints:

- One pending request per source address, and four pending requests in total.
- A pending request expires after 120 seconds.
- Three wrong PIN submissions cancel every pending request.
- The existing request body size cap applies.

When the stored host address stops answering, Calliope browses Bonjour again and
selects the host whose certificate matches the pinned fingerprint.

### Mic device list

`GET /api/mic/list` returns the same shape as `/api/clients/list`:

```json
{
  "status": true,
  "named_mics": [
    { "name": "Living room iPhone", "uuid": "…", "connected": true },
    { "name": "MacBook Pro", "uuid": "…", "connected": false }
  ]
}
```

One mic session exists at a time, so at most one entry has `connected: true`. The
response never contains a token or a token hash.

### Mic session

1. Calliope sends `POST /api/mic/session` with `Authorization: Bearer <token>`.
2. The host replies with `session_id` (32-bit), `key` (16 random bytes, base64),
   and `port`.
3. The port is `net::map_port(13)`, which is 48002 with the default base port.
   Offsets 9, 10, and 11 carry video, control, and audio.
4. When a mic session already exists, the new mic session replaces it. The host
   sends pong code 4 to the replaced mic device.
5. The mic session ends on `DELETE /api/mic/session`, or after 5 seconds with no
   valid packet.

### UDP packets

| Field | Size | Notes |
|---|---|---|
| Type | 1 byte | 0 audio, 1 ping, 2 pong |
| Session id | 4 bytes | Big-endian, from the session reply |
| Sequence | 4 bytes | Big-endian, starts at 0. Each packet type has its own counter |
| Tag | 16 bytes | GCM tag |
| Payload | Variable | AES-128-GCM ciphertext |

- The tag precedes the payload. This matches the `[tag][ciphertext]` layout that
  `crypto::cipher::gcm_t` reads and writes.
- The GCM nonce is 12 bytes: 1 direction byte (0 for Calliope to host, 1 for host
  to Calliope), the type byte, 2 zero bytes, the session id, and the sequence. A
  changed header produces a different nonce, and the tag check fails.
- The type byte in the nonce lets each packet type keep its own sequence counter.
  Audio sequences are therefore contiguous, and a ping never appears as a lost
  audio frame.
- An audio payload is one Opus frame: 48 kHz, mono, 20 ms, about 32 kbps.
- A ping payload is empty. A pong payload is 1 byte holding the error code, and
  the pong repeats the sequence of the ping.
- While muted, Calliope sends no audio and one ping each second. While live,
  Calliope also sends one ping each second.
- After 3 seconds with no pong, Calliope requests a new mic session.
- The host drops a duplicate packet and a packet older than the playout point.
  Opus loss concealment fills a lost frame.

### Pong error codes

| Code | Meaning |
|---|---|
| 0 | No error |
| 1 | The virtual microphone is missing |
| 2 | The virtual microphone cannot be opened |
| 3 | Repeated Opus decode failures |
| 4 | Another mic device replaced this mic session |

Codes 1, 2, and 3 also raise a Windows toast on the host.

### Known limit of the PIN proof

An active interceptor on the LAN can record the proof, test all 10,000 PINs
offline, and then compute a valid proof for the real host certificate. The attack
requires an interceptor that is active during the 120-second pairing period on the
home network. A password-authenticated key exchange removes the limit and is the
upgrade path.

## Host design

### New files

- **`src/mic.h` and `src/mic.cpp`.** Platform neutral. They hold the mic device
  store, pending pairing requests, the single mic session, the UDP receive loop,
  packet decryption with `crypto::cipher::gcm_t`, the jitter buffer, Opus decode,
  and the current pong error code. The config server routes call into this module.
- **`src/platform/windows/mic_write.h` and `mic_write.cpp`.** Adapted from Apollo
  pull request #1428, with attribution in the file header. Both projects use
  GPL-3.0. The code finds the virtual microphone, installs the driver when it is
  absent, writes PCM through WASAPI, and switches and restores the default
  capture device.

### Platform interface

`src/platform/common.h` gains one class, `virtual_mic_t`, with a `write` method
for PCM frames, and one factory function. The existing `mic_t` class is the host
capture source and stays unchanged. The Linux and macOS factory functions return
null. When the factory returns null, `POST /api/mic/session` returns HTTP 501.

### Virtual microphone driver

Steam stores the driver beside the speaker driver that Apollo already installs:
`%CommonProgramFiles(x86)%\Steam\drivers\Windows10\x64\SteamStreamingMicrophone.inf`.
The path is confirmed by the driver package and by Apollo pull request #1428. The
install runs at the first mic session, never at stream start, and only when
`install_steam_drivers` is enabled.

The Steam driver copies audio from its render endpoint to its capture endpoint
with no conversion. Before the render client is initialised, the writer sets both
endpoints to 2 channels, 32-bit PCM, 48000 Hz. A format mismatch between the two
endpoints produces garbled voice. The device buffer is 200 ms and is primed with
silence before the client starts. The writer reopens the client when Windows
invalidates the device. Evidence is in
`docs/superpowers/research/2026-09-20-windows-virtual-mic.md`.

### Mic device store

- The store is `mic_state.json`, in the directory that holds
  `sunshine_state.json`.
- Each entry holds `name`, `uuid`, and `token_hash`. The hash is unsalted SHA-256
  through the existing `http::hash_api_token`, which is sufficient for a random
  32-byte token.
- The file also holds `previous_default_capture`, the device id saved before a
  switch. At start, a non-empty value causes a restore and is then cleared.
- Mic code never reads or writes `sunshine_state.json` or `named_devices`.

### Lifecycle and threading

- At launch, the mic thread starts only when `mic_state.json` holds a mic device.
  Otherwise the mic thread starts after the first successful pairing.
- The receive loop decrypts packets and places Opus frames in the jitter buffer.
- Playout is pull-based. The render thread of the virtual microphone requests one
  decoded packet at a time, so the audio device clock is the only playout clock.
  A timer is unsuitable: the default Windows timer granularity is 15.625 ms, and
  Apollo raises it only while a stream runs.
- The render thread requests packets only while the device holds less than 40 ms,
  so the jitter buffer holds the delay and the device buffer adds none.
- The jitter buffer holds a fixed 40 ms before playout starts. Above 100 ms of
  queued audio, the jitter buffer drops the oldest frames.
- While playing, an empty jitter buffer conceals up to 5 frames with Opus loss
  concealment, then returns to prebuffering. Opus concealment is silent by the
  fifth frame, so a late packet costs no restart of the 40 ms prebuffer.
- The decoder state is reset when playout restarts after a silent period.
- Evidence for these choices is in
  `docs/superpowers/research/2026-09-20-voice-playout.md`.
- `main.cpp` starts the mic module after the config server and stops it before
  exit. The stop path restores the default capture device.

### Edits to existing files

| File | Change |
|---|---|
| `src/confighttp.cpp` | Seven routes, the LAN rule, one allowlist entry |
| `src/system_tray.cpp` and `.h` | Four functions: pairing request, connected, disconnected, error |
| `src/main.cpp` | Start and stop calls |
| `src/platform/common.h` | `virtual_mic_t` and its factory |
| `src/platform/linux/` and `src/platform/macos/` | Null factory |
| `cmake/compile_definitions/` | New source files |
| `src_assets/common/assets/web/pin.html` | Microphone tab and mic device list |
| `src_assets/common/assets/web/public/assets/locale/en.json` | New strings |
| `tests/unit/` | New tests |
| `docs/` | New remote microphone page |

## Calliope design

Calliope lives in its own repository beside the Apollo checkout, in a directory
named `Calliope`.

### Structure

- **`MicCore`, a Swift package.** It holds packet encoding, the pairing client,
  the session client with certificate pinning, the Bonjour browser, audio capture,
  the UDP sender, ping and pong monitoring, and Keychain storage. It has no UI and
  runs under `swift test`.
- **`micsend`, a macOS command-line target.** It pairs, starts a mic session, and
  sends the Mac microphone. It tests the host before any UI exists.
- **One SwiftUI app target for iOS 17 and macOS 14.** The screens are shared. On
  macOS the app is a menu bar item.

### Platform features used in place of dependencies

- CryptoKit supplies AES-GCM and HMAC-SHA256.
- `AVAudioConverter` supplies Opus encoding at 960 frames per packet and a constant
  32000 bits per second. A test run on macOS produced one 20 ms frame per packet,
  and stock libopus decoded every packet. A device test confirms that the encoder
  exists on the iPhone. When it does not, libopus through Swift Package Manager
  replaces it on iOS.
- `NWBrowser` supplies Bonjour discovery, and `NWConnection` supplies UDP.
- `SMAppService` supplies the login item.

### iOS behaviour

- The audio session uses the record category with no options, and sets the
  built-in microphone as the preferred input after activation. With no Bluetooth
  input option, iOS uses the built-in microphone. A device test confirms that
  AirPods stay connected to another device such as an Apple TV.
- Voice processing stays off. It requires a play-and-record session, which implies
  the voice chat mode and the Bluetooth hands-free option. That combination routes
  capture to AirPods, which is the behaviour this design exists to prevent.
- Evidence is in `docs/superpowers/research/2026-09-20-calliope-apple-frameworks.md`.
- The background audio mode keeps capture running while the phone is locked.
- An error produces a haptic in the foreground and a notification when the phone
  is locked.
- Calliope requests three permissions: microphone, local network, and
  notifications. Before each system prompt, one sentence states the purpose.
- An audio interruption such as a phone call pauses audio. Calliope sends pings
  during the interruption and resumes audio when the interruption ends.

### macOS behaviour

- The app is a menu bar item with a panel. It has no Dock icon.
- The Input card selects the capture device inside Calliope. The system default
  input stays unchanged.
- When the selected input is a Bluetooth device, the Input card shows the
  Bluetooth warning from the text section.
- The menu bar icon is a monochrome microphone glyph: outline for Ready, filled
  for Live, slashed for Muted. The Error state adds a coloured badge.

### Shared screens on iOS

1. **Hosts.** The first launch shows a screen that explains the local network
   permission before iOS asks. The list is titled "Available on this network" and
   shows a spinner during discovery. Each row shows the host name and "Paired" or
   "Not paired". A connected host shows green status text. A help link sits below
   the list.
2. **Pairing.** A three-step checklist sits above the PIN, and each step icon
   changes as the step completes. The PIN sits in a large grey box. A countdown
   shows the remaining time. "Get a new PIN" and "Cancel" sit below.
3. **Microphone.** A lock caption sits at the top. A central ring pulses with the
   input level, and in the Ready state the ring is the Connect button. A status
   line with a timer sits below the ring. The bottom row holds Input, Mute, and a
   red Disconnect. An error appears as an inline pill with a help button.

### macOS panel, top to bottom

1. **Header.** A coloured dot and a large status word. The line below holds the
   host name and the input name. The connected time sits on the right, and the
   Connect switch sits in the header.
2. **Level meter.** A horizontal input level bar with the Mute button at its end.
3. **Input card.** A title, the current selection as subtitle, one radio row per
   input, and a chevron that collapses the card.
4. **PC card.** One row per paired host, with a status word right-aligned in
   green. "Pair a new PC" is the last row.
5. **Launch at login.** A toggle row, off by default.
6. **Footer.** Settings, Help, and "Quit Calliope" as icon-and-label actions.

### States

Calliope has seven states: Not paired, Ready, Connecting, Live, Muted,
Reconnecting, and Error. The Error state shows the reason from the table below.

## Interface text

Example names in this section are "GAMING-PC", "Living room iPhone", and
"MacBook Pro".

### Apollo web UI, Microphone tab

- Introduction: "Pair a phone or Mac running Calliope as a microphone for this
  PC. Open Calliope, choose this PC, and enter the PIN it shows."
- After pairing: "Living room iPhone is paired as a microphone. Tap Connect in
  Calliope to start. While connected, Apollo makes it the default microphone on
  this PC."
- Remove confirmation: "Remove Living room iPhone? It will disconnect now and
  must be paired again to reconnect."

### Apollo tray

- "Incoming microphone pairing request". A click opens the PIN page.
- "Microphone connected: Living room iPhone"
- "Microphone disconnected: Living room iPhone". A timeout appends
  "(connection lost)".
- One toast for each of pong codes 1, 2, and 3, with the text from the error
  table.

### Calliope

- Pairing steps: "PIN created", "Enter it in Apollo on your PC: open the PIN page
  and choose the Microphone tab", "Paired".
- After pairing: "Paired with GAMING-PC. Tap Connect to start sending this
  microphone. Apollo will make it the default microphone on the PC until you
  disconnect."
- Bluetooth warning: "This input is a Bluetooth headset. macOS lowers headset
  sound quality while its microphone is on. Choose a different input to keep full
  quality."

## Error handling

| Condition | Host action | Calliope text |
|---|---|---|
| No host found | None | "Looking for Apollo on your network. Check that Apollo is running on your PC and that this device is on the same network." |
| PIN expired, or three wrong PINs | Cancels the pending request | "This PIN is no longer valid. Tap Get a new PIN." |
| Mic device removed in the web UI | Rejects the token with HTTP 401 | "This device was removed from Apollo on GAMING-PC. Pair again to use it." |
| Host is Linux or macOS | Returns HTTP 501 | "Apollo on GAMING-PC cannot receive a microphone. This needs Apollo on Windows." |
| Virtual microphone missing | Pong code 1 and a toast | "Apollo cannot find the Steam Streaming Microphone. Install Steam on the PC, then tap Connect." |
| Virtual microphone cannot be opened | Pong code 2 and a toast | "Another program on the PC is blocking the Steam Streaming Microphone. Close it, then tap Connect." |
| Repeated decode failures | Pong code 3 and a toast | "Apollo cannot decode the audio from this device. Tap Disconnect, then tap Connect." |
| Network loss | After 5 seconds, restores the default capture device and shows the disconnected tray notice | "Reconnecting" for 30 seconds, then "Lost connection to GAMING-PC. Tap Connect to try again." |
| A second mic device connects | Pong code 4 to the first mic device, then switches | "MacBook Pro is now the microphone for GAMING-PC." The first mic device stops and does not retry. |
| Phone call on the iPhone | Receives pings and keeps the mic session | "Paused during your call. Calliope resumes when the call ends." |
| Microphone permission denied | None | "Calliope needs the microphone. Open Settings and turn on Microphone." A Settings button follows. |
| Apollo exits or crashes | Restores the default capture device on exit, or at the next start | "Reconnecting", then a new mic session when Apollo returns. |

Pong code 4 stops two mic devices from replacing each other in a loop.

## Testing

- **Host unit tests.** Packet parsing, nonce construction, duplicate and late
  packet rejection, jitter buffer order, the mic device store, the PIN proof, and
  the PIN attempt limits.
- **Shared test vectors.** One file of packet bytes, keys, and expected plaintext
  is stored in both repositories. The host tests and the `MicCore` tests both read
  it, so both implementations are checked against the same bytes.
- **End to end.** `micsend` pairs with a real host and sends audio.
- **Manual verification.** The Apollo contributing rules require manual
  verification of each change. Each milestone lists its verification steps in the
  implementation plan. UI verification happens on real devices.

## Build order

1. Host protocol core in `src/mic.cpp`, with unit tests.
2. Windows virtual microphone writer, config server routes, tray functions, and
   the Microphone tab. Verification: a pairing completes in the web UI, and
   `/api/mic/list` shows the mic device.
3. `MicCore` and `micsend`. Verification: a voice spoken into the Mac is heard in
   Steam voice chat on the host.
4. The iOS and macOS interface.
5. Documentation: a remote microphone page in the Apollo `docs/` directory and a
   README in the Calliope repository.

Apollo work goes on a branch named `feature/calliope-mic`. The first Calliope task
in the plan copies the Calliope sections of this document into the Calliope
repository.
