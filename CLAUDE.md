# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Apollo is a self-hosted game-stream host (low-latency desktop streaming with hardware
encoding) paired with the **Artemis / Moonlight Noir** client. It is a **fork of
LizardByte's Sunshine** and the lineage is load-bearing: the npm package is named
`sunshine`, the project FQDN is `dev.lizardbyte.app.Sunshine`, the test binary is
`test_sunshine`, and all compile definitions / env vars use the `SUNSHINE_` prefix. When
searching the codebase, expect "Sunshine" even though the product is "Apollo". Docs link
to `LizardByte/Sunshine`, but this fork diverges and is **not** wire-compatible with
upstream Sunshine/Moonlight.

The stack is a **C++ core** (the streaming server) plus a **Vue 3 web UI** for
configuration and client pairing.

## Build & test

Submodules are required — clone with `git clone ... --recurse-submodules` (or
`git submodule update --init --recursive`). `third-party/` holds `moonlight-common-c`,
`Simple-Web-Server`, `googletest`, `inputtino`, `ViGEmClient`, `libdisplaydevice`, and more.

```bash
cmake -B build -G Ninja -S .   # configure (defaults to Release)
ninja -C build                 # build the `sunshine` binary + web UI
```

- Build options live in `cmake/prep/options.cmake` (e.g. `-DBUILD_TESTS=ON`,
  `-DBUILD_DOCS=ON`, `-DBUILD_WERROR=ON`, `SUNSHINE_ENABLE_*` feature toggles).
- The **web UI** is built by the `web-ui` CMake target, which runs `npm install` via
  `find_program(NPM npm)` — so `node`/`npm` must be on `PATH` before running `cmake`.
- `cpack -G <DEB|RPM|DragNDrop|NSIS|ZIP> --config ./build/CPackConfig.cmake` packages installers.

### Tests (GoogleTest)

Tests are **off by default**. Build with `-DBUILD_TESTS=ON`; the executable
`test_sunshine` lands in the `tests/` subdir of the build directory.

```bash
cmake -B build -G Ninja -S . -DBUILD_TESTS=ON && ninja -C build
./build/tests/test_sunshine                              # run all
./build/tests/test_sunshine --gtest_filter='SuiteName.*' # run a subset / single test
```

Tests are split into `tests/unit/` and `tests/integration/`; fixtures in `tests/fixtures/`.
Note `.github/CONTRIBUTING.md`: **don't trust AI-generated tests — verify each change manually.**

### Web UI (standalone, for fast iteration)

```bash
npm install
npm run dev     # vite build --watch
npm run build   # one-off debug build
```

Vite reads source from `src_assets/common/assets/web` and emits to `build/assets/web`
(`vite.config.js`). CMake overrides these paths via `SUNSHINE_SOURCE_ASSETS_DIR` /
`SUNSHINE_ASSETS_DIR`.

### Windows builds

Per `.github/copilot-instructions.md`: build with **MSYS2 / UCRT64**, prefixing commands
with `C:\msys64\msys2_shell.cmd -defterm -here -no-start -ucrt64 -c`. Prefix build
directories with `cmake-build-`. Ensure the official `node.exe` is on `PATH` (the
MSYS2 `nodejs` package is intentionally avoided — see `docs/building.md`).

## Code style

- C/C++: follow `.clang-format` exactly (LLVM-based, 2-space indent, `ColumnLimit: 0`,
  right-aligned pointers). This is a hard requirement for contributions.
- Python (helper scripts): `.flake8`. JS/Vue: Prettier (`.prettierrc.json`).
- `CMAKE_EXPORT_COMPILE_COMMANDS` is on, so `build/compile_commands.json` is available for tooling.

## Architecture (the big picture)

`src/main.cpp` is the orchestrator. It parses CLI subcommands (`creds`, `help`,
`version`, plus Windows-only nvprefs helpers), initializes subsystems
(`logging` → `config` → `display_device` → `platf` → `proc` → `input`), probes video
encoders, then **launches three long-running server threads and joins them**:

| Thread | File | Role |
|--------|------|------|
| `nvhttp::start`     | `src/nvhttp.cpp`     | **GameStream protocol** server — client pairing & app list (the API Artemis/Moonlight talks to). |
| `confighttp::start` | `src/confighttp.cpp` | **Web UI config** server — serves the Vue app, handles PIN pairing & permission management. |
| `rtsp_stream::start`| `src/rtsp.cpp`       | **RTSP control channel** — negotiates a streaming `Session`. |

Streaming data path: `rtsp.cpp` sets up a session → `stream.cpp` (RTP video/audio/control
protocols, built on the `moonlight-common-c` submodule) pulls from `video.cpp` (capture +
encode) and `audio.cpp`. Encoders live in `src/nvenc/` (NVIDIA D3D11 / D3D11-on-CUDA) and
in platform backends (VAAPI, etc.).

Supporting modules in `src/`:
- `process.cpp` (`proc::`) — launches/stops the configured apps for a session and runs
  its prep/undo commands.
- `input.cpp` — injects gamepad/keyboard/mouse (via `inputtino`; `ViGEmClient` on Windows).
- `config.cpp` — global config singletons (`config::sunshine`, `config::video`, `config::stream`).
- `crypto.cpp` — pairing certificates & encryption. `httpcommon.cpp` is shared HTTP plumbing
  for both HTTP servers. `upnp.cpp` does port mapping; `network.cpp`, `uuid.h` round it out.
- `display_device.cpp` — resolution / refresh-rate / HDR management (wraps the
  `libdisplaydevice` submodule). On Windows the auto-created virtual display uses
  `src/platform/windows/virtual_display.*` backed by the **SudoVDA** driver.

### Platform abstraction

`src/platform/common.h` defines the OS-agnostic interface; `platf::init()` selects the
implementation under `src/platform/{windows,linux,macos}/` (capture, audio, input,
display, mDNS publishing). Windows is the most complete backend (virtual display, HDR);
Linux uses X11/Wayland/KMS grab + VAAPI/CUDA; macOS uses AVFoundation. When changing
capture/input/display behavior, expect to touch all three backends or guard with platform
`#ifdef`s.

### Web UI

`src_assets/common/assets/web/` is a **Vue 3 multi-page app** — each top-level page
(`index`, `apps`, `config`, `pin`, `password`, `login`, `welcome`, `troubleshooting`) is a
separate HTML entry in `vite.config.js`'s `rollupOptions.input`. A `vite-plugin-ejs`
prebuild injects a shared `template_header.html`. i18n uses `vue-i18n` with translations
managed through Crowdin (`crowdin.yml`). The built assets are served by `confighttp`.

## Permission model (important runtime behavior)

Apollo gates client capabilities per-device. The **first** paired client gets FULL
permissions; subsequent clients get only `View Streams` + `List Apps`. A client that
can't launch apps or send input is almost always missing the relevant permission rather
than hitting a bug — grant `Launch Apps` / `Mouse Input` / `Keyboard Input` in the web UI.

## Docs

Authoritative docs are in `docs/` (`building.md`, `configuration.md`, `getting_started.md`,
`troubleshooting.md`, etc.) and on the project Wiki. `docs/changelog.md` tracks releases.

## Blog content capture

This project is tracked for a portfolio blog post. Throughout sessions, maintain `blog/notes.md`.

Log the user's thinking — not the assistant's process. Entries are about the user's decisions, questions, and changes, written from their perspective:
- The user's key decisions and the reasoning behind them (especially where they rejected the obvious approach)
- Questions the user asked, and what was at stake in them
- Moments the user changed direction mid-build, and why
- Trade-offs and constraints the user weighed; anything that changed how they think about the problem

Do not log issues the assistant ran into — tool errors, debugging detours, bugs you fixed. This is the user's story. If a problem matters, capture the user's decision or question it triggered, not your struggle with it.

Capture the thinking live — don't just reconstruct it afterwards. When the user makes a non-obvious decision, rejects an approach, changes direction, or asks a sharp question, treat it as a logworthy moment in the same turn. If they already explained their reasoning in the conversation, record it in their own words. If the reasoning is unstated, ask one short question to draw it out (what tipped the decision, what they were weighing, what worried them) and log their answer. Keep it low-friction: prompt only at genuine decision points, never mid-flow for trivia, and drop it the moment they'd rather not.

**With every capture, also plan and present story beats.** Alongside logging the raw note, sketch the narrative beats it could become — the hook, the tension/wall, the turn or insight, the payoff — and show them to the user in the same turn. The note is the record; the beats are how it might read as a post. Keep them brief (a few bullets, not prose), group related captures into candidate post arcs, and frame everything from the user's perspective. The user explicitly asked for this on every capture.

Format each entry as:

```
**YYYY-MM-DD — Short title**
One to three sentences. Raw observations only — the `portfolio-content` skill turns them into prose.
```

Blog posts, images, and drafts go in `blog/`. This folder is gitignored — nothing in it is committed to the repo.
