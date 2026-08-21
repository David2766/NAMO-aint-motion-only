[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)]
  [string]$Owner,

  [Parameter(Mandatory = $true)]
  [string]$Peer,

  [string[]]$Device = @(),
  [double]$DurationSeconds = 0,
  [double]$PollSeconds = 1,
  [double]$SlowPollSeconds = 2,
  [double]$TimeoutSeconds = 3,
  [int]$Port = 0,
  [string]$PublicHost = "",
  [string]$OutputDir = "",
  [string]$PythonPath = (Join-Path $env:USERPROFILE ".venv\Scripts\python.exe")
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$tool = Join-Path $root "tools\pairing_trace_capture.py"

if (-not (Test-Path -LiteralPath $PythonPath -PathType Leaf)) {
  throw "Python interpreter not found: $PythonPath"
}
if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) {
  throw "Pairing trace tool not found: $tool"
}

$arguments = @(
  $tool,
  "--owner", $Owner,
  "--peer", $Peer,
  "--duration-seconds", $DurationSeconds,
  "--poll-seconds", $PollSeconds,
  "--slow-poll-seconds", $SlowPollSeconds,
  "--timeout-seconds", $TimeoutSeconds,
  "--port", $Port
)

foreach ($item in $Device) {
  $arguments += @("--device", $item)
}
if ($PublicHost) {
  $arguments += @("--public-host", $PublicHost)
}
if ($OutputDir) {
  $arguments += @("--output-dir", $OutputDir)
}

Push-Location $root
try {
  & $PythonPath @arguments
  if ($LASTEXITCODE -ne 0) {
    throw "pairing trace capture failed with exit code $LASTEXITCODE"
  }
}
finally {
  Pop-Location
}
