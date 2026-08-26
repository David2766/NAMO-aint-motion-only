[CmdletBinding()]
param(
  [Parameter(Position = 0)]
  [ValidateSet("roles", "capture", "mark", "analyze", "session", "help")]
  [string]$Action = "roles",

  [Parameter(Position = 1)]
  [string]$Label = "",

  [string[]]$Device = @(),
  [double]$DurationHours = 2,
  [double]$DurationSeconds = 0,
  [double]$StateIntervalSeconds = 1,
  [double]$GroupIntervalSeconds = 5,
  [double]$SystemIntervalSeconds = 30,
  [int]$TimeoutSeconds = 3,
  [int]$RotateSeconds = 3600,
  [string]$Prefix = "failure-7node",
  [string]$OutputRoot = "",
  [string]$Session = "",
  [string]$PythonPath = ""
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$captureTool = Join-Path $repoRoot "tools\office_noise_capture.py"
$analyzeTool = Join-Path $repoRoot "tools\office_noise_analyze.py"

if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
  $OutputRoot = Join-Path $repoRoot "test-result\namo-failure"
}
$OutputRoot = [System.IO.Path]::GetFullPath($OutputRoot)
$currentSessionPath = Join-Path $OutputRoot "current-session.txt"

$defaultDevices = [ordered]@{
  "78"  = "http://192.168.29.78"
  "96"  = "http://192.168.29.96"
  "104" = "http://192.168.29.104"
  "173" = "http://192.168.29.173"
  "203" = "http://192.168.29.203"
  "208" = "http://192.168.29.208"
  "218" = "http://192.168.29.218"
}

function Show-Usage {
  Write-Host @"
NAMO physical failure test helper

Usage:
  .\scripts\namo-failure-test.ps1 roles
  .\scripts\namo-failure-test.ps1 capture [-DurationHours 2]
  .\scripts\namo-failure-test.ps1 mark power_off_owner_218
  .\scripts\namo-failure-test.ps1 analyze
  .\scripts\namo-failure-test.ps1 session

The capture action creates a timestamped directory under:
  $OutputRoot

The mark and analyze actions automatically use the latest session started by
this helper. Use -Session to select a different directory explicitly.
"@
}

function Convert-ToInvariantString {
  param([Parameter(Mandatory = $true)]$Value)
  return [System.Convert]::ToString(
    $Value,
    [System.Globalization.CultureInfo]::InvariantCulture
  )
}

function Get-DeviceMap {
  $result = [ordered]@{}
  if ($Device.Count -eq 0) {
    foreach ($entry in $defaultDevices.GetEnumerator()) {
      $result[$entry.Key] = $entry.Value
    }
    return $result
  }

  $nextAlias = 1
  foreach ($item in $Device) {
    if ($item -match "^([^=]+)=(https?://.+)$") {
      $result[$Matches[1]] = $Matches[2].TrimEnd("/")
      continue
    }
    if ($item -match "^https?://") {
      $result["device$nextAlias"] = $item.TrimEnd("/")
      $nextAlias++
      continue
    }
    throw "Invalid -Device value '$item'. Use alias=http://address or http://address."
  }
  return $result
}

function Resolve-PythonPath {
  if (-not [string]::IsNullOrWhiteSpace($PythonPath)) {
    if (-not (Test-Path -LiteralPath $PythonPath -PathType Leaf)) {
      throw "Python interpreter not found: $PythonPath"
    }
    return [System.IO.Path]::GetFullPath($PythonPath)
  }

  $candidates = @()
  if (-not [string]::IsNullOrWhiteSpace($env:ESPHOME_PYTHON)) {
    $candidates += $env:ESPHOME_PYTHON
  }
  if (-not [string]::IsNullOrWhiteSpace($env:VIRTUAL_ENV)) {
    $candidates += Join-Path $env:VIRTUAL_ENV "Scripts\python.exe"
  }
  $candidates += Join-Path $env:USERPROFILE ".venv\Scripts\python.exe"
  $candidates += Join-Path $repoRoot ".venv\Scripts\python.exe"

  foreach ($candidate in $candidates) {
    if (Test-Path -LiteralPath $candidate -PathType Leaf) {
      return [System.IO.Path]::GetFullPath($candidate)
    }
  }

  $command = Get-Command python.exe -ErrorAction SilentlyContinue
  if ($null -ne $command) {
    return $command.Source
  }
  throw "Python interpreter not found. Use -PythonPath to provide one."
}

function Resolve-SessionPath {
  if (-not [string]::IsNullOrWhiteSpace($Session)) {
    $resolved = [System.IO.Path]::GetFullPath($Session)
  }
  else {
    if (-not (Test-Path -LiteralPath $currentSessionPath -PathType Leaf)) {
      throw "No active capture session. Run the capture action first or use -Session."
    }
    $resolved = (Get-Content -LiteralPath $currentSessionPath -Raw).Trim()
  }

  if (-not (Test-Path -LiteralPath $resolved -PathType Container)) {
    throw "Capture session directory not found: $resolved"
  }
  return $resolved
}

function Get-JsonOrNull {
  param(
    [Parameter(Mandatory = $true)][string]$Uri,
    [Parameter(Mandatory = $true)][int]$RequestTimeoutSeconds
  )
  try {
    return Invoke-RestMethod `
      -Uri $Uri `
      -Method Get `
      -TimeoutSec $RequestTimeoutSeconds `
      -ErrorAction Stop
  }
  catch {
    return $null
  }
}

function Show-NamoRoles {
  $devices = Get-DeviceMap
  $rows = foreach ($device in $devices.GetEnumerator()) {
    $groups = Get-JsonOrNull `
      -Uri "$($device.Value)/api/platform/groups" `
      -RequestTimeoutSeconds $TimeoutSeconds
    $site = Get-JsonOrNull `
      -Uri "$($device.Value)/api/platform/site-configuration" `
      -RequestTimeoutSeconds $TimeoutSeconds
    $state = Get-JsonOrNull `
      -Uri "$($device.Value)/api/state" `
      -RequestTimeoutSeconds $TimeoutSeconds

    if ($null -eq $groups -and $null -eq $site -and $null -eq $state) {
      [PSCustomObject]@{
        IP = $device.Key
        Owner = "-"
        SiteLeader = "-"
        GroupLeader = "-"
        Members = "-"
        Health = "offline"
        Consistent = "-"
        Fusion = "-"
        Authority = "-"
      }
      continue
    }

    $group = $null
    if ($null -ne $groups) {
      $group = @(
        $groups.groups | Where-Object { $_.configured -eq $true }
      ) | Select-Object -First 1
    }

    $members = "-"
    $health = "-"
    if ($null -ne $group) {
      $members = "$($group.participantsOnline)/$($group.participantsExpected)"
      $health = [string]$group.health
    }
    elseif ($null -ne $groups.site) {
      $members = "$($groups.site.participantsOnline)/$($groups.site.participantsExpected)"
      $health = [string]$groups.site.health
    }

    [PSCustomObject]@{
      IP = $device.Key
      Owner = if ($null -ne $site) { [bool]$site.localOwner } else { "-" }
      SiteLeader = if ($null -ne $groups.site) {
        [bool]$groups.site.localLeader
      } else { "-" }
      GroupLeader = if ($null -ne $group) {
        [bool]$group.localLeader
      } else { "-" }
      Members = $members
      Health = $health
      Consistent = if ($null -ne $site) {
        [bool]$site.configurationConsistent
      } else { "-" }
      Fusion = if ($null -ne $state.fusion) {
        [string]$state.fusion.state
      } else { "-" }
      Authority = if ($null -ne $state.fusion) {
        [bool]$state.fusion.authoritative
      } else { "-" }
    }
  }

  $rows | Format-Table -AutoSize | Out-Host
}

function Start-NamoCapture {
  if (-not (Test-Path -LiteralPath $captureTool -PathType Leaf)) {
    throw "Capture tool not found: $captureTool"
  }
  $python = Resolve-PythonPath
  New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
  $sessionName = "{0}-{1}" -f $Prefix, (Get-Date -Format "yyyyMMdd-HHmmss")
  $sessionPath = Join-Path $OutputRoot $sessionName
  New-Item -ItemType Directory -Force -Path $sessionPath | Out-Null
  Set-Content -LiteralPath $currentSessionPath -Value $sessionPath -Encoding UTF8

  $arguments = @($captureTool)
  foreach ($device in (Get-DeviceMap).GetEnumerator()) {
    $arguments += @("--device", "$($device.Key)=$($device.Value)")
  }
  if ($DurationSeconds -gt 0) {
    $arguments += @(
      "--duration-seconds",
      (Convert-ToInvariantString $DurationSeconds)
    )
  }
  else {
    $arguments += @(
      "--duration-hours",
      (Convert-ToInvariantString $DurationHours)
    )
  }
  $arguments += @(
    "--state-interval-seconds",
    (Convert-ToInvariantString $StateIntervalSeconds),
    "--group-interval-seconds",
    (Convert-ToInvariantString $GroupIntervalSeconds),
    "--system-interval-seconds",
    (Convert-ToInvariantString $SystemIntervalSeconds),
    "--timeout-seconds",
    (Convert-ToInvariantString $TimeoutSeconds),
    "--rotate-seconds",
    (Convert-ToInvariantString $RotateSeconds),
    "--output-dir", $sessionPath,
    "--prefix", $Prefix
  )

  Write-Host "namo-failure-test: session=$sessionPath"
  Write-Host "namo-failure-test: use another terminal to add markers:"
  Write-Host "  .\scripts\namo-failure-test.ps1 mark power_off_owner_IP"

  Push-Location $repoRoot
  try {
    & $python @arguments
    if ($LASTEXITCODE -ne 0) {
      throw "office noise capture failed with exit code $LASTEXITCODE"
    }
  }
  finally {
    Pop-Location
  }
}

function Add-NamoMarker {
  if ([string]::IsNullOrWhiteSpace($Label)) {
    throw "The mark action requires a label. Example: mark power_off_owner_218"
  }
  $sessionPath = Resolve-SessionPath
  $markerPath = Join-Path $sessionPath "markers.txt"
  $line = "{0:o} {1}" -f [DateTimeOffset]::Now, $Label.Trim()
  Add-Content -LiteralPath $markerPath -Value $line -Encoding UTF8
  Write-Host $line
  Write-Host "namo-failure-test: marker=$markerPath"
}

function Invoke-NamoAnalysis {
  if (-not (Test-Path -LiteralPath $analyzeTool -PathType Leaf)) {
    throw "Analysis tool not found: $analyzeTool"
  }
  $python = Resolve-PythonPath
  $sessionPath = Resolve-SessionPath
  $reportPath = Join-Path $sessionPath "analysis.json"

  Push-Location $repoRoot
  try {
    & $python $analyzeTool $sessionPath "--output" $reportPath
    if ($LASTEXITCODE -ne 0) {
      throw "office noise analysis failed with exit code $LASTEXITCODE"
    }
  }
  finally {
    Pop-Location
  }
  Write-Host "namo-failure-test: analysis=$reportPath"
}

switch ($Action) {
  "roles" { Show-NamoRoles }
  "capture" { Start-NamoCapture }
  "mark" { Add-NamoMarker }
  "analyze" { Invoke-NamoAnalysis }
  "session" { Write-Host (Resolve-SessionPath) }
  "help" { Show-Usage }
}
