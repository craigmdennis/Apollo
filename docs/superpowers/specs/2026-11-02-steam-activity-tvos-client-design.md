# Moonlight for Apple TV and Steam game activity

**Status:** Draft (design)

## Sources and precedence

- **The brief.** A Claude Docs document titled "Brief: Moonlight for Apple TV and the
  Apollo fork" (<https://claude.ai/artifact/64kP66QPMo25d2Y6zXdFNu>). Tab V1 states
  the scope and the interface. Tab V2 lists what was cut from version 1 and why.
- **The V2 board.** The artboard "Apple TV — Computers (V2)" (`TVPortrait.dc.html`)
  in the design canvas "Moonlight Redesign". It is interactive and sets the screen
  into each state through a scenario control.
- **This document.** It records the technical design, the code facts that constrain
  it, and what the implementer verifies.

Where sources disagree, the brief decides scope and copy, the V2 board decides look
and motion, and this document states the resolution.

## Problem

Stock Moonlight shows each host as a plain tile. A tile does not say what the host
is used for, and it says nothing when the host is asleep, which is the state a
viewer sees most.

The new tvOS client is a reskinned Moonlight. The focused host fills the screen with
its last Steam game's art, and a status line says what was last played and when. A
sleeping host cannot answer a request, so the client keeps the last answer and draws
from it. The client also shows who is using a host, why a launch failed, and a
wallpaper for hosts without Steam.

Apollo does not expose this data. Apollo needs new read-only endpoints on the paired
HTTPS channel and one new field in `serverinfo`. The client needs a data layer that
fetches, caches, and draws from them.

## Goals

- A paired host used yesterday shows the correct art and text, awake or asleep.
- A host with no Steam data shows its wallpaper, or its colour and a computer symbol.
  The screen never shows an empty frame.
- The owner of a host decides which paired clients see activity on it.
- A fault in the new code cannot affect pairing, app launch, or streaming.
- Apollo needs no Steam login and no API key. Every Apollo read is read-only.

## Non-goals (version 1)

From the brief:

- Hours played.
- Launchers other than Steam (Game Pass, Battle.net, Epic), and non-Steam shortcuts.
- A remote sleep command, and the Sleep menu item.
- Taking over a stream from another device ("Play on This Apple TV"), and rebuilding
  the virtual display on resume. A device that needs its own resolution quits the
  session from the hold menu and launches again.
- A network test that measures latency and bandwidth to the host. Version 1 uses the
  existing Moonlight port test.
- The relay that refreshes the cache of a sleeping host (version 2).
- A recently played row, launching the last-played game directly, SteamGridDB art
  (version 2), and Top Shelf.
- A rotating backdrop (version 2).
- The iPhone and Mac apps. The data layer is written so they can reuse it.

Added here:

- Compatibility with upstream Sunshine. Apollo is already not wire-compatible, and
  stock clients ignore the new routes and field.

## Anti-goals

- Any change that makes a stream less reliable in exchange for a nicer screen.
  Reliability of streaming outranks every feature in this document.
- Showing activity, the active device's name, or the wallpaper to a client the host
  owner did not allow.
- Displaying the Steam ID anywhere in the interface.

## Terms

| Term | Meaning |
|---|---|
| Host | The Windows PC running Apollo. |
| Client | The tvOS app. |
| Backdrop | The full-screen image behind the row of tiles. |
| Tile | The portrait tile for one host in the row. |
| Status line | The single line above the row that describes the focused host. |
| Activity endpoint | `GET /moonlight/steam`, returns JSON. |
| Art endpoint | `GET /moonlight/steam/art/{appId}/{kind}`, returns an image. |
| Wallpaper endpoint | `GET /moonlight/wallpaper`, returns an image. |
| Sharing permission | A per-client permission bit named `share_activity`. |
| Host switch | A host-wide setting named `share_game_activity`. |
| Steam root | The Steam installation directory on the host. |
| Account ID | The 32-bit Steam account number. It names the `userdata` folder. |
| Steam ID | The 64-bit Steam ID. It equals the account ID plus `76561197960265728`. |
| User context | Code that runs with the signed-in console user's token. |
| Fallback backdrop | The host colour with the `desktopcomputer` symbol. |

## Part A: Apollo

### Mechanisms involved

The change adds one new mechanism and extends four existing ones.

- **Activity reader (new).** Reads Steam files and the wallpaper, and produces the
  responses. The parsing, selection, and file lookup are platform-neutral code that
  takes a Steam root and an account ID, so they compile and test on a Mac. Only
  locating the Steam root, the account, and the wallpaper is Windows-specific.
- **Client permissions (extended).** `crypto::PERM` in `src/crypto.h` gains one bit.
- **HTTPS server (extended).** `src/nvhttp.cpp` registers three routes on
  `https_server` only and adds one element to the HTTPS `serverinfo`. The plain HTTP
  server serves none of them, so only a client with a paired certificate can call
  them. The handlers reuse `get_verified_cert`.
- **Settings and web interface (extended).** One boolean in `config`, one toggle in
  the web interface, one checkbox in the client permission list.

Unchanged: the GameStream XML routes other than `serverinfo`, `/appasset`, `launch`,
`resume`, and `cancel`, the config server, the RTSP control channel, and every
streaming path.

### Reading the signed-in user's data

Apollo's main process does not run as the signed-in user. The Windows service
(`tools/sunshinesvc.cpp`) launches `Sunshine.exe` into the console session with a
SYSTEM token. In that process, `HKEY_CURRENT_USER` is the SYSTEM hive, so a direct
read of `HKCU\Software\Valve\Steam` returns nothing useful.

`src/platform/windows/misc.cpp` already solves this for launched commands:
`retrieve_users_token` returns the console user's token, `impersonate_current_user`
runs a callback as that user, and `override_per_user_predefined_keys` points
`HKEY_CURRENT_USER` at the user's hive. Every per-user read in the activity reader
runs inside that pattern:

1. Obtain the console user's token. If none exists (no one is signed in), read
   nothing.
2. Impersonate the user, open the user's hive with `RegOpenCurrentUser`, read the
   values, and revert.

Per-user reads are the Steam account and running game, the Steam files, and the
wallpaper. All of them run while impersonating the user. When no one is signed in,
`/moonlight/steam` returns the last result built while someone was signed in, which
Apollo keeps in memory, or the no-Steam response when it has none (for example after
an Apollo restart with nobody signed in). The art and wallpaper endpoints return 404.

**Trust boundary.** SYSTEM can read files the signed-in user cannot, and the user
controls the wallpaper path, the contents of Steam's folders, and anything in their
own hive. Two rules keep the user from using Apollo to read such files:

1. Every read of an image whose path derives from user-controlled data (art and
   wallpaper) runs while impersonating the console user, so the user's own access
   rights apply. The bytes are read inside the impersonation and then served.
2. The Steam root comes only from the machine-wide key, which administrators write.
   The user-hive `SteamPath` value is not used.

The art lookup also rejects any link at `appcache` or `librarycache` (a symbolic link
or a junction), because the resolved path must equal the literal path below the root.

### Sharing permission

Add `share_activity = _operation << 5` (`0x00200000`) to the operation group in
`crypto::PERM`. Add it to `_all_opeiations`, which makes it part of `_all`. Do not
add it to `_default`.

Effect on the suggested default:

- The first paired client receives `_all`, so it receives the new bit. This matches
  the intent "on for the owner's own devices".
- Later clients receive `_default`, so the bit is off until the owner turns it on.
- Clients stored before this change hold a numeric `perm` value that lacks the bit.
  The load path masks with `PERM::_all` and does not add the bit. The owner turns it
  on once per existing client.

`pin.html` mirrors the enum and the permission list in JavaScript. Add the bit
(`share_activity: 0x00200000`), its list entry, and an English label in
`public/assets/locale/en.json`. Other locales fall back to English.

### Host switch

Add `share_game_activity` (boolean, default `true`) to `config::sunshine`, parse it
with `bool_f` beside the other settings in `src/config.cpp`, and add one checkbox to
the general tab of the web interface. With the switch off, the host behaves as if
every client lacked the sharing permission. The per-client permission stays the real
gate, which is why the host switch defaults to on.

### Activity endpoint

`GET /moonlight/steam` returns `application/json`.

Resolution order:

1. If the host switch is off or the client lacks the sharing permission, return
   `{"version":1,"sharing":false}`.
2. If the Steam root cannot be found, return `{"version":1,"steamInstalled":false}`.
3. Otherwise return the full object below.

Both short responses use status 200 so the client reads one shape. The first omits
`steamInstalled` on purpose, so a host that withholds activity does not reveal
whether Steam is installed. The client shows the "No Steam library" wording only when
`steamInstalled` is present and `false`.

| Field | Type | Notes |
|---|---|---|
| `version` | integer | `1`. Increases when a field changes meaning. |
| `steamInstalled` | boolean | `true` in the full object. |
| `account` | string or null | Steam ID of the signed-in account, `null` when unknown. Never displayed. |
| `runningAppId` | integer or null | `null` when nothing runs or the value is unavailable. |
| `runningName` | string or null | Name of the running game, from its manifest. `null` whenever `runningAppId` is `null`. |
| `lastPlayed` | object or null | `{appId, name, at}`. `null` when no qualifying game exists. |
| `recent` | array | Up to 5 `{appId, name, at}`, newest first. Includes `lastPlayed`. Optional for the client. |
| `updatedAt` | integer | Unix seconds when the files were last read. |

`at` is Unix seconds. The client formats it.

### Steam data sources

Every location below is the expected one and must be verified against the current
Steam client on both hosts before implementation (see the verification checklist).

| Data | Source |
|---|---|
| Steam root | Registry `HKLM\SOFTWARE\WOW6432Node\Valve\Steam`, value `InstallPath`. The user-hive `SteamPath` is not used. |
| Account ID | User hive, `Software\Valve\Steam\ActiveProcess`, value `ActiveUser`. The value is 0 when Steam is not signed in. |
| Account when none is signed in | `<root>\config\loginusers.vdf`, the entry whose `MostRecent` is `1`. Its key is the Steam ID, so the account ID is the Steam ID minus `76561197960265728`. |
| Running game | User hive, `Software\Valve\Steam`, value `RunningAppID`. 0 maps to `null`. |
| Last played per game | `<root>\userdata\<account ID>\config\localconfig.vdf`, section `UserLocalConfigStore / Software / Valve / Steam / apps / <appId>`, value `LastPlayed`. |
| Library folders | `<root>\steamapps\libraryfolders.vdf`, each `path`, plus `<root>` itself. |
| Game name | `<library>\steamapps\appmanifest_<appId>.acf`, value `name`. |
| Art | `<root>\appcache\librarycache`, see the art endpoint. |

The machine-wide `HKLM` value and `loginusers.vdf` work in every state, so the host
can answer with the last known account after a Windows sign-out.

Parsing:

- Steam text files use the KeyValues format. Write a small recursive parser (quoted
  strings, braces, `//` comments, backslash escapes) in the activity reader. Do not
  add a dependency.
- Match section keys case-insensitively. Steam writes both `Software` and
  `software`.

Selection:

1. Collect every app in the `apps` section with `LastPlayed` greater than 0.
2. Drop any app with no `appmanifest` in any library folder. The name is not
   available locally for an uninstalled game.
3. Sort by `LastPlayed`, newest first. The first entry is `lastPlayed`. The first
   five are `recent`.

The account that is signed in decides the result, so the card follows whoever uses
the host, not whoever owns it.

Cache the assembled result in memory for 30 seconds behind a mutex. Do not cache
images in memory.

Failure handling: a missing file, a parse error, or an exception in any step is
logged at debug level and treated as "that field is unavailable". A handler never
lets an exception reach the server loop.

On Linux and macOS builds the reader compiles but the locate step returns "not
found", so the endpoint answers `steamInstalled: false`.

### Art endpoint

`GET /moonlight/steam/art/{appId}/{kind}` returns the file, or 404.

- `appId` must be 1 to 10 digits. `kind` must be `hero`, `logo`, or `capsule`.
  Anything else returns 404.
- The same sharing checks apply. A client without the permission receives 404, the
  same response as a missing file.
- Steam stores art in two layouts, and the reader tries both in order:
  1. Flat: `librarycache\<appId>_library_hero.jpg`, `<appId>_logo.png`,
     `<appId>_library_600x900.jpg`.
  2. Nested: `librarycache\<appId>\<hash>\library_hero.jpg`, `logo.png`,
     `library_600x900.jpg`. The hash folder name is not known in advance, so list the
     subfolders of `librarycache\<appId>` and take the first match.
- `capsule` falls back to `header.jpg` in the same locations.
- The resolved canonical path must lie inside `librarycache`, and `appcache` and
  `librarycache` must not be links. Reject anything else.
- The file is read while impersonating the console user (see the trust boundary). With
  no user signed in, the endpoint returns 404.
- Reject files over 8 MiB.
- Set `Content-Type` from the extension (`jpg`, `png`, `webp`) and
  `Cache-Control: max-age=3600`.

### Wallpaper endpoint

`GET /moonlight/wallpaper` returns the signed-in user's desktop wallpaper, or 404. It
serves a host that has no Steam library, where the wallpaper fills the backdrop.

- The same sharing checks apply as for the art endpoint, and a client without the
  permission receives 404. The wallpaper shows what the person at the host has open,
  so it is treated as activity data. This is the author's choice, to be confirmed.
- The path is read in user context from the user hive: `Control Panel\Desktop`, value
  `WallPaper`. If the value is empty or the file is missing, use
  `%AppData%\Microsoft\Windows\Themes\TranscodedWallpaper` for the same user. The
  file is opened and read while impersonating that user (see the trust boundary).
- `TranscodedWallpaper` has no extension. Set `Content-Type` from the first bytes of
  the file: JPEG (`FF D8`), PNG (`89 50 4E 47`), or BMP (`42 4D`). Any other content
  returns 404.
- Reject files over 20 MiB. The client reduces the image to the screen size.
- Windows Spotlight and slideshow wallpapers have no single stable file. The first
  version serves whatever the two sources above return and otherwise answers 404. The
  brief lists this as an open question.
- The brief asks whether Apollo can read the wallpaper while running as a service. It
  can, through the user-context pattern described above, provided a user is signed in.
  With no user signed in, the endpoint returns 404.

### Session data in serverinfo

The `serverinfo` response already reports `state` (busy or free), `currentgame`
(the Apollo app identifier), and `currentgameuuid`. The In Use status line needs two
more facts.

**The game name.** `currentgame` identifies an Apollo app, for example Steam Big
Picture, and not the Steam game inside it. The game name is `runningName` from the
activity endpoint. When `runningName` is `null`, the client uses the name of the
Apollo app whose identifier equals `currentgame`, taken from its cached app list.

**The active client name.** Add one element, `activeclient`, to the `serverinfo`
response on the HTTPS server only. Its value is the device name of the running stream
session, which Apollo stores as `launch_session->device_name` (`src/nvhttp.cpp`, set
when the session is launched). The element is empty when no session runs. Stock
clients read `serverinfo` by element name and ignore an element they do not know. The
implementer confirms this against the `moonlight-ios` parser (`ServerInfoResponse.m`)
before merging. The element is not sent on the plain HTTP `serverinfo`, which is
available to unpaired callers. The element is empty when sharing is withheld for the
calling client.

### Launch, resume, and quit

These need no Apollo change. The facts below constrain the client.

- `launch` and `resume` return failures as XML with a `status_code` and a
  `status_message`. Permission failures return 403 with "Permission denied", and
  capture failures return 503 with "Failed to initialize video capture/encoding. Is
  a display connected and turned on?".
- 403 is also returned for "Encryption is mandatory for this host but unsupported by
  the client". The client therefore maps on the pair of status code and message, not
  on the code alone.
- Watch Alongside is a `resume` or `launch` of the running app by a client that holds
  the `view` permission. Apollo allows it while an app runs and skips display setup.
  A client without `view` receives 403.
- Quit is the existing `cancel` request. It ends every session and stops the app
  Apollo launched.
- Test Network is the existing Moonlight port test. The implementer confirms its
  entry point in `moonlight-common-c` (`LiTestClientConnectivity`).

### Files changed

- `src/steam_activity.h`, `src/steam_activity.cpp`: parser, selection, art lookup,
  wallpaper lookup, and the image loaders. Platform-neutral.
- `src/steam_activity_host.cpp`: the Windows locate and impersonation steps behind
  `#ifdef _WIN32`, and the 30-second cache.
- `src/nvhttp.cpp`: three handlers and routes on `https_server`, and the
  `activeclient` element in the HTTPS `serverinfo`.
- `src/crypto.h`: the permission bit.
- `src/config.h`, `src/config.cpp`: the host switch.
- `cmake/compile_definitions/common.cmake` (or the file that lists sources): the new
  source file.
- `src_assets/common/assets/web/pin.html`, the general settings tab,
  `public/assets/locale/en.json`: interface changes.
- A standalone test directory following the pattern of `tests/mic_standalone`, with
  fixture Steam folders.

### Tests

The standalone tests build on a Mac with fixture folders and cover:

1. Parsing a real `localconfig.vdf`, `libraryfolders.vdf`, `appmanifest`, and
   `loginusers.vdf` copied from a host with personal data removed.
2. Selection: newest first, uninstalled games skipped, zero `LastPlayed` skipped.
3. Account ID to Steam ID conversion and back.
4. Art lookup in both layouts, and rejection of `..`, a non-numeric `appId`, and a
   symbolic link leaving `librarycache`.
5. Wallpaper type detection by leading bytes, and rejection of other content.
6. Response shape for each of the three activity resolution cases.

Windows verification is manual, by `curl` with the client certificate against the
live host. Check each case in the edge-case table of Part B.

### Verification checklist for host files

Run on each host before writing code. Each item produces a fixture or a correction to
this document.

1. Read each registry value in the sources table with Registry Editor.
2. Open `localconfig.vdf` and confirm `LastPlayed` is present per app, in Unix seconds.
3. Confirm `libraryfolders.vdf` lists every library, and that games in a second library
   have their manifest there.
4. List `appcache\librarycache` for one old game and one recent game. Record which
   layout each uses and the exact file names.
5. Sign out of Windows and confirm `/moonlight/steam` returns the last result from
   before the sign-out, with its original `updatedAt`, and that Apollo reads no Steam
   file while signed out.
6. Confirm Steam in offline mode still writes `LastPlayed`.
7. Log the `HKCU` Steam values from inside Apollo, once without and once with user
   impersonation, to confirm the user hive is the one that holds them.
8. Check `Control Panel\Desktop\WallPaper` and `TranscodedWallpaper` on a host with a
   single picture and on a host with a slideshow or Spotlight wallpaper.
9. Quit Steam Big Picture from the hold menu and record whether a game started from
   inside Steam stops or keeps running.

## Part B: tvOS client

### How the client is made

The client forks `moonlight-ios`, which contains a tvOS target and the connection,
pairing, streaming, and input code. The licence is GPLv3, so the fork is published
under the same licence. Pairing, streaming, and input stay unchanged.

### Base evaluation (initial)

This evaluation reads `moonlight-ios` at commit `02dc978` (2026-09-26). It is a
reading of the source and the open issue list, not a test run. Each finding
carries a verification step.

**Structure.**

- The app is Objective-C with UIKit. The app target contains no Swift.
- Three storyboards define the interface: `iPhone.storyboard`, `iPad.storyboard`,
  and `Moonlight TV/Base.lproj/Main.storyboard`. About 85 `TARGET_OS_TV`
  conditionals in 18 files separate tvOS from iOS behaviour.
- `ViewControllers/MainFrameViewController.m` (1,440 lines) holds discovery, host
  state, pairing, wake, the app list, and launch.
- The layers below the interface are separate and reusable: `Network`
  (`HttpManager`, `PairManager`, `DiscoveryManager`, `WakeOnLanManager`), `Stream`,
  `Crypto`, `Input`, and `Database`.

**Findings.**

1. *A new TLS connection for every request.* `HttpManager
   executeRequestSynchronously:` creates a new `NSURLSession` per request and
   invalidates it afterwards. Each request therefore opens a new connection with
   mutual client-certificate authentication. `DiscoveryWorker` polls every host
   every 2 seconds on its own thread. With ten hosts this is five handshakes per
   second while the list is on screen. Candidate improvement: one long-lived session
   per host, and a slower poll for a host that is offline. Verify by counting
   handshakes on the host with ten paired entries before changing anything.
2. *The server certificate check is correct.* `HttpManager` compares the presented
   certificate byte for byte with the stored one and rejects a mismatch. No change.
3. *Inherited tvOS defects.* 58 of the 161 open upstream issues mention tvOS or Apple
   TV. Those that could affect this client: oversaturated HDR (#582), SDR output
   (#685), no sound (#598), 5.1 audio (#371), multiple controllers (#657), the
   bitrate cap (#658), 1440p (#393), and Top Shelf (#449). Version 1 does not fix
   them. The implementer reproduces each on the target Apple TV and records the
   result in this document.
4. *No upstream interface work to merge with.* Upstream issue #713 asks about a
   SwiftUI tvOS interface and has no implementation. The fork carries its own
   interface and diverges from upstream at that layer.

**Recommended architecture (the author's call, to be confirmed).**

- Keep the Objective-C core (`Network`, `Stream`, `Crypto`, `Input`, `Database`)
  unchanged, so pairing, streaming, and input keep their upstream behaviour.
- Build the new interface in SwiftUI and bridge to the core. The brief specifies
  Liquid Glass materials (tvOS 26), a system context menu, custom dialogs and button
  styles, and a sliding panel. These are SwiftUI features and do not map onto the
  existing tvOS storyboard.
- Replace `MainFrameViewController` with three Swift types: a host store that wraps
  `DiscoveryManager` and `TemporaryHost`, a `HostCardState` enumeration with one case
  per state in the table below, and the `SteamActivityStore` described below.
- Host `StreamFrameViewController` unchanged inside a `UIViewControllerRepresentable`.
- Reject restyling the existing storyboard. The state machine cannot be expressed in
  the current controller without growing it further.

### tvOS dependencies decided in the brief

| Part of the design | tvOS piece | Decision |
|---|---|---|
| Hold for options | System context menu (tvOS 14 and later) | Use it. |
| Remove and In Use dialogs | Custom views | Build them. A system alert cannot show a solid red focused button, so focus, Back, and VoiceOver need building and testing. |
| Buttons that do not grow on focus | Custom button style | Apply everywhere. Focused buttons turn white with black text. |
| Glass badges, spinners, menu, panel | Liquid Glass, tvOS 26 | Earlier tvOS versions get a flat, blurred fallback. The minimum version depends on the SF Symbols used. |
| Settings › Apps › Moonlight | App Settings bundle | Check that tvOS lists third-party bundles and that every option fits its toggles and pickers. |
| Address entry | System keyboard and iPhone keyboard notification | Use them. |
| Spinners at 32 points | System spinner | The system spinner has fixed sizes. A custom spinner may be needed. |
| Discovery and Wake-on-LAN | Bonjour and local network access | Check whether tvOS asks for local network permission and add the Bonjour service names Moonlight uses. |

The symbols in use are `lock.desktopcomputer`, `desktopcomputer`, `plus.circle.fill`,
`gearshape.fill`, `clock.fill`, `gamecontroller.fill`, and `lock.fill`. The warning
icon in the board is a stand-in for `exclamationmark.triangle.fill`. The
implementer records each symbol's minimum tvOS version.

### Data layer

One store per host, keyed by the host's UUID, persisted in Application Support:

- the last activity JSON,
- the hero image,
- the wallpaper image,
- the fetch time of each.

Fetching reuses the client certificate and the HTTPS port the client already uses for
pairing and the app list. No new credential is created.

Fetch triggers for the activity JSON and the `activeclient` value:

1. A paired host comes online.
2. The app enters the foreground.
3. Every 60 seconds while the computer list is on screen.

The wallpaper is fetched on triggers 1 and 2 only, because the file can be several
megabytes. This is the author's choice, to be confirmed.

A fetch that fails or times out leaves the store unchanged.

Cache lifetime: until the host is removed, or until a response reports a different
`lastPlayed.appId` (the hero is replaced) or the wallpaper is fetched again.

### Backdrop order

For a paired host:

1. The last game's hero, from the art endpoint.
2. The same hero from Steam's content delivery network:
   `https://cdn.cloudflare.steamstatic.com/steam/apps/<appId>/library_hero.jpg`. Newer
   games use hashed paths, so a 404 is expected for some. Verify the host name.
3. The `capsule` or header image, if the game has no hero.
4. The wallpaper, when the host has no last-played game.
5. The host colour with the `desktopcomputer` symbol.

The wallpaper appears on the backdrop only, never on the tile. An unpaired host uses
one generic image bundled with the app, because nothing can be fetched from a host
that has not paired. The tile shows the game art, or the `desktopcomputer` symbol
when there is none, and the host name below it. An unpaired tile shows
`lock.desktopcomputer`.

### Status line

One line, with a dot divider between what happened and what to do next. An icon
precedes it: `clock.fill` for last played, `gamecontroller.fill` for currently
playing, a spinner while something is in progress, a warning or lock icon on errors.
Icons and spinners are 32 points everywhere.

| Situation | Line |
|---|---|
| Last played | "Last played `<name>` · `<when>`" |
| In use | "Currently playing `<game>` · `<device>`" |
| In use, device name withheld | "Currently playing `<game>`" (the author's choice, to be confirmed) |
| Steam not installed | "No Steam library on this PC" |
| Not paired | "Found on your network · Not paired with this Apple TV yet" |

`<when>` comes from `RelativeDateTimeFormatter` with named style, capitalised
("Yesterday", "2 hours ago"). The title above the line is empty by decision, because
the tile already names the host. Strings for the remaining states are in the brief.

### Host states and sources

| State | Badge | How the client knows | Press does |
|---|---|---|---|
| Offline | Offline | `serverinfo` unreachable | Wake-on-LAN, then start Steam Big Picture |
| Waking | Offline | Wake sent | Nothing |
| Online | Online | `serverinfo` state free | Start Steam Big Picture, or resume the running app |
| Connecting | Online | Launch in progress | Nothing |
| In use | In Use | `serverinfo` state busy, `currentgame` above 0 | Dialog: Watch Alongside or Cancel |
| Not paired | Not Paired | Pairing state in `serverinfo` | Start pairing |
| Pairing | Not Paired | Pairing request open | Nothing, the line shows the PIN |
| Pairing failed | Not Paired | Pairing request rejected | Try pairing again |
| Error | Error | A launch, wake, or connection failed | Try again from the start |

Wake-on-LAN needs the host's MAC address, which the client stores from an earlier
`serverinfo`. A host never seen online cannot be woken, and the error line says so.

### Errors

| Situation | Status line | Source |
|---|---|---|
| Wake failed | "Couldn't wake the PC · Check that Wake-on-LAN is turned on" | Client |
| Connection failed | "Couldn't connect to the PC · Press OK to try again" | Client |
| Stream dropped | "The stream ended unexpectedly · Press OK to reconnect" | Client |
| Not allowed to start apps | "Apollo hasn't allowed this Apple TV to start apps · Change it in Apollo on the PC" | `status_code` 403 and message "Permission denied" |
| No display | "The PC has no display to stream · Turn on a virtual display in Apollo" | `status_code` 503 and the capture message |
| Anything else | "Couldn't reach this computer · Press OK to try again" | Client |

### Hold menu

A hold on a tile opens the system context menu above it. For a paired host:

1. Open Steam Big Picture and Open Desktop.
2. "Quit `<app>`", when an app runs (Online or In Use). It sends `cancel`.
3. Test Network, using the existing Moonlight port test. The result shows in the
   status line. The board's "Excellent · [N] ms · [N] Mbps" result is replaced by the
   port test result.
4. Remove Computer, in destructive red. It asks "Remove `<name>`?" with "To stream
   from it again, you'll need to pair it again." Cancel has focus first.

An unpaired host lists Test Network and Remove Computer only. Holding the Add tile does
nothing. Interface copy not given here comes from the brief, section "The Apple TV
app".

### Pairing and adding

Pairing shows a four-digit PIN with a spinner. The person types the PIN into Apollo on
the host. Pairing starts only when the person selects an unpaired tile with OK, and
cancels when focus moves away. Resting on a tile starts nothing: an unpaired computer
that is merely focused looks like any other card. A new PIN replaces the old one when
it expires. The lifetime is a prototype value of 30 seconds and the real value is
undecided.

Add Computer opens a full-screen field prefilled with the first three parts of the
Apple TV's own address. A search follows, then a new Not Paired tile or the "Couldn't
find a computer at `<address>`" line.

Pairing on focus was tried in the prototype and rejected: starting a request on a
computer the person only looked at was confusing, and it read as something to stop
rather than something they had asked for.

### Settings

In the app, Stream Settings (the gear) slides in from the right: Resolution, Frame
Rate, Bitrate, HDR, Statistics Overlay, and a last row that opens the Settings app. In
Settings › Apps › Moonlight: Video Codec, Frame Pacing, Audio Channels, Play Audio on
Computer, Swap A/B and X/Y, Automatic Controller Detection, and Optimize Game Settings.
The option values are in the brief.

### Motion

Taken from the V2 board and the brief:

- The hero cross-fades and shifts 80 px when focus moves (0.3 s opacity, 0.45 s
  transform).
- The options menu scales in over 0.18 s. The Stream Settings panel slides in over
  0.32 s on a fading scrim.
- With Reduce Motion on, the hero fades without the shift, and menus and panels appear
  without animating.
- Focused buttons do not grow. They turn white with black text. Destructive buttons
  turn solid red `#FF3B30` with white text.

### Edge cases

| Situation | What the screen shows |
|---|---|
| Host offline, data cached | Cached backdrop and "Last played …", Offline badge |
| Host offline, nothing cached | Fallback backdrop, Offline badge |
| Another Steam account in use on the host | That account's last game |
| `steamInstalled` is `false`, wallpaper available | Wallpaper on the backdrop, "No Steam library on this PC" |
| `steamInstalled` is `false`, no wallpaper | Fallback backdrop, "No Steam library on this PC" |
| `sharing` is `false` | Fallback backdrop, no wording |
| Non-Steam game running | The last Steam game |
| Game has no hero art | Capsule or header image, else wallpaper or fallback backdrop |
| Host not paired | Bundled generic backdrop, `lock.desktopcomputer`, no request sent |
| Steam in offline mode | Normal, because Apollo reads local files |
| Host in use by another device | In Use badge, "Currently playing …", Watch Alongside or Cancel |
| Apollo refuses to start apps | Error badge, the not-allowed line |
| No display on the host | Error badge, the no-display line |

## Order of work

1. Run the verification checklist on both hosts and update Part A.
2. Activity reader and standalone tests.
3. Endpoints, `activeclient`, permission bit, host switch, web interface.
4. Test against the live host with `curl`.
5. Reproduce the inherited tvOS defects (Base evaluation, finding 3) and measure
   the handshake rate (finding 1) on the target Apple TV.
6. Client data layer against the live host.
7. SwiftUI shell, host states, and the status line.

Part A has no dependency on Part B. Part B step 6 needs step 3.

## Open items

These need a decision before the matching step.

1. The Apple TV model, and the minimum tvOS version once the symbols are checked.
2. The pairing PIN lifetime.
3. Where the game logo appears. The brief's goal names a logo, and the layout has no
   place for one now that the title is empty. Until a placement exists, the client
   does not fetch logos.
4. The default sharing permission for clients of the second host.
5. How to handle slideshow and Spotlight wallpapers (see the wallpaper endpoint).
6. Whether quitting Steam Big Picture also closes a game started from inside it
   (checklist item 9).
7. In version 2, whether `rtime_last_played` is returned for both accounts.

## Version 2

Decided during Windows host testing on 2026-11-03. Version 1 ships without these.

### Why

Steam's local art is too small for a television. The capsule is 300×450 and the
header 460×215. Only the hero (1920×620) and the logo (640×224) are usable as they are.
The Apple TV is where this screen is seen, so the backdrop needs the highest
resolution available and should not sit on one still image.

### SteamGridDB art

- Source: the SteamGridDB API v2 (`https://www.steamgriddb.com/api/v2`), queried by
  Steam app ID.
- Assume the app is published. Each user then supplies their own SteamGridDB API key,
  because every key belongs to a SteamGridDB account, and SteamGridDB's terms on
  shipping one shared key in a published app are unconfirmed. Ask SteamGridDB before
  relying on a shared key.
- The key lives in Apollo, entered once per host in the web UI, so every client of
  that host gets the art without its own setup. This drops the version 1 goal
  "Apollo needs no Steam login and no API key" for version 2.
- Apollo fetches from SteamGridDB, caches the images on disk, and serves them through
  the existing art endpoint with the same sharing checks. Local Steam art stays the
  fallback when there is no key, no network, or no result.
- A SteamGridDB failure must not affect pairing, app launch, or streaming (see
  Anti-goals).

### Choosing the image

- Take the first result. Do not build a picker.
- Before taking it, filter out NSFW, humor, and epilepsy-flagged images, and animated
  images. Confirm the filter parameters against the API documentation.
- Take that image at its maximum resolution, preferring a 16:9 image where the API
  offers one.

### Display

- The backdrop uses aspect fill (object fill): the image covers the whole screen and
  is cropped where its aspect ratio differs from 16:9. SteamGridDB heroes are wider
  than 16:9 (for example 1920×620 or 3840×1240), so the sides are cropped.

### Rotating backdrop

- While a host is focused, the backdrop rotates through the hero of each recent game
  (up to five), starting with the last-played game. Each image is the first
  SteamGridDB result for that game, or the local hero when there is none.
- Each image shows for about 8 to 10 seconds and changes with the same cross-fade as a
  focus change.
- A game with no hero is skipped. A host with no Steam games shows its wallpaper as in
  version 1.
- The status line keeps describing the last-played game. It does not change with each
  image.
- With Reduce Motion on, rotation stops and the last-played hero is shown.
- The client's data layer caches up to five heroes per host instead of one.

### Fetching and caching

The client never knows where an image came from. It asks the host's art endpoint, and
the host answers with the SteamGridDB image when it has one and the local Steam image
otherwise. Two caches each do one job:

| Cache | Lives on | Job |
|---|---|---|
| Image cache | Host (Apollo) | Avoid a SteamGridDB request per art request, and keep serving when SteamGridDB is slow or down. |
| Per-host store | Client | Show the right art when the host is asleep or off (version 1 goal). |

Host:

- When the activity reader sees a game in `recent` with no cached SteamGridDB image, it
  fetches that game's images in the background and saves them on disk, keyed by app ID
  and kind.
- The art endpoint never waits on SteamGridDB. Until the fetch finishes, it serves the
  local Steam image.
- Cached images are refreshed after about 30 days, because SteamGridDB rankings change
  as people vote. A failed refresh keeps the old image.
- Images for games no longer in `recent` may be removed after the same period.

Client:

- The fetch triggers stay as in version 1. The activity JSON is small, so the 60-second
  poll detects a game change cheaply.
- Images are keyed by app ID. When `recent` changes, the client fetches only the heroes
  it does not hold and drops those no longer listed. Usually that is one image.
- A fetch that fails leaves the store unchanged, as in version 1.

## Known limitation

Steam records a last-played time for every item in its library, and the library
includes programs that are not games. If the most recent item is a program, it
appears as the last game. Version 1 accepts this. A later change can read the app
type from Steam's binary app cache to filter programs out.

## Assumptions to confirm

- The fork of `moonlight-ios` is the client base, with a SwiftUI interface over the
  existing Objective-C core.
- `sharing: false` is a new response shape. It replaces the brief's "only
  `steamInstalled`", which cannot distinguish "withheld" from "no Steam".
- The host switch defaults to on.
- Uninstalled games are skipped when choosing the last-played game.
- The new routes use the `/moonlight/` prefix from the brief, although existing
  Apollo routes have no prefix.
- The `activeclient` element in `serverinfo` is ignored by stock clients.
- The wallpaper endpoint is gated by the sharing permission, and the client fetches
  the wallpaper only when a host comes online or the app returns to the foreground.
- When the device name is withheld, the In Use line omits it.
