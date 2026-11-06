<#
.SYNOPSIS
  Build Apollo on Windows in one command.

.DESCRIPTION
  Runs the full Windows build through the MSYS2 UCRT64 toolchain — configure with
  CMake, compile with Ninja (C++ core + web UI), and package the NSIS installer — so
  you don't have to hand-type the cmake/ninja/cpack commands.

  Prerequisites (one-time, see docs/building.md):
    - MSYS2 at C:\msys64 with the mingw-w64-ucrt-x86_64-toolchain and dependencies.
    - Official Node.js (nodejs.org or nvm-windows) on PATH, NOT MSYS2's nodejs package.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\build-windows.ps1

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\build-windows.ps1 -Version 0.5.0 -Tests

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\build-windows.ps1 -NoPackage   # build only, skip installer
#>
[CmdletBinding()]
param(
  [string]$Version   = "",                  # optional: bake a version into the build (e.g. 0.5.0)
  [string]$BuildDir  = "cmake-build-release",
  [string]$Msys2     = "C:\msys64\msys2_shell.cmd",
  [string]$NodeDir   = "",                  # directory containing node.exe; auto-detected when blank
  [string]$BuildType = "Release",
  [switch]$Tests,                           # build and run the GoogleTest suite
  [switch]$NoPackage                         # skip the NSIS installer step
)

# Native tools (cmake/ninja/cpack through the MSYS2 cmd wrapper) write progress to
# stderr; under EAP=Stop PowerShell would turn that into a terminating error. So run
# with Continue and gate explicitly on a success marker / artifact checks instead.
$ErrorActionPreference = "Continue"

if (-not (Test-Path $Msys2)) { throw "MSYS2 not found at $Msys2. Install it from https://www.msys2.org." }

$repoRoot = git rev-parse --show-toplevel
if ($LASTEXITCODE -ne 0 -or -not $repoRoot) { throw "Not inside a git repo." }
$repoRoot = $repoRoot.Trim()

# --- locate node.exe (the web-ui target runs `npm install` via CMake) ---
if ($NodeDir) {
  if (-not (Test-Path (Join-Path $NodeDir "node.exe"))) { throw "node.exe not found in $NodeDir." }
}
else {
  $nodeCmd = Get-Command node -ErrorAction SilentlyContinue
  if ($nodeCmd) {
    $NodeDir = Split-Path $nodeCmd.Source -Parent
  }
  else {
    $NodeDir = "C:\Program Files\nodejs"
    Write-Warning "node.exe is not on PATH; assuming '$NodeDir'. Use -NodeDir if it lives elsewhere."
  }
}
Write-Host "Node directory: $NodeDir"

$msysRoot    = Split-Path $Msys2 -Parent
$buildScript = Join-Path $msysRoot "tmp\apollo_build.sh"
$okMarker    = Join-Path $msysRoot "tmp\apollo_build_ok"

$testsOn  = if ($Tests) { "ON" } else { "OFF" }
$testStep = if ($Tests) { "`n`necho '=== test ==='`n./$BuildDir/tests/test_sunshine" } else { "" }
$pkgStep  = if ($NoPackage) { "" } else { "`n`necho '=== package (NSIS) ==='`ncpack -G NSIS --config $BuildDir/CPackConfig.cmake" }
$verExport = if ($Version) { "`nexport BUILD_VERSION='$Version'`nexport BRANCH='manual'" } else { "" }

# --- build + package inside MSYS2 UCRT64 (cygpath converts the Windows paths) ---
$bash = @"
#!/usr/bin/env bash
set -eo pipefail$verExport
export PATH="`$(cygpath -u '$NodeDir'):/ucrt64/bin:`$PATH"
cd "`$(cygpath -u '$repoRoot')"
echo "=== configure ($BuildType, tests=$testsOn) ==="
cmake -B $BuildDir -G Ninja -S . -DCMAKE_BUILD_TYPE=$BuildType -DBUILD_TESTS=$testsOn
echo "=== build ==="
ninja -C $BuildDir$testStep$pkgStep
touch /tmp/apollo_build_ok
"@
Set-Content -Path $buildScript -Value ($bash -replace "`r`n","`n") -Encoding ascii -NoNewline -ErrorAction Stop

if (Test-Path $okMarker) { Remove-Item $okMarker -Force -ErrorAction Stop }  # so we can detect a fresh run

Write-Host "Building Apollo ($BuildType)..."
& $Msys2 -defterm -here -no-start -ucrt64 -c "bash /tmp/apollo_build.sh"

# The MSYS2 cmd wrapper does not reliably propagate the inner exit code, so verify the
# run completed via a success marker the bash script touches at the very end.
if (-not (Test-Path $okMarker)) {
  throw "Build did not complete (see output above)."
}

if ($NoPackage) {
  Write-Host "Build finished (packaging skipped)."
}
else {
  $installer = Join-Path $repoRoot "$BuildDir\cpack_artifacts\Apollo.exe"
  if (-not (Test-Path $installer)) { throw "Packaging did not produce an installer at $installer (see output above)." }
  Write-Host "Installer: $installer ($([math]::Round((Get-Item $installer).Length/1MB,1)) MB)"
}
