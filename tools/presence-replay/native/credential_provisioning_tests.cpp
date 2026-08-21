#include "credential_provisioning.h"
#include "site_directory.h"
#include "site_heartbeat.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace esphome::radar_api_server;

namespace {

void require(bool condition, const char *message) {
  if (condition)
    return;
  std::fprintf(stderr, "credential_provisioning_tests: %s\n", message);
  std::exit(1);
}

PlatformIdentity make_identity(const char *node_id) {
  PlatformIdentity identity{};
  std::strcpy(identity.site_id, "site-home");
  std::strcpy(identity.node_id, node_id);
  return identity;
}

SiteConfigurationSnapshot make_configuration() {
  SiteConfigurationSnapshot configuration{};
  std::strcpy(configuration.site_id, "site-home");
  std::strcpy(configuration.configuration_owner_node_id, "node-owner");
  configuration.authority_epoch = 1;
  configuration.generation = 9;
  configuration.roster_revision = 9;
  configuration.roster_committed = true;
  configuration.floorplan.committed = true;
  std::strcpy(configuration.floorplan.floorplan_id, "floor-main");
  configuration.floorplan.geometry_version = 1;
  configuration.floorplan.config_hash = 101;
  configuration.floorplan.image_hash = 202;
  const std::array<const char *, 7> ids{
      "node-owner", "node-a", "node-b", "node-c",
      "node-d", "node-e", "node-f"};
  for (std::size_t index = 0; index < ids.size(); ++index) {
    auto &member = configuration.members[index];
    member.occupied = true;
    std::strcpy(member.participant_id, ids[index]);
    member.type = ParticipantType::NAMO_NODE;
    member.trust = ParticipantTrust::OFFICIAL_VERIFIED;
    member.membership = ParticipantMembership::ACTIVE;
    member.leader_eligible = true;
    member.spatial_profile.committed = true;
    std::strcpy(member.spatial_profile.space_id, "room-main");
    std::strcpy(member.spatial_profile.group_id, "group-main");
    member.spatial_profile.pose_version = 1;
    member.spatial_profile.calibration_version = 1;
    member.spatial_profile.transform.valid = true;
    member.spatial_profile.transform.sample_count = 1;
  }
  return configuration;
}

SiteDirectorySnapshot make_directory() {
  SiteDirectorySnapshot directory{};
  std::strcpy(directory.site_id, "site-home");
  directory.generation = 1;
  directory.topology_revision = 9;
  directory.committed = true;
  auto &group = directory.groups[0];
  group.occupied = true;
  std::strcpy(group.group_id, "group-main");
  std::strcpy(group.floorplan_id, "floor-main");
  std::strcpy(group.leader_node_id, "node-a");
  group.assignment_revision = 4;
  group.member_count = 7;
  return directory;
}

void set_credential(ParticipantCredentialRegistry *registry,
                    const char *participant_id, uint8_t seed) {
  std::array<uint8_t, 32> key{};
  key.fill(seed);
  require(registry->set(participant_id, 1000 + seed, 1, key.data(), true) ==
              ParticipantCredentialResult::OK,
          "credential setup failed");
}

class HandoffCredentialBackend : public ParticipantCredentialStorageBackend {
 public:
  bool load_participant_credential_slot(
      uint8_t slot, ParticipantCredentialStorageBlob *blob) override {
    if (slot >= 2 || !slot_present[slot] || blob == nullptr)
      return false;
    *blob = slots[slot];
    return true;
  }

  bool save_participant_credential_slot(
      uint8_t slot, const ParticipantCredentialStorageBlob &blob) override {
    if (slot >= 2 || fail_slot_write)
      return false;
    slots[slot] = blob;
    slot_present[slot] = true;
    return true;
  }

  bool load_participant_credential_commit(
      ParticipantCredentialCommitBlob *blob) override {
    if (!commit_present || blob == nullptr)
      return false;
    *blob = commit;
    return true;
  }

  bool save_participant_credential_commit(
      const ParticipantCredentialCommitBlob &blob) override {
    if (fail_commit_write)
      return false;
    commit = blob;
    commit_present = true;
    return true;
  }

  bool sync() override { return !fail_sync; }

  ParticipantCredentialStorageBlob slots[2]{};
  ParticipantCredentialCommitBlob commit{};
  bool slot_present[2]{false, false};
  bool commit_present{false};
  bool fail_slot_write{false};
  bool fail_commit_write{false};
  bool fail_sync{false};
};

void seven_member_group_receives_bounded_mesh() {
  const auto owner = make_identity("node-owner");
  const auto configuration = make_configuration();
  const auto directory = make_directory();
  ParticipantCredentialRegistry owner_credentials{};
  for (uint8_t index = 1; index < 7; ++index) {
    char id[16]{};
    std::snprintf(id, sizeof(id), "node-%c", 'a' + index - 1);
    set_credential(&owner_credentials, id, index);
  }

  for (uint8_t requester_index = 1; requester_index < 7;
       ++requester_index) {
    char requester_id[16]{};
    std::snprintf(requester_id, sizeof(requester_id), "node-%c",
                  'a' + requester_index - 1);
    ParticipantCredentialRegistry bundle{};
    require(prepare_site_credential_bundle(
                owner, configuration, &directory, "node-b",
                owner_credentials, requester_id, &bundle) ==
                CredentialProvisioningResult::OK &&
                bundle.size() == 6,
            "each group member should receive the owner and five peer keys");

    auto requester = make_identity(requester_id);
    ParticipantCredentialRegistry local{};
    ParticipantCredentialRegistry merge_scratch{};
    set_credential(&local, "node-owner", requester_index);
    bool changed = false;
    require(merge_site_credential_bundle(
                requester, configuration, &directory, "node-b", bundle,
                &local, &merge_scratch, &changed) ==
                CredentialProvisioningResult::OK &&
                changed && local.size() == 6 &&
                site_credentials_complete_for_node(
                    requester, configuration, &directory, "node-b", local),
            "every merged seven-member credential view should be complete");
    require(group_leader_credentials_complete(
                requester_id, "group-main", configuration, local),
            "every eligible replacement leader should own six remote keys");
  }
}

void missing_owner_record_fails_closed() {
  const auto owner = make_identity("node-owner");
  const auto configuration = make_configuration();
  const auto directory = make_directory();
  ParticipantCredentialRegistry incomplete{};
  set_credential(&incomplete, "node-a", 1);
  ParticipantCredentialRegistry bundle{};
  require(prepare_site_credential_bundle(
              owner, configuration, &directory, "node-b", incomplete,
              "node-b", &bundle) ==
              CredentialProvisioningResult::SOURCE_CREDENTIAL_MISSING &&
              bundle.size() == 0,
          "the owner must not emit a partial credential bundle");
}

void quarantined_pair_uses_only_its_direct_owner_bootstrap() {
  auto configuration = make_configuration();
  const auto owner = make_identity("node-owner");
  const auto requester = make_identity("node-b");
  SiteConfigurationMember *requester_member = nullptr;
  for (auto &member : configuration.members) {
    if (member.occupied &&
        std::strcmp(member.participant_id, requester.node_id) == 0) {
      requester_member = &member;
      break;
    }
  }
  require(requester_member != nullptr,
          "quarantined bootstrap requester fixture is missing");
  requester_member->membership = ParticipantMembership::QUARANTINED;
  requester_member->leader_eligible = false;
  requester_member->spatial_profile = {};

  ParticipantRegistry direct_pairs{};
  require(direct_pairs.discover(owner.node_id, ParticipantType::NAMO_NODE) ==
              ParticipantRegistryResult::OK &&
              direct_pairs.classify(owner.node_id,
                                    ParticipantTrust::OFFICIAL_VERIFIED) ==
                  ParticipantRegistryResult::OK,
          "direct Owner pairing fixture should be accepted");
  ParticipantCredentialRegistry credentials{};
  set_credential(&credentials, owner.node_id, 2);

  require(site_credential_bootstrap_ready_for_node(
              requester, configuration, direct_pairs, credentials),
          "a directly paired quarantined Node must retain control-plane heartbeat bootstrap");

  ParticipantCredentialRegistry bundle{};
  require(prepare_site_credential_bundle(
              owner, configuration, nullptr, owner.node_id, credentials,
              requester.node_id, &bundle) ==
              CredentialProvisioningResult::REQUESTER_INACTIVE,
          "heartbeat bootstrap must not widen into an active credential bundle");

  require(direct_pairs.revoke(owner.node_id) == ParticipantRegistryResult::OK &&
              !site_credential_bootstrap_ready_for_node(
                  requester, configuration, direct_pairs, credentials),
          "revoking the direct Owner relationship must close bootstrap");

  ParticipantRegistry restored_pair{};
  require(restored_pair.discover(owner.node_id, ParticipantType::NAMO_NODE) ==
              ParticipantRegistryResult::OK &&
              restored_pair.classify(owner.node_id,
                                     ParticipantTrust::OFFICIAL_VERIFIED) ==
                  ParticipantRegistryResult::OK,
          "direct Owner pairing fixture should be restorable");
  require(credentials.erase(owner.node_id) == ParticipantCredentialResult::OK &&
              !site_credential_bootstrap_ready_for_node(
                  requester, configuration, restored_pair, credentials),
          "missing pairwise key must close bootstrap");

  set_credential(&credentials, owner.node_id, 2);
  requester_member->membership = ParticipantMembership::ACTIVE;
  require(!site_credential_bootstrap_ready_for_node(
              requester, configuration, restored_pair, credentials),
          "active Nodes must use normal bounded credential provisioning");
}

void staged_join_expands_the_credential_mesh_from_two_to_seven_members() {
  const std::array<const char *, 7> ids{
      "node-owner", "node-a", "node-b", "node-c",
      "node-d", "node-e", "node-f"};
  const auto owner = make_identity(ids[0]);

  for (std::size_t total = 2; total <= ids.size(); ++total) {
    auto configuration = make_configuration();
    for (std::size_t index = total; index < ids.size(); ++index)
      configuration.members[index] = {};
    auto &joining_member = configuration.members[total - 1];
    const auto joining_profile = joining_member.spatial_profile;
    joining_member.membership = ParticipantMembership::QUARANTINED;
    joining_member.leader_eligible = false;
    joining_member.spatial_profile = {};

    auto directory = make_directory();
    directory.groups[0].member_count = static_cast<uint8_t>(total - 1);
    ParticipantCredentialRegistry owner_credentials{};
    for (std::size_t index = 1; index < total; ++index) {
      set_credential(&owner_credentials, ids[index],
                     static_cast<uint8_t>(index));
    }

    ParticipantCredentialRegistry joining_bundle{};
    require(prepare_site_credential_bundle(
                owner, configuration, &directory, ids[0], owner_credentials,
                ids[total - 1], &joining_bundle) ==
                CredentialProvisioningResult::REQUESTER_INACTIVE &&
                joining_bundle.size() == 0,
            "a staged joining Node must not receive the active Site mesh");

    for (std::size_t requester = 1; requester + 1 < total; ++requester) {
      ParticipantCredentialRegistry bundle{};
      require(prepare_site_credential_bundle(
                  owner, configuration, &directory, ids[0],
                  owner_credentials, ids[requester], &bundle) ==
                  CredentialProvisioningResult::OK &&
                  bundle.size() == total - 2,
              "a staged join must not widen an existing member credential bundle");
    }

    joining_member.membership = ParticipantMembership::ACTIVE;
    joining_member.leader_eligible = true;
    joining_member.spatial_profile = joining_profile;
    directory.groups[0].member_count = static_cast<uint8_t>(total);
    for (std::size_t requester = 1; requester < total; ++requester) {
      ParticipantCredentialRegistry bundle{};
      require(prepare_site_credential_bundle(
                  owner, configuration, &directory, ids[0],
                  owner_credentials, ids[requester], &bundle) ==
                  CredentialProvisioningResult::OK &&
                  bundle.size() == total - 1,
              "activation must provision every required Site credential");

      ParticipantCredentialRegistry local{};
      ParticipantCredentialRegistry merge_scratch{};
      set_credential(&local, ids[0], static_cast<uint8_t>(requester));
      bool changed = false;
      const auto merge_result = merge_site_credential_bundle(
          make_identity(ids[requester]), configuration, &directory,
          ids[0], bundle, &local, &merge_scratch, &changed);
      require((merge_result == CredentialProvisioningResult::OK ||
               merge_result ==
                   CredentialProvisioningResult::ALREADY_COMPLETE) &&
                  changed ==
                      (merge_result == CredentialProvisioningResult::OK) &&
                  local.size() == total - 1 &&
                  site_credentials_complete_for_node(
                      make_identity(ids[requester]), configuration,
                      &directory, ids[0], local),
              "the activated joining topology must converge at every capacity");
    }
  }
}

void unrelated_bundle_record_is_rejected() {
  auto configuration = make_configuration();
  std::strcpy(configuration.members[6].spatial_profile.group_id,
              "group-other");
  configuration.members[6].leader_eligible = false;
  const auto directory = make_directory();
  auto node_b = make_identity("node-b");
  ParticipantCredentialRegistry local{};
  set_credential(&local, "node-owner", 2);
  ParticipantCredentialRegistry bundle{};
  ParticipantCredentialRegistry merge_scratch{};
  set_credential(&bundle, "node-f", 6);
  bool changed = false;
  require(merge_site_credential_bundle(
              node_b, configuration, &directory, "node-owner", bundle,
              &local, &merge_scratch, &changed) ==
              CredentialProvisioningResult::INVALID_BUNDLE &&
              !changed,
          "a bundle must not widen trust outside the required topology");
}

void successful_merge_prunes_credentials_outside_current_topology() {
  const auto configuration = make_configuration();
  const auto directory = make_directory();
  const auto owner = make_identity("node-owner");
  const auto requester = make_identity("node-b");
  ParticipantCredentialRegistry owner_credentials{};
  for (uint8_t index = 1; index < 7; ++index) {
    char id[16]{};
    std::snprintf(id, sizeof(id), "node-%c", 'a' + index - 1);
    set_credential(&owner_credentials, id, index);
  }

  ParticipantCredentialRegistry bundle{};
  require(prepare_site_credential_bundle(
              owner, configuration, &directory, "node-b",
              owner_credentials, requester.node_id, &bundle) ==
              CredentialProvisioningResult::OK,
          "the owner should prepare a complete pruning bundle");
  ParticipantCredentialRegistry local = bundle;
  set_credential(&local, "node-obsolete", 20);
  require(local.size() == bundle.size() + 1,
          "obsolete credential fixture was not created");

  ParticipantCredentialRegistry merge_scratch{};
  bool changed = false;
  require(merge_site_credential_bundle(
              requester, configuration, &directory, "node-b", bundle,
              &local, &merge_scratch, &changed) ==
                  CredentialProvisioningResult::OK &&
              changed && local.size() == bundle.size() &&
              local.find("node-obsolete") == nullptr,
          "a successful merge must prune credentials outside the current topology");

  changed = true;
  require(merge_site_credential_bundle(
              requester, configuration, &directory, "node-b", bundle,
              &local, &merge_scratch, &changed) ==
                  CredentialProvisioningResult::ALREADY_COMPLETE &&
              !changed,
          "an exact credential topology should remain unchanged");
}

void leader_candidates_form_a_low_frequency_site_mesh() {
  auto configuration = make_configuration();
  std::strcpy(configuration.members[3].spatial_profile.group_id,
              "group-other");
  const auto directory = make_directory();
  const auto node_b = make_identity("node-b");
  require(site_credential_required_for_node(
              node_b, configuration, &directory, "node-a", "node-c"),
          "leader-eligible Nodes must authenticate across Group boundaries");

  configuration.members[4].leader_eligible = false;
  std::strcpy(configuration.members[4].spatial_profile.group_id,
              "group-third");
  require(!site_credential_required_for_node(
              node_b, configuration, &directory, "node-a", "node-d"),
          "an unrelated non-leader must stay outside the Site credential mesh");
}

void sender_identity_uses_the_directional_owner_key() {
  const auto configuration = make_configuration();
  const auto owner = make_identity("node-owner");
  const auto node_b = make_identity("node-b");

  ParticipantCredentialRegistry owner_credentials{};
  set_credential(&owner_credentials, "node-a", 1);
  set_credential(&owner_credentials, "node-b", 2);
  const auto *owner_to_b = select_site_sender_credential(
      owner, &configuration, owner_credentials, "node-b");
  require(owner_to_b != nullptr && owner_to_b->key[0] == 2,
          "the Owner must use the target's direct credential");

  ParticipantCredentialRegistry node_b_credentials{};
  set_credential(&node_b_credentials, "node-owner", 2);
  set_credential(&node_b_credentials, "node-a", 1);
  const auto *node_b_to_a = select_site_sender_credential(
      node_b, &configuration, node_b_credentials, "node-a");
  require(node_b_to_a != nullptr && node_b_to_a->key[0] == 2 &&
              std::strcmp(node_b_to_a->participant_id, "node-owner") == 0,
          "a non-Owner must identify itself with its Owner pairing key");
}

void owner_handover_rebinds_each_directional_pair() {
  auto configuration = make_configuration();
  const auto directory = make_directory();
  const auto new_owner = make_identity("node-b");
  std::strcpy(configuration.configuration_owner_node_id, "node-b");
  configuration.authority_epoch = 2;
  configuration.roster_revision = 10;

  ParticipantCredentialRegistry new_owner_credentials{};
  set_credential(&new_owner_credentials, "node-owner", 2);
  set_credential(&new_owner_credentials, "node-a", 1);
  set_credential(&new_owner_credentials, "node-c", 3);
  set_credential(&new_owner_credentials, "node-d", 4);
  set_credential(&new_owner_credentials, "node-e", 5);
  set_credential(&new_owner_credentials, "node-f", 6);

  ParticipantCredentialRegistry bundle{};
  require(prepare_site_credential_bundle(
              new_owner, configuration, &directory, "node-c",
              new_owner_credentials, "node-a", &bundle) ==
              CredentialProvisioningResult::OK,
          "the new Owner should prepare a handover bundle");
  const auto *owner_record = bundle.find("node-b");
  require(owner_record != nullptr && owner_record->key[0] == 1,
          "the new Owner record must carry the requester's sender key");

  auto node_a = make_identity("node-a");
  ParticipantCredentialRegistry node_a_credentials{};
  set_credential(&node_a_credentials, "node-b", 2);
  set_credential(&node_a_credentials, "node-owner", 1);
  set_credential(&node_a_credentials, "node-c", 3);
  set_credential(&node_a_credentials, "node-d", 4);
  set_credential(&node_a_credentials, "node-e", 5);
  set_credential(&node_a_credentials, "node-f", 6);
  const auto *handover_request_key =
      node_a_credentials.find("node-owner");
  require(handover_request_key != nullptr &&
              credential_bundle_binds_requester(
                  bundle, "node-b", *handover_request_key),
          "the replacement bundle must bind the expected Owner to the requester's key");
  bool changed = false;
  ParticipantCredentialRegistry merge_scratch{};
  require(merge_site_credential_bundle(
              node_a, configuration, &directory, "node-c", bundle,
              &node_a_credentials, &merge_scratch, &changed) ==
              CredentialProvisioningResult::OK &&
              changed,
          "the follower should replace its new-Owner directional key");
  const auto *node_a_sender = select_site_sender_credential(
      node_a, &configuration, node_a_credentials, "node-c");
  const auto *new_owner_to_a = select_site_sender_credential(
      new_owner, &configuration, new_owner_credentials, "node-a");
  require(node_a_sender != nullptr && new_owner_to_a != nullptr &&
              node_a_sender->participant_token ==
                  new_owner_to_a->participant_token &&
              node_a_sender->key == new_owner_to_a->key,
          "both directions must converge on the same pair after handover");
}

void seven_member_owner_handover_rebinds_and_persists_every_pair() {
  const std::array<const char *, 7> ids{
      "node-owner", "node-a", "node-b", "node-c",
      "node-d", "node-e", "node-f"};
  const auto old_owner = make_identity(ids[0]);
  const auto old_configuration = make_configuration();
  const auto directory = make_directory();
  std::array<ParticipantCredentialRegistry, 7> before{};
  std::array<ParticipantCredentialRegistry, 7> after{};
  std::array<ParticipantCredentialRegistry, 7> stale_bundles{};

  for (uint8_t index = 1; index < ids.size(); ++index)
    set_credential(&before[0], ids[index], index);
  after[0] = before[0];
  for (uint8_t requester = 1; requester < ids.size(); ++requester) {
    set_credential(&before[requester], ids[0], requester);
    require(prepare_site_credential_bundle(
                old_owner, old_configuration, &directory, "node-c",
                before[0], ids[requester], &stale_bundles[requester]) ==
                CredentialProvisioningResult::OK,
            "old Owner failed to provision a seven-member requester");
    ParticipantCredentialRegistry scratch{};
    bool changed = false;
    require(merge_site_credential_bundle(
                make_identity(ids[requester]), old_configuration, &directory,
                "node-c", stale_bundles[requester], &before[requester],
                &scratch, &changed) == CredentialProvisioningResult::OK &&
                changed && site_credentials_complete_for_node(
                               make_identity(ids[requester]),
                               old_configuration, &directory, "node-c",
                               before[requester]),
            "seven-member pre-handover credential mesh did not converge");
    after[requester] = before[requester];
  }

  const auto *successor =
      automatic_configuration_successor(old_configuration);
  require(successor != nullptr &&
              std::strcmp(successor->participant_id, old_owner.node_id) != 0,
          "seven-member handover did not select a successor");
  const auto new_owner = make_identity(successor->participant_id);
  SiteConfigurationSnapshot handed_over{};
  require(prepare_automatic_configuration_handover(
              new_owner, old_configuration, &handed_over) ==
                  SiteConfigurationResult::OK &&
              handed_over.member_count() == ids.size() &&
              handed_over.authority_epoch ==
                  old_configuration.authority_epoch + 1,
          "seven-member automatic handover configuration failed");
  std::size_t new_owner_index = 0;
  while (new_owner_index < ids.size() &&
         std::strcmp(ids[new_owner_index], new_owner.node_id) != 0)
    ++new_owner_index;
  require(new_owner_index < ids.size(),
          "seven-member successor is outside the credential fixture");

  for (std::size_t requester = 0; requester < ids.size(); ++requester) {
    if (requester == new_owner_index)
      continue;
    const auto requester_identity = make_identity(ids[requester]);
    ParticipantEndpointCache handover_routes{};
    require(handover_routes.observe_discovery(
                new_owner.node_id,
                static_cast<uint32_t>(0x0100007FU + requester), 49191, 1000),
            "handover requester could not retain the new Owner route");
    require(select_credential_provisioning_source(
                requester_identity, handed_over, handover_routes,
                before[requester], nullptr, 0, 1000) == nullptr,
            "an unauthenticated route entered normal credential refresh");
    const auto *bootstrap_source = select_credential_provisioning_source(
        requester_identity, handed_over, handover_routes, before[requester],
        nullptr, 0, 1000, true);
    require(bootstrap_source != nullptr &&
                std::strcmp(bootstrap_source->participant_id,
                            new_owner.node_id) == 0,
            "Owner handover could not open the bounded credential bootstrap route");

    bool requester_probe_resolves = false;
    for (std::size_t probe_index = 0;; ++probe_index) {
      const auto *probe =
          credential_probe_at(before[requester], probe_index);
      if (probe == nullptr)
        break;
      const auto *resolved = before[new_owner_index].resolve_provisioned(
          probe->participant_token, probe->key_version);
      if (resolved != nullptr && resolved->key == probe->key &&
          std::strcmp(resolved->participant_id, ids[requester]) == 0) {
        requester_probe_resolves = true;
        break;
      }
    }
    require(requester_probe_resolves,
            "new Owner cannot authenticate the requester's pre-handover credential probe");

    require(site_credentials_complete_for_node(
                requester_identity, handed_over, &directory, "node-c",
                before[requester]),
            "a complete pre-handover snapshot must keep authenticated traffic "
            "available while the new Owner bundle refreshes");

    const auto *request_key = select_site_sender_credential(
        requester_identity, &old_configuration, before[requester],
        new_owner.node_id);
    require(request_key != nullptr,
            "handover requester lost its pre-handover authentication key");

    ParticipantCredentialRegistry bundle{};
    require(prepare_site_credential_bundle(
                new_owner, handed_over, &directory, "node-c",
                before[new_owner_index], ids[requester], &bundle) ==
                    CredentialProvisioningResult::OK &&
                credential_bundle_binds_requester(
                    bundle, new_owner.node_id, *request_key),
            "new Owner bundle did not bind a seven-member requester");
    if (requester != 0) {
      require(!credential_bundle_binds_requester(
                  stale_bundles[requester], new_owner.node_id,
                  *request_key),
              "stale Owner bundle remained valid after handover");
    }

    ParticipantCredentialRegistry scratch{};
    bool changed = false;
    const auto merge_result = merge_site_credential_bundle(
        requester_identity, handed_over, &directory, "node-c", bundle,
        &after[requester], &scratch, &changed);
    require((merge_result == CredentialProvisioningResult::OK ||
             merge_result == CredentialProvisioningResult::ALREADY_COMPLETE) &&
                changed ==
                    (merge_result == CredentialProvisioningResult::OK) &&
                site_credentials_complete_for_node(
                    requester_identity, handed_over, &directory, "node-c",
                    after[requester]),
            "seven-member handover bundle did not converge");

    HandoffCredentialBackend backend{};
    ParticipantCredentialStore store{};
    ParticipantCredentialSnapshot runtime{};
    require(store.create(backend, before[requester], &runtime) ==
                ParticipantCredentialStoreResult::CREATED,
            "handover credential persistence bootstrap failed");
    if (changed) {
      backend.fail_commit_write = true;
      require(store.update(backend, after[requester], &runtime) ==
                  ParticipantCredentialStoreResult::STORAGE_FAILED &&
                  site_credentials_complete_for_node(
                      requester_identity, old_configuration, &directory,
                      "node-c", runtime.registry),
              "failed credential commit published the new authority");
      backend.fail_commit_write = false;
      ParticipantCredentialStore failed_reboot_store{};
      ParticipantCredentialSnapshot failed_reboot{};
      require(failed_reboot_store.load(backend, &failed_reboot) ==
                  ParticipantCredentialStoreResult::LOADED &&
                  site_credentials_complete_for_node(
                      requester_identity, old_configuration, &directory,
                      "node-c", failed_reboot.registry) &&
                  !credential_bundle_binds_requester(
                      failed_reboot.registry, new_owner.node_id,
                      *request_key),
              "reboot after credential commit failure did not restore the old authority");
      require(store.update(backend, after[requester], &runtime) ==
                  ParticipantCredentialStoreResult::UPDATED,
              "credential handover retry did not persist");
    }
    ParticipantCredentialStore successful_reboot_store{};
    ParticipantCredentialSnapshot successful_reboot{};
    require(successful_reboot_store.load(backend, &successful_reboot) ==
                ParticipantCredentialStoreResult::LOADED &&
                site_credentials_complete_for_node(
                    requester_identity, handed_over, &directory, "node-c",
                    successful_reboot.registry),
            "persisted handover credentials did not survive reboot");
  }

  require(site_credentials_complete_for_node(
              new_owner, handed_over, &directory, "node-c",
              after[new_owner_index]),
          "new Owner credential registry is incomplete after handover");
  for (std::size_t sender = 0; sender < ids.size(); ++sender) {
    require(group_leader_credentials_complete(
                ids[sender], "group-main", handed_over, after[sender]),
            "a seven-member replacement Group Leader lacks peer keys");
    for (std::size_t receiver = 0; receiver < ids.size(); ++receiver) {
      if (sender == receiver)
        continue;
      const auto *send_credential = select_site_sender_credential(
          make_identity(ids[sender]), &handed_over, after[sender],
          ids[receiver]);
      const auto *receive_credential =
          send_credential != nullptr
              ? after[receiver].resolve(send_credential->participant_token,
                                        send_credential->key_version)
              : nullptr;
      require(send_credential != nullptr && receive_credential != nullptr &&
                  receive_credential->key == send_credential->key &&
                  std::strcmp(receive_credential->participant_id,
                              ids[sender]) == 0,
              "post-handover authenticated sender key did not resolve at its peer");
    }
  }
}

void stale_owner_bundle_cannot_complete_handover() {
  auto old_configuration = make_configuration();
  const auto directory = make_directory();
  const auto old_owner = make_identity("node-owner");
  ParticipantCredentialRegistry old_owner_credentials{};
  for (uint8_t index = 1; index < 7; ++index) {
    char id[16]{};
    std::snprintf(id, sizeof(id), "node-%c", 'a' + index - 1);
    set_credential(&old_owner_credentials, id, index);
  }
  ParticipantCredentialRegistry stale_bundle{};
  require(prepare_site_credential_bundle(
              old_owner, old_configuration, &directory, "node-c",
              old_owner_credentials, "node-a", &stale_bundle) ==
              CredentialProvisioningResult::OK,
          "the old Owner fixture should produce its own valid bundle");

  auto new_configuration = old_configuration;
  std::strcpy(new_configuration.configuration_owner_node_id, "node-b");
  const auto *requester_key = old_owner_credentials.find("node-a");
  require(requester_key != nullptr &&
              !credential_bundle_binds_requester(
                  stale_bundle,
                  new_configuration.configuration_owner_node_id,
                  *requester_key),
          "a bundle from the previous Owner must not bind to the replacement Owner");
}

void owner_handover_probe_finds_the_requester_key() {
  ParticipantCredentialRegistry credentials{};
  set_credential(&credentials, "node-b", 2);
  set_credential(&credentials, "node-owner", 1);
  set_credential(&credentials, "node-c", 3);

  const auto *first = credential_probe_at(credentials, 0);
  const auto *second = credential_probe_at(credentials, 1);
  const auto *third = credential_probe_at(credentials, 2);
  require(first != nullptr && second != nullptr && third != nullptr &&
              credential_probe_at(credentials, 3) == nullptr,
          "credential probing must be bounded by the stored registry");

  const ParticipantCredentialRecord *requester_key = nullptr;
  for (std::size_t index = 0;; ++index) {
    const auto *candidate = credential_probe_at(credentials, index);
    if (candidate == nullptr)
      break;
    if (candidate->key[0] == 1) {
      requester_key = candidate;
      break;
    }
  }
  require(requester_key != nullptr &&
              std::strcmp(requester_key->participant_id, "node-owner") == 0,
          "a follower must retain a bounded path to its own sender key after Owner handover");
}

void owner_handover_bootstrap_reaches_every_requester_from_two_to_seven() {
  const std::array<const char *, 7> ids{
      "node-owner", "node-a", "node-b", "node-c",
      "node-d", "node-e", "node-f"};
  const auto directory = make_directory();

  for (std::size_t member_count = 2; member_count <= ids.size();
       ++member_count) {
    auto old_configuration = make_configuration();
    for (std::size_t index = member_count;
         index < old_configuration.members.size(); ++index)
      old_configuration.members[index] = {};

    std::array<ParticipantCredentialRegistry, 7> credentials{};
    for (std::size_t index = 1; index < member_count; ++index)
      set_credential(&credentials[0], ids[index],
                     static_cast<uint8_t>(index));
    for (std::size_t requester = 1; requester < member_count; ++requester) {
      set_credential(&credentials[requester], ids[0],
                     static_cast<uint8_t>(requester));
      ParticipantCredentialRegistry bundle{};
      require(prepare_site_credential_bundle(
                  make_identity(ids[0]), old_configuration, &directory,
                  "node-a", credentials[0], ids[requester], &bundle) ==
                  CredentialProvisioningResult::OK,
              "pre-handover credential matrix could not build a bundle");
      ParticipantCredentialRegistry scratch{};
      bool changed = false;
      const auto merged = merge_site_credential_bundle(
          make_identity(ids[requester]), old_configuration, &directory,
          "node-a", bundle, &credentials[requester], &scratch, &changed);
      require(merged == CredentialProvisioningResult::OK ||
                  merged == CredentialProvisioningResult::ALREADY_COMPLETE,
              "pre-handover credential matrix could not merge a bundle");
    }

    const auto *successor =
        automatic_configuration_successor(old_configuration);
    require(successor != nullptr &&
                std::strcmp(successor->participant_id, "node-a") == 0,
            "credential bootstrap matrix selected an unexpected successor");
    SiteConfigurationSnapshot handed_over{};
    require(prepare_automatic_configuration_handover(
                make_identity(successor->participant_id), old_configuration,
                &handed_over) == SiteConfigurationResult::OK,
            "credential bootstrap matrix could not build the handover state");

    for (std::size_t requester = 0; requester < member_count; ++requester) {
      if (requester == 1)
        continue;
      ParticipantEndpointCache routes{};
      require(routes.observe_discovery(
                  successor->participant_id,
                  static_cast<uint32_t>(0x1000007FU + requester), 49191,
                  1000),
              "credential bootstrap matrix lost the successor route");
      const auto *source = select_credential_provisioning_source(
          make_identity(ids[requester]), handed_over, routes,
          credentials[requester], nullptr, 0, 1000, true);
      require(source != nullptr &&
                  std::strcmp(source->participant_id,
                              successor->participant_id) == 0,
              "credential bootstrap matrix could not select the successor");

      bool resolved_requester = false;
      for (std::size_t probe_index = 0;; ++probe_index) {
        const auto *probe =
            credential_probe_at(credentials[requester], probe_index);
        if (probe == nullptr)
          break;
        const auto *resolved = credentials[1].resolve_provisioned(
            probe->participant_token, probe->key_version);
        if (resolved != nullptr && resolved->key == probe->key &&
            std::strcmp(resolved->participant_id, ids[requester]) == 0) {
          resolved_requester = true;
          break;
        }
      }
      require(resolved_requester,
              "credential bootstrap matrix could not authenticate a requester");
    }
  }
}

void roster_refresh_does_not_impersonate_an_authority_change() {
  auto configuration = make_configuration();
  require(credential_provisioning_context_change(
              configuration.authority_epoch, configuration.roster_revision,
              configuration.configuration_owner_node_id, configuration) ==
              CredentialProvisioningContextChange::NONE,
          "an unchanged credential context should remain stable");

  configuration.roster_revision++;
  configuration.generation++;
  require(credential_provisioning_context_change(
              configuration.authority_epoch,
              configuration.roster_revision - 1,
              configuration.configuration_owner_node_id, configuration) ==
              CredentialProvisioningContextChange::ROSTER_REFRESH,
          "a roster-only update must request refresh without resetting authority");

  configuration.authority_epoch++;
  std::strcpy(configuration.configuration_owner_node_id, "node-a");
  require(credential_provisioning_context_change(
              configuration.authority_epoch - 1,
              configuration.roster_revision,
              "node-owner", configuration) ==
              CredentialProvisioningContextChange::AUTHORITY_REFRESH,
          "an Owner or epoch transition must request a non-destructive "
          "credential refresh");
}

void credential_refresh_waits_for_the_owner_to_converge() {
  constexpr uint32_t now_ms = 10000;
  const auto configuration = make_configuration();
  ParticipantEndpoint endpoint{};
  endpoint.occupied = true;
  endpoint.authenticated = true;
  endpoint.ipv4 = 0x0100007FU;
  endpoint.authenticated_at_ms = now_ms - 1000;
  endpoint.heartbeat_wire_version =
      SiteHeartbeatEnvelope::CURRENT_WIRE_VERSION;
  endpoint.site_configuration_authority_epoch =
      configuration.authority_epoch;
  endpoint.site_configuration_generation = configuration.generation;
  endpoint.site_configuration_digest =
      site_configuration_digest(configuration);
  endpoint.site_topology_revision = configuration.roster_revision;
  endpoint.site_topology_digest = site_topology_digest(configuration);
  endpoint.site_credentials_complete = true;

  require(credential_provisioning_source_readiness(
              nullptr, configuration, now_ms) ==
              CredentialProvisioningSourceReadiness::ENDPOINT_MISSING,
          "credential refresh must wait for the Owner endpoint");

  endpoint.authenticated = false;
  require(credential_provisioning_source_readiness(
              &endpoint, configuration, now_ms) ==
              CredentialProvisioningSourceReadiness::ENDPOINT_UNAUTHENTICATED,
          "credential refresh must wait for an authenticated Owner endpoint");
  endpoint.authenticated = true;

  endpoint.ipv4 = 0;
  require(credential_provisioning_source_readiness(
              &endpoint, configuration, now_ms) ==
              CredentialProvisioningSourceReadiness::ENDPOINT_ADDRESS_MISSING,
          "credential refresh must wait for an Owner address");
  endpoint.ipv4 = 0x0100007FU;

  endpoint.authenticated_at_ms =
      now_ms - ParticipantEndpointCache::ONLINE_TIMEOUT_MS;
  require(credential_provisioning_source_readiness(
              &endpoint, configuration, now_ms) ==
              CredentialProvisioningSourceReadiness::ENDPOINT_OFFLINE,
          "credential refresh must not use an expired Owner endpoint");
  endpoint.authenticated_at_ms = now_ms - 1000;

  endpoint.heartbeat_wire_version = 7;
  require(credential_provisioning_source_readiness(
              &endpoint, configuration, now_ms) ==
              CredentialProvisioningSourceReadiness::LEGACY_HEARTBEAT,
          "credential refresh must wait for the credential mesh contract");
  endpoint.heartbeat_wire_version = 8;

  endpoint.site_configuration_authority_epoch--;
  require(credential_provisioning_source_readiness(
              &endpoint, configuration, now_ms) ==
              CredentialProvisioningSourceReadiness::AUTHORITY_EPOCH_MISMATCH,
          "credential refresh must wait for the Owner authority epoch");
  endpoint.site_configuration_authority_epoch =
      configuration.authority_epoch;

  endpoint.site_configuration_generation--;
  require(credential_provisioning_source_readiness(
              &endpoint, configuration, now_ms) ==
              CredentialProvisioningSourceReadiness::
                  CONFIGURATION_GENERATION_MISMATCH,
          "credential refresh must wait for the Owner generation");
  endpoint.site_configuration_generation = configuration.generation;

  endpoint.site_configuration_digest ^= 1U;
  require(credential_provisioning_source_readiness(
              &endpoint, configuration, now_ms) ==
              CredentialProvisioningSourceReadiness::
                  CONFIGURATION_DIGEST_MISMATCH,
          "credential refresh must wait for the Owner configuration digest");
  endpoint.site_configuration_digest =
      site_configuration_digest(configuration);

  endpoint.site_topology_revision--;
  require(credential_provisioning_source_readiness(
              &endpoint, configuration, now_ms) ==
              CredentialProvisioningSourceReadiness::
                  TOPOLOGY_REVISION_MISMATCH,
          "credential refresh must wait for the Owner roster topology");
  endpoint.site_topology_revision = configuration.roster_revision;

  endpoint.site_topology_digest ^= 1U;
  require(credential_provisioning_source_readiness(
              &endpoint, configuration, now_ms) ==
              CredentialProvisioningSourceReadiness::TOPOLOGY_DIGEST_MISMATCH,
          "credential refresh must wait for the Owner topology digest");
  endpoint.site_topology_digest = site_topology_digest(configuration);

  endpoint.site_credentials_complete = false;
  require(credential_provisioning_source_readiness(
              &endpoint, configuration, now_ms) ==
              CredentialProvisioningSourceReadiness::READY,
          "a delayed credential-complete advertisement must not block a definitive bundle request");

  endpoint.heartbeat_wire_version =
      SiteHeartbeatEnvelope::CURRENT_WIRE_VERSION + 1;
  require(credential_provisioning_source_readiness(
              &endpoint, configuration, now_ms) ==
              CredentialProvisioningSourceReadiness::READY,
          "a future heartbeat must retain the credential mesh capability");
  endpoint.heartbeat_wire_version =
      SiteHeartbeatEnvelope::CURRENT_WIRE_VERSION;

  require(credential_provisioning_source_readiness(
              &endpoint, configuration, now_ms) ==
              CredentialProvisioningSourceReadiness::READY,
          "a fully converged Owner must admit credential refresh");
}

void authority_handover_bootstrap_is_addressed_and_fail_closed() {
  constexpr uint32_t now_ms = 12000;
  auto configuration = make_configuration();
  std::strcpy(configuration.configuration_owner_node_id, "node-a");
  configuration.authority_epoch++;
  configuration.generation++;
  configuration.roster_revision++;
  const auto identity = make_identity("node-f");

  ParticipantCredentialRegistry credentials{};
  set_credential(&credentials, "node-a", 1);
  ParticipantEndpointCache endpoints{};
  require(endpoints.observe_discovery("node-a", 0x0100007FU, 49191,
                                      now_ms),
          "authority bootstrap fixture could not retain the Owner address");
  const auto *owner = endpoints.find("node-a");
  require(credential_provisioning_authority_bootstrap_source_ready(
              owner, configuration, "node-a"),
          "an active NAMO Owner with a discovered address must be usable for authenticated probing");
  require(select_credential_provisioning_source(
              identity, configuration, endpoints, credentials, nullptr, 0,
              now_ms) == nullptr,
          "normal credential refresh accepted an unauthenticated source");
  const auto *selected = select_credential_provisioning_source(
      identity, configuration, endpoints, credentials, nullptr, 0, now_ms,
      true);
  require(selected != nullptr &&
              std::strcmp(selected->participant_id, "node-a") == 0,
          "authority transition did not select its bounded bootstrap source");

  ParticipantEndpoint missing_address = *owner;
  missing_address.ipv4 = 0;
  require(!credential_provisioning_authority_bootstrap_source_ready(
              &missing_address, configuration, "node-a"),
          "authority bootstrap accepted a source without a route");
  require(!credential_provisioning_authority_bootstrap_source_ready(
              owner, configuration, "node-b"),
          "authority bootstrap accepted a mismatched endpoint identity");

  auto inactive = configuration;
  for (auto &member : inactive.members) {
    if (member.occupied &&
        std::strcmp(member.participant_id, "node-a") == 0) {
      member.membership = ParticipantMembership::QUARANTINED;
      break;
    }
  }
  require(!credential_provisioning_authority_bootstrap_source_ready(
              owner, inactive, "node-a"),
          "authority bootstrap accepted a non-active source");
}

void boot_reconciliation_preserves_indirect_canonical_credentials() {
  auto configuration = make_configuration();
  for (auto &member : configuration.members) {
    if (member.occupied &&
        std::strcmp(member.participant_id, "node-e") == 0) {
      member.membership = ParticipantMembership::REVOKED;
      break;
    }
  }
  ParticipantRegistry direct_pairs{};
  require(direct_pairs.discover("node-owner", ParticipantType::NAMO_NODE) ==
                  ParticipantRegistryResult::OK &&
              direct_pairs.classify("node-owner",
                                    ParticipantTrust::OFFICIAL_VERIFIED) ==
                  ParticipantRegistryResult::OK &&
              direct_pairs.activate("node-owner") ==
                  ParticipantRegistryResult::OK,
          "direct pairing fixture failed");

  ParticipantCredentialRegistry credentials{};
  set_credential(&credentials, "node-owner", 1);
  set_credential(&credentials, "node-a", 2);
  set_credential(&credentials, "node-b", 3);
  set_credential(&credentials, "node-c", 4);
  set_credential(&credentials, "node-d", 5);
  set_credential(&credentials, "node-e", 6);
  set_credential(&credentials, "node-orphan", 7);
  require(credentials.set_enabled("node-c", false) ==
              ParticipantCredentialResult::OK,
          "disabled canonical credential fixture failed");

  const auto summary = reconcile_site_credential_registry(
      configuration, direct_pairs, &credentials);
  require(summary.changed() && summary.erased_orphans == 1 &&
              summary.disabled_revoked == 1,
          "boot reconciliation did not report its bounded mutations");
  require(credentials.find("node-a") != nullptr &&
              credentials.find("node-b") != nullptr &&
              credentials.find("node-d") != nullptr &&
              credentials.find("node-orphan") == nullptr,
          "boot reconciliation erased an indirect canonical credential");
  require(!credentials.find("node-c")->enabled &&
              !credentials.find("node-e")->enabled,
          "boot reconciliation re-enabled a disabled canonical credential");
}

void missing_owner_key_selects_a_converged_canonical_custodian() {
  constexpr uint32_t now_ms = 20000;
  const auto configuration = make_configuration();
  const auto identity = make_identity("node-f");
  ParticipantCredentialRegistry credentials{};
  set_credential(&credentials, "node-a", 1);
  ParticipantEndpointCache endpoints{};
  endpoints.observe_authenticated("node-a", 0x0100007FU, 49191,
                                   now_ms - 100);
  endpoints.observe_floorplan_replica(
      "node-a", configuration.authority_epoch, configuration.generation,
      site_configuration_digest(configuration),
      configuration.floorplan.geometry_version,
      configuration.floorplan.config_hash,
      configuration.floorplan.image_hash, 1, true);
  endpoints.observe_site_topology("node-a", configuration.roster_revision,
                                  site_topology_digest(configuration));
  endpoints.observe_site_credentials(
      "node-a", SiteHeartbeatEnvelope::CURRENT_WIRE_VERSION, true);

  const auto *selected = select_credential_provisioning_source(
      identity, configuration, endpoints, credentials, "node-a", 0,
      now_ms);
  require(selected != nullptr &&
              std::strcmp(selected->participant_id, "node-a") == 0,
          "a missing Owner key should fall back to an exact canonical custodian");

  endpoints.observe_site_topology("node-a", configuration.roster_revision,
                                  site_topology_digest(configuration) ^ 1U);
  require(select_credential_provisioning_source(
              identity, configuration, endpoints, credentials, "node-a",
              0, now_ms) == nullptr,
          "a topology-mismatched custodian must not provision credentials");
}

void failed_credential_sources_are_tried_once_per_bounded_cycle() {
  constexpr uint32_t now_ms = 30000;
  const auto configuration = make_configuration();
  const auto identity = make_identity("node-f");
  ParticipantCredentialRegistry credentials{};
  ParticipantEndpointCache endpoints{};
  const std::array<const char *, 6> expected_order{
      "node-owner", "node-a", "node-b", "node-c", "node-d", "node-e"};
  uint8_t seed = 1;
  for (const auto *id : expected_order) {
    set_credential(&credentials, id, seed++);
    endpoints.observe_authenticated(id, 0x0100007FU, 49191, now_ms - 100);
    endpoints.observe_floorplan_replica(
        id, configuration.authority_epoch, configuration.generation,
        site_configuration_digest(configuration),
        configuration.floorplan.geometry_version,
        configuration.floorplan.config_hash,
        configuration.floorplan.image_hash, 1, true);
    endpoints.observe_site_topology(id, configuration.roster_revision,
                                    site_topology_digest(configuration));
    endpoints.observe_site_credentials(
        id, SiteHeartbeatEnvelope::CURRENT_WIRE_VERSION, false);
  }

  uint32_t attempted = 0;
  for (const auto *expected : expected_order) {
    const auto *selected = select_credential_provisioning_source(
        identity, configuration, endpoints, credentials, "node-a", attempted,
        now_ms);
    require(selected != nullptr &&
                std::strcmp(selected->participant_id, expected) == 0,
            "the seven-member source cycle skipped or repeated a candidate");
    const uint32_t selected_bit = credential_provisioning_source_bit(
        configuration, selected->participant_id);
    require(selected_bit != 0 && (attempted & selected_bit) == 0,
            "the seven-member source cycle reused an attempted source bit");
    attempted |= selected_bit;
  }
  require(select_credential_provisioning_source(
              identity, configuration, endpoints, credentials, "node-a",
              attempted, now_ms) == nullptr,
          "an exhausted source cycle selected an already attempted node");
}

void source_selection_rejects_satellites_and_prefers_complete_nodes() {
  constexpr uint32_t now_ms = 40000;
  auto configuration = make_configuration();
  const auto identity = make_identity("node-f");
  for (auto &member : configuration.members) {
    if (member.occupied &&
        std::strcmp(member.participant_id, "node-a") == 0) {
      member.type = ParticipantType::SATELLITE;
      break;
    }
  }

  ParticipantCredentialRegistry credentials{};
  set_credential(&credentials, "node-a", 1);
  set_credential(&credentials, "node-b", 2);
  set_credential(&credentials, "node-c", 3);
  ParticipantEndpointCache endpoints{};
  for (const auto *id : {"node-a", "node-b", "node-c"}) {
    endpoints.observe_authenticated(id, 0x0100007FU, 49191, now_ms - 100);
    endpoints.observe_floorplan_replica(
        id, configuration.authority_epoch, configuration.generation,
        site_configuration_digest(configuration),
        configuration.floorplan.geometry_version,
        configuration.floorplan.config_hash,
        configuration.floorplan.image_hash, 1, true);
    endpoints.observe_site_topology(id, configuration.roster_revision,
                                    site_topology_digest(configuration));
    endpoints.observe_site_credentials(
        id, SiteHeartbeatEnvelope::CURRENT_WIRE_VERSION,
        std::strcmp(id, "node-c") == 0);
  }

  const auto *selected = select_credential_provisioning_source(
      identity, configuration, endpoints, credentials, nullptr, 0, now_ms);
  require(selected != nullptr &&
              std::strcmp(selected->participant_id, "node-c") == 0,
          "source selection admitted a Satellite or ignored a complete node");
}

void seven_member_mesh_recovers_through_a_trusted_custodian() {
  auto configuration = make_configuration();
  std::strcpy(configuration.configuration_owner_node_id, "node-b");
  configuration.authority_epoch = 2;
  configuration.roster_revision = 10;
  const auto directory = make_directory();
  const auto custodian = make_identity("node-owner");
  ParticipantCredentialRegistry custodian_credentials{};
  set_credential(&custodian_credentials, "node-a", 1);
  set_credential(&custodian_credentials, "node-b", 2);
  set_credential(&custodian_credentials, "node-c", 3);
  set_credential(&custodian_credentials, "node-d", 4);
  set_credential(&custodian_credentials, "node-e", 5);
  set_credential(&custodian_credentials, "node-f", 6);
  ParticipantCredentialRegistry bundle{};
  require(prepare_site_credential_bundle(
              custodian, configuration, &directory, "node-c",
              custodian_credentials, "node-a", &bundle) ==
              CredentialProvisioningResult::OK,
          "the custodian failed to prepare a bounded recovery bundle");
  auto incomplete_custodian = custodian_credentials;
  require(incomplete_custodian.erase("node-f") ==
                  ParticipantCredentialResult::OK &&
              prepare_site_credential_bundle(
                  custodian, configuration, &directory, "node-c",
                  incomplete_custodian, "node-a", &bundle) ==
                  CredentialProvisioningResult::SOURCE_CREDENTIAL_MISSING,
          "the definitive bundle builder accepted an incomplete custodian");
  require(prepare_site_credential_bundle(
              custodian, configuration, &directory, "node-c",
              custodian_credentials, "node-a", &bundle) ==
              CredentialProvisioningResult::OK,
          "the complete custodian did not recover after a rejected attempt");

  const auto requester = make_identity("node-a");
  ParticipantCredentialRegistry requester_credentials{};
  set_credential(&requester_credentials, "node-owner", 1);
  const auto *request_key = requester_credentials.find("node-owner");
  require(request_key != nullptr &&
              credential_bundle_binds_requester(
                  bundle, configuration.configuration_owner_node_id,
                  *request_key),
          "the custodian bundle is not bound to the recovering requester");

  ParticipantCredentialRegistry scratch{};
  bool changed = false;
  require(merge_site_credential_bundle(
              requester, configuration, &directory, "node-c", bundle,
              &requester_credentials, &scratch, &changed) ==
                  CredentialProvisioningResult::OK &&
              changed && site_credentials_complete_for_node(
                             requester, configuration, &directory, "node-c",
                             requester_credentials),
          "the seven-member requester did not recover its canonical mesh");
  const auto *sender = select_site_sender_credential(
      requester, &configuration, requester_credentials, "node-f");
  const auto *resolved =
      sender != nullptr
          ? custodian_credentials.resolve(sender->participant_token,
                                          sender->key_version)
          : nullptr;
  require(sender != nullptr && resolved != nullptr &&
              std::strcmp(resolved->participant_id, requester.node_id) == 0,
          "the recovered sender identity does not resolve at the custodian");
}

}  // namespace

void run_credential_provisioning_tests() {
  seven_member_group_receives_bounded_mesh();
  missing_owner_record_fails_closed();
  quarantined_pair_uses_only_its_direct_owner_bootstrap();
  staged_join_expands_the_credential_mesh_from_two_to_seven_members();
  unrelated_bundle_record_is_rejected();
  successful_merge_prunes_credentials_outside_current_topology();
  leader_candidates_form_a_low_frequency_site_mesh();
  sender_identity_uses_the_directional_owner_key();
  owner_handover_rebinds_each_directional_pair();
  seven_member_owner_handover_rebinds_and_persists_every_pair();
  owner_handover_probe_finds_the_requester_key();
  owner_handover_bootstrap_reaches_every_requester_from_two_to_seven();
  stale_owner_bundle_cannot_complete_handover();
  roster_refresh_does_not_impersonate_an_authority_change();
  credential_refresh_waits_for_the_owner_to_converge();
  authority_handover_bootstrap_is_addressed_and_fail_closed();
  boot_reconciliation_preserves_indirect_canonical_credentials();
  missing_owner_key_selects_a_converged_canonical_custodian();
  failed_credential_sources_are_tried_once_per_bounded_cycle();
  source_selection_rejects_satellites_and_prefers_complete_nodes();
  seven_member_mesh_recovers_through_a_trusted_custodian();
}
