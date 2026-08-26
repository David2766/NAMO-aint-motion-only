# NAMO Firmware Signing

Korean version: [firmware-signing.ko.md](firmware-signing.ko.md)

## Security Boundary

Signed OTA protects the firmware update path from an unsigned or incorrectly
signed image. It does not use Secure Boot, Flash Encryption, eFuse, HTTPS, a
mobile application, or Home Assistant as a trust dependency.

| Key | Online status | Purpose | Recovery |
| --- | --- | --- | --- |
| Root | Offline | Authorize release or developer key replacement | USB recovery is required if compromised |
| Release | Release host only | Sign official release packages | Revoke and replace with a Root-signed recovery package |
| Developer | Developer host only | Sign controlled custom builds | Revoke and replace with a Root-signed recovery package |

Only public keys and key IDs enter firmware. Private keys are encrypted PKCS#8
files and must never enter the repository, build artifact, firmware image, or
device.

## Package And Device Rules

- Package format: fixed 320-byte signed envelope followed by the exact ESP32
  OTA image.
- Algorithm: ECDSA P-256 with SHA-256 and IEEE P1363 `r || s` signatures.
- The signature covers product, hardware, image size, trust revision, security
  epoch, signer identity, image digest, version, build ID, and recovery fields.
- The device verifies the envelope before opening the OTA partition.
- The uploaded envelope must exactly match the preflight envelope.
- The device hashes the flashed partition and compares it with the signed image
  digest before activation.
- A signed build has no ESPHome Native OTA service and disables ESPHome's raw
  web OTA handler. Dashboard fallback to `/update` is allowed only when
  `/api/firmware/trust` returns HTTP 404 from an older development firmware.
- Release and manual signing tools reject an inner image that does not contain
  the signed OTA implementation marker and the complete Root, Release, and
  Developer public-key set. The declared role must match the supplied private
  key. Root recovery also requires the replacement public key in the image.
- Trust storage uses two NVS slots plus a commit record. Corrupt committed trust
  fails closed and does not resurrect an older revoked keyring.
- Trust changes are staged in the inactive NVS slot, the OTA backend activates
  the verified image, and only then does firmware publish the staged commit. A
  commit failure reselects the running partition. An interrupted staged update
  leaves the previous committed trust authoritative. A newer Root trust
  revision may rebase the security epoch to recover from an epoch pinned by a
  compromised Release key.
- Every `--release` build advances and persists `firmwareSecurityEpoch` exactly
  once, so an older official package cannot be replayed after a newer release.
  A Developer package must use the current epoch and cannot raise it.
- Signed OTA requires PSRAM. Allocation failure closes OTA instead of consuming
  scarce internal RAM.

## Key Creation

Create a new directory outside the repository:

```powershell
$keyDirectory = Join-Path $HOME "namo-signing-keys-v1"
$env:NAMO_ROOT_KEY_PASSPHRASE='use-a-long-unique-passphrase'
$env:NAMO_RELEASE_KEY_PASSPHRASE='use-a-different-long-passphrase'
$env:NAMO_DEVELOPER_KEY_PASSPHRASE='use-another-long-passphrase'
node scripts/firmware-keygen.mjs --out $keyDirectory
$env:NAMO_FIRMWARE_KEY_DIRECTORY=$keyDirectory
```

Keep `root.private.pem` offline. Store a second offline backup of the Root key
and its passphrase separately. Losing the Root key removes the supported remote
key-rotation path.

## Build Passphrase Registration

Register the Developer and Release private-key passphrases once:

```powershell
.\scripts\register-firmware-secret.ps1 -Role developer
.\scripts\register-firmware-secret.ps1 -Role release
```

The registration script reads `NAMO_FIRMWARE_KEY_DIRECTORY`, or the directory
provided with `-KeyDirectory`, decrypts the private key, and verifies it against
the same role's public key before writing a Windows DPAPI ciphertext under its
`.secrets` directory. A wrong passphrase is neither saved nor
retried. Replacing an existing cache requires `-Replace`; failed replacement
validation preserves the existing cache. The cache is bound to the current
Windows user, so register it again from the printed recovery value after moving
to another PC or account. The Root passphrase is never cached.

## Official Release

The release build reads external keys and writes a verified `.namo-ota`
package. It never writes private key material into generated firmware headers.

```powershell
.\scripts\build-firmware.ps1 -Release -Signer release
```

For a patch-only hotfix release, use the explicit alias:

```powershell
.\scripts\build-firmware.ps1 -Hotfix -Signer release
```

`-Hotfix` is fixed to patch versioning. It produces the same signed Release
package, advances the security epoch, and cannot be combined with `-Release` or
`-ReleaseType`. Export it with:

```powershell
node .\scripts\export-firmware-dashboard.mjs --hotfix --allow-dirty-target
```

`-Release` advances the version and security epoch. `-Signer release` selects
the Release signing identity. They remain separate options, and the wrapper
rejects either option without the other before starting a build. Set
`NAMO_FIRMWARE_KEY_DIRECTORY` once in the current shell or pass `-KeyDirectory`
explicitly. The wrapper decrypts and validates the cache once, stops before
compilation on failure, and restores the temporary environment after success
or failure.

Do not edit `firmwareSecurityEpoch` downward. The release command increments it
automatically and restores the prior value if the build fails.

Before a release upload, run the non-destructive physical preflight smoke:

```powershell
python tools\firmware_update_security_smoke.py `
  --device http://192.168.29.218 `
  --package .esphome\build\presence-sensor\.pioenvs\presence-sensor-build\firmware-release-<version>.namo-ota
```

It opens and cancels one valid session, verifies that a wrong cancellation
token cannot terminate it, and checks truncation, excess bytes, and all 320
single-byte envelope mutations. It never writes the OTA image.

The `.sha256` file detects download corruption. It is not a substitute for the
signature that the device verifies.

## Signed Developer Build

Developer packages keep the current security epoch and cannot advance it.

```powershell
.\scripts\build-firmware.ps1 -Signer developer
```

The command embeds the complete public trust set, disables raw OTA, verifies
the Developer signing identity, and writes `firmware-developer-<version>.namo-ota`.

Build the legacy unsigned development image without loading any key:

```powershell
.\scripts\build-firmware.ps1 -Signer none
```

All three wrapper modes build the dashboard and clean by default. Use
`-NoDashboard` or `-NoClean` only when intentionally skipping either step. The
equivalent direct compiler arguments are `--signer none`, `--signer developer`,
`--release --signer release`, and `--hotfix --signer release`.

## Release Or Developer Key Recovery

1. Stop using the compromised private key.
2. Generate a distinct replacement key pair.
3. Build an OTA image whose embedded initial trust contains the replacement
   public key.
4. Increment `trustRevision`. Increment `firmwareSecurityEpoch` when older
   firmware must be blocked.
5. Sign one recovery package with the offline Root private key. The package must
   name the revoked key ID, replacement role, replacement key ID, and public
   key.
6. Verify the package offline, deploy it, reboot, and confirm the new trust
   revision and key ID through `/api/firmware/trust`.
7. Verify that a package signed by the revoked key is rejected.

Trust revision and security epoch never decrease. A Root key compromise cannot
be repaired by another remotely delivered signature because the attacker can
also produce Root signatures. Establish a new trust baseline through controlled
USB recovery in that case.

## Migration

The unpublished legacy development firmware may receive the first signed build
through its existing raw web OTA path. Keep that bootstrap `.bin` local and do
not publish it as a normal release artifact. After the signed build boots, the
dashboard accepts only `.namo-ota` packages and never falls back on trust errors.

## Release Gates

- Host signing, mutation, malformed package, rollback, rotation, interrupted
  storage, chunk ordering, timeout, and sink failure tests pass.
- Dashboard signed/legacy routing and no-downgrade tests pass.
- `tools/firmware_update_security_smoke.py` passes all 320 one-byte envelope
  mutations without writing an OTA image.
- Firmware compiles with provisioned public keys and no raw OTA route.
- A physical device passes valid release, valid developer, forged package,
  revoked key, interrupted upload, reboot, and Root recovery tests.
- Resource smoke passes during upload and post-write partition verification.
