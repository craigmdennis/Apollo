<#
.SYNOPSIS
  Build the Apollo NSIS installer locally and publish it as a GitHub Release on your fork.

.DESCRIPTION
  Run on the dev machine that has the MSYS2 UCRT64 toolchain + official Node on PATH.
  Steps: build a clean Release with BUILD_VERSION baked in -> cpack -G NSIS ->
  tag the commit -> create a GitHub Release with the installer attached (named
  Apollo-v<Version>.exe so update-apollo.ps1 can fetch it).

  Prerequisites: MSYS2 at C:\msys64 with the toolchain, official node on PATH,
  GitHub CLI authenticated (gh auth login), clean git tree on the commit to release.

.EXAMPLE
  powershell -File scripts\publish-release.ps1 -Version 0.5.0
.EXAMPLE
  powershell -File scripts\publish-release.ps1 -Version 0.5.1 -Notes "Cookie + cover fixes"
#>
[CmdletBinding()]
param(
  [Parameter(Mandatory)] [string]$Version,                 # e.g. 0.5.0 (leading 'v' optional)
  [string]$Repo     = "craigmdennis/Apollo",
  [string]$BuildDir = "cmake-build-release",
  [string]$Msys2    = "C:\msys64\msys2_shell.cmd",
  [string]$Notes    = "",
  [switch]$AllowDirty
)

# Native commands (git / gh / the MSYS2 cmd wrapper) legitimately write progress to
# stderr; under EAP=Stop PowerShell would turn that into a terminating error. So run
# with Continue and gate explicitly on $LASTEXITCODE / artifact checks instead.
$ErrorActionPreference = "Continue"

$Version = $Version.TrimStart('v','V')
$tag = "v$Version"

# --- preflight ---
if (-not (Get-Command gh -ErrorAction SilentlyContinue)) { throw "gh not found; install GitHub CLI and run 'gh auth login'." }
if (-not (Test-Path $Msys2)) { throw "MSYS2 not found at $Msys2." }

$repoRoot = git rev-parse --show-toplevel
if ($LASTEXITCODE -ne 0 -or -not $repoRoot) { throw "Not inside a git repo." }
$repoRoot = $repoRoot.Trim()

if (-not $AllowDirty -and (git status --porcelain)) { throw "Working tree not clean. Commit/stash first, or pass -AllowDirty." }
if (git tag --list $tag) { throw "Tag $tag already exists. Bump -Version." }

$msysRoot = Split-Path $Msys2 -Parent
$buildScript = Join-Path $msysRoot "tmp\apollo_release_build.sh"

# --- build + package inside MSYS2 UCRT64 (cygpath converts the Windows repo path) ---
$bash = @"
#!/usr/bin/env bash
set -eo pipefail
export BUILD_VERSION='$Version'
export PATH="/c/Program Files/nodejs:/ucrt64/bin:`$PATH"
cd "`$(cygpath -u '$repoRoot')"
echo "=== configure (BUILD_VERSION=`$BUILD_VERSION) ==="
cmake -B '$BuildDir' -G Ninja -S . -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF
echo "=== build ==="
ninja -C '$BuildDir'
echo "=== package ==="
cpack -G NSIS --config '$BuildDir/CPackConfig.cmake'
"@
Set-Content -Path $buildScript -Value ($bash -replace "`r`n","`n") -Encoding ascii -NoNewline -ErrorAction Stop

$installer = Join-Path $repoRoot "$BuildDir\cpack_artifacts\Apollo.exe"
if (Test-Path $installer) { Remove-Item $installer -Force -ErrorAction Stop }  # so we can detect a fresh build

Write-Host "Building Release v$Version (first run compiles Boost from source; later runs are incremental)..."
$started = Get-Date
& $Msys2 -defterm -here -no-start -ucrt64 -c "bash /tmp/apollo_release_build.sh"

# The MSYS2 cmd wrapper does not reliably propagate the inner exit code, so verify by
# the artifact being (re)created after we started.
if (-not (Test-Path $installer) -or (Get-Item $installer).LastWriteTime -lt $started) {
  throw "Build/package did not produce a fresh installer at $installer (see output above)."
}

$asset = Join-Path $repoRoot "$BuildDir\cpack_artifacts\Apollo-$tag.exe"
Copy-Item $installer $asset -Force -ErrorAction Stop
Write-Host "Built installer: $asset ($([math]::Round((Get-Item $asset).Length/1MB,1)) MB)"

# --- tag + push + release ---
git tag $tag
if ($LASTEXITCODE -ne 0) { throw "git tag $tag failed." }
git push fork $tag
if ($LASTEXITCODE -ne 0) { throw "git push of $tag failed." }

if ([string]::IsNullOrWhiteSpace($Notes)) { $Notes = "Apollo $tag" }
gh release create $tag --repo $Repo --title "Apollo $tag" --notes $Notes "$asset"
if ($LASTEXITCODE -ne 0) { throw "gh release create failed (tag $tag was pushed; re-run just the gh release create, or delete the tag)." }

Write-Host "Published release $tag with installer attached."
Write-Host "On each machine run: scripts\update-apollo.ps1"
