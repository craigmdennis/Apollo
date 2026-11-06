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

> **Just want to build without publishing?** Use `scripts\build-windows.ps1` instead —
> it does the same configure/build/package steps but skips the tag, push, and GitHub
> Release.

## Cut a release (on the dev machine with the MSYS2 toolchain)
From the repo root, on `master`, after the PR you want to ship is merged:
```powershell
powershell -ExecutionPolicy Bypass -File scripts\publish-release.ps1 -Version 0.7.0
```
(Windows PowerShell 5.1 is enough — `pwsh`/PowerShell 7 is not required.)
What it does:
1. Fetches `origin` and resolves the commit to release — by default the tip of the
   remote default branch (`master`), so a merge commit is included automatically.
2. Refuses unless local `HEAD` is exactly that commit and tracked files are clean, so
   the installer is built from the tree the tag will point at.
3. Builds a Release with the version baked in and makes the NSIS installer.
4. Runs `gh release create v0.7.0 --target <sha>`, which creates the tag **at that
   commit on GitHub** and attaches `Apollo-v0.7.0.exe` in one step.

Omit `-Version` to bump the minor of the newest tag (`v0.6.0` → `0.7.0`). Preview
without changing anything with `-DryRun`.

Already built the installer elsewhere? Publish it without rebuilding:
```powershell
powershell -ExecutionPolicy Bypass -File scripts\publish-release.ps1 -Version 0.7.0 -InstallerPath .\Apollo.exe
```
Release an older commit that is still on `master` (e.g. a hotfix): `-Target <sha>`.
The first build is slow — Boost compiles from source; later releases reuse
`cmake-build-release` and are incremental.

> **Merging a PR does not tag or release anything.** `master` moves; the tag only
> exists once `publish-release.ps1` (or a manual GitHub release) creates it. A tag
> always means "this exact commit is what shipped".

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
- **Merging a PR does not create a tag or release.** `master` simply moves; you cut a
  release explicitly with `publish-release.ps1`. (Auto-build-on-push needs CI; not set up.)
- The tag is created by `gh release create --target`, so it points at the resolved
  commit even if another machine did the build. The script then verifies the remote
  tag resolves to that commit.
- `-Target` must be a commit already on the default branch; the script refuses to tag
  a feature branch.
- A prebuilt `-InstallerPath` is uploaded as-is; the script cannot verify it was built
  from the target commit, so check that yourself.
- The first time you run `update-apollo.ps1`, verify the **silent install** behaves
  (the SudoVDA driver step is the thing to watch). If a release changes the driver,
  Windows may prompt to trust it.
- The installer needs admin; the updater self-elevates.
- `update-apollo.ps1` requires at least one published release to exist.
