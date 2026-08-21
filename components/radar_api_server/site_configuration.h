#pragma once

#include "participant_registry.h"
#include "participant_readiness.h"
#include "platform_identity.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace esphome {
namespace radar_api_server {

class RadarStorage;
class ParticipantCredentialRegistry;
struct SiteDirectorySnapshot;

struct SiteSpatialProfile {
  bool committed{false};
  char space_id[ParticipantReadinessConfig::MAX_ASSIGNMENT_ID_LENGTH + 1]{};
  char group_id[ParticipantReadinessConfig::MAX_ASSIGNMENT_ID_LENGTH + 1]{};
  uint32_t pose_version{0};
  uint32_t calibration_version{0};
  SpatialTransform2D transform{};
};

struct SiteConfigurationMember {
  bool occupied{false};
  char participant_id[ParticipantRecord::MAX_ID_LENGTH + 1]{};
  ParticipantType type{ParticipantType::NAMO_NODE};
  ParticipantTrust trust{ParticipantTrust::UNKNOWN};
  ParticipantMembership membership{ParticipantMembership::QUARANTINED};
  bool leader_eligible{false};
  SiteSpatialProfile spatial_profile{};
};

struct SiteFloorplanManifest {
  bool committed{false};
  char floorplan_id[ParticipantRecord::MAX_ID_LENGTH + 1]{};
  uint32_t geometry_version{0};
  uint32_t config_hash{0};
  uint32_t image_hash{0};
};

struct SiteConfigurationSnapshot {
  char site_id[PlatformIdentity::MAX_ID_LENGTH + 1]{};
  char configuration_owner_node_id[ParticipantRecord::MAX_ID_LENGTH + 1]{};
  uint32_t authority_epoch{1};
  uint32_t generation{0};
  uint32_t roster_revision{0};
  bool roster_committed{false};
  std::array<SiteConfigurationMember, ParticipantRegistry::MAX_PARTICIPANTS>
      members{};
  SiteFloorplanManifest floorplan{};

  const SiteConfigurationMember *find(const char *participant_id) const;
  std::size_t member_count() const;
  std::size_t active_member_count() const;
};

static constexpr uint8_t MAX_AUTOMATIC_SPATIAL_GROUP_PARTICIPANTS = 7;

enum class AutomaticSpatialGroupResult : uint8_t {
  OK = 0,
  INVALID_ARGUMENT,
  NOT_READY,
  GROUP_FULL,
};

struct AutomaticSpatialGroupDecision {
  char space_id[ParticipantReadinessConfig::MAX_ASSIGNMENT_ID_LENGTH + 1]{};
  char group_id[ParticipantReadinessConfig::MAX_ASSIGNMENT_ID_LENGTH + 1]{};
  uint8_t participant_count{0};
  bool normalization_required{false};
};

enum class SiteConfigurationResult : uint8_t;

AutomaticSpatialGroupResult resolve_automatic_spatial_group(
    const PlatformIdentity &identity,
    const SiteConfigurationSnapshot &configuration,
    const char *peer_participant_id, const char *seed_space_id,
    const char *seed_group_id,
    AutomaticSpatialGroupDecision *decision);
SiteConfigurationResult apply_automatic_spatial_group_assignment(
    const PlatformIdentity &identity,
    const AutomaticSpatialGroupDecision &decision,
    const SiteConfigurationSnapshot &current,
    SiteConfigurationSnapshot *updated, bool *changed = nullptr);
const char *automatic_spatial_group_result_to_string(
    AutomaticSpatialGroupResult result);

enum class SiteConfigurationRevisionOrder : uint8_t {
  OLDER = 0,
  SAME,
  NEWER,
};

SiteConfigurationRevisionOrder compare_site_configuration_revision(
    uint32_t left_authority_epoch, uint32_t left_generation,
    uint32_t right_authority_epoch, uint32_t right_generation);
SiteConfigurationRevisionOrder compare_site_configuration_revision(
    const SiteConfigurationSnapshot &left,
    const SiteConfigurationSnapshot &right);
uint64_t site_configuration_digest(
    const SiteConfigurationSnapshot &configuration);
uint64_t site_topology_digest(
    const SiteConfigurationSnapshot &configuration);
uint64_t automatic_configuration_handover_fingerprint(
    const SiteConfigurationSnapshot &configuration,
    const char *successor_node_id);
bool same_site_topology(const SiteConfigurationSnapshot &left,
                        const SiteConfigurationSnapshot &right);

bool site_configuration_member_active(
    const SiteConfigurationSnapshot &configuration,
    const char *participant_id, bool require_leader_eligible = false);
uint8_t site_leader_role_penalty(
    const SiteConfigurationSnapshot *configuration,
    const char *participant_id);
uint8_t site_leader_role_penalty(
    const SiteConfigurationSnapshot *configuration,
    const SiteDirectorySnapshot *directory, const char *participant_id);
uint8_t group_leader_role_penalty(
    const SiteConfigurationSnapshot *configuration,
    const char *site_leader_node_id, const char *participant_id);
bool group_leader_role_combination_allowed(
    const SiteConfigurationSnapshot &configuration,
    const char *site_leader_node_id, const char *participant_id);
bool group_leader_assignment_allowed(
    const SiteConfigurationSnapshot &configuration, const char *group_id,
    const char *site_leader_node_id, const char *participant_id);
bool leader_candidate_preferred(
    const char *candidate_id, uint8_t candidate_penalty,
    const char *current_id, uint8_t current_penalty);
bool local_node_is_enrolled_site_member(
    const PlatformIdentity &identity,
    const SiteConfigurationSnapshot &configuration);
const SiteConfigurationMember *automatic_configuration_successor(
    const SiteConfigurationSnapshot &configuration);
bool automatic_configuration_handover_bootstrap_allowed(
    const SiteConfigurationSnapshot &current,
    const char *credential_participant_id,
    const char *advertised_participant_id, uint32_t authority_epoch,
    uint32_t generation, uint64_t configuration_digest,
    uint32_t topology_revision, uint64_t topology_digest);
bool site_configuration_needs_spatial_profile_reconciliation(
    const SiteConfigurationSnapshot &configuration);

enum class SiteConfigurationResult : uint8_t {
  OK = 0,
  INVALID_ARGUMENT,
  INVALID_CONFIGURATION,
  NOT_OWNER,
  FULL,
};

enum class SpatialProfileReconciliationMode : uint8_t {
  MISSING_ONLY = 0,
  EXPLICIT_CALIBRATION,
};

SiteConfigurationResult initialize_site_configuration(
    const PlatformIdentity &identity, bool enrolled,
    SiteConfigurationSnapshot *snapshot);
SiteConfigurationResult reconcile_unattested_namo_trust(
    const PlatformIdentity &identity,
    const SiteConfigurationSnapshot &current,
    SiteConfigurationSnapshot *updated, bool *changed = nullptr);
SiteConfigurationResult claim_local_site_ownership(
    const PlatformIdentity &identity, const ParticipantRegistry &participants,
    const SiteConfigurationSnapshot &current,
    SiteConfigurationSnapshot *updated);
SiteConfigurationResult commit_local_site_roster(
    const PlatformIdentity &identity, const ParticipantRegistry &participants,
    const SiteConfigurationSnapshot &current,
    SiteConfigurationSnapshot *updated);
SiteConfigurationResult reconcile_local_spatial_profiles(
    const PlatformIdentity &identity,
    const ParticipantReadinessRegistry &readiness,
    uint32_t local_pose_version,
    const SpatialTransform2D &local_transform,
    const SiteConfigurationSnapshot &current,
    SpatialProfileReconciliationMode mode,
    const char *explicit_participant_id,
    SiteConfigurationSnapshot *updated, bool *changed = nullptr);
SiteConfigurationResult reconcile_direct_spatial_readiness(
    const PlatformIdentity &identity,
    const ParticipantRegistry &participants,
    const SiteConfigurationSnapshot &configuration,
    const ParticipantReadinessRegistry &current,
    ParticipantReadinessRegistry *updated, bool *changed = nullptr);
SiteConfigurationResult prepare_local_pairing_site(
    const PlatformIdentity &identity, const ParticipantRegistry &participants,
    const SiteConfigurationSnapshot &current, const char *peer_participant_id,
    SiteConfigurationSnapshot *updated);
bool is_local_standalone_site_configuration(
    const PlatformIdentity &identity, const ParticipantRegistry &participants,
    const SiteConfigurationSnapshot &current);
SiteConfigurationResult reconcile_local_standalone_site_configuration(
    const PlatformIdentity &identity, const ParticipantRegistry &participants,
    const SiteConfigurationSnapshot &current,
    SiteConfigurationSnapshot *updated);
SiteConfigurationResult commit_local_floorplan_manifest(
    const PlatformIdentity &identity,
    const SiteConfigurationSnapshot &current, const char *floorplan_id,
    uint32_t geometry_version, uint32_t config_hash, uint32_t image_hash,
    SiteConfigurationSnapshot *updated,
    int32_t coordinate_width_mm = INT32_MAX,
    int32_t coordinate_height_mm = INT32_MAX);
bool site_configuration_has_remote_members(
    const PlatformIdentity &identity,
    const SiteConfigurationSnapshot &configuration);
bool site_floorplan_manifest_matches(
    const SiteConfigurationSnapshot &current, const char *floorplan_id,
    uint32_t geometry_version, uint32_t config_hash, uint32_t image_hash);
SiteConfigurationResult clear_local_floorplan_manifest(
    const PlatformIdentity &identity,
    const SiteConfigurationSnapshot &current,
    SiteConfigurationSnapshot *updated);
SiteConfigurationResult prepare_automatic_configuration_handover(
    const PlatformIdentity &identity,
    const SiteConfigurationSnapshot &current,
    SiteConfigurationSnapshot *updated,
    const char *designated_successor_node_id = nullptr);
SiteConfigurationResult prepare_local_configuration_conflict_resolution(
    const PlatformIdentity &identity,
    const SiteConfigurationSnapshot &current,
    SiteConfigurationSnapshot *updated);
SiteConfigurationResult set_local_site_member_leader_eligible(
    const PlatformIdentity &identity,
    const SiteConfigurationSnapshot &current, const char *participant_id,
    bool leader_eligible, SiteConfigurationSnapshot *updated);
SiteConfigurationResult revoke_local_site_member(
    const PlatformIdentity &identity,
    const SiteConfigurationSnapshot &current, const char *participant_id,
    SiteConfigurationSnapshot *updated);
SiteConfigurationResult forget_local_site_member(
    const PlatformIdentity &identity,
    const SiteConfigurationSnapshot &current, const char *participant_id,
    SiteConfigurationSnapshot *updated);
bool can_move_site_without_orphaning_participants(
    const ParticipantRegistry &participants,
    const SiteConfigurationSnapshot &configuration,
    const char *retry_participant_id = nullptr);
bool pairing_joiner_site_already_converged(
    const PlatformIdentity &identity,
    const SiteConfigurationSnapshot &configuration,
    const char *leader_site_id, const char *owner_node_id);

enum class SiteConfigurationReplicationResult : uint8_t {
  NOT_EVALUATED = 0,
  OK,
  ALREADY_CURRENT,
  INVALID_ARGUMENT,
  SITE_MISMATCH,
  OWNER_MISMATCH,
  LOCAL_MEMBER_MISSING,
  STALE_REMOTE,
  REVISION_CONFLICT,
};

enum class SiteConfigurationReplicationMode : uint8_t {
  STRICT = 0,
  AUTOMATIC_OWNER_HANDOVER,
  AUTHENTICATED_ENROLLMENT,
  QUORUM_AUTHORITY_REJOIN,
};

bool authenticated_enrollment_source_allowed(
    const PlatformIdentity &identity,
    const SiteConfigurationSnapshot &current,
    const ParticipantRegistry &participants,
    const ParticipantCredentialRegistry &credentials,
    const char *source_node_id);

SiteConfigurationReplicationResult validate_replicated_site_configuration(
    const PlatformIdentity &identity,
    const SiteConfigurationSnapshot &current,
    const SiteConfigurationSnapshot &remote,
    SiteConfigurationReplicationMode mode =
        SiteConfigurationReplicationMode::STRICT,
    const char *source_node_id = nullptr);
const char *site_configuration_replication_result_to_string(
    SiteConfigurationReplicationResult result);
const char *site_configuration_replication_mode_to_string(
    SiteConfigurationReplicationMode mode);

enum class AutomaticConfigurationHandoverCondition : uint8_t {
  NOT_REQUIRED = 0,
  OWNER_ONLINE,
  NO_SUCCESSOR,
  NOT_DESIGNATED_SUCCESSOR,
  CONFIGURATION_CONFLICT,
  QUORUM_UNAVAILABLE,
  REPLICA_NOT_READY,
  CREDENTIALS_INCOMPLETE,
  ELIGIBLE,
};

enum class AutomaticConfigurationHandoverState : uint8_t {
  IDLE = 0,
  BLOCKED,
  WAITING,
  READY,
  COMPLETE,
  FAILED,
};

const char *automatic_configuration_handover_condition_to_string(
    AutomaticConfigurationHandoverCondition condition);
const char *automatic_configuration_handover_state_to_string(
    AutomaticConfigurationHandoverState state);

AutomaticConfigurationHandoverCondition
evaluate_automatic_configuration_handover(
    const PlatformIdentity &identity,
    const SiteConfigurationSnapshot &configuration, bool owner_online,
    bool quorum_available, bool local_replica_ready,
    bool credentials_complete,
    const char *designated_successor_node_id = nullptr);

class AutomaticConfigurationHandoverTracker {
 public:
  static constexpr uint32_t STABILITY_MS = 30000;
  static constexpr uint32_t RETRY_BASE_MS = 5000;
  static constexpr uint32_t RETRY_MAX_MS = 60000;

  bool update(AutomaticConfigurationHandoverCondition condition,
              uint64_t authority_fingerprint, uint32_t now_ms);
  void complete(bool success, uint32_t now_ms);
  void reset();

  AutomaticConfigurationHandoverState state() const { return this->state_; }
  AutomaticConfigurationHandoverCondition condition() const {
    return this->condition_;
  }
  uint32_t wait_remaining_ms(uint32_t now_ms) const;

 private:
  AutomaticConfigurationHandoverState state_{
      AutomaticConfigurationHandoverState::IDLE};
  AutomaticConfigurationHandoverCondition condition_{
      AutomaticConfigurationHandoverCondition::NOT_REQUIRED};
  uint32_t stable_since_ms_{0};
  uint32_t retry_due_ms_{0};
  uint64_t authority_fingerprint_{0};
  uint8_t retry_count_{0};
};

enum class SiteActivationAdmissionResult : uint8_t {
  OK = 0,
  INVALID_ARGUMENT,
  ROSTER_NOT_COMMITTED,
  FLOORPLAN_NOT_COMMITTED,
  MEMBER_MISSING,
  READINESS_INCOMPLETE,
  FLOORPLAN_MISMATCH,
  ENDPOINT_OFFLINE,
  REPLICA_NOT_READY,
  REVISION_MISMATCH,
  GROUP_FULL,
};

SiteActivationAdmissionResult validate_namo_node_activation(
    const SiteConfigurationSnapshot &configuration,
    const ParticipantReadinessRecord *readiness, const char *participant_id,
    bool endpoint_online, bool replica_ready,
    uint32_t remote_configuration_generation,
    uint32_t remote_geometry_version, uint32_t remote_config_hash,
    uint32_t remote_image_hash);
const char *site_activation_admission_result_to_string(
    SiteActivationAdmissionResult result);

enum class SiteConfigurationCodecResult : uint8_t {
  OK = 0,
  INVALID_ARGUMENT,
  INVALID_CONFIGURATION,
  EMPTY,
  CORRUPT,
  UNSUPPORTED_VERSION,
};

class SiteConfigurationCodec {
 public:
  static constexpr uint16_t STORAGE_VERSION = 4;
  static constexpr std::size_t MAX_ENCODED_SIZE = 4096;

  static SiteConfigurationCodecResult encode(
      const SiteConfigurationSnapshot &snapshot, uint8_t *output,
      std::size_t output_capacity, std::size_t *output_size);
  static SiteConfigurationCodecResult decode(
      const uint8_t *data, std::size_t size,
      SiteConfigurationSnapshot *snapshot);
};

struct SiteConfigurationStorageBlob {
  std::array<uint8_t, SiteConfigurationCodec::MAX_ENCODED_SIZE> bytes{};
};

struct SiteConfigurationCommitBlob {
  std::array<uint8_t, 16> bytes{};
};

enum class SiteConfigurationStoreResult : uint8_t {
  EMPTY = 0,
  LOADED,
  CREATED,
  UPDATED,
  COMMIT_REPAIRED,
  CORRUPT,
  UNSUPPORTED_VERSION,
  STORAGE_FAILED,
  INVALID_CONFIGURATION,
};

enum class StandaloneSiteReconciliationState : uint8_t {
  NOT_REQUIRED = 0,
  PENDING,
  COMPLETE,
  FAILED,
};

enum class StandaloneSiteReconciliationResult : uint8_t {
  IDLE = 0,
  OK,
  INVALID_STATE,
  BUILD_FAILED,
  SITE_STORAGE_FAILED,
  MEMBERSHIP_STORAGE_FAILED,
};

const char *standalone_site_reconciliation_state_to_string(
    StandaloneSiteReconciliationState state);
const char *standalone_site_reconciliation_result_to_string(
    StandaloneSiteReconciliationResult result);

class StandaloneSiteReconciliationTracker {
 public:
  void mark_not_required();
  void schedule(uint32_t now_ms, uint32_t delay_ms);
  bool due(uint32_t now_ms) const;
  void complete(StandaloneSiteReconciliationResult result);

  StandaloneSiteReconciliationState state() const { return this->state_; }
  StandaloneSiteReconciliationResult result() const { return this->result_; }
  bool ready_for_pairing() const {
    return this->state_ == StandaloneSiteReconciliationState::NOT_REQUIRED ||
           this->state_ == StandaloneSiteReconciliationState::COMPLETE;
  }

 private:
  StandaloneSiteReconciliationState state_{
      StandaloneSiteReconciliationState::NOT_REQUIRED};
  StandaloneSiteReconciliationResult result_{
      StandaloneSiteReconciliationResult::IDLE};
  uint32_t due_ms_{0};
};

class SiteConfigurationStorageBackend {
 public:
  virtual ~SiteConfigurationStorageBackend() = default;
  virtual bool load_site_configuration_slot(
      uint8_t slot, SiteConfigurationStorageBlob *blob) = 0;
  virtual bool save_site_configuration_slot(
      uint8_t slot, const SiteConfigurationStorageBlob &blob) = 0;
  virtual bool load_site_configuration_commit(
      SiteConfigurationCommitBlob *blob) = 0;
  virtual bool save_site_configuration_commit(
      const SiteConfigurationCommitBlob &blob) = 0;
  virtual bool sync() = 0;
};

class SiteConfigurationStore {
 public:
  SiteConfigurationStoreResult load(SiteConfigurationStorageBackend &backend,
                                    SiteConfigurationSnapshot *snapshot);
  SiteConfigurationStoreResult create(
      SiteConfigurationStorageBackend &backend,
      const SiteConfigurationSnapshot &configuration,
      SiteConfigurationSnapshot *snapshot = nullptr);
  SiteConfigurationStoreResult update(
      SiteConfigurationStorageBackend &backend,
      const SiteConfigurationSnapshot &configuration,
      SiteConfigurationSnapshot *snapshot = nullptr);
  SiteConfigurationStoreResult replace_for_enrollment(
      SiteConfigurationStorageBackend &backend,
      const SiteConfigurationSnapshot &configuration,
      SiteConfigurationSnapshot *snapshot = nullptr);
  SiteConfigurationStoreResult replace_for_local_reset(
      SiteConfigurationStorageBackend &backend,
      const SiteConfigurationSnapshot &configuration,
      SiteConfigurationSnapshot *snapshot = nullptr);
  SiteConfigurationStoreResult replace_with_newer(
      SiteConfigurationStorageBackend &backend,
      const SiteConfigurationSnapshot &configuration,
      SiteConfigurationSnapshot *snapshot = nullptr);
  SiteConfigurationStoreResult replace_with_quorum_authority(
      SiteConfigurationStorageBackend &backend,
      const SiteConfigurationSnapshot &configuration,
      SiteConfigurationSnapshot *snapshot = nullptr);

 private:
  SiteConfigurationStorageBlob blob_{};
  SiteConfigurationSnapshot scratch_{};

  SiteConfigurationStoreResult load_committed_(
      SiteConfigurationStorageBackend &backend, uint8_t *active_slot);
  SiteConfigurationStoreResult decode_slot_();
  SiteConfigurationStoreResult write_slot_(
      SiteConfigurationStorageBackend &backend, uint8_t slot);
};

const char *site_configuration_store_result_to_string(
    SiteConfigurationStoreResult result);

#ifdef USE_ESP32
SiteConfigurationStoreResult initialize_esp32_site_configuration(
    RadarStorage *storage, const PlatformIdentity &identity,
    const ParticipantRegistry &participants, bool enrolled,
    SiteConfigurationSnapshot *snapshot);
bool get_esp32_site_configuration_persistence(
    SiteConfigurationStorageBackend **backend, SiteConfigurationStore **store);
#endif

static_assert(std::is_trivially_copyable<SiteConfigurationSnapshot>::value,
              "Site configuration must not own heap-backed state");
static_assert(sizeof(SiteSpatialProfile) <= 112,
              "Site spatial profile exceeded its embedded budget");
static_assert(
    std::is_trivially_copyable<AutomaticConfigurationHandoverTracker>::value,
    "Automatic handover tracker must not own heap-backed state");
static_assert(sizeof(AutomaticConfigurationHandoverTracker) <= 32,
              "Automatic handover tracker exceeded its embedded budget");
static_assert(sizeof(SiteConfigurationSnapshot) <= 4608,
              "Site configuration runtime state exceeded its embedded budget");
static_assert(sizeof(SiteConfigurationStore) <= 8704,
              "Site configuration store workspace exceeded its embedded budget");

}  // namespace radar_api_server
}  // namespace esphome
