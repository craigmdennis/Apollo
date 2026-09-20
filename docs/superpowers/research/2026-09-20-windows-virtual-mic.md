# Rendering decoded voice into the Steam Streaming Microphone on Windows

Research date: 2026-09-20. Scope: validating the unverified choices in
`.superpowers/sdd/2026-09-20-calliope-host-receiver/task-5-brief.md` against working
implementations and Microsoft documentation.

Four independent host-side implementations were read in full:

| Short name | Source | Virtual device |
|---|---|---|
| PR 1428 | `logabell/apollo-microphone` `src/platform/windows/mic_write.cpp` (head of <https://github.com/ClassicOldSong/Apollo/pull/1428>) | Steam Streaming Microphone |
| Vibepollo | <https://github.com/xenstalker02/Vibepollo> `src/platform/windows/mic_write.cpp` | Steam Streaming Microphone |
| moonlight-mic | <https://github.com/JimothySnicket/Apollo> branch `moonlight-mic-stable`, `src/stream.cpp` | Steam Streaming Microphone |
| foundation-sunshine | <https://github.com/AlkaidLab/foundation-sunshine> `src/platform/windows/mic_write.cpp` | VB-Cable primary, Steam only as a default-device shim |

---

## 1. Format handling: AUTOCONVERTPCM versus forcing the device format

### Verified from source

**PR 1428 does not use AUTOCONVERTPCM at all.** It uses two different formats: an integer
PCM format pushed onto both endpoints through `IPolicyConfig::SetDeviceFormat`, and an
IEEE float format used for the render stream.

`src/platform/windows/mic_write.cpp` lines 189–223
(<https://github.com/logabell/apollo-microphone/blob/master/src/platform/windows/mic_write.cpp>):

```cpp
std::vector<BYTE> make_recommended_steam_mic_device_waveformat() {
  ...
  pcm_format.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
...
std::vector<BYTE> make_required_steam_mic_render_waveformat() {
  ...
  float_format.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
```

Both endpoints are normalised before the client is initialised, lines 441–442:

```cpp
const bool render_format_enforced = ensure_recommended_steam_mic_format(render_device_id, target_device_name, eRender);
const bool capture_format_enforced = ensure_recommended_steam_mic_format(capture_device_id, capture_device_name, eCapture);
```

and the stream is opened with `AUDCLNT_STREAMFLAGS_EVENTCALLBACK` only, line 475 calling the
helper at lines 318–327:

```cpp
const auto init_status = initialize_shared_audio_client(audio_client.get(), required_render_format, AUDCLNT_STREAMFLAGS_EVENTCALLBACK);
```

**Vibepollo independently reached the same split and documented why.**
`src/platform/windows/mic_write.cpp` lines 39–45
(<https://github.com/xenstalker02/Vibepollo/blob/master/src/platform/windows/mic_write.cpp>):

```cpp
 * THE CRITICAL FIX: SubFormat must be KSDATAFORMAT_SUBTYPE_PCM here.
 * Using IEEE_FLOAT for SetDeviceFormat causes WASAPI Initialize to fail
 * with 0x88890008 (AUDCLNT_E_UNSUPPORTED_FORMAT) on Steam Streaming Microphone.
```

Its init order, lines 105–111, is: find render endpoint, `ensure_recommended_steam_mic_format`
("Normalize device format to PCM (critical — must run before Initialize)"), then
`Initialize(..., AUDCLNT_STREAMFLAGS_EVENTCALLBACK, ...)` with the IEEE float format (line 354).
Vibepollo also normalises the paired capture endpoint (lines 262–325).

**moonlight-mic uses AUTOCONVERTPCM but never forces a format**; instead it takes whatever
`GetMixFormat` returns and initialises with that. `src/stream.cpp` lines 541–575
(<https://github.com/JimothySnicket/Apollo/blob/moonlight-mic-stable/src/stream.cpp>):

```cpp
    // 5. Negotiate format via GetMixFormat. In AUDCLNT_SHAREMODE_SHARED
    //    this is the only format guaranteed not to fail with
    //    AUDCLNT_E_UNSUPPORTED_FORMAT.
...
        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
```

It then converts the decoded mono s16 into whichever of s16/f32 and 1/2 channels the mix
format reported (lines 1845–1872), and refuses to render at all if the endpoint is not
48 kHz (lines 1805–1808: "skipping frame (no resampler)").

**foundation-sunshine uses AUTOCONVERTPCM but is not a Steam-mic render implementation.**
`src/platform/windows/mic_write.cpp` lines 156–222 pick VB-Cable, or failing that the default
console render endpoint, and try 16-bit PCM mono then 16-bit PCM stereo with
`AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY`. Its only use
of the Steam Streaming Microphone is `setup_steam_mic_loopback()` (lines 724–738), which merely
makes the Steam capture endpoint the default recording device. It is therefore not evidence
about writing to the Steam render endpoint.

**Microsoft documentation.** The AUDCLNT_STREAMFLAGS_XXX reference
(<https://learn.microsoft.com/en-us/windows/win32/coreaudio/audclnt-streamflags-xxx-constants>)
defines the flag as, verbatim:

> **AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM** — 0x80000000 — A channel matrixer and a sample rate
> converter are inserted as necessary to convert between the uncompressed format supplied to
> IAudioClient::Initialize and the audio engine mix format.

and

> **AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY** — 0x08000000 — When used with
> AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM, a sample rate converter with better quality than the
> default conversion but with a higher performance cost is used. This should be used if the
> audio is ultimately intended to be heard by humans …

There is no documented restriction to capture streams and no documented latency cost beyond
"a higher performance cost" for the better SRC.

### Evidence that endpoint format mismatch is the distortion cause

`xenstalker02` spent a day porting PR 1428's code and reported
(<https://github.com/ClassicOldSong/Apollo/pull/1428#issuecomment-4111386962>, 2026-03-26):

> Every attempt produced the same result: loud full-volume white noise in the Windows audio
> test and garbled or silent audio in Discord.

The Apollo maintainer answered the same day
(<https://github.com/ClassicOldSong/Apollo/pull/1428#issuecomment-4111586880>):

> Steam streaming microphone is still exactly a loop back, no hardware is involved. VB Cable
> is also pure software loopback. What you encountered is sample rate/bit depth/channel
> configuration mismatch. Steam Streaming Microphone doesn't do resampling so different bit
> depth can directly cause garbled output. VB cable may have done resampling so it can work
> across different input rates and output rates …

and PR 1428's author confirmed the mechanism
(<https://github.com/ClassicOldSong/Apollo/pull/1428#issuecomment-4111808171>):

> The issue you faced was the audio formatting. The Apollo PR automatically sets the steam
> streaming microphone to 32 bit when a session from moonlight is started.

Vibepollo's repository description today reads "Windows game streaming host with Steam Deck
mic passthrough via Steam Streaming Microphone", so the same author's later Steam-mic path
works once the two endpoints are normalised.

### Inferred

The decisive argument against relying on AUTOCONVERTPCM alone is structural rather than
anecdotal. AUTOCONVERTPCM only converts between the stream format supplied to `Initialize`
and the audio engine mix format of the **render** endpoint. The Steam driver's loopback runs
from the render endpoint's device format to the capture endpoint's device format, entirely
downstream of the render stream. A mismatch between those two endpoints' device formats is
outside anything AUTOCONVERTPCM can reach, and that mismatch is exactly what the maintainer
identified as the cause of garbled output. Forcing both endpoints with
`IPolicyConfig::SetDeviceFormat` is the only approach in the evidence that addresses it.

moonlight-mic avoids the same trap differently, by refusing to render when the endpoint is not
48 kHz, and by adapting the channel count and sample type to whatever the endpoint reports —
so it never creates a mismatch, rather than repairing one. The draft does neither: it fixes
the stream at float32 stereo 48 kHz and leaves the endpoint device formats untouched.

---

## 2. The Steam Streaming Microphone driver INF

### Verified from source

The exact path in the draft is correct and matches PR 1428 verbatim. PR 1428's diff to
`src/platform/windows/audio.cpp`
(<https://github.com/ClassicOldSong/Apollo/pull/1428/files>):

```cpp
constexpr auto STEAM_SPEAKERS_DRIVER_PATH   = L"%CommonProgramFiles(x86)%\\Steam\\drivers\\Windows10\\" STEAM_DRIVER_SUBDIR L"\\SteamStreamingSpeakers.inf";
constexpr auto STEAM_MICROPHONE_DRIVER_PATH = L"%CommonProgramFiles(x86)%\\Steam\\drivers\\Windows10\\" STEAM_DRIVER_SUBDIR L"\\SteamStreamingMicrophone.inf";
```

`STEAM_DRIVER_SUBDIR` is `L"x64"` on amd64 and undefined elsewhere, from Apollo's existing
`/Users/craigmdennis/Sites/Apollo/src/platform/windows/audio.cpp` lines 33–37.

The file name and folder layout are independently confirmed by a mirrored copy of Valve's
driver package, <https://github.com/wyrmdancer/SteamLinkAudioDrivers>, whose tree contains
exactly:

```
Steam VAC/drivers/Windows10/x64/SteamStreamingMicrophone.inf
Steam VAC/drivers/Windows10/x64/SteamStreamingMicrophone.sys
Steam VAC/drivers/Windows10/x64/SteamStreamingSpeakers.inf
Steam VAC/drivers/Windows10/x64/SteamStreamingSpeakers.sys
Steam VAC/drivers/Windows10/x86/…   Steam VAC/drivers/Windows8.1/{x64,x86}/…
```

There is no 16-channel INF and no other microphone INF in the package.

**Installation method.** PR 1428 does *not* install the driver from `mic_write.cpp`. It
extends Apollo's existing `install_steam_audio_drivers()` in
`src/platform/windows/audio.cpp` into a parameterised helper and installs the microphone INF
only when the endpoint is absent:

```cpp
bool install_steam_audio_drivers() {
  bool ok = true;
  if (!find_device_id(match_steam_speakers())) {
    ok = install_driver_from_local_steam_inf(STEAM_SPEAKERS_DRIVER_PATH, L"Steam Streaming Speakers", true) && ok;
  }
  if (!find_device_id(match_steam_microphone())) {
    ok = install_driver_from_local_steam_inf(STEAM_MICROPHONE_DRIVER_PATH, L"Steam Streaming Microphone", false) && ok;
  }
  return ok;
}
```

Two details matter: the third argument (`restore_default_output_device`) is `false` for the
microphone, so the mic install does not touch the default playback device, and the whole call
is gated on `config::audio.install_steam_drivers` in the PR's `init_mic_redirect_device()`:

```cpp
if (config::audio.install_steam_drivers) {
  BOOST_LOG(info) << "Attempting to install missing Steam audio drivers for microphone redirection"sv;
  install_steam_audio_drivers();
```

Apollo already declares that option (`/Users/craigmdennis/Sites/Apollo/src/config.cpp:1232`).

Vibepollo does not install anything; it logs "is Steam running on this machine? Mic passthrough
disabled" (`src/platform/windows/vibepollo_vmic.cpp` lines 50–56). moonlight-mic ships a batch
file the operator runs once (docs/using/setup.md line 11:
"Apollo bundles an installer at `tools/install_steam_audio_drivers.bat` … Without the drivers the
'Microphone (Steam Streaming Microphone)' virtual device will not appear",
<https://github.com/JimothySnicket/moonlight-mic/blob/main/docs/using/setup.md>).

Runtime loading of `DiInstallDriverW` from `newdev.dll` is correct for the MinGW build and is
already Apollo's established pattern
(`/Users/craigmdennis/Sites/Apollo/src/platform/windows/audio.cpp:1048–1063`, comment: "MinGW's
libnewdev.a is missing DiInstallDriverW() even though the headers have it, so we have to load
it at runtime"). Existing code also reports `ERROR_ACCESS_DENIED` and
`ERROR_FILE_NOT_FOUND`/`ERROR_PATH_NOT_FOUND` distinctly; the draft logs only a raw
`GetLastError()`.

---

## 3. Endpoint discovery and locale

### Verified from source

A single INF creates both endpoints. `SteamStreamingMicrophone.inf` registers the same KS
filter under the render and capture categories
(<https://github.com/wyrmdancer/SteamLinkAudioDrivers/blob/main/Steam%20VAC/drivers/Windows10/x64/SteamStreamingMicrophone.inf>):

```
[SteamStreamingMicrophone.NT.Interfaces]
AddInterface=%KSCATEGORY_AUDIO%,%KSNAME_Wave%,SteamStreamingMicrophone.I.Wave
AddInterface=%KSCATEGORY_RENDER%,%KSNAME_Wave%,SteamStreamingMicrophone.I.Wave
AddInterface=%KSCATEGORY_CAPTURE%,%KSNAME_Wave%,SteamStreamingMicrophone.I.Wave
```

and its string table fixes the locale-invariant names:

```
[Strings]
Provider="Valve Corporation"
Manufacturer="Valve Corporation Audio DDK"
SteamStreamingMicrophone.DeviceDesc="Steam Streaming Microphone"
```

So `PKEY_Device_DeviceDesc` and `PKEY_DeviceInterface_FriendlyName` both carry the bare,
untranslated string "Steam Streaming Microphone". `PKEY_Device_FriendlyName` is what Windows
composes as `"<form factor> (<device desc>)"`, and the form-factor word is localised — Spanish
and Italian driver-catalogue listings for the same device show "Altavoces (Steam Streaming
Microphone)" and "Altoparlanti (Steam Streaming Microphone)"
(<https://www.runonpc.com/altavoces-steam-streaming-microphone-driver-downloads-update/>,
<https://www.runonpc.com/altoparlanti-steam-streaming-microphone-driver-downloads-update/>).

**Name matching is what everyone does, but the safe key differs.** PR 1428 tests three keys
per device and accepts a match on any (`mic_write.cpp` lines 382–405), which is locale-safe
because `PKEY_Device_DeviceDesc` and `PKEY_DeviceInterface_FriendlyName` are not localised.
Vibepollo matches on `PKEY_Device_FriendlyName` only (line 143), with the patterns
`L"Steam Streaming Microphone"` and `L"Speakers (Steam Streaming Microphone)"` — the first
pattern is a substring of the localised name, so it still matches, but its
`L"Speakers (...)"` pattern is dead weight outside English. moonlight-mic also matches
`PKEY_Device_FriendlyName` on the substring `L"Steam Streaming Microphone"`, with the
comment at `src/stream.cpp` lines 489–491:

> Substring + case-insensitive match so both "Microphone (Steam Streaming Microphone)"
> (Win10/Win11 display name) and bare "Steam Streaming Microphone" resolve.

Apollo's own matcher for the speakers uses `match_field_e::adapter_friendly_name` with
`L"Steam Streaming Speakers"` (`src/platform/windows/audio.cpp:892–896`), that is, the
non-localised adapter name. PR 1428 adds the parallel `match_steam_microphone()` listing the
friendly name, adapter friendly name and description.

**The both-flows hazard is real.** `xenstalker02` lists among the bugs they had to fix
(<https://github.com/ClassicOldSong/Apollo/pull/1428#issuecomment-4111386962>):

> Fixing a device matching bug where the substring "Steam Streaming Microphone" matched both
> the render and capture endpoints, causing WASAPI writes to go to the wrong device

The draft avoids this by enumerating per `EDataFlow`.

**"16ch" has no supporting evidence.** The filter exists in PR 1428 (`mic_write.cpp` lines
386–391, comment "Steam also ships a 16-channel variant") but neither Steam INF in the driver
package declares such a device, and no other implementation filters on it. It is harmless and
cheap, but it is unverified folklore.

---

## 4. Default capture device switching

### Verified from source

**No Apollo-lineage implementation switches the default capture device.**

- PR 1428: `grep` over `mic_write.cpp` and `apollo_vmic.cpp` finds no `SetDefaultEndpoint` and
  no `GetDefaultAudioEndpoint`. Its user documentation instructs the operator instead
  (`docs/remote_microphone.md`, added by the PR): "In host applications, select
  `Microphone (Steam Streaming Microphone)` as the microphone/recording source."
- Vibepollo: no `SetDefaultEndpoint` anywhere in its mic files. It does use
  `IPolicyConfig::SetEndpointVisibility(id, TRUE)` to re-enable a *disabled* render endpoint
  (lines 213–235), which is a different operation.
- moonlight-mic: no `SetDefaultEndpoint` in `src/stream.cpp`. Its troubleshooting doc is
  explicit that the app must be pointed at the device by name rather than relying on the
  default: "Check the app's audio input setting explicitly — 'Default' may resolve to a
  different device depending on the OS default"
  (<https://github.com/JimothySnicket/moonlight-mic/blob/main/docs/using/troubleshooting.md>).

**Only foundation-sunshine switches the default, and only for two roles.**
`src/platform/windows/mic_write.cpp` lines 673–700:

```cpp
hr = policy->SetDefaultEndpoint(device_id.c_str(), eCommunications);
...
hr = policy->SetDefaultEndpoint(device_id.c_str(), eConsole);
```

It stores the original capture device id (`restoration_state.original_input_device_id`,
lines 824–849) and restores it in `restore_audio_devices()` (lines 854–895), which is invoked
from the destructor (`mic_write.h:132`). The stored id lives in process memory only, so a
crash leaves the switch in place with nothing to restore from.

Apollo's existing render-side code does switch all roles
(`src/platform/windows/audio.cpp:860–862`) and has a dedicated crash-recovery path,
`reset_default_device()` (lines 1005–1042), which temporarily hides the Steam Streaming
Speakers endpoint so Windows itself picks a replacement, and this is called unconditionally
from `platf::init()` (line 1193). That is the pattern to copy if the draft keeps the switch.

### Inferred

Switching the default capture device for all three roles is the draft's most invasive and
least-supported choice. It hijacks the microphone of every application on the host for the
duration of a stream, including applications that are not part of the streaming session, and
the draft's restore is a best-effort call in a destructor with no crash-safe fallback — Apollo
persists nothing equivalent to `reset_default_device()` for capture. The three Steam-mic
implementations all decided instead that the user selects the device in the consuming app.

---

## 5. Render loop structure

### Verified from source

| | PR 1428 | Vibepollo | moonlight-mic |
|---|---|---|---|
| Drive | dedicated thread, `WaitForSingleObject(render_event, 20)` | same | none; writes inline on the packet-receive thread |
| Init flags | `EVENTCALLBACK` | `EVENTCALLBACK` | `AUTOCONVERTPCM \| SRC_DEFAULT_QUALITY` |
| `hnsBufferDuration` | `1000000` (100 ms) | `config::audio.mic_buffer_ms * 10000`, default 50 ms | `2000000` (200 ms) |
| Prebuffer | `4 * 960` frames = 80 ms (`target_prebuffer_frames`, line 49) | `960 * 2` frames = 40 ms (`kPrebufFrames`, line 443) | half the WASAPI buffer, written as silence before `Start()` |
| Queue cap | 48000 frames (1 s) and 64 packets (lines 46–47) | 48000 frames (line 421) | none; drops frames when full |
| Underrun | waits for prebuffer, logs; decodes Opus PLC/FEC for missing sequences | waits for prebuffer, logs | primed silence cushion |
| Overrun | trims oldest frames/packets, reports through the debug UI | erases oldest frames | `GetCurrentPadding` check, then drop the frame at debug level; treats `AUDCLNT_E_BUFFER_TOO_LARGE` as a benign race |
| Device loss | `is_recoverable_device_error()` on `AUDCLNT_E_DEVICE_INVALIDATED`, `_RESOURCES_INVALIDATED`, `_SERVICE_NOT_RUNNING`; `recover_device()` calls `cleanup()` then `init()` | on `GetCurrentPadding` failure, stops, re-finds the device, re-inits, and **clears the queue** so stale audio is not replayed (lines 464–485) | logs and drops |
| Thread priority | `platf::adjust_thread_priority(thread_priority_e::high)` | same | n/a |

**moonlight-mic's buffer and priming comments are the clearest statement of the underrun
failure mode.** `src/stream.cpp` lines 446–449:

```cpp
    // 200 ms buffer in REFERENCE_TIME units (100 ns each). 100 ms caused
    // chronic underruns in POC live testing — buffer drained to zero between
    // mic packets every ~20 ms, producing helicopter-chop distortion.
    constexpr REFERENCE_TIME MIC_BUFFER_DURATION = 2000000;
```

and lines 610–625:

```cpp
    // 9. Prime the render buffer with silence before Start(). Without a
    //    cushion the buffer drains to zero before the first mic packet
    //    arrives (~20 ms later with jitter), producing helicopter-chop
    //    distortion. Half the buffer gives enough cushion without a
    //    noticeable latency hit (T11/T12 fix from POC).
      const UINT32 prime_frames = frame_count / 2;
      ...
        render_client->ReleaseBuffer(prime_frames, AUDCLNT_BUFFERFLAGS_SILENT);
```

**Microsoft contradicts the buffer duration all three WASAPI implementations pass.**
`IAudioClient::Initialize`
(<https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclient-initialize>),
verbatim:

> For a shared-mode stream that uses event-driven buffering, the caller must set both
> *hnsPeriodicity* and *hnsBufferDuration* to 0. The **Initialize** method determines how large
> a buffer to allocate based on the scheduling period of the audio engine.

PR 1428, Vibepollo and the draft all pass a nonzero `hnsBufferDuration` with
`AUDCLNT_STREAMFLAGS_EVENTCALLBACK` in shared mode. Both shipped implementations report working
audio with that combination, so Windows evidently honours the request rather than failing, but
it is outside the documented contract.

### The July 2026 "fan-like" report

`jackson00w` reported on 2026-07-16
(<https://github.com/ClassicOldSong/Apollo/pull/1428#issuecomment-4291877632>):

> My voice sounds very distorted, and people say I'm speaking near a fan or something

**No cause or fix was identified in the thread.** It is the last comment on the pull request
(25 issue comments total, fetched via
`https://api.github.com/repos/ClassicOldSong/Apollo/issues/1428/comments?per_page=100`), and
`logabell/apollo-microphone` has no commits after 2026-03-19
(`https://api.github.com/repos/logabell/apollo-microphone/commits?per_page=100`); the last six
commits are `e556bb6d first working version` through `99794062 Refine microphone troubleshooting UI`.
The only three review comments on the PR are from the Codex bot and concern concurrent
sessions sharing one Opus decoder, mic packet routing by source IP alone, and an installer
exit code — none of them audio quality.

### Inferred

"Speaking near a fan" is the same perceptual description as moonlight-mic's independently
named "helicopter-chop", and moonlight-mic attributes that to buffer underrun rather than
format mismatch. PR 1428's 80 ms software prebuffer sits in front of a 100 ms WASAPI buffer that
is never primed, so the first ~100 ms after `Start()` is played from an empty buffer, and any
jitter excursion larger than the queue depth re-empties it. This is a plausible, unconfirmed
explanation for the July report, distinct from the March format-mismatch problem which the
maintainer diagnosed. Both failure modes should be defended against; they are not the same bug.

---

## 6. What the working implementations found to be a mistake

### Verified from source

1. **Relying on the engine to fix an endpoint-to-endpoint format mismatch.** Section 1. The
   driver does not resample; both endpoints must be normalised.
2. **Using `KSDATAFORMAT_SUBTYPE_IEEE_FLOAT` with `SetDeviceFormat`.** Vibepollo's comment,
   lines 41–45: it makes the subsequent `Initialize` fail with `0x88890008`. The device format
   must be `KSDATAFORMAT_SUBTYPE_PCM`; the stream format is float.
3. **Matching the device name without separating render from capture.** `xenstalker02`'s bug
   list; writes landed on the wrong endpoint.
4. **Starting the render stream with a cold buffer.** moonlight-mic lines 610–625.
5. **A 100 ms render buffer for a 20 ms packet cadence.** moonlight-mic lines 446–449 raised it
   to 200 ms after chronic underruns.
6. **Not clearing the software queue after a WASAPI re-init.** Vibepollo lines 483–485:
   "Clear stale audio queued before the re-init to prevent replaying it into the
   freshly-opened WASAPI session."
7. **Sharing one Opus decoder across concurrent sessions.** The Codex review on PR 1428,
   `src/stream.cpp` line 2254: "packets from independent clients will be decoded through the
   same decoder/render path and corrupt each other's audio". `JimothySnicket` cites
   "Per-session decoder + WASAPI lifecycle (avoids the concurrent-session decoder collision
   Codex flagged on this PR)" as a deliberate design difference
   (<https://github.com/ClassicOldSong/Apollo/pull/1428#issuecomment-4172604901>).

### Additional defect found in the draft while reviewing it

`init(virtual_mic_error_e &error)` shadows the global Boost.Log severity logger named `error`
(`/Users/craigmdennis/Sites/Apollo/src/logging.h:17`,
`extern boost::log::sources::severity_logger<int> error;`). Inside that function the draft
writes `BOOST_LOG(error) << "Remote microphone: could not initialize …"`, which expands with
`error` resolving to the `virtual_mic_error_e &` parameter. This will not compile. PR 1428
avoids it by not taking a parameter of that name.

---

## Recommended changes to the draft

1. **Replace `AUTOCONVERTPCM | SRC_DEFAULT_QUALITY` with explicit endpoint normalisation.**
   Before `IAudioClient::Initialize`, call `IPolicyConfig::SetDeviceFormat` on **both** the
   render and the capture endpoint with `WAVE_FORMAT_EXTENSIBLE` /
   `KSDATAFORMAT_SUBTYPE_PCM`, 2 channels, 32 bits, 48000 Hz; then initialise the render
   stream with `AUDCLNT_STREAMFLAGS_EVENTCALLBACK` alone and the IEEE-float version of the
   same layout. Evidence: PR 1428 `mic_write.cpp:189–223, 441–442, 475`; Vibepollo
   `mic_write.cpp:39–58, 105–111, 354`; maintainer diagnosis at PR 1428 comment
   4111586880; the AUTOCONVERTPCM definition, which only spans stream format to render mix
   format and cannot reach the driver's render-to-capture loopback. Skip `SetDeviceFormat`
   when `GetDeviceFormat` already reports 2ch/32-bit/48 kHz, as PR 1428 does at lines 298–300.

2. **Do not pass `KSDATAFORMAT_SUBTYPE_IEEE_FLOAT` to `SetDeviceFormat`.** Vibepollo
   `mic_write.cpp:41–45` — it yields `AUDCLNT_E_UNSUPPORTED_FORMAT` (0x88890008) from the
   following `Initialize`.

3. **Prime the WASAPI buffer with silence before `Start()`** — `GetBuffer(frame_count / 2)`
   then `ReleaseBuffer(prime_frames, AUDCLNT_BUFFERFLAGS_SILENT)`. moonlight-mic
   `src/stream.cpp:610–625`.

4. **Add a playout prebuffer and do not render until it is reached.** The draft renders as
   soon as one sample is queued. Use 2–4 packets (40–80 ms). PR 1428 `mic_write.cpp:48–49,
   854–870`; Vibepollo `mic_write.cpp:443, 449–462`.

5. **Raise `BUFFER_DURATION_100NS` from 100 ms to 200 ms, or drop it to 0.** moonlight-mic
   `src/stream.cpp:446–449` found 100 ms caused chronic underruns at a 20 ms packet cadence.
   Note that Microsoft's `Initialize` documentation requires 0 for a shared-mode event-driven
   stream; if the buffer is left at 0 the software prebuffer of item 4 becomes the only jitter
   absorber and should be sized accordingly.

6. **Remove the default-capture-device switching, `previous_default_capture()` and
   `restore_default_capture()`, or make them opt-in and off by default.** None of the three
   Steam-mic implementations do it; all three instruct the user to select
   "Microphone (Steam Streaming Microphone)" in the consuming application (PR 1428
   `docs/remote_microphone.md`; moonlight-mic `docs/using/troubleshooting.md`). If the switch
   is kept, it needs a crash-safe restore equivalent to Apollo's existing
   `reset_default_device()` (`src/platform/windows/audio.cpp:1005–1042`, called from
   `platf::init()` at line 1193), because a destructor-only restore leaves a hijacked
   microphone after any abnormal exit — the exact gap foundation-sunshine has.

7. **Keep the INF path as is.** `%CommonProgramFiles(x86)%\Steam\drivers\Windows10\x64\SteamStreamingMicrophone.inf`
   is verified by PR 1428's own constant and by the driver package tree at
   `wyrmdancer/SteamLinkAudioDrivers`. Keep `DiInstallDriverW` loaded at runtime from
   `newdev.dll`.

8. **Gate the driver install on `config::audio.install_steam_drivers`** and reuse Apollo's
   existing `install_steam_audio_drivers()` plumbing rather than a private copy. PR 1428
   refactors `install_driver_from_local_steam_inf(path, name, restore_default_output_device)`
   and passes `false` for the microphone so the install does not disturb the default playback
   device; the option already exists at `src/config.cpp:1232`. Also distinguish
   `ERROR_ACCESS_DENIED` (needs administrator) from `ERROR_FILE_NOT_FOUND` /
   `ERROR_PATH_NOT_FOUND` (Steam not installed), as `audio.cpp:1093–1108` already does.

9. **Keep matching on all three property keys**, and keep the per-`EDataFlow` enumeration.
   `PKEY_Device_DeviceDesc` and `PKEY_DeviceInterface_FriendlyName` both carry the
   untranslated "Steam Streaming Microphone" from the INF `[Strings]` section, so the draft is
   already locale-safe. Do not add any match on the `"Speakers ("` or `"Microphone ("` prefix —
   those words are localised.

10. **Drop or keep the `16ch` exclusion as a judgement call, but treat it as unverified.** No
    16-channel Steam microphone INF exists in Valve's driver package. It costs one substring
    search and is harmless; it is not supported by evidence.

11. **Recover from recoverable WASAPI errors instead of latching `failed`.** Treat
    `AUDCLNT_E_DEVICE_INVALIDATED`, `AUDCLNT_E_RESOURCES_INVALIDATED` and
    `AUDCLNT_E_SERVICE_NOT_RUNNING` as re-init triggers (PR 1428 `mic_write.cpp:163–187`;
    Vibepollo `mic_write.cpp:464–485`), and **clear the sample queue after a successful
    re-init** so stale audio is not replayed.

12. **Rename the `virtual_mic_error_e &error` parameter** (for example `error_out`). It shadows
    the global Boost.Log `error` logger (`src/logging.h:17`) and breaks the draft's own
    `BOOST_LOG(error)` call.

13. **Keep the mono-to-stereo duplication.** All three implementations write the same sample to
    both channels: PR 1428 `mic_write.cpp:901–906`, Vibepollo `mic_write.cpp:501–506`,
    moonlight-mic `src/stream.cpp:1866–1871`. PR 1428 even labels it in its debug UI as
    "Duplicate mono microphone input to stereo render channels" (line 516).

14. **Clamp decoded samples to [-1.0, 1.0] before queueing.** PR 1428 `mic_write.cpp:680`,
    Vibepollo `mic_write.cpp:419`. The draft queues raw decoder output.

15. **Consider refusing to render rather than rendering at the wrong rate.** moonlight-mic
    skips frames and logs when the endpoint is not 48 kHz (`src/stream.cpp:1805–1808`); PR 1428
    rejects writes when the active format is not 2ch/32-bit/48 kHz (`mic_write.cpp:597–600`).
    A silent wrong-rate render is what produces the reports that read as "distortion".

---

## Still unverified

- **The cause of the July 2026 "fan-like" distortion report.** It was never answered on the
  pull request and no commit followed it. The underrun hypothesis in section 5 is inference
  from moonlight-mic's independently reported "helicopter-chop", not a confirmed diagnosis.
- **Whether a nonzero `hnsBufferDuration` with `AUDCLNT_STREAMFLAGS_EVENTCALLBACK` in shared
  mode behaves as intended on all Windows builds.** Microsoft documents that both values must
  be 0; two shipped implementations pass nonzero and report working audio. No first-party
  statement was found explaining the discrepancy, and this could not be tested here.
- **Whether a 16-channel Steam Streaming Microphone endpoint exists in any Steam build.** No
  INF, forum post or issue was found describing one. PR 1428 asserts it in a comment.
- **Whether the Steam driver's loopback works with Steam closed.** `xenstalker02` reported a
  render endpoint that produced nothing with Steam running but no Remote Play session
  (PR 1428 comment 4111386962); the maintainer replied that the device is a pure driver-level
  loopback with no Steam involvement, and the INF's single KS filter registered under both the
  render and capture categories supports the maintainer. The contradiction was never resolved
  in the thread, and no implementation documents a Steam-must-be-running requirement.
- **Whether the mic INF install requires a reboot or a service restart on current Windows
  builds.** PR 1428 and Apollo both sleep 5000 ms after `DiInstallDriverW`; moonlight-mic's
  docs say "A reboot is not required but the virtual device may take a few seconds to appear".
  Neither claim was independently verified.
- **The exact `AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM` minimum Windows version.** The constants
  page lists Windows Vista as the header's minimum supported client but does not annotate this
  flag individually the way it annotates `AUDCLNT_STREAMFLAGS_RATEADJUST` as "new in Windows 7".
- **Nothing was built or run.** Every finding here is from reading source, documentation and
  issue threads; no Windows host was available.
