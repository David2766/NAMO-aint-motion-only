import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { resolve } from "node:path";
import test from "node:test";

const root = resolve(import.meta.dirname, "..");
const read = (path) => readFileSync(resolve(root, path), "utf8");

test("signed builds close every compiled raw OTA route", () => {
  const rootConfig = read("namo.yaml");
  const web = read("packages/namo/web.yaml");
  const legacyOta = read("packages/namo/ota-legacy.yaml");
  const signedOta = read("packages/namo/ota-signed.yaml");
  const options = read("scripts/firmware-build-options.mjs");
  const component = read("components/radar_api_server/__init__.py");
  const server = read("components/radar_api_server/radar_api_server.cpp");
  const updateHandler = read("components/radar_api_server/firmware_update_handler.cpp");
  const dashboardPackage = read("dashboard/src/web/state/firmware-package.ts");
  const signer = read("scripts/sign-firmware.mjs");
  assert.doesNotMatch(web, /platform:\s*esphome/);
  assert.doesNotMatch(web, /platform:\s*web_server/);
  assert.doesNotMatch(web, /^\s*ota:/m);
  assert.match(rootConfig, /ota_policy:\s*!include packages\/namo\/ota-\$\{ota_policy\}\.yaml/);
  assert.match(legacyOta, /^ota:/m);
  assert.match(legacyOta, /platform:\s*esphome/);
  assert.match(legacyOta, /platform:\s*web_server/);
  assert.doesNotMatch(signedOta, /platform:\s*esphome/);
  assert.match(signedOta, /ota:\s*\n\s*- platform:\s*web_server/);
  assert.match(signedOta, /web_server:\s*\n\s*ota:\s*false/);
  assert.match(options, /"ota_policy", signedOta \? "signed" : "legacy"/);
  assert.match(options, /"signed_ota", signedOta \? "true" : "false"/);
  assert.match(component, /NAMO_SIGNED_OTA/);
  assert.match(server, /Signed OTA handler requires PSRAM/);
  assert.match(server, /firmware_trust_unavailable/);
  assert.doesNotMatch(server, /firmware_update_handler_->loop/);
  assert.match(server, /FirmwareUpdateHandler::can_handle_upload\(request, url\)/);
  assert.match(updateHandler, /url == "\/api\/firmware\/update"/);
  assert.match(updateHandler, /request->method\(\) == HTTP_POST/);
  assert.match(updateHandler, /if \(!can_handle_upload\(request, url\)\)/);
  assert.match(updateHandler, /marker == FirmwareMultipartMarker::START/);
  assert.match(updateHandler, /marker == FirmwareMultipartMarker::END/);
  assert.match(updateHandler, /this->session_\.finish\(session_id, index/);
  assert.match(updateHandler, /url == "\/api\/firmware\/alpha-permission"/);
  assert.match(updateHandler, /alpha_firmware_not_allowed/);
  assert.ok(
    updateHandler.indexOf("this->session_.start(") <
      updateHandler.indexOf("this->developer_install_gate_.allowed(millis())"),
    "alpha permission must only be checked after package trust and signature verification"
  );
  assert.ok(
    updateHandler.indexOf("this->developer_install_gate_.allowed(millis())") <
      updateHandler.indexOf("this->session_.image_size()"),
    "an unarmed Developer package must be rejected before upload begins"
  );
  assert.match(
    updateHandler,
    /package\.manifest\.signer_role == FirmwareSignerRole::DEVELOPER[\s\S]*developer_install_gate_\.consume\(\)/
  );
  assert.match(
    updateHandler,
    /if \(this->upload_result_ != FirmwareUploadResult::OK\)\s+return;/
  );
  assert.ok(
    updateHandler.indexOf("marker == FirmwareMultipartMarker::START") <
      updateHandler.indexOf("this->session_.consume("),
    "ESP-IDF multipart start markers must be handled before signed bytes are consumed"
  );
  assert.ok(
    updateHandler.indexOf("this->trust_store_.stage_update(") <
      updateHandler.indexOf("this->ota_backend_->end()"),
    "anti-rollback trust must be staged before the new boot partition is activated"
  );
  assert.ok(
    updateHandler.indexOf("this->ota_backend_->end()") <
      updateHandler.indexOf("this->trust_store_.commit_staged("),
    "staged trust must only commit after OTA activation succeeds"
  );
  assert.match(updateHandler, /response->addHeader\("Connection", "close"\)/);
  assert.match(
    updateHandler,
    /request->beginResponse\(\s*200,\s*"application\/json",\s*"\{\\"ok\\":true,\\"status\\":\{\\"code\\":\\"ota_reboot_waiting\\"/,
    "signed OTA success must use the HTTP 200 status supported by web_server_idf"
  );
  assert.ok(
    updateHandler.indexOf("request->send(response)") <
      updateHandler.indexOf("this->server_->reboot_after(OTA_REBOOT_DELAY_MS)"),
    "the success response must be queued before reboot is scheduled"
  );
  assert.doesNotMatch(dashboardPackage, /crypto\.subtle/);
  assert.match(signer, /assertSignedOtaFirmwareImage\(firmware\)/);
  assert.match(signer, /assertFirmwareEmbedsPublicKeys\(firmware/);
});

test("release builds require external keys and embed only the provisioned public trust material", () => {
  const compile = read("scripts/compile-firmware.mjs");
  const wrapper = read("scripts/build-firmware.ps1");
  const registration = read("scripts/register-firmware-secret.ps1");
  const trustHeader = read("components/radar_api_server/firmware_trust_keys.h");
  const version = JSON.parse(read("version.json"));
  assert.match(compile, /NAMO_ROOT_PUBLIC_KEY/);
  assert.match(compile, /NAMO_RELEASE_PRIVATE_KEY/);
  assert.match(compile, /NAMO_DEVELOPER_PUBLIC_KEY/);
  assert.match(compile, /--signer/);
  assert.match(compile, /--signed-developer/);
  assert.match(compile, /NAMO_RELEASE_PUBLIC_KEY/);
  assert.match(compile, /NAMO_DEVELOPER_PRIVATE_KEY/);
  assert.match(compile, /NAMO_DEVELOPER_KEY_PASSPHRASE/);
  assert.match(compile, /role: "developer"/);
  assert.match(compile, /writeDeveloperImages/);
  assert.match(compile, /NAMO_RELEASE_KEY_PASSPHRASE/);
  assert.match(compile, /runEsphomeCompile\([^)]*options\.signer !== "none"/);
  assert.doesNotMatch(trustHeader, /PRIVATE KEY|private_key|privateKey/);
  assert.match(trustHeader, /PROVISIONED = true/);
  assert.match(trustHeader, /TRUST_REVISION = [1-9][0-9]*U/);
  assert.match(
    trustHeader,
    new RegExp(`SECURITY_EPOCH = ${version.firmwareSecurityEpoch}U`)
  );
  assert.match(trustHeader, /ROOT_KEY_ID/);
  assert.match(compile, /ROOT_KEY_ID.*p256KeyId\(context\.rootPublic\)/);
  assert.match(compile, /nextFirmwareSecurityEpoch/);
  assert.match(compile, /assertSignedOtaFirmwareImage\(rawFirmware\)/);
  assert.match(compile, /assertFirmwareEmbedsPublicKeys\(rawFirmware/);
  assert.match(wrapper, /\[ValidateSet\("none", "developer", "release"\)\]/);
  assert.match(wrapper, /ProtectedData\]::Unprotect/);
  assert.match(wrapper, /firmware-key-validation\.mjs/);
  assert.match(wrapper, /finally \{/);
  assert.match(wrapper, /-Release requires the separate -Signer release option/);
  assert.match(wrapper, /NAMO_FIRMWARE_KEY_DIRECTORY/);
  assert.doesNotMatch(wrapper, /[A-Z]:\\[^"']*namo-signing-keys/i);
  assert.doesNotMatch(wrapper, /BEGIN (?:ENCRYPTED )?PRIVATE KEY/);
  assert.match(compile, /VIRTUAL_ENV/);
  assert.match(compile, /repoRoot, "\.venv"/);
  assert.doesNotMatch(compile, /C:\/Users\/user|C:\\\\Users\\\\user/i);
  assert.match(registration, /Read-Host .* -AsSecureString/);
  assert.match(registration, /ProtectedData\]::Protect/);
  assert.match(registration, /no secret was saved/);
  assert.match(registration, /NAMO_FIRMWARE_KEY_DIRECTORY/);
  assert.doesNotMatch(registration, /[A-Z]:\\[^"']*namo-signing-keys/i);
  assert.doesNotMatch(registration, /BEGIN (?:ENCRYPTED )?PRIVATE KEY/);
  const manualSigner = read("scripts/sign-firmware.mjs");
  assert.match(manualSigner, /--root-public/);
  assert.match(manualSigner, /--release-public/);
  assert.match(manualSigner, /--developer-public/);
  assert.match(manualSigner, /assertFirmwareSigningIdentity/);
  assert.match(manualSigner, /assertFirmwareEmbedsPublicKeys\(firmware, embeddedTrustKeys\)/);
});
