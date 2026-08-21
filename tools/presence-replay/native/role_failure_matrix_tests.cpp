#include "fusion_automation.h"
#include "group_coordinator.h"
#include "local_observation.h"
#include "observation_transport.h"
#include "site_configuration.h"
#include "site_coordinator.h"
#include "site_directory.h"
#include "site_replication.h"
#include "site_space_summary.h"
#include "spatial_production_fusion.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace esphome::radar_api_server;

namespace {

constexpr const char *SITE_ID = "site-failure-matrix";
constexpr const char *FLOORPLAN_ID = "floor-main";
constexpr const char *SPACE_ID = "room-main";
constexpr const char *GROUP_ID = "group-main";
constexpr uint32_t START_MS = 1000;
constexpr uint32_t SNAPSHOT_OUTAGE_MS = 3000;
constexpr uint32_t COORDINATOR_OUTAGE_MS = 9000;
constexpr uint32_t OWNER_OUTAGE_MS =
    SiteCoordinator::DEFAULT_LEASE_MS +
    AutomaticConfigurationHandoverTracker::STABILITY_MS;

enum class TestPacketChannel : uint8_t {
  SITE_HEARTBEAT = 0,
  GROUP_HEARTBEAT,
  OBSERVATION,
  FUSION_SNAPSHOT,
  SITE_SUMMARY,
  SITE_DIRECTORY,
  COUNT,
};

enum class ParticipantTopology : uint8_t {
  FULL_MESH = 0,
  OWNER_HUB,
};

template <std::size_t N> class VirtualPacketNetwork {
 public:
  void block(TestPacketChannel channel, std::size_t sender,
             std::size_t receiver, bool blocked = true) {
    this->blocked_[slot_(channel)][sender][receiver] = blocked;
  }

  void set_drop_every(TestPacketChannel channel, uint16_t every) {
    this->drop_every_[slot_(channel)] = every;
    this->keep_every_[slot_(channel)] = 0;
  }

  void set_keep_every(TestPacketChannel channel, uint16_t every) {
    this->keep_every_[slot_(channel)] = every;
    this->drop_every_[slot_(channel)] = 0;
  }

  void clear_faults() {
    this->blocked_ = {};
    this->drop_every_ = {};
    this->keep_every_ = {};
  }

  bool deliver(TestPacketChannel channel, std::size_t sender,
               std::size_t receiver) {
    if (sender == receiver)
      return true;
    const std::size_t channel_slot = slot_(channel);
    uint32_t &attempt = this->attempts_[channel_slot][sender][receiver];
    ++attempt;
    const uint16_t drop_every = this->drop_every_[channel_slot];
    const uint16_t keep_every = this->keep_every_[channel_slot];
    const bool dropped = this->blocked_[channel_slot][sender][receiver] ||
        (drop_every != 0 && attempt % drop_every == 0) ||
        (keep_every != 0 && attempt % keep_every != 0);
    if (dropped)
      ++this->dropped_[channel_slot][sender][receiver];
    return !dropped;
  }

  uint32_t dropped(TestPacketChannel channel) const {
    uint32_t total = 0;
    const auto &channel_drops = this->dropped_[slot_(channel)];
    for (const auto &sender : channel_drops) {
      for (const uint32_t count : sender)
        total += count;
    }
    return total;
  }

 private:
  static constexpr std::size_t CHANNEL_COUNT =
      static_cast<std::size_t>(TestPacketChannel::COUNT);
  using LinkMatrix = std::array<std::array<bool, N>, N>;
  using CounterMatrix = std::array<std::array<uint32_t, N>, N>;

  std::array<LinkMatrix, CHANNEL_COUNT> blocked_{};
  std::array<CounterMatrix, CHANNEL_COUNT> attempts_{};
  std::array<CounterMatrix, CHANNEL_COUNT> dropped_{};
  std::array<uint16_t, CHANNEL_COUNT> drop_every_{};
  std::array<uint16_t, CHANNEL_COUNT> keep_every_{};

  static constexpr std::size_t slot_(TestPacketChannel channel) {
    return static_cast<std::size_t>(channel);
  }
};

char test_context[160] = "role failure matrix";

void set_context(const char *topology, const char *node_id,
                 uint32_t outage_ms) {
  std::snprintf(test_context, sizeof(test_context),
                "%s node=%s outage=%ums", topology, node_id,
                static_cast<unsigned>(outage_ms));
}

void require(bool condition, const char *message) {
  if (condition)
    return;
  std::fprintf(stderr, "role_failure_matrix_tests: %s: %s\n",
               test_context, message);
  std::exit(1);
}

void copy_text(char *target, std::size_t capacity, const char *source) {
  std::strncpy(target, source, capacity - 1);
  target[capacity - 1] = '\0';
}

SpatialTransform2D valid_transform() {
  SpatialTransform2D value{};
  value.sample_count = 1;
  value.valid = true;
  return value;
}

ParticipantReadinessConfig readiness_config() {
  ParticipantReadinessConfig value{};
  value.capability_flags = PARTICIPANT_CAP_POINT_TRACKS |
                           PARTICIPANT_CAP_VELOCITY |
                           PARTICIPANT_CAP_RANGE |
                           PARTICIPANT_CAP_PRESENCE |
                           PARTICIPANT_CAP_MOTION;
  value.max_targets = 3;
  copy_text(value.floorplan_id, sizeof(value.floorplan_id), FLOORPLAN_ID);
  copy_text(value.space_id, sizeof(value.space_id), SPACE_ID);
  copy_text(value.group_id, sizeof(value.group_id), GROUP_ID);
  value.geometry_version = 1;
  value.local_pose_version = 1;
  value.pose_version = 1;
  value.calibration_version = 1;
  value.transform = valid_transform();
  return value;
}

void add_direct_runtime_peer(ParticipantRegistry *participants,
                             ParticipantReadinessRegistry *readiness,
                             const char *participant_id) {
  require(participants->discover(participant_id,
                                 ParticipantType::NAMO_NODE) ==
              ParticipantRegistryResult::OK &&
              participants->classify(
                  participant_id, ParticipantTrust::OFFICIAL_VERIFIED) ==
                  ParticipantRegistryResult::OK &&
              participants->activate(participant_id) ==
                  ParticipantRegistryResult::OK &&
              readiness->configure(participant_id, readiness_config()) ==
                  ParticipantReadinessResult::OK,
          "direct runtime peer fixture failed");
}

struct TestNode {
  PlatformIdentity identity{};
  ParticipantRegistry participants{};
  ParticipantReadinessRegistry readiness{};
  ParticipantCredentialRegistry credentials{};
  SiteConfigurationSnapshot configuration{};
  SiteCoordinator site{};
  SiteDirectorySnapshot directory{};
  GroupCoordinator group{};
  FusionAutomationState automation{};
  FusionSnapshotCache fusion_snapshots{};
  SiteSpaceSummaryCache site_summaries{};
  AutomaticConfigurationHandoverTracker handover{};
  bool powered{true};
  uint32_t boot_generation{1};
  uint32_t site_sequence{0};
  uint32_t group_sequence{0};
  uint32_t fusion_source_sequence{0};
  uint32_t fusion_transport_sequence{0};
  uint32_t summary_sequence{0};
  char boot_id[32]{};
};

template <std::size_t N> class FailureCluster {
 public:
  FailureCluster(const std::array<const char *, N> &ids,
                 std::size_t owner_index,
                 ParticipantTopology participant_topology =
                     ParticipantTopology::FULL_MESH,
                 bool canonical_group_runtime = true)
      : canonical_group_runtime_(canonical_group_runtime) {
    require(N >= 2 && N <= MAX_AUTOMATIC_SPATIAL_GROUP_PARTICIPANTS,
            "fixture must fit one bounded Fusion Group");
    require(owner_index < N, "owner index is out of range");
    for (std::size_t index = 0; index < N; ++index) {
      auto &node = this->nodes_[index];
      copy_text(node.identity.site_id, sizeof(node.identity.site_id), SITE_ID);
      copy_text(node.identity.node_id, sizeof(node.identity.node_id), ids[index]);
      this->refresh_boot_id_(index);
      for (std::size_t peer = 0; peer < N; ++peer) {
        if (peer == index)
          continue;
        if (participant_topology == ParticipantTopology::OWNER_HUB &&
            index != owner_index && peer != owner_index)
          continue;
        require(node.participants.discover(ids[peer],
                                           ParticipantType::NAMO_NODE) ==
                    ParticipantRegistryResult::OK,
                "participant discovery failed");
        require(node.participants.classify(
                    ids[peer], ParticipantTrust::OFFICIAL_VERIFIED) ==
                    ParticipantRegistryResult::OK,
                "participant classification failed");
        require(node.participants.activate(ids[peer]) ==
                    ParticipantRegistryResult::OK,
                "participant activation failed");
        require(node.readiness.configure(ids[peer], readiness_config()) ==
                    ParticipantReadinessResult::OK,
                "participant readiness failed");
      }
    }

    std::array<uint8_t, 32> credential_key{};
    credential_key.fill(0x5A);
    for (std::size_t index = 0; index < N; ++index) {
      auto &node = this->nodes_[index];
      for (std::size_t peer = 0; peer < N; ++peer) {
        if (peer == index)
          continue;
        require(node.credentials.set(
                    ids[peer], static_cast<uint64_t>(peer + 1U), 1,
                    credential_key.data(), true) ==
                    ParticipantCredentialResult::OK,
                "canonical credential fixture failed");
      }
    }

    SiteConfigurationSnapshot base{};
    require(initialize_site_configuration(this->nodes_[owner_index].identity,
                                          false, &base) ==
                SiteConfigurationResult::OK,
            "configuration bootstrap failed");
    SiteConfigurationSnapshot canonical{};
    require(commit_local_site_roster(
                this->nodes_[owner_index].identity,
                this->nodes_[owner_index].participants, base, &canonical) ==
                SiteConfigurationResult::OK,
            "canonical roster commit failed");
    canonical.floorplan.committed = true;
    copy_text(canonical.floorplan.floorplan_id,
              sizeof(canonical.floorplan.floorplan_id), FLOORPLAN_ID);
    canonical.floorplan.geometry_version = 1;
    canonical.floorplan.config_hash = 11;
    canonical.floorplan.image_hash = 22;
    for (auto &member : canonical.members) {
      if (!member.occupied)
        continue;
      member.spatial_profile.committed = true;
      copy_text(member.spatial_profile.space_id,
                sizeof(member.spatial_profile.space_id), SPACE_ID);
      copy_text(member.spatial_profile.group_id,
                sizeof(member.spatial_profile.group_id), GROUP_ID);
      member.spatial_profile.pose_version = 1;
      member.spatial_profile.calibration_version = 1;
      member.spatial_profile.transform = valid_transform();
    }
    this->canonical_ = canonical;
    require(migrate_site_configuration_to_directory(
                canonical, &this->canonical_directory_) ==
                SiteDirectoryResult::OK,
            "Site Directory bootstrap failed");
    for (auto &node : this->nodes_) {
      node.configuration = canonical;
      node.directory = this->canonical_directory_;
    }

    this->settle(START_MS);
    SiteDirectorySnapshot promoted{};
    bool directory_update_required = false;
    require(prepare_legacy_single_floorplan_projection(
                canonical, this->nodes_[0].group,
                this->canonical_directory_, &promoted,
                &directory_update_required) == SiteDirectoryResult::OK &&
                directory_update_required,
            "Site Directory promotion failed");
    this->canonical_directory_ = promoted;
    for (auto &node : this->nodes_)
      node.directory = promoted;
    this->settle(START_MS);
    this->publish_or_expire_fusion(START_MS, 0);
    this->assert_configuration_consensus();
    this->assert_role_consensus(N);
    this->assert_automation(true);
  }

  const char *node_id(std::size_t index) const {
    return this->nodes_[index].identity.node_id;
  }

  bool powered(std::size_t index) const { return this->nodes_[index].powered; }

  void set_group_credential_readiness(std::size_t index, bool ready) {
    require(index < N, "credential readiness index is out of range");
    this->group_credentials_ready_[index] = ready;
  }

  uint32_t group_term() const {
    const auto *node = this->first_powered_();
    require(node != nullptr, "no powered Node can expose the Group term");
    const auto *group = this->local_group_(*node);
    require(group != nullptr, "powered Node has no canonical Group term");
    return group->term;
  }

  uint32_t site_term() const {
    const auto *node = this->first_powered_();
    require(node != nullptr, "no powered Node can expose the Site term");
    return node->site.state().term;
  }

  void converge_leader_eligibility(std::size_t target_index, bool eligible,
                                   uint32_t now_ms) {
    this->begin_leader_eligibility_rollout(target_index, eligible);
    for (std::size_t index = 0; index < N; ++index) {
      if (this->nodes_[index].powered)
        this->nodes_[index].configuration = this->canonical_;
    }
    for (std::size_t index = 0; index < N; ++index) {
      if (this->nodes_[index].powered)
        this->nodes_[index].directory = this->canonical_directory_;
    }
    this->settle(now_ms);
  }

  void begin_leader_eligibility_rollout(std::size_t target_index,
                                        bool eligible) {
    require(target_index < N, "leader eligibility target is out of range");
    const std::size_t owner_index =
        this->index_of_(this->canonical_.configuration_owner_node_id);
    SiteConfigurationSnapshot updated{};
    require(set_local_site_member_leader_eligible(
                this->nodes_[owner_index].identity, this->canonical_,
                this->nodes_[target_index].identity.node_id, eligible,
                &updated) == SiteConfigurationResult::OK,
            "leader eligibility rollout build failed");
    require(updated.generation != UINT32_MAX,
            "leader eligibility rollout generation overflowed");
    ++updated.generation;

    const char *site_leader_id = this->site_leader();
    require(site_leader_id != nullptr && site_leader_id[0] != '\0',
            "leader eligibility rollout has no Site Leader");
    const auto &site_leader =
        this->nodes_[this->index_of_(site_leader_id)];
    SiteDirectorySnapshot directory{};
    bool update_required = false;
    require(prepare_legacy_single_floorplan_projection(
                updated, site_leader.group, this->canonical_directory_,
                &directory, &update_required) == SiteDirectoryResult::OK &&
                update_required,
            "leader eligibility rollout Directory build failed");

    this->canonical_ = updated;
    this->canonical_directory_ = directory;
    this->nodes_[owner_index].configuration = updated;
  }

  void replicate_canonical_configuration(std::size_t index) {
    require(index < N && this->nodes_[index].powered,
            "configuration rollout target is unavailable");
    this->nodes_[index].configuration = this->canonical_;
  }

  void replicate_canonical_directory(std::size_t index) {
    require(index < N && this->nodes_[index].powered,
            "Directory rollout target is unavailable");
    this->nodes_[index].directory = this->canonical_directory_;
  }

  void assert_owner_hub_runtime_contract(std::size_t owner_index) const {
    require(owner_index < N, "owner-hub owner index is out of range");
    const char *leader_id = this->group_leader();
    const std::size_t leader_index = this->index_of_(leader_id);

    for (std::size_t receiver = 0; receiver < N; ++receiver) {
      const auto &node = this->nodes_[receiver];
      const auto *group = this->local_group_(node);
      require(group != nullptr && group->participants_expected == N,
              "owner-hub Node lost canonical Group membership");

      for (std::size_t peer = 0; peer < N; ++peer) {
        if (receiver == peer)
          continue;
        const bool direct_expected =
            receiver == owner_index || peer == owner_index;
        require((node.participants.find(this->nodes_[peer].identity.node_id) !=
                 nullptr) == direct_expected,
                "owner-hub fixture contains an unexpected direct peer");

        ParticipantRecord projected_participant{};
        ParticipantReadinessRecord projected_readiness{};
        require(build_canonical_group_runtime_records(
                    &node.configuration, node.identity.node_id,
                    this->nodes_[peer].identity.node_id,
                    &projected_participant, &projected_readiness),
                "owner-hub canonical peer projection failed");
      }
    }

    const auto &leader = this->nodes_[leader_index];
    const auto *leader_group = this->local_group_(leader);
    require(leader_group != nullptr && leader_group->local_leader,
            "owner-hub Group Leader is not authoritative");
    for (std::size_t sender = 0; sender < N; ++sender) {
      if (sender == leader_index)
        continue;

      const auto &source = this->nodes_[sender];
      const auto *source_group = this->local_group_(source);
      ParticipantRecord projected_leader{};
      ParticipantReadinessRecord projected_leader_readiness{};
      require(build_canonical_group_runtime_records(
                  &source.configuration, source.identity.node_id, leader_id,
                  &projected_leader, &projected_leader_readiness),
              "owner-hub observation target projection failed");
      ParticipantCredentialRecord credential{};
      credential.occupied = true;
      credential.enabled = true;
      ParticipantEndpoint endpoint{};
      endpoint.occupied = true;
      endpoint.authenticated = true;
      endpoint.ipv4 = 1;
      endpoint.authenticated_at_ms = START_MS;
      const auto send_result = evaluate_observation_send(
          source.identity.node_id, source_group, false, &projected_leader,
          &projected_leader_readiness, &credential, &endpoint, 1, START_MS);
      if (send_result != ObservationSendResult::OK) {
        std::fprintf(
            stderr,
            "role_failure_matrix_tests: owner-hub send diagnostic source=%s "
            "leader=%s result=%u groupLeader=%s term=%u quorum=%u "
            "canonical=%u credentials=%u\n",
            source.identity.node_id, leader_id,
            static_cast<unsigned>(send_result),
            source_group != nullptr ? source_group->leader_node_id : "",
            static_cast<unsigned>(source_group != nullptr ? source_group->term
                                                          : 0),
            static_cast<unsigned>(source_group != nullptr &&
                                  source_group->quorum_available),
            static_cast<unsigned>(source_group != nullptr &&
                                  source_group->canonical_assignment),
            static_cast<unsigned>(source_group != nullptr &&
                                  source_group->credentials_complete));
      }
      require(send_result == ObservationSendResult::OK,
              "owner-hub member could not send to the indirect Group Leader");

      ParticipantRecord projected_sender{};
      ParticipantReadinessRecord projected_sender_readiness{};
      require(build_canonical_group_runtime_records(
                  &leader.configuration, leader.identity.node_id,
                  source.identity.node_id, &projected_sender,
                  &projected_sender_readiness),
              "owner-hub observation source projection failed");
      ParticipantObservationMetadata observation{};
      observation.participant_id = source.identity.node_id;
      observation.participant_type = ParticipantType::NAMO_NODE;
      observation.site_id = SITE_ID;
      observation.group_id = GROUP_ID;
      observation.boot_id = source.boot_id;
      observation.sequence = 1;
      observation.ttl_ms = LocalObservation::DEFAULT_TTL_MS;
      observation.pose_version = 1;
      observation.calibration_version = 1;
      observation.track_count = 1;
      observation.presence = true;
      observation.motion = true;
      observation.has_velocity = true;
      observation.has_position_uncertainty = true;
      observation.has_range = true;
      ObservationAdmission admission{};
      require(admission.admit(&projected_sender, &projected_sender_readiness,
                              SITE_ID, observation, GROUP_ID) ==
                  ObservationAdmissionResult::OK,
              "owner-hub Group Leader rejected an indirect observation");
    }

    FusionSnapshot snapshot{};
    snapshot.occupied = true;
    copy_text(snapshot.site_id, sizeof(snapshot.site_id), SITE_ID);
    copy_text(snapshot.leader_node_id, sizeof(snapshot.leader_node_id),
              leader_id);
    copy_text(snapshot.group_id, sizeof(snapshot.group_id), GROUP_ID);
    copy_text(snapshot.boot_id, sizeof(snapshot.boot_id), leader.boot_id);
    snapshot.sequence = 1;
    snapshot.group_term = leader_group->term;
    snapshot.source_sequence = 1;
    snapshot.ttl_ms = FusionSnapshot::DEFAULT_TTL_MS;
    for (std::size_t follower = 0; follower < N; ++follower) {
      if (follower == leader_index)
        continue;
      const auto &node = this->nodes_[follower];
      const auto *group = this->local_group_(node);
      ParticipantRecord projected_leader{};
      ParticipantReadinessRecord projected_leader_readiness{};
      require(build_canonical_group_runtime_records(
                  &node.configuration, node.identity.node_id, leader_id,
                  &projected_leader, &projected_leader_readiness),
              "owner-hub fusion source projection failed");
      require(evaluate_fusion_snapshot_admission(
                  snapshot, leader_id, SITE_ID, &projected_leader,
                  &projected_leader_readiness, group) ==
                  FusionSnapshotAdmissionResult::OK,
              "owner-hub follower rejected the indirect fusion snapshot");
    }
  }

  std::size_t powered_count() const {
    std::size_t count = 0;
    for (const auto &node : this->nodes_)
      count += node.powered ? 1 : 0;
    return count;
  }

  const char *configuration_owner() const {
    return this->canonical_.configuration_owner_node_id;
  }

  const char *configuration_successor() const {
    const SiteConfigurationMember *selected = nullptr;
    uint8_t selected_penalty = 0;
    const char *site = this->site_leader();
    const char *group = this->group_leader();
    for (const auto &member : this->canonical_.members) {
      if (!member.occupied ||
          member.membership != ParticipantMembership::ACTIVE ||
          member.type != ParticipantType::NAMO_NODE || !member.leader_eligible ||
          std::strcmp(member.participant_id,
                      this->canonical_.configuration_owner_node_id) == 0 ||
          !this->participant_powered(member.participant_id))
        continue;
      uint8_t penalty =
          std::strcmp(member.participant_id, site) == 0 ? 2U : 0U;
      penalty += std::strcmp(member.participant_id, group) == 0 ? 1U : 0U;
      if (selected == nullptr ||
          leader_candidate_preferred(member.participant_id, penalty,
                                     selected->participant_id,
                                     selected_penalty)) {
        selected = &member;
        selected_penalty = penalty;
      }
    }
    return selected != nullptr ? selected->participant_id : nullptr;
  }

  bool participant_powered(const char *participant_id) const {
    if (participant_id == nullptr || participant_id[0] == '\0')
      return false;
    for (const auto &node : this->nodes_) {
      if (std::strcmp(node.identity.node_id, participant_id) == 0)
        return node.powered;
    }
    return false;
  }

  void block_packet(TestPacketChannel channel, std::size_t sender,
                    std::size_t receiver, bool blocked = true) {
    require(sender < N && receiver < N, "packet fault endpoint is invalid");
    this->network_.block(channel, sender, receiver, blocked);
  }

  void block_partition(TestPacketChannel channel, uint32_t partition_mask) {
    for (std::size_t sender = 0; sender < N; ++sender) {
      for (std::size_t receiver = 0; receiver < N; ++receiver) {
        const bool sender_side = (partition_mask & (1U << sender)) != 0;
        const bool receiver_side = (partition_mask & (1U << receiver)) != 0;
        if (sender_side != receiver_side)
          this->network_.block(channel, sender, receiver);
      }
    }
  }

  void block_direction(TestPacketChannel channel, uint32_t sender_mask,
                       uint32_t receiver_mask) {
    for (std::size_t sender = 0; sender < N; ++sender) {
      if ((sender_mask & (1U << sender)) == 0)
        continue;
      for (std::size_t receiver = 0; receiver < N; ++receiver) {
        if ((receiver_mask & (1U << receiver)) != 0)
          this->network_.block(channel, sender, receiver);
      }
    }
  }

  void set_periodic_drop(TestPacketChannel channel, uint16_t every) {
    this->network_.set_drop_every(channel, every);
  }

  void set_periodic_keep(TestPacketChannel channel, uint16_t every) {
    this->network_.set_keep_every(channel, every);
  }

  void clear_packet_faults() { this->network_.clear_faults(); }

  uint32_t dropped_packets(TestPacketChannel channel) const {
    return this->network_.dropped(channel);
  }

  uint32_t directory_revision(std::size_t index) const {
    return this->nodes_[index].directory.topology_revision;
  }

  uint32_t directory_digest(std::size_t index) const {
    return site_directory_payload_hash(this->nodes_[index].directory);
  }

  void advance_directory(std::size_t index, uint32_t marker) {
    require(index < N && this->nodes_[index].powered,
            "Directory update target is unavailable");
    auto &directory = this->nodes_[index].directory;
    require(directory.topology_revision != UINT32_MAX &&
                directory.floorplans[0].occupied,
            "Directory update fixture is invalid");
    ++directory.topology_revision;
    directory.floorplans[0].config_hash ^= marker == 0 ? 1U : marker;
    this->canonical_directory_ = directory;
  }

  void advance_directory_partition(uint32_t member_mask, uint32_t marker) {
    std::size_t source = N;
    for (std::size_t index = 0; index < N; ++index) {
      if ((member_mask & (1U << index)) != 0 && this->nodes_[index].powered) {
        source = index;
        break;
      }
    }
    require(source < N, "Directory partition has no powered source");
    this->advance_directory(source, marker);
    for (std::size_t index = 0; index < N; ++index) {
      if (index != source && (member_mask & (1U << index)) != 0 &&
          this->nodes_[index].powered)
        this->nodes_[index].directory = this->nodes_[source].directory;
    }
  }

  void assert_directory_consensus() const {
    const TestNode *reference = this->first_powered_();
    require(reference != nullptr, "Directory consensus has no powered Node");
    for (const auto &node : this->nodes_) {
      if (!node.powered)
        continue;
      require(node.directory.topology_revision ==
                      reference->directory.topology_revision &&
                  site_directory_payload_hash(node.directory) ==
                      site_directory_payload_hash(reference->directory),
              "powered Nodes disagree on the persisted Site Directory");
    }
  }

  void commit_runtime_group_assignment() {
    if (!this->canonical_group_runtime_)
      return;
    const char *site_leader_id = this->site_leader();
    if (site_leader_id == nullptr || site_leader_id[0] == '\0')
      return;
    const auto &site_leader = this->nodes_[this->index_of_(site_leader_id)];
    SiteDirectorySnapshot candidate{};
    bool update_required = false;
    require(prepare_legacy_single_floorplan_projection(
                this->canonical_, site_leader.group,
                this->canonical_directory_, &candidate, &update_required) ==
                SiteDirectoryResult::OK,
            "runtime Group assignment could not be projected to the Site "
            "Directory");
    if (!update_required)
      return;
    this->canonical_directory_ = candidate;
    for (auto &node : this->nodes_) {
      if (node.powered)
        node.directory = candidate;
    }
  }

  const char *site_leader() const {
    const auto *node = this->first_powered_();
    require(node != nullptr, "no powered node can expose Site authority");
    return node->site.state().leader_node_id;
  }

  const char *group_leader() const {
    const auto *node = this->first_powered_();
    require(node != nullptr, "no powered node can expose Group authority");
    const auto *group = this->local_group_(*node);
    require(group != nullptr, "powered node has no canonical Group");
    return group->leader_node_id;
  }

  void power_off(std::size_t index) {
    require(index < N && this->nodes_[index].powered,
            "power-off target is unavailable");
    this->nodes_[index].powered = false;
  }

  void power_on(std::size_t index, bool restore_persisted_directory = false) {
    require(index < N && !this->nodes_[index].powered,
            "power-on target is already online");
    auto &node = this->nodes_[index];
    node.powered = true;
    ++node.boot_generation;
    node.site_sequence = 0;
    node.group_sequence = 0;
    node.fusion_source_sequence = 0;
    node.fusion_transport_sequence = 0;
    node.summary_sequence = 0;
    node.site.clear();
    node.group.clear();
    node.automation = FusionAutomationState{};
    node.fusion_snapshots.clear();
    node.site_summaries.clear();
    node.handover.reset();
    this->refresh_boot_id_(index);
    this->replicate_configuration_to_(index);
    if (!restore_persisted_directory)
      node.directory = this->canonical_directory_;
  }

  void settle(uint32_t now_ms) {
    for (uint8_t round = 0; round < 6; ++round) {
      this->tick(now_ms);
    }
  }

  void tick(uint32_t now_ms) {
    this->update_site_(now_ms);
    this->broadcast_site_(now_ms);
    this->update_site_(now_ms);
    this->update_groups_(now_ms);
    this->broadcast_groups_(now_ms);
    this->update_groups_(now_ms);
    this->replicate_directories_();
  }

  void perform_owner_handover(uint32_t eligible_at_ms,
                              uint32_t ready_at_ms) {
    const char *successor = this->configuration_successor();
    require(successor != nullptr, "owner outage has no available successor");
    const std::size_t successor_index = this->index_of_(successor);
    require(this->nodes_[successor_index].powered,
            "designated owner successor is offline");
    for (std::size_t index = 0; index < N; ++index) {
      const auto &node = this->nodes_[index];
      if (!node.powered)
        continue;
      const auto condition = evaluate_automatic_configuration_handover(
          node.identity, node.configuration, false,
          node.site.state().quorum_available, true, true, successor);
      if (index == successor_index)
        require(condition == AutomaticConfigurationHandoverCondition::ELIGIBLE,
                "designated successor is not eligible for owner handover");
      else
        require(condition ==
                    AutomaticConfigurationHandoverCondition::NOT_DESIGNATED_SUCCESSOR,
                "a non-successor attempted owner handover");
    }

    auto &tracker = this->nodes_[successor_index].handover;
    const uint64_t handover_fingerprint =
        automatic_configuration_handover_fingerprint(
            this->canonical_, successor);
    require(!tracker.update(AutomaticConfigurationHandoverCondition::ELIGIBLE,
                            handover_fingerprint, eligible_at_ms),
            "owner handover skipped its stability guard");
    require(!tracker.update(AutomaticConfigurationHandoverCondition::ELIGIBLE,
                            handover_fingerprint, ready_at_ms - 1),
            "owner handover completed before the stability deadline");
    for (uint32_t heartbeat_at =
             eligible_at_ms + SiteCoordinator::DEFAULT_LEASE_MS / 2U;
         heartbeat_at < ready_at_ms;
         heartbeat_at += SiteCoordinator::DEFAULT_LEASE_MS / 2U) {
      this->settle(heartbeat_at);
    }
    require(tracker.update(AutomaticConfigurationHandoverCondition::ELIGIBLE,
                           handover_fingerprint, ready_at_ms),
            "owner handover did not complete after the stability deadline");

    const auto *role_view_before = this->first_powered_();
    const auto *group_before = this->local_group_(*role_view_before);
    char site_leader_before[ParticipantRecord::MAX_ID_LENGTH + 1]{};
    char group_leader_before[ParticipantRecord::MAX_ID_LENGTH + 1]{};
    copy_text(site_leader_before, sizeof(site_leader_before),
              role_view_before->site.state().leader_node_id);
    copy_text(group_leader_before, sizeof(group_leader_before),
              group_before->leader_node_id);
    const uint32_t site_term_before = role_view_before->site.state().term;
    const uint32_t group_term_before = group_before->term;
    const bool owner_handover_can_preserve_roles =
        std::strcmp(successor, site_leader_before) != 0;
    const uint32_t roster_revision_before = this->canonical_.roster_revision;
    const uint64_t topology_digest_before =
        site_topology_digest(this->canonical_);

    SiteConfigurationSnapshot updated{};
    require(prepare_automatic_configuration_handover(
                this->nodes_[successor_index].identity, this->canonical_,
                &updated, successor) == SiteConfigurationResult::OK,
            "owner handover configuration build failed");
    require(updated.authority_epoch == this->canonical_.authority_epoch + 1,
            "owner handover did not advance the authority epoch");
    require(updated.roster_revision == roster_revision_before &&
                site_topology_digest(updated) == topology_digest_before,
            "owner-only handover changed the canonical Site topology");
    this->canonical_ = updated;
    this->nodes_[successor_index].configuration = updated;
    this->settle(ready_at_ms);
    uint32_t propagation_step = 1;
    for (std::size_t index = 0; index < N; ++index) {
      auto &node = this->nodes_[index];
      if (!node.powered || index == successor_index)
        continue;
      node.configuration = updated;
      this->settle(ready_at_ms + propagation_step++);
    }
    tracker.complete(true, ready_at_ms);
    this->settle(ready_at_ms);
    this->assert_configuration_consensus();
    const auto *role_view_after = this->first_powered_();
    const auto *group_after = this->local_group_(*role_view_after);
    if (owner_handover_can_preserve_roles) {
      if (std::strcmp(role_view_after->site.state().leader_node_id,
                      site_leader_before) != 0 ||
          role_view_after->site.state().term != site_term_before ||
          std::strcmp(group_after->leader_node_id,
                      group_leader_before) != 0 ||
          group_after->term != group_term_before) {
        std::fprintf(
            stderr,
            "role_failure_matrix_tests: owner-only transition diagnostic "
            "successor=%s site=%s/%u->%s/%u group=%s/%u->%s/%u\n",
            successor, site_leader_before,
            static_cast<unsigned>(site_term_before),
            role_view_after->site.state().leader_node_id,
            static_cast<unsigned>(role_view_after->site.state().term),
            group_leader_before, static_cast<unsigned>(group_term_before),
            group_after->leader_node_id,
            static_cast<unsigned>(group_after->term));
      }
      require(std::strcmp(role_view_after->site.state().leader_node_id,
                          site_leader_before) == 0 &&
                  role_view_after->site.state().term == site_term_before,
              "owner-only handover changed an independent Site Leader term");
      require(std::strcmp(group_after->leader_node_id,
                          group_leader_before) == 0 &&
                  group_after->term == group_term_before,
              "owner-only handover changed an independent Group Leader term");
    }
  }

  bool owner_handover_is_blocked_while_successor_offline() const {
    const auto *successor = automatic_configuration_successor(this->canonical_);
    require(successor != nullptr, "blocked handover fixture has no successor");
    require(!this->nodes_[this->index_of_(successor->participant_id)].powered,
            "blocked handover successor must be offline");
    return this->owner_handover_is_blocked();
  }

  bool owner_handover_is_blocked() const {
    for (const auto &node : this->nodes_) {
      if (!node.powered)
        continue;
      const auto condition = evaluate_automatic_configuration_handover(
          node.identity, node.configuration, false,
          node.site.state().quorum_available, true, true);
      if (condition == AutomaticConfigurationHandoverCondition::ELIGIBLE)
        return false;
    }
    return true;
  }

  void publish_or_expire_fusion(uint32_t now_ms, uint32_t outage_ms) {
    (void) outage_ms;
    const char *leader_id = this->group_leader();
    if (leader_id[0] == '\0') {
      for (auto &node : this->nodes_) {
        if (!node.powered)
          continue;
        const auto *group = this->local_group_(node);
        if (group != nullptr && group->quorum_available) {
          std::fprintf(
              stderr,
              "role_failure_matrix_tests: leaderless Group diagnostic "
              "node=%s site=%s/%u group=%s/%u online=%u expected=%u "
              "endorsements=%u digest=%u previous=%u canonical=%u\n",
              node.identity.node_id, node.site.state().leader_node_id,
              static_cast<unsigned>(node.site.state().term),
              group->leader_node_id, static_cast<unsigned>(group->term),
              static_cast<unsigned>(group->participants_online),
              static_cast<unsigned>(group->participants_expected),
              static_cast<unsigned>(group->election_endorsements),
              static_cast<unsigned>(group->election_view_digest),
              static_cast<unsigned>(group->previous_election_view_digest),
              static_cast<unsigned>(group->canonical_assignment));
        }
        require(group != nullptr && !group->quorum_available,
                "a Group without a Leader must be below quorum");
        node.automation.mark_unavailable(
            SITE_ID, SPACE_ID, GROUP_ID, "", group->term, false);
      }
      this->reconcile_site_summary_caches_(now_ms);
      return;
    }
    const std::size_t leader_index = this->index_of_(leader_id);
    uint8_t valid_inputs = this->nodes_[leader_index].powered ? 1 : 0;
    if (this->nodes_[leader_index].powered) {
      for (std::size_t sender = 0; sender < N; ++sender) {
        if (sender == leader_index)
          continue;
        if (this->nodes_[sender].powered &&
            this->network_.deliver(TestPacketChannel::OBSERVATION,
                                   sender, leader_index)) {
          this->observation_expires_at_[leader_index][sender] =
              now_ms + LocalObservation::DEFAULT_TTL_MS;
        }
        if (static_cast<int32_t>(
                now_ms - this->observation_expires_at_[leader_index][sender]) <
            0)
          ++valid_inputs;
      }
    }
    const bool can_fuse = valid_inputs >= 2;
    if (!can_fuse) {
      const auto *view = this->first_powered_();
      const auto *group = this->local_group_(*view);
      for (std::size_t index = 0; index < N; ++index) {
        auto &node = this->nodes_[index];
        if (!node.powered)
          continue;
        const auto &previous = node.automation.snapshot();
        if (index == leader_index && previous.authoritative &&
            previous.updated_at_ms != 0 &&
            static_cast<uint32_t>(now_ms - previous.updated_at_ms) <
                FusionSnapshot::DEFAULT_TTL_MS)
          continue;
        FusionSnapshot cached{};
        if (index != leader_index &&
            node.fusion_snapshots.copy(now_ms, &cached) &&
            std::strcmp(cached.leader_node_id, leader_id) == 0 &&
            cached.group_term == group->term &&
            node.automation.update_remote(cached, SPACE_ID,
                                          valid_transform(), true))
          continue;
        node.automation.mark_unavailable(
            SITE_ID, SPACE_ID, GROUP_ID, leader_id, group->term,
            index == leader_index);
      }
      this->reconcile_site_summary_caches_(now_ms);
      return;
    }

    auto &leader = this->nodes_[leader_index];
    const auto *leader_group = this->local_group_(leader);
    require(leader_group != nullptr && leader_group->local_leader,
            "fusion publisher is not the local Group Leader");
    SpatialProductionFusionOutput fusion{};
    fusion.build_enabled = true;
    fusion.authoritative = true;
    fusion.state = SpatialProductionFusionState::ACTIVE;
    fusion.source_sequence = ++leader.fusion_source_sequence;
    fusion.target_count = 1;
    fusion.targets[0].valid = true;
    fusion.targets[0].fused_track_id = fusion.source_sequence;
    fusion.targets[0].state = SpatialShadowTrackState::CONFIRMED;
    fusion.targets[0].source_count = valid_inputs;
    fusion.targets[0].confidence = 90;
    fusion.targets[0].x_mm = 500.0f;
    fusion.targets[0].y_mm = 1200.0f;

    require(leader.automation.update_local(
                SITE_ID, SPACE_ID, GROUP_ID, leader_id, leader.boot_id,
                leader_group->term, fusion, now_ms),
            "local Group fusion did not enter automation state");

    FusionSnapshot snapshot{};
    snapshot.occupied = true;
    copy_text(snapshot.site_id, sizeof(snapshot.site_id), SITE_ID);
    copy_text(snapshot.leader_node_id, sizeof(snapshot.leader_node_id), leader_id);
    copy_text(snapshot.group_id, sizeof(snapshot.group_id), GROUP_ID);
    copy_text(snapshot.boot_id, sizeof(snapshot.boot_id), leader.boot_id);
    snapshot.sequence = ++leader.fusion_transport_sequence;
    snapshot.group_term = leader_group->term;
    snapshot.source_sequence = fusion.source_sequence;
    snapshot.received_at_ms = now_ms;
    snapshot.expires_at_ms = now_ms + FusionSnapshot::DEFAULT_TTL_MS;
    snapshot.ttl_ms = FusionSnapshot::DEFAULT_TTL_MS;
    snapshot.target_count = 1;
    snapshot.presence = true;
    snapshot.targets[0].valid = true;
    snapshot.targets[0].fused_track_id = fusion.targets[0].fused_track_id;
    snapshot.targets[0].state = SpatialShadowTrackState::CONFIRMED;
    snapshot.targets[0].source_count = fusion.targets[0].source_count;
    snapshot.targets[0].confidence = 90;
    snapshot.targets[0].x_mm = 500.0f;
    snapshot.targets[0].y_mm = 1200.0f;
    for (std::size_t index = 0; index < N; ++index) {
      if (!this->nodes_[index].powered || index == leader_index)
        continue;
      auto &follower = this->nodes_[index];
      const auto *follower_group = this->local_group_(follower);
      if (this->network_.deliver(TestPacketChannel::FUSION_SNAPSHOT,
                                 leader_index, index)) {
        const ParticipantRecord *participant =
            follower.participants.find(leader_id);
        const ParticipantReadinessRecord *readiness =
            follower.readiness.find(leader_id);
        ParticipantRecord projected_participant{};
        ParticipantReadinessRecord projected_readiness{};
        const auto *direct = follower.participants.find(leader_id);
        if (direct == nullptr ||
            direct->membership != ParticipantMembership::REVOKED) {
          if (build_canonical_group_runtime_records(
                  &follower.configuration, follower.identity.node_id,
                  leader_id, &projected_participant, &projected_readiness)) {
            participant = &projected_participant;
            readiness = &projected_readiness;
          }
        }
        const auto admission = evaluate_fusion_snapshot_admission(
            snapshot, leader_id, SITE_ID, participant, readiness,
            follower_group);
        if (admission != FusionSnapshotAdmissionResult::OK) {
          std::fprintf(
              stderr,
              "role_failure_matrix_tests: fusion admission diagnostic "
              "follower=%s leader=%s result=%u snapshot_term=%u "
              "group_term=%u group_leader=%s local_leader=%s\n",
              follower.identity.node_id, leader_id,
              static_cast<unsigned>(admission),
              static_cast<unsigned>(snapshot.group_term),
              static_cast<unsigned>(follower_group->term),
              follower_group->leader_node_id,
              follower_group->local_leader ? "true" : "false");
          std::fprintf(
              stderr,
              "role_failure_matrix_tests: follower authority diagnostic "
              "site_leader=%s site_term=%u site_quorum=%s site_view=%08x "
              "site_endorsements=%u site_fenced=%s group_quorum=%s "
              "group_view=%08x group_endorsements=%u group_fenced=%s\n",
              follower.site.state().leader_node_id,
              static_cast<unsigned>(follower.site.state().term),
              follower.site.state().quorum_available ? "true" : "false",
              static_cast<unsigned>(follower.site.state().election_view_digest),
              static_cast<unsigned>(follower.site.state().election_endorsements),
              follower.site.state().election_fenced ? "true" : "false",
              follower_group->quorum_available ? "true" : "false",
              static_cast<unsigned>(follower_group->election_view_digest),
              static_cast<unsigned>(follower_group->election_endorsements),
              follower_group->election_fenced ? "true" : "false");
        }
        require(admission == FusionSnapshotAdmissionResult::OK,
                "follower rejected an admitted Group fusion snapshot");
        require(follower.fusion_snapshots.store(snapshot),
                "follower failed to cache a new Group fusion snapshot");
      }
      FusionSnapshot cached{};
      const bool accepted =
          follower.fusion_snapshots.copy(now_ms, &cached) &&
          std::strcmp(cached.leader_node_id,
                      follower_group->leader_node_id) == 0 &&
          cached.group_term == follower_group->term &&
          follower.automation.update_remote(cached, SPACE_ID,
                                            valid_transform(), true);
      if (!accepted) {
        follower.automation.mark_unavailable(
            SITE_ID, SPACE_ID, GROUP_ID, follower_group->leader_node_id,
            follower_group->term, false);
      }
    }

    this->publish_site_summary_(leader_index, fusion, now_ms);
  }

  void assert_configuration_consensus() const {
    const uint64_t digest = site_configuration_digest(this->canonical_);
    for (const auto &node : this->nodes_) {
      if (!node.powered)
        continue;
      require(site_configuration_digest(node.configuration) == digest,
              "powered nodes disagree on canonical configuration");
      require(std::strcmp(node.configuration.configuration_owner_node_id,
                          this->canonical_.configuration_owner_node_id) == 0,
              "powered node exposes a stale configuration owner");
    }
  }

  void assert_role_consensus(std::size_t expected_online) const {
    const auto *view = this->first_powered_();
    require(view != nullptr, "role assertion has no powered node");
    const char *site_leader_id = view->site.state().leader_node_id;
    const auto *view_group = this->local_group_(*view);
    require(view_group != nullptr, "role assertion has no Group state");
    const char *group_leader_id = view_group->leader_node_id;
    const bool quorum_expected =
        expected_online >= strict_majority(static_cast<uint8_t>(N));
    require((site_leader_id[0] != '\0') == quorum_expected &&
                (group_leader_id[0] != '\0') == quorum_expected &&
                view->site.state().quorum_available == quorum_expected &&
                view_group->quorum_available == quorum_expected,
            "authority availability does not match canonical quorum");
    if (quorum_expected && expected_online > 1 &&
        std::strcmp(site_leader_id, group_leader_id) == 0) {
      std::fprintf(stderr,
                   "role_failure_matrix_tests: overlap diagnostic "
                   "site=%s/%u group=%s/%u expected_online=%u\n",
                   site_leader_id,
                   static_cast<unsigned>(view->site.state().term),
                   group_leader_id,
                   static_cast<unsigned>(view_group->term),
                   static_cast<unsigned>(expected_online));
    }
    if (quorum_expected && expected_online > 1)
      require(std::strcmp(site_leader_id, group_leader_id) != 0,
              "Site and Group leader overlapped while a replacement was online");
    for (const auto &node : this->nodes_) {
      if (!node.powered)
        continue;
      const auto *group = this->local_group_(node);
      require(group != nullptr, "powered node lost its Group membership");
      if (std::strcmp(node.site.state().leader_node_id, site_leader_id) != 0 ||
          std::strcmp(group->leader_node_id, group_leader_id) != 0) {
        std::fprintf(
            stderr,
            "role_failure_matrix_tests: consensus diagnostic node=%s "
            "expected=%s/%s actual=%s/%s siteQ=%u siteEndorsements=%u "
            "siteDigest=%u groupQ=%u groupEndorsements=%u groupDigest=%u\n",
            node.identity.node_id, site_leader_id, group_leader_id,
            node.site.state().leader_node_id, group->leader_node_id,
            static_cast<unsigned>(node.site.state().quorum_available),
            static_cast<unsigned>(node.site.state().election_endorsements),
            static_cast<unsigned>(node.site.state().election_view_digest),
            static_cast<unsigned>(group->quorum_available),
            static_cast<unsigned>(group->election_endorsements),
            static_cast<unsigned>(group->election_view_digest));
      }
      require(std::strcmp(node.site.state().leader_node_id, site_leader_id) == 0,
              "powered nodes disagree on Site Leader");
      require(std::strcmp(group->leader_node_id, group_leader_id) == 0,
              "powered nodes disagree on Group Leader");
      require(node.site.state().term == view->site.state().term,
              "powered nodes disagree on Site term");
      require(group->term == view_group->term,
              "powered nodes disagree on Group term");
      require(node.site.state().local_leader ==
                  (std::strcmp(node.identity.node_id, site_leader_id) == 0),
              "Site local authority flag is inconsistent");
      require(group->local_leader ==
                  (std::strcmp(node.identity.node_id, group_leader_id) == 0),
              "Group local authority flag is inconsistent");
      if (!quorum_expected) {
        require(!node.site.state().local_leader && !group->local_leader,
                "a minority partition retained local authority");
      }
      require(node.site.state().participants_online == expected_online &&
                  group->participants_online == expected_online,
              "online participant count does not match lease state");
      require(node.site.state().participants_expected == N &&
                  group->participants_expected == N,
              "configured participant count changed during outage");
    }
  }

  void assert_no_split_authority() const {
    const char *site_leader = nullptr;
    const char *group_leader = nullptr;
    std::size_t local_site_leaders = 0;
    std::size_t local_group_leaders = 0;
    for (const auto &node : this->nodes_) {
      if (!node.powered)
        continue;
      const auto &site = node.site.state();
      const auto *group = this->local_group_(node);
      require(group != nullptr, "partitioned Node lost Group state");
      if (site.leader_node_id[0] != '\0') {
        if (site_leader == nullptr)
          site_leader = site.leader_node_id;
        else
          require(std::strcmp(site_leader, site.leader_node_id) == 0,
                  "active partition produced two Site Leaders");
      }
      if (group->leader_node_id[0] != '\0') {
        if (group_leader == nullptr)
          group_leader = group->leader_node_id;
        else
          require(std::strcmp(group_leader, group->leader_node_id) == 0,
                  "active partition produced two Group Leaders");
      }
      local_site_leaders += site.local_leader ? 1U : 0U;
      local_group_leaders += group->local_leader ? 1U : 0U;
    }
    require(local_site_leaders <= 1 && local_group_leaders <= 1,
            "active partition retained duplicate local authority");
  }

  void assert_expected_roles(const char *expected_site_leader,
                             const char *expected_group_leader) const {
    if (std::strcmp(this->site_leader(), expected_site_leader) != 0 ||
        std::strcmp(this->group_leader(), expected_group_leader) != 0) {
      std::fprintf(stderr,
                   "role_failure_matrix_tests: role mismatch expected=%s/%s actual=%s/%s\n",
                   expected_site_leader, expected_group_leader,
                   this->site_leader(), this->group_leader());
      for (const auto &node : this->nodes_) {
        if (!node.powered)
          continue;
        const auto *group = this->local_group_(node);
        std::fprintf(
            stderr,
            "  node=%s site=%s/%u siteLocal=%u siteQuorum=%u siteEndorsements=%u siteDigest=%u sitePrevious=%u group=%s/%u local=%u quorum=%u endorsements=%u digest=%u previous=%u\n",
            node.identity.node_id, node.site.state().leader_node_id,
            static_cast<unsigned>(node.site.state().term),
            static_cast<unsigned>(node.site.state().local_leader),
            static_cast<unsigned>(node.site.state().quorum_available),
            static_cast<unsigned>(node.site.state().election_endorsements),
            static_cast<unsigned>(node.site.state().election_view_digest),
            static_cast<unsigned>(node.site.state().previous_election_view_digest),
            group != nullptr ? group->leader_node_id : "",
            static_cast<unsigned>(group != nullptr ? group->term : 0),
            static_cast<unsigned>(group != nullptr && group->local_leader),
            static_cast<unsigned>(group != nullptr && group->quorum_available),
            static_cast<unsigned>(group != nullptr
                                      ? group->election_endorsements
                                      : 0),
            static_cast<unsigned>(group != nullptr
                                      ? group->election_view_digest
                                      : 0),
            static_cast<unsigned>(group != nullptr
                                      ? group->previous_election_view_digest
                                      : 0));
      }
    }
    require(std::strcmp(this->site_leader(), expected_site_leader) == 0,
            "unexpected Site Leader");
    require(std::strcmp(this->group_leader(), expected_group_leader) == 0,
            "unexpected Group Leader");
  }

  void assert_roles_remain_stable(uint32_t now_ms) {
    char site_leader_id[ParticipantRecord::MAX_ID_LENGTH + 1]{};
    char group_leader_id[ParticipantRecord::MAX_ID_LENGTH + 1]{};
    copy_text(site_leader_id, sizeof(site_leader_id), this->site_leader());
    copy_text(group_leader_id, sizeof(group_leader_id), this->group_leader());
    const auto *before = this->first_powered_();
    const auto *before_group = this->local_group_(*before);
    const uint32_t site_term = before->site.state().term;
    const uint32_t group_term = before_group->term;
    this->settle(now_ms);
    const auto *after = this->first_powered_();
    const auto *after_group = this->local_group_(*after);
    require(std::strcmp(after->site.state().leader_node_id,
                        site_leader_id) == 0 &&
                after->site.state().term == site_term,
            "settled Site authority kept changing");
    require(std::strcmp(after_group->leader_node_id,
                        group_leader_id) == 0 &&
                after_group->term == group_term,
            "settled Group authority kept changing");
  }

  void assert_automation(bool expected_healthy) const {
    const char *leader_id = this->group_leader();
    for (const auto &node : this->nodes_) {
      if (!node.powered)
        continue;
      const auto &snapshot = node.automation.snapshot();
      if (expected_healthy) {
        if (!(snapshot.authoritative &&
              snapshot.health == FusionAutomationHealth::HEALTHY &&
              std::strcmp(snapshot.leader_node_id, leader_id) == 0)) {
          const auto *group = this->local_group_(node);
          std::fprintf(
              stderr,
              "role_failure_matrix_tests: automation diagnostic node=%s "
              "site_leader=%s site_quorum=%s site_view=%08x "
              "site_endorsements=%u group_leader=%s group_quorum=%s "
              "group_view=%08x group_endorsements=%u authoritative=%s "
              "health=%u snapshot_leader=%s expected_leader=%s\n",
              node.identity.node_id, node.site.state().leader_node_id,
              node.site.state().quorum_available ? "true" : "false",
              static_cast<unsigned>(node.site.state().election_view_digest),
              static_cast<unsigned>(node.site.state().election_endorsements),
              group != nullptr ? group->leader_node_id : "",
              group != nullptr && group->quorum_available ? "true" : "false",
              static_cast<unsigned>(group != nullptr
                                        ? group->election_view_digest
                                        : 0),
              static_cast<unsigned>(group != nullptr
                                        ? group->election_endorsements
                                        : 0),
              snapshot.authoritative ? "true" : "false",
              static_cast<unsigned>(snapshot.health), snapshot.leader_node_id,
              leader_id);
        }
        require(snapshot.authoritative &&
                    snapshot.health == FusionAutomationHealth::HEALTHY &&
                    std::strcmp(snapshot.leader_node_id, leader_id) == 0,
                "healthy fusion automation did not converge");
      } else {
        require(!snapshot.authoritative &&
                    snapshot.health != FusionAutomationHealth::HEALTHY,
                "unavailable fusion remained authoritative");
      }
    }
  }

  void preserve_group_handoff_continuity(
      uint32_t now_ms, const char *expected_source_leader,
      uint32_t expected_remaining_ms) {
    const char *leader_id = this->group_leader();
    require(leader_id[0] != '\0',
            "Group handoff continuity has no elected Leader");
    for (auto &node : this->nodes_) {
      if (!node.powered)
        continue;
      const auto *group = this->local_group_(node);
      require(group != nullptr &&
                  std::strcmp(group->leader_node_id, leader_id) == 0,
              "Group handoff continuity observed inconsistent authority");
      require(node.automation.preserve_continuity_or_mark_unavailable(
                  SITE_ID, SPACE_ID, GROUP_ID, leader_id, group->term,
                  now_ms, group->local_leader),
              "canonical Group preparation discarded bounded continuity");
      const auto &snapshot = node.automation.snapshot();
      require(!snapshot.authoritative && snapshot.continuity_held &&
                  snapshot.health == FusionAutomationHealth::RECOVERING &&
                  snapshot.presence && !snapshot.motion &&
                  snapshot.fusion.target_count == 1 &&
                  std::strcmp(snapshot.leader_node_id, leader_id) == 0 &&
                  snapshot.group_term == group->term &&
                  std::strcmp(snapshot.continuity_source_leader_node_id,
                              expected_source_leader) == 0 &&
                  FusionAutomationState::continuity_remaining_ms(
                      snapshot, now_ms) == expected_remaining_ms,
              "Group handoff continuity lost its read-only source or deadline");
    }
  }

  void assert_node_automation(std::size_t index,
                              bool expected_healthy) const {
    require(index < N && this->nodes_[index].powered,
            "automation assertion target is unavailable");
    const auto &snapshot = this->nodes_[index].automation.snapshot();
    require(expected_healthy
                ? snapshot.authoritative &&
                      snapshot.health == FusionAutomationHealth::HEALTHY
                : !snapshot.authoritative &&
                      snapshot.health != FusionAutomationHealth::HEALTHY,
            expected_healthy ? "Node fusion automation did not recover"
                             : "Node fusion automation ignored packet loss");
  }

  void assert_site_summary(bool expected_available) const {
    const std::size_t site_index = this->index_of_(this->site_leader());
    const auto *summary = this->nodes_[site_index].site_summaries.find(GROUP_ID);
    require((summary != nullptr) == expected_available,
            expected_available ? "Site summary did not recover"
                               : "expired Site summary remained available");
  }

  const char *expected_site_leader_from_powered() const {
    if (this->powered_count() < strict_majority(static_cast<uint8_t>(N)))
      return "";
    const char *winner = nullptr;
    uint8_t winner_penalty = 0;
    for (const auto &node : this->nodes_) {
      if (!node.powered)
        continue;
      const uint8_t penalty = site_leader_role_penalty(
          &this->canonical_, &this->canonical_directory_,
          node.identity.node_id);
      if (winner == nullptr || penalty < winner_penalty ||
          (penalty == winner_penalty &&
           std::strcmp(node.identity.node_id, winner) < 0)) {
        winner = node.identity.node_id;
        winner_penalty = penalty;
      }
    }
    return winner;
  }

  const char *expected_group_leader_from_powered(
      const char *site_leader_id) const {
    if (this->powered_count() < strict_majority(static_cast<uint8_t>(N)))
      return "";
    const auto *directory_group =
        this->canonical_directory_.find_group(GROUP_ID);
    if (this->canonical_group_runtime_ && directory_group != nullptr &&
        directory_group->assignment_revision != 0 &&
        directory_group->member_count == N &&
        this->participant_powered(directory_group->leader_node_id) &&
        group_leader_assignment_allowed(
            this->canonical_, GROUP_ID, site_leader_id,
            directory_group->leader_node_id))
      return directory_group->leader_node_id;
    const char *winner = nullptr;
    uint8_t winner_penalty = 0;
    for (const auto &node : this->nodes_) {
      if (!node.powered)
        continue;
      const bool owner =
          std::strcmp(node.identity.node_id, this->configuration_owner()) == 0;
      const bool site =
          std::strcmp(node.identity.node_id, site_leader_id) == 0;
      const uint8_t penalty = site ? (owner ? 3 : 2) : (owner ? 1 : 0);
      if (winner == nullptr || penalty < winner_penalty ||
          (penalty == winner_penalty &&
           std::strcmp(node.identity.node_id, winner) < 0)) {
        winner = node.identity.node_id;
        winner_penalty = penalty;
      }
    }
    return winner;
  }

 private:
  std::array<TestNode, N> nodes_{};
  std::array<bool, N> group_credentials_ready_ = [] {
    std::array<bool, N> ready{};
    ready.fill(true);
    return ready;
  }();
  SiteConfigurationSnapshot canonical_{};
  SiteDirectorySnapshot canonical_directory_{};
  bool canonical_group_runtime_{false};
  VirtualPacketNetwork<N> network_{};
  std::array<std::array<uint32_t, N>, N> observation_expires_at_{};

  void refresh_boot_id_(std::size_t index) {
    std::snprintf(this->nodes_[index].boot_id,
                  sizeof(this->nodes_[index].boot_id), "boot-%u-%u",
                  static_cast<unsigned>(index),
                  static_cast<unsigned>(this->nodes_[index].boot_generation));
  }

  std::size_t index_of_(const char *participant_id) const {
    for (std::size_t index = 0; index < N; ++index) {
      if (std::strcmp(this->nodes_[index].identity.node_id, participant_id) == 0)
        return index;
    }
    std::fprintf(stderr,
                 "role_failure_matrix_tests: %s: participant ID is not in "
                 "the fixture: '%s'\n",
                 test_context,
                 participant_id != nullptr ? participant_id : "<null>");
    std::exit(1);
    return 0;
  }

  TestNode *first_powered_() {
    for (auto &node : this->nodes_) {
      if (node.powered)
        return &node;
    }
    return nullptr;
  }

  const TestNode *first_powered_() const {
    for (const auto &node : this->nodes_) {
      if (node.powered)
        return &node;
    }
    return nullptr;
  }

  const GroupCoordinatorState *local_group_(const TestNode &node) const {
    return node.group.local_group(&node.configuration, node.identity.node_id);
  }

  void update_site_(uint32_t now_ms) {
    for (auto &node : this->nodes_) {
      if (node.powered) {
        node.site.set_local_directory_fence(
            node.directory.topology_revision,
            site_directory_payload_hash(node.directory));
        node.site.update(node.identity, node.participants,
                         &node.configuration, &node.directory, true, now_ms);
      }
    }
  }

  void broadcast_site_(uint32_t now_ms) {
    std::array<SiteLeaderHeartbeat, N> heartbeats{};
    for (std::size_t sender = 0; sender < N; ++sender) {
      auto &node = this->nodes_[sender];
      if (!node.powered)
        continue;
      auto &heartbeat = heartbeats[sender];
      heartbeat.participant_id = node.identity.node_id;
      heartbeat.boot_id = node.boot_id;
      heartbeat.sequence = ++node.site_sequence;
      heartbeat.term = node.site.state().term == 0 ? 1 : node.site.state().term;
      heartbeat.lease_ms = SiteCoordinator::DEFAULT_LEASE_MS;
      heartbeat.directory_revision = node.directory.topology_revision;
      heartbeat.directory_digest =
          site_directory_payload_hash(node.directory);
      heartbeat.roster_revision = node.configuration.roster_revision;
      heartbeat.election_view_digest =
          node.site.state().election_view_digest;
      heartbeat.credentials_complete = true;
    }
    for (std::size_t receiver = 0; receiver < N; ++receiver) {
      if (!this->nodes_[receiver].powered)
        continue;
      for (std::size_t sender = 0; sender < N; ++sender) {
        if (receiver == sender || !this->nodes_[sender].powered)
          continue;
        if (!this->network_.deliver(TestPacketChannel::SITE_HEARTBEAT,
                                    sender, receiver))
          continue;
        const auto result = this->nodes_[receiver].site.accept_heartbeat(
            this->nodes_[receiver].participants,
            &this->nodes_[receiver].configuration, heartbeats[sender], now_ms);
        require(result != SiteHeartbeatAdmissionResult::INVALID_ARGUMENT &&
                    result != SiteHeartbeatAdmissionResult::FULL,
                "Site heartbeat failed structurally");
      }
    }
  }

  void update_groups_(uint32_t now_ms) {
    for (auto &node : this->nodes_) {
      if (!node.powered)
        continue;
      if (this->canonical_group_runtime_) {
        node.group.update(node.identity, node.participants, node.readiness,
                          &node.configuration, &node.credentials,
                          &node.directory,
                          node.site.state().leader_node_id, now_ms);
      } else {
        node.group.update(node.identity, node.participants, node.readiness,
                          &node.configuration,
                          node.site.state().leader_node_id, now_ms);
      }
    }
  }

  void broadcast_groups_(uint32_t now_ms) {
    std::array<GroupHeartbeat, N> heartbeats{};
    std::array<bool, N> ready{};
    for (std::size_t sender = 0; sender < N; ++sender) {
      auto &node = this->nodes_[sender];
      if (!node.powered)
        continue;
      const auto *group = this->local_group_(node);
      if (group == nullptr)
        continue;
      ready[sender] = true;
      auto &heartbeat = heartbeats[sender];
      heartbeat.participant_id = node.identity.node_id;
      heartbeat.group_id = GROUP_ID;
      heartbeat.boot_id = node.boot_id;
      heartbeat.sequence = ++node.group_sequence;
      heartbeat.term = group->term == 0 ? 1 : group->term;
      heartbeat.lease_ms = GroupCoordinator::DEFAULT_LEASE_MS;
      heartbeat.roster_revision = node.configuration.roster_revision;
      heartbeat.assignment_revision = group->assignment_revision;
      heartbeat.election_view_digest = group->election_view_digest;
      heartbeat.credentials_complete = this->group_credentials_ready_[sender];
    }
    for (std::size_t receiver = 0; receiver < N; ++receiver) {
      if (!this->nodes_[receiver].powered)
        continue;
      for (std::size_t sender = 0; sender < N; ++sender) {
        if (receiver == sender || !ready[sender])
          continue;
        if (!this->network_.deliver(TestPacketChannel::GROUP_HEARTBEAT,
                                    sender, receiver))
          continue;
        const auto result = this->nodes_[receiver].group.accept_heartbeat(
            this->nodes_[receiver].participants,
            this->nodes_[receiver].readiness,
            &this->nodes_[receiver].configuration, heartbeats[sender], now_ms);
        require(result != GroupHeartbeatResult::INVALID_ARGUMENT &&
                    result != GroupHeartbeatResult::FULL,
                "Group heartbeat failed structurally");
      }
    }
  }

  void replicate_configuration_to_(std::size_t index) {
    auto &node = this->nodes_[index];
    if (site_configuration_digest(node.configuration) ==
        site_configuration_digest(this->canonical_))
      return;
    const auto result = validate_replicated_site_configuration(
        node.identity, node.configuration, this->canonical_,
        SiteConfigurationReplicationMode::AUTOMATIC_OWNER_HANDOVER);
    require(result == SiteConfigurationReplicationResult::OK ||
                result == SiteConfigurationReplicationResult::ALREADY_CURRENT,
            "rejoining node rejected the canonical owner configuration");
    node.configuration = this->canonical_;
  }

  void replicate_directories_() {
    for (std::size_t receiver = 0; receiver < N; ++receiver) {
      auto &target = this->nodes_[receiver];
      if (!target.powered || target.site.state().local_leader ||
          target.site.state().leader_node_id[0] == '\0')
        continue;
      const std::size_t source_index =
          this->index_of_(target.site.state().leader_node_id);
      const auto &source = this->nodes_[source_index];
      if (!source.powered ||
          !this->network_.deliver(TestPacketChannel::SITE_DIRECTORY,
                                  source_index, receiver))
        continue;
      const auto validation = validate_replicated_site_directory(
          SITE_ID, target.directory, source.directory, true);
      if (validation == SiteDirectoryReplicationValidation::OK)
        target.directory = source.directory;
    }
  }

  void reconcile_site_summary_caches_(uint32_t now_ms) {
    const char *leader_id = this->site_leader();
    for (auto &node : this->nodes_) {
      if (!node.powered)
        continue;
      if (std::strcmp(node.identity.node_id, leader_id) == 0)
        node.site_summaries.expire(now_ms);
      else
        node.site_summaries.clear();
    }
  }

  void publish_site_summary_(std::size_t group_leader_index,
                             const SpatialProductionFusionOutput &fusion,
                             uint32_t now_ms) {
    this->reconcile_site_summary_caches_(now_ms);
    const std::size_t site_leader_index = this->index_of_(this->site_leader());
    auto &group_leader = this->nodes_[group_leader_index];
    auto &site_leader = this->nodes_[site_leader_index];
    const auto *group = this->local_group_(site_leader);
    SiteSpaceSummary summary{};
    require(build_site_space_summary(
                SITE_ID, SPACE_ID, GROUP_ID, group_leader.identity.node_id,
                group_leader.boot_id, ++group_leader.summary_sequence,
                group->term, fusion, &summary),
            "Group Leader failed to build a Site summary");
    require(evaluate_site_space_summary_admission(
                summary, group_leader.identity.node_id,
                site_leader.identity.node_id, SITE_ID,
                site_leader.site.state(), &site_leader.configuration,
                group) == SiteSpaceSummaryAdmissionResult::OK,
            "Site Leader rejected the current Group summary");
    if (!this->network_.deliver(TestPacketChannel::SITE_SUMMARY,
                                group_leader_index, site_leader_index))
      return;
    require(site_leader.site_summaries.store(summary, now_ms) ==
                SiteSpaceSummaryAdmissionResult::OK,
            "Site Leader failed to cache the current Group summary");
  }
};

template <std::size_t N>
void exercise_failure_subset(const char *topology,
                             const std::array<const char *, N> &ids,
                             std::size_t owner_index, uint32_t failed_mask,
                             uint32_t outage_ms,
                             ParticipantTopology participant_topology =
                                 ParticipantTopology::FULL_MESH) {
  const uint32_t all_nodes = (1U << N) - 1U;
  require(failed_mask != 0 && failed_mask != all_nodes,
          "failure subset must leave at least one powered Node");
  char scenario[64]{};
  std::snprintf(scenario, sizeof(scenario), "owner=%u mask=0x%02X",
                static_cast<unsigned>(owner_index),
                static_cast<unsigned>(failed_mask));
  set_context(topology, scenario, outage_ms);
  FailureCluster<N> cluster(ids, owner_index, participant_topology);
  char initial_site_id[ParticipantRecord::MAX_ID_LENGTH + 1]{};
  char initial_group_id[ParticipantRecord::MAX_ID_LENGTH + 1]{};
  char initial_owner_id[ParticipantRecord::MAX_ID_LENGTH + 1]{};
  copy_text(initial_site_id, sizeof(initial_site_id), cluster.site_leader());
  copy_text(initial_group_id, sizeof(initial_group_id), cluster.group_leader());
  copy_text(initial_owner_id, sizeof(initial_owner_id),
            cluster.configuration_owner());

  bool owner_offline = false;
  for (std::size_t index = 0; index < N; ++index) {
    if ((failed_mask & (1U << index)) == 0)
      continue;
    owner_offline = owner_offline ||
                    std::strcmp(ids[index], initial_owner_id) == 0;
    cluster.power_off(index);
  }

  if (outage_ms >= OWNER_OUTAGE_MS && owner_offline) {
    cluster.settle(START_MS + SiteCoordinator::DEFAULT_LEASE_MS);
    const char *intermediate_site = cluster.expected_site_leader_from_powered();
    const char *intermediate_group =
        cluster.expected_group_leader_from_powered(intermediate_site);
    cluster.assert_expected_roles(intermediate_site, intermediate_group);
    cluster.assert_role_consensus(cluster.powered_count());
    cluster.publish_or_expire_fusion(
        START_MS + SiteCoordinator::DEFAULT_LEASE_MS,
        SiteCoordinator::DEFAULT_LEASE_MS);
    const bool intermediate_fusion = cluster.powered_count() >= 2;
    const bool intermediate_quorum =
        cluster.powered_count() >= strict_majority(static_cast<uint8_t>(N));
    cluster.assert_automation(intermediate_fusion && intermediate_quorum);
    const char *successor = cluster.configuration_successor();
    if (intermediate_quorum && successor != nullptr) {
      cluster.perform_owner_handover(
          START_MS + SiteCoordinator::DEFAULT_LEASE_MS,
          START_MS + OWNER_OUTAGE_MS);
    } else {
      require(cluster.owner_handover_is_blocked(),
              "owner handover bypassed its authority prerequisites");
      cluster.settle(START_MS + outage_ms);
    }
  } else {
    cluster.settle(START_MS + outage_ms);
  }

  if (outage_ms < SiteCoordinator::DEFAULT_LEASE_MS) {
    cluster.assert_expected_roles(initial_site_id, initial_group_id);
    cluster.assert_role_consensus(N);
  } else {
    const char *expected_site = cluster.expected_site_leader_from_powered();
    const char *expected_group =
        cluster.expected_group_leader_from_powered(expected_site);
    cluster.assert_expected_roles(expected_site, expected_group);
    cluster.assert_role_consensus(cluster.powered_count());
  }
  cluster.assert_configuration_consensus();
  cluster.publish_or_expire_fusion(START_MS + outage_ms, outage_ms);

  bool initial_group_leader_powered = true;
  for (std::size_t index = 0; index < N; ++index) {
    if ((failed_mask & (1U << index)) != 0 &&
        std::strcmp(ids[index], initial_group_id) == 0) {
      initial_group_leader_powered = false;
    }
  }
  const bool healthy_automation = outage_ms < FusionSnapshot::DEFAULT_TTL_MS ||
      (outage_ms < GroupCoordinator::DEFAULT_LEASE_MS
           ? initial_group_leader_powered && cluster.powered_count() >= 2
           : cluster.powered_count() >= 2 &&
                 cluster.powered_count() >=
                     strict_majority(static_cast<uint8_t>(N)));
  cluster.assert_automation(healthy_automation);

  char surviving_site_id[ParticipantRecord::MAX_ID_LENGTH + 1]{};
  char surviving_group_id[ParticipantRecord::MAX_ID_LENGTH + 1]{};
  copy_text(surviving_site_id, sizeof(surviving_site_id),
            cluster.site_leader());
  copy_text(surviving_group_id, sizeof(surviving_group_id),
            cluster.group_leader());
  if (!cluster.participant_powered(surviving_site_id))
    surviving_site_id[0] = '\0';
  if (!cluster.participant_powered(surviving_group_id))
    surviving_group_id[0] = '\0';
  cluster.commit_runtime_group_assignment();

  for (std::size_t index = 0; index < N; ++index) {
    if ((failed_mask & (1U << index)) != 0)
      cluster.power_on(index, true);
  }
  cluster.settle(START_MS + outage_ms + 1000);
  const char *rejoined_site =
      surviving_site_id[0] != '\0'
          ? surviving_site_id
          : cluster.site_leader();
  const char *rejoined_group =
      surviving_group_id[0] != '\0' &&
              std::strcmp(surviving_group_id, rejoined_site) != 0
          ? surviving_group_id
          : cluster.group_leader();
  require(rejoined_site[0] != '\0' && rejoined_group[0] != '\0',
          "rejoined canonical quorum did not restore both runtime Leaders");
  cluster.assert_expected_roles(rejoined_site, rejoined_group);
  cluster.assert_role_consensus(N);
  cluster.assert_configuration_consensus();
  cluster.publish_or_expire_fusion(START_MS + outage_ms + 1000, 0);
  cluster.assert_automation(true);
}

template <std::size_t N>
void exhaustive_topology_matrix(const char *topology,
                                const std::array<const char *, N> &ids,
                                ParticipantTopology participant_topology =
                                    ParticipantTopology::FULL_MESH) {
  const std::array<uint32_t, 7> outages{
      1,
      FusionSnapshot::DEFAULT_TTL_MS - 1,
      FusionSnapshot::DEFAULT_TTL_MS,
      SiteCoordinator::DEFAULT_LEASE_MS - 1,
      SiteCoordinator::DEFAULT_LEASE_MS,
      OWNER_OUTAGE_MS - 1,
      OWNER_OUTAGE_MS};
  const uint32_t all_nodes = (1U << N) - 1U;
  for (std::size_t owner = 0; owner < N; ++owner) {
    for (uint32_t failed_mask = 1; failed_mask < all_nodes; ++failed_mask) {
      for (const uint32_t outage : outages) {
        exercise_failure_subset(topology, ids, owner, failed_mask, outage,
                                participant_topology);
      }
    }
  }
}

void all_supported_topologies_and_failure_subsets() {
  const std::array<const char *, 2> n2{"node-10", "node-20"};
  const std::array<const char *, 3> n3{"node-10", "node-20", "node-30"};
  const std::array<const char *, 4> n4{
      "node-10", "node-20", "node-30", "node-40"};
  const std::array<const char *, 5> n5{
      "node-10", "node-20", "node-30", "node-40", "node-50"};
  const std::array<const char *, 6> n6{
      "node-10", "node-20", "node-30", "node-40", "node-50", "node-60"};
  const std::array<const char *, 7> n7{
      "node-10", "node-20", "node-30", "node-40",
      "node-50", "node-60", "node-70"};
  exhaustive_topology_matrix("exhaustive-2", n2);
  exhaustive_topology_matrix("exhaustive-3", n3);
  exhaustive_topology_matrix("exhaustive-4", n4);
  exhaustive_topology_matrix("exhaustive-5", n5);
  exhaustive_topology_matrix("exhaustive-6", n6);
  exhaustive_topology_matrix("exhaustive-7", n7);
  exhaustive_topology_matrix("owner-hub-exhaustive-2", n2,
                             ParticipantTopology::OWNER_HUB);
  exhaustive_topology_matrix("owner-hub-exhaustive-3", n3,
                             ParticipantTopology::OWNER_HUB);
  exhaustive_topology_matrix("owner-hub-exhaustive-4", n4,
                             ParticipantTopology::OWNER_HUB);
  exhaustive_topology_matrix("owner-hub-exhaustive-5", n5,
                             ParticipantTopology::OWNER_HUB);
  exhaustive_topology_matrix("owner-hub-exhaustive-6", n6,
                             ParticipantTopology::OWNER_HUB);
  exhaustive_topology_matrix("owner-hub-exhaustive-7", n7,
                             ParticipantTopology::OWNER_HUB);
}

struct PeriodicLossProfile {
  const char *name;
  uint16_t period;
  bool keep_only;
};

template <std::size_t N>
void periodic_loss_preserves_live_authority(
    const char *topology, const std::array<const char *, N> &ids,
    std::size_t owner_index,
    ParticipantTopology participant_topology =
        ParticipantTopology::FULL_MESH) {
  const std::array<PeriodicLossProfile, 5> profiles{{
      {"drop-1-percent", 100, false},
      {"drop-10-percent", 10, false},
      {"drop-25-percent", 4, false},
      {"drop-50-percent", 2, false},
      {"drop-75-percent", 4, true},
  }};
  const std::array<TestPacketChannel, 5> channels{
      TestPacketChannel::SITE_HEARTBEAT,
      TestPacketChannel::GROUP_HEARTBEAT,
      TestPacketChannel::OBSERVATION,
      TestPacketChannel::FUSION_SNAPSHOT,
      TestPacketChannel::SITE_SUMMARY};
  for (const auto &profile : profiles) {
    set_context(topology, profile.name, 100000);
    FailureCluster<N> cluster(ids, owner_index, participant_topology);
    char site[ParticipantRecord::MAX_ID_LENGTH + 1]{};
    char group[ParticipantRecord::MAX_ID_LENGTH + 1]{};
    copy_text(site, sizeof(site), cluster.site_leader());
    copy_text(group, sizeof(group), cluster.group_leader());
    for (const auto channel : channels) {
      if (profile.keep_only)
        cluster.set_periodic_keep(channel, profile.period);
      else
        cluster.set_periodic_drop(channel, profile.period);
    }
    uint32_t now_ms = START_MS;
    for (uint16_t cycle = 0; cycle < 400; ++cycle) {
      now_ms += 250;
      cluster.tick(now_ms);
      cluster.publish_or_expire_fusion(now_ms, 0);
    }
    cluster.assert_expected_roles(site, group);
    cluster.assert_role_consensus(N);
    cluster.assert_configuration_consensus();
    cluster.assert_automation(true);
    cluster.assert_site_summary(true);
    for (const auto channel : channels) {
      require(cluster.dropped_packets(channel) != 0,
              "periodic loss profile did not exercise a channel");
    }
    cluster.clear_packet_faults();
    cluster.assert_roles_remain_stable(now_ms + 1);
  }
}

template <std::size_t N>
void partition_matrix_recovers(const char *topology,
                               const std::array<const char *, N> &ids,
                               std::size_t owner_index,
                               ParticipantTopology participant_topology =
                                   ParticipantTopology::FULL_MESH) {
  const uint32_t all_nodes = (1U << N) - 1U;
  for (uint32_t mask = 1; mask < all_nodes; ++mask) {
    if ((mask & 1U) == 0)
      continue;
    const uint32_t other = all_nodes ^ mask;
    for (uint8_t direction = 0; direction < 3; ++direction) {
      char label[64]{};
      std::snprintf(label, sizeof(label), "mask=0x%02X direction=%u",
                    static_cast<unsigned>(mask),
                    static_cast<unsigned>(direction));
      set_context(topology, label, SiteCoordinator::DEFAULT_LEASE_MS + 1);
      FailureCluster<N> cluster(ids, owner_index, participant_topology);
      char owner[ParticipantRecord::MAX_ID_LENGTH + 1]{};
      copy_text(owner, sizeof(owner), cluster.configuration_owner());
      for (const auto channel : {TestPacketChannel::SITE_HEARTBEAT,
                                 TestPacketChannel::GROUP_HEARTBEAT}) {
        if (direction == 0) {
          cluster.block_partition(channel, mask);
        } else if (direction == 1) {
          cluster.block_direction(channel, mask, other);
        } else {
          cluster.block_direction(channel, other, mask);
        }
      }
      const uint32_t partitioned_at =
          START_MS + SiteCoordinator::DEFAULT_LEASE_MS + 1;
      cluster.tick(partitioned_at);
      cluster.tick(partitioned_at + 1);
      cluster.assert_no_split_authority();
      cluster.clear_packet_faults();
      cluster.settle(partitioned_at + 2);
      cluster.publish_or_expire_fusion(partitioned_at + 2, 0);
      const char *site = cluster.site_leader();
      const char *group = cluster.group_leader();
      require(site[0] != '\0' && group[0] != '\0',
              "healed partition did not restore both runtime Leaders");
      cluster.assert_expected_roles(site, group);
      cluster.assert_role_consensus(N);
      cluster.assert_configuration_consensus();
      cluster.assert_automation(true);
      cluster.assert_site_summary(true);
      cluster.assert_roles_remain_stable(partitioned_at + 3);
      require(std::strcmp(cluster.configuration_owner(), owner) == 0,
              "bounded network partition changed configuration owner");
    }
  }
}

void packet_loss_ttl_boundaries_fail_closed_and_recover() {
  const std::array<const char *, 3> ids{
      "node-20-group", "node-30-owner", "node-10-site"};

  set_context("packet-loss-ttl", "observation", 3500);
  FailureCluster<3> observation(ids, 1);
  observation.block_packet(TestPacketChannel::OBSERVATION, 1, 0);
  observation.block_packet(TestPacketChannel::OBSERVATION, 2, 0);
  observation.publish_or_expire_fusion(
      START_MS + LocalObservation::DEFAULT_TTL_MS - 1, 0);
  observation.assert_automation(true);
  observation.publish_or_expire_fusion(
      START_MS + LocalObservation::DEFAULT_TTL_MS - 1 +
          FusionSnapshot::DEFAULT_TTL_MS,
      0);
  observation.assert_automation(false);
  observation.clear_packet_faults();
  observation.publish_or_expire_fusion(
      START_MS + LocalObservation::DEFAULT_TTL_MS +
          FusionSnapshot::DEFAULT_TTL_MS,
      0);
  observation.assert_automation(true);

  set_context("packet-loss-ttl", "fusion-snapshot",
              FusionSnapshot::DEFAULT_TTL_MS);
  FailureCluster<3> fusion(ids, 1);
  fusion.block_packet(TestPacketChannel::FUSION_SNAPSHOT, 0, 2);
  fusion.publish_or_expire_fusion(
      START_MS + FusionSnapshot::DEFAULT_TTL_MS - 1, 0);
  fusion.assert_node_automation(2, true);
  fusion.publish_or_expire_fusion(
      START_MS + FusionSnapshot::DEFAULT_TTL_MS, 0);
  fusion.assert_node_automation(2, false);
  fusion.assert_node_automation(0, true);
  fusion.assert_node_automation(1, true);
  fusion.clear_packet_faults();
  fusion.publish_or_expire_fusion(
      START_MS + FusionSnapshot::DEFAULT_TTL_MS + 1, 0);
  fusion.assert_automation(true);

  set_context("packet-loss-ttl", "site-summary",
              SiteSpaceSummary::DEFAULT_TTL_MS);
  FailureCluster<3> summary(ids, 1);
  summary.block_packet(TestPacketChannel::SITE_SUMMARY, 0, 2);
  summary.publish_or_expire_fusion(
      START_MS + SiteSpaceSummary::DEFAULT_TTL_MS - 1, 0);
  summary.assert_site_summary(true);
  summary.publish_or_expire_fusion(
      START_MS + SiteSpaceSummary::DEFAULT_TTL_MS, 0);
  summary.assert_site_summary(false);
  summary.clear_packet_faults();
  summary.publish_or_expire_fusion(
      START_MS + SiteSpaceSummary::DEFAULT_TTL_MS + 1, 0);
  summary.assert_site_summary(true);
}

void selective_heartbeat_bursts_recover() {
  const std::array<const char *, 3> ids{
      "node-20-group", "node-30-owner", "node-10-site"};
  struct HeartbeatBurst {
    TestPacketChannel channel;
    std::size_t sender;
    const char *name;
  };
  const std::array<HeartbeatBurst, 2> bursts{{
      {TestPacketChannel::SITE_HEARTBEAT, 2, "site-heartbeat"},
      {TestPacketChannel::GROUP_HEARTBEAT, 0, "group-heartbeat"},
  }};
  const std::array<uint32_t, 2> durations{
      SiteCoordinator::DEFAULT_LEASE_MS - 1,
      SiteCoordinator::DEFAULT_LEASE_MS + 1};
  for (const auto &burst : bursts) {
    for (const uint32_t duration : durations) {
      set_context("packet-loss-selective", burst.name, duration);
      FailureCluster<3> cluster(ids, 1);
      char owner[ParticipantRecord::MAX_ID_LENGTH + 1]{};
      copy_text(owner, sizeof(owner), cluster.configuration_owner());
      for (std::size_t receiver = 0; receiver < ids.size(); ++receiver) {
        if (receiver != burst.sender)
          cluster.block_packet(burst.channel, burst.sender, receiver);
      }
      cluster.tick(START_MS + duration);
      if (duration < SiteCoordinator::DEFAULT_LEASE_MS)
        cluster.assert_role_consensus(ids.size());
      cluster.clear_packet_faults();
      cluster.settle(START_MS + duration + 1);
      cluster.publish_or_expire_fusion(START_MS + duration + 1, 0);
      const char *site = cluster.site_leader();
      const char *group = cluster.group_leader();
      require(site[0] != '\0' && group[0] != '\0',
              "heartbeat recovery did not restore both runtime Leaders");
      cluster.assert_expected_roles(site, group);
      cluster.assert_role_consensus(ids.size());
      cluster.assert_configuration_consensus();
      cluster.assert_automation(true);
      cluster.assert_site_summary(true);
      cluster.assert_roles_remain_stable(START_MS + duration + 2);
      require(std::strcmp(cluster.configuration_owner(), owner) == 0,
              "selective heartbeat loss changed configuration owner");
    }
  }
}

void packet_loss_role_matrix() {
  const std::array<const char *, 2> n2{"node-10", "node-20"};
  const std::array<const char *, 3> n3{
      "node-20-group", "node-30-owner", "node-10-site"};
  const std::array<const char *, 7> n7{
      "node-20", "node-30", "node-10", "node-40",
      "node-50", "node-60", "node-70"};
  periodic_loss_preserves_live_authority("loss-periodic-2", n2, 1);
  periodic_loss_preserves_live_authority("loss-periodic-3", n3, 1);
  periodic_loss_preserves_live_authority("loss-periodic-7", n7, 1);
  periodic_loss_preserves_live_authority(
      "owner-hub-loss-periodic-7", n7, 1,
      ParticipantTopology::OWNER_HUB);
  partition_matrix_recovers("loss-partition-2", n2, 1);
  partition_matrix_recovers("loss-partition-3", n3, 1);
  partition_matrix_recovers("loss-partition-7", n7, 1);
  partition_matrix_recovers("owner-hub-loss-partition-7", n7, 1,
                            ParticipantTopology::OWNER_HUB);
  packet_loss_ttl_boundaries_fail_closed_and_recover();
  selective_heartbeat_bursts_recover();
}

template <std::size_t N>
void exercise_complete_power_cycle(const char *topology,
                                   const std::array<const char *, N> &ids,
                                   std::size_t owner_index,
                                   ParticipantTopology participant_topology =
                                       ParticipantTopology::FULL_MESH) {
  set_context(topology, "all", COORDINATOR_OUTAGE_MS);
  FailureCluster<N> cluster(ids, owner_index, participant_topology);
  char owner[ParticipantRecord::MAX_ID_LENGTH + 1]{};
  copy_text(owner, sizeof(owner), cluster.configuration_owner());
  for (std::size_t index = 0; index < N; ++index)
    cluster.power_off(index);
  for (std::size_t index = 0; index < N; ++index)
    cluster.power_on(index);
  cluster.settle(START_MS + COORDINATOR_OUTAGE_MS);
  cluster.publish_or_expire_fusion(START_MS + COORDINATOR_OUTAGE_MS, 0);
  const char *expected_site = cluster.expected_site_leader_from_powered();
  const char *expected_group =
      cluster.expected_group_leader_from_powered(expected_site);
  cluster.assert_expected_roles(expected_site, expected_group);
  cluster.assert_role_consensus(N);
  cluster.assert_configuration_consensus();
  cluster.assert_automation(true);
  cluster.assert_roles_remain_stable(
      START_MS + COORDINATOR_OUTAGE_MS + 1000);
  require(std::strcmp(cluster.configuration_owner(), owner) == 0,
          "complete power cycle changed configuration owner");
}

void complete_power_cycles_recover_canonical_roles() {
  const std::array<const char *, 2> two_ids{
      "node-10-site", "node-20-owner"};
  const std::array<const char *, 3> three_ids{
      "node-20-group", "node-30-owner", "node-10-site"};
  const std::array<const char *, 7> seven_ids{
      "node-20-group", "node-30-owner", "node-10-site",
      "node-40-normal", "node-50-normal", "node-60-normal",
      "node-70-normal"};
  exercise_complete_power_cycle("two-node-full-cycle", two_ids, 1);
  exercise_complete_power_cycle("three-node-full-cycle", three_ids, 1);
  exercise_complete_power_cycle("seven-node-full-cycle", seven_ids, 1);
  exercise_complete_power_cycle("seven-node-owner-hub-full-cycle", seven_ids,
                                1, ParticipantTopology::OWNER_HUB);
}

void repeated_short_role_flaps_recover_without_owner_change() {
  const std::array<const char *, 7> ids{
      "node-20-group", "node-30-owner", "node-10-site",
      "node-40-normal", "node-50-normal", "node-60-normal",
      "node-70-normal"};
  set_context("seven-node-owner-hub-flap", "roles", SNAPSHOT_OUTAGE_MS);
  FailureCluster<7> cluster(ids, 1, ParticipantTopology::OWNER_HUB);
  char owner[ParticipantRecord::MAX_ID_LENGTH + 1]{};
  copy_text(owner, sizeof(owner), cluster.configuration_owner());
  uint32_t now_ms = START_MS;
  for (uint8_t cycle = 0; cycle < 5; ++cycle) {
    for (const std::size_t node : {std::size_t{0}, std::size_t{1},
                                   std::size_t{2}}) {
      cluster.power_off(node);
      now_ms += SNAPSHOT_OUTAGE_MS;
      cluster.settle(now_ms);
      cluster.power_on(node);
      now_ms += 500;
      cluster.settle(now_ms);
      cluster.publish_or_expire_fusion(now_ms, 0);
      cluster.assert_configuration_consensus();
      cluster.assert_role_consensus(7);
      cluster.assert_automation(true);
      require(std::strcmp(cluster.configuration_owner(), owner) == 0,
              "short role flap unexpectedly changed configuration owner");
    }
  }
}

void every_role_failure_order_recovers() {
  const std::array<const char *, 7> ids{
      "node-20-group", "node-30-owner", "node-10-site",
      "node-40-normal", "node-50-normal", "node-60-normal",
      "node-70-normal"};
  const std::array<std::array<std::size_t, 3>, 6> orders{{
      {0, 1, 2}, {0, 2, 1}, {1, 0, 2},
      {1, 2, 0}, {2, 0, 1}, {2, 1, 0}}};
  for (const auto &order : orders) {
    char label[48]{};
    std::snprintf(label, sizeof(label), "%u-%u-%u",
                  static_cast<unsigned>(order[0]),
                  static_cast<unsigned>(order[1]),
                  static_cast<unsigned>(order[2]));
    set_context("seven-node-owner-hub-role-order", label,
                SiteCoordinator::DEFAULT_LEASE_MS);
    FailureCluster<7> cluster(ids, 1, ParticipantTopology::OWNER_HUB);
    char owner[ParticipantRecord::MAX_ID_LENGTH + 1]{};
    copy_text(owner, sizeof(owner), cluster.configuration_owner());
    uint32_t now_ms = START_MS;
    for (const std::size_t index : order) {
      cluster.power_off(index);
      now_ms += SiteCoordinator::DEFAULT_LEASE_MS;
      cluster.settle(now_ms);
      const char *site = cluster.expected_site_leader_from_powered();
      const char *group = cluster.expected_group_leader_from_powered(site);
      cluster.assert_expected_roles(site, group);
      cluster.assert_role_consensus(cluster.powered_count());
      cluster.assert_configuration_consensus();
      cluster.publish_or_expire_fusion(
          now_ms, SiteCoordinator::DEFAULT_LEASE_MS);
      cluster.assert_automation(true);
      require(std::strcmp(cluster.configuration_owner(), owner) == 0,
              "bounded sequential outage unexpectedly changed owner");
    }
    char surviving_site[ParticipantRecord::MAX_ID_LENGTH + 1]{};
    char surviving_group[ParticipantRecord::MAX_ID_LENGTH + 1]{};
    copy_text(surviving_site, sizeof(surviving_site), cluster.site_leader());
    copy_text(surviving_group, sizeof(surviving_group),
              cluster.group_leader());
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
      cluster.power_on(*it);
      now_ms += 1000;
      cluster.settle(now_ms);
      cluster.publish_or_expire_fusion(now_ms, 0);
      cluster.assert_expected_roles(surviving_site, surviving_group);
      cluster.assert_role_consensus(cluster.powered_count());
      cluster.assert_configuration_consensus();
      cluster.assert_automation(true);
    }
  }
}

void sequential_role_failures_remain_bounded_and_fail_closed() {
  const std::array<const char *, 7> ids{
      "node-20-group", "node-30-owner", "node-10-site",
      "node-40-normal", "node-50-normal", "node-60-normal",
      "node-70-normal"};
  set_context("seven-node-owner-hub-sequential", "group-site-owner",
              OWNER_OUTAGE_MS);
  FailureCluster<7> cluster(ids, 1, ParticipantTopology::OWNER_HUB);

  cluster.power_off(0);
  cluster.settle(START_MS + COORDINATOR_OUTAGE_MS);
  cluster.assert_expected_roles("node-10-site", "node-40-normal");
  cluster.assert_role_consensus(6);

  cluster.power_off(2);
  cluster.settle(START_MS + COORDINATOR_OUTAGE_MS * 2);
  cluster.assert_expected_roles("node-40-normal", "node-50-normal");
  cluster.assert_role_consensus(5);

  cluster.power_off(1);
  cluster.settle(START_MS + COORDINATOR_OUTAGE_MS * 3);
  cluster.assert_expected_roles("node-40-normal", "node-50-normal");
  cluster.assert_role_consensus(4);
  cluster.perform_owner_handover(
      START_MS + COORDINATOR_OUTAGE_MS * 3,
      START_MS + COORDINATOR_OUTAGE_MS * 3 +
          AutomaticConfigurationHandoverTracker::STABILITY_MS);
  require(std::strcmp(cluster.configuration_owner(), "node-30-owner") != 0,
          "an offline fixed successor prevented an available owner handover");

  cluster.power_on(2);
  cluster.settle(START_MS + COORDINATOR_OUTAGE_MS * 3 +
                 AutomaticConfigurationHandoverTracker::STABILITY_MS + 1000);
  cluster.assert_expected_roles("node-40-normal", "node-50-normal");

  cluster.power_on(0);
  cluster.power_on(1);
  cluster.settle(START_MS + COORDINATOR_OUTAGE_MS * 3 + 2000 +
                 AutomaticConfigurationHandoverTracker::STABILITY_MS);
  cluster.publish_or_expire_fusion(
      START_MS + COORDINATOR_OUTAGE_MS * 3 + 2000 +
          AutomaticConfigurationHandoverTracker::STABILITY_MS,
      0);
  cluster.assert_expected_roles("node-40-normal", "node-50-normal");
  cluster.assert_role_consensus(7);
  cluster.assert_configuration_consensus();
  cluster.assert_automation(true);
}

void persisted_directory_fence_blocks_a_stale_rebooted_site_leader() {
  const std::array<const char *, 3> ids{
      "node-10-site", "node-20-owner", "node-30-normal"};
  set_context("three-node-directory-reboot", "persisted-directory",
              SiteCoordinator::DEFAULT_LEASE_MS);
  FailureCluster<3> cluster(ids, 1);
  std::size_t leader = 0;
  while (leader < ids.size() &&
         std::strcmp(ids[leader], cluster.site_leader()) != 0)
    ++leader;
  require(leader < ids.size(), "initial Site Leader is not in the fixture");
  const std::size_t stale = (leader + 1U) % ids.size();
  const std::size_t fresh = 3U - leader - stale;

  cluster.power_off(stale);
  cluster.settle(START_MS + SiteCoordinator::DEFAULT_LEASE_MS);
  const uint32_t stale_revision = cluster.directory_revision(stale);
  cluster.advance_directory(leader, 0x101U);
  cluster.settle(START_MS + SiteCoordinator::DEFAULT_LEASE_MS + 1U);
  require(cluster.directory_revision(fresh) > stale_revision,
          "the online successor did not receive the newer Directory");

  cluster.power_off(leader);
  cluster.power_on(stale, true);
  cluster.settle(START_MS + SiteCoordinator::DEFAULT_LEASE_MS * 2U + 1U);
  require(std::strcmp(cluster.site_leader(), ids[fresh]) == 0,
          "a stale rebooted Node became Site Leader before convergence");
  cluster.assert_directory_consensus();
}

void equal_revision_directory_split_brain_converges_after_partition() {
  const std::array<const char *, 3> ids{
      "node-10-site", "node-20-owner", "node-30-normal"};
  set_context("three-node-directory-partition", "equal-revision",
              SiteCoordinator::DEFAULT_LEASE_MS);
  FailureCluster<3> cluster(ids, 1);
  cluster.block_partition(TestPacketChannel::SITE_HEARTBEAT, 0x1U);
  cluster.block_partition(TestPacketChannel::SITE_DIRECTORY, 0x1U);
  cluster.settle(START_MS + SiteCoordinator::DEFAULT_LEASE_MS);
  cluster.advance_directory_partition(0x1U, 0x111U);
  cluster.advance_directory_partition(0x6U, 0x222U);
  require(cluster.directory_revision(0) == cluster.directory_revision(1) &&
              cluster.directory_digest(0) != cluster.directory_digest(1),
          "the partition fixture did not create equal-revision divergence");

  cluster.clear_packet_faults();
  cluster.settle(START_MS + SiteCoordinator::DEFAULT_LEASE_MS * 2U);
  cluster.assert_role_consensus(3);
  cluster.assert_directory_consensus();
}

template <std::size_t N>
void exercise_owner_hub_runtime_contract(
    const char *topology, const std::array<const char *, N> &ids,
    std::size_t owner_index) {
  set_context(topology, "canonical-runtime", 0);
  FailureCluster<N> cluster(ids, owner_index,
                            ParticipantTopology::OWNER_HUB);
  cluster.assert_owner_hub_runtime_contract(owner_index);
  cluster.assert_role_consensus(N);
  cluster.assert_configuration_consensus();
  cluster.assert_automation(true);
  cluster.assert_site_summary(true);
}

void all_owner_hub_sizes_carry_indirect_group_runtime() {
  const std::array<const char *, 2> n2{
      "node-20-group", "node-30-owner"};
  const std::array<const char *, 3> n3{
      "node-20-group", "node-30-owner", "node-10-site"};
  const std::array<const char *, 4> n4{
      "node-20-group", "node-30-owner", "node-10-site", "node-40-normal"};
  const std::array<const char *, 5> n5{
      "node-20-group", "node-30-owner", "node-10-site", "node-40-normal",
      "node-50-normal"};
  const std::array<const char *, 6> n6{
      "node-20-group", "node-30-owner", "node-10-site", "node-40-normal",
      "node-50-normal", "node-60-normal"};
  const std::array<const char *, 7> n7{
      "node-20-group", "node-30-owner", "node-10-site", "node-40-normal",
      "node-50-normal", "node-60-normal", "node-70-normal"};
  exercise_owner_hub_runtime_contract("owner-hub-runtime-2", n2, 1);
  exercise_owner_hub_runtime_contract("owner-hub-runtime-3", n3, 1);
  exercise_owner_hub_runtime_contract("owner-hub-runtime-4", n4, 1);
  exercise_owner_hub_runtime_contract("owner-hub-runtime-5", n5, 1);
  exercise_owner_hub_runtime_contract("owner-hub-runtime-6", n6, 1);
  exercise_owner_hub_runtime_contract("owner-hub-runtime-7", n7, 1);
}

void three_node_owner_hub_indirect_runtime_regression() {
  set_context("three-node-owner-hub", "indirect-group-runtime", 0);
  PlatformIdentity owner{};
  PlatformIdentity local{};
  PlatformIdentity indirect{};
  copy_text(owner.site_id, sizeof(owner.site_id), SITE_ID);
  copy_text(local.site_id, sizeof(local.site_id), SITE_ID);
  copy_text(indirect.site_id, sizeof(indirect.site_id), SITE_ID);
  copy_text(owner.node_id, sizeof(owner.node_id), "node-owner");
  copy_text(local.node_id, sizeof(local.node_id), "node-z-local");
  copy_text(indirect.node_id, sizeof(indirect.node_id), "node-a-indirect");

  ParticipantRegistry owner_participants{};
  ParticipantReadinessRegistry owner_readiness{};
  add_direct_runtime_peer(&owner_participants, &owner_readiness,
                          local.node_id);
  add_direct_runtime_peer(&owner_participants, &owner_readiness,
                          indirect.node_id);
  SiteConfigurationSnapshot base{};
  SiteConfigurationSnapshot configuration{};
  require(initialize_site_configuration(owner, false, &base) ==
                  SiteConfigurationResult::OK &&
              commit_local_site_roster(owner, owner_participants, base,
                                       &configuration) ==
                  SiteConfigurationResult::OK,
          "owner-hub canonical roster fixture failed");
  configuration.floorplan.committed = true;
  copy_text(configuration.floorplan.floorplan_id,
            sizeof(configuration.floorplan.floorplan_id), FLOORPLAN_ID);
  configuration.floorplan.geometry_version = 1;
  for (auto &member : configuration.members) {
    if (!member.occupied)
      continue;
    member.spatial_profile.committed = true;
    copy_text(member.spatial_profile.space_id,
              sizeof(member.spatial_profile.space_id), SPACE_ID);
    copy_text(member.spatial_profile.group_id,
              sizeof(member.spatial_profile.group_id), GROUP_ID);
    member.spatial_profile.pose_version = 1;
    member.spatial_profile.calibration_version = 1;
    member.spatial_profile.transform = valid_transform();
  }

  ParticipantRegistry local_participants{};
  ParticipantReadinessRegistry local_readiness{};
  add_direct_runtime_peer(&local_participants, &local_readiness,
                          owner.node_id);
  require(local_participants.find(indirect.node_id) == nullptr &&
              local_readiness.find(indirect.node_id) == nullptr,
          "owner-hub fixture accidentally created a direct peer record");

  GroupCoordinator groups{};
  groups.update(local, local_participants, local_readiness, &configuration,
                owner.node_id, 1000);
  GroupHeartbeat heartbeat{};
  heartbeat.participant_id = indirect.node_id;
  heartbeat.group_id = GROUP_ID;
  heartbeat.boot_id = "boot-indirect";
  heartbeat.sequence = 1;
  heartbeat.term = 1;
  heartbeat.lease_ms = GroupCoordinator::DEFAULT_LEASE_MS;
  require(groups.accept_heartbeat(local_participants, local_readiness,
                                  &configuration, heartbeat, 1000) ==
              GroupHeartbeatResult::OK,
          "indirect Group heartbeat was rejected in the owner-hub topology");
  groups.update(local, local_participants, local_readiness, &configuration,
                owner.node_id, 1000);
  const auto *group = groups.local_group(&configuration, local.node_id);
  require(group != nullptr && !group->local_leader &&
              std::strcmp(group->leader_node_id, indirect.node_id) == 0,
          "owner-hub topology did not elect the indirect Group Leader");

  ParticipantRecord projected_leader{};
  ParticipantReadinessRecord projected_leader_readiness{};
  require(build_canonical_group_runtime_records(
              &configuration, local.node_id, indirect.node_id,
              &projected_leader, &projected_leader_readiness),
          "indirect Group Leader runtime projection failed");
  ParticipantCredentialRecord credential{};
  credential.occupied = true;
  credential.enabled = true;
  ParticipantEndpoint endpoint{};
  endpoint.occupied = true;
  endpoint.authenticated = true;
  endpoint.ipv4 = 1;
  endpoint.authenticated_at_ms = 1000;
  require(evaluate_observation_send(
              local.node_id, group, false, &projected_leader,
              &projected_leader_readiness, &credential, &endpoint, 1,
              1000) == ObservationSendResult::OK,
          "indirect Group Leader could not receive a local observation");

  ParticipantRecord projected_sender{};
  ParticipantReadinessRecord projected_sender_readiness{};
  require(build_canonical_group_runtime_records(
              &configuration, indirect.node_id, local.node_id,
              &projected_sender, &projected_sender_readiness),
          "indirect observation sender runtime projection failed");
  ParticipantObservationMetadata observation{};
  observation.participant_id = local.node_id;
  observation.participant_type = ParticipantType::NAMO_NODE;
  observation.site_id = SITE_ID;
  observation.group_id = GROUP_ID;
  observation.boot_id = "boot-local";
  observation.sequence = 1;
  observation.ttl_ms = LocalObservation::DEFAULT_TTL_MS;
  observation.pose_version = 1;
  observation.calibration_version = 1;
  observation.track_count = 1;
  observation.presence = true;
  observation.motion = true;
  observation.has_velocity = true;
  observation.has_position_uncertainty = true;
  observation.has_range = true;
  ObservationAdmission admission{};
  require(admission.admit(&projected_sender,
                          &projected_sender_readiness, SITE_ID,
                          observation, GROUP_ID) ==
              ObservationAdmissionResult::OK,
          "indirect Group observation failed canonical admission");

  FusionSnapshot snapshot{};
  snapshot.occupied = true;
  copy_text(snapshot.site_id, sizeof(snapshot.site_id), SITE_ID);
  copy_text(snapshot.leader_node_id, sizeof(snapshot.leader_node_id),
            indirect.node_id);
  copy_text(snapshot.group_id, sizeof(snapshot.group_id), GROUP_ID);
  copy_text(snapshot.boot_id, sizeof(snapshot.boot_id), "boot-indirect");
  snapshot.sequence = 1;
  snapshot.group_term = group->term;
  snapshot.source_sequence = 1;
  snapshot.ttl_ms = FusionSnapshot::DEFAULT_TTL_MS;
  require(evaluate_fusion_snapshot_admission(
              snapshot, indirect.node_id, SITE_ID, &projected_leader,
              &projected_leader_readiness, group) ==
              FusionSnapshotAdmissionResult::OK,
          "indirect Group Leader fusion snapshot failed canonical admission");
}

void three_role_separation_owner_hub_regression() {
  const std::array<const char *, 3> ids{
      "node-20-group", "node-30-owner", "node-10-site"};
  for (const uint32_t outage : {uint32_t{1}, COORDINATOR_OUTAGE_MS}) {
    for (uint32_t failed_mask : {uint32_t{1}, uint32_t{2}, uint32_t{4}}) {
      char scenario[64]{};
      std::snprintf(scenario, sizeof(scenario),
                    "owner-hub mask=0x%02X",
                    static_cast<unsigned>(failed_mask));
      set_context("three-node-owner-hub-failure", scenario, outage);
      FailureCluster<3> cluster(ids, 1, ParticipantTopology::OWNER_HUB);
      char initial_site[ParticipantRecord::MAX_ID_LENGTH + 1]{};
      char initial_group[ParticipantRecord::MAX_ID_LENGTH + 1]{};
      copy_text(initial_site, sizeof(initial_site), cluster.site_leader());
      copy_text(initial_group, sizeof(initial_group), cluster.group_leader());

      std::size_t failed_index = 0;
      while ((failed_mask & (1U << failed_index)) == 0)
        ++failed_index;
      cluster.power_off(failed_index);
      cluster.settle(START_MS + outage);
      if (outage < SiteCoordinator::DEFAULT_LEASE_MS) {
        cluster.assert_expected_roles(initial_site, initial_group);
        cluster.assert_role_consensus(ids.size());
      } else {
        const char *site = cluster.expected_site_leader_from_powered();
        const char *group = cluster.expected_group_leader_from_powered(site);
        cluster.assert_expected_roles(site, group);
        cluster.assert_role_consensus(cluster.powered_count());
      }
      cluster.assert_configuration_consensus();
      cluster.publish_or_expire_fusion(START_MS + outage, outage);
      cluster.assert_automation(true);

      char rejoin_site[ParticipantRecord::MAX_ID_LENGTH + 1]{};
      char rejoin_group[ParticipantRecord::MAX_ID_LENGTH + 1]{};
      copy_text(rejoin_site, sizeof(rejoin_site),
                outage < SiteCoordinator::DEFAULT_LEASE_MS
                    ? initial_site
                    : cluster.site_leader());
      copy_text(rejoin_group, sizeof(rejoin_group),
                outage < SiteCoordinator::DEFAULT_LEASE_MS
                    ? initial_group
                    : cluster.group_leader());
      cluster.power_on(failed_index);
      cluster.settle(START_MS + outage + 1000);
      cluster.publish_or_expire_fusion(START_MS + outage + 1000, 0);
      cluster.assert_expected_roles(rejoin_site, rejoin_group);
      cluster.assert_role_consensus(ids.size());
      cluster.assert_configuration_consensus();
      cluster.assert_automation(true);
    }
  }
}

void owner_handover_preserves_live_group_runtime_during_credential_skew() {
  set_context("five-node-owner-handover", "credential-skew", 0);
  const std::array<const char *, 5> ids{
      "node-10-owner", "node-20-site", "node-30-group",
      "node-40-follower", "node-50-follower"};
  FailureCluster<5> cluster(ids, 0, ParticipantTopology::FULL_MESH, true);

  char site_leader[ParticipantRecord::MAX_ID_LENGTH + 1]{};
  char group_leader[ParticipantRecord::MAX_ID_LENGTH + 1]{};
  copy_text(site_leader, sizeof(site_leader), cluster.site_leader());
  copy_text(group_leader, sizeof(group_leader), cluster.group_leader());
  const uint32_t group_term = cluster.group_term();
  require(std::strcmp(cluster.configuration_owner(), ids[0]) == 0 &&
              std::strcmp(site_leader, ids[0]) != 0 &&
              std::strcmp(group_leader, ids[0]) != 0,
          "owner handover regression fixture did not separate all roles");

  cluster.set_group_credential_readiness(4, false);
  set_context("five-node-owner-handover", "owner-offline", 0);
  cluster.power_off(0);
  const uint32_t expired_at =
      START_MS + GroupCoordinator::DEFAULT_LEASE_MS + 1;
  for (uint32_t now_ms = START_MS + GroupCoordinator::HEARTBEAT_INTERVAL_MS;
       now_ms < expired_at;
       now_ms += GroupCoordinator::HEARTBEAT_INTERVAL_MS)
    cluster.tick(now_ms);
  cluster.tick(expired_at);
  cluster.assert_expected_roles(site_leader, group_leader);
  require(cluster.group_term() == group_term,
          "owner outage changed a surviving Group Leader term");
  cluster.publish_or_expire_fusion(expired_at, 0);
  cluster.assert_automation(true);

  const uint32_t handover_at =
      expired_at + AutomaticConfigurationHandoverTracker::STABILITY_MS;
  set_context("five-node-owner-handover", "owner-commit", handover_at);
  cluster.perform_owner_handover(expired_at, handover_at);
  require(std::strcmp(cluster.group_leader(), group_leader) == 0,
          "owner commit changed an independent Group Leader");
  require(cluster.group_term() == group_term,
          "owner commit changed an independent Group Leader term");

  cluster.set_group_credential_readiness(0, false);
  set_context("five-node-owner-handover", "former-owner-rejoin",
              handover_at + 1000);
  cluster.power_on(0);
  const uint32_t rejoined_at = handover_at + 1000;
  cluster.settle(rejoined_at);
  require(std::strcmp(cluster.group_leader(), group_leader) == 0,
          "former Owner rejoin changed an independent Group Leader");
  require(cluster.group_term() == group_term,
          "credential-incomplete former Owner changed Group authority");
  cluster.publish_or_expire_fusion(rejoined_at, 0);
  cluster.assert_automation(true);

  cluster.set_group_credential_readiness(0, true);
  cluster.set_group_credential_readiness(4, true);
  set_context("five-node-owner-handover", "settled", rejoined_at + 1000);
  cluster.assert_roles_remain_stable(rejoined_at + 1000);
  cluster.assert_configuration_consensus();
}

void group_leader_replacement_advances_term_exactly_once() {
  set_context("five-node-group-replacement", "term", 0);
  const std::array<const char *, 5> ids{
      "node-10-owner", "node-20-site", "node-30-group",
      "node-40-follower", "node-50-follower"};
  FailureCluster<5> cluster(ids, 0, ParticipantTopology::FULL_MESH, true);

  char initial_site[ParticipantRecord::MAX_ID_LENGTH + 1]{};
  char initial_group[ParticipantRecord::MAX_ID_LENGTH + 1]{};
  copy_text(initial_site, sizeof(initial_site), cluster.site_leader());
  copy_text(initial_group, sizeof(initial_group), cluster.group_leader());
  const uint32_t initial_term = cluster.group_term();
  require(std::strcmp(initial_site, initial_group) != 0 &&
              std::strcmp(cluster.configuration_owner(), initial_group) != 0,
          "Group replacement fixture did not separate authority roles");

  std::size_t failed_index = ids.size();
  for (std::size_t index = 0; index < ids.size(); ++index) {
    if (std::strcmp(ids[index], initial_group) == 0) {
      failed_index = index;
      break;
    }
  }
  require(failed_index < ids.size(),
          "Group replacement fixture could not find its Leader");
  cluster.power_off(failed_index);
  const uint32_t replaced_at =
      START_MS + GroupCoordinator::DEFAULT_LEASE_MS + 1;
  cluster.settle(replaced_at);
  const char *replacement =
      cluster.expected_group_leader_from_powered(cluster.site_leader());
  cluster.assert_expected_roles(initial_site, replacement);
  cluster.assert_role_consensus(cluster.powered_count());
  require(cluster.group_term() == next_role_term(initial_term),
          "one Group Leader replacement did not advance the term exactly once");
  cluster.assert_roles_remain_stable(replaced_at + 1000);
  require(cluster.group_term() == next_role_term(initial_term),
          "a settled replacement amplified the Group term");
}

void leader_eligibility_rollout_preserves_existing_authority() {
  const std::array<const char *, 5> ids{
      "node-10-owner", "node-20-site", "node-30-group",
      "node-40-follower", "node-50-follower"};
  const std::array<std::array<std::size_t, 4>, 2> rollout_orders{{
      {{1, 2, 3, 4}},
      {{4, 3, 2, 1}},
  }};

  for (std::size_t rollout = 0; rollout < rollout_orders.size(); ++rollout) {
    set_context("five-node-eligibility-rollout", "canonical-transition",
                rollout);
    FailureCluster<5> cluster(ids, 0, ParticipantTopology::FULL_MESH, true);

    cluster.converge_leader_eligibility(4, false, START_MS + 1000);
    char site_leader[ParticipantRecord::MAX_ID_LENGTH + 1]{};
    char group_leader[ParticipantRecord::MAX_ID_LENGTH + 1]{};
    copy_text(site_leader, sizeof(site_leader), cluster.site_leader());
    copy_text(group_leader, sizeof(group_leader), cluster.group_leader());
    const uint32_t site_term = cluster.site_term();
    const uint32_t group_term = cluster.group_term();

    cluster.begin_leader_eligibility_rollout(4, true);
    uint32_t now_ms = START_MS + 2000;
    for (uint8_t round = 0; round < 3; ++round) {
      cluster.tick(now_ms++);
      cluster.assert_expected_roles(site_leader, group_leader);
      cluster.assert_role_consensus(ids.size());
    }

    for (const std::size_t index : rollout_orders[rollout]) {
      set_context("five-node-eligibility-rollout", "configuration",
                  rollout * 10 + index);
      cluster.replicate_canonical_configuration(index);
      for (uint8_t round = 0; round < 3; ++round) {
        cluster.tick(now_ms++);
        cluster.assert_expected_roles(site_leader, group_leader);
        cluster.assert_role_consensus(ids.size());
      }
    }

    for (const std::size_t index : rollout_orders[rollout]) {
      set_context("five-node-eligibility-rollout", "directory",
                  rollout * 10 + index);
      cluster.replicate_canonical_directory(index);
      cluster.tick(now_ms++);
      cluster.assert_expected_roles(site_leader, group_leader);
      cluster.assert_role_consensus(ids.size());
    }
    cluster.replicate_canonical_directory(0);
    cluster.settle(now_ms);
    cluster.assert_configuration_consensus();
    cluster.assert_expected_roles(site_leader, group_leader);
    cluster.assert_role_consensus(ids.size());
    require(cluster.site_term() == site_term,
            "leader eligibility rollout changed the Site term");
    require(cluster.group_term() == group_term,
            "leader eligibility rollout changed the Group term");
    cluster.publish_or_expire_fusion(now_ms, 0);
    cluster.assert_automation(true);
  }
}

template <std::size_t N>
void group_leader_handoff_preserves_bounded_automation(
    const char *topology, const std::array<const char *, N> &ids) {
  set_context(topology, "canonical-preparation", 20000);
  FailureCluster<N> cluster(ids, 0, ParticipantTopology::FULL_MESH, true);

  char owner[ParticipantRecord::MAX_ID_LENGTH + 1]{};
  char site[ParticipantRecord::MAX_ID_LENGTH + 1]{};
  char previous_group[ParticipantRecord::MAX_ID_LENGTH + 1]{};
  copy_text(owner, sizeof(owner), cluster.configuration_owner());
  copy_text(site, sizeof(site), cluster.site_leader());
  copy_text(previous_group, sizeof(previous_group), cluster.group_leader());

  std::size_t failed_index = ids.size();
  for (std::size_t index = 0; index < ids.size(); ++index) {
    if (std::strcmp(ids[index], previous_group) == 0) {
      failed_index = index;
      break;
    }
  }
  require(failed_index < ids.size(),
          "bounded handoff fixture could not find its Group Leader");
  cluster.power_off(failed_index);

  const uint32_t handoff_at =
      START_MS + GroupCoordinator::DEFAULT_LEASE_MS + 1;
  cluster.settle(handoff_at);
  const char *replacement =
      cluster.expected_group_leader_from_powered(cluster.site_leader());
  cluster.assert_expected_roles(site, replacement);
  require(std::strcmp(cluster.configuration_owner(), owner) == 0,
          "Group handoff changed an independent Configuration Owner");

  cluster.preserve_group_handoff_continuity(
      handoff_at, previous_group,
      FusionAutomationState::CONTINUITY_HOLD_MS);
  cluster.preserve_group_handoff_continuity(
      handoff_at + 20000, previous_group,
      FusionAutomationState::CONTINUITY_HOLD_MS - 20000);

  cluster.publish_or_expire_fusion(handoff_at + 20001, 0);
  cluster.assert_automation(true);
  cluster.assert_expected_roles(site, replacement);
  require(std::strcmp(cluster.configuration_owner(), owner) == 0,
          "canonical Group completion changed an independent Owner");
}

void five_and_seven_node_group_handoffs_preserve_automation() {
  const std::array<const char *, 5> five_ids{
      "node-10-owner", "node-20-site", "node-30-group",
      "node-40-follower", "node-50-follower"};
  const std::array<const char *, 7> seven_ids{
      "node-10-owner", "node-20-site", "node-30-group",
      "node-40-follower", "node-50-follower", "node-60-follower",
      "node-70-follower"};
  group_leader_handoff_preserves_bounded_automation(
      "five-node-group-continuity", five_ids);
  group_leader_handoff_preserves_bounded_automation(
      "seven-node-group-continuity", seven_ids);
}

void site_leader_replacement_preserves_group_authority() {
  set_context("five-node-site-replacement", "role-affinity", 0);
  const std::array<const char *, 5> ids{
      "node-10-owner", "node-20-site", "node-30-group",
      "node-40-follower", "node-50-follower"};
  FailureCluster<5> cluster(ids, 0, ParticipantTopology::FULL_MESH, true);

  char initial_site[ParticipantRecord::MAX_ID_LENGTH + 1]{};
  char initial_group[ParticipantRecord::MAX_ID_LENGTH + 1]{};
  copy_text(initial_site, sizeof(initial_site), cluster.site_leader());
  copy_text(initial_group, sizeof(initial_group), cluster.group_leader());
  const uint32_t initial_group_term = cluster.group_term();
  require(std::strcmp(initial_site, initial_group) != 0,
          "Site replacement fixture did not separate Leader roles");

  std::size_t failed_index = ids.size();
  for (std::size_t index = 0; index < ids.size(); ++index) {
    if (std::strcmp(ids[index], initial_site) == 0) {
      failed_index = index;
      break;
    }
  }
  require(failed_index < ids.size(),
          "Site replacement fixture could not find its Leader");
  cluster.power_off(failed_index);
  const uint32_t replaced_at =
      START_MS + SiteCoordinator::DEFAULT_LEASE_MS + 1;
  cluster.settle(replaced_at);
  const char *replacement = cluster.expected_site_leader_from_powered();
  cluster.assert_expected_roles(replacement, initial_group);
  cluster.assert_role_consensus(cluster.powered_count());
  require(cluster.group_term() == initial_group_term,
          "Site Leader replacement changed an independent Group term");
  cluster.assert_roles_remain_stable(replaced_at + 1000);
  require(cluster.group_term() == initial_group_term,
          "settled Site replacement amplified the Group term");
}

}  // namespace

void run_role_failure_matrix_tests() {
  owner_handover_preserves_live_group_runtime_during_credential_skew();
  leader_eligibility_rollout_preserves_existing_authority();
  site_leader_replacement_preserves_group_authority();
  group_leader_replacement_advances_term_exactly_once();
  five_and_seven_node_group_handoffs_preserve_automation();
  all_supported_topologies_and_failure_subsets();
  packet_loss_role_matrix();
  complete_power_cycles_recover_canonical_roles();
  repeated_short_role_flaps_recover_without_owner_change();
  every_role_failure_order_recovers();
  sequential_role_failures_remain_bounded_and_fail_closed();
  persisted_directory_fence_blocks_a_stale_rebooted_site_leader();
  equal_revision_directory_split_brain_converges_after_partition();
  all_owner_hub_sizes_carry_indirect_group_runtime();
  three_node_owner_hub_indirect_runtime_regression();
  three_role_separation_owner_hub_regression();
}
