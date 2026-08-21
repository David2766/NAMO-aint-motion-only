[CmdletBinding()]
param(
  [ValidateSet("none", "developer", "release")]
  [string]$Signer = "none",

  [switch]$Release,

  [ValidateSet("Patch", "Minor", "Major")]
  [string]$ReleaseType = "Patch",

  [string]$KeyDirectory = $env:NAMO_FIRMWARE_KEY_DIRECTORY,

  [ValidateRange(1, 4294967295)]
  [uint32]$TrustRevision = 1,

  [string]$Firmware = "namo.yaml",

  [switch]$NoDashboard,

  [switch]$NoClean,

  [switch]$ValidateOnly,

  [switch]$DryRun
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if ([Environment]::OSVersion.Platform -eq [PlatformID]::Win32NT) {
  Add-Type -AssemblyName System.Security
}

if ($Release -and $Signer -ne "release") {
  throw "-Release requires the separate -Signer release option"
}
if (-not $Release -and $Signer -eq "release") {
  throw "-Signer release requires -Release"
}
if (-not $Release -and $PSBoundParameters.ContainsKey("ReleaseType")) {
  throw "-ReleaseType requires -Release"
}
if ($ValidateOnly -and $Signer -eq "none") {
  throw "-ValidateOnly requires developer or release signing"
}
if ($ValidateOnly -and $DryRun) {
  throw "-ValidateOnly cannot be combined with -DryRun"
}
if ($Signer -ne "none" -and [string]::IsNullOrWhiteSpace($KeyDirectory)) {
  throw "Signed builds require -KeyDirectory or NAMO_FIRMWARE_KEY_DIRECTORY"
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$compileScript = Join-Path $PSScriptRoot "compile-firmware.mjs"
$managedEnvironment = @(
  "NAMO_ROOT_PUBLIC_KEY",
  "NAMO_RELEASE_PUBLIC_KEY",
  "NAMO_RELEASE_PRIVATE_KEY",
  "NAMO_DEVELOPER_PUBLIC_KEY",
  "NAMO_DEVELOPER_PRIVATE_KEY",
  "NAMO_RELEASE_KEY_PASSPHRASE",
  "NAMO_DEVELOPER_KEY_PASSPHRASE",
  "NAMO_FIRMWARE_KEY_VALIDATION_PASSPHRASE",
  "NAMO_TRUST_REVISION"
)
$previousEnvironment = @{}

foreach ($name in $managedEnvironment) {
  $previousEnvironment[$name] = [Environment]::GetEnvironmentVariable(
    $name,
    [EnvironmentVariableTarget]::Process
  )
  [Environment]::SetEnvironmentVariable(
    $name,
    $null,
    [EnvironmentVariableTarget]::Process
  )
}

function Resolve-KeyFile {
  param([string]$Name)

  $path = Join-Path $KeyDirectory $Name
  if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
    throw "Signing key file not found: $path"
  }
  return (Resolve-Path -LiteralPath $path).Path
}

function Get-CachedPassphrase {
  param(
    [string]$Role
  )

  $secretPath = Join-Path $KeyDirectory ".secrets\$Role.dpapi"
  if (-not (Test-Path -LiteralPath $secretPath -PathType Leaf)) {
    throw "Cached $Role passphrase not found; run .\scripts\register-firmware-secret.ps1 -Role $Role"
  }

  $protectedBytes = $null
  $plainBytes = $null
  try {
    $lines = [IO.File]::ReadAllLines($secretPath)
    if ($lines.Length -ne 2 -or $lines[0] -ne "NAMO-DPAPI-V1") {
      throw "invalid cache format"
    }
    $protectedBytes = [Convert]::FromBase64String($lines[1])
    $entropy = [Text.Encoding]::UTF8.GetBytes(
      "NAMO firmware signing secret cache v1"
    )
    $plainBytes = [System.Security.Cryptography.ProtectedData]::Unprotect(
      $protectedBytes,
      $entropy,
      [System.Security.Cryptography.DataProtectionScope]::CurrentUser
    )
    $plain = [Text.Encoding]::UTF8.GetString($plainBytes)
    if ($plain.Length -lt 16) {
      throw "invalid cached passphrase"
    }
    return $plain
  }
  catch {
    throw "Cached $Role passphrase cannot be opened; register it again on this Windows account"
  }
  finally {
    if ($null -ne $plainBytes) {
      [Array]::Clear($plainBytes, 0, $plainBytes.Length)
    }
  }
}

function Set-ValidatedCachedPassphrase {
  param(
    [string]$Role,
    [string]$BuildEnvironmentName
  )

  $passphrase = Get-CachedPassphrase $Role
  [Environment]::SetEnvironmentVariable(
    "NAMO_FIRMWARE_KEY_VALIDATION_PASSPHRASE",
    $passphrase,
    [EnvironmentVariableTarget]::Process
  )
  try {
    & node (Join-Path $PSScriptRoot "firmware-key-validation.mjs") `
      --role $Role `
      --key-directory $KeyDirectory
    if ($LASTEXITCODE -ne 0) {
      throw "Cached $Role passphrase is invalid; register it again with -Replace"
    }
  }
  finally {
    [Environment]::SetEnvironmentVariable(
      "NAMO_FIRMWARE_KEY_VALIDATION_PASSPHRASE",
      $null,
      [EnvironmentVariableTarget]::Process
    )
  }

  [Environment]::SetEnvironmentVariable(
    $BuildEnvironmentName,
    $passphrase,
    [EnvironmentVariableTarget]::Process
  )
}

try {
  $nodeArguments = @($compileScript)
  if ($Release) {
    $nodeArguments += @(
      "--release",
      "--release-type",
      $ReleaseType.ToLowerInvariant()
    )
  }
  $nodeArguments += @("--signer", $Signer)
  if (-not $NoDashboard) {
    $nodeArguments += "--dashboard"
  }
  if (-not $NoClean) {
    $nodeArguments += "--clean"
  }
  $nodeArguments += $Firmware

  if ($Signer -eq "developer") {
    [Environment]::SetEnvironmentVariable(
      "NAMO_ROOT_PUBLIC_KEY",
      (Resolve-KeyFile "root.public.pem"),
      [EnvironmentVariableTarget]::Process
    )
    [Environment]::SetEnvironmentVariable(
      "NAMO_RELEASE_PUBLIC_KEY",
      (Resolve-KeyFile "release.public.pem"),
      [EnvironmentVariableTarget]::Process
    )
    [Environment]::SetEnvironmentVariable(
      "NAMO_DEVELOPER_PRIVATE_KEY",
      (Resolve-KeyFile "developer.private.pem"),
      [EnvironmentVariableTarget]::Process
    )
    [Environment]::SetEnvironmentVariable(
      "NAMO_TRUST_REVISION",
      [string]$TrustRevision,
      [EnvironmentVariableTarget]::Process
    )
    if (-not $DryRun) {
      Set-ValidatedCachedPassphrase `
        -Role "developer" `
        -BuildEnvironmentName "NAMO_DEVELOPER_KEY_PASSPHRASE"
    }
  }
  elseif ($Signer -eq "release") {
    [Environment]::SetEnvironmentVariable(
      "NAMO_ROOT_PUBLIC_KEY",
      (Resolve-KeyFile "root.public.pem"),
      [EnvironmentVariableTarget]::Process
    )
    [Environment]::SetEnvironmentVariable(
      "NAMO_RELEASE_PRIVATE_KEY",
      (Resolve-KeyFile "release.private.pem"),
      [EnvironmentVariableTarget]::Process
    )
    [Environment]::SetEnvironmentVariable(
      "NAMO_DEVELOPER_PUBLIC_KEY",
      (Resolve-KeyFile "developer.public.pem"),
      [EnvironmentVariableTarget]::Process
    )
    [Environment]::SetEnvironmentVariable(
      "NAMO_TRUST_REVISION",
      [string]$TrustRevision,
      [EnvironmentVariableTarget]::Process
    )
    if (-not $DryRun) {
      Set-ValidatedCachedPassphrase `
        -Role "release" `
        -BuildEnvironmentName "NAMO_RELEASE_KEY_PASSPHRASE"
    }
  }

  if ($DryRun) {
    Write-Host "build-firmware: dry run"
    Write-Host "build-firmware: signer=$Signer release=$($Release.IsPresent) releaseType=$ReleaseType"
    Write-Host "build-firmware: node $($nodeArguments -join ' ')"
    return
  }

  if ($ValidateOnly) {
    Write-Host "build-firmware: cached $Signer passphrase verified"
    return
  }

  Push-Location $repoRoot
  try {
    & node @nodeArguments
    if ($LASTEXITCODE -ne 0) {
      throw "compile-firmware failed with exit code $LASTEXITCODE"
    }
  }
  finally {
    Pop-Location
  }
}
finally {
  foreach ($name in $managedEnvironment) {
    [Environment]::SetEnvironmentVariable(
      $name,
      $previousEnvironment[$name],
      [EnvironmentVariableTarget]::Process
    )
  }
}
