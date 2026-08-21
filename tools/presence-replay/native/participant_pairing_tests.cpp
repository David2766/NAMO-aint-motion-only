#include "participant_pairing.h"
#include "participant_pairing_runtime.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace esphome::radar_api_server;

namespace {

void require(bool condition, const char *message) {
  if (condition)
    return;
  std::fprintf(stderr, "participant_pairing_tests: %s\n", message);
  std::exit(1);
}

class FakePairingCrypto : public ParticipantPairingCrypto {
 public:
  explicit FakePairingCrypto(uint8_t seed = 1) : next_(seed) {}

  bool random_bytes(uint8_t *output, std::size_t size) override {
    if (output == nullptr)
      return false;
    for (std::size_t index = 0; index < size; ++index)
      output[index] = static_cast<uint8_t>(next_ + index + 1);
    next_ = static_cast<uint8_t>(next_ + size + 7);
    return true;
  }

  bool generate_key_pair(
      std::array<uint8_t, ParticipantPairingOffer::PRIVATE_KEY_SIZE> *private_key,
      std::array<uint8_t, ParticipantPairingOffer::PUBLIC_KEY_SIZE> *public_key) override {
    if (private_key == nullptr || public_key == nullptr ||
        !this->random_bytes(private_key->data(), private_key->size()))
      return false;
    public_key->fill(0);
    (*public_key)[0] = 4;
    for (std::size_t index = 0; index < private_key->size(); ++index) {
      (*public_key)[index + 1] = (*private_key)[index];
      (*public_key)[index + 33] = (*private_key)[private_key->size() - index - 1];
    }
    return true;
  }

  bool derive_shared_secret(
      const std::array<uint8_t, ParticipantPairingOffer::PRIVATE_KEY_SIZE> &private_key,
      const std::array<uint8_t, ParticipantPairingOffer::PUBLIC_KEY_SIZE> &peer_public_key,
      std::array<uint8_t, 32> *shared_secret) override {
    if (shared_secret == nullptr || peer_public_key[0] != 4)
      return false;
    for (std::size_t index = 0; index < shared_secret->size(); ++index)
      (*shared_secret)[index] = private_key[index] ^ peer_public_key[index + 1];
    return true;
  }

  bool sha256(const uint8_t *data, std::size_t size,
              std::array<uint8_t, 32> *digest) override {
    return this->digest_(nullptr, data, size, digest);
  }

  bool hmac_sha256(const std::array<uint8_t, 32> &key, const uint8_t *data,
                   std::size_t size, std::array<uint8_t, 32> *digest) override {
    return this->digest_(&key, data, size, digest);
  }

  bool hkdf_sha256(const std::array<uint8_t, 32> &input_key,
                   const std::array<uint8_t, 32> &salt, const uint8_t *info,
                   std::size_t info_size,
                   std::array<uint8_t, 32> *output_key) override {
    std::array<uint8_t, 32> mixed{};
    for (std::size_t index = 0; index < mixed.size(); ++index)
      mixed[index] = input_key[index] ^ salt[index];
    return this->digest_(&mixed, info, info_size, output_key);
  }

 private:
  uint8_t next_{1};

  bool digest_(const std::array<uint8_t, 32> *key, const uint8_t *data,
               std::size_t size, std::array<uint8_t, 32> *digest) {
    if (data == nullptr || digest == nullptr)
      return false;
    uint32_t value = 2166136261U;
    if (key != nullptr) {
      for (uint8_t byte : *key) {
        value ^= byte;
        value *= 16777619U;
      }
    }
    for (std::size_t index = 0; index < size; ++index) {
      value ^= data[index];
      value *= 16777619U;
    }
    for (std::size_t index = 0; index < digest->size(); ++index) {
      value ^= static_cast<uint32_t>(index + 1U) * 0x9E3779B9U;
      value *= 16777619U;
      (*digest)[index] = static_cast<uint8_t>(value >> ((index % 4U) * 8U));
    }
    return true;
  }
};

PlatformIdentity identity(const char *site, const char *node) {
  PlatformIdentity value{};
  std::strncpy(value.site_id, site, PlatformIdentity::MAX_ID_LENGTH);
  std::strncpy(value.node_id, node, PlatformIdentity::MAX_ID_LENGTH);
  return value;
}

void complete_exchange(FakePairingCrypto &leader_crypto,
                       FakePairingCrypto &joiner_crypto,
                       ParticipantPairingSession *leader,
                       ParticipantPairingSession *joiner) {
  require(leader->exchange(leader_crypto, joiner->local_offer(), 1100) ==
              ParticipantPairingResult::OK,
          "leader exchange should succeed");
  require(joiner->exchange(joiner_crypto, leader->local_offer(), 1100) ==
              ParticipantPairingResult::OK,
          "joiner exchange should succeed");
  require(leader->confirmation_code() == joiner->confirmation_code(),
          "both peers must derive the same confirmation code");
  require(leader->verify_peer_proof(joiner->local_proof(), 1200) ==
              ParticipantPairingResult::OK &&
              joiner->verify_peer_proof(leader->local_proof(), 1200) ==
                  ParticipantPairingResult::OK,
          "both peers must prove possession of the transcript key");
}

void symmetric_exchange_derives_matching_pair_key() {
  FakePairingCrypto leader_crypto(1);
  FakePairingCrypto joiner_crypto(91);
  ParticipantPairingSession leader;
  ParticipantPairingSession joiner;
  require(leader.prepare(leader_crypto, ParticipantPairingRole::LEADER,
                         identity("site-main", "namo-node-leader"), 1000,
                         120000) == ParticipantPairingResult::OK &&
              joiner.prepare(joiner_crypto, ParticipantPairingRole::JOINER,
                             identity("site-temporary", "namo-node-joiner"),
                             1000, 120000) == ParticipantPairingResult::OK,
          "both peers should prepare bounded sessions");
  complete_exchange(leader_crypto, joiner_crypto, &leader, &joiner);

  ParticipantPairingCredential leader_view{};
  ParticipantPairingCredential joiner_view{};
  require(leader.credential(leader_crypto, leader.confirmation_code(), 1300,
                            &leader_view) == ParticipantPairingResult::OK &&
              joiner.credential(joiner_crypto, joiner.confirmation_code(), 1300,
                                &joiner_view) == ParticipantPairingResult::OK,
          "verified sessions should expose persistence material");
  require(leader_view.key == joiner_view.key,
          "both peers must persist the same pairwise observation key");
  require(std::strcmp(leader_view.participant_id, "namo-node-joiner") == 0 &&
              std::strcmp(joiner_view.participant_id, "namo-node-leader") == 0,
          "each peer must store the opposite participant identity");
  require(leader_view.participant_token == joiner_view.participant_token,
          "both peers must persist the same pairwise transport token");
}

void mismatched_proof_and_confirmation_fail_closed() {
  FakePairingCrypto leader_crypto(1);
  FakePairingCrypto joiner_crypto(91);
  ParticipantPairingSession leader;
  ParticipantPairingSession joiner;
  leader.prepare(leader_crypto, ParticipantPairingRole::LEADER,
                 identity("site-main", "leader"), 10, 1000);
  joiner.prepare(joiner_crypto, ParticipantPairingRole::JOINER,
                 identity("site-other", "joiner"), 10, 1000);
  leader.exchange(leader_crypto, joiner.local_offer(), 20);
  joiner.exchange(joiner_crypto, leader.local_offer(), 20);

  auto wrong_proof = joiner.local_proof();
  wrong_proof[0] ^= 0x80;
  require(leader.verify_peer_proof(wrong_proof, 30) ==
              ParticipantPairingResult::PROOF_MISMATCH &&
              leader.state() == ParticipantPairingState::EXCHANGED,
          "wrong peer proof must not advance the session");
  require(leader.verify_peer_proof(joiner.local_proof(), 30) ==
              ParticipantPairingResult::OK,
          "correct proof should remain retryable");
  ParticipantPairingCredential credential{};
  require(leader.credential(leader_crypto,
                            (leader.confirmation_code() + 1) % 1000000U, 40,
                            &credential) ==
              ParticipantPairingResult::CONFIRMATION_MISMATCH,
          "wrong user confirmation must not release credential material");
}

void expiry_and_role_conflict_clear_or_reject_sessions() {
  FakePairingCrypto first_crypto(1);
  FakePairingCrypto second_crypto(91);
  ParticipantPairingSession first;
  ParticipantPairingSession second;
  first.prepare(first_crypto, ParticipantPairingRole::LEADER,
                identity("site-a", "node-a"), 100, 100);
  second.prepare(second_crypto, ParticipantPairingRole::LEADER,
                 identity("site-b", "node-b"), 100, 100);
  require(first.exchange(first_crypto, second.local_offer(), 150) ==
              ParticipantPairingResult::ROLE_CONFLICT,
          "two leaders must not create an ambiguous transcript");
  require(!first.active(200) && first.state() == ParticipantPairingState::IDLE,
          "expired session must wipe itself back to idle");
}

void runtime_defers_crypto_and_commit_outside_request_context() {
  FakePairingCrypto leader_crypto(1);
  FakePairingCrypto joiner_crypto(91);
  ParticipantPairingRuntime leader;
  ParticipantPairingRuntime joiner;
  const auto leader_identity = identity("site-main", "namo-node-leader");
  const auto joiner_identity = identity("site-temporary", "namo-node-joiner");
  uint32_t leader_operation = 0;
  uint32_t joiner_operation = 0;
  ParticipantPairingPendingCommit ignored{};

  require(leader.queue_prepare(ParticipantPairingRole::LEADER, 120000, 1000,
                               &leader_operation) &&
              joiner.queue_prepare(ParticipantPairingRole::JOINER, 120000, 1000,
                                   &joiner_operation),
          "prepare commands should enter the fixed one-slot inbox");
  uint32_t rejected_operation = 0;
  require(!leader.queue_cancel(&rejected_operation),
          "a second request must be rejected while one is pending");
  require(leader.snapshot(1000).state == ParticipantPairingState::IDLE &&
              leader.snapshot(1000).busy,
          "queued work must not execute in the request context");
  require(!leader.process(leader_crypto, leader_identity, 1000, &ignored) &&
              !joiner.process(joiner_crypto, joiner_identity, 1000, &ignored),
          "prepare does not request persistence");

  const auto leader_prepared = leader.snapshot(1000);
  const auto joiner_prepared = joiner.snapshot(1000);
  require(leader_prepared.state == ParticipantPairingState::PREPARED &&
              joiner_prepared.state == ParticipantPairingState::PREPARED &&
              !leader_prepared.busy && !joiner_prepared.busy,
          "main-loop processing must publish prepared state");
  leader.expire(1500);
  require(leader.snapshot(1500).remaining_ms ==
              leader_prepared.remaining_ms,
          "active pairing countdown publication must be rate limited");
  leader.expire(2000);
  require(leader.snapshot(2000).remaining_ms <
              leader_prepared.remaining_ms,
          "active pairing countdown must still refresh once per second");
  require(leader.queue_exchange(joiner_prepared.offer, &leader_operation) &&
              joiner.queue_exchange(leader_prepared.offer, &joiner_operation),
          "peer offers should queue without heap-backed state");
  leader.process(leader_crypto, leader_identity, 2100, &ignored);
  joiner.process(joiner_crypto, joiner_identity, 2100, &ignored);

  const auto leader_exchanged = leader.snapshot(2100);
  const auto joiner_exchanged = joiner.snapshot(2100);
  require(leader_exchanged.confirmation_code ==
              joiner_exchanged.confirmation_code &&
              leader_exchanged.has_proof && joiner_exchanged.has_proof,
          "runtime snapshots must expose matching SAS and public proofs");
  require(leader.queue_verify(joiner_exchanged.proof, &leader_operation) &&
              joiner.queue_verify(leader_exchanged.proof, &joiner_operation),
          "proof verification should queue");
  leader.process(leader_crypto, leader_identity, 2200, &ignored);
  joiner.process(joiner_crypto, joiner_identity, 2200, &ignored);

  require(leader.queue_confirm(leader_exchanged.confirmation_code,
                               &leader_operation) &&
              joiner.queue_confirm(joiner_exchanged.confirmation_code,
                                   &joiner_operation),
          "matching user confirmations should queue");
  ParticipantPairingPendingCommit leader_commit{};
  ParticipantPairingPendingCommit joiner_commit{};
  require(leader.process(leader_crypto, leader_identity, 2300,
                         &leader_commit) &&
              joiner.process(joiner_crypto, joiner_identity, 2300,
                             &joiner_commit),
          "confirm must hand persistence back to the component loop");
  require(leader_commit.credential.key == joiner_commit.credential.key &&
              leader_commit.credential.participant_token ==
                  joiner_commit.credential.participant_token,
          "deferred commits must carry the same pairwise credential");
  leader.complete_commit(ParticipantPairingCommitResult::OK, 2300);
  joiner.complete_commit(ParticipantPairingCommitResult::OK, 2300);
  require(leader.snapshot(2300).state == ParticipantPairingState::COMMITTED &&
              joiner.snapshot(2300).state == ParticipantPairingState::COMMITTED,
          "successful persistence must be the only path to committed state");
}

void runtime_bounds_connection_invitations_without_starting_pairing() {
  ParticipantPairingRuntime runtime;
  require(runtime.set_invitation("192.168.1.40", 1000, 60000),
          "a valid connection invitation should be retained in RAM");
  auto invitation = runtime.snapshot(1000);
  require(invitation.has_invitation && !invitation.invitation_rejected &&
              invitation.invitation_remaining_ms == 60000 &&
              std::strcmp(invitation.invitation_requester_address,
                          "192.168.1.40") == 0 &&
              invitation.state == ParticipantPairingState::IDLE,
          "an invitation must not open or advance the cryptographic session");
  require(!runtime.set_invitation("192.168.1.41", 1100, 60000),
          "a different requester must not replace a live invitation");
  require(runtime.reject_invitation(1200),
          "the local user should be able to reject a live invitation");
  invitation = runtime.snapshot(1200);
  require(invitation.has_invitation && invitation.invitation_rejected &&
              invitation.invitation_remaining_ms == 15000,
          "rejection should remain visible briefly to the requesting wizard");
  require(!runtime.snapshot(16201).has_invitation,
          "the rejected invitation must expire without persistence");

  FakePairingCrypto crypto(1);
  ParticipantPairingPendingCommit ignored{};
  uint32_t operation_id = 0;
  require(runtime.set_invitation("192.168.1.40", 20000, 60000) &&
              runtime.queue_prepare(ParticipantPairingRole::JOINER, 120000,
                                    20000,
                                    &operation_id),
          "accepting an invitation should queue the existing pairing flow");
  runtime.process(crypto, identity("site-a", "namo-node-a"), 20000, &ignored);
  require(!runtime.snapshot(20000).has_invitation &&
              runtime.snapshot(20000).state ==
                  ParticipantPairingState::PREPARED,
          "successful prepare must clear the prompt and publish normal pairing");
}

void runtime_rate_limits_repeated_prepare_crypto() {
  ParticipantPairingRuntime runtime;
  FakePairingCrypto crypto(1);
  ParticipantPairingPendingCommit ignored{};
  uint32_t operation_id = 0;
  const auto local = identity("site-a", "namo-node-a");

  require(runtime.queue_prepare(ParticipantPairingRole::LEADER, 120000, 1000,
                                &operation_id),
          "the first prepare should be accepted");
  runtime.process(crypto, local, 1000, &ignored);
  require(runtime.queue_cancel(&operation_id),
          "the active pairing session should be cancellable");
  runtime.process(crypto, local, 1100, &ignored);
  require(!runtime.queue_prepare(ParticipantPairingRole::LEADER, 120000, 1500,
                                 &operation_id) &&
              runtime.snapshot(1500).prepare_rate_limited == 1,
          "rapid prepare churn must not repeat public-key work");
  require(runtime.queue_prepare(ParticipantPairingRole::LEADER, 120000, 2000,
                                &operation_id),
          "the bounded cooldown must not lock out normal pairing");
}

struct RuntimePairFixture {
  FakePairingCrypto leader_crypto{1};
  FakePairingCrypto joiner_crypto{91};
  ParticipantPairingRuntime leader{};
  ParticipantPairingRuntime joiner{};
  PlatformIdentity leader_identity{identity("site-main", "namo-node-leader")};
  PlatformIdentity joiner_identity{identity("site-temporary", "namo-node-joiner")};
  ParticipantPairingPendingCommit ignored{};
  uint32_t operation_id{0};

  void prepare(uint32_t now_ms = 1000, uint32_t duration_ms = 120000) {
    require(leader.queue_prepare(ParticipantPairingRole::LEADER, duration_ms,
                                 now_ms, &operation_id) &&
                joiner.queue_prepare(ParticipantPairingRole::JOINER,
                                     duration_ms, now_ms, &operation_id),
            "runtime pair prepare was rejected");
    require(!leader.process(leader_crypto, leader_identity, now_ms, &ignored) &&
                !joiner.process(joiner_crypto, joiner_identity, now_ms,
                                &ignored),
            "prepare must not request persistence");
  }

  void exchange(uint32_t now_ms = 1100) {
    const auto leader_status = leader.snapshot(now_ms);
    const auto joiner_status = joiner.snapshot(now_ms);
    require(leader.queue_exchange(joiner_status.offer, &operation_id) &&
                joiner.queue_exchange(leader_status.offer, &operation_id),
            "runtime pair exchange was rejected");
    require(!leader.process(leader_crypto, leader_identity, now_ms, &ignored) &&
                !joiner.process(joiner_crypto, joiner_identity, now_ms,
                                &ignored),
            "exchange must not request persistence");
  }

  void verify(uint32_t now_ms = 1200) {
    const auto leader_status = leader.snapshot(now_ms);
    const auto joiner_status = joiner.snapshot(now_ms);
    require(leader.queue_verify(joiner_status.proof, &operation_id) &&
                joiner.queue_verify(leader_status.proof, &operation_id),
            "runtime pair verification was rejected");
    require(!leader.process(leader_crypto, leader_identity, now_ms, &ignored) &&
                !joiner.process(joiner_crypto, joiner_identity, now_ms,
                                &ignored),
            "verification must not request persistence");
  }

  void advance_to(ParticipantPairingState state) {
    if (state == ParticipantPairingState::IDLE)
      return;
    prepare();
    if (state == ParticipantPairingState::PREPARED)
      return;
    exchange();
    if (state == ParticipantPairingState::EXCHANGED)
      return;
    verify();
    if (state == ParticipantPairingState::VERIFIED)
      return;
    const auto leader_status = leader.snapshot(1250);
    const auto joiner_status = joiner.snapshot(1250);
    require(leader.queue_confirm(leader_status.confirmation_code,
                                 &operation_id) &&
                joiner.queue_confirm(joiner_status.confirmation_code,
                                     &operation_id),
            "runtime pair confirm was rejected");
    ParticipantPairingPendingCommit leader_commit{};
    ParticipantPairingPendingCommit joiner_commit{};
    require(leader.process(leader_crypto, leader_identity, 1300,
                           &leader_commit) &&
                joiner.process(joiner_crypto, joiner_identity, 1300,
                               &joiner_commit),
            "runtime pair confirm did not request persistence");
    leader.complete_commit(ParticipantPairingCommitResult::OK, 1300);
    joiner.complete_commit(ParticipantPairingCommitResult::OK, 1300);
  }
};

void runtime_cancel_matrix_clears_every_uncommitted_stage() {
  const std::array<ParticipantPairingState, 4> stages{
      ParticipantPairingState::IDLE,
      ParticipantPairingState::PREPARED,
      ParticipantPairingState::EXCHANGED,
      ParticipantPairingState::VERIFIED,
  };
  for (const auto stage : stages) {
    RuntimePairFixture fixture{};
    fixture.advance_to(stage);
    require(fixture.leader.queue_cancel(&fixture.operation_id) &&
                fixture.joiner.queue_cancel(&fixture.operation_id),
            "cancel must be accepted at every uncommitted stage");
    require(!fixture.leader.process(fixture.leader_crypto,
                                    fixture.leader_identity, 1300,
                                    &fixture.ignored) &&
                !fixture.joiner.process(fixture.joiner_crypto,
                                        fixture.joiner_identity, 1300,
                                        &fixture.ignored),
            "cancel must never request persistence");
    const auto leader = fixture.leader.snapshot(1300);
    const auto joiner = fixture.joiner.snapshot(1300);
    require(leader.state == ParticipantPairingState::IDLE &&
                joiner.state == ParticipantPairingState::IDLE &&
                !leader.busy && !joiner.busy && !leader.has_offer &&
                !joiner.has_offer && !leader.has_proof && !joiner.has_proof &&
                leader.peer_participant_id[0] == '\0' &&
                joiner.peer_participant_id[0] == '\0',
            "cancel left transient pairing material visible");

    require(fixture.leader.queue_prepare(ParticipantPairingRole::LEADER,
                                         120000, 3000,
                                         &fixture.operation_id) &&
                fixture.joiner.queue_prepare(ParticipantPairingRole::JOINER,
                                             120000, 3000,
                                             &fixture.operation_id),
            "cancelled runtime did not admit a later clean retry");
  }
}

void runtime_operation_state_cross_product_is_deterministic() {
  const std::array<ParticipantPairingState, 5> stages{
      ParticipantPairingState::IDLE,
      ParticipantPairingState::PREPARED,
      ParticipantPairingState::EXCHANGED,
      ParticipantPairingState::VERIFIED,
      ParticipantPairingState::COMMITTED,
  };
  const std::array<ParticipantPairingOperation, 5> operations{
      ParticipantPairingOperation::PREPARE,
      ParticipantPairingOperation::EXCHANGE,
      ParticipantPairingOperation::VERIFY,
      ParticipantPairingOperation::CONFIRM,
      ParticipantPairingOperation::CANCEL,
  };

  for (const auto stage : stages) {
    for (const auto operation : operations) {
      RuntimePairFixture fixture{};
      fixture.advance_to(stage);
      const auto before = fixture.leader.snapshot(3000);
      const auto peer = fixture.joiner.snapshot(3000);
      bool queued = false;
      switch (operation) {
        case ParticipantPairingOperation::PREPARE:
          queued = fixture.leader.queue_prepare(
              ParticipantPairingRole::LEADER, 120000, 3000,
              &fixture.operation_id);
          break;
        case ParticipantPairingOperation::EXCHANGE:
          queued = fixture.leader.queue_exchange(peer.offer,
                                                 &fixture.operation_id);
          break;
        case ParticipantPairingOperation::VERIFY:
          queued = fixture.leader.queue_verify(peer.proof,
                                               &fixture.operation_id);
          break;
        case ParticipantPairingOperation::CONFIRM:
          queued = fixture.leader.queue_confirm(before.confirmation_code,
                                                &fixture.operation_id);
          break;
        case ParticipantPairingOperation::CANCEL:
          queued = fixture.leader.queue_cancel(&fixture.operation_id);
          break;
        case ParticipantPairingOperation::NONE:
          break;
      }

      const bool valid = operation == ParticipantPairingOperation::CANCEL ||
                         (operation == ParticipantPairingOperation::PREPARE &&
                          (stage == ParticipantPairingState::IDLE ||
                           stage == ParticipantPairingState::COMMITTED)) ||
                         (operation == ParticipantPairingOperation::EXCHANGE &&
                          stage == ParticipantPairingState::PREPARED) ||
                         (operation == ParticipantPairingOperation::VERIFY &&
                          stage == ParticipantPairingState::EXCHANGED) ||
                         (operation == ParticipantPairingOperation::CONFIRM &&
                          stage == ParticipantPairingState::VERIFIED);
      if (operation == ParticipantPairingOperation::PREPARE && !valid) {
        require(!queued,
                "prepare entered a runtime state that must reject new crypto work");
        require(fixture.leader.snapshot(3000).state == before.state,
                "rejected prepare changed the pairing state");
        continue;
      }

      require(queued, "runtime rejected an operation before state validation");
      ParticipantPairingPendingCommit commit{};
      const bool commit_requested = fixture.leader.process(
          fixture.leader_crypto, fixture.leader_identity, 3000, &commit);
      const auto after = fixture.leader.snapshot(3000);
      if (!valid) {
        require(!commit_requested &&
                    after.operation_result ==
                        ParticipantPairingResult::INVALID_STATE &&
                    after.state == before.state && !after.busy,
                "invalid operation advanced or committed the pairing state");
        continue;
      }
      require(after.operation_result == ParticipantPairingResult::OK,
              "valid operation failed its state contract");
      require(commit_requested ==
                  (operation == ParticipantPairingOperation::CONFIRM),
              "only a valid confirm may request persistence");
      if (commit_requested)
        fixture.leader.complete_commit(ParticipantPairingCommitResult::OK,
                                       3000);
    }
  }
}

void runtime_expiry_matrix_wipes_every_uncommitted_stage() {
  const std::array<ParticipantPairingState, 3> stages{
      ParticipantPairingState::PREPARED,
      ParticipantPairingState::EXCHANGED,
      ParticipantPairingState::VERIFIED,
  };
  for (const auto stage : stages) {
    RuntimePairFixture fixture{};
    fixture.prepare(1000, 250);
    if (stage >= ParticipantPairingState::EXCHANGED)
      fixture.exchange(1100);
    if (stage >= ParticipantPairingState::VERIFIED)
      fixture.verify(1200);
    fixture.leader.expire(1250);
    fixture.joiner.expire(1250);
    const auto leader = fixture.leader.snapshot(1250);
    const auto joiner = fixture.joiner.snapshot(1250);
    require(leader.state == ParticipantPairingState::IDLE &&
                joiner.state == ParticipantPairingState::IDLE &&
                !leader.has_offer && !joiner.has_offer && !leader.has_proof &&
                !joiner.has_proof && leader.remaining_ms == 0 &&
                joiner.remaining_ms == 0,
            "expired pairing stage retained transient material");
  }
}

void runtime_one_slot_inbox_rejects_duplicate_clicks_at_every_stage() {
  const std::array<ParticipantPairingState, 5> stages{
      ParticipantPairingState::IDLE,
      ParticipantPairingState::PREPARED,
      ParticipantPairingState::EXCHANGED,
      ParticipantPairingState::VERIFIED,
      ParticipantPairingState::COMMITTED,
  };
  for (const auto stage : stages) {
    RuntimePairFixture fixture{};
    fixture.advance_to(stage);
    const auto peer = fixture.joiner.snapshot(3000);
    bool first_queued = false;
    switch (stage) {
      case ParticipantPairingState::IDLE:
      case ParticipantPairingState::COMMITTED:
        first_queued = fixture.leader.queue_prepare(
            ParticipantPairingRole::LEADER, 120000, 3000,
            &fixture.operation_id);
        break;
      case ParticipantPairingState::PREPARED:
        first_queued = fixture.leader.queue_exchange(peer.offer,
                                                     &fixture.operation_id);
        break;
      case ParticipantPairingState::EXCHANGED:
        first_queued = fixture.leader.queue_verify(peer.proof,
                                                   &fixture.operation_id);
        break;
      case ParticipantPairingState::VERIFIED:
        first_queued = fixture.leader.queue_confirm(
            fixture.leader.snapshot(3000).confirmation_code,
            &fixture.operation_id);
        break;
    }
    require(first_queued,
            "duplicate-click fixture could not queue its first operation");
    uint32_t duplicate_operation_id = 0;
    require(!fixture.leader.queue_cancel(&duplicate_operation_id) &&
                !fixture.leader.queue_verify(
                    std::array<uint8_t,
                               ParticipantPairingOffer::PROOF_SIZE>{},
                    &duplicate_operation_id),
            "one-slot inbox admitted a competing click while busy");
    ParticipantPairingPendingCommit pending{};
    const bool commit_requested = fixture.leader.process(
        fixture.leader_crypto, fixture.leader_identity, 3000, &pending);
    require(commit_requested ==
                (stage == ParticipantPairingState::VERIFIED),
            "duplicate-click processing changed commit behavior");
    if (commit_requested)
      fixture.leader.complete_commit(ParticipantPairingCommitResult::OK,
                                     3000);
  }
}

void reboot_drops_only_uncommitted_runtime_state() {
  const std::array<ParticipantPairingState, 4> stages{
      ParticipantPairingState::IDLE,
      ParticipantPairingState::PREPARED,
      ParticipantPairingState::EXCHANGED,
      ParticipantPairingState::VERIFIED,
  };
  for (const auto stage : stages) {
    RuntimePairFixture before{};
    before.advance_to(stage);
    ParticipantPairingRuntime rebooted{};
    const auto snapshot = rebooted.snapshot(5000);
    require(snapshot.state == ParticipantPairingState::IDLE &&
                !snapshot.busy && !snapshot.has_offer &&
                !snapshot.has_proof && !snapshot.peer_verified &&
                snapshot.peer_participant_id[0] == '\0' &&
                snapshot.leader_site_id[0] == '\0',
            "reboot restored an uncommitted in-RAM pairing session");
  }
}

void runtime_failed_commit_stays_retryable_without_false_success() {
  RuntimePairFixture fixture{};
  fixture.advance_to(ParticipantPairingState::VERIFIED);
  const auto verified = fixture.leader.snapshot(1250);
  require(fixture.leader.queue_confirm(verified.confirmation_code,
                                       &fixture.operation_id),
          "verified runtime did not accept confirm");
  ParticipantPairingPendingCommit pending{};
  require(fixture.leader.process(fixture.leader_crypto,
                                 fixture.leader_identity, 1300, &pending),
          "confirm did not produce a deferred commit");
  require(!fixture.leader.queue_cancel(&fixture.operation_id),
          "a persistence-pending confirm must reject competing commands");
  fixture.leader.complete_commit(
      ParticipantPairingCommitResult::PERSISTENCE_FAILED, 1300);
  auto failed = fixture.leader.snapshot(1300);
  require(failed.state == ParticipantPairingState::VERIFIED &&
              failed.commit_result ==
                  ParticipantPairingCommitResult::PERSISTENCE_FAILED &&
              !failed.busy,
          "failed persistence was exposed as committed or left busy");

  require(fixture.leader.queue_confirm(failed.confirmation_code,
                                       &fixture.operation_id) &&
              fixture.leader.process(fixture.leader_crypto,
                                     fixture.leader_identity, 1400, &pending),
          "failed persistence did not allow an idempotent confirm retry");
  fixture.leader.complete_commit(ParticipantPairingCommitResult::OK, 1400);
  require(fixture.leader.snapshot(1400).state ==
              ParticipantPairingState::COMMITTED,
          "successful commit retry did not publish committed state");
}

void invitation_deadlines_survive_uint32_wraparound() {
  ParticipantPairingRuntime runtime{};
  const uint32_t opened_at = UINT32_MAX - 100U;
  require(runtime.set_invitation("192.168.1.40", opened_at, 200U),
          "wraparound invitation fixture was rejected");
  require(runtime.snapshot(UINT32_MAX - 1U).has_invitation &&
              runtime.snapshot(50U).has_invitation &&
              !runtime.snapshot(100U).has_invitation,
          "invitation deadline was not wraparound-safe");
}

void commit_results_distinguish_site_state_from_storage_failure() {
  require(std::strcmp(participant_pairing_commit_result_to_string(
                          ParticipantPairingCommitResult::
                              SITE_CONFIGURATION_OWNER_REQUIRED),
                      "site_configuration_owner_required") == 0,
          "a non-owner pairing attempt needs a distinct actionable result");
  require(std::strcmp(participant_pairing_commit_result_to_string(
                          ParticipantPairingCommitResult::SITE_STATE_INVALID),
                      "site_state_invalid") == 0,
          "invalid Site state must not be reported as persistence failure");
  require(std::strcmp(participant_pairing_commit_result_to_string(
                          ParticipantPairingCommitResult::PERSISTENCE_FAILED),
                      "persistence_failed") == 0,
          "storage failure result changed unexpectedly");
}

}  // namespace

void run_participant_pairing_tests() {
  symmetric_exchange_derives_matching_pair_key();
  mismatched_proof_and_confirmation_fail_closed();
  expiry_and_role_conflict_clear_or_reject_sessions();
  runtime_defers_crypto_and_commit_outside_request_context();
  runtime_bounds_connection_invitations_without_starting_pairing();
  runtime_rate_limits_repeated_prepare_crypto();
  runtime_cancel_matrix_clears_every_uncommitted_stage();
  runtime_operation_state_cross_product_is_deterministic();
  runtime_expiry_matrix_wipes_every_uncommitted_stage();
  runtime_one_slot_inbox_rejects_duplicate_clicks_at_every_stage();
  reboot_drops_only_uncommitted_runtime_state();
  runtime_failed_commit_stays_retryable_without_false_success();
  invitation_deadlines_survive_uint32_wraparound();
  commit_results_distinguish_site_state_from_storage_failure();
}
