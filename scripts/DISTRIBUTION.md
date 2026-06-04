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
tags `v0.5.0`, pushes the tag to the fork, and creates a GitHub Release with
`Apollo-v0.5.0.exe` attached. (The first build is slow — Boost compiles from
source; later releases reuse `cmake-build-release` and are incremental.)

## Update a machine
```powershell
powershell -ExecutionPolicy Bypass -File update-apollo.ps1
```
It self-elevates, downloads the latest release installer, stops the service,
installs silently (`/S`), restarts the service, and records the installed tag so
re-runs are no-ops until the next release. Use `-Force` to reinstall the latest.

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
- The first time you run `update-apollo.ps1`, verify the **silent install** behaves
  (the SudoVDA driver step is the thing to watch). If a release changes the driver,
  Windows may prompt to trust it.
- The installer needs admin; the updater self-elevates.
- `update-apollo.ps1` requires at least one published release to exist.
