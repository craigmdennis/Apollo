# Apollo

Apollo is a self-hosted desktop stream host for [Artemis(Moonlight Noir)](https://github.com/ClassicOldSong/moonlight-android). Offering low latency, native client resolution, cloud gaming server capabilities with support for AMD, Intel, and Nvidia GPUs for hardware encoding. Software encoding is also available. A web UI is provided to allow configuration and client pairing from your favorite web browser. Pair from the local server or any mobile device.

## What this fork adds

This fork of Apollo adds the following to upstream Apollo. Each item has a section below.

- **Remote microphone.** The Calliope app on an iPhone or Mac sends microphone audio to the PC over an encrypted UDP channel. The PC plays it into the Steam Streaming Microphone. Windows only. Moonlight and Artemis are unchanged.
- **Steam game activity.** Three read-only endpoints on the paired HTTPS channel return the last Steam game played, its artwork, and the desktop wallpaper. A new `activeclient` element in `serverinfo` names the device that is streaming. A new Apple TV client reads them. Windows only.
- **A "Share game activity" permission and a host switch.** The permission is per client. The `share_game_activity` setting turns sharing off for every client.
- **Seven microphone routes in the config API.** The read-only API key can also read `GET /api/mic/list`.
- **Web UI changes.** The PIN page has a Microphone tab and a Microphones list. The client permission list has "Share game activity". The Advanced tab has a "Share game activity" checkbox.
- **Request log redaction.** Request logs hide the `Authorization` and `Cookie` header values whatever the case of the header name.
- **Standalone tests.** The microphone and Steam logic builds and tests without building all of Apollo.

Verification status is stated in each section. The Steam activity code has passed its automated tests on macOS only. No record of a run against a Windows host exists in this repository.

Major features:

- [x] Built-in Virtual Display with HDR support that matches the resolution/framerate config of your client automatically
- [x] Permission management for clients
- [x] Clipboard sync
- [x] Commands for client connection/disconnection (checkout [Auto pause/resume games](https://github.com/ClassicOldSong/Apollo/wiki/Auto-pause-resume-games))
- [x] Input only mode
- [x] Remote microphone from an iPhone or Mac (Windows)
- [x] Steam game activity for the Apple TV client (Windows)

## Usage

Refer to LizardByte's documentation hosted on [Read the Docs](https://docs.lizardbyte.dev/projects/sunshine) for now.

Currently Virtual Display support is Windows only, Linux support is planned and will be implemented in the future.

## Building and installing from source

On Windows, helper scripts in `scripts/` wrap the MSYS2 UCRT64 toolchain so you can build and install without hand-typing the CMake/Ninja/CPack commands. The one-time prerequisites (MSYS2, the UCRT64 package set, and the official Node.js on `PATH`) are in [docs/building.md](docs/building.md).

Build the whole app (C++ core + web UI) and the NSIS installer:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build-windows.ps1
```

The installer lands in `cmake-build-release\cpack_artifacts\Apollo.exe`. Useful switches: `-Version 0.5.0` bakes a version, `-Tests` builds and runs the test suite, and `-NoPackage` stops after the build.

Install the build you just made, replacing a running installation:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\install-local.ps1
```

It stops `ApolloService` and any running `sunshine`/`apollo` process first (which avoids the NSIS "Error opening file for writing" failure), self-elevates, silent-installs, and restarts the service. It defaults to the `cmake-build-release\cpack_artifacts\Apollo.exe` build; pass `-Installer <path>` for another one.

To publish a build as a GitHub Release and update other machines, use `scripts\publish-release.ps1` and `scripts\update-apollo.ps1` — see [scripts/DISTRIBUTION.md](scripts/DISTRIBUTION.md).

## About Permission System

Check out the [Wiki](https://github.com/ClassicOldSong/Apollo/wiki/Permission-System)

> [!NOTE]
> The **FIRST** client paired with Apollo will be granted with FULL permissions, then other newly paired clients will only be granted with `View Streams` and `List Apps` permission. If you encounter `Permission Denied` error when trying to launch any app, go check the permission for that device and grant `Launch Apps` permission. The same applies to the situation when you find that you can't move mouse or type with keyboard on newly paired clients, grant the corresponding client `Mouse Input` and `Keyboard Input` permissions.

## Remote microphone

Apollo receives a microphone from an iPhone or a Mac running Calliope. The audio arrives on the PC as the Steam Streaming Microphone, which Steam voice chat, games, and Discord read as an ordinary microphone. The microphone path shares no code with the stream path, and a failure in it ends only the microphone session.

Requirements: Apollo on Windows, Steam installed on the PC (Steam supplies the driver), and Calliope on the same network. The first microphone session installs the driver when it is missing. That takes several seconds, and the web UI does not respond during that time.

### How it works

1. **Pairing.** Calliope calls `POST /api/mic/pair` on the config server and shows a 4-digit PIN. The PIN is entered on the Microphone tab of the PIN page. Calliope sends an HMAC-SHA256 proof, keyed by the PIN, over the host certificate fingerprint and a random nonce. The PIN is never sent. A PIN is valid for 120 seconds. Three wrong PINs cancel every pending request. At most four requests are pending at once.
2. **Credential.** A successful pairing gives Calliope a 48-character token. Apollo stores only the SHA-256 hash of the token, with the device name and UUID, in `mic_state.json` beside `sunshine_state.json`.
3. **Session.** Calliope calls `POST /api/mic/session` with the token. Apollo replies with a session ID, a random AES-128 key, and the UDP port. The port is the base port plus 13, which is 48002 by default.
4. **Audio.** Calliope sends Opus frames of 20 ms in AES-128-GCM datagrams. The nonce is built from the direction, packet type, session ID, and a sequence number that only increases. A jitter buffer holds 40 ms before playback starts and at most 100 ms. Lost frames are concealed by Opus.
5. **Playout.** The audio device clock pulls frames from the buffer through WASAPI. While a session runs, Apollo makes the Steam Streaming Microphone the default capture device, and it restores the previous default when the session ends. The previous default is written to `mic_state.json`, so the next start restores it after a crash.
6. **Keepalive and diagnostics.** Calliope sends pings. Apollo answers with a pong that carries an error code and five counters: frames accepted, rejected, concealed, failed to decode, and played. A session with no valid packet for 5 seconds ends with the reason `connection lost`.

Only one microphone is connected at a time. A second device that connects replaces the first, and the first receives pong error code 4 for 5 seconds. A pong is sent only for a ping newer than every earlier ping of that session. The jitter buffer rejects any frame older than the highest sequence it has seen after a restart. Both rules stop a captured packet from being replayed.

The microphone socket stays closed until a microphone is paired. The tray shows notices for an incoming pairing request, a connection, a disconnection, and an error.

### Pong error codes

| Code | Meaning |
|---|---|
| 0 | No error |
| 1 | The Steam Streaming Microphone is missing, or not visible in a Remote Desktop session |
| 2 | The device exists and cannot be opened |
| 3 | Opus decoding failed repeatedly |
| 4 | This session was replaced by another device |

### Status

The host receiver builds on Windows, and its unit tests pass there. The Windows verification checklist passed for pairing, sessions, the tray notices, the device format, session replacement, and the read-only key scope. The handoff document lists the items still unverified, including audible audio, the default-device restore after an unclean exit, and the Remote Desktop message. The pong counters have a macOS test pass and no live check yet. See [the handoff](docs/superpowers/2026-09-20-calliope-mic-handoff.md), [the user guide](docs/remote_microphone.md), and [the design](docs/superpowers/specs/2026-09-20-calliope-mic-sidecar-design.md).

The Calliope app is a separate repository.

## Steam game activity

A paired client that holds the "Share game activity" permission can read the last Steam game played on the PC, artwork for it, and the desktop wallpaper. The Apple TV client uses them to draw the focused PC. The design is in [the specification](docs/superpowers/specs/2026-11-02-steam-activity-tvos-client-design.md).

All routes are on the HTTPS GameStream port (47984 with the default base port) and need the mutual-TLS client certificate that pairing issues. They answer only `GET`.

| Route | Response |
|---|---|
| `/moonlight/steam` | JSON activity object |
| `/moonlight/steam/art/{appId}/{kind}` | Image. `appId` is 1 to 10 digits. `kind` is `hero`, `logo`, or `capsule`. |
| `/moonlight/wallpaper` | The wallpaper image of the signed-in user |

### Activity object

`/moonlight/steam` returns status 200 and one of three shapes.

| Case | Body |
|---|---|
| The host switch is off, or the client lacks the permission | `{"version":1,"sharing":false}` |
| Steam is not installed | `{"version":1,"steamInstalled":false}` |
| Otherwise | The full object below |

The first shape omits `steamInstalled`, so a host that withholds activity does not reveal whether Steam is installed.

| Field | Type | Meaning |
|---|---|---|
| `version` | integer | 1 |
| `steamInstalled` | boolean | `true` |
| `account` | string or null | SteamID64 of the account, `null` when unknown |
| `runningAppId` | integer or null | The running Steam game, `null` when nothing runs |
| `runningName` | string or null | Name of the running game, `null` when `runningAppId` is `null` |
| `lastPlayed` | object or null | `{appId, name, at}`, `at` in Unix seconds |
| `recent` | array | Up to 5 `{appId, name, at}`, newest first |
| `updatedAt` | integer | Unix seconds when Apollo read the files |

Games that are not installed are skipped, because their names are not available on the PC. Apollo caches the object for 30 seconds.

### Art and wallpaper

Art comes from Steam's library cache. Apollo tries the flat layout (`<appId>_library_hero.jpg`) and then the nested layout (`<appId>/<hash>/library_hero.jpg`). The `capsule` kind tries `library_600x900.jpg`, `library_capsule.jpg`, `header.jpg`, and `library_header.jpg` in that order. Art files must be `.jpg`, `.png`, or `.webp`, and at most 8 MiB. A link at `appcache` or `librarycache` is refused. The response carries `Cache-Control: max-age=3600`.

The wallpaper is the registry value `Control Panel\Desktop\WallPaper`, or else `TranscodedWallpaper` in the user's roaming profile. The first bytes must be a JPEG, PNG, or BMP, and the file at most 20 MiB. The response carries `Cache-Control: no-cache`.

Both routes answer 404 when the file is missing, the identifier is invalid, the client lacks the permission, or no user is signed in.

### Sleeping the PC

The Apple TV client offers Sleep in the hold menu. It sends `POST /moonlight/sleep` to the same paired HTTPS channel. The response is the usual GameStream XML.

| Case | Body |
|---|---|
| Accepted | Status 200, `sleep` is 1 |
| The client lacks the `sleep` permission | Status 403, `Permission denied` |
| A stream is running | Status 409, `A session is running` |
| Apollo does not run on Windows | Status 501, `Sleep is not supported on this host` |

Sleep ends any session, so Apollo refuses while a stream runs instead of cutting another device off. Before it suspends, it reverts the display configuration that a finished session may have left behind. It answers 200 first, because `SetSuspendState` does not return until the PC wakes. Suspending needs the shutdown privilege, which Apollo enables on its own process; a failure is logged only, as the client has been answered by then.

### The device that is streaming

The HTTPS `serverinfo` response gains an `activeclient` element. It holds the device name of the running stream, or is empty when none runs. It is empty for a client without the permission. The plain HTTP `serverinfo` does not carry it. Whether stock Moonlight clients ignore the element is not yet confirmed against their parsers.

### Permission and switch

The permission `share_activity` has the value `0x00200000`. The first paired client receives it, as part of full permissions. Later clients do not, until it is granted in the client list on the PIN page. Clients paired before this change do not hold it either. The host setting `share_game_activity` is on by default. When it is off, every client is treated as lacking the permission. See [configuration](docs/configuration.md).

The permission `sleep` has the value `0x00400000` and is granted the same way, as **Sleep the PC** in the client list on the PIN page. It gates `POST /moonlight/sleep`. Pairing a client before this change leaves it without the permission, so an existing client must be granted it before Sleep works from its hold menu.

### Reading user data on Windows

Apollo's process runs as SYSTEM, so the per-user registry hive and the user's files need care. Apollo reads the Steam account, the running game, the Steam files, the art, and the wallpaper while impersonating the signed-in console user, so the user's own access rights apply. The Steam folder comes only from the machine-wide key `HKLM\SOFTWARE\WOW6432Node\Valve\Steam`. Library folders that are relative or network paths are ignored. A KeyValues file over 32 MiB is refused.

When nobody is signed in, Apollo reads nothing. It returns the last object built while someone was signed in, or `{"version":1,"steamInstalled":false}` if there is none, for example after Apollo restarts with nobody signed in. When `loginusers.vdf` is the source of the account, the entry ranks by `MostRecent`, then `AutoLogin`, then the newest `timestamp`.

### Status

The parsing, selection, art lookup, and response logic is cross-platform and has automated tests. The Windows registry reads, the impersonation, and the network behaviour have not been checked on a live host in this repository. `tests/steam_standalone/check_endpoints.py` pairs as a client and compares each endpoint with the Steam files and the registry. Run it on the host as the signed-in user.

The Apple TV client is a separate repository. A version 2 plan for SteamGridDB artwork and a rotating backdrop is in the specification and is not implemented.

## Config API additions

The config server (default port 47990) gains seven routes. Pairing and session routes serve Calliope. The rest serve the web UI.

| Route | Authentication | Purpose |
|---|---|---|
| `POST /api/mic/pair` | None, LAN only | Start a pairing |
| `POST /api/mic/pair/status` | None, LAN only | Poll for the pairing result, which returns the token once |
| `POST /api/mic/pin` | Web UI login | Submit the PIN and a device name |
| `GET /api/mic/list` | Web UI login or read-only API key | List paired microphones |
| `POST /api/mic/remove` | Web UI login | Remove one microphone |
| `POST /api/mic/session` | Mic token | Start a session |
| `DELETE /api/mic/session` | Mic token | End the session |

`GET /api/mic/list` returns `{"status": true, "named_mics": [{"name": "...", "uuid": "...", "connected": false}]}`. At most one entry has `connected: true`, and the response never contains a token. A device name is at most 64 bytes and cannot contain control characters. Full details are in [the API reference](docs/api.md).

## Tests

The microphone and Steam units build without the rest of Apollo. They need CMake, a C++23 compiler, and network access to fetch `nlohmann/json`. The microphone target also needs OpenSSL and libopus.

```bash
git submodule update --init third-party/googletest
cmake -S tests/mic_standalone -B build-mic && cmake --build build-mic && ./build-mic/test_mic_standalone
cmake -S tests/steam_standalone -B build-steam && cmake --build build-steam && ./build-steam/test_steam_standalone
```

The same tests run inside `test_sunshine` when Apollo is configured with `-DBUILD_TESTS=ON`. `tests/mic_standalone/send_test_tone.py` is a reference sender that pairs and streams Opus silence to a host.

## About Virtual Display

> [!WARNING]
> ***It is highly recommend to remove any other virtual display solutions from your system and Apollo/Sunshine config, to reduce confusions and compatibility issues.***

> [!NOTE]
> **TL;DR** Just treat your Artemis/Moonlight client like a dedicated PnP monitor with Apollo.

Apollo uses SudoVDA for virtual display. It features auto resolution and framerate matching for your Artemis/Moonlight clients. The virtual display is created upon the stream starts and removed once the app quits. **If you do not see a new virtual display added or removed when the stream starts or stops, there may be a driver misconfiguration, or another persistent virtual display might still be active.**

The virtual display works just like any physically attached monitors with SudoVDA, there's completely no need for a super complicated solution to "fix" resolution configurations for your devices. Unlike all other solutions that reuses one identity or generate a random one each time for any virtual display sessions, **Apollo assigns a fixed identity for each Artemis/Moonlight client, so your display configuration will be automatically remembered and managed by Windows natively.**

## Configuration for dual GPU laptops

Apollo supports dual GPUs seamlessly.

If you want to use your dGPU, just set the `Adapter Name` to your dGPU and enable `Headless mode` in `Audio/Video` tab, save and restart your computer. No dummy plug is needed any more, the image will be rendered and encoded directly from your dGPU.

## About HDR

HDR starts supporting from Windows 11 23H2 and generally supported on 24H2. Some systems might not have HDR toggle on 23H2 and you just need to upgrade to 24H2. Any system lower than 23H2/Windows 10 will not have HDR option available.

> [!NOTE]
> The below section is written for professional media workers. It doesn't stop you from enabling HDR if you know what you're doing and have deep understanding about how HDR works.
>
> Apollo and SudoVDA can handle HDR just fine like any other streaming solutions.
>
> If you have had good experience with HDR previously, you can safely ignore this section.
>
> If you're curious, read on, but don't blame Apollo for poor HDR support.

Whether HDR streaming looks good, it depends completely on your client.

In short, ICC color correction should be totally useless while streaming HDR. It's your client's job to get HDR content displayed right, not the host. But in fact, it does affect the captured video stream and reflect changes on devices that can handle HDR correctly. On other devices that can't, the info is not respected at all.

It's very complicated to explain why HDR is a total mess, and why enabling HDR makes the image appear dark/yellow. If it's your first time got HDR streaming working, and thinks HDR looks awful, you're right, but that's not Apollo's fault, it's your device that tone mapped SDR content to the maximum of the capability of its screen, there's no headroom for anything beyond that actual peak brightness for HDR. For details, please take a look [here](https://github.com/ClassicOldSong/Apollo/issues/164).

For client devices, usually Apple products that have HDR capability can be trusted to have good results, other than that, your luck depends.

<details>
<summary>DEPRECATION ALERT</summary>

Enabling HDR is **generally not recommended** with **ANY streaming solutions** at this moment, probably in the long term. The issue with **HDR itself** is huge, with loads of semi-incompatible standards, and massive variance between device configurations and capabilities. Game support for HDR is still choppy.

SDR actually provides much more stable color accuracy, and are widely supported throughout most devices you can imagine. For games, art style can easily overcome the shortcoming with no HDR, and SDR has pretty standard workflows to ensure their visual performance. So HDR isn't *that* important in most of the cases.

</details>

## How to run multiple instances of Apollo for multiple virtual displays

Follow the instructions in the [Wiki](https://github.com/ClassicOldSong/Apollo/wiki/How-to-start-multiple-instances-of-Apollo).

## FAQ
Moved to [WiKi](https://github.com/ClassicOldSong/Apollo/wiki/FAQ)

## Stuttering Clinic
Here're some common causes and solutions for stutters: [WiKi](https://github.com/ClassicOldSong/Apollo/wiki/Stuttering-Clinic).

## Device specific setups
- Pixel devices might not be able to use native resolution:
  - Change the device resolution to Max: https://github.com/ClassicOldSong/Apollo/issues/700

## System Requirements

> **Warning**: This table is a work in progress. Do not purchase hardware based on this.

**Minimum Requirements**

| **Component** | **Description** |
|---------------|-----------------|
| GPU           | AMD: VCE 1.0 or higher, see: [obs-amd hardware support](https://github.com/obsproject/obs-amd-encoder/wiki/Hardware-Support) |
|               | Intel: VAAPI-compatible, see: [VAAPI hardware support](https://www.intel.com/content/www/us/en/developer/articles/technical/linuxmedia-vaapi.html) |
|               | Nvidia: NVENC enabled cards, see: [nvenc support matrix](https://developer.nvidia.com/video-encode-and-decode-gpu-support-matrix-new) |
| CPU           | AMD: Ryzen 3 or higher |
|               | Intel: Core i3 or higher |
| RAM           | 4GB or more |
| OS            | Windows: 10+ (Windows Server requires [manual installation](https://github.com/nefarius/ViGEmBus/issues/153) for gamepad support) |
|               | macOS: 12+ |
|               | Linux/Debian: 11 (bullseye) |
|               | Linux/Fedora: 39+ |
|               | Linux/Ubuntu: 22.04+ (jammy) |
| Network       | Host: 5GHz, 802.11ac |
|               | Client: 5GHz, 802.11ac |

**4k Suggestions**

| **Component** | **Description** |
|---------------|-----------------|
| GPU           | AMD: Video Coding Engine 3.1 or higher |
|               | Intel: HD Graphics 510 or higher |
|               | Nvidia: GeForce GTX 1080 or higher |
| CPU           | AMD: Ryzen 5 or higher |
|               | Intel: Core i5 or higher |
| Network       | Host: CAT5e ethernet or better |
|               | Client: CAT5e ethernet or better |

**HDR Suggestions**

| **Component** | **Description** |
|---------------|-----------------|
| GPU           | AMD: Video Coding Engine 3.4 or higher |
|               | Intel: UHD Graphics 730 or higher |
|               | Nvidia: Pascal-based GPU (GTX 10-series) or higher |
| CPU           | AMD: todo |
|               | Intel: todo |
| Network       | Host: CAT5e ethernet or better |
|               | Client: CAT5e ethernet or better |

## Integrations

SudoVDA: Virtual Display Adapter Driver used in Apollo

[Artemis](https://github.com/ClassicOldSong/moonlight-android): Integrated Virtual Display options control from client side

**NOTE**: Artemis currently supports Android only. Other platforms will come later.

## Support

Currently support is only provided via GitHub Issues/Discussions.

No real time chat support will ever be provided for Apollo and Artemis. Including but not limited to:

- Discord
- Telegram
- Whatsapp
- QQ
- WeChat 

> When there's a chat, there're dramas. -- Confucius

## Downloads

### Direct Download

**Recommended**

[Releases](https://github.com/ClassicOldSong/Apollo/releases)

### WinGet

**Note:** Community maintained

In an elevated PowerShell window, run

```pwsh
winget install ClassicOldSong.Apollo

```

You'll need WinGet installed first.

### Chocolatey

**Note:** Community maintained

You can also install the apollo streaming server with chocolatey.

Install Chocolatey if you don't have it, then run the following command in an elevated PowerShell/CMD window:

```pwsh
choco upgrade apollo -y 
```

Same command can be used to upgrade, add to a scheduled task to automate updates.

See more details on the chocolatey package [here](https://community.chocolatey.org/packages/apollo)

## Disclaimer

I got kicked from Moonlight and Sunshine's Discord server and banned from Sunshine's GitHub repo literally for helping people out.

This is what I got for finding a bug, opened an issue, getting no response, troubleshoot myself, fixed the issue myself, shared it by PR to the main repo hoping my efforts can help someone else during the maintenance gap.

Yes, I'm going away. [Apollo](https://github.com/ClassicOldSong/Apollo) and [Artemis(Moonlight Noir)](https://github.com/ClassicOldSong/moonlight-android) will no longer be compatible with OG Sunshine and OG Moonlight eventually, but they'll work even better with much more carefully designed features.

The Moonlight repo had stayed silent for 5 months, with nobody actually responding to issues, and people are getting totally no help besides the limited FAQ in their Discord server. I tried to answer issues and questions, solve problems within my ability but I got kicked out just for helping others.

**PRs for feature improvements are welcomed here unlike the main repo, your ideas are more likely to be appreciated and your efforts are actually being respected. We welcome people who can and willing to share their efforts, helping yourselves and other people in need.**

**Update**: They have contacted me and apologized for this incident, but the fact it **happened** still motivated me to start my own fork.

## License

GPLv3
