# Distributing Apollo to your own machines

Private distribution to your own Windows hosts via **GitHub Releases on your fork**
(`craigmdennis/Apollo`). One machine builds the installer; every machine pulls it.
Works with a private repo — only `gh` auth is needed.

## One-time setup on each target machine
1. Install the GitHub CLI and sign in:
   ```powershell
   winget install GitHub.cli
   gh auth login          # GitHub.com -> HTTPS -> browser
   ```
2. Install Apollo once (run any release installer, or your current install) so the
   `ApolloService` and SudoVDA driver exist.
3. Copy `scripts\update-apollo.ps1` onto the machine (or fetch it from the repo).

## Cut a release (on the dev machine with the MSYS2 toolchain)
From the repo root, on the commit you want to ship (e.g. `master`):
```powershell
pwsh -File scripts\publish-release.ps1 -Version 0.5.0
```
This builds a clean Release with the version baked in, makes the NSIS installer,
tags `v0.5.0`, pushes the tag to the remote that points at your fork (`origin`,
`fork`, or whatever else it is called), and creates a GitHub Release with
`Apollo-v0.5.0.exe` attached. (The first build is slow — Boost compiles from
source; later releases reuse `cmake-build-release` and are incremental.)

## Update a machine
In the app: right-click the Apollo tray icon → **Check for Updates**. It reads the latest
release from GitHub and tells you what it found, then asks twice:

1. **Download it now?** Nothing has been installed yet. Say no and nothing changes.
2. **Install it now?** This is the point of no return — Apollo stops, the installer runs
   silently, and the service starts again on the new build. Any active stream ends.

The download is verified against GitHub's published SHA-256 before either question moves
on, and a declined install keeps the installer, so the next attempt skips straight to the
install prompt. Pairings, settings, and cover art are untouched — only the program files
are replaced.

The updater runs as a detached process, so it survives the service stop that would
otherwise kill Apollo mid-update. Its log is `%ProgramData%\Apollo\updater\updater.log`.

### Bootstrapping a machine that has no in-app updater
Machines running a build from before the tray item existed need the standalone script once:
```powershell
powershell -ExecutionPolicy Bypass -File update-apollo.ps1
```
It self-elevates, downloads the latest release installer, stops the service,
installs silently (`/S`), restarts the service, and records the installed tag so
re-runs are no-ops until the next release. Use `-Force` to reinstall the latest.
After that, the tray item is the normal path and this copy is only a fallback.

> The two `update-apollo.ps1` files are different. The one in this folder is the
> gh-CLI bootstrap described above; the one the tray runs ships in the installer at
> `%ProgramFiles%\Apollo\scripts\update-apollo.ps1` and needs no `gh` login.

### Hands-off updates (optional)
Register a Scheduled Task so each machine checks daily:
```powershell
$action  = New-ScheduledTaskAction -Execute powershell.exe `
  -Argument "-NoProfile -ExecutionPolicy Bypass -File C:\path\to\update-apollo.ps1"
$trigger = New-ScheduledTaskTrigger -Daily -At 4am
Register-ScheduledTask -TaskName "Apollo Auto-Update" -Action $action -Trigger $trigger `
  -RunLevel Highest -User "SYSTEM"
```

## Notes / caveats
- **Bare pushes to `master` do not rebuild** — you publish a build by cutting a
  release with `publish-release.ps1`. (Auto-build-on-push needs CI; not set up.)
- Releases must carry a real version number for the in-app update check to compare
  them. `publish-release.ps1` passes `BRANCH` along with `BUILD_VERSION` for this
  reason; without it the build reports `0.0.0.<sha>`.
- The first time you run `update-apollo.ps1`, verify the **silent install** behaves
  (the SudoVDA driver step is the thing to watch). If a release changes the driver,
  Windows may prompt to trust it.
- The installer needs admin; the updater self-elevates.
- `update-apollo.ps1` requires at least one published release to exist.
