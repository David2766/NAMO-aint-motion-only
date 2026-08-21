#include "role_quorum.h"
#include "site_replication.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace esphome::radar_api_server;

namespace {

void require(bool condition, const char *message) {
  if (condition)
    return;
  std::fprintf(stderr, "pairing_bootstrap_activation_tests: %s\n", message);
  std::exit(1);
}

class FlowConfigurationBackend : public SiteConfigurationStorageBackend {
 public:
  bool load_site_configuration_slot(
      uint8_t slot, SiteConfigurationStorageBlob *blob) override {
    if (slot >= 2 || blob == nullptr || !this->slot_present[slot])
      return false;
    *blob = this->slots[slot];
    return true;
  }

  bool save_site_configuration_slot(
      uint8_t slot, const SiteConfigurationStorageBlob &blob) override {
    if (slot >= 2)
      return false;
    this->slots[slot] = blob;
    this->slot_present[slot] = true;
    return true;
  }

  bool load_site_configuration_commit(
      SiteConfigurationCommitBlob *blob) override {
    if (blob == nullptr || !this->commit_present)
      return false;
    *blob = this->commit;
    return true;
  }

  bool save_site_configuration_commit(
      const SiteConfigurationCommitBlob &blob) override {
    this->commit = blob;
    this->commit_present = true;
    return true;
  }

  bool sync() override { return true; }

 private:
  SiteConfigurationStorageBlob slots[2]{};
  bool slot_present[2]{false, false};
  SiteConfigurationCommitBlob commit{};
  bool commit_present{false};
};

class FlowDirectoryBackend : public SiteDirectoryStorageBackend {
 public:
  FlowDirectoryBackend() {
    for (auto &slot : this->slots)
      slot.bytes.fill(0xFF);
    this->commit.bytes.fill(0xFF);
  }

  bool load_site_directory_slot(
      uint8_t slot, SiteDirectoryStorageBlob *blob) override {
    if (slot >= 2 || blob == nullptr)
      return false;
    *blob = this->slots[slot];
    return true;
  }

  bool save_site_directory_slot(
      uint8_t slot, const SiteDirectoryStorageBlob &blob) override {
    if (slot >= 2)
      return false;
    this->slots[slot] = blob;
    return true;
  }

  bool load_site_directory_commit(
      SiteDirectoryCommitBlob *blob) override {
    if (blob == nullptr)
      return false;
    *blob = this->commit;
    return true;
  }

  bool save_site_directory_commit(
      const SiteDirectoryCommitBlob &blob) override {
    this->commit = blob;
    return true;
  }

  bool sync() override { return true; }

 private:
  SiteDirectoryStorageBlob slots[2]{};
  SiteDirectoryCommitBlob commit{};
};

PlatformIdentity flow_identity(const char *node_id) {
  PlatformIdentity value{};
  std::strcpy(value.site_id, "site-flow");
  std::strcpy(value.node_id, node_id);
  return value;
}

void add_flow_participant(ParticipantRegistry *registry, const char *node_id,
                          ParticipantMembership membership) {
  require(registry != nullptr &&
              registry->discover(node_id, ParticipantType::NAMO_NODE) ==
                  ParticipantRegistryResult::OK &&
              registry->classify(
                  node_id, ParticipantTrust::COMMUNITY_UNVERIFIED) ==
                  ParticipantRegistryResult::OK,
          "participant enrollment fixture failed");
  if (membership == ParticipantMembership::ACTIVE) {
    require(registry->activate(node_id) == ParticipantRegistryResult::OK,
            "participant activation fixture failed");
  }
}

void provision_flow_credential(ParticipantCredentialRegistry *credentials,
                               const char *node_id, uint8_t fill) {
  std::array<uint8_t, 32> key{};
  key.fill(fill);
  uint64_t token = 1469598103934665603ULL;
  for (const char *cursor = node_id; *cursor != '\0'; ++cursor) {
    token ^= static_cast<uint8_t>(*cursor);
    token *= 1099511628211ULL;
  }
  require(credentials != nullptr &&
              credentials->set(node_id, token, 1, key.data(), true) ==
                  ParticipantCredentialResult::OK,
          "credential fixture failed");
}

void observe_flow_peer(ParticipantEndpointCache *endpoints,
                       const SiteConfigurationSnapshot &configuration,
                       const SiteDirectorySnapshot &directory,
                       const char *node_id, uint32_t ipv4) {
  require(endpoints != nullptr &&
              endpoints->observe_authenticated(node_id, ipv4, 49191, 1000) &&
              endpoints->observe_floorplan_replica(
                  node_id, configuration.authority_epoch,
                  configuration.generation,
                  site_configuration_digest(configuration),
                  configuration.floorplan.geometry_version,
                  configuration.floorplan.config_hash,
                  configuration.floorplan.image_hash, 1, true) &&
              endpoints->observe_site_directory(
                  node_id, directory.topology_revision,
                  site_directory_payload_hash(directory)),
          "endpoint fixture failed");
}

ParticipantReadinessConfig flow_readiness() {
  ParticipantReadinessConfig readiness{};
  readiness.capability_flags =
      PARTICIPANT_CAP_POINT_TRACKS | PARTICIPANT_CAP_PRESENCE;
  readiness.max_targets = 3;
  std::strcpy(readiness.floorplan_id, "floorplan-main");
  std::strcpy(readiness.space_id, "space-main");
  std::strcpy(readiness.group_id, "group-main");
  readiness.geometry_version = 7;
  readiness.local_pose_version = 1;
  readiness.pose_version = 1;
  readiness.calibration_version = 1;
  readiness.transform.residual_mm = 0;
  readiness.transform.sample_count = 1;
  readiness.transform.valid = true;
  return readiness;
}

void pairing_bootstrap_activation_flow_survives_restart_to_seven_nodes() {
  for (uint8_t total_nodes = 2; total_nodes <= 7; ++total_nodes) {
    const auto owner = flow_identity("namo-node-owner");
    const auto joiner = flow_identity("namo-node-joiner");

    SiteConfigurationSnapshot owner_configuration{};
    require(initialize_site_configuration(owner, false,
                                          &owner_configuration) ==
                SiteConfigurationResult::OK,
            "owner configuration bootstrap failed");
    FlowConfigurationBackend owner_configuration_backend{};
    SiteConfigurationStore owner_configuration_store{};
    require(owner_configuration_store.create(
                owner_configuration_backend, owner_configuration,
                &owner_configuration) ==
                SiteConfigurationStoreResult::CREATED,
            "owner configuration persistence bootstrap failed");
    ParticipantRegistry owner_participants{};
    for (uint8_t index = 0; index + 2 < total_nodes; ++index) {
      char node_id[ParticipantRecord::MAX_ID_LENGTH + 1]{};
      std::snprintf(node_id, sizeof(node_id), "namo-node-existing-%u",
                    static_cast<unsigned>(index));
      add_flow_participant(&owner_participants, node_id,
                           ParticipantMembership::ACTIVE);
    }
    add_flow_participant(&owner_participants, joiner.node_id,
                         ParticipantMembership::QUARANTINED);
    SiteConfigurationSnapshot owner_update{};
    require(commit_local_site_roster(owner, owner_participants,
                                     owner_configuration, &owner_update) ==
                SiteConfigurationResult::OK &&
                owner_configuration_store.update(
                    owner_configuration_backend, owner_update,
                    &owner_configuration) ==
                    SiteConfigurationStoreResult::UPDATED,
            "owner canonical roster commit failed");
    require(commit_local_floorplan_manifest(
                owner, owner_configuration, "floorplan-main", 7, 101, 202,
                &owner_update) == SiteConfigurationResult::OK &&
                owner_configuration_store.update(
                    owner_configuration_backend, owner_update,
                    &owner_configuration) ==
                    SiteConfigurationStoreResult::UPDATED,
            "owner canonical floorplan commit failed");

    SiteDirectorySnapshot owner_directory{};
    require(migrate_site_configuration_to_directory(
                owner_configuration, &owner_directory) ==
                SiteDirectoryResult::OK,
            "owner directory migration failed");
    owner_directory.compatibility_mirror = false;
    require(site_directory_is_canonical_for_configuration(
                owner_configuration, owner_directory),
            "owner directory was not canonical");

    SiteConfigurationSnapshot joiner_configuration{};
    require(initialize_site_configuration(joiner, true,
                                          &joiner_configuration) ==
                SiteConfigurationResult::OK,
            "joiner enrollment configuration bootstrap failed");
    FlowConfigurationBackend configuration_backend{};
    SiteConfigurationStore configuration_store{};
    require(configuration_store.create(configuration_backend,
                                       joiner_configuration,
                                       &joiner_configuration) ==
                SiteConfigurationStoreResult::CREATED,
            "joiner enrollment configuration persistence failed");

    SiteDirectorySnapshot joiner_mirror{};
    bool mirror_update_required = false;
    require(prepare_site_directory_compatibility_mirror(
                joiner_configuration, nullptr, &joiner_mirror,
                &mirror_update_required) == SiteDirectoryResult::OK &&
                mirror_update_required && joiner_mirror.compatibility_mirror,
            "joiner mirror bootstrap failed");
    FlowDirectoryBackend directory_backend{};
    SiteDirectoryStore directory_store{};
    require(directory_store.create(directory_backend, joiner_mirror,
                                   &joiner_mirror) ==
                SiteDirectoryStoreResult::CREATED,
            "joiner mirror persistence failed");

    const auto enrollment_validation =
        validate_replicated_site_configuration(
            joiner, joiner_configuration, owner_configuration,
            SiteConfigurationReplicationMode::AUTHENTICATED_ENROLLMENT,
            owner.node_id);
    if (enrollment_validation != SiteConfigurationReplicationResult::OK) {
      std::fprintf(
          stderr,
          "pairing_bootstrap_activation_tests: total_nodes=%u "
          "enrollment_validation=%s\n",
          static_cast<unsigned>(total_nodes),
          site_configuration_replication_result_to_string(
              enrollment_validation));
    }
    require(enrollment_validation == SiteConfigurationReplicationResult::OK,
            "authenticated enrollment Configuration was rejected");
    require(configuration_store.replace_with_newer(
                configuration_backend, owner_configuration,
                &joiner_configuration) ==
                SiteConfigurationStoreResult::UPDATED,
            "authenticated enrollment Configuration was not persisted");
    require(pairing_joiner_site_already_converged(
                joiner, joiner_configuration, owner.site_id, owner.node_id),
            "persisted joiner configuration did not become retry-safe");

    auto stale_same_site_directory = owner_directory;
    stale_same_site_directory.topology_revision =
        owner_directory.topology_revision == 1
            ? 2
            : owner_directory.topology_revision - 1;
    SiteDirectorySnapshot preserved_stale_directory{};
    bool stale_update_required = true;
    require(prepare_site_directory_compatibility_mirror(
                joiner_configuration, &stale_same_site_directory,
                &preserved_stale_directory, &stale_update_required) ==
                SiteDirectoryResult::OK &&
                !stale_update_required &&
                !preserved_stale_directory.compatibility_mirror &&
                !same_site_directory_payload(preserved_stale_directory,
                                             owner_directory),
            "same-Site canonical rejoin fixture did not preserve its stale copy");

    SiteDirectorySnapshot refreshed_mirror{};
    require(prepare_site_directory_compatibility_mirror(
                joiner_configuration, &joiner_mirror, &refreshed_mirror,
                &mirror_update_required) == SiteDirectoryResult::OK &&
                mirror_update_required &&
                refreshed_mirror.compatibility_mirror &&
                directory_store.update(directory_backend, refreshed_mirror,
                                       &joiner_mirror) ==
                    SiteDirectoryStoreResult::UPDATED,
            "joiner mirror did not follow the replicated Configuration");

    ParticipantEndpointCache joiner_endpoints{};
    ParticipantCredentialRegistry joiner_credentials{};
    observe_flow_peer(&joiner_endpoints, joiner_configuration,
                      owner_directory, owner.node_id, 0xC0A80101U);
    provision_flow_credential(&joiner_credentials, owner.node_id, 0x41);
    SiteCoordinatorState joiner_site{};
    joiner_site.configuration_ready = true;
    const char *bootstrap_source = site_directory_bootstrap_source(
        joiner, joiner_configuration, joiner_mirror, joiner_site,
        joiner_endpoints, joiner_credentials, 1000);
    require(bootstrap_source != nullptr &&
                std::strcmp(bootstrap_source, owner.node_id) == 0,
            "joiner did not select the paired Owner for Directory bootstrap");

    SiteCoordinatorState owner_site{};
    owner_site.configuration_ready = true;
    owner_site.participants_expected =
        owner_configuration.active_member_count();
    owner_site.quorum_required =
        strict_majority(owner_site.participants_expected);
    owner_site.participants_online = owner_site.participants_expected;
    owner_site.quorum_available = true;
    ParticipantEndpointCache owner_endpoints{};
    if (owner_site.participants_expected > 2) {
      const auto *site_leader = owner_configuration.find(
          "namo-node-existing-0");
      require(site_leader != nullptr,
              "large Site fixture has no independent Site Leader");
      std::strcpy(owner_site.leader_node_id, site_leader->participant_id);
      observe_flow_peer(&owner_endpoints, owner_configuration,
                        owner_directory, site_leader->participant_id,
                        0xC0A80102U);
    }
    require(site_directory_bootstrap_request_authorized(
                owner, owner_configuration, joiner.node_id, owner_directory,
                owner_site, owner_endpoints, 1000),
            "Owner rejected the bounded read-only bootstrap request");

    ParticipantReadinessRegistry owner_readiness{};
    ParticipantReadinessRegistry joiner_readiness{};
    const auto readiness = flow_readiness();
    require(owner_readiness.configure(joiner.node_id, readiness) ==
                ParticipantReadinessResult::OK &&
                joiner_readiness.configure(owner.node_id, readiness) ==
                    ParticipantReadinessResult::OK,
            "activation readiness fixture failed");
    require(validate_namo_node_activation(
                joiner_configuration, joiner_readiness.find(owner.node_id),
                owner.node_id, true, true,
                joiner_configuration.generation,
                joiner_configuration.floorplan.geometry_version,
                joiner_configuration.floorplan.config_hash,
                joiner_configuration.floorplan.image_hash) ==
                SiteActivationAdmissionResult::OK,
            "a stale same-Site Directory must converge after activation instead of blocking it");

    require(validate_replicated_site_directory(
                joiner.site_id, joiner_mirror, owner_directory) ==
                SiteDirectoryReplicationValidation::OK &&
                directory_store.update(directory_backend, owner_directory,
                                       &joiner_mirror) ==
                    SiteDirectoryStoreResult::UPDATED &&
                site_directory_is_canonical_for_configuration(
                    joiner_configuration, joiner_mirror),
            "canonical Directory bootstrap did not persist");

    SiteDirectoryStore restarted_directory_store{};
    SiteDirectorySnapshot restarted_directory{};
    require(restarted_directory_store.load(directory_backend,
                                           &restarted_directory) ==
                SiteDirectoryStoreResult::LOADED &&
                same_site_directory_payload(restarted_directory,
                                            owner_directory),
            "canonical Directory did not survive restart reload");
    require(site_directory_bootstrap_source(
                joiner, joiner_configuration, restarted_directory,
                joiner_site, joiner_endpoints, joiner_credentials, 1000) ==
                nullptr,
            "a canonical restart attempted enrollment bootstrap again");

    require(validate_namo_node_activation(
                owner_configuration, owner_readiness.find(joiner.node_id),
                joiner.node_id, true, true,
                owner_configuration.generation,
                owner_configuration.floorplan.geometry_version,
                owner_configuration.floorplan.config_hash,
                owner_configuration.floorplan.image_hash) ==
                SiteActivationAdmissionResult::OK &&
                validate_namo_node_activation(
                    joiner_configuration,
                    joiner_readiness.find(owner.node_id), owner.node_id, true,
                    true, joiner_configuration.generation,
                    joiner_configuration.floorplan.geometry_version,
                    joiner_configuration.floorplan.config_hash,
                    joiner_configuration.floorplan.image_hash) ==
                    SiteActivationAdmissionResult::OK,
            "bilateral activation admission failed after canonical bootstrap");
  }
}

}  // namespace

void run_pairing_bootstrap_activation_tests() {
  pairing_bootstrap_activation_flow_survives_restart_to_seven_nodes();
}

#ifdef NAMO_PAIRING_BOOTSTRAP_ACTIVATION_TEST_MAIN
int main() {
  run_pairing_bootstrap_activation_tests();
  std::puts("pairing_bootstrap_activation_tests: ok");
  return 0;
}
#endif
