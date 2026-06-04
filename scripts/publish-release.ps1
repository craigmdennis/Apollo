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
  pwsh -File scripts/publish-release.ps1 -Version 0.5.0
.EXAMPLE
  pwsh -File scripts/publish-release.ps1 -Version 0.5.1 -Notes "Cookie + cover fixes"
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
$ErrorActionPreference = "Stop"
$Version = $Version.TrimStart('v','V')
$tag = "v$Version"

# --- preflight ---
if (-not (Get-Command gh -ErrorAction SilentlyContinue)) { throw "gh not found; install GitHub CLI and 'gh auth login'." }
if (-not (Test-Path $Msys2)) { throw "MSYS2 not found at $Msys2." }
$repoRoot = (git rev-parse --show-toplevel).Trim()
if (-not $repoRoot) { throw "Not inside a git repo." }
if (-not $AllowDirty -and (git status --porcelain)) { throw "Working tree not clean. Commit/stash first, or pass -AllowDirty." }
git rev-parse $tag *> $null
if ($LASTEXITCODE -eq 0) { throw "Tag $tag already exists. Bump -Version." }

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
Set-Content -Path $buildScript -Value ($bash -replace "`r`n","`n") -Encoding ascii -NoNewline
Write-Host "Building Release v$Version (this can take a while on the first run)..."
& $Msys2 -defterm -here -no-start -ucrt64 -c "bash /tmp/apollo_release_build.sh"
if ($LASTEXITCODE -ne 0) { throw "Build/package failed (exit $LASTEXITCODE). See output above." }

$installer = Join-Path $repoRoot "$BuildDir\cpack_artifacts\Apollo.exe"
if (-not (Test-Path $installer)) { throw "Installer not found at $installer." }
$asset = Join-Path $repoRoot "$BuildDir\cpack_artifacts\Apollo-$tag.exe"
Copy-Item $installer $asset -Force
Write-Host "Built installer: $asset ($([math]::Round((Get-Item $asset).Length/1MB,1)) MB)"

# --- tag + push + release ---
git tag $tag
git push fork $tag
if ([string]::IsNullOrWhiteSpace($Notes)) { $Notes = "Apollo $tag" }
gh release create $tag --repo $Repo --title "Apollo $tag" --notes $Notes "$asset"
Write-Host "Published release $tag with installer attached."
Write-Host "On each machine run: scripts\update-apollo.ps1"
