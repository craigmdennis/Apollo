<#
.SYNOPSIS
  Update Apollo in place from a GitHub Release.

.DESCRIPTION
  Started by the "Check for Updates" item in the Apollo tray menu. Apollo launches this
  script as a detached process, because installing an update stops the Apollo service and
  that terminates Apollo itself.

  Pairings, configuration, and cover art are kept. The installer only replaces the program
  files in the install directory.

.PARAMETER Repo
  GitHub repository to read releases from.

.PARAMETER AssetPattern
  Wildcard matching the installer asset attached to the release.

.PARAMETER ServiceName
  Name of the Apollo service that has to stop while files are replaced.

.PARAMETER CurrentVersion
  Version of the running Apollo build, used to decide whether the release is newer.

.PARAMETER Force
  Install the latest release even if the running version looks newer or the same.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File update-apollo.ps1 -CurrentVersion 0.6.0

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File update-apollo.ps1 -CurrentVersion 0.6.0 -Force
#>
[CmdletBinding()]
param(
  [string]$Repo = 'craigmdennis/Apollo',
  [string]$AssetPattern = 'Apollo-*.exe',
  [string]$ServiceName = 'ApolloService',
  [string]$CurrentVersion = '',
  [switch]$Force
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$Title = 'Apollo Update'
$WorkDir = Join-Path $env:ProgramData 'Apollo\updater'
$LogFile = Join-Path $WorkDir 'updater.log'
$MarkerFile = Join-Path $WorkDir 'installed-tag.txt'

function Write-ApolloLog {
  param([string]$Message)

  $line = '{0} {1}' -f (Get-Date).ToString('s'), $Message
  try {
    Add-Content -Path $LogFile -Value $line -ErrorAction SilentlyContinue
  } catch {
    # logging must never take the update down
  }
  Write-Host $line
}

# Shows a message in the console session. Yes/No returns 6 when the user agrees.
function Show-ApolloMessage {
  param(
    [Parameter(Mandatory = $true)][string]$Text,
    [ValidateSet('Info', 'Warning', 'Error')][string]$Kind = 'Info',
    [switch]$YesNo,
    [int]$TimeoutSeconds = 0
  )

  $icons = @{ Info = 64; Warning = 48; Error = 16 }
  $type = $icons[$Kind]
  if ($YesNo) {
    $type += 4
  }

  try {
    $shell = New-Object -ComObject WScript.Shell
    return [int]$shell.Popup($Text, $TimeoutSeconds, $Title, $type)
  } catch {
    Write-Host "[$Title] $Text"
    return 1
  }
}

function Format-ApolloVersion {
  param([string]$Version)

  if ([string]::IsNullOrWhiteSpace($Version)) {
    return 'unknown'
  }
  return 'v' + $Version.Trim().TrimStart('v', 'V')
}

# Reduces a version or release tag to the three numbers that order releases.
function ConvertTo-ApolloVersionKey {
  param([string]$Version)

  $key = @(0, 0, 0)
  if ([string]::IsNullOrWhiteSpace($Version)) {
    return $key
  }

  $parts = $Version.Trim().TrimStart('v', 'V') -split '[.\-+]'
  for ($i = 0; $i -lt 3 -and $i -lt $parts.Count; $i++) {
    if ($parts[$i] -notmatch '^\d+$') {
      break
    }
    $key[$i] = [int]$parts[$i]
  }
  return $key
}

# Returns 1 when Left is newer than Right, -1 when older, and 0 when the same.
function Compare-ApolloVersion {
  param([string]$Left, [string]$Right)

  $leftKey = ConvertTo-ApolloVersionKey $Left
  $rightKey = ConvertTo-ApolloVersionKey $Right

  for ($i = 0; $i -lt 3; $i++) {
    if ($leftKey[$i] -gt $rightKey[$i]) {
      return 1
    }
    if ($leftKey[$i] -lt $rightKey[$i]) {
      return -1
    }
  }
  return 0
}

# Builds made outside a release script carry a 0.0.0 placeholder version, so they are not
# a reliable record of what is installed.
function Test-ApolloReleaseVersion {
  param([string]$Version)

  $key = ConvertTo-ApolloVersionKey $Version
  return ($key[0] -ne 0 -or $key[1] -ne 0 -or $key[2] -ne 0)
}

# Prefer the version baked into the running build, and fall back to the tag recorded at the
# last update for builds that predate versioned releases.
function Get-ApolloInstalledVersion {
  param([string]$BuildVersion, [string]$RecordedTag)

  if (Test-ApolloReleaseVersion $BuildVersion) {
    return $BuildVersion
  }
  if (-not [string]::IsNullOrWhiteSpace($RecordedTag)) {
    return $RecordedTag
  }
  return $BuildVersion
}

# The installer needs administrator rights for program files and the service, so elevate
# before touching anything on disk.
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltinRole]::Administrator)
if (-not $isAdmin) {
  $arguments = @(
    '-NoProfile', '-ExecutionPolicy', 'Bypass', '-WindowStyle', 'Hidden',
    '-File', "`"$PSCommandPath`"",
    '-Repo', "`"$Repo`"",
    '-CurrentVersion', "`"$CurrentVersion`""
  )
  if ($Force) {
    $arguments += '-Force'
  }

  try {
    Start-Process -FilePath 'powershell.exe' -Verb RunAs -ArgumentList $arguments
  } catch {
    Show-ApolloMessage -Kind Error -Text "Apollo needs administrator rights to install an update.`r`n`r`nNothing was changed." | Out-Null
  }
  exit 0
}

New-Item -ItemType Directory -Path $WorkDir -Force | Out-Null

# Keep the log from growing without bound across updates.
if ((Test-Path $LogFile) -and ((Get-Item $LogFile).Length -gt 1MB)) {
  Remove-Item -Path $LogFile -Force -ErrorAction SilentlyContinue
}

$recordedTag = ''
if (Test-Path $MarkerFile) {
  $recordedTag = (Get-Content -Path $MarkerFile -Raw).Trim()
}

$installedVersion = Get-ApolloInstalledVersion -BuildVersion $CurrentVersion -RecordedTag $recordedTag
Write-ApolloLog "Update check started (installed: $(Format-ApolloVersion $installedVersion), repo: $Repo)."

# Only one update at a time, so repeated clicks cannot race each other.
$mutex = New-Object System.Threading.Mutex($false, 'ApolloUpdaterMutex')
if (-not $mutex.WaitOne(0)) {
  Write-ApolloLog 'Another update is already running.'
  Show-ApolloMessage -Kind Warning -TimeoutSeconds 6 -Text 'An Apollo update is already running.' | Out-Null
  exit 0
}

try {
  $release = Invoke-RestMethod -UseBasicParsing -Headers @{ 'User-Agent' = 'Apollo-Updater'; 'Accept' = 'application/vnd.github+json' } -Uri "https://api.github.com/repos/$Repo/releases/latest"
} catch {
  Write-ApolloLog "Could not read the latest release: $($_.Exception.Message)"
  Show-ApolloMessage -Kind Error -Text "Could not check for Apollo updates.`r`n`r`n$($_.Exception.Message)" | Out-Null
  exit 1
}

$tag = $release.tag_name
if ([string]::IsNullOrWhiteSpace($tag)) {
  Write-ApolloLog 'The latest release has no tag.'
  Show-ApolloMessage -Kind Error -Text 'Could not check for Apollo updates: the latest release has no version tag.' | Out-Null
  exit 1
}
Write-ApolloLog "Latest release is $tag."

if (-not $Force -and (Compare-ApolloVersion $tag $installedVersion) -le 0) {
  Write-ApolloLog 'Already up to date.'
  Show-ApolloMessage -TimeoutSeconds 8 -Text "Apollo is up to date ($(Format-ApolloVersion $installedVersion))." | Out-Null
  exit 0
}

$asset = $release.assets | Where-Object { $_.name -like $AssetPattern } | Select-Object -First 1
if (-not $asset) {
  Write-ApolloLog "Release $tag has no asset matching '$AssetPattern'."
  Show-ApolloMessage -Kind Error -Text "Release $tag has no installer attached to it." | Out-Null
  exit 1
}

$installer = Join-Path $WorkDir $asset.name
$expectedHash = $null
if ($asset.digest -and $asset.digest -match '^sha256:(.+)$') {
  $expectedHash = $Matches[1].ToUpperInvariant()
}

# A previously downloaded installer is reused while it still matches the release, so an
# install that was declined does not cost another download.
$installerReady = $false
if ((Test-Path $installer) -and $expectedHash) {
  $existingHash = (Get-FileHash -Path $installer -Algorithm SHA256).Hash
  if ($existingHash -eq $expectedHash) {
    $installerReady = $true
    Write-ApolloLog 'Reusing the verified installer from an earlier attempt.'
  } else {
    Write-ApolloLog 'The downloaded installer no longer matches the release, so it will be fetched again.'
  }
}

if (-not $installerReady) {
  # Downloading changes nothing on this machine, so it is a separate decision from installing.
  $answer = Show-ApolloMessage -YesNo -Text ("Apollo {0} is available.`r`n`r`nInstalled: {1}`r`n`r`nDownload it now? Nothing is installed until you confirm the next step." -f $tag, (Format-ApolloVersion $installedVersion))
  if ($answer -ne 6) {
    Write-ApolloLog 'The download was declined.'
    exit 0
  }

  try {
    Write-ApolloLog "Downloading $($asset.name) ($([math]::Round($asset.size / 1MB, 1)) MB)."
    if (Test-Path $installer) {
      Remove-Item $installer -Force
    }
    Invoke-WebRequest -UseBasicParsing -Uri $asset.browser_download_url -OutFile $installer
  } catch {
    Write-ApolloLog "Download failed: $($_.Exception.Message)"
    Show-ApolloMessage -Kind Error -Text "The Apollo installer could not be downloaded. Nothing was changed.`r`n`r`n$($_.Exception.Message)" | Out-Null
    exit 1
  }

  # Verify against the checksum GitHub publishes for the asset, when there is one.
  if ($expectedHash) {
    $actualHash = (Get-FileHash -Path $installer -Algorithm SHA256).Hash
    if ($actualHash -ne $expectedHash) {
      Write-ApolloLog "Checksum mismatch: expected $expectedHash but got $actualHash."
      Remove-Item $installer -Force -ErrorAction SilentlyContinue
      Show-ApolloMessage -Kind Error -Text 'The downloaded installer failed its checksum check. Nothing was changed.' | Out-Null
      exit 1
    }
    Write-ApolloLog 'Checksum verified.'
  } else {
    Write-ApolloLog 'The release publishes no checksum for this asset; skipping verification.'
  }
}

# Installing is the only step that changes the machine, so it is confirmed on its own, as
# late as possible. Past this point Apollo is down and the update cannot be called off.
$answer = Show-ApolloMessage -Kind Warning -YesNo -Text ("Install Apollo {0} now?`r`n`r`nApollo will stop, install the update, and start again. Any active stream or microphone session ends. Pairings and settings are kept.`r`n`r`nChoose No to install it later from the same menu item." -f $tag)
if ($answer -ne 6) {
  Write-ApolloLog 'The install was declined; the verified installer stays in the update directory.'
  exit 0
}

# Stopping the service terminates Apollo, so everything has to be ready by now.
$service = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
if ($service -and $service.Status -ne 'Stopped') {
  Write-ApolloLog "Stopping $ServiceName."
  try {
    Stop-Service -Name $ServiceName -Force
    (Get-Service -Name $ServiceName).WaitForStatus('Stopped', [TimeSpan]::FromSeconds(30))
  } catch {
    Write-ApolloLog "The service did not stop: $($_.Exception.Message)"
    Show-ApolloMessage -Kind Error -Text "The Apollo service could not be stopped, so the update was not installed.`r`n`r`n$($_.Exception.Message)" | Out-Null
    exit 1
  }
}

Write-ApolloLog "Installing $($asset.name)."

# Apollo runs in the service's job object, so stopping the service should take it down.
# Wait for it to disappear before touching its files, and force it if it lingers.
$deadline = (Get-Date).AddSeconds(10)
while ((Get-Date) -lt $deadline -and (Get-Process -Name 'sunshine' -ErrorAction SilentlyContinue)) {
  Start-Sleep -Milliseconds 200
}
if (Get-Process -Name 'sunshine' -ErrorAction SilentlyContinue) {
  Write-ApolloLog 'Apollo is still running, stopping it.'
  Get-Process -Name 'sunshine' -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
  Start-Sleep -Seconds 2
}

$process = Start-Process -FilePath $installer -ArgumentList '/S' -Wait -PassThru
if ($process.ExitCode -ne 0) {
  Write-ApolloLog "The installer exited with code $($process.ExitCode)."

  # Leave Apollo running rather than leaving the machine without a host.
  $service = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
  if ($service -and $service.Status -ne 'Running') {
    Start-Service -Name $ServiceName -ErrorAction SilentlyContinue
  }

  Show-ApolloMessage -Kind Error -Text "The Apollo installer failed (exit code $($process.ExitCode)). Apollo was started again on the previous version." | Out-Null
  exit 1
}

$service = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
if ($service -and $service.Status -ne 'Running') {
  Write-ApolloLog "Starting $ServiceName."
  try {
    Start-Service -Name $ServiceName
  } catch {
    Write-ApolloLog "The service did not start: $($_.Exception.Message)"
  }
}

Remove-Item $installer -Force -ErrorAction SilentlyContinue
Set-Content -Path $MarkerFile -Value $tag -Encoding ascii -ErrorAction SilentlyContinue
Write-ApolloLog "Apollo updated to $tag."
Show-ApolloMessage -TimeoutSeconds 10 -Text "Apollo was updated to $tag." | Out-Null

exit 0
