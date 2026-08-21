#include "site_configuration.h"
#include "participant_credentials.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace esphome::radar_api_server;

namespace {

void require(bool condition, const char *message) {
  if (condition)
    return;
  std::fprintf(stderr, "site_configuration_tests: %s\n", message);
  std::exit(1);
}

PlatformIdentity identity(const char *site_id, const char *node_id) {
  PlatformIdentity value{};
  std::strcpy(value.site_id, site_id);
  std::strcpy(value.node_id, node_id);
  return value;
}

SpatialTransform2D transform(int32_t x_mm, int32_t y_mm,
                             int16_t rotation_cdeg) {
  SpatialTransform2D value{};
  value.origin_x_mm = x_mm;
  value.origin_y_mm = y_mm;
  value.rotation_cdeg = rotation_cdeg;
  value.residual_mm = 0;
  value.sample_count = 1;
  value.valid = true;
  return value;
}

uint32_t fnv1a(const uint8_t *data, std::size_t size) {
  uint32_t hash = 2166136261U;
  for (std::size_t index = 0; index < size; ++index) {
    hash ^= data[index];
    hash *= 16777619U;
  }
  return hash;
}

void write_u32(uint8_t *data, uint32_t value) {
  data[0] = static_cast<uint8_t>(value & 0xFFU);
  data[1] = static_cast<uint8_t>((value >> 8U) & 0xFFU);
  data[2] = static_cast<uint8_t>((value >> 16U) & 0xFFU);
  data[3] = static_cast<uint8_t>((value >> 24U) & 0xFFU);
}

void add_participant(ParticipantRegistry *registry, const char *id,
                     ParticipantMembership membership) {
  require(registry->discover(id, ParticipantType::NAMO_NODE) ==
              ParticipantRegistryResult::OK,
          "discover failed");
  require(registry->classify(id, ParticipantTrust::OFFICIAL_VERIFIED) ==
              ParticipantRegistryResult::OK,
          "classify failed");
  if (membership == ParticipantMembership::ACTIVE)
    require(registry->activate(id) == ParticipantRegistryResult::OK,
            "activate failed");
  else if (membership == ParticipantMembership::REVOKED)
    require(registry->revoke(id) == ParticipantRegistryResult::OK,
            "revoke failed");
}

SiteConfigurationMember *mutable_member(SiteConfigurationSnapshot *snapshot,
                                        const char *participant_id) {
  for (auto &member : snapshot->members) {
    if (member.occupied &&
        std::strcmp(member.participant_id, participant_id) == 0)
      return &member;
  }
  return nullptr;
}

class FakeBackend : public SiteConfigurationStorageBackend {
 public:
  bool load_site_configuration_slot(
      uint8_t slot, SiteConfigurationStorageBlob *blob) override {
    if (slot >= 2 || !slots_present[slot])
      return false;
    *blob = slots[slot];
    return true;
  }
  bool save_site_configuration_slot(
      uint8_t slot, const SiteConfigurationStorageBlob &blob) override {
    if (slot >= 2 || fail_slot_write)
      return false;
    slots[slot] = blob;
    slots_present[slot] = true;
    return true;
  }
  bool load_site_configuration_commit(
      SiteConfigurationCommitBlob *blob) override {
    if (!commit_present)
      return false;
    *blob = commit;
    return true;
  }
  bool save_site_configuration_commit(
      const SiteConfigurationCommitBlob &blob) override {
    if (fail_commit_write)
      return false;
    commit = blob;
    commit_present = true;
    return true;
  }
  bool sync() override { return !fail_sync; }

  SiteConfigurationStorageBlob slots[2]{};
  bool slots_present[2]{false, false};
  SiteConfigurationCommitBlob commit{};
  bool commit_present{false};
  bool fail_slot_write{false};
  bool fail_commit_write{false};
  bool fail_sync{false};
};

void standalone_bootstrap_is_owned_and_committed() {
  SiteConfigurationSnapshot snapshot{};
  require(initialize_site_configuration(
              identity("site-home", "namo-node-local"), false, &snapshot) ==
              SiteConfigurationResult::OK,
          "standalone bootstrap failed");
  require(snapshot.roster_committed && snapshot.member_count() == 1 &&
              snapshot.active_member_count() == 1 &&
              std::strcmp(snapshot.configuration_owner_node_id,
                          "namo-node-local") == 0,
          "standalone must own a committed one-node roster");
}

void unattested_namo_trust_is_community_and_migrates_once() {
  const auto owner = identity("site-home", "namo-node-owner");
  SiteConfigurationSnapshot current{};
  require(initialize_site_configuration(owner, false, &current) ==
              SiteConfigurationResult::OK,
          "community trust bootstrap failed");
  const auto *local = current.find(owner.node_id);
  require(local != nullptr &&
              local->trust == ParticipantTrust::COMMUNITY_UNVERIFIED &&
              local->leader_eligible,
          "standalone NAMO trust must not claim unavailable attestation");

  auto *legacy_local = mutable_member(&current, owner.node_id);
  legacy_local->trust = ParticipantTrust::OFFICIAL_VERIFIED;
  auto &legacy_peer = current.members[1];
  legacy_peer.occupied = true;
  std::strcpy(legacy_peer.participant_id, "namo-node-peer");
  legacy_peer.type = ParticipantType::NAMO_NODE;
  legacy_peer.trust = ParticipantTrust::OFFICIAL_VERIFIED;
  legacy_peer.membership = ParticipantMembership::ACTIVE;
  legacy_peer.leader_eligible = false;

  SiteConfigurationSnapshot migrated{};
  bool changed = false;
  require(reconcile_unattested_namo_trust(owner, current, &migrated,
                                          &changed) ==
              SiteConfigurationResult::OK &&
              changed &&
              migrated.roster_revision == current.roster_revision + 1 &&
              migrated.find(owner.node_id)->trust ==
                  ParticipantTrust::COMMUNITY_UNVERIFIED &&
              migrated.find(owner.node_id)->leader_eligible &&
              migrated.find("namo-node-peer")->trust ==
                  ParticipantTrust::COMMUNITY_UNVERIFIED &&
              !migrated.find("namo-node-peer")->leader_eligible,
          "legacy official trust was not demoted without changing eligibility");

  SiteConfigurationSnapshot repeated{};
  changed = true;
  require(reconcile_unattested_namo_trust(owner, migrated, &repeated,
                                          &changed) ==
              SiteConfigurationResult::OK &&
              !changed && repeated.roster_revision == migrated.roster_revision,
          "trust migration must be idempotent");
  require(reconcile_unattested_namo_trust(
              identity("site-home", "namo-node-peer"), current, &repeated,
              &changed) == SiteConfigurationResult::NOT_OWNER,
          "a follower must not rewrite canonical trust");

  FakeBackend backend{};
  SiteConfigurationStore store{};
  SiteConfigurationSnapshot persisted{};
  require(store.create(backend, current, &persisted) ==
              SiteConfigurationStoreResult::CREATED &&
              store.update(backend, migrated, &persisted) ==
                  SiteConfigurationStoreResult::UPDATED &&
              persisted.generation == current.generation + 1 &&
              persisted.find(owner.node_id)->trust ==
                  ParticipantTrust::COMMUNITY_UNVERIFIED &&
              persisted.find("namo-node-peer")->trust ==
                  ParticipantTrust::COMMUNITY_UNVERIFIED,
          "trust migration must persist as one canonical generation");

  ParticipantRegistry legacy_registry{};
  add_participant(&legacy_registry, "namo-node-direct",
                  ParticipantMembership::ACTIVE);
  SiteConfigurationSnapshot refreshed{};
  require(commit_local_site_roster(owner, legacy_registry, migrated,
                                   &refreshed) ==
              SiteConfigurationResult::OK &&
              refreshed.find("namo-node-direct") != nullptr &&
              refreshed.find("namo-node-direct")->trust ==
                  ParticipantTrust::COMMUNITY_UNVERIFIED &&
              refreshed.find("namo-node-direct")->leader_eligible,
          "a legacy direct record must not mint official canonical trust");
}

void enrolled_migration_fails_closed_without_guessing_owner() {
  SiteConfigurationSnapshot snapshot{};
  require(initialize_site_configuration(
              identity("site-home", "namo-node-joiner"), true, &snapshot) ==
              SiteConfigurationResult::OK,
          "enrolled bootstrap failed");
  require(!snapshot.roster_committed &&
              snapshot.configuration_owner_node_id[0] == '\0' &&
              snapshot.member_count() == 1,
          "an enrolled migration must not guess the configuration owner");
}

void committed_remote_owner_identifies_an_enrolled_local_member() {
  const auto owner = identity("site-home", "namo-node-owner");
  SiteConfigurationSnapshot base{};
  require(initialize_site_configuration(owner, false, &base) ==
              SiteConfigurationResult::OK,
          "enrolled membership fixture bootstrap failed");
  ParticipantRegistry registry{};
  add_participant(&registry, "namo-node-follower",
                  ParticipantMembership::ACTIVE);
  SiteConfigurationSnapshot configuration{};
  require(commit_local_site_roster(owner, registry, base, &configuration) ==
              SiteConfigurationResult::OK,
          "enrolled membership fixture commit failed");

  require(local_node_is_enrolled_site_member(
              identity("site-home", "namo-node-follower"), configuration),
          "a committed active member with a remote owner must be enrolled");
  require(!local_node_is_enrolled_site_member(owner, configuration),
          "the Site creator must not be repaired as an enrolled follower");
  require(!local_node_is_enrolled_site_member(
              identity("site-other", "namo-node-follower"), configuration),
          "a different Site identity must not be repaired");
}

void unresolved_owner_can_be_claimed_once_by_explicit_local_action() {
  const auto local = identity("site-home", "namo-node-local");
  SiteConfigurationSnapshot current{};
  require(initialize_site_configuration(local, true, &current) ==
              SiteConfigurationResult::OK,
          "enrolled owner recovery bootstrap failed");
  FakeBackend backend{};
  SiteConfigurationStore store{};
  require(store.create(backend, current, &current) ==
              SiteConfigurationStoreResult::CREATED,
          "owner recovery storage bootstrap failed");
  ParticipantRegistry registry{};
  add_participant(&registry, "namo-node-peer",
                  ParticipantMembership::QUARANTINED);

  SiteConfigurationSnapshot claimed{};
  require(claim_local_site_ownership(local, registry, current, &claimed) ==
              SiteConfigurationResult::OK,
          "explicit local owner claim failed");
  require(claimed.roster_committed && claimed.member_count() == 2 &&
              std::strcmp(claimed.configuration_owner_node_id,
                          local.node_id) == 0 &&
              claimed.find("namo-node-peer") != nullptr &&
              claimed.generation == 1 && claimed.roster_revision == 2,
          "owner claim must atomically commit the canonical local roster");

  require(store.update(backend, claimed, &claimed) ==
              SiteConfigurationStoreResult::UPDATED,
          "owner claim persistence failed");
  SiteConfigurationSnapshot loaded{};
  require(store.load(backend, &loaded) ==
              SiteConfigurationStoreResult::LOADED &&
              loaded.generation == 2 && loaded.roster_committed &&
              std::strcmp(loaded.configuration_owner_node_id,
                          local.node_id) == 0 &&
              loaded.find("namo-node-peer") != nullptr,
          "owner and canonical roster must survive reload together");

  SiteConfigurationSnapshot rejected{};
  require(claim_local_site_ownership(local, registry, loaded, &rejected) ==
              SiteConfigurationResult::NOT_OWNER,
          "an established owner must not be claimed again");
}

void only_owner_can_commit_a_canonical_bounded_roster() {
  const auto local = identity("site-home", "namo-node-z");
  SiteConfigurationSnapshot current{};
  require(initialize_site_configuration(local, false, &current) ==
              SiteConfigurationResult::OK,
          "owner bootstrap failed");
  ParticipantRegistry registry{};
  add_participant(&registry, "namo-node-b", ParticipantMembership::ACTIVE);
  add_participant(&registry, "namo-node-a", ParticipantMembership::QUARANTINED);
  add_participant(&registry, "namo-node-revoked", ParticipantMembership::REVOKED);

  SiteConfigurationSnapshot updated{};
  require(commit_local_site_roster(local, registry, current, &updated) ==
              SiteConfigurationResult::OK,
          "owner roster commit failed");
  require(updated.member_count() == 3 && updated.active_member_count() == 2 &&
              updated.roster_revision == 2 &&
              std::strcmp(updated.members[0].participant_id,
                          "namo-node-a") == 0 &&
              updated.find("namo-node-revoked") == nullptr,
          "roster must be sorted, bounded, and omit revoked peers");

  SiteConfigurationSnapshot foreign = current;
  std::strcpy(foreign.configuration_owner_node_id, "namo-node-b");
  foreign.members[1].occupied = true;
  std::strcpy(foreign.members[1].participant_id, "namo-node-b");
  foreign.members[1].type = ParticipantType::NAMO_NODE;
  foreign.members[1].trust = ParticipantTrust::OFFICIAL_VERIFIED;
  foreign.members[1].membership = ParticipantMembership::ACTIVE;
  foreign.members[1].leader_eligible = true;
  require(commit_local_site_roster(local, registry, foreign, &updated) ==
              SiteConfigurationResult::NOT_OWNER,
          "a non-owner must not commit Site roster state");
}

void unchanged_roster_commit_is_idempotent() {
  const auto local = identity("site-home", "namo-node-local");
  SiteConfigurationSnapshot current{};
  require(initialize_site_configuration(local, false, &current) ==
              SiteConfigurationResult::OK,
          "idempotent roster bootstrap failed");
  ParticipantRegistry registry{};
  add_participant(&registry, "namo-node-peer",
                  ParticipantMembership::QUARANTINED);

  SiteConfigurationSnapshot committed{};
  require(commit_local_site_roster(local, registry, current, &committed) ==
              SiteConfigurationResult::OK &&
              committed.roster_revision == current.roster_revision + 1,
          "initial roster commit failed");
  SiteConfigurationSnapshot repeated{};
  require(commit_local_site_roster(local, registry, committed, &repeated) ==
              SiteConfigurationResult::OK &&
              repeated.generation == committed.generation &&
              repeated.roster_revision == committed.roster_revision &&
              repeated.member_count() == committed.member_count(),
          "an unchanged roster must not publish another revision");

  require(registry.activate("namo-node-peer") ==
              ParticipantRegistryResult::OK &&
              commit_local_site_roster(local, registry, repeated, &repeated) ==
                  SiteConfigurationResult::OK &&
              repeated.roster_revision == committed.roster_revision + 1 &&
              repeated.active_member_count() == 2,
          "a real membership change must publish a new roster revision");
}

void approved_spatial_profiles_are_canonical_and_survive_roster_refresh() {
  const auto owner = identity("site-home", "namo-node-owner");
  SiteConfigurationSnapshot current{};
  require(initialize_site_configuration(owner, false, &current) ==
              SiteConfigurationResult::OK,
          "spatial profile bootstrap failed");
  ParticipantRegistry participants{};
  add_participant(&participants, "namo-node-peer",
                  ParticipantMembership::ACTIVE);
  require(commit_local_site_roster(owner, participants, current, &current) ==
              SiteConfigurationResult::OK,
          "spatial profile roster failed");
  require(commit_local_floorplan_manifest(
              owner, current, "floorplan-main", 4, 10, 20, &current) ==
              SiteConfigurationResult::OK,
          "spatial profile floorplan failed");

  ParticipantReadinessRegistry readiness{};
  ParticipantReadinessConfig peer{};
  peer.capability_flags =
      PARTICIPANT_CAP_POINT_TRACKS | PARTICIPANT_CAP_PRESENCE;
  peer.max_targets = 3;
  std::strcpy(peer.floorplan_id, "floorplan-main");
  std::strcpy(peer.space_id, "room-main");
  std::strcpy(peer.group_id, "room-main");
  peer.geometry_version = 4;
  peer.local_pose_version = 3;
  peer.pose_version = 5;
  peer.calibration_version = 7;
  peer.transform = transform(1400, 900, -250);
  require(readiness.configure("namo-node-peer", peer) ==
              ParticipantReadinessResult::OK,
          "spatial profile readiness failed");

  SiteConfigurationSnapshot profiled{};
  bool changed = false;
  require(reconcile_local_spatial_profiles(
              owner, readiness, 3, transform(1000, 800, 0), current,
              SpatialProfileReconciliationMode::EXPLICIT_CALIBRATION,
              "namo-node-peer",
              &profiled, &changed) == SiteConfigurationResult::OK &&
              changed,
          "approved spatial profiles were not published");
  const auto *local = profiled.find(owner.node_id);
  const auto *remote = profiled.find("namo-node-peer");
  require(local != nullptr && local->spatial_profile.committed &&
              std::strcmp(local->spatial_profile.space_id, "room-main") == 0 &&
              std::strcmp(local->spatial_profile.group_id, "room-main") == 0 &&
              local->spatial_profile.pose_version == 3 &&
              local->spatial_profile.calibration_version == 1 &&
              local->spatial_profile.transform.origin_x_mm == 1000 &&
              remote != nullptr && remote->spatial_profile.committed &&
              std::strcmp(remote->spatial_profile.space_id, "room-main") == 0 &&
              std::strcmp(remote->spatial_profile.group_id, "room-main") == 0 &&
              remote->spatial_profile.pose_version == 5 &&
              remote->spatial_profile.calibration_version == 7 &&
              remote->spatial_profile.transform.origin_x_mm == 1400,
          "canonical profiles do not contain both approved transforms");

  SiteConfigurationSnapshot refreshed{};
  require(commit_local_site_roster(owner, participants, profiled,
                                   &refreshed) ==
              SiteConfigurationResult::OK,
          "profile-preserving roster refresh failed");
  require(refreshed.find(owner.node_id)->spatial_profile.committed &&
              refreshed.find("namo-node-peer")
                  ->spatial_profile.transform.origin_x_mm == 1400,
          "roster refresh discarded approved spatial profiles");

  changed = true;
  require(reconcile_local_spatial_profiles(
              owner, readiness, 3, transform(1000, 800, 0), refreshed,
              SpatialProfileReconciliationMode::EXPLICIT_CALIBRATION,
              "namo-node-peer",
              &refreshed, &changed) == SiteConfigurationResult::OK &&
              !changed,
          "unchanged spatial profiles must be idempotent");
}

void legacy_assignment_reconciliation_preserves_approved_transforms() {
  const auto owner = identity("site-home", "namo-node-owner");
  SiteConfigurationSnapshot current{};
  require(initialize_site_configuration(owner, false, &current) ==
              SiteConfigurationResult::OK,
          "legacy assignment bootstrap failed");
  ParticipantRegistry participants{};
  add_participant(&participants, "namo-node-peer",
                  ParticipantMembership::ACTIVE);
  require(commit_local_site_roster(owner, participants, current, &current) ==
              SiteConfigurationResult::OK &&
              commit_local_floorplan_manifest(
                  owner, current, "floorplan-main", 3, 10, 20, &current) ==
                  SiteConfigurationResult::OK,
          "legacy assignment fixture failed");

  auto *local = mutable_member(&current, owner.node_id);
  auto *remote = mutable_member(&current, "namo-node-peer");
  require(local != nullptr && remote != nullptr,
          "legacy assignment members are missing");
  local->spatial_profile.committed = true;
  local->spatial_profile.pose_version = 6;
  local->spatial_profile.calibration_version = 2;
  local->spatial_profile.transform = transform(1100, 700, 125);
  remote->spatial_profile.committed = true;
  remote->spatial_profile.pose_version = 4;
  remote->spatial_profile.calibration_version = 2;
  remote->spatial_profile.transform = transform(2300, 900, -250);
  require(site_configuration_needs_spatial_profile_reconciliation(current),
          "legacy profiles with empty assignments must request reconciliation");

  ParticipantReadinessRegistry readiness{};
  ParticipantReadinessConfig peer{};
  peer.capability_flags =
      PARTICIPANT_CAP_POINT_TRACKS | PARTICIPANT_CAP_PRESENCE;
  peer.max_targets = 3;
  std::strcpy(peer.floorplan_id, "floorplan-main");
  std::strcpy(peer.space_id, "room-main");
  std::strcpy(peer.group_id, "wall-room-main");
  peer.geometry_version = 3;
  peer.local_pose_version = 4;
  peer.pose_version = 4;
  peer.calibration_version = 2;
  peer.transform = transform(9999, 8888, 777);
  require(readiness.configure("namo-node-peer", peer) ==
              ParticipantReadinessResult::OK,
          "legacy assignment readiness failed");

  SiteConfigurationSnapshot repaired{};
  bool changed = false;
  require(reconcile_local_spatial_profiles(
              owner, readiness, 6, transform(7777, 6666, 555), current,
              SpatialProfileReconciliationMode::MISSING_ONLY, nullptr,
              &repaired, &changed) == SiteConfigurationResult::OK &&
              changed,
          "legacy assignment reconciliation failed");
  local = mutable_member(&repaired, owner.node_id);
  remote = mutable_member(&repaired, "namo-node-peer");
  require(local != nullptr && remote != nullptr &&
              std::strcmp(local->spatial_profile.space_id, "room-main") == 0 &&
              std::strcmp(local->spatial_profile.group_id,
                          "wall-room-main") == 0 &&
              local->spatial_profile.pose_version == 6 &&
              local->spatial_profile.calibration_version == 2 &&
              local->spatial_profile.transform.origin_x_mm == 1100 &&
              local->spatial_profile.transform.origin_y_mm == 700 &&
              local->spatial_profile.transform.rotation_cdeg == 125,
          "local legacy assignment repair changed the approved transform");
  require(std::strcmp(remote->spatial_profile.space_id, "room-main") == 0 &&
              std::strcmp(remote->spatial_profile.group_id,
                          "wall-room-main") == 0 &&
              remote->spatial_profile.pose_version == 4 &&
              remote->spatial_profile.calibration_version == 2 &&
              remote->spatial_profile.transform.origin_x_mm == 2300 &&
              remote->spatial_profile.transform.origin_y_mm == 900 &&
              remote->spatial_profile.transform.rotation_cdeg == -250 &&
              !site_configuration_needs_spatial_profile_reconciliation(
                  repaired),
          "remote legacy assignment repair changed the approved transform");

  auto mismatched = current;
  mutable_member(&mismatched, "namo-node-peer")
      ->spatial_profile.pose_version = 5;
  changed = false;
  require(reconcile_local_spatial_profiles(
              owner, readiness, 6, transform(7777, 6666, 555), mismatched,
              SpatialProfileReconciliationMode::MISSING_ONLY, nullptr,
              &repaired, &changed) == SiteConfigurationResult::OK,
          "mismatched legacy assignment reconciliation failed");
  remote = mutable_member(&repaired, "namo-node-peer");
  require(remote != nullptr && remote->spatial_profile.space_id[0] == '\0' &&
              remote->spatial_profile.group_id[0] == '\0' &&
              remote->spatial_profile.pose_version == 5 &&
              remote->spatial_profile.transform.origin_x_mm == 2300 &&
              site_configuration_needs_spatial_profile_reconciliation(
                  repaired),
          "mismatched revisions must defer assignment without moving a radar");
}

void local_assignment_is_never_guessed_across_multiple_groups() {
  const auto owner = identity("site-home", "namo-node-owner");
  SiteConfigurationSnapshot current{};
  require(initialize_site_configuration(owner, false, &current) ==
              SiteConfigurationResult::OK,
          "ambiguous assignment bootstrap failed");
  ParticipantRegistry participants{};
  add_participant(&participants, "namo-node-room-a",
                  ParticipantMembership::ACTIVE);
  add_participant(&participants, "namo-node-room-b",
                  ParticipantMembership::ACTIVE);
  require(commit_local_site_roster(owner, participants, current, &current) ==
              SiteConfigurationResult::OK &&
              commit_local_floorplan_manifest(
                  owner, current, "floorplan-main", 1, 10, 20, &current) ==
                  SiteConfigurationResult::OK,
          "ambiguous assignment fixture failed");

  ParticipantReadinessRegistry readiness{};
  ParticipantReadinessConfig profile{};
  profile.capability_flags =
      PARTICIPANT_CAP_POINT_TRACKS | PARTICIPANT_CAP_PRESENCE;
  profile.max_targets = 3;
  std::strcpy(profile.floorplan_id, "floorplan-main");
  profile.geometry_version = 1;
  profile.local_pose_version = 1;
  profile.pose_version = 1;
  profile.calibration_version = 2;
  profile.transform = transform(1400, 900, 0);
  std::strcpy(profile.space_id, "room-a");
  std::strcpy(profile.group_id, "group-a");
  require(readiness.configure("namo-node-room-a", profile) ==
              ParticipantReadinessResult::OK,
          "room A readiness failed");
  profile.calibration_version = 9;
  profile.transform = transform(2400, 900, 0);
  std::strcpy(profile.space_id, "room-b");
  std::strcpy(profile.group_id, "group-b");
  require(readiness.configure("namo-node-room-b", profile) ==
              ParticipantReadinessResult::OK,
          "room B readiness failed");

  SiteConfigurationSnapshot updated{};
  bool changed = false;
  require(reconcile_local_spatial_profiles(
              owner, readiness, 1, transform(1000, 800, 0), current,
              SpatialProfileReconciliationMode::MISSING_ONLY, nullptr,
              &updated, &changed) == SiteConfigurationResult::OK &&
              changed,
          "remote canonical assignments were not reconciled");
  const auto *local = updated.find(owner.node_id);
  require(local != nullptr && !local->spatial_profile.committed,
          "different Groups must not assign the local Node by revision order");

  auto *fixed_local = mutable_member(&updated, owner.node_id);
  require(fixed_local != nullptr, "fixed local profile is missing");
  fixed_local->spatial_profile.committed = true;
  std::strcpy(fixed_local->spatial_profile.space_id, "room-a");
  std::strcpy(fixed_local->spatial_profile.group_id, "group-a");
  fixed_local->spatial_profile.pose_version = 1;
  fixed_local->spatial_profile.calibration_version = 2;
  fixed_local->spatial_profile.transform = transform(1000, 800, 0);

  SiteConfigurationSnapshot preserved{};
  require(reconcile_local_spatial_profiles(
              owner, readiness, 1, transform(1100, 800, 0), updated,
              SpatialProfileReconciliationMode::MISSING_ONLY, nullptr,
              &preserved, &changed) == SiteConfigurationResult::OK,
          "fixed local assignment reconciliation failed");
  local = preserved.find(owner.node_id);
  require(local != nullptr &&
              std::strcmp(local->spatial_profile.group_id, "group-a") == 0 &&
              local->spatial_profile.calibration_version == 2 &&
              local->spatial_profile.transform.origin_x_mm == 1000,
          "a newer calibration from another Group must not move the local Node");
}

void role_changes_cannot_reanchor_committed_spatial_profiles() {
  const auto owner = identity("site-home", "namo-node-owner");
  SiteConfigurationSnapshot current{};
  require(initialize_site_configuration(owner, false, &current) ==
              SiteConfigurationResult::OK,
          "spatial reanchor bootstrap failed");
  ParticipantRegistry participants{};
  add_participant(&participants, "namo-node-peer",
                  ParticipantMembership::ACTIVE);
  require(commit_local_site_roster(owner, participants, current, &current) ==
              SiteConfigurationResult::OK &&
              commit_local_floorplan_manifest(
                  owner, current, "floorplan-main", 1, 10, 20, &current) ==
                  SiteConfigurationResult::OK,
          "spatial reanchor fixture failed");

  auto &local = *const_cast<SiteConfigurationMember *>(
      current.find(owner.node_id));
  local.spatial_profile.committed = true;
  local.spatial_profile.pose_version = 2;
  local.spatial_profile.calibration_version = 4;
  local.spatial_profile.transform = transform(1500, 700, 300);

  ParticipantReadinessRegistry readiness{};
  ParticipantReadinessConfig peer{};
  peer.capability_flags =
      PARTICIPANT_CAP_POINT_TRACKS | PARTICIPANT_CAP_PRESENCE;
  peer.max_targets = 3;
  std::strcpy(peer.floorplan_id, "floorplan-main");
  std::strcpy(peer.space_id, "room-main");
  std::strcpy(peer.group_id, "room-main");
  peer.geometry_version = 1;
  peer.local_pose_version = 2;
  peer.pose_version = 3;
  peer.calibration_version = 4;
  peer.transform = transform(2000, 700, 0);
  require(readiness.configure("namo-node-peer", peer) ==
              ParticipantReadinessResult::OK,
          "same calibration readiness failed");

  SiteConfigurationSnapshot updated{};
  bool changed = false;
  require(reconcile_local_spatial_profiles(
              owner, readiness, 2, transform(1000, 700, 0), current,
              SpatialProfileReconciliationMode::MISSING_ONLY, nullptr,
              &updated, &changed) == SiteConfigurationResult::OK &&
              updated.find(owner.node_id)
                      ->spatial_profile.transform.origin_x_mm == 1500,
          "passive reconciliation changed a handed-over local transform");

  peer.calibration_version = 7;
  peer.transform = transform(2050, 700, 0);
  require(readiness.configure("namo-node-peer", peer) ==
              ParticipantReadinessResult::OK &&
              reconcile_local_spatial_profiles(
                  owner, readiness, 2, transform(1000, 700, 0), updated,
                  SpatialProfileReconciliationMode::MISSING_ONLY, nullptr,
                  &updated, &changed) == SiteConfigurationResult::OK &&
              updated.find(owner.node_id)
                      ->spatial_profile.transform.origin_x_mm == 1500 &&
              updated.find(owner.node_id)
                      ->spatial_profile.calibration_version == 4 &&
              updated.find("namo-node-peer")
                      ->spatial_profile.calibration_version == 4,
          "a role-driven refresh accepted an uncommitted readiness version");

  require(reconcile_local_spatial_profiles(
              owner, readiness, 2, transform(1000, 700, 0), updated,
              SpatialProfileReconciliationMode::EXPLICIT_CALIBRATION,
              "namo-node-peer", &updated, &changed) ==
              SiteConfigurationResult::OK &&
              updated.find(owner.node_id)
                      ->spatial_profile.transform.origin_x_mm == 1000 &&
              updated.find(owner.node_id)
                      ->spatial_profile.calibration_version == 5 &&
              updated.find("namo-node-peer")
                      ->spatial_profile.calibration_version == 7 &&
              updated.find("namo-node-peer")
                      ->spatial_profile.transform.origin_x_mm == 2050,
          "an explicit calibration did not update only the calibrated pair");

  const auto successor = identity("site-home", "namo-node-peer");
  SiteConfigurationSnapshot handed_over{};
  require(prepare_automatic_configuration_handover(
              successor, updated, &handed_over, "namo-node-peer") ==
              SiteConfigurationResult::OK &&
              handed_over.find(owner.node_id)
                      ->spatial_profile.calibration_version == 5 &&
              handed_over.find(owner.node_id)
                      ->spatial_profile.transform.origin_x_mm == 1000 &&
              handed_over.find("namo-node-peer")
                      ->spatial_profile.calibration_version == 7 &&
              handed_over.find("namo-node-peer")
                      ->spatial_profile.transform.origin_x_mm == 2050,
          "owner handover changed committed spatial profiles");
}

void canonical_profiles_repair_divergent_direct_readiness() {
  const auto local = identity("site-home", "namo-node-local");
  SiteConfigurationSnapshot configuration{};
  require(initialize_site_configuration(local, false, &configuration) ==
              SiteConfigurationResult::OK,
          "direct readiness bootstrap failed");
  ParticipantRegistry participants{};
  add_participant(&participants, "namo-node-peer",
                  ParticipantMembership::ACTIVE);
  require(commit_local_site_roster(local, participants, configuration,
                                   &configuration) ==
              SiteConfigurationResult::OK &&
              commit_local_floorplan_manifest(
                  local, configuration, "floorplan-main", 3, 10, 20,
                  &configuration) == SiteConfigurationResult::OK,
          "direct readiness fixture failed");
  auto *local_member = mutable_member(&configuration, local.node_id);
  auto *remote_member = mutable_member(&configuration, "namo-node-peer");
  require(local_member != nullptr && remote_member != nullptr,
          "direct readiness canonical members are missing");
  local_member->spatial_profile.committed = true;
  std::strcpy(local_member->spatial_profile.space_id, "room-main");
  std::strcpy(local_member->spatial_profile.group_id, "room-main");
  local_member->spatial_profile.pose_version = 5;
  local_member->spatial_profile.calibration_version = 2;
  local_member->spatial_profile.transform = transform(1000, 700, 0);
  remote_member->spatial_profile.committed = true;
  std::strcpy(remote_member->spatial_profile.space_id, "room-main");
  std::strcpy(remote_member->spatial_profile.group_id, "room-main");
  remote_member->spatial_profile.pose_version = 4;
  remote_member->spatial_profile.calibration_version = 7;
  remote_member->spatial_profile.transform = transform(2200, 900, -200);

  ParticipantReadinessRegistry readiness{};
  ParticipantReadinessConfig stale{};
  stale.capability_flags =
      PARTICIPANT_CAP_POINT_TRACKS | PARTICIPANT_CAP_PRESENCE;
  stale.max_targets = 3;
  std::strcpy(stale.floorplan_id, "floorplan-main");
  std::strcpy(stale.space_id, "room-main");
  std::strcpy(stale.group_id, "room-main");
  stale.geometry_version = 3;
  stale.local_pose_version = 3;
  stale.pose_version = 4;
  stale.calibration_version = 2;
  stale.transform = transform(1800, 800, 0);
  require(readiness.configure("namo-node-peer", stale) ==
              ParticipantReadinessResult::OK,
          "stale direct readiness setup failed");

  ParticipantReadinessRegistry repaired{};
  bool changed = false;
  require(reconcile_direct_spatial_readiness(
              local, participants, configuration, readiness, &repaired,
              &changed) == SiteConfigurationResult::OK && changed,
          "canonical direct readiness repair failed");
  const auto *profile = repaired.find("namo-node-peer");
  require(profile != nullptr && profile->config.local_pose_version == 5 &&
              profile->config.pose_version == 4 &&
              profile->config.calibration_version == 7 &&
              profile->config.transform.origin_x_mm == 2200,
          "direct readiness did not converge to the canonical profile");

  stale.calibration_version = 8;
  stale.transform = transform(2400, 900, -200);
  require(readiness.configure("namo-node-peer", stale) ==
              ParticipantReadinessResult::OK &&
              reconcile_direct_spatial_readiness(
                  local, participants, configuration, readiness, &repaired,
                  &changed) == SiteConfigurationResult::OK && changed &&
              repaired.find("namo-node-peer")
                      ->config.calibration_version == 7 &&
              repaired.find("namo-node-peer")
                      ->config.transform.origin_x_mm == 2200,
          "a divergent newer direct cache did not return to canonical state");
}

void only_owner_can_clear_the_shared_floorplan() {
  const auto local = identity("site-home", "namo-node-local");
  SiteConfigurationSnapshot current{};
  require(initialize_site_configuration(local, false, &current) ==
              SiteConfigurationResult::OK,
          "floorplan clear bootstrap failed");
  SiteConfigurationSnapshot committed{};
  require(commit_local_floorplan_manifest(
              local, current, "floorplan-main", 4, 0x11223344U,
              0x55667788U, &committed) == SiteConfigurationResult::OK &&
              committed.floorplan.committed,
          "floorplan manifest fixture failed");

  SiteConfigurationSnapshot cleared{};
  require(clear_local_floorplan_manifest(local, committed, &cleared) ==
              SiteConfigurationResult::OK &&
              !cleared.floorplan.committed &&
              cleared.floorplan.floorplan_id[0] == '\0' &&
              cleared.floorplan.geometry_version == 0 &&
              cleared.floorplan.config_hash == 0 &&
              cleared.floorplan.image_hash == 0,
          "the owner must be able to publish an empty floorplan state");

  auto foreign = local;
  std::strcpy(foreign.node_id, "namo-node-foreign");
  require(clear_local_floorplan_manifest(foreign, committed, &cleared) ==
              SiteConfigurationResult::NOT_OWNER,
          "a follower must not clear the shared floorplan");
}

void exact_floorplan_manifest_match_is_idempotent() {
  const auto owner = identity("site-home", "namo-node-owner");
  SiteConfigurationSnapshot current{};
  require(initialize_site_configuration(owner, false, &current) ==
              SiteConfigurationResult::OK &&
              commit_local_floorplan_manifest(
                  owner, current, "floorplan-main", 4, 0x11223344U,
                  0x55667788U, &current) == SiteConfigurationResult::OK,
          "floorplan match fixture failed");

  require(site_floorplan_manifest_matches(
              current, "floorplan-main", 4, 0x11223344U, 0x55667788U),
          "an exact persisted manifest should be an idempotent match");
  require(!site_floorplan_manifest_matches(
              current, "floorplan-main", 5, 0x11223344U, 0x55667788U) &&
              !site_floorplan_manifest_matches(
                  current, "floorplan-main", 4, 0x11223345U,
                  0x55667788U) &&
              !site_floorplan_manifest_matches(
                  current, "floorplan-other", 4, 0x11223344U,
                  0x55667788U),
          "a changed manifest must not bypass the continuity gate");
}

void config_only_floorplan_updates_preserve_spatial_calibration() {
  const auto owner = identity("site-home", "namo-node-owner");
  SiteConfigurationSnapshot current{};
  require(initialize_site_configuration(owner, false, &current) ==
              SiteConfigurationResult::OK,
          "config-only floorplan bootstrap failed");

  ParticipantRegistry participants{};
  add_participant(&participants, "namo-node-peer",
                  ParticipantMembership::ACTIVE);
  require(commit_local_site_roster(owner, participants, current, &current) ==
              SiteConfigurationResult::OK &&
              commit_local_floorplan_manifest(
                  owner, current, "floorplan-main", 4, 0x11223344U,
                  0x55667788U, &current) == SiteConfigurationResult::OK,
          "config-only floorplan fixture failed");

  for (auto &member : current.members) {
    if (!member.occupied)
      continue;
    member.spatial_profile.committed = true;
    std::strcpy(member.spatial_profile.space_id, "room-main");
    std::strcpy(member.spatial_profile.group_id, "room-main");
    member.spatial_profile.pose_version = 3;
    member.spatial_profile.calibration_version = 7;
    member.spatial_profile.transform = transform(1200, 900, 150);
  }
  auto *peer = mutable_member(&current, "namo-node-peer");
  require(peer != nullptr, "config-only floorplan peer fixture missing");
  peer->spatial_profile.transform = transform(300, 200, -50);

  SiteConfigurationSnapshot room_edit{};
  require(commit_local_floorplan_manifest(
              owner, current, "floorplan-main", 4, 0x11223345U,
              0x55667788U, &room_edit) == SiteConfigurationResult::OK,
          "room-layout manifest update failed");
  for (const auto &member : room_edit.members) {
    if (!member.occupied)
      continue;
    require(member.spatial_profile.committed &&
                member.spatial_profile.calibration_version == 7 &&
                member.spatial_profile.transform.valid &&
                member.spatial_profile.transform.origin_x_mm ==
                    (std::strcmp(member.participant_id, "namo-node-peer") == 0
                         ? 300
                         : 1200),
            "a CONFIG-only floorplan update erased spatial calibration");
  }

  SiteConfigurationSnapshot scale_edit{};
  require(commit_local_floorplan_manifest(
              owner, room_edit, "floorplan-main", 5, 0x11223346U,
              0x55667788U, &scale_edit, 1000, 800) ==
              SiteConfigurationResult::OK,
          "coordinate-frame manifest update failed");
  for (const auto &member : scale_edit.members) {
    if (!member.occupied)
      continue;
    const bool peer_member =
        std::strcmp(member.participant_id, "namo-node-peer") == 0;
    require(member.spatial_profile.committed &&
                member.spatial_profile.calibration_version ==
                    (peer_member ? 7U : 8U) &&
                member.spatial_profile.transform.origin_x_mm ==
                    (peer_member ? 300 : 500) &&
                member.spatial_profile.transform.origin_y_mm ==
                    (peer_member ? 200 : 400),
            "coordinate-frame replacement did not preserve valid radar positions and center invalid ones");
  }
}

void connected_site_floorplan_delete_policy_tracks_non_revoked_peers() {
  const auto owner = identity("site-home", "namo-node-owner");
  SiteConfigurationSnapshot configuration{};
  require(initialize_site_configuration(owner, false, &configuration) ==
              SiteConfigurationResult::OK &&
              !site_configuration_has_remote_members(owner, configuration),
          "standalone floorplan delete policy fixture failed");

  configuration.members[1].occupied = true;
  std::strcpy(configuration.members[1].participant_id, "namo-node-peer");
  configuration.members[1].type = ParticipantType::NAMO_NODE;
  configuration.members[1].membership = ParticipantMembership::QUARANTINED;
  require(site_configuration_has_remote_members(owner, configuration),
          "a quarantined connected peer must block floorplan deletion");
  configuration.members[1].membership = ParticipantMembership::ACTIVE;
  require(site_configuration_has_remote_members(owner, configuration),
          "an active connected peer must block floorplan deletion");
  configuration.members[1].membership = ParticipantMembership::REVOKED;
  require(!site_configuration_has_remote_members(owner, configuration),
          "a revoked peer must not block standalone floorplan deletion");
}

void site_move_allows_only_a_device_without_live_dependents() {
  const auto local = identity("site-old", "namo-node-local");
  SiteConfigurationSnapshot standalone{};
  require(initialize_site_configuration(local, false, &standalone) ==
              SiteConfigurationResult::OK,
          "site move bootstrap failed");

  ParticipantRegistry revoked_only{};
  add_participant(&revoked_only, "namo-node-old",
                  ParticipantMembership::REVOKED);
  require(can_move_site_without_orphaning_participants(
              revoked_only, standalone),
          "revoked records must not block leaving a one-device Site");

  ParticipantRegistry retry_only{};
  add_participant(&retry_only, "namo-node-inviter",
                  ParticipantMembership::QUARANTINED);
  require(can_move_site_without_orphaning_participants(
              retry_only, standalone, "namo-node-inviter"),
          "the same interrupted pairing may be retried");
  require(!can_move_site_without_orphaning_participants(
              retry_only, standalone),
          "an unrelated live connection must block Site movement");

  ParticipantRegistry active_peer{};
  add_participant(&active_peer, "namo-node-dependent",
                  ParticipantMembership::ACTIVE);
  SiteConfigurationSnapshot multi_member{};
  require(commit_local_site_roster(local, active_peer, standalone,
                                   &multi_member) ==
              SiteConfigurationResult::OK &&
              multi_member.active_member_count() == 2,
          "multi-member Site fixture failed");
  require(!can_move_site_without_orphaning_participants(
              ParticipantRegistry{}, multi_member),
          "a device that owns another active member must not leave its Site");
}

void pairing_reclaims_authority_only_from_the_same_single_peer() {
  const auto local = identity("site-home", "namo-node-local");
  SiteConfigurationSnapshot current{};
  require(initialize_site_configuration(local, false, &current) ==
              SiteConfigurationResult::OK,
          "pairing reclaim bootstrap failed");
  std::strcpy(current.configuration_owner_node_id, "namo-node-peer");
  current.members[1].occupied = true;
  std::strcpy(current.members[1].participant_id, "namo-node-peer");
  current.members[1].type = ParticipantType::NAMO_NODE;
  current.members[1].trust = ParticipantTrust::COMMUNITY_UNVERIFIED;
  current.members[1].membership = ParticipantMembership::ACTIVE;
  current.members[1].leader_eligible = true;
  current.members[0].membership = ParticipantMembership::QUARANTINED;
  current.members[0].leader_eligible = false;
  current.floorplan.committed = true;
  std::strcpy(current.floorplan.floorplan_id, "floorplan-main");
  current.floorplan.geometry_version = 3;
  current.floorplan.config_hash = 0x1234U;
  current.floorplan.image_hash = 0x5678U;

  ParticipantRegistry paired{};
  add_participant(&paired, "namo-node-peer",
                  ParticipantMembership::QUARANTINED);
  SiteConfigurationSnapshot reclaimed{};
  require(prepare_local_pairing_site(
              local, paired, current, "namo-node-peer", &reclaimed) ==
              SiteConfigurationResult::OK,
          "the same two devices must be able to reverse pairing roles");
  require(std::strcmp(reclaimed.configuration_owner_node_id, local.node_id) ==
              0 &&
              reclaimed.roster_committed && reclaimed.member_count() == 2 &&
              reclaimed.find("namo-node-peer") != nullptr &&
              reclaimed.floorplan.committed &&
              std::strcmp(reclaimed.floorplan.floorplan_id,
                          "floorplan-main") == 0,
          "role reversal must preserve the floorplan and commit one owner");

  ParticipantRegistry third_party{};
  add_participant(&third_party, "namo-node-peer",
                  ParticipantMembership::QUARANTINED);
  add_participant(&third_party, "namo-node-third",
                  ParticipantMembership::ACTIVE);
  require(prepare_local_pairing_site(
              local, third_party, current, "namo-node-peer", &reclaimed) ==
              SiteConfigurationResult::NOT_OWNER,
          "role reversal must not orphan another connected device");

  std::strcpy(current.configuration_owner_node_id, "namo-node-third");
  std::strcpy(current.members[1].participant_id, "namo-node-third");
  require(prepare_local_pairing_site(
              local, paired, current, "namo-node-peer", &reclaimed) ==
              SiteConfigurationResult::NOT_OWNER,
          "a different configuration owner must block takeover");
}

void pairing_retry_preserves_only_an_exact_converged_joiner_site() {
  const auto owner = identity("site-home", "namo-node-owner");
  const auto joiner = identity("site-home", "namo-node-joiner");
  SiteConfigurationSnapshot configuration{};
  require(initialize_site_configuration(owner, false, &configuration) ==
              SiteConfigurationResult::OK,
          "pairing retry fixture bootstrap failed");
  ParticipantRegistry participants{};
  add_participant(&participants, joiner.node_id,
                  ParticipantMembership::QUARANTINED);
  require(commit_local_site_roster(owner, participants, configuration,
                                   &configuration) ==
              SiteConfigurationResult::OK,
          "pairing retry fixture roster failed");
  require(pairing_joiner_site_already_converged(
              joiner, configuration, owner.site_id, owner.node_id),
          "an exact quarantined joiner retry should preserve canonical state");

  require(!pairing_joiner_site_already_converged(
              joiner, configuration, owner.site_id, "namo-node-other"),
          "a different pairing peer must not reuse canonical state");
  require(!pairing_joiner_site_already_converged(
              joiner, configuration, "site-other", owner.node_id),
          "a different leader Site must not reuse canonical state");

  auto uncommitted = configuration;
  uncommitted.roster_committed = false;
  require(!pairing_joiner_site_already_converged(
              joiner, uncommitted, owner.site_id, owner.node_id),
          "an uncommitted roster must resume enrollment instead of being kept");

  auto revoked_joiner = configuration;
  mutable_member(&revoked_joiner, joiner.node_id)->membership =
      ParticipantMembership::REVOKED;
  require(!pairing_joiner_site_already_converged(
              joiner, revoked_joiner, owner.site_id, owner.node_id),
          "a revoked local member must not be treated as converged");

  auto inactive_owner = configuration;
  mutable_member(&inactive_owner, owner.node_id)->membership =
      ParticipantMembership::QUARANTINED;
  require(!pairing_joiner_site_already_converged(
              joiner, inactive_owner, owner.site_id, owner.node_id),
          "a non-active owner must not anchor an idempotent retry");
}

void empty_registry_reconciles_to_local_owner_without_changing_site_data() {
  const auto local = identity("site-home", "namo-node-local");
  SiteConfigurationSnapshot current{};
  require(initialize_site_configuration(local, false, &current) ==
              SiteConfigurationResult::OK,
          "standalone reconciliation bootstrap failed");
  std::strcpy(current.configuration_owner_node_id, "namo-node-old-owner");
  current.members[1].occupied = true;
  std::strcpy(current.members[1].participant_id, "namo-node-old-owner");
  current.members[1].type = ParticipantType::NAMO_NODE;
  current.members[1].trust = ParticipantTrust::COMMUNITY_UNVERIFIED;
  current.members[1].membership = ParticipantMembership::ACTIVE;
  current.members[1].leader_eligible = true;
  current.members[0].membership = ParticipantMembership::QUARANTINED;
  current.members[0].leader_eligible = false;
  current.floorplan.committed = true;
  std::strcpy(current.floorplan.floorplan_id, "floorplan-main");
  current.floorplan.geometry_version = 9;
  current.floorplan.config_hash = 0x12345678U;
  current.floorplan.image_hash = 0x87654321U;

  ParticipantRegistry empty{};
  require(!is_local_standalone_site_configuration(local, empty, current),
          "a remote owner must not be accepted as standalone");
  SiteConfigurationSnapshot reconciled{};
  require(reconcile_local_standalone_site_configuration(
              local, empty, current, &reconciled) ==
              SiteConfigurationResult::OK,
          "empty registry reconciliation failed");
  require(is_local_standalone_site_configuration(local, empty, reconciled) &&
              std::strcmp(reconciled.site_id, current.site_id) == 0 &&
              reconciled.member_count() == 1 &&
              reconciled.roster_revision == current.roster_revision + 1,
          "reconciliation must retain the Site and publish one local member");
  require(reconciled.floorplan.committed &&
              std::strcmp(reconciled.floorplan.floorplan_id,
                          current.floorplan.floorplan_id) == 0 &&
              reconciled.floorplan.geometry_version ==
                  current.floorplan.geometry_version &&
              reconciled.floorplan.config_hash ==
                  current.floorplan.config_hash &&
              reconciled.floorplan.image_hash ==
                  current.floorplan.image_hash,
          "reconciliation must preserve the local floorplan manifest");

  FakeBackend backend{};
  SiteConfigurationStore store{};
  require(store.create(backend, current, &current) ==
              SiteConfigurationStoreResult::CREATED &&
              store.update(backend, reconciled, &reconciled) ==
                  SiteConfigurationStoreResult::UPDATED,
          "reconciled standalone Site persistence failed");
  SiteConfigurationSnapshot loaded{};
  require(store.load(backend, &loaded) ==
              SiteConfigurationStoreResult::LOADED &&
              is_local_standalone_site_configuration(local, empty, loaded) &&
              std::strcmp(loaded.site_id, "site-home") == 0 &&
              loaded.floorplan.committed &&
              std::strcmp(loaded.floorplan.floorplan_id,
                          "floorplan-main") == 0,
          "reconciled standalone Site must survive reload");
}

void standalone_reconciliation_restores_a_missing_local_member() {
  const auto local = identity("site-home", "namo-node-local");
  SiteConfigurationSnapshot current{};
  require(initialize_site_configuration(local, false, &current) ==
              SiteConfigurationResult::OK,
          "missing-local reconciliation bootstrap failed");
  std::strcpy(current.configuration_owner_node_id, "namo-node-old-owner");
  current.members = {};
  current.members[0].occupied = true;
  std::strcpy(current.members[0].participant_id, "namo-node-old-owner");
  current.members[0].type = ParticipantType::NAMO_NODE;
  current.members[0].trust = ParticipantTrust::COMMUNITY_UNVERIFIED;
  current.members[0].membership = ParticipantMembership::ACTIVE;
  current.members[0].leader_eligible = true;

  SiteConfigurationSnapshot reconciled{};
  require(reconcile_local_standalone_site_configuration(
              local, ParticipantRegistry{}, current, &reconciled) ==
              SiteConfigurationResult::OK,
          "a stale roster without the local member must be recoverable");
  const auto *restored = reconciled.find(local.node_id);
  require(restored != nullptr &&
              restored->trust == ParticipantTrust::COMMUNITY_UNVERIFIED &&
              restored->membership == ParticipantMembership::ACTIVE &&
              restored->leader_eligible && reconciled.member_count() == 1 &&
              std::strcmp(reconciled.configuration_owner_node_id,
                          local.node_id) == 0,
          "standalone recovery must rebuild one authoritative local member");
}

void standalone_reconciliation_ignores_revoked_records_but_refuses_live_peers() {
  const auto local = identity("site-home", "namo-node-local");
  SiteConfigurationSnapshot current{};
  require(initialize_site_configuration(local, false, &current) ==
              SiteConfigurationResult::OK,
          "standalone guard bootstrap failed");

  ParticipantRegistry revoked_only{};
  add_participant(&revoked_only, "namo-node-revoked",
                  ParticipantMembership::REVOKED);
  SiteConfigurationSnapshot reconciled{};
  require(reconcile_local_standalone_site_configuration(
              local, revoked_only, current, &reconciled) ==
              SiteConfigurationResult::OK &&
              is_local_standalone_site_configuration(
                  local, revoked_only, reconciled),
          "revoked records must not prevent local-only recovery");

  ParticipantRegistry live{};
  add_participant(&live, "namo-node-live",
                  ParticipantMembership::QUARANTINED);
  require(reconcile_local_standalone_site_configuration(
              local, live, current, &reconciled) ==
              SiteConfigurationResult::INVALID_CONFIGURATION,
          "a live pairing record must block automatic standalone recovery");
}

void standalone_reconciliation_tracker_runs_once_and_handles_wraparound() {
  StandaloneSiteReconciliationTracker tracker{};
  require(tracker.ready_for_pairing() &&
              tracker.state() ==
                  StandaloneSiteReconciliationState::NOT_REQUIRED,
          "an unused reconciliation tracker must not block pairing");

  tracker.schedule(1000, 1000);
  require(!tracker.ready_for_pairing() && !tracker.due(1999) &&
              tracker.due(2000) &&
              std::strcmp(standalone_site_reconciliation_state_to_string(
                              tracker.state()),
                          "pending") == 0,
          "scheduled reconciliation must become due exactly once");
  tracker.complete(StandaloneSiteReconciliationResult::OK);
  require(tracker.ready_for_pairing() && !tracker.due(3000) &&
              tracker.state() ==
                  StandaloneSiteReconciliationState::COMPLETE,
          "successful reconciliation must stop the one-shot schedule");

  tracker.schedule(0xFFFFFFF0U, 32U);
  require(!tracker.due(0x0000000FU) && tracker.due(0x00000010U),
          "reconciliation deadline must remain valid across millis wrap");
  tracker.complete(
      StandaloneSiteReconciliationResult::SITE_STORAGE_FAILED);
  require(!tracker.ready_for_pairing() && !tracker.due(0x00000020U) &&
              tracker.state() ==
                  StandaloneSiteReconciliationState::FAILED &&
              std::strcmp(standalone_site_reconciliation_result_to_string(
                              tracker.result()),
                          "site_storage_failed") == 0,
          "a failed one-shot must stay failed without an automatic retry");
  tracker.mark_not_required();
  require(tracker.ready_for_pairing() &&
              tracker.result() ==
                  StandaloneSiteReconciliationResult::IDLE,
          "a live participant relationship must clear stale recovery state");
}

void codec_round_trip_and_corruption_are_bounded() {
  SiteConfigurationSnapshot source{};
  require(initialize_site_configuration(
              identity("site-home", "namo-node-local"), false, &source) ==
              SiteConfigurationResult::OK,
          "codec source bootstrap failed");
  std::array<uint8_t, SiteConfigurationCodec::MAX_ENCODED_SIZE> bytes{};
  std::size_t size = 0;
  require(SiteConfigurationCodec::encode(source, bytes.data(), bytes.size(),
                                         &size) ==
              SiteConfigurationCodecResult::OK &&
              size < bytes.size(),
          "configuration encode failed");
  SiteConfigurationSnapshot decoded{};
  require(SiteConfigurationCodec::decode(bytes.data(), bytes.size(), &decoded) ==
              SiteConfigurationCodecResult::OK &&
              std::strcmp(decoded.configuration_owner_node_id,
                          "namo-node-local") == 0,
          "configuration round trip failed");

  std::memmove(bytes.data() + 32, bytes.data() + 36, size - 36);
  size -= 4;
  bytes[4] = 1;
  bytes[5] = 0;
  write_u32(bytes.data() + size - 4, fnv1a(bytes.data(), size - 4));
  std::fill(bytes.begin() + size, bytes.end(), 0xFF);
  require(SiteConfigurationCodec::decode(bytes.data(), bytes.size(), &decoded) ==
              SiteConfigurationCodecResult::OK &&
              !decoded.members[0].spatial_profile.committed,
          "version 1 Site configuration must remain readable");

  require(SiteConfigurationCodec::encode(source, bytes.data(), bytes.size(),
                                         &size) ==
              SiteConfigurationCodecResult::OK,
          "configuration re-encode failed");
  bytes[4] = static_cast<uint8_t>(
      SiteConfigurationCodec::STORAGE_VERSION & 0xFFU);
  bytes[5] = static_cast<uint8_t>(
      SiteConfigurationCodec::STORAGE_VERSION >> 8U);
  write_u32(bytes.data() + size - 4, fnv1a(bytes.data(), size - 4));
  bytes[20] ^= 0x01;
  require(SiteConfigurationCodec::decode(bytes.data(), bytes.size(), &decoded) ==
              SiteConfigurationCodecResult::CORRUPT,
          "configuration corruption must fail closed");
}

void version_two_spatial_profiles_migrate_without_guessing_assignments() {
  const auto local = identity("site-home", "namo-node-local");
  SiteConfigurationSnapshot source{};
  require(initialize_site_configuration(local, false, &source) ==
              SiteConfigurationResult::OK &&
              commit_local_floorplan_manifest(
                  local, source, "floorplan-main", 1, 10, 20, &source) ==
                  SiteConfigurationResult::OK,
          "version two fixture bootstrap failed");
  auto *member = mutable_member(&source, local.node_id);
  require(member != nullptr, "version two fixture member missing");
  member->spatial_profile.committed = true;
  member->spatial_profile.pose_version = 2;
  member->spatial_profile.calibration_version = 3;
  member->spatial_profile.transform = transform(1000, 800, 0);

  std::array<uint8_t, SiteConfigurationCodec::MAX_ENCODED_SIZE> bytes{};
  std::size_t size = 0;
  require(SiteConfigurationCodec::encode(source, bytes.data(), bytes.size(),
                                         &size) ==
              SiteConfigurationCodecResult::OK,
          "version three fixture encode failed");
  std::memmove(bytes.data() + 32, bytes.data() + 36, size - 36);
  size -= 4;
  const std::size_t profile_offset =
      32 + std::strlen(source.site_id) +
      std::strlen(source.configuration_owner_node_id) +
      std::strlen(source.floorplan.floorplan_id) + 5 +
      std::strlen(member->participant_id);
  require(bytes[profile_offset] == 0 && bytes[profile_offset + 1] == 0,
          "legacy fixture requires empty canonical assignments");
  std::memmove(bytes.data() + profile_offset, bytes.data() + profile_offset + 2,
               size - profile_offset - 2);
  size -= 2;
  bytes[4] = 2;
  bytes[5] = 0;
  write_u32(bytes.data() + size - 4, fnv1a(bytes.data(), size - 4));
  std::fill(bytes.begin() + size, bytes.end(), 0xFF);

  SiteConfigurationSnapshot decoded{};
  require(SiteConfigurationCodec::decode(bytes.data(), bytes.size(),
                                         &decoded) ==
              SiteConfigurationCodecResult::OK,
          "version two spatial profile must remain readable");
  const auto *decoded_member = decoded.find(local.node_id);
  require(decoded_member != nullptr &&
              decoded_member->spatial_profile.committed &&
              decoded_member->spatial_profile.space_id[0] == '\0' &&
              decoded_member->spatial_profile.group_id[0] == '\0' &&
              decoded_member->spatial_profile.pose_version == 2 &&
              decoded_member->spatial_profile.calibration_version == 3,
          "version two migration must preserve pose without inventing a group");
}

void transaction_keeps_previous_commit_after_interruption() {
  FakeBackend backend{};
  SiteConfigurationStore store{};
  SiteConfigurationSnapshot initial{};
  require(initialize_site_configuration(
              identity("site-home", "namo-node-local"), false, &initial) ==
              SiteConfigurationResult::OK,
          "transaction source bootstrap failed");
  require(store.create(backend, initial, &initial) ==
              SiteConfigurationStoreResult::CREATED,
          "initial configuration commit failed");

  ParticipantRegistry registry{};
  add_participant(&registry, "namo-node-peer", ParticipantMembership::ACTIVE);
  SiteConfigurationSnapshot updated{};
  require(commit_local_site_roster(
              identity("site-home", "namo-node-local"), registry, initial,
              &updated) == SiteConfigurationResult::OK,
          "updated roster build failed");
  backend.fail_commit_write = true;
  require(store.update(backend, updated, nullptr) ==
              SiteConfigurationStoreResult::STORAGE_FAILED,
          "interrupted commit must report storage failure");
  backend.fail_commit_write = false;
  SiteConfigurationSnapshot loaded{};
  require(store.load(backend, &loaded) == SiteConfigurationStoreResult::LOADED &&
              loaded.member_count() == 1 && loaded.generation == 1,
          "the previous committed roster must survive an interrupted update");
}

void transaction_keeps_previous_commit_when_slot_write_fails() {
  FakeBackend backend{};
  SiteConfigurationStore store{};
  SiteConfigurationSnapshot initial{};
  require(initialize_site_configuration(
              identity("site-home", "namo-node-local"), false, &initial) ==
              SiteConfigurationResult::OK &&
              store.create(backend, initial, &initial) ==
                  SiteConfigurationStoreResult::CREATED,
          "slot failure transaction bootstrap failed");

  ParticipantRegistry registry{};
  add_participant(&registry, "namo-node-peer",
                  ParticipantMembership::ACTIVE);
  SiteConfigurationSnapshot updated{};
  require(commit_local_site_roster(
              identity("site-home", "namo-node-local"), registry, initial,
              &updated) == SiteConfigurationResult::OK,
          "slot failure roster build failed");
  backend.fail_slot_write = true;
  require(store.update(backend, updated, nullptr) ==
              SiteConfigurationStoreResult::STORAGE_FAILED,
          "failed inactive slot write must report storage failure");
  backend.fail_slot_write = false;

  SiteConfigurationSnapshot loaded{};
  require(store.load(backend, &loaded) ==
              SiteConfigurationStoreResult::LOADED &&
              loaded.generation == initial.generation &&
              loaded.member_count() == 1,
          "failed inactive slot write must preserve the committed Site");
}

void enrollment_restarts_generation_without_erasing_the_previous_commit() {
  FakeBackend backend{};
  SiteConfigurationStore store{};
  SiteConfigurationSnapshot standalone{};
  require(initialize_site_configuration(
              identity("site-old", "namo-node-joiner"), false,
              &standalone) == SiteConfigurationResult::OK &&
              store.create(backend, standalone, &standalone) ==
                  SiteConfigurationStoreResult::CREATED,
          "enrollment transaction bootstrap failed");
  SiteConfigurationSnapshot incremented = standalone;
  require(store.update(backend, incremented, &standalone) ==
              SiteConfigurationStoreResult::UPDATED &&
              standalone.generation == 2,
          "standalone generation fixture failed");

  SiteConfigurationSnapshot joined{};
  require(initialize_site_configuration(
              identity("site-new", "namo-node-joiner"), true, &joined) ==
              SiteConfigurationResult::OK,
          "joined configuration fixture failed");
  std::strcpy(joined.configuration_owner_node_id, "namo-node-owner");
  auto &owner = joined.members[1];
  owner.occupied = true;
  std::strcpy(owner.participant_id, "namo-node-owner");
  owner.type = ParticipantType::NAMO_NODE;
  owner.trust = ParticipantTrust::COMMUNITY_UNVERIFIED;
  owner.membership = ParticipantMembership::ACTIVE;
  owner.leader_eligible = true;

  backend.fail_commit_write = true;
  require(store.replace_for_enrollment(backend, joined, nullptr) ==
              SiteConfigurationStoreResult::STORAGE_FAILED,
          "interrupted enrollment replacement must fail");
  backend.fail_commit_write = false;
  SiteConfigurationSnapshot loaded{};
  require(store.load(backend, &loaded) == SiteConfigurationStoreResult::LOADED &&
              std::strcmp(loaded.site_id, "site-old") == 0 &&
              loaded.generation == 2,
          "interrupted enrollment must preserve the prior Site");

  require(store.replace_for_enrollment(backend, joined, &loaded) ==
              SiteConfigurationStoreResult::UPDATED &&
              std::strcmp(loaded.site_id, "site-new") == 0 &&
              loaded.generation == 1,
          "a successful Site enrollment must start a fresh generation domain");
}

void local_connection_reset_replaces_an_older_authority_domain_atomically() {
  FakeBackend backend{};
  SiteConfigurationStore store{};
  SiteConfigurationSnapshot connected{};
  require(initialize_site_configuration(
              identity("site-connected", "namo-node-local"), false,
              &connected) == SiteConfigurationResult::OK,
          "connection reset bootstrap failed");
  connected.authority_epoch = 7;
  require(store.create(backend, connected, &connected) ==
              SiteConfigurationStoreResult::CREATED,
          "connection reset persisted fixture failed");

  SiteConfigurationSnapshot standalone{};
  require(initialize_site_configuration(
              identity("site-standalone", "namo-node-local"), false,
              &standalone) == SiteConfigurationResult::OK &&
              store.update(backend, standalone, nullptr) ==
                  SiteConfigurationStoreResult::INVALID_CONFIGURATION,
          "ordinary update unexpectedly accepted a reset to an older authority domain");

  backend.fail_commit_write = true;
  require(store.replace_for_local_reset(backend, standalone, nullptr) ==
              SiteConfigurationStoreResult::STORAGE_FAILED,
          "interrupted local reset replacement must fail");
  backend.fail_commit_write = false;
  SiteConfigurationSnapshot loaded{};
  require(store.load(backend, &loaded) ==
              SiteConfigurationStoreResult::LOADED &&
              std::strcmp(loaded.site_id, "site-connected") == 0 &&
              loaded.authority_epoch == 7,
          "interrupted local reset did not preserve the prior committed Site");

  require(store.replace_for_local_reset(backend, standalone, &loaded) ==
              SiteConfigurationStoreResult::UPDATED &&
              std::strcmp(loaded.site_id, "site-standalone") == 0 &&
              loaded.authority_epoch == 1 && loaded.generation == 1 &&
              loaded.member_count() == 1 &&
              std::strcmp(loaded.configuration_owner_node_id,
                          "namo-node-local") == 0,
          "local reset did not commit a fresh standalone authority domain");
}

void standalone_reconciliation_finishes_an_interrupted_site_reset() {
  SiteConfigurationSnapshot previous{};
  require(initialize_site_configuration(
              identity("site-previous", "namo-node-local"), false,
              &previous) == SiteConfigurationResult::OK,
          "interrupted reset reconciliation fixture failed");
  ParticipantRegistry empty{};
  SiteConfigurationSnapshot recovered{};
  const auto reset_identity =
      identity("site-reset-target", "namo-node-local");
  require(reconcile_local_standalone_site_configuration(
              reset_identity, empty, previous, &recovered) ==
              SiteConfigurationResult::OK &&
              is_local_standalone_site_configuration(reset_identity, empty,
                                                     recovered) &&
              std::strcmp(recovered.site_id, "site-reset-target") == 0 &&
              recovered.generation == 1,
          "standalone reconciliation did not finish an interrupted Site reset");

  ParticipantRegistry still_connected{};
  add_participant(&still_connected, "namo-node-peer",
                  ParticipantMembership::ACTIVE);
  require(reconcile_local_standalone_site_configuration(
              reset_identity, still_connected, previous, &recovered) ==
              SiteConfigurationResult::INVALID_CONFIGURATION,
          "mismatched Site recovery must not discard a live participant roster");
}

void twenty_members_fit_the_contract() {
  const auto local = identity("site-home", "namo-node-local");
  SiteConfigurationSnapshot current{};
  require(initialize_site_configuration(local, false, &current) ==
              SiteConfigurationResult::OK,
          "twenty-member bootstrap failed");
  ParticipantRegistry registry{};
  char id[32]{};
  for (std::size_t index = 0;
       index < ParticipantRegistry::MAX_REMOTE_PARTICIPANTS; ++index) {
    std::snprintf(id, sizeof(id), "namo-node-%02u",
                  static_cast<unsigned>(index));
    add_participant(&registry, id, ParticipantMembership::ACTIVE);
  }
  SiteConfigurationSnapshot updated{};
  require(commit_local_site_roster(local, registry, current, &updated) ==
              SiteConfigurationResult::OK &&
              updated.member_count() == ParticipantRegistry::MAX_PARTICIPANTS,
          "the canonical roster must support twenty members");
  updated.floorplan.committed = true;
  std::strcpy(updated.floorplan.floorplan_id, "floorplan-main");
  updated.floorplan.geometry_version = 1;
  updated.floorplan.config_hash = 10;
  updated.floorplan.image_hash = 20;
  for (auto &member : updated.members) {
    if (!member.occupied)
      continue;
    member.spatial_profile.committed = true;
    std::strcpy(member.spatial_profile.space_id,
                "space-0123456789012345678901234567890123");
    std::strcpy(member.spatial_profile.group_id,
                "group-0123456789012345678901234567890123");
    member.spatial_profile.pose_version = 1;
    member.spatial_profile.calibration_version = 1;
    member.spatial_profile.transform = transform(1000, 1000, 0);
  }
  std::array<uint8_t, SiteConfigurationCodec::MAX_ENCODED_SIZE> bytes{};
  std::size_t encoded_size = 0;
  require(SiteConfigurationCodec::encode(
              updated, bytes.data(), bytes.size(), &encoded_size) ==
              SiteConfigurationCodecResult::OK &&
              encoded_size <= SiteConfigurationCodec::MAX_ENCODED_SIZE,
          "twenty approved spatial profiles must fit one flash sector blob");
}

void automatic_handover_uses_one_deterministic_successor() {
  const auto owner = identity("site-home", "namo-node-owner");
  SiteConfigurationSnapshot current{};
  require(initialize_site_configuration(owner, false, &current) ==
              SiteConfigurationResult::OK,
          "automatic handover bootstrap failed");
  ParticipantRegistry registry{};
  add_participant(&registry, "namo-node-z", ParticipantMembership::ACTIVE);
  add_participant(&registry, "namo-node-a", ParticipantMembership::ACTIVE);
  require(commit_local_site_roster(owner, registry, current, &current) ==
              SiteConfigurationResult::OK,
          "automatic handover roster fixture failed");
  const auto *successor = automatic_configuration_successor(current);
  require(successor != nullptr &&
              std::strcmp(successor->participant_id, "namo-node-a") == 0,
          "successor selection must be deterministic and independent of insertion order");

  current.floorplan.committed = true;
  std::strcpy(current.floorplan.floorplan_id, "floorplan-main");
  current.floorplan.geometry_version = 7;
  current.floorplan.config_hash = 0x12345678U;
  current.floorplan.image_hash = 0x87654321U;
  for (auto &member : current.members) {
    if (!member.occupied)
      continue;
    member.spatial_profile.committed = true;
    member.spatial_profile.pose_version =
        std::strcmp(member.participant_id, "namo-node-a") == 0 ? 9 : 4;
    member.spatial_profile.calibration_version = 12;
    member.spatial_profile.transform =
        std::strcmp(member.participant_id, "namo-node-a") == 0
            ? transform(2450, 1380, -725)
            : transform(900, 700, 125);
  }
  SiteConfigurationSnapshot handed_over{};
  require(prepare_automatic_configuration_handover(
              identity("site-home", "namo-node-a"), current,
              &handed_over) == SiteConfigurationResult::OK &&
              std::strcmp(handed_over.configuration_owner_node_id,
                          "namo-node-a") == 0 &&
              handed_over.authority_epoch == current.authority_epoch + 1 &&
              handed_over.roster_revision == current.roster_revision &&
              site_topology_digest(handed_over) ==
                  site_topology_digest(current) &&
              handed_over.member_count() == current.member_count() &&
              handed_over.floorplan.config_hash ==
                  current.floorplan.config_hash &&
              handed_over.floorplan.image_hash ==
                  current.floorplan.image_hash &&
              handed_over.find("namo-node-a")
                      ->spatial_profile.pose_version == 9 &&
              handed_over.find("namo-node-a")
                      ->spatial_profile.calibration_version == 12 &&
              handed_over.find("namo-node-a")
                      ->spatial_profile.transform.origin_x_mm == 2450 &&
              handed_over.find("namo-node-a")
                      ->spatial_profile.transform.origin_y_mm == 1380 &&
              handed_over.find("namo-node-a")
                      ->spatial_profile.transform.rotation_cdeg == -725,
          "handover must preserve the canonical roster, floorplan, and "
          "approved transforms");
  require(prepare_automatic_configuration_handover(
              identity("site-home", "namo-node-z"), current,
              &handed_over) == SiteConfigurationResult::NOT_OWNER,
          "a non-designated Node must not claim automatic ownership");
}

void automatic_handover_bootstrap_accepts_only_an_exact_eligible_state() {
  const auto owner = identity("site-home", "namo-node-owner");
  SiteConfigurationSnapshot current{};
  require(initialize_site_configuration(owner, false, &current) ==
              SiteConfigurationResult::OK,
          "handover bootstrap fixture failed");
  ParticipantRegistry registry{};
  add_participant(&registry, "namo-node-z", ParticipantMembership::ACTIVE);
  add_participant(&registry, "namo-node-a", ParticipantMembership::ACTIVE);
  require(commit_local_site_roster(owner, registry, current, &current) ==
              SiteConfigurationResult::OK,
          "handover bootstrap roster failed");

  SiteConfigurationSnapshot successor{};
  require(prepare_automatic_configuration_handover(
              identity("site-home", "namo-node-a"), current,
              &successor) == SiteConfigurationResult::OK,
          "handover bootstrap successor build failed");
  ++successor.generation;
  const uint64_t digest = site_configuration_digest(successor);
  const uint64_t topology_digest = site_topology_digest(successor);
  const auto allowed = [&](const char *credential_participant_id,
                           const char *advertised_participant_id,
                           uint32_t epoch, uint32_t generation,
                           uint64_t configuration_digest,
                           uint32_t topology_revision,
                           uint64_t advertised_topology_digest) {
    return automatic_configuration_handover_bootstrap_allowed(
        current, credential_participant_id, advertised_participant_id,
        epoch, generation, configuration_digest, topology_revision,
        advertised_topology_digest);
  };

  require(allowed(owner.node_id, "namo-node-a", successor.authority_epoch,
                  successor.generation, digest, successor.roster_revision,
                  topology_digest),
          "the exact eligible successor state was rejected");
  SiteConfigurationSnapshot alternate{};
  require(prepare_automatic_configuration_handover(
              identity("site-home", "namo-node-z"), current, &alternate,
              "namo-node-z") == SiteConfigurationResult::OK,
          "alternate handover bootstrap state could not be built");
  ++alternate.generation;
  require(allowed(owner.node_id, "namo-node-z", alternate.authority_epoch,
                  alternate.generation,
                  site_configuration_digest(alternate),
                  alternate.roster_revision, site_topology_digest(alternate)),
          "an exact eligible alternate successor state was rejected");
  require(!allowed("namo-node-z", "namo-node-a",
                   successor.authority_epoch, successor.generation, digest,
                   successor.roster_revision, topology_digest),
          "a handover state authenticated by a non-owner credential was accepted");
  require(!allowed(owner.node_id, "namo-node-a", current.authority_epoch,
                   successor.generation, digest, successor.roster_revision,
                   topology_digest) &&
              !allowed(owner.node_id, "namo-node-a",
                       successor.authority_epoch, current.generation, digest,
                       successor.roster_revision, topology_digest) &&
              !allowed(owner.node_id, "namo-node-a",
                       successor.authority_epoch, successor.generation,
                       digest ^ 1U, successor.roster_revision,
                       topology_digest) &&
              !allowed(owner.node_id, "namo-node-a",
                       successor.authority_epoch, successor.generation,
                       digest, successor.roster_revision + 1U,
                       topology_digest) &&
              !allowed(owner.node_id, "namo-node-a",
                       successor.authority_epoch, successor.generation,
                       digest, successor.roster_revision,
                       topology_digest ^ 1U),
          "a partial or mutated handover state entered bootstrap");
}

void seven_member_handover_persistence_is_atomic_across_reboot() {
  const auto owner = identity("site-home", "namo-node-owner");
  SiteConfigurationSnapshot current{};
  require(initialize_site_configuration(owner, false, &current) ==
              SiteConfigurationResult::OK,
          "seven-member handover bootstrap failed");
  ParticipantRegistry registry{};
  const std::array<const char *, 6> peers{
      "namo-node-a", "namo-node-b", "namo-node-c",
      "namo-node-d", "namo-node-e", "namo-node-f"};
  for (const char *peer : peers)
    add_participant(&registry, peer, ParticipantMembership::ACTIVE);
  require(commit_local_site_roster(owner, registry, current, &current) ==
              SiteConfigurationResult::OK &&
              current.member_count() == 7,
          "seven-member handover roster failed");
  SiteConfigurationSnapshot with_floorplan{};
  require(commit_local_floorplan_manifest(
              owner, current, "floorplan-main", 7, 0x12345678U,
              0x87654321U, &with_floorplan) == SiteConfigurationResult::OK,
          "seven-member handover Floorplan failed");
  current = with_floorplan;
  uint32_t profile_index = 1;
  for (auto &member : current.members) {
    if (!member.occupied)
      continue;
    member.spatial_profile.committed = true;
    std::strcpy(member.spatial_profile.space_id, "room-main");
    std::strcpy(member.spatial_profile.group_id, "group-main");
    member.spatial_profile.pose_version = profile_index;
    member.spatial_profile.calibration_version = profile_index + 10;
    member.spatial_profile.transform =
        transform(static_cast<int32_t>(profile_index * 100),
                  static_cast<int32_t>(profile_index * 200),
                  static_cast<int16_t>(profile_index * 10));
    ++profile_index;
  }

  FakeBackend backend{};
  SiteConfigurationStore store{};
  SiteConfigurationSnapshot persisted{};
  require(store.create(backend, current, &persisted) ==
              SiteConfigurationStoreResult::CREATED &&
              persisted.member_count() == 7,
          "seven-member handover persistence bootstrap failed");
  const auto *successor = automatic_configuration_successor(persisted);
  require(successor != nullptr,
          "seven-member handover did not select a successor");
  const auto successor_identity = identity("site-home", successor->participant_id);
  SiteConfigurationSnapshot handed_over{};
  require(prepare_automatic_configuration_handover(
              successor_identity, persisted, &handed_over) ==
                  SiteConfigurationResult::OK &&
              handed_over.member_count() == 7,
          "seven-member handover snapshot build failed");
  for (const auto &member : persisted.members) {
    if (!member.occupied ||
        std::strcmp(member.participant_id, successor_identity.node_id) == 0)
      continue;
    require(validate_replicated_site_configuration(
                identity("site-home", member.participant_id), persisted,
                handed_over,
                SiteConfigurationReplicationMode::AUTOMATIC_OWNER_HANDOVER,
                successor_identity.node_id) ==
                SiteConfigurationReplicationResult::OK,
            "a seven-member follower rejected the authenticated handover snapshot");
  }

  AutomaticConfigurationHandoverTracker tracker{};
  constexpr uint32_t eligible_at = 1000;
  const uint64_t handover_fingerprint =
      automatic_configuration_handover_fingerprint(
          persisted, successor_identity.node_id);
  require(!tracker.update(AutomaticConfigurationHandoverCondition::ELIGIBLE,
                          handover_fingerprint, eligible_at) &&
              tracker.update(
                  AutomaticConfigurationHandoverCondition::ELIGIBLE,
                  handover_fingerprint,
                  eligible_at +
                      AutomaticConfigurationHandoverTracker::STABILITY_MS),
          "seven-member handover stability guard failed");
  backend.fail_commit_write = true;
  require(store.replace_with_newer(backend, handed_over, &persisted) ==
              SiteConfigurationStoreResult::STORAGE_FAILED,
          "seven-member handover commit failure was not reported");
  tracker.complete(false, eligible_at +
                              AutomaticConfigurationHandoverTracker::STABILITY_MS);
  require(tracker.state() == AutomaticConfigurationHandoverState::FAILED,
          "failed seven-member handover did not enter the failed state");
  backend.fail_commit_write = false;

  SiteConfigurationStore failed_reboot_store{};
  SiteConfigurationSnapshot failed_reboot{};
  require(failed_reboot_store.load(backend, &failed_reboot) ==
              SiteConfigurationStoreResult::LOADED &&
              failed_reboot.member_count() == 7 &&
              std::strcmp(failed_reboot.configuration_owner_node_id,
                          owner.node_id) == 0 &&
              failed_reboot.authority_epoch < handed_over.authority_epoch &&
              failed_reboot.floorplan.config_hash == 0x12345678U &&
              failed_reboot.floorplan.image_hash == 0x87654321U,
          "reboot after handover persistence failure did not restore the old authority");

  AutomaticConfigurationHandoverTracker rebooted_tracker{};
  require(!rebooted_tracker.update(
              AutomaticConfigurationHandoverCondition::ELIGIBLE,
              handover_fingerprint, eligible_at) &&
              rebooted_tracker.update(
                  AutomaticConfigurationHandoverCondition::ELIGIBLE,
                  handover_fingerprint,
                  eligible_at +
                      AutomaticConfigurationHandoverTracker::STABILITY_MS),
          "rebooted handover did not restart its stability guard");
  require(failed_reboot_store.replace_with_newer(
              backend, handed_over, &failed_reboot) ==
                  SiteConfigurationStoreResult::UPDATED,
          "seven-member handover retry did not persist");
  rebooted_tracker.complete(
      true, eligible_at + AutomaticConfigurationHandoverTracker::STABILITY_MS);

  SiteConfigurationStore successful_reboot_store{};
  SiteConfigurationSnapshot successful_reboot{};
  require(successful_reboot_store.load(backend, &successful_reboot) ==
              SiteConfigurationStoreResult::LOADED &&
              successful_reboot.member_count() == 7 &&
              std::strcmp(successful_reboot.configuration_owner_node_id,
                          successor_identity.node_id) == 0 &&
              successful_reboot.authority_epoch ==
                  handed_over.authority_epoch &&
              successful_reboot.roster_revision ==
                  handed_over.roster_revision &&
              successful_reboot.floorplan.geometry_version == 7 &&
              successful_reboot.floorplan.config_hash == 0x12345678U &&
              successful_reboot.floorplan.image_hash == 0x87654321U,
          "successful seven-member handover did not survive reboot");
  for (const auto &member : successful_reboot.members) {
    if (!member.occupied)
      continue;
    const auto *expected = handed_over.find(member.participant_id);
    require(expected != nullptr && expected->spatial_profile.committed &&
                member.spatial_profile.pose_version ==
                    expected->spatial_profile.pose_version &&
                member.spatial_profile.calibration_version ==
                    expected->spatial_profile.calibration_version &&
                member.spatial_profile.transform.origin_x_mm ==
                    expected->spatial_profile.transform.origin_x_mm &&
                member.spatial_profile.transform.origin_y_mm ==
                    expected->spatial_profile.transform.origin_y_mm,
            "seven-member handover changed an approved spatial profile");
  }
}

void successor_roster_refresh_preserves_indirect_canonical_members() {
  const auto old_owner = identity("site-home", "namo-node-owner");
  SiteConfigurationSnapshot current{};
  require(initialize_site_configuration(old_owner, false, &current) ==
              SiteConfigurationResult::OK,
          "successor roster bootstrap failed");
  ParticipantRegistry original_registry{};
  const std::array<const char *, 6> peers{
      "namo-node-a", "namo-node-b", "namo-node-c",
      "namo-node-d", "namo-node-e", "namo-node-f"};
  for (const char *peer : peers)
    add_participant(&original_registry, peer, ParticipantMembership::ACTIVE);
  require(commit_local_site_roster(old_owner, original_registry, current,
                                   &current) == SiteConfigurationResult::OK &&
              current.member_count() == 7,
          "successor roster fixture failed");

  const auto successor = identity("site-home", "namo-node-a");
  SiteConfigurationSnapshot handed_over{};
  require(prepare_automatic_configuration_handover(
              successor, current, &handed_over) ==
              SiteConfigurationResult::OK,
          "successor handover failed");

  ParticipantRegistry successor_direct_pairs{};
  add_participant(&successor_direct_pairs, "namo-node-owner",
                  ParticipantMembership::ACTIVE);
  SiteConfigurationSnapshot refreshed{};
  require(commit_local_site_roster(successor, successor_direct_pairs,
                                   handed_over, &refreshed) ==
              SiteConfigurationResult::OK &&
              refreshed.member_count() == 7 &&
              refreshed.active_member_count() == 7,
          "successor refresh dropped indirect canonical members");
  for (const char *peer : peers)
    require(refreshed.find(peer) != nullptr,
            "successor refresh lost a canonical peer");
}

void successor_can_change_leader_eligibility_for_two_to_seven_members() {
  const std::array<const char *, 6> peers{
      "namo-node-a", "namo-node-b", "namo-node-c",
      "namo-node-d", "namo-node-e", "namo-node-f"};
  for (std::size_t member_count = 2; member_count <= 7; ++member_count) {
    const auto old_owner = identity("site-home", "namo-node-owner");
    SiteConfigurationSnapshot current{};
    require(initialize_site_configuration(old_owner, false, &current) ==
                SiteConfigurationResult::OK,
            "leader eligibility bootstrap failed");
    ParticipantRegistry original_registry{};
    for (std::size_t index = 0; index + 1 < member_count; ++index)
      add_participant(&original_registry, peers[index],
                      ParticipantMembership::ACTIVE);
    require(commit_local_site_roster(old_owner, original_registry, current,
                                     &current) ==
                SiteConfigurationResult::OK,
            "leader eligibility roster commit failed");

    const auto successor = identity("site-home", peers[0]);
    SiteConfigurationSnapshot handed_over{};
    require(prepare_automatic_configuration_handover(
                successor, current, &handed_over) ==
                SiteConfigurationResult::OK,
            "leader eligibility handover failed");
    const char *target = member_count == 2 ? old_owner.node_id
                                           : peers[member_count - 2];
    const uint32_t revision_before = handed_over.roster_revision;
    SiteConfigurationSnapshot disabled{};
    require(set_local_site_member_leader_eligible(
                successor, handed_over, target, false, &disabled) ==
                SiteConfigurationResult::OK &&
                disabled.roster_revision == revision_before + 1 &&
                disabled.find(target) != nullptr &&
                !disabled.find(target)->leader_eligible,
            "successor could not disable a canonical leader candidate");

    SiteConfigurationSnapshot repeated{};
    require(set_local_site_member_leader_eligible(
                successor, disabled, target, false, &repeated) ==
                SiteConfigurationResult::OK &&
                repeated.roster_revision == disabled.roster_revision,
            "leader eligibility mutation was not idempotent");

    ParticipantRegistry successor_direct_pairs{};
    if (std::strcmp(target, old_owner.node_id) == 0) {
      require(successor_direct_pairs.discover(
                  old_owner.node_id, ParticipantType::NAMO_NODE) ==
                      ParticipantRegistryResult::OK &&
                  successor_direct_pairs.classify(
                      old_owner.node_id,
                      ParticipantTrust::COMMUNITY_UNVERIFIED) ==
                      ParticipantRegistryResult::OK &&
                  successor_direct_pairs.activate(old_owner.node_id) ==
                      ParticipantRegistryResult::OK,
              "direct leader policy mirror fixture failed");
    } else {
      add_participant(&successor_direct_pairs, old_owner.node_id,
                      ParticipantMembership::ACTIVE);
    }
    SiteConfigurationSnapshot refreshed{};
    require(commit_local_site_roster(successor, successor_direct_pairs,
                                     repeated, &refreshed) ==
                SiteConfigurationResult::OK &&
                refreshed.member_count() == member_count &&
                refreshed.find(target) != nullptr &&
                !refreshed.find(target)->leader_eligible,
            "roster refresh lost canonical leader eligibility");

    SiteConfigurationSnapshot enabled{};
    require(set_local_site_member_leader_eligible(
                successor, refreshed, target, true, &enabled) ==
                SiteConfigurationResult::OK &&
                enabled.find(target)->leader_eligible,
            "successor could not restore a canonical leader candidate");
    require(set_local_site_member_leader_eligible(
                old_owner, handed_over, target, false, &enabled) ==
                SiteConfigurationResult::NOT_OWNER,
            "former Owner changed leader eligibility after handover");
  }
}

void leader_eligibility_rejects_ineligible_canonical_members() {
  const auto owner = identity("site-home", "namo-node-owner");
  SiteConfigurationSnapshot current{};
  require(initialize_site_configuration(owner, false, &current) ==
              SiteConfigurationResult::OK,
          "ineligible leader bootstrap failed");
  ParticipantRegistry registry{};
  add_participant(&registry, "namo-node-peer", ParticipantMembership::ACTIVE);
  require(commit_local_site_roster(owner, registry, current, &current) ==
              SiteConfigurationResult::OK,
          "ineligible leader roster failed");

  SiteConfigurationSnapshot updated{};
  require(set_local_site_member_leader_eligible(
              owner, current, owner.node_id, false, &updated) ==
              SiteConfigurationResult::INVALID_CONFIGURATION,
          "configuration Owner eligibility was disabled");
  require(set_local_site_member_leader_eligible(
              owner, current, "namo-node-missing", true, &updated) ==
              SiteConfigurationResult::INVALID_ARGUMENT,
          "missing canonical member eligibility was accepted");

  auto *peer = mutable_member(&current, "namo-node-peer");
  require(peer != nullptr, "ineligible leader peer missing");
  peer->membership = ParticipantMembership::QUARANTINED;
  peer->leader_eligible = false;
  require(set_local_site_member_leader_eligible(
              owner, current, peer->participant_id, true, &updated) ==
              SiteConfigurationResult::INVALID_CONFIGURATION,
          "quarantined canonical member became leader eligible");
}

void successor_can_revoke_and_forget_an_indirect_canonical_member() {
  const auto old_owner = identity("site-home", "namo-node-owner");
  SiteConfigurationSnapshot current{};
  require(initialize_site_configuration(old_owner, false, &current) ==
              SiteConfigurationResult::OK,
          "canonical removal bootstrap failed");
  ParticipantRegistry registry{};
  add_participant(&registry, "namo-node-a", ParticipantMembership::ACTIVE);
  add_participant(&registry, "namo-node-b", ParticipantMembership::ACTIVE);
  require(commit_local_site_roster(old_owner, registry, current, &current) ==
              SiteConfigurationResult::OK,
          "canonical removal roster failed");
  const auto successor = identity("site-home", "namo-node-a");
  SiteConfigurationSnapshot handed_over{};
  require(prepare_automatic_configuration_handover(
              successor, current, &handed_over) ==
              SiteConfigurationResult::OK,
          "canonical removal handover failed");

  SiteConfigurationSnapshot revoked{};
  require(revoke_local_site_member(successor, handed_over, "namo-node-b",
                                   &revoked) == SiteConfigurationResult::OK &&
              revoked.find("namo-node-b") != nullptr &&
              revoked.find("namo-node-b")->membership ==
                  ParticipantMembership::REVOKED &&
              !revoked.find("namo-node-b")->leader_eligible &&
              revoked.active_member_count() == 2,
          "successor could not revoke an indirect canonical member");

  SiteConfigurationSnapshot forgotten{};
  require(forget_local_site_member(successor, revoked, "namo-node-b",
                                   &forgotten) == SiteConfigurationResult::OK &&
              forgotten.find("namo-node-b") == nullptr &&
              forgotten.member_count() == 2,
          "successor could not forget a revoked canonical member");
  require(revoke_local_site_member(old_owner, handed_over, "namo-node-b",
                                   &forgotten) ==
              SiteConfigurationResult::NOT_OWNER,
          "former Owner changed the canonical roster after handover");
}

void member_removal_is_canonical_for_two_to_seven_nodes() {
  const auto owner = identity("site-home", "namo-node-zz-owner");
  for (std::size_t member_count = 2; member_count <= 7; ++member_count) {
    ParticipantRegistry registry{};
    char participant_ids[6][24]{};
    for (std::size_t peer = 0; peer + 1 < member_count; ++peer) {
      std::snprintf(participant_ids[peer], sizeof(participant_ids[peer]),
                    "namo-node-%02u", static_cast<unsigned>(peer));
      add_participant(&registry, participant_ids[peer],
                      ParticipantMembership::ACTIVE);
    }

    SiteConfigurationSnapshot configuration{};
    require(initialize_site_configuration(owner, false, &configuration) ==
                SiteConfigurationResult::OK &&
                commit_local_site_roster(owner, registry, configuration,
                                         &configuration) ==
                    SiteConfigurationResult::OK &&
                configuration.member_count() == member_count,
            "canonical removal roster bootstrap failed");

    for (std::size_t removed = 0; removed + 1 < member_count; ++removed) {
      SiteConfigurationSnapshot revoked{};
      SiteConfigurationSnapshot forgotten{};
      require(revoke_local_site_member(owner, configuration,
                                       participant_ids[removed], &revoked) ==
                  SiteConfigurationResult::OK &&
                  forget_local_site_member(owner, revoked,
                                           participant_ids[removed],
                                           &forgotten) ==
                      SiteConfigurationResult::OK,
              "canonical member removal failed");

      bool empty_seen = false;
      const char *previous_id = nullptr;
      for (const auto &member : forgotten.members) {
        if (!member.occupied) {
          empty_seen = true;
          continue;
        }
        require(!empty_seen,
                "canonical member removal left an occupied slot after a gap");
        require(previous_id == nullptr ||
                    std::strcmp(previous_id, member.participant_id) < 0,
                "canonical member removal did not preserve member ordering");
        previous_id = member.participant_id;
      }

      std::array<uint8_t, SiteConfigurationCodec::MAX_ENCODED_SIZE> bytes{};
      std::size_t encoded_size = 0;
      SiteConfigurationSnapshot replicated{};
      require(SiteConfigurationCodec::encode(
                  forgotten, bytes.data(), bytes.size(), &encoded_size) ==
                  SiteConfigurationCodecResult::OK &&
                  SiteConfigurationCodec::decode(bytes.data(), encoded_size,
                                                 &replicated) ==
                      SiteConfigurationCodecResult::OK &&
                  site_configuration_digest(forgotten) ==
                      site_configuration_digest(replicated) &&
                  site_topology_digest(forgotten) ==
                      site_topology_digest(replicated),
              "canonical member removal changed digest after replication");
    }
  }
}

void owner_roster_refresh_readmits_an_explicitly_repaired_member() {
  const auto owner = identity("site-home", "namo-node-owner");
  SiteConfigurationSnapshot current{};
  require(initialize_site_configuration(owner, false, &current) ==
              SiteConfigurationResult::OK,
          "re-admission bootstrap failed");
  ParticipantRegistry initial{};
  add_participant(&initial, "namo-node-peer", ParticipantMembership::ACTIVE);
  require(commit_local_site_roster(owner, initial, current, &current) ==
              SiteConfigurationResult::OK,
          "re-admission initial roster failed");

  SiteConfigurationSnapshot revoked{};
  require(revoke_local_site_member(owner, current, "namo-node-peer",
                                   &revoked) == SiteConfigurationResult::OK,
          "re-admission canonical revoke failed");
  ParticipantRegistry repaired_direct_pair{};
  add_participant(&repaired_direct_pair, "namo-node-peer",
                  ParticipantMembership::ACTIVE);
  SiteConfigurationSnapshot repaired{};
  require(commit_local_site_roster(owner, repaired_direct_pair, revoked,
                                   &repaired) == SiteConfigurationResult::OK &&
              repaired.find("namo-node-peer") != nullptr &&
              repaired.find("namo-node-peer")->membership ==
                  ParticipantMembership::ACTIVE &&
              repaired.active_member_count() == 2,
          "an explicit repaired pair did not replace its revoked canonical record");
}

void replicated_owner_change_requires_an_authenticated_eligible_successor() {
  const auto owner = identity("site-home", "namo-node-owner");
  SiteConfigurationSnapshot current{};
  require(initialize_site_configuration(owner, false, &current) ==
              SiteConfigurationResult::OK,
          "owner change validation bootstrap failed");
  ParticipantRegistry registry{};
  add_participant(&registry, "namo-node-successor",
                  ParticipantMembership::ACTIVE);
  add_participant(&registry, "namo-node-z", ParticipantMembership::ACTIVE);
  require(commit_local_site_roster(owner, registry, current, &current) ==
              SiteConfigurationResult::OK,
          "owner change validation roster failed");

  SiteConfigurationSnapshot remote{};
  const auto follower =
      identity("site-home", "namo-node-owner");
  require(prepare_automatic_configuration_handover(
              identity("site-home", "namo-node-successor"), current,
              &remote) == SiteConfigurationResult::OK,
          "owner change validation handover build failed");
  remote.generation = current.generation + 1;
  require(validate_replicated_site_configuration(
              follower, current, remote) ==
              SiteConfigurationReplicationResult::OWNER_MISMATCH,
          "manual replication must not accept an owner change");
  require(validate_replicated_site_configuration(
              follower, current, remote,
              SiteConfigurationReplicationMode::AUTOMATIC_OWNER_HANDOVER,
              "namo-node-successor") ==
              SiteConfigurationReplicationResult::OK,
          "authenticated automatic replication must accept the elected successor");

  SiteConfigurationSnapshot alternate{};
  require(prepare_automatic_configuration_handover(
              identity("site-home", "namo-node-z"), current, &alternate,
              "namo-node-z") == SiteConfigurationResult::OK,
          "an online eligible non-static successor could not build handover state");
  alternate.generation = current.generation + 1;
  require(validate_replicated_site_configuration(
              follower, current, alternate,
              SiteConfigurationReplicationMode::AUTOMATIC_OWNER_HANDOVER,
              "namo-node-z") == SiteConfigurationReplicationResult::OK,
          "an authenticated eligible alternate successor was rejected");
  require(validate_replicated_site_configuration(
              follower, current, alternate,
              SiteConfigurationReplicationMode::AUTOMATIC_OWNER_HANDOVER,
              "namo-node-successor") ==
              SiteConfigurationReplicationResult::OWNER_MISMATCH,
          "automatic replication must reject a source that is not the new owner");
}

void authority_epoch_fences_stale_owner_writes_and_equal_revisions() {
  const auto owner = identity("site-home", "namo-node-owner");
  const auto successor = identity("site-home", "namo-node-successor");
  SiteConfigurationSnapshot current{};
  require(initialize_site_configuration(owner, false, &current) ==
              SiteConfigurationResult::OK,
          "authority fencing bootstrap failed");
  ParticipantRegistry registry{};
  add_participant(&registry, successor.node_id,
                  ParticipantMembership::ACTIVE);
  require(commit_local_site_roster(owner, registry, current, &current) ==
              SiteConfigurationResult::OK,
          "authority fencing roster failed");

  SiteConfigurationSnapshot handed_over{};
  require(prepare_automatic_configuration_handover(
              successor, current, &handed_over) ==
              SiteConfigurationResult::OK,
          "authority fencing handover failed");
  handed_over.generation = current.generation;
  require(compare_site_configuration_revision(handed_over, current) ==
              SiteConfigurationRevisionOrder::NEWER &&
              validate_replicated_site_configuration(
                  owner, current, handed_over,
                  SiteConfigurationReplicationMode::AUTOMATIC_OWNER_HANDOVER) ==
                  SiteConfigurationReplicationResult::OK,
          "a newer authority epoch must supersede an equal content generation");

  SiteConfigurationSnapshot same_owner_recovery = current;
  ++same_owner_recovery.authority_epoch;
  require(validate_replicated_site_configuration(
              successor, current, same_owner_recovery) ==
              SiteConfigurationReplicationResult::OK,
          "an authenticated current owner may advance exactly one recovery epoch");
  ++same_owner_recovery.authority_epoch;
  require(validate_replicated_site_configuration(
              successor, current, same_owner_recovery) ==
              SiteConfigurationReplicationResult::OWNER_MISMATCH,
          "a current owner may not skip authority epochs");

  SiteConfigurationSnapshot stale_owner_write = current;
  stale_owner_write.generation = current.generation + 100;
  stale_owner_write.floorplan.committed = true;
  std::strcpy(stale_owner_write.floorplan.floorplan_id, "stale-floorplan");
  stale_owner_write.floorplan.geometry_version = 1;
  stale_owner_write.floorplan.config_hash = 11;
  stale_owner_write.floorplan.image_hash = 22;
  require(compare_site_configuration_revision(stale_owner_write,
                                                handed_over) ==
              SiteConfigurationRevisionOrder::OLDER &&
              validate_replicated_site_configuration(
                  successor, handed_over, stale_owner_write,
                  SiteConfigurationReplicationMode::AUTOMATIC_OWNER_HANDOVER) ==
                  SiteConfigurationReplicationResult::OWNER_MISMATCH,
          "a stale owner must not regain authority with a larger generation");

  SiteConfigurationSnapshot divergent = current;
  divergent.floorplan.committed = true;
  std::strcpy(divergent.floorplan.floorplan_id, "divergent-floorplan");
  divergent.floorplan.geometry_version = 1;
  divergent.floorplan.config_hash = 33;
  divergent.floorplan.image_hash = 44;
  require(compare_site_configuration_revision(divergent, current) ==
              SiteConfigurationRevisionOrder::SAME &&
              site_configuration_digest(divergent) !=
                  site_configuration_digest(current) &&
              validate_replicated_site_configuration(
                  owner, current, divergent) ==
                  SiteConfigurationReplicationResult::REVISION_CONFLICT,
          "equal fenced revisions with different payloads must fail closed");

  FakeBackend backend{};
  SiteConfigurationStore store{};
  SiteConfigurationSnapshot persisted{};
  require(store.create(backend, current, &persisted) ==
              SiteConfigurationStoreResult::CREATED,
          "authority fencing persistence bootstrap failed");
  handed_over.generation = persisted.generation;
  require(store.replace_with_newer(backend, handed_over, &persisted) ==
              SiteConfigurationStoreResult::UPDATED &&
              persisted.authority_epoch == handed_over.authority_epoch &&
              std::strcmp(persisted.configuration_owner_node_id,
                          successor.node_id) == 0,
          "persistence must accept a newer epoch without requiring a larger generation");
}

void quorum_authority_rejoin_accepts_replica_and_multiple_epoch_gap() {
  const auto stale_owner = identity("site-home", "namo-node-owner");
  SiteConfigurationSnapshot current{};
  require(initialize_site_configuration(stale_owner, false, &current) ==
              SiteConfigurationResult::OK,
          "quorum rejoin bootstrap failed");
  ParticipantRegistry registry{};
  add_participant(&registry, "namo-node-replica",
                  ParticipantMembership::ACTIVE);
  add_participant(&registry, "namo-node-current-owner",
                  ParticipantMembership::ACTIVE);
  require(commit_local_site_roster(stale_owner, registry, current, &current) ==
              SiteConfigurationResult::OK,
          "quorum rejoin roster failed");
  current.authority_epoch = 3;

  auto remote = current;
  std::strcpy(remote.configuration_owner_node_id,
              "namo-node-current-owner");
  remote.authority_epoch = current.authority_epoch + 4;
  remote.generation = current.generation + 9;
  remote.roster_revision = current.roster_revision + 4;
  require(validate_replicated_site_configuration(
              stale_owner, current, remote,
              SiteConfigurationReplicationMode::QUORUM_AUTHORITY_REJOIN,
              "namo-node-replica") ==
              SiteConfigurationReplicationResult::OK,
          "a quorum-backed replica must recover a multiple-epoch stale owner");

  require(validate_replicated_site_configuration(
              stale_owner, current, remote,
              SiteConfigurationReplicationMode::QUORUM_AUTHORITY_REJOIN,
              "namo-node-missing") ==
              SiteConfigurationReplicationResult::OWNER_MISMATCH,
          "a replica outside the canonical roster must not recover stale authority");

  auto owner_ineligible = remote;
  bool owner_found = false;
  for (auto &member : owner_ineligible.members) {
    if (!member.occupied ||
        std::strcmp(member.participant_id, "namo-node-current-owner") != 0)
      continue;
    member.leader_eligible = false;
    owner_found = true;
    break;
  }
  require(owner_found, "quorum rejoin owner fixture missing");
  require(validate_replicated_site_configuration(
              stale_owner, current, owner_ineligible,
              SiteConfigurationReplicationMode::QUORUM_AUTHORITY_REJOIN,
              "namo-node-replica") !=
              SiteConfigurationReplicationResult::OK,
          "an ineligible remote owner must fail closed");

  auto stale_remote = current;
  stale_remote.authority_epoch = current.authority_epoch - 1;
  require(validate_replicated_site_configuration(
              stale_owner, current, stale_remote,
              SiteConfigurationReplicationMode::QUORUM_AUTHORITY_REJOIN,
              "namo-node-replica") ==
              SiteConfigurationReplicationResult::STALE_REMOTE,
          "quorum mode must not revive an older authority");
}

void multiple_stale_owner_stores_converge_and_survive_reboot() {
  const auto owner_a = identity("site-home", "namo-node-owner-a");
  const auto owner_b = identity("site-home", "namo-node-owner-b");
  SiteConfigurationSnapshot roster{};
  require(initialize_site_configuration(owner_a, false, &roster) ==
              SiteConfigurationResult::OK,
          "multiple stale owner bootstrap failed");
  ParticipantRegistry registry{};
  add_participant(&registry, owner_b.node_id, ParticipantMembership::ACTIVE);
  add_participant(&registry, "namo-node-current-owner",
                  ParticipantMembership::ACTIVE);
  add_participant(&registry, "namo-node-replica",
                  ParticipantMembership::ACTIVE);
  add_participant(&registry, "namo-node-follower",
                  ParticipantMembership::ACTIVE);
  require(commit_local_site_roster(owner_a, registry, roster, &roster) ==
              SiteConfigurationResult::OK &&
              roster.member_count() == 5,
          "multiple stale owner roster failed");

  auto stale_a = roster;
  stale_a.authority_epoch = 2;
  auto stale_b = roster;
  std::strcpy(stale_b.configuration_owner_node_id, owner_b.node_id);
  stale_b.authority_epoch = 4;

  auto canonical = roster;
  std::strcpy(canonical.configuration_owner_node_id,
              "namo-node-current-owner");
  canonical.authority_epoch = 8;
  canonical.generation = 23;
  canonical.roster_revision += 7;
  canonical.floorplan.committed = true;
  std::strcpy(canonical.floorplan.floorplan_id, "floorplan-main");
  canonical.floorplan.geometry_version = 12;
  canonical.floorplan.config_hash = 0x12345678U;
  canonical.floorplan.image_hash = 0x87654321U;

  struct StaleOwnerStore {
    PlatformIdentity identity;
    SiteConfigurationSnapshot snapshot;
    FakeBackend backend;
  } stores[]{{owner_a, stale_a, {}}, {owner_b, stale_b, {}}};

  for (auto &entry : stores) {
    SiteConfigurationStore store{};
    SiteConfigurationSnapshot persisted{};
    require(store.create(entry.backend, entry.snapshot, &persisted) ==
                SiteConfigurationStoreResult::CREATED,
            "stale owner store bootstrap failed");
    require(validate_replicated_site_configuration(
                entry.identity, persisted, canonical,
                SiteConfigurationReplicationMode::QUORUM_AUTHORITY_REJOIN,
                "namo-node-replica") ==
                SiteConfigurationReplicationResult::OK,
            "a stale owner rejected the shared canonical authority");
    require(store.replace_with_newer(entry.backend, canonical, &persisted) ==
                SiteConfigurationStoreResult::UPDATED &&
                persisted.authority_epoch == canonical.authority_epoch &&
                persisted.generation == canonical.generation &&
                site_configuration_digest(persisted) ==
                    site_configuration_digest(canonical),
            "a stale owner did not persist the canonical authority exactly");

    SiteConfigurationStore rebooted_store{};
    SiteConfigurationSnapshot rebooted{};
    require(rebooted_store.load(entry.backend, &rebooted) ==
                SiteConfigurationStoreResult::LOADED &&
                std::strcmp(rebooted.configuration_owner_node_id,
                            "namo-node-current-owner") == 0 &&
                rebooted.authority_epoch == canonical.authority_epoch &&
                rebooted.generation == canonical.generation &&
                site_configuration_digest(rebooted) ==
                    site_configuration_digest(canonical),
            "a converged stale owner lost canonical authority after reboot");
  }
}

void equal_revision_owner_fork_uses_only_quorum_authority_repair() {
  const auto abandoned_owner =
      identity("site-home", "namo-node-abandoned-owner");
  SiteConfigurationSnapshot abandoned{};
  require(initialize_site_configuration(abandoned_owner, false, &abandoned) ==
              SiteConfigurationResult::OK,
          "equal-revision repair bootstrap failed");
  ParticipantRegistry registry{};
  add_participant(&registry, "namo-node-surviving-owner",
                  ParticipantMembership::ACTIVE);
  add_participant(&registry, "namo-node-replica-a",
                  ParticipantMembership::ACTIVE);
  add_participant(&registry, "namo-node-replica-b",
                  ParticipantMembership::ACTIVE);
  add_participant(&registry, "namo-node-replica-c",
                  ParticipantMembership::ACTIVE);
  require(commit_local_site_roster(abandoned_owner, registry, abandoned,
                                   &abandoned) ==
              SiteConfigurationResult::OK &&
              abandoned.active_member_count() == 5,
          "equal-revision repair roster failed");

  FakeBackend backend{};
  SiteConfigurationStore store{};
  SiteConfigurationSnapshot persisted{};
  require(store.create(backend, abandoned, &persisted) ==
              SiteConfigurationStoreResult::CREATED,
          "equal-revision abandoned authority persistence failed");
  auto surviving = persisted;
  std::strcpy(surviving.configuration_owner_node_id,
              "namo-node-surviving-owner");

  require(validate_replicated_site_configuration(
              abandoned_owner, persisted, surviving,
              SiteConfigurationReplicationMode::QUORUM_AUTHORITY_REJOIN,
              "namo-node-replica-a") ==
              SiteConfigurationReplicationResult::OK,
          "quorum authority rejected an owner-only equal-revision repair");
  require(validate_replicated_site_configuration(
              abandoned_owner, persisted, surviving,
              SiteConfigurationReplicationMode::STRICT,
              "namo-node-replica-a") ==
              SiteConfigurationReplicationResult::OWNER_MISMATCH,
          "strict replication accepted an equal-revision owner fork");

  auto changed_payload = surviving;
  changed_payload.floorplan.committed = true;
  std::strcpy(changed_payload.floorplan.floorplan_id, "floorplan-conflict");
  changed_payload.floorplan.geometry_version = 1;
  changed_payload.floorplan.config_hash = 11;
  changed_payload.floorplan.image_hash = 12;
  require(validate_replicated_site_configuration(
              abandoned_owner, persisted, changed_payload,
              SiteConfigurationReplicationMode::QUORUM_AUTHORITY_REJOIN,
              "namo-node-replica-a") ==
              SiteConfigurationReplicationResult::REVISION_CONFLICT,
          "quorum repair accepted non-owner data at the same revision");

  require(store.replace_with_quorum_authority(backend, surviving,
                                               &persisted) ==
                  SiteConfigurationStoreResult::UPDATED &&
              std::strcmp(persisted.configuration_owner_node_id,
                          "namo-node-surviving-owner") == 0 &&
              persisted.authority_epoch == surviving.authority_epoch &&
              persisted.generation == surviving.generation,
          "equal-revision quorum authority was not persisted exactly");
  SiteConfigurationStore rebooted_store{};
  SiteConfigurationSnapshot rebooted{};
  require(rebooted_store.load(backend, &rebooted) ==
                  SiteConfigurationStoreResult::LOADED &&
              rebooted.authority_epoch == surviving.authority_epoch &&
              rebooted.generation == surviving.generation &&
              site_configuration_digest(rebooted) ==
                  site_configuration_digest(surviving),
          "equal-revision quorum repair did not survive reboot");
}

void explicit_conflict_source_advances_the_epoch_without_losing_site_data() {
  const auto node_a = identity("site-home", "namo-node-a");
  const auto node_b = identity("site-home", "namo-node-b");
  SiteConfigurationSnapshot node_a_view{};
  require(initialize_site_configuration(node_a, false, &node_a_view) ==
              SiteConfigurationResult::OK,
          "conflict recovery bootstrap failed");
  ParticipantRegistry registry{};
  add_participant(&registry, node_b.node_id, ParticipantMembership::ACTIVE);
  require(commit_local_site_roster(node_a, registry, node_a_view,
                                   &node_a_view) ==
              SiteConfigurationResult::OK,
          "conflict recovery roster failed");
  node_a_view.floorplan.committed = true;
  std::strcpy(node_a_view.floorplan.floorplan_id, "floorplan-main");
  node_a_view.floorplan.geometry_version = 3;
  node_a_view.floorplan.config_hash = 111;
  node_a_view.floorplan.image_hash = 222;

  SiteConfigurationSnapshot node_b_view = node_a_view;
  std::strcpy(node_b_view.configuration_owner_node_id, node_b.node_id);
  node_b_view.floorplan.config_hash = 333;
  require(compare_site_configuration_revision(node_a_view, node_b_view) ==
                  SiteConfigurationRevisionOrder::SAME &&
              site_configuration_digest(node_a_view) !=
                  site_configuration_digest(node_b_view),
          "the repair fixture must begin as an equal-revision split");

  SiteConfigurationSnapshot selected{};
  require(prepare_local_configuration_conflict_resolution(
              node_b, node_b_view, &selected) ==
                  SiteConfigurationResult::OK &&
              selected.authority_epoch == node_b_view.authority_epoch + 1 &&
              selected.roster_revision == node_b_view.roster_revision &&
              selected.floorplan.config_hash == 333 &&
              selected.member_count() == node_b_view.member_count(),
          "conflict recovery must preserve the selected Site payload");
  selected.generation = node_b_view.generation + 1;
  require(validate_replicated_site_configuration(
              node_a, node_a_view, selected,
              SiteConfigurationReplicationMode::AUTOMATIC_OWNER_HANDOVER) ==
              SiteConfigurationReplicationResult::OK,
          "the deterministic peer must accept the selected higher epoch");
}

void replication_allows_a_quarantined_local_member_to_finish_reconfiguration() {
  const auto owner = identity("site-home", "namo-node-owner");
  const auto follower = identity("site-home", "namo-node-follower");
  SiteConfigurationSnapshot current{};
  require(initialize_site_configuration(owner, false, &current) ==
              SiteConfigurationResult::OK,
          "quarantine replication bootstrap failed");
  ParticipantRegistry registry{};
  add_participant(&registry, follower.node_id, ParticipantMembership::ACTIVE);
  require(commit_local_site_roster(owner, registry, current, &current) ==
              SiteConfigurationResult::OK,
          "quarantine replication roster failed");
  require(commit_local_floorplan_manifest(
              owner, current, "floorplan-main", 3, 0x10203040U, 0x50607080U,
              &current) == SiteConfigurationResult::OK,
          "quarantine replication floorplan failed");

  SiteConfigurationSnapshot quarantined = current;
  auto *quarantined_member =
      mutable_member(&quarantined, follower.node_id);
  require(quarantined_member != nullptr,
          "quarantine replication follower missing");
  quarantined_member->membership = ParticipantMembership::QUARANTINED;
  quarantined_member->leader_eligible = false;
  quarantined_member->spatial_profile.committed = true;
  quarantined_member->spatial_profile.pose_version = 7;
  quarantined_member->spatial_profile.calibration_version = 4;
  quarantined_member->spatial_profile.transform = transform(1200, 850, -300);
  ++quarantined.generation;
  ++quarantined.roster_revision;

  require(validate_replicated_site_configuration(
              follower, current, quarantined) ==
              SiteConfigurationReplicationResult::OK,
          "a quarantined local NAMO Node must receive the canonical "
          "reconfiguration needed for later activation");

  SiteConfigurationSnapshot activated = quarantined;
  auto *activated_member = mutable_member(&activated, follower.node_id);
  activated_member->membership = ParticipantMembership::ACTIVE;
  activated_member->leader_eligible = true;
  ++activated.generation;
  ++activated.roster_revision;
  require(validate_replicated_site_configuration(
              follower, quarantined, activated) ==
              SiteConfigurationReplicationResult::OK,
          "the reconfigured follower must accept the final active roster");

  SiteConfigurationSnapshot revoked = quarantined;
  auto *revoked_member = mutable_member(&revoked, follower.node_id);
  revoked_member->membership = ParticipantMembership::REVOKED;
  ++revoked.generation;
  ++revoked.roster_revision;
  require(validate_replicated_site_configuration(
              follower, current, revoked) ==
              SiteConfigurationReplicationResult::LOCAL_MEMBER_MISSING,
          "a revoked local Node must not receive a canonical replica");

  SiteConfigurationSnapshot missing = quarantined;
  *mutable_member(&missing, follower.node_id) = {};
  ++missing.generation;
  ++missing.roster_revision;
  require(validate_replicated_site_configuration(
              follower, current, missing) ==
              SiteConfigurationReplicationResult::LOCAL_MEMBER_MISSING,
          "a Node removed from the canonical roster must not receive a replica");
}

void authenticated_enrollment_accepts_only_the_verified_mature_site_owner() {
  const auto owner = identity("site-home", "namo-node-owner");
  const auto joiner = identity("site-home", "namo-node-joiner");
  SiteConfigurationSnapshot local{};
  require(initialize_site_configuration(joiner, true, &local) ==
              SiteConfigurationResult::OK,
          "enrollment fixture did not create an unresolved local Site");

  ParticipantRegistry local_participants{};
  require(local_participants.discover(owner.node_id,
                                      ParticipantType::NAMO_NODE) ==
                  ParticipantRegistryResult::OK &&
              local_participants.classify(
                  owner.node_id, ParticipantTrust::COMMUNITY_UNVERIFIED) ==
                  ParticipantRegistryResult::OK,
          "enrollment source participant fixture failed");
  ParticipantCredentialRegistry local_credentials{};
  std::array<uint8_t, 32> key{};
  key[0] = 0x5A;
  require(local_credentials.set(owner.node_id, 0x123456789ULL, 1,
                                key.data(), true) ==
              ParticipantCredentialResult::OK,
          "enrollment source credential fixture failed");
  require(authenticated_enrollment_source_allowed(
              joiner, local, local_participants, local_credentials,
              owner.node_id),
          "a single verified quarantined source should resume after reboot");

  SiteConfigurationSnapshot remote{};
  require(initialize_site_configuration(owner, false, &remote) ==
              SiteConfigurationResult::OK,
          "mature Site owner fixture failed");
  ParticipantRegistry owner_participants{};
  require(owner_participants.discover(joiner.node_id,
                                      ParticipantType::NAMO_NODE) ==
                  ParticipantRegistryResult::OK &&
              owner_participants.classify(
                  joiner.node_id, ParticipantTrust::COMMUNITY_UNVERIFIED) ==
                  ParticipantRegistryResult::OK &&
              commit_local_site_roster(owner, owner_participants, remote,
                                       &remote) ==
                  SiteConfigurationResult::OK,
          "mature Site did not retain the quarantined joiner");
  remote.authority_epoch = 5;
  remote.generation = 74;
  remote.roster_revision = 58;

  require(validate_replicated_site_configuration(
              joiner, local, remote,
              SiteConfigurationReplicationMode::STRICT, owner.node_id) ==
              SiteConfigurationReplicationResult::OWNER_MISMATCH,
          "strict replication must not accept a mature Site epoch jump");
  require(validate_replicated_site_configuration(
              joiner, local, remote,
              SiteConfigurationReplicationMode::AUTHENTICATED_ENROLLMENT,
              owner.node_id) == SiteConfigurationReplicationResult::OK,
          "verified enrollment did not accept the canonical mature Site");
  require(validate_replicated_site_configuration(
              joiner, local, remote,
              SiteConfigurationReplicationMode::AUTHENTICATED_ENROLLMENT,
              "namo-node-attacker") ==
              SiteConfigurationReplicationResult::OWNER_MISMATCH,
          "enrollment accepted a source other than the canonical owner");

  auto missing_joiner = remote;
  *mutable_member(&missing_joiner, joiner.node_id) = {};
  require(validate_replicated_site_configuration(
              joiner, local, missing_joiner,
              SiteConfigurationReplicationMode::AUTHENTICATED_ENROLLMENT,
              owner.node_id) ==
              SiteConfigurationReplicationResult::OWNER_MISMATCH,
          "enrollment accepted a roster that omitted the joining Node");

  auto revoked_joiner = remote;
  mutable_member(&revoked_joiner, joiner.node_id)->membership =
      ParticipantMembership::REVOKED;
  require(validate_replicated_site_configuration(
              joiner, local, revoked_joiner,
              SiteConfigurationReplicationMode::AUTHENTICATED_ENROLLMENT,
              owner.node_id) ==
              SiteConfigurationReplicationResult::OWNER_MISMATCH,
          "enrollment accepted a roster that revoked the joining Node");

  auto ineligible_owner = remote;
  mutable_member(&ineligible_owner, owner.node_id)->leader_eligible = false;
  require(validate_replicated_site_configuration(
              joiner, local, ineligible_owner,
              SiteConfigurationReplicationMode::AUTHENTICATED_ENROLLMENT,
              owner.node_id) != SiteConfigurationReplicationResult::OK,
          "enrollment accepted an ineligible canonical owner");

  auto canonical_local = remote;
  require(validate_replicated_site_configuration(
              joiner, canonical_local, remote,
              SiteConfigurationReplicationMode::AUTHENTICATED_ENROLLMENT,
              owner.node_id) ==
              SiteConfigurationReplicationResult::OWNER_MISMATCH,
          "enrollment mode remained usable after canonical convergence");

  auto disabled_credentials = local_credentials;
  require(disabled_credentials.set_enabled(owner.node_id, false) ==
                  ParticipantCredentialResult::OK &&
              !authenticated_enrollment_source_allowed(
                  joiner, local, local_participants, disabled_credentials,
                  owner.node_id),
          "a disabled pairing credential reopened enrollment bootstrap");

  auto ambiguous_participants = local_participants;
  require(ambiguous_participants.discover(
              "namo-node-other", ParticipantType::NAMO_NODE) ==
                  ParticipantRegistryResult::OK &&
              ambiguous_participants.classify(
                  "namo-node-other",
                  ParticipantTrust::COMMUNITY_UNVERIFIED) ==
                  ParticipantRegistryResult::OK &&
              !authenticated_enrollment_source_allowed(
                  joiner, local, ambiguous_participants, local_credentials,
                  owner.node_id),
          "ambiguous live pairing records must fail enrollment closed");

  auto revoked_participants = local_participants;
  auto revoked_credentials = local_credentials;
  std::array<uint8_t, 32> revoked_key{};
  revoked_key[0] = 0x33;
  require(revoked_participants.discover(
              "namo-node-revoked", ParticipantType::NAMO_NODE) ==
                  ParticipantRegistryResult::OK &&
              revoked_participants.classify(
                  "namo-node-revoked",
                  ParticipantTrust::COMMUNITY_UNVERIFIED) ==
                  ParticipantRegistryResult::OK &&
              revoked_participants.revoke("namo-node-revoked") ==
                  ParticipantRegistryResult::OK &&
              revoked_credentials.set("namo-node-revoked", 777, 1,
                                      revoked_key.data(), false) ==
                  ParticipantCredentialResult::OK &&
              authenticated_enrollment_source_allowed(
                  joiner, local, revoked_participants, revoked_credentials,
                  owner.node_id),
          "a disabled credential tied to a revoked record should not block "
          "the exact enrollment source");
  require(revoked_credentials.set_enabled("namo-node-revoked", true) ==
                  ParticipantCredentialResult::OK &&
              !authenticated_enrollment_source_allowed(
                  joiner, local, revoked_participants, revoked_credentials,
                  owner.node_id),
          "an enabled stale credential must keep enrollment fail-closed");
  auto orphan_credentials = local_credentials;
  require(orphan_credentials.set("namo-node-orphan", 778, 1,
                                 revoked_key.data(), false) ==
                  ParticipantCredentialResult::OK &&
              !authenticated_enrollment_source_allowed(
                  joiner, local, local_participants, orphan_credentials,
                  owner.node_id),
          "an orphan credential must keep enrollment fail-closed");
}

void automatic_handover_waits_for_a_stable_safe_candidate() {
  const auto owner = identity("site-home", "namo-node-owner");
  const auto successor = identity("site-home", "namo-node-successor");
  SiteConfigurationSnapshot configuration{};
  require(initialize_site_configuration(owner, false, &configuration) ==
              SiteConfigurationResult::OK,
          "handover tracker bootstrap failed");
  ParticipantRegistry registry{};
  add_participant(&registry, successor.node_id,
                  ParticipantMembership::ACTIVE);
  require(commit_local_site_roster(owner, registry, configuration,
                                   &configuration) ==
              SiteConfigurationResult::OK,
          "handover tracker roster failed");
  configuration.floorplan.committed = true;
  std::strcpy(configuration.floorplan.floorplan_id, "floorplan-main");
  configuration.floorplan.geometry_version = 1;
  configuration.floorplan.config_hash = 1;
  configuration.floorplan.image_hash = 2;

  require(evaluate_automatic_configuration_handover(
              successor, configuration, true, true, true, true) ==
              AutomaticConfigurationHandoverCondition::OWNER_ONLINE,
          "an online owner must suppress handover");
  require(evaluate_automatic_configuration_handover(
              successor, configuration, false, true, false, true) ==
              AutomaticConfigurationHandoverCondition::REPLICA_NOT_READY,
          "a successor without the shared floorplan must be blocked");
  require(evaluate_automatic_configuration_handover(
              successor, configuration, false, true, true, false) ==
              AutomaticConfigurationHandoverCondition::CREDENTIALS_INCOMPLETE,
          "a successor without all pairwise credentials must be blocked");

  AutomaticConfigurationHandoverTracker tracker{};
  const uint64_t fingerprint =
      automatic_configuration_handover_fingerprint(
          configuration, successor.node_id);
  require(!tracker.update(
              AutomaticConfigurationHandoverCondition::ELIGIBLE,
              fingerprint, 100) &&
              tracker.state() ==
                  AutomaticConfigurationHandoverState::WAITING &&
              tracker.wait_remaining_ms(100) ==
                  AutomaticConfigurationHandoverTracker::STABILITY_MS,
          "an eligible successor must first enter the stability window");
  require(!tracker.update(
              AutomaticConfigurationHandoverCondition::ELIGIBLE,
              fingerprint,
              100 + AutomaticConfigurationHandoverTracker::STABILITY_MS - 1) &&
              tracker.update(
                  AutomaticConfigurationHandoverCondition::ELIGIBLE,
                  fingerprint,
                  100 + AutomaticConfigurationHandoverTracker::STABILITY_MS) &&
              tracker.state() == AutomaticConfigurationHandoverState::READY,
          "handover must wait for the complete stability guard");
  const uint32_t failed_at =
      100 + AutomaticConfigurationHandoverTracker::STABILITY_MS;
  tracker.complete(false, failed_at);
  const uint32_t retry_wait = tracker.wait_remaining_ms(failed_at);
  require(retry_wait >= AutomaticConfigurationHandoverTracker::RETRY_BASE_MS &&
              retry_wait <=
                  AutomaticConfigurationHandoverTracker::RETRY_MAX_MS,
          "a persistence failure did not schedule a bounded retry");
  require(!tracker.update(
              AutomaticConfigurationHandoverCondition::ELIGIBLE,
              fingerprint, failed_at + retry_wait - 1) &&
              tracker.state() == AutomaticConfigurationHandoverState::FAILED,
          "a persistence failure must not hot-loop flash writes");
  require(tracker.update(
              AutomaticConfigurationHandoverCondition::ELIGIBLE,
              fingerprint, failed_at + retry_wait) &&
              tracker.state() == AutomaticConfigurationHandoverState::READY,
          "a stable failed handover did not retry after its bounded delay");
  tracker.complete(false, failed_at + retry_wait);
  const uint64_t changed_fingerprint = fingerprint ^ 1U;
  require(!tracker.update(
              AutomaticConfigurationHandoverCondition::ELIGIBLE,
              changed_fingerprint, failed_at + retry_wait + 1) &&
              tracker.state() == AutomaticConfigurationHandoverState::WAITING,
          "a changed authority candidate did not restart the stability fence");
  require(!tracker.update(
              AutomaticConfigurationHandoverCondition::OWNER_ONLINE,
              0, failed_at + retry_wait + 2) &&
              tracker.state() == AutomaticConfigurationHandoverState::IDLE,
          "owner recovery must reset a failed handover attempt");
}

void site_topology_digest_ignores_floorplan_authority_only() {
  const auto owner = identity("site-home", "namo-node-owner");
  SiteConfigurationSnapshot first{};
  require(initialize_site_configuration(owner, false, &first) ==
              SiteConfigurationResult::OK,
          "topology digest bootstrap failed");
  first.floorplan.committed = true;
  std::strcpy(first.floorplan.floorplan_id, "floorplan-a");
  first.floorplan.geometry_version = 1;
  first.floorplan.config_hash = 11;
  first.floorplan.image_hash = 22;

  auto second = first;
  std::strcpy(second.configuration_owner_node_id, "namo-node-other");
  std::strcpy(second.floorplan.floorplan_id, "floorplan-b");
  second.floorplan.geometry_version = 7;
  second.floorplan.config_hash = 33;
  second.floorplan.image_hash = 44;
  second.generation += 10;
  require(site_configuration_digest(first) !=
              site_configuration_digest(second) &&
              same_site_topology(first, second),
          "Floorplan authority must not split the shared Site topology");

  second.members[0].leader_eligible = false;
  require(!same_site_topology(first, second),
          "a roster role change must change the Site topology fence");
}

void automatic_spatial_groups_preserve_legacy_ids_within_one_space() {
  const auto owner = identity("site-home", "namo-node-00");
  SiteConfigurationSnapshot configuration{};
  require(initialize_site_configuration(owner, false, &configuration) ==
              SiteConfigurationResult::OK,
          "automatic Group bootstrap failed");
  configuration.floorplan.committed = true;
  std::strcpy(configuration.floorplan.floorplan_id, "floorplan-main");
  configuration.floorplan.geometry_version = 1;
  configuration.floorplan.config_hash = 101;
  configuration.floorplan.image_hash = 202;
  auto add_member = [&](std::size_t slot, const char *participant_id,
                        const char *space_id, const char *group_id) {
    auto &member = configuration.members[slot];
    member.occupied = true;
    std::strcpy(member.participant_id, participant_id);
    member.type = ParticipantType::NAMO_NODE;
    member.trust = ParticipantTrust::OFFICIAL_VERIFIED;
    member.membership = ParticipantMembership::ACTIVE;
    member.leader_eligible = true;
    if (group_id == nullptr)
      return;
    member.spatial_profile.committed = true;
    std::strcpy(member.spatial_profile.space_id, space_id);
    std::strcpy(member.spatial_profile.group_id, group_id);
    member.spatial_profile.pose_version = 1;
    member.spatial_profile.calibration_version = 1;
    member.spatial_profile.transform = transform(
        static_cast<int32_t>(slot * 500), 500, 0);
  };
  auto *local = mutable_member(&configuration, owner.node_id);
  require(local != nullptr, "automatic Group local member missing");
  local->spatial_profile.committed = true;
  std::strcpy(local->spatial_profile.space_id, "room-bedroom");
  std::strcpy(local->spatial_profile.group_id, "room-bedroom");
  local->spatial_profile.pose_version = 1;
  local->spatial_profile.calibration_version = 1;
  local->spatial_profile.transform = transform(0, 500, 0);
  add_member(1, "namo-node-01", nullptr, nullptr);

  AutomaticSpatialGroupDecision decision{};
  require(resolve_automatic_spatial_group(
              owner, configuration, "namo-node-01", "room-corridor",
              "room-corridor",
              &decision) == AutomaticSpatialGroupResult::OK &&
              std::strcmp(decision.space_id, "room-corridor") == 0 &&
              std::strcmp(decision.group_id, "room-corridor") == 0 &&
              !decision.normalization_required &&
              decision.participant_count == 1,
          "a new peer inherited a Group from another space");

  std::strcpy(local->spatial_profile.group_id, "shared-legacy-group");
  add_member(1, "namo-node-01", "room-corridor", "shared-legacy-group");
  require(resolve_automatic_spatial_group(
              owner, configuration, "namo-node-01", "room-corridor",
              "shared-legacy-group", &decision) ==
              AutomaticSpatialGroupResult::OK &&
              std::strcmp(decision.group_id, "room-corridor") == 0 &&
              decision.normalization_required,
          "a legacy Group shared by different spaces was not split");
  SiteConfigurationSnapshot split{};
  bool changed = false;
  require(apply_automatic_spatial_group_assignment(
              owner, decision, configuration, &split, &changed) ==
              SiteConfigurationResult::OK &&
              changed &&
              std::strcmp(split.find(owner.node_id)->spatial_profile.group_id,
                          "shared-legacy-group") == 0 &&
              std::strcmp(split.find("namo-node-01")
                              ->spatial_profile.group_id,
                          "room-corridor") == 0,
          "cross-space legacy Group repair changed the wrong Space");

  std::strcpy(local->spatial_profile.group_id, "room-bedroom");
  add_member(1, "namo-node-01", "room-corridor", "room-corridor");
  add_member(2, "namo-node-02", "room-corridor", "legacy-corridor");
  add_member(3, "namo-node-03", "room-bedroom", "room-bedroom");
  require(resolve_automatic_spatial_group(
              owner, configuration, "namo-node-01", "room-corridor",
              "room-corridor",
              &decision) == AutomaticSpatialGroupResult::OK &&
              decision.normalization_required &&
              std::strcmp(decision.group_id, "room-corridor") == 0 &&
              decision.participant_count == 2,
          "same-space legacy Groups were not selected for normalization");
  SiteConfigurationSnapshot normalized{};
  changed = false;
  require(apply_automatic_spatial_group_assignment(
              owner, decision, configuration, &normalized, &changed) ==
              SiteConfigurationResult::OK &&
              changed &&
              std::strcmp(normalized.find("namo-node-01")
                              ->spatial_profile.group_id,
                          "room-corridor") == 0 &&
              std::strcmp(normalized.find("namo-node-02")
                              ->spatial_profile.group_id,
                          "room-corridor") == 0 &&
              std::strcmp(normalized.find("namo-node-02")
                              ->spatial_profile.space_id,
                          "room-corridor") == 0 &&
              std::strcmp(normalized.find("namo-node-03")
                              ->spatial_profile.group_id,
                          "room-bedroom") == 0,
          "same-space Group normalization changed another space");

  for (std::size_t slot = 4; slot < 10; ++slot) {
    char participant_id[24]{};
    std::snprintf(participant_id, sizeof(participant_id), "namo-node-%02u",
                  static_cast<unsigned>(slot));
    add_member(slot, participant_id, "room-corridor",
               slot < 7 ? "room-corridor" : "legacy-corridor");
  }
  require(resolve_automatic_spatial_group(
              owner, configuration, "namo-node-01", "room-corridor",
              "room-corridor",
              &decision) == AutomaticSpatialGroupResult::GROUP_FULL &&
              decision.participant_count == 8,
          "an eight-participant same-space Group must fail closed");
}

}  // namespace

void run_site_configuration_tests() {
  standalone_bootstrap_is_owned_and_committed();
  unattested_namo_trust_is_community_and_migrates_once();
  enrolled_migration_fails_closed_without_guessing_owner();
  committed_remote_owner_identifies_an_enrolled_local_member();
  unresolved_owner_can_be_claimed_once_by_explicit_local_action();
  only_owner_can_commit_a_canonical_bounded_roster();
  unchanged_roster_commit_is_idempotent();
  approved_spatial_profiles_are_canonical_and_survive_roster_refresh();
  legacy_assignment_reconciliation_preserves_approved_transforms();
  local_assignment_is_never_guessed_across_multiple_groups();
  role_changes_cannot_reanchor_committed_spatial_profiles();
  canonical_profiles_repair_divergent_direct_readiness();
  only_owner_can_clear_the_shared_floorplan();
  exact_floorplan_manifest_match_is_idempotent();
  config_only_floorplan_updates_preserve_spatial_calibration();
  connected_site_floorplan_delete_policy_tracks_non_revoked_peers();
  site_move_allows_only_a_device_without_live_dependents();
  pairing_reclaims_authority_only_from_the_same_single_peer();
  pairing_retry_preserves_only_an_exact_converged_joiner_site();
  empty_registry_reconciles_to_local_owner_without_changing_site_data();
  standalone_reconciliation_restores_a_missing_local_member();
  standalone_reconciliation_ignores_revoked_records_but_refuses_live_peers();
  standalone_reconciliation_tracker_runs_once_and_handles_wraparound();
  codec_round_trip_and_corruption_are_bounded();
  version_two_spatial_profiles_migrate_without_guessing_assignments();
  transaction_keeps_previous_commit_after_interruption();
  transaction_keeps_previous_commit_when_slot_write_fails();
  enrollment_restarts_generation_without_erasing_the_previous_commit();
  local_connection_reset_replaces_an_older_authority_domain_atomically();
  standalone_reconciliation_finishes_an_interrupted_site_reset();
  twenty_members_fit_the_contract();
  automatic_handover_uses_one_deterministic_successor();
  automatic_handover_bootstrap_accepts_only_an_exact_eligible_state();
  seven_member_handover_persistence_is_atomic_across_reboot();
  successor_roster_refresh_preserves_indirect_canonical_members();
  successor_can_change_leader_eligibility_for_two_to_seven_members();
  leader_eligibility_rejects_ineligible_canonical_members();
  successor_can_revoke_and_forget_an_indirect_canonical_member();
  member_removal_is_canonical_for_two_to_seven_nodes();
  owner_roster_refresh_readmits_an_explicitly_repaired_member();
  replicated_owner_change_requires_an_authenticated_eligible_successor();
  authority_epoch_fences_stale_owner_writes_and_equal_revisions();
  quorum_authority_rejoin_accepts_replica_and_multiple_epoch_gap();
  multiple_stale_owner_stores_converge_and_survive_reboot();
  equal_revision_owner_fork_uses_only_quorum_authority_repair();
  explicit_conflict_source_advances_the_epoch_without_losing_site_data();
  replication_allows_a_quarantined_local_member_to_finish_reconfiguration();
  authenticated_enrollment_accepts_only_the_verified_mature_site_owner();
  automatic_handover_waits_for_a_stable_safe_candidate();
  site_topology_digest_ignores_floorplan_authority_only();
  automatic_spatial_groups_preserve_legacy_ids_within_one_space();
}
