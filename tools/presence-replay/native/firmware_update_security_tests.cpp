#include "firmware_update_security.h"
#include "firmware_update_session.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace esphome::radar_api_server;

namespace {

void require(bool condition, const char *message) {
  if (condition)
    return;
  std::fprintf(stderr, "firmware_update_security_tests: %s\n", message);
  std::exit(1);
}

void write_u16(uint8_t *data, uint16_t value) {
  data[0] = static_cast<uint8_t>(value);
  data[1] = static_cast<uint8_t>(value >> 8U);
}

void write_u32(uint8_t *data, uint32_t value) {
  data[0] = static_cast<uint8_t>(value);
  data[1] = static_cast<uint8_t>(value >> 8U);
  data[2] = static_cast<uint8_t>(value >> 16U);
  data[3] = static_cast<uint8_t>(value >> 24U);
}

template <std::size_t N>
std::array<uint8_t, N> bytes(uint8_t seed) {
  std::array<uint8_t, N> value{};
  for (std::size_t index = 0; index < N; ++index)
    value[index] = static_cast<uint8_t>(seed + index);
  return value;
}

std::array<uint8_t, FirmwarePackageManifest::PUBLIC_KEY_SIZE> public_key(
    uint8_t seed) {
  auto value = bytes<FirmwarePackageManifest::PUBLIC_KEY_SIZE>(seed);
  value[0] = 0x04;
  return value;
}

std::array<uint8_t, FirmwarePackageHeader::HEADER_SIZE> package_bytes(
    FirmwareSignerRole role = FirmwareSignerRole::RELEASE) {
  std::array<uint8_t, FirmwarePackageHeader::HEADER_SIZE> raw{};
  const uint8_t magic[8] = {'N', 'A', 'M', 'O', 'O', 'T', 'A', '1'};
  std::memcpy(raw.data(), magic, sizeof(magic));
  write_u16(raw.data() + 8, FirmwarePackageHeader::FORMAT_VERSION);
  write_u16(raw.data() + 10, FirmwarePackageHeader::HEADER_SIZE);
  raw[12] = static_cast<uint8_t>(role);
  raw[13] = static_cast<uint8_t>(FirmwareSignatureAlgorithm::ES256_P1363);
  write_u32(raw.data() + 16, FirmwarePackageHeader::PRODUCT_ID);
  write_u32(raw.data() + 20, FirmwarePackageHeader::HARDWARE_ID);
  write_u32(raw.data() + 24, 123456);
  write_u32(raw.data() + 28, role == FirmwareSignerRole::ROOT_RECOVERY ? 2 : 1);
  write_u32(raw.data() + 32,
            role == FirmwareSignerRole::ROOT_RECOVERY ? 7 : 5);
  const auto signer_id =
      role == FirmwareSignerRole::ROOT_RECOVERY ? bytes<16>(0x61)
                                                 : bytes<16>(0x11);
  const auto digest = bytes<32>(0x51);
  std::memcpy(raw.data() + 36, signer_id.data(), signer_id.size());
  std::memcpy(raw.data() + 52, digest.data(), digest.size());
  std::memcpy(raw.data() + 84, "0.5.6", 6);
  std::memcpy(raw.data() + 108, "20260806T120000Z", 17);
  if (role == FirmwareSignerRole::ROOT_RECOVERY) {
    const auto revoked_id = bytes<16>(0x11);
    const auto next_id = bytes<16>(0x31);
    const auto next_key = public_key(0x71);
    std::memcpy(raw.data() + 132, revoked_id.data(), revoked_id.size());
    std::memcpy(raw.data() + 148, next_id.data(), next_id.size());
    std::memcpy(raw.data() + 164, next_key.data(), next_key.size());
    raw[229] = static_cast<uint8_t>(FirmwareSignerRole::RELEASE);
  }
  const auto signature = bytes<64>(0x91);
  std::memcpy(raw.data() + FirmwarePackageHeader::SIGNED_SIZE,
              signature.data(), signature.size());
  return raw;
}

FirmwarePackageHeader parse(
    const std::array<uint8_t, FirmwarePackageHeader::HEADER_SIZE> &raw) {
  FirmwarePackageHeader package{};
  require(parse_firmware_package_header(raw.data(), raw.size(), &package) ==
              FirmwarePackageResult::OK,
          "valid package header must parse");
  return package;
}

FirmwareTrustState trust_state() {
  FirmwareTrustState state{};
  state.trust_revision = 1;
  state.minimum_security_epoch = 5;
  state.release_keys[0].occupied = true;
  state.release_keys[0].role = FirmwareSignerRole::RELEASE;
  state.release_keys[0].key_id = bytes<16>(0x11);
  state.release_keys[0].public_key = public_key(0x21);
  state.developer_keys[0].occupied = true;
  state.developer_keys[0].role = FirmwareSignerRole::DEVELOPER;
  state.developer_keys[0].key_id = bytes<16>(0x41);
  state.developer_keys[0].public_key = public_key(0x51);
  return state;
}

class ExactVerifier : public FirmwareSignatureVerifier {
 public:
  bool allow{true};
  bool public_key_valid{true};
  bool public_key_id_valid{true};
  std::array<uint8_t, FirmwarePackageManifest::PUBLIC_KEY_SIZE> expected_key{};
  std::array<uint8_t, FirmwarePackageHeader::SIGNED_SIZE> expected_message{};
  std::array<uint8_t, FirmwarePackageHeader::SIGNATURE_SIZE> expected_signature{};
  uint32_t calls{0};

  bool validate_es256_public_key(
      const std::array<uint8_t, FirmwarePackageManifest::PUBLIC_KEY_SIZE> &key,
      const std::array<uint8_t, FirmwarePackageManifest::KEY_ID_SIZE> &)
      override {
    return this->public_key_valid && this->public_key_id_valid &&
           key[0] == 0x04;
  }

  bool verify_es256_p1363(
      const std::array<uint8_t, FirmwarePackageManifest::PUBLIC_KEY_SIZE> &key,
      const uint8_t *message, std::size_t message_size,
      const std::array<uint8_t, FirmwarePackageHeader::SIGNATURE_SIZE>
          &signature) override {
    ++this->calls;
    return this->allow && key == this->expected_key &&
           message_size == this->expected_message.size() &&
           std::memcmp(message, this->expected_message.data(), message_size) ==
               0 &&
           signature == this->expected_signature;
  }
};

ExactVerifier verifier_for(
    const FirmwarePackageHeader &package,
    const std::array<uint8_t, FirmwarePackageManifest::PUBLIC_KEY_SIZE> &key) {
  ExactVerifier verifier;
  verifier.expected_key = key;
  verifier.expected_message = package.signed_bytes;
  verifier.expected_signature = package.signature;
  return verifier;
}

FirmwareTrustDecision evaluate_for_test(
    const FirmwarePackageHeader &package, const FirmwareTrustState &state,
    const std::array<uint8_t, FirmwarePackageManifest::PUBLIC_KEY_SIZE> &root,
    FirmwareSignatureVerifier &verifier) {
  return evaluate_firmware_package_trust(package, state, bytes<16>(0x61),
                                         root, verifier);
}

class MemoryTrustStorage : public FirmwareTrustStorageBackend {
 public:
  std::array<FirmwareTrustStorageBlob, 2> slots{};
  std::array<bool, 2> slot_present{};
  FirmwareTrustCommitBlob commit{};
  bool commit_present{false};
  bool fail_slot_save{false};
  bool fail_commit_save{false};
  uint32_t sync_calls{0};
  uint32_t fail_sync_call{0};

  bool load_firmware_trust_slot(uint8_t slot,
                                FirmwareTrustStorageBlob *blob) override {
    if (slot > 1 || blob == nullptr || !this->slot_present[slot])
      return false;
    *blob = this->slots[slot];
    return true;
  }

  bool save_firmware_trust_slot(
      uint8_t slot, const FirmwareTrustStorageBlob &blob) override {
    if (slot > 1 || this->fail_slot_save)
      return false;
    this->slots[slot] = blob;
    this->slot_present[slot] = true;
    return true;
  }

  bool load_firmware_trust_commit(FirmwareTrustCommitBlob *blob) override {
    if (blob == nullptr || !this->commit_present)
      return false;
    *blob = this->commit;
    return true;
  }

  bool save_firmware_trust_commit(
      const FirmwareTrustCommitBlob &blob) override {
    if (this->fail_commit_save)
      return false;
    this->commit = blob;
    this->commit_present = true;
    return true;
  }

  bool sync() override {
    ++this->sync_calls;
    return this->fail_sync_call == 0 ||
           this->sync_calls != this->fail_sync_call;
  }
};

class MemoryFirmwareSink : public FirmwareUploadSink {
 public:
  bool begin_ok{true};
  bool write_ok{true};
  bool finish_ok{true};
  uint32_t expected_size{0};
  uint32_t begin_calls{0};
  uint32_t finish_calls{0};
  uint32_t abort_calls{0};
  std::array<uint8_t, 64> image{};
  std::size_t image_size{0};

  bool begin_firmware_image(uint32_t size) override {
    ++this->begin_calls;
    this->expected_size = size;
    this->image_size = 0;
    return this->begin_ok;
  }
  bool write_firmware_image(const uint8_t *data,
                            std::size_t size) override {
    if (!this->write_ok || data == nullptr ||
        size > this->image.size() - this->image_size)
      return false;
    std::memcpy(this->image.data() + this->image_size, data, size);
    this->image_size += size;
    return true;
  }
  bool finish_firmware_image(const FirmwarePackageHeader &,
                             const FirmwareTrustDecision &) override {
    ++this->finish_calls;
    return this->finish_ok && this->image_size == this->expected_size;
  }
  void abort_firmware_image() override { ++this->abort_calls; }
};

void package_parser_rejects_every_bounded_contract_violation() {
  const auto valid = package_bytes();
  FirmwarePackageHeader parsed{};
  require(parse_firmware_package_header(valid.data(), valid.size(), &parsed) ==
              FirmwarePackageResult::OK,
          "baseline package must parse");
  require(std::memcmp(parsed.signed_bytes.data(), valid.data(),
                      parsed.signed_bytes.size()) == 0,
          "signature input must preserve the exact wire bytes");
  require(parse_firmware_package_header(nullptr, valid.size(), &parsed) ==
              FirmwarePackageResult::INVALID_ARGUMENT &&
              parse_firmware_package_header(valid.data(), 319, &parsed) ==
                  FirmwarePackageResult::INVALID_ARGUMENT &&
              parse_firmware_package_header(valid.data(), 321, &parsed) ==
                  FirmwarePackageResult::INVALID_ARGUMENT,
          "null, truncated, and oversized package headers must fail closed");

  const auto reject = [&](std::size_t offset, uint8_t value,
                          FirmwarePackageResult expected,
                          const char *message) {
    auto raw = valid;
    raw[offset] = value;
    require(parse_firmware_package_header(raw.data(), raw.size(), &parsed) ==
                expected,
            message);
  };
  reject(0, 'X', FirmwarePackageResult::INVALID_MAGIC,
         "package magic mutation must fail");
  reject(8, 2, FirmwarePackageResult::UNSUPPORTED_FORMAT,
         "format mutation must fail");
  reject(13, 2, FirmwarePackageResult::UNSUPPORTED_ALGORITHM,
         "algorithm mutation must fail");
  reject(12, 0, FirmwarePackageResult::INVALID_ROLE,
         "unknown signer role must fail");
  reject(14, 1, FirmwarePackageResult::INVALID_RESERVED,
         "unknown flags must fail");
  reject(230, 1, FirmwarePackageResult::INVALID_RESERVED,
         "nonzero reserved bytes must fail");
  reject(16, 0, FirmwarePackageResult::INVALID_PRODUCT,
         "wrong product must fail");
  reject(20, 0, FirmwarePackageResult::INVALID_HARDWARE,
         "wrong hardware must fail");

  auto raw = valid;
  write_u32(raw.data() + 24, 0);
  require(parse_firmware_package_header(raw.data(), raw.size(), &parsed) ==
              FirmwarePackageResult::INVALID_IMAGE_SIZE,
          "zero image size must fail");
  raw = valid;
  write_u32(raw.data() + 24, FirmwarePackageHeader::MAX_IMAGE_SIZE + 1);
  require(parse_firmware_package_header(raw.data(), raw.size(), &parsed) ==
              FirmwarePackageResult::INVALID_IMAGE_SIZE,
          "oversized image must fail");
  raw = valid;
  std::memset(raw.data() + 84, 'A', 24);
  require(parse_firmware_package_header(raw.data(), raw.size(), &parsed) ==
              FirmwarePackageResult::INVALID_TEXT,
          "unterminated version must fail");
  raw = valid;
  raw[84] = 0x1f;
  require(parse_firmware_package_header(raw.data(), raw.size(), &parsed) ==
              FirmwarePackageResult::INVALID_TEXT,
          "control characters must fail");
  raw = valid;
  raw[132] = 1;
  require(parse_firmware_package_header(raw.data(), raw.size(), &parsed) ==
              FirmwarePackageResult::INVALID_RECOVERY,
          "normal package carrying recovery fields must fail");
  raw = package_bytes(FirmwareSignerRole::ROOT_RECOVERY);
  std::memset(raw.data() + 148, 0, 16);
  require(parse_firmware_package_header(raw.data(), raw.size(), &parsed) ==
              FirmwarePackageResult::INVALID_RECOVERY,
          "incomplete recovery package must fail");
  raw = package_bytes(FirmwareSignerRole::ROOT_RECOVERY);
  std::memcpy(raw.data() + 148, raw.data() + 132, 16);
  require(parse_firmware_package_header(raw.data(), raw.size(), &parsed) ==
              FirmwarePackageResult::INVALID_RECOVERY,
          "recovery cannot revoke and install the same key id");
}

void trust_evaluation_enforces_role_signature_and_rollback_fences() {
  const auto root = public_key(0x71);
  auto state = trust_state();
  auto package = parse(package_bytes());
  auto verifier = verifier_for(package, state.release_keys[0].public_key);
  auto decision = evaluate_for_test(package, state, root, verifier);
  require(decision.accepted() && !decision.trust_update_required &&
              verifier.calls == 1,
          "trusted release signature must be accepted without changing trust");

  auto epoch_raw = package_bytes();
  write_u32(epoch_raw.data() + 32, 6);
  auto epoch_package = parse(epoch_raw);
  auto epoch_verifier =
      verifier_for(epoch_package, state.release_keys[0].public_key);
  const auto epoch_decision =
      evaluate_for_test(epoch_package, state, root, epoch_verifier);
  require(epoch_decision.accepted() &&
              epoch_decision.trust_update_required &&
              epoch_decision.next_trust.minimum_security_epoch == 6,
          "release package must durably advance the security epoch");
  auto replay_verifier = verifier_for(package, state.release_keys[0].public_key);
  require(evaluate_for_test(package, epoch_decision.next_trust, root,
                            replay_verifier)
              .result == FirmwareTrustResult::SECURITY_ROLLBACK,
          "an older Release package must fail after the epoch advances");

  auto developer_raw = package_bytes(FirmwareSignerRole::DEVELOPER);
  std::memcpy(developer_raw.data() + 36,
              state.developer_keys[0].key_id.data(), 16);
  auto developer_package = parse(developer_raw);
  auto developer_verifier =
      verifier_for(developer_package, state.developer_keys[0].public_key);
  require(evaluate_for_test(developer_package, state, root,
                            developer_verifier)
              .accepted(),
          "trusted developer package at the current epoch must be accepted");
  write_u32(developer_raw.data() + 32, 6);
  developer_package = parse(developer_raw);
  developer_verifier =
      verifier_for(developer_package, state.developer_keys[0].public_key);
  require(evaluate_for_test(developer_package, state, root,
                            developer_verifier)
              .result == FirmwareTrustResult::SECURITY_EPOCH_MISMATCH,
          "developer package must not raise the durable security epoch");

  verifier.allow = false;
  require(evaluate_for_test(package, state, root, verifier).result ==
              FirmwareTrustResult::INVALID_SIGNATURE,
          "invalid release signature must fail");
  verifier.allow = true;
  package.manifest.security_epoch = 4;
  require(evaluate_for_test(package, state, root, verifier).result ==
              FirmwareTrustResult::SECURITY_ROLLBACK,
          "security epoch rollback must fail before verification");
  package = parse(package_bytes());
  package.manifest.trust_revision = 2;
  require(evaluate_for_test(package, state, root, verifier).result ==
              FirmwareTrustResult::TRUST_REVISION_MISMATCH,
          "normal packages cannot silently change trust revision");
  package = parse(package_bytes());
  package.manifest.signer_role = FirmwareSignerRole::DEVELOPER;
  require(evaluate_for_test(package, state, root, verifier).result ==
              FirmwareTrustResult::ROLE_MISMATCH,
          "a key registered for another role must fail explicitly");
  package.manifest.signer_key_id = bytes<16>(0x91);
  require(evaluate_for_test(package, state, root, verifier).result ==
              FirmwareTrustResult::UNKNOWN_KEY,
          "unknown developer key must fail");
  package = parse(package_bytes());
  state.revoked_key_ids[0] = package.manifest.signer_key_id;
  require(evaluate_for_test(package, state, root, verifier).result ==
              FirmwareTrustResult::TRUST_UNAVAILABLE,
          "a malformed keyring that still contains a revoked key must fail closed");
  state = trust_state();
  state.release_keys[0] = {};
  require(evaluate_for_test(package, state, root, verifier).result ==
              FirmwareTrustResult::TRUST_UNAVAILABLE,
          "a keyring without a release key must fail closed");
}

void root_recovery_rotates_release_key_without_private_material() {
  const auto root = public_key(0x71);
  auto state = trust_state();
  auto recovery = parse(package_bytes(FirmwareSignerRole::ROOT_RECOVERY));
  auto verifier = verifier_for(recovery, root);
  require(evaluate_firmware_package_trust(
              recovery, state, bytes<16>(0x62), root, verifier)
              .result == FirmwareTrustResult::UNKNOWN_KEY,
          "root recovery signer id must match the embedded root key id");
  auto decision =
      evaluate_for_test(recovery, state, root, verifier);
  require(decision.accepted() && decision.trust_update_required &&
              decision.next_trust.trust_revision == 2 &&
              decision.next_trust.minimum_security_epoch == 7,
          "root recovery must advance trust and security epochs");
  std::size_t replacement_count = 0;
  for (const auto &key : decision.next_trust.release_keys) {
    if (key.occupied &&
        key.key_id == recovery.manifest.replacement_key_id)
      ++replacement_count;
    require(!key.occupied || key.key_id != recovery.manifest.revoked_key_id,
            "revoked release key must not remain in an occupied slot");
  }
  require(replacement_count == 1,
          "root recovery must remove the compromised release key and add its replacement");
  require(decision.next_trust.revoked_key_ids[0] ==
              recovery.manifest.revoked_key_id,
          "compromised release key id must be durably revoked");

  auto old_package = parse(package_bytes());
  old_package.manifest.trust_revision =
      decision.next_trust.trust_revision;
  old_package.manifest.security_epoch =
      decision.next_trust.minimum_security_epoch;
  auto old_verifier = verifier_for(old_package, state.release_keys[0].public_key);
  require(evaluate_for_test(old_package, decision.next_trust,
                            root, old_verifier)
              .result == FirmwareTrustResult::REVOKED_KEY,
          "old release key must remain unusable after recovery");
  auto new_package = parse(package_bytes());
  new_package.manifest.signer_key_id = recovery.manifest.replacement_key_id;
  new_package.manifest.trust_revision =
      decision.next_trust.trust_revision;
  new_package.manifest.security_epoch =
      decision.next_trust.minimum_security_epoch;
  auto new_verifier = verifier_for(
      new_package, recovery.manifest.replacement_public_key);
  require(evaluate_for_test(new_package, decision.next_trust,
                            root, new_verifier)
              .accepted(),
          "new release key must work after recovery");

  recovery.manifest.trust_revision = 1;
  require(evaluate_for_test(recovery, state, root, verifier).result ==
              FirmwareTrustResult::TRUST_ROLLBACK,
          "root recovery must be strictly monotonic");
  recovery = parse(package_bytes(FirmwareSignerRole::ROOT_RECOVERY));
  verifier = verifier_for(recovery, root);
  verifier.allow = false;
  require(evaluate_for_test(recovery, state, root, verifier).result ==
              FirmwareTrustResult::INVALID_SIGNATURE,
          "forged root recovery must fail");
  verifier.allow = true;
  verifier.public_key_valid = false;
  require(evaluate_for_test(recovery, trust_state(), root, verifier).result ==
              FirmwareTrustResult::INVALID_RECOVERY,
          "root recovery must reject a replacement key outside P-256");
  verifier.public_key_valid = true;
  verifier.public_key_id_valid = false;
  require(evaluate_for_test(recovery, trust_state(), root, verifier).result ==
              FirmwareTrustResult::INVALID_RECOVERY,
          "root recovery must reject a replacement key id mismatch");
  state = trust_state();
  for (std::size_t index = 0; index < state.revoked_key_ids.size(); ++index)
    state.revoked_key_ids[index] = bytes<16>(static_cast<uint8_t>(0x80 + index * 0x10));
  verifier.public_key_id_valid = true;
  require(evaluate_for_test(recovery, state, root, verifier).result ==
              FirmwareTrustResult::KEY_CAPACITY_EXCEEDED,
          "full revocation storage must fail without partial trust changes");
}

void root_recovery_rotates_developer_key_independently() {
  const auto root = public_key(0x71);
  const auto state = trust_state();
  auto raw = package_bytes(FirmwareSignerRole::ROOT_RECOVERY);
  const auto replacement_id = bytes<16>(0x81);
  const auto replacement_key = public_key(0x91);
  std::memcpy(raw.data() + 132, state.developer_keys[0].key_id.data(), 16);
  std::memcpy(raw.data() + 148, replacement_id.data(), 16);
  std::memcpy(raw.data() + 164, replacement_key.data(), 65);
  raw[229] = static_cast<uint8_t>(FirmwareSignerRole::DEVELOPER);
  auto recovery = parse(raw);
  auto verifier = verifier_for(recovery, root);
  const auto decision = evaluate_for_test(recovery, state, root, verifier);
  require(decision.accepted() && decision.trust_update_required,
          "root recovery must rotate a developer key");

  auto old_raw = package_bytes(FirmwareSignerRole::DEVELOPER);
  write_u32(old_raw.data() + 28, 2);
  write_u32(old_raw.data() + 32, 7);
  std::memcpy(old_raw.data() + 36, state.developer_keys[0].key_id.data(), 16);
  auto old_package = parse(old_raw);
  auto old_verifier =
      verifier_for(old_package, state.developer_keys[0].public_key);
  require(evaluate_for_test(old_package, decision.next_trust, root,
                            old_verifier)
              .result == FirmwareTrustResult::REVOKED_KEY,
          "old developer key must not work after recovery");

  auto next_raw = package_bytes(FirmwareSignerRole::DEVELOPER);
  write_u32(next_raw.data() + 28, 2);
  write_u32(next_raw.data() + 32, 7);
  std::memcpy(next_raw.data() + 36, replacement_id.data(), 16);
  auto next_package = parse(next_raw);
  auto next_verifier = verifier_for(next_package, replacement_key);
  require(evaluate_for_test(next_package, decision.next_trust, root,
                            next_verifier)
              .accepted(),
          "replacement developer key must work at the recovered epoch");

  auto pinned = state;
  pinned.minimum_security_epoch = 0xffffffffU;
  auto recovery_reset = recovery;
  recovery_reset.manifest.security_epoch = 1;
  auto recovery_reset_verifier = verifier_for(recovery_reset, root);
  const auto reset_decision = evaluate_for_test(
      recovery_reset, pinned, root, recovery_reset_verifier);
  require(reset_decision.accepted() &&
              reset_decision.next_trust.minimum_security_epoch == 1,
          "newer Root trust revision must recover from a Release-pinned epoch");
}

void trust_store_survives_interrupted_updates_and_fails_closed_on_corruption() {
  FirmwareTrustStore store;
  MemoryTrustStorage storage;
  FirmwareTrustSnapshot snapshot{};
  auto initial = trust_state();
  require(store.load(storage, &snapshot) == FirmwareTrustStoreResult::EMPTY,
          "blank trust storage must be distinguishable from corruption");
  require(store.create(storage, initial, &snapshot) ==
              FirmwareTrustStoreResult::CREATED &&
              snapshot.generation == 1,
          "initial trust must be committed atomically");

  FirmwareTrustStore after_reboot;
  FirmwareTrustSnapshot loaded{};
  require(after_reboot.load(storage, &loaded) ==
              FirmwareTrustStoreResult::LOADED &&
              loaded.generation == 1 && loaded.state.trust_revision == 1,
          "committed initial trust must survive reboot");

  MemoryTrustStorage staged_storage = storage;
  FirmwareTrustSnapshot staged_snapshot = loaded;
  auto staged_next = initial;
  staged_next.trust_revision = 2;
  staged_next.minimum_security_epoch = 6;
  FirmwareTrustStagedUpdate staged_update{};
  FirmwareTrustStore staged_store;
  require(staged_store.stage_update(staged_storage, staged_next,
                                    staged_snapshot, &staged_update) ==
              FirmwareTrustStoreResult::STAGED &&
              staged_update.valid && staged_update.generation == 2 &&
              staged_snapshot.generation == 1,
          "staging must not expose the next trust state in RAM");
  FirmwareTrustSnapshot before_activation{};
  FirmwareTrustStore before_activation_store;
  require(before_activation_store.load(staged_storage,
                                       &before_activation) ==
              FirmwareTrustStoreResult::LOADED &&
              before_activation.generation == 1 &&
              before_activation.state.trust_revision == 1,
          "a reboot before OTA activation must ignore staged trust");
  require(staged_store.commit_staged(staged_storage, staged_update,
                                     &staged_snapshot) ==
              FirmwareTrustStoreResult::UPDATED &&
              staged_snapshot.generation == 2 &&
              staged_snapshot.state.trust_revision == 2,
          "only an explicit post-activation commit may publish staged trust");
  FirmwareTrustSnapshot after_activation{};
  require(before_activation_store.load(staged_storage, &after_activation) ==
              FirmwareTrustStoreResult::LOADED &&
              after_activation.generation == 2 &&
              after_activation.state.minimum_security_epoch == 6,
          "committed staged trust must survive reboot");
  require(staged_store.commit_staged(staged_storage, staged_update,
                                     &staged_snapshot) ==
              FirmwareTrustStoreResult::STORAGE_FAILED,
          "a staged trust token must not be reusable after commit");

  MemoryTrustStorage epoch_storage = storage;
  FirmwareTrustSnapshot epoch_snapshot = loaded;
  auto epoch_only = initial;
  epoch_only.minimum_security_epoch = initial.minimum_security_epoch + 1;
  FirmwareTrustStore epoch_store;
  require(epoch_store.update(epoch_storage, epoch_only, &epoch_snapshot) ==
              FirmwareTrustStoreResult::UPDATED &&
              epoch_snapshot.generation == 2 &&
              epoch_snapshot.state.trust_revision == 1 &&
              epoch_snapshot.state.minimum_security_epoch == 6,
          "security epoch advance must commit without a keyring revision change");
  FirmwareTrustSnapshot epoch_loaded{};
  require(epoch_store.load(epoch_storage, &epoch_loaded) ==
              FirmwareTrustStoreResult::LOADED &&
              epoch_loaded.state.minimum_security_epoch == 6,
          "security epoch advance must survive reboot");

  auto next = initial;
  next.trust_revision = 2;
  next.minimum_security_epoch = 7;
  next.release_keys[1].occupied = true;
  next.release_keys[1].role = FirmwareSignerRole::RELEASE;
  next.release_keys[1].key_id = bytes<16>(0x31);
  next.release_keys[1].public_key = public_key(0x71);
  require(store.update(storage, next, &snapshot) ==
              FirmwareTrustStoreResult::UPDATED &&
              snapshot.generation == 2,
          "trust update must alternate slots and commit generation two");
  storage.commit_present = false;
  FirmwareTrustSnapshot repaired{};
  FirmwareTrustStore repair_store;
  require(repair_store.load(storage, &repaired) ==
              FirmwareTrustStoreResult::COMMIT_REPAIRED &&
              repaired.generation == 2 && repaired.state.trust_revision == 2,
          "missing commit must repair to the newest valid slot");

  MemoryTrustStorage interrupted = storage;
  FirmwareTrustSnapshot stable = repaired;
  auto third = next;
  third.trust_revision = 3;
  interrupted.fail_commit_save = true;
  FirmwareTrustStore interrupted_store;
  require(interrupted_store.update(interrupted, third, &stable) ==
              FirmwareTrustStoreResult::STORAGE_FAILED &&
              stable.generation == 2 && stable.state.trust_revision == 2,
          "commit failure must not advance the in-memory trust snapshot");
  interrupted.fail_commit_save = false;
  FirmwareTrustSnapshot after_failure{};
  require(interrupted_store.load(interrupted, &after_failure) ==
              FirmwareTrustStoreResult::LOADED &&
              after_failure.generation == 2,
          "reboot after commit failure must retain the prior committed trust");

  MemoryTrustStorage slot_save_failed = storage;
  slot_save_failed.fail_slot_save = true;
  stable = repaired;
  require(interrupted_store.update(slot_save_failed, third, &stable) ==
              FirmwareTrustStoreResult::STORAGE_FAILED &&
              stable.generation == 2,
          "slot save failure must retain the committed trust");
  FirmwareTrustSnapshot slot_save_reboot{};
  require(interrupted_store.load(slot_save_failed, &slot_save_reboot) ==
              FirmwareTrustStoreResult::LOADED &&
              slot_save_reboot.generation == 2,
          "reboot after slot save failure must load the committed trust");

  MemoryTrustStorage slot_sync_failed = storage;
  slot_sync_failed.sync_calls = 0;
  slot_sync_failed.fail_sync_call = 1;
  stable = repaired;
  require(interrupted_store.update(slot_sync_failed, third, &stable) ==
              FirmwareTrustStoreResult::STORAGE_FAILED &&
              stable.generation == 2,
          "slot sync failure must not advance the in-memory trust");
  FirmwareTrustSnapshot slot_sync_reboot{};
  require(interrupted_store.load(slot_sync_failed, &slot_sync_reboot) ==
              FirmwareTrustStoreResult::LOADED &&
              slot_sync_reboot.generation == 2,
          "old commit must fence an uncommitted slot after slot sync failure");

  MemoryTrustStorage commit_sync_failed = storage;
  commit_sync_failed.sync_calls = 0;
  commit_sync_failed.fail_sync_call = 2;
  stable = repaired;
  require(interrupted_store.update(commit_sync_failed, third, &stable) ==
              FirmwareTrustStoreResult::STORAGE_FAILED &&
              stable.generation == 2,
          "commit sync failure must report failure without exposing partial RAM state");
  FirmwareTrustSnapshot commit_sync_reboot{};
  const auto commit_sync_result =
      interrupted_store.load(commit_sync_failed, &commit_sync_reboot);
  require(commit_sync_result == FirmwareTrustStoreResult::LOADED &&
              commit_sync_reboot.generation >= 2 &&
              commit_sync_reboot.state.trust_revision >= 2,
          "reboot after commit sync failure must recover an old or fail-forward trust, never corruption");

  MemoryTrustStorage corrupted = storage;
  const uint8_t committed_slot = corrupted.commit.bytes[8];
  corrupted.slots[committed_slot].bytes[40] ^= 0x80;
  FirmwareTrustSnapshot corrupt_snapshot{};
  FirmwareTrustStore corrupt_store;
  require(corrupt_store.load(corrupted, &corrupt_snapshot) ==
              FirmwareTrustStoreResult::CORRUPT &&
              corrupt_snapshot.generation == 0,
          "corrupt committed slot must fail closed instead of resurrecting an old keyring");
  corrupted = storage;
  corrupted.commit.bytes[20] ^= 1;
  require(corrupt_store.load(corrupted, &corrupt_snapshot) ==
              FirmwareTrustStoreResult::CORRUPT,
          "corrupt commit marker must fail closed");

  for (std::size_t index = 0;
       index < FirmwareTrustStorageBlob::ENCODED_SIZE; ++index) {
    corrupted = storage;
    corrupted.slots[committed_slot].bytes[index] ^= 1;
    corrupt_snapshot = {};
    const auto result = corrupt_store.load(corrupted, &corrupt_snapshot);
    require(result != FirmwareTrustStoreResult::LOADED &&
                result != FirmwareTrustStoreResult::COMMIT_REPAIRED &&
                corrupt_snapshot.generation == 0,
            "every committed keyring byte must be covered by fail-closed validation");
  }
  for (std::size_t index = 0;
       index < FirmwareTrustCommitBlob::SIZE; ++index) {
    corrupted = storage;
    corrupted.commit.bytes[index] ^= 1;
    corrupt_snapshot = {};
    require(corrupt_store.load(corrupted, &corrupt_snapshot) ==
                FirmwareTrustStoreResult::CORRUPT &&
                corrupt_snapshot.generation == 0,
            "every commit byte must be covered by fail-closed validation");
  }

  auto rollback = next;
  rollback.trust_revision = 1;
  require(store.update(storage, rollback, &snapshot) ==
              FirmwareTrustStoreResult::ROLLBACK_REJECTED,
          "persistent trust revision rollback must fail");
  auto epoch_rollback = next;
  epoch_rollback.trust_revision = 2;
  epoch_rollback.minimum_security_epoch = 4;
  require(store.update(storage, epoch_rollback, &snapshot) ==
              FirmwareTrustStoreResult::ROLLBACK_REJECTED,
          "persistent security epoch rollback must fail");
  auto root_rebase = next;
  root_rebase.trust_revision = 3;
  root_rebase.minimum_security_epoch = 1;
  require(store.update(storage, root_rebase, &snapshot) ==
              FirmwareTrustStoreResult::UPDATED &&
              snapshot.state.minimum_security_epoch == 1,
          "higher Root-authorized trust revision must permit an epoch rebase");

  MemoryTrustStorage generation_exhausted = storage;
  FirmwareTrustSnapshot exhausted_snapshot = snapshot;
  exhausted_snapshot.generation = 0xffffffffU;
  generation_exhausted.commit.bytes[12] = 0xff;
  generation_exhausted.commit.bytes[13] = 0xff;
  generation_exhausted.commit.bytes[14] = 0xff;
  generation_exhausted.commit.bytes[15] = 0xff;
  write_u32(generation_exhausted.commit.bytes.data() + 20, 0);
  uint32_t commit_checksum = 2166136261U;
  for (std::size_t index = 0; index < 20; ++index) {
    commit_checksum ^= generation_exhausted.commit.bytes[index];
    commit_checksum *= 16777619U;
  }
  write_u32(generation_exhausted.commit.bytes.data() + 20,
            commit_checksum);
  auto exhausted_next = exhausted_snapshot.state;
  exhausted_next.trust_revision++;
  require(store.update(generation_exhausted, exhausted_next,
                       &exhausted_snapshot) ==
              FirmwareTrustStoreResult::INVALID_STATE,
          "trust generation wrap must fail closed");

  MemoryTrustStorage failed_sync;
  failed_sync.fail_sync_call = 1;
  FirmwareTrustSnapshot failed_snapshot{};
  FirmwareTrustStore failed_store;
  require(failed_store.create(failed_sync, initial, &failed_snapshot) ==
              FirmwareTrustStoreResult::STORAGE_FAILED &&
              failed_snapshot.generation == 0,
          "slot sync failure must not expose uncommitted trust");
}

void upload_session_rejects_reordering_mutation_timeout_and_sink_failures() {
  auto raw = package_bytes();
  constexpr uint32_t IMAGE_SIZE = 9;
  write_u32(raw.data() + 24, IMAGE_SIZE);
  auto package = parse(raw);
  const auto state = trust_state();
  const auto root = public_key(0x71);
  const auto root_id = bytes<16>(0x61);
  auto make_verifier = [&]() {
    return verifier_for(package, state.release_keys[0].public_key);
  };
  std::array<uint8_t, FirmwarePackageHeader::HEADER_SIZE + IMAGE_SIZE> wire{};
  std::memcpy(wire.data(), raw.data(), raw.size());
  for (std::size_t index = 0; index < IMAGE_SIZE; ++index)
    wire[FirmwarePackageHeader::HEADER_SIZE + index] =
        static_cast<uint8_t>(0xa0 + index);

  require(classify_firmware_multipart_marker(0, nullptr, 0, false) ==
              FirmwareMultipartMarker::START &&
              classify_firmware_multipart_marker(wire.size(), nullptr, 0,
                                                  true) ==
                  FirmwareMultipartMarker::END &&
              classify_firmware_multipart_marker(0, wire.data(), 1, false) ==
                  FirmwareMultipartMarker::NONE,
          "IDF multipart sentinels must remain distinct from firmware bytes");

  FirmwareUploadSession session;
  MemoryFirmwareSink sink;
  auto verifier = make_verifier();
  require(session.start(raw.data(), raw.size(), 0x1234, 1000, state, root_id, root,
                        verifier) == FirmwareUploadResult::OK,
          "trusted preflight header must open a bounded upload session");
  FirmwareUploadSession oversized_header_session;
  auto oversized_header_verifier = make_verifier();
  std::array<uint8_t, FirmwarePackageHeader::HEADER_SIZE + 1>
      oversized_header{};
  std::memcpy(oversized_header.data(), raw.data(), raw.size());
  require(oversized_header_session.start(
              oversized_header.data(), oversized_header.size(), 0x4321,
              1000, state, root_id, root, oversized_header_verifier) ==
              FirmwareUploadResult::HEADER_MISMATCH,
          "preflight must require an exact 320-byte header");
  require(session.start(raw.data(), raw.size(), 0x5678, 1001, state, root_id, root,
                        verifier) == FirmwareUploadResult::BUSY,
          "concurrent preflight must not replace an active session");
  for (std::size_t index = 0; index < wire.size(); ++index) {
    require(session.consume(0x1234, index, wire.data() + index, 1,
                            index + 1 == wire.size(),
                            static_cast<uint32_t>(1100 + index), sink) ==
                (index + 1 == wire.size() ? FirmwareUploadResult::COMPLETE
                                          : FirmwareUploadResult::OK),
            "all one-byte chunk boundaries must preserve the package");
  }
  require(sink.begin_calls == 1 && sink.finish_calls == 1 &&
              sink.abort_calls == 0 && sink.image_size == IMAGE_SIZE,
          "valid upload must begin, write, and finish exactly once");

  FirmwareUploadSession multipart_session;
  MemoryFirmwareSink multipart_sink;
  auto multipart_verifier = make_verifier();
  constexpr uint32_t MULTIPART_TOKEN = 0x7788;
  require(multipart_session.start(raw.data(), raw.size(), MULTIPART_TOKEN,
                                  1400, state, root_id, root,
                                  multipart_verifier) ==
              FirmwareUploadResult::OK &&
              multipart_session.received_size() == 0,
          "multipart preflight must remain open across the transport start marker");
  require(multipart_session.consume(MULTIPART_TOKEN, 0, wire.data(), 173,
                                    false, 1401, multipart_sink) ==
              FirmwareUploadResult::OK &&
              multipart_session.consume(MULTIPART_TOKEN, 173,
                                        wire.data() + 173,
                                        wire.size() - 173, false, 1402,
                                        multipart_sink) ==
                  FirmwareUploadResult::OK &&
              multipart_session.finish(MULTIPART_TOKEN, wire.size(), 1403,
                                       multipart_sink) ==
                  FirmwareUploadResult::COMPLETE,
          "IDF data callbacks followed by an empty end marker must complete exactly once");
  require(multipart_sink.begin_calls == 1 &&
              multipart_sink.finish_calls == 1 &&
              multipart_sink.abort_calls == 0,
          "multipart completion must not reopen or abort the OTA sink");

  for (std::size_t split = 1; split < wire.size(); ++split) {
    FirmwareUploadSession split_session;
    MemoryFirmwareSink split_sink;
    auto split_verifier = make_verifier();
    const uint32_t token = static_cast<uint32_t>(0x10000 + split);
    require(split_session.start(raw.data(), raw.size(), token, 1500, state,
                                root_id, root, split_verifier) ==
                FirmwareUploadResult::OK,
            "split upload preflight failed");
    require(split_session.consume(token, 0, wire.data(), split, false, 1501,
                                  split_sink) == FirmwareUploadResult::OK &&
                split_session.consume(token, split, wire.data() + split,
                                      wire.size() - split, true, 1502,
                                      split_sink) ==
                    FirmwareUploadResult::COMPLETE &&
                split_sink.image_size == IMAGE_SIZE,
            "every two-chunk split must preserve exact envelope and image boundaries");
  }

  verifier = make_verifier();
  require(session.start(raw.data(), raw.size(), 1, 2000, state, root_id, root,
                        verifier) == FirmwareUploadResult::OK,
          "session restart after completion must succeed");
  auto changed = wire;
  changed[255] ^= 1;
  require(session.consume(1, 0, changed.data(), changed.size(), true, 2001,
                          sink) == FirmwareUploadResult::HEADER_MISMATCH,
          "uploaded header must exactly match the preflight signature input");
  require(sink.abort_calls == 0,
          "header mismatch before OTA begin must not touch the image sink");

  verifier = make_verifier();
  require(session.start(raw.data(), raw.size(), 2, 3000, state, root_id, root,
                        verifier) == FirmwareUploadResult::OK,
          "offset test preflight failed");
  require(session.consume(2, 0, nullptr, 0, false, 3001, sink) ==
              FirmwareUploadResult::OFFSET_MISMATCH,
          "zero-length chunks must not keep an upload session alive");
  verifier = make_verifier();
  require(session.start(raw.data(), raw.size(), 2, 3002, state, root_id, root,
                        verifier) == FirmwareUploadResult::OK,
          "offset test must restart after rejecting a zero-length chunk");
  require(session.consume(2, 1, wire.data(), 8, false, 3003, sink) ==
              FirmwareUploadResult::OFFSET_MISMATCH,
          "out-of-order first chunk must fail");
  verifier = make_verifier();
  require(session.start(raw.data(), raw.size(), 3, 4000, state, root_id, root,
                        verifier) == FirmwareUploadResult::OK,
          "session mismatch preflight failed");
  require(session.consume(4, 0, wire.data(), 8, false, 4001, sink) ==
              FirmwareUploadResult::SESSION_MISMATCH,
          "wrong session token must be rejected");
  require(session.active(),
          "wrong session token must not cancel another client's upload");
  require(!session.cancel(4, sink) && session.active(),
          "wrong cancel token must not cancel another client's upload");
  require(session.cancel(3, sink) && !session.active(),
          "matching cancel token must terminate its upload");

  verifier = make_verifier();
  require(session.start(raw.data(), raw.size(), 5, 5000, state, root_id, root,
                        verifier) == FirmwareUploadResult::OK,
          "truncation preflight failed");
  require(session.consume(5, 0, wire.data(), wire.size() - 1, true, 5001,
                          sink) == FirmwareUploadResult::SIZE_MISMATCH &&
              sink.abort_calls == 1,
          "truncated final chunk must abort an opened sink");
  verifier = make_verifier();
  require(session.start(raw.data(), raw.size(), 6, 6000, state, root_id, root,
                        verifier) == FirmwareUploadResult::OK,
          "overflow preflight failed");
  std::array<uint8_t, FirmwarePackageHeader::HEADER_SIZE + IMAGE_SIZE + 1>
      oversized{};
  require(session.consume(6, 0, oversized.data(), oversized.size(), true,
                          6001, sink) == FirmwareUploadResult::SIZE_MISMATCH,
          "oversized upload must fail before writing flash");

  verifier = make_verifier();
  require(session.start(raw.data(), raw.size(), 7, 7000, state, root_id, root,
                        verifier) == FirmwareUploadResult::OK &&
              session.expire(7000 + FirmwareUploadSession::TIMEOUT_MS + 1,
                             sink),
          "inactive upload must expire on a bounded deadline");
  require(!session.active(), "expired session must release its token");

  verifier = make_verifier();
  sink.begin_ok = false;
  require(session.start(raw.data(), raw.size(), 8, 8000, state, root_id, root,
                        verifier) == FirmwareUploadResult::OK &&
              session.consume(8, 0, wire.data(), wire.size(), true, 8001,
                              sink) ==
                  FirmwareUploadResult::SINK_BEGIN_FAILED,
          "OTA begin failure must be explicit");
  sink.begin_ok = true;
  sink.write_ok = false;
  verifier = make_verifier();
  require(session.start(raw.data(), raw.size(), 9, 9000, state, root_id, root,
                        verifier) == FirmwareUploadResult::OK &&
              session.consume(9, 0, wire.data(), wire.size(), true, 9001,
                              sink) ==
                  FirmwareUploadResult::SINK_WRITE_FAILED,
          "OTA write failure must abort the session");
  sink.write_ok = true;
  sink.finish_ok = false;
  verifier = make_verifier();
  require(session.start(raw.data(), raw.size(), 10, 10000, state, root_id, root,
                        verifier) == FirmwareUploadResult::OK &&
              session.consume(10, 0, wire.data(), wire.size(), true, 10001,
                              sink) ==
                  FirmwareUploadResult::SINK_FINISH_FAILED,
          "OTA finalize failure must abort the session");
}

}  // namespace

void run_firmware_update_security_tests() {
  FirmwareDeveloperInstallGate developer_gate;
  require(!developer_gate.allowed(1000) &&
              developer_gate.remaining_seconds(1000) == 0,
          "developer install gate must start disabled");
  developer_gate.set_enabled(true, 1000);
  require(developer_gate.allowed(1000) &&
              developer_gate.remaining_seconds(1000) == 600,
          "developer install gate must expose its ten minute window");
  require(developer_gate.allowed(
              1000 + FirmwareDeveloperInstallGate::WINDOW_MS - 1) &&
              developer_gate.remaining_seconds(
                  1000 + FirmwareDeveloperInstallGate::WINDOW_MS - 1) == 1,
          "developer install gate must remain valid through its final millisecond");
  require(!developer_gate.allowed(
              1000 + FirmwareDeveloperInstallGate::WINDOW_MS) &&
              developer_gate.remaining_seconds(
                  1000 + FirmwareDeveloperInstallGate::WINDOW_MS) == 0,
          "developer install gate must expire at its bounded deadline");
  developer_gate.set_enabled(true, 0xffffffffU - 1000U);
  require(developer_gate.allowed(500U),
          "developer install gate must survive millis rollover");
  developer_gate.consume();
  require(!developer_gate.allowed(501U),
          "developer install gate must be consumed after one successful install");
  package_parser_rejects_every_bounded_contract_violation();
  trust_evaluation_enforces_role_signature_and_rollback_fences();
  root_recovery_rotates_release_key_without_private_material();
  root_recovery_rotates_developer_key_independently();
  trust_store_survives_interrupted_updates_and_fails_closed_on_corruption();
  upload_session_rejects_reordering_mutation_timeout_and_sink_failures();
}
