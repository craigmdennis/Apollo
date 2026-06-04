<#
.SYNOPSIS
  Update Apollo on this machine to the latest GitHub Release from your fork.

.DESCRIPTION
  Pulls the newest release's NSIS installer from the given repo, stops the Apollo
  service, installs silently, and restarts the service. Skips work if the latest
  release is already installed (tracked in %ProgramData%\ApolloUpdater\installed-tag.txt).

  Prerequisites on each machine (one-time):
    - GitHub CLI:  winget install GitHub.cli   then   gh auth login
    - Apollo already installed once (so the service + driver exist).

.EXAMPLE
  # Run on demand (will self-elevate):
  powershell -ExecutionPolicy Bypass -File update-apollo.ps1

.EXAMPLE
  # Force reinstall of the latest release even if already installed:
  powershell -ExecutionPolicy Bypass -File update-apollo.ps1 -Force
#>
[CmdletBinding()]
param(
  [string]$Repo = "craigmdennis/Apollo",
  [string]$AssetPattern = "Apollo*.exe",
  [string]$ServiceName = "ApolloService",
  [switch]$Force
)
$ErrorActionPreference = "Stop"

# --- self-elevate (installer needs admin for the service + SudoVDA driver) ---
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltinRole]::Administrator)
if (-not $isAdmin) {
  Write-Host "Re-launching elevated..."
  $a = @("-NoProfile","-ExecutionPolicy","Bypass","-File","`"$PSCommandPath`"","-Repo","`"$Repo`"")
  if ($Force) { $a += "-Force" }
  Start-Process powershell.exe -Verb RunAs -ArgumentList $a
  return
}

if (-not (Get-Command gh -ErrorAction SilentlyContinue)) {
  throw "GitHub CLI 'gh' not found. Install it (winget install GitHub.cli) and run 'gh auth login'."
}

# --- find the latest release tag ---
Write-Host "Checking latest release in $Repo ..."
$latest = gh release view --repo $Repo --json tagName | ConvertFrom-Json
$tag = $latest.tagName
if (-not $tag) { throw "No releases found in $Repo." }

$markerDir  = Join-Path $env:ProgramData "ApolloUpdater"
$markerFile = Join-Path $markerDir "installed-tag.txt"
$installed  = if (Test-Path $markerFile) { (Get-Content $markerFile -Raw).Trim() } else { "" }

if (($installed -eq $tag) -and -not $Force) {
  Write-Host "Already up to date (installed = $installed). Use -Force to reinstall."
  return
}
Write-Host "Updating: installed='$installed' -> latest='$tag'"

# --- download the installer asset ---
$work = Join-Path $env:TEMP "apollo-update-$tag"
if (Test-Path $work) { Remove-Item $work -Recurse -Force }
New-Item -ItemType Directory -Path $work | Out-Null
gh release download $tag --repo $Repo --pattern $AssetPattern --dir $work --clobber
$installer = Get-ChildItem $work -Filter *.exe | Select-Object -First 1
if (-not $installer) { throw "Release $tag has no asset matching '$AssetPattern'." }

# --- stop the service and any running instance ---
$svc = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
if ($svc -and $svc.Status -ne 'Stopped') {
  Write-Host "Stopping $ServiceName ..."
  Stop-Service $ServiceName -Force
  (Get-Service $ServiceName).WaitForStatus('Stopped','00:00:30')
}
Get-Process sunshine -ErrorAction SilentlyContinue | Stop-Process -Force

# --- silent install (NSIS /S) ---
Write-Host "Installing $($installer.Name) ..."
$p = Start-Process -FilePath $installer.FullName -ArgumentList '/S' -Wait -PassThru
if ($p.ExitCode -ne 0) { throw "Installer exited with code $($p.ExitCode)." }

# --- restart service (the installer may already start it) ---
$svc = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
if ($svc -and $svc.Status -ne 'Running') {
  Write-Host "Starting $ServiceName ..."
  Start-Service $ServiceName
}

# --- record what we installed ---
New-Item -ItemType Directory -Path $markerDir -Force | Out-Null
Set-Content -Path $markerFile -Value $tag -Encoding ascii
Write-Host "Apollo updated to $tag."
