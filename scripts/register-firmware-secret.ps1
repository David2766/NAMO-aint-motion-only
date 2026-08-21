[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)]
  [ValidateSet("developer", "release")]
  [string]$Role,

  [string]$KeyDirectory = $env:NAMO_FIRMWARE_KEY_DIRECTORY,

  [switch]$Replace,

  [string]$PassphraseEnvironment
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) {
  throw "Firmware secret registration requires Windows DPAPI"
}
Add-Type -AssemblyName System.Security

if ([string]::IsNullOrWhiteSpace($KeyDirectory)) {
  throw "Key registration requires -KeyDirectory or NAMO_FIRMWARE_KEY_DIRECTORY"
}

$resolvedKeyDirectory = (Resolve-Path -LiteralPath $KeyDirectory).Path
$privateKeyPath = Join-Path $resolvedKeyDirectory "$Role.private.pem"
$publicKeyPath = Join-Path $resolvedKeyDirectory "$Role.public.pem"
foreach ($path in @($privateKeyPath, $publicKeyPath)) {
  if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
    throw "Signing key file not found: $path"
  }
}

$secretDirectory = Join-Path $resolvedKeyDirectory ".secrets"
$secretPath = Join-Path $secretDirectory "$Role.dpapi"
if ((Test-Path -LiteralPath $secretPath -PathType Leaf) -and -not $Replace) {
  throw "Cached $Role passphrase already exists; use -Replace to replace it"
}

$validator = Join-Path $PSScriptRoot "firmware-key-validation.mjs"
$validationEnvironment = "NAMO_FIRMWARE_KEY_VALIDATION_PASSPHRASE"
$previousValidationEnvironment = [Environment]::GetEnvironmentVariable(
  $validationEnvironment,
  [EnvironmentVariableTarget]::Process
)
$pointer = [IntPtr]::Zero
$plainBytes = $null
$tempPath = $null

try {
  if ([string]::IsNullOrWhiteSpace($PassphraseEnvironment)) {
    $secure = Read-Host "$Role key passphrase" -AsSecureString
  }
  else {
    $providedPassphrase = [Environment]::GetEnvironmentVariable(
      $PassphraseEnvironment,
      [EnvironmentVariableTarget]::Process
    )
    if ([string]::IsNullOrEmpty($providedPassphrase)) {
      throw "Passphrase environment variable is empty"
    }
    $secure = ConvertTo-SecureString $providedPassphrase -AsPlainText -Force
  }
  if ($secure.Length -lt 16) {
    throw "$Role passphrase was rejected; no secret was saved"
  }

  $pointer = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)
  $plain = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($pointer)
  [Environment]::SetEnvironmentVariable(
    $validationEnvironment,
    $plain,
    [EnvironmentVariableTarget]::Process
  )

  & node $validator `
    --role $Role `
    --key-directory $resolvedKeyDirectory
  if ($LASTEXITCODE -ne 0) {
    throw "$Role passphrase was rejected; no secret was saved"
  }

  $plainBytes = [Text.Encoding]::UTF8.GetBytes($plain)
  $entropy = [Text.Encoding]::UTF8.GetBytes(
    "NAMO firmware signing secret cache v1"
  )
  $protectedBytes = [System.Security.Cryptography.ProtectedData]::Protect(
    $plainBytes,
    $entropy,
    [System.Security.Cryptography.DataProtectionScope]::CurrentUser
  )
  $payload = "NAMO-DPAPI-V1`n$([Convert]::ToBase64String($protectedBytes))`n"

  if (-not (Test-Path -LiteralPath $secretDirectory -PathType Container)) {
    $created = New-Item -ItemType Directory -Path $secretDirectory
    $created.Attributes = $created.Attributes -bor [IO.FileAttributes]::Hidden
  }

  $tempPath = "$secretPath.tmp-$PID-$([Guid]::NewGuid().ToString('N'))"
  [IO.File]::WriteAllText(
    $tempPath,
    $payload,
    [Text.UTF8Encoding]::new($false)
  )
  if (Test-Path -LiteralPath $secretPath -PathType Leaf) {
    Remove-Item -LiteralPath $secretPath -Force
  }
  Move-Item -LiteralPath $tempPath -Destination $secretPath
  $tempPath = $null
  Write-Host "firmware-secret: cached $Role passphrase with Windows DPAPI"
}
finally {
  [Environment]::SetEnvironmentVariable(
    $validationEnvironment,
    $previousValidationEnvironment,
    [EnvironmentVariableTarget]::Process
  )
  if ($pointer -ne [IntPtr]::Zero) {
    [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($pointer)
  }
  if ($null -ne $plainBytes) {
    [Array]::Clear($plainBytes, 0, $plainBytes.Length)
  }
  if ($null -ne $tempPath -and (Test-Path -LiteralPath $tempPath)) {
    Remove-Item -LiteralPath $tempPath -Force
  }
}
