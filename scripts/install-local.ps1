<#
.SYNOPSIS
  Install a locally built Apollo installer (from scripts\build-windows.ps1).

.DESCRIPTION
  Stops the ApolloService and any running sunshine/apollo process first, so the NSIS
  installer can overwrite C:\Program Files\Apollo\sunshine.exe (the usual "Error opening
  file for writing" failure), then runs a silent install and restarts the service.

  Self-elevates: the installer writes to Program Files and registers the service/driver.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\install-local.ps1

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\install-local.ps1 -Installer C:\path\to\Apollo.exe

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\install-local.ps1 -NoStart   # don't restart the service
#>
[CmdletBinding()]
param(
  [string]$Installer   = "",          # defaults to <repo>\cmake-build-release\cpack_artifacts\Apollo.exe
  [string]$ServiceName = "ApolloService",
  [switch]$NoStart                    # leave the service stopped after installing
)

$ErrorActionPreference = "Stop"

# Resolve the installer to an absolute path before any elevation: the elevated
# process starts in a different working directory, so a relative path would break.
$repoRoot = Split-Path $PSScriptRoot -Parent
if ([string]::IsNullOrWhiteSpace($Installer)) {
  $Installer = Join-Path $repoRoot "cmake-build-release\cpack_artifacts\Apollo.exe"
}
elseif (-not [System.IO.Path]::IsPathRooted($Installer)) {
  $Installer = Join-Path $repoRoot $Installer
}
if (-not (Test-Path $Installer)) {
  throw "Installer not found: $Installer. Build it first with scripts\build-windows.ps1."
}
$Installer = (Resolve-Path $Installer).Path

# --- self-elevate (installer needs admin for Program Files + the service/driver) ---
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltinRole]::Administrator)
if (-not $isAdmin) {
  Write-Host "Re-launching elevated..."
  $a = @("-NoProfile","-ExecutionPolicy","Bypass","-File","`"$PSCommandPath`"","-Installer","`"$Installer`"","-ServiceName","`"$ServiceName`"")
  if ($NoStart) { $a += "-NoStart" }
  Start-Process powershell.exe -Verb RunAs -ArgumentList $a
  return
}

# --- stop the service and any running instance, so sunshine.exe is not locked ---
$svc = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
if ($svc -and $svc.Status -ne 'Stopped') {
  Write-Host "Stopping $ServiceName ..."
  Stop-Service $ServiceName -Force
  (Get-Service $ServiceName).WaitForStatus('Stopped','00:00:30')
}
Get-Process sunshine, apollo -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 2   # let Windows release the file handle

# --- silent install (NSIS /S) ---
Write-Host "Installing $(Split-Path $Installer -Leaf) ..."
$p = Start-Process -FilePath $Installer -ArgumentList '/S' -Wait -PassThru
if ($p.ExitCode -ne 0) { throw "Installer exited with code $($p.ExitCode)." }

# --- restart the service (the installer may already have started it) ---
if (-not $NoStart) {
  $svc = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
  if ($svc -and $svc.Status -ne 'Running') {
    Write-Host "Starting $ServiceName ..."
    Start-Service $ServiceName
  }
}

Write-Host "Installed $(Split-Path $Installer -Leaf)."
