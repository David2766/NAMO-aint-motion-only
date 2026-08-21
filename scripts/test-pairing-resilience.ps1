[CmdletBinding()]
param(
  [string]$PythonPath = (Join-Path $env:USERPROFILE ".venv\Scripts\python.exe"),
  [switch]$FrontendOnly,
  [switch]$NativeOnly,
  [switch]$SkipTypecheck
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$dashboard = Join-Path $root "dashboard"

if ($FrontendOnly -and $NativeOnly) {
  throw "FrontendOnly and NativeOnly cannot be used together"
}

function Invoke-Checked {
  param(
    [Parameter(Mandatory = $true)]
    [scriptblock]$Command,
    [Parameter(Mandatory = $true)]
    [string]$Description
  )

  & $Command
  if ($LASTEXITCODE -ne 0) {
    throw "$Description failed with exit code $LASTEXITCODE"
  }
}

if (-not $NativeOnly) {
  Push-Location $dashboard
  try {
    if (-not $SkipTypecheck) {
      Invoke-Checked -Description "dashboard typecheck" -Command {
        & npm.cmd run typecheck
      }
    }
    Invoke-Checked -Description "pairing frontend resilience tests" -Command {
      & npx.cmd vitest run `
        src/web/api/platform-pairing-resilience.test.ts `
        src/web/api/platform-pairing.test.ts `
        src/web/api/platform-pairing-flow.test.ts `
        src/web/platform-calibration.test.ts `
        src/web/state/shared-floorplan-write.test.ts
    }
  }
  finally {
    Pop-Location
  }
}

if (-not $FrontendOnly) {
  if (-not (Test-Path -LiteralPath $PythonPath -PathType Leaf)) {
    throw "Python interpreter not found: $PythonPath"
  }
  Invoke-Checked -Description "native pairing and authority tests" -Command {
    & $PythonPath (Join-Path $root "tools\presence-replay\native\test.py")
  }
}

Write-Host "pairing resilience suite: ok"
