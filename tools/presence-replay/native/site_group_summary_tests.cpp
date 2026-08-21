#include "site_group_summary.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace esphome::radar_api_server;

namespace {

void require(bool condition, const char *message) {
  if (condition)
    return;
  std::fprintf(stderr, "site_group_summary_tests: %s\n", message);
  std::exit(1);
}

SiteDirectorySnapshot directory_with_groups(std::size_t group_count) {
  SiteDirectorySnapshot directory{};
  std::strcpy(directory.site_id, "site-test");
  directory.generation = 1;
  directory.topology_revision = 1;
  directory.committed = true;
  for (std::size_t index = 0; index < group_count; ++index) {
    auto &group = directory.groups[index];
    group.occupied = true;
    std::snprintf(group.group_id, sizeof(group.group_id), "group-%u",
                  static_cast<unsigned>(index));
    std::snprintf(group.floorplan_id, sizeof(group.floorplan_id),
                  "floorplan-%u", static_cast<unsigned>(index));
    std::snprintf(group.leader_node_id, sizeof(group.leader_node_id),
                  "leader-%u", static_cast<unsigned>(index));
    group.assignment_revision = static_cast<uint32_t>(index + 1);
    group.member_count = 7;
  }
  return directory;
}

SiteGroupSummaryReport report_for(std::size_t index) {
  static char participant_ids[SiteDirectoryLimits::MAX_GROUPS][24]{};
  static char group_ids[SiteDirectoryLimits::MAX_GROUPS][24]{};
  std::snprintf(participant_ids[index], sizeof(participant_ids[index]),
                "leader-%u", static_cast<unsigned>(index));
  std::snprintf(group_ids[index], sizeof(group_ids[index]), "group-%u",
                static_cast<unsigned>(index));
  SiteGroupSummaryReport report{};
  report.participant_id = participant_ids[index];
  report.group_id = group_ids[index];
  report.assignment_revision = static_cast<uint32_t>(index + 1);
  report.term = static_cast<uint32_t>(index + 1);
  report.lease_ms = 6000;
  report.participants_expected = 7;
  report.participants_online = 6;
  return report;
}

void accepts_only_the_directory_leader_and_exact_assignment() {
  const auto directory = directory_with_groups(1);
  SiteGroupSummaryCache cache{};
  auto report = report_for(0);
  require(cache.accept(directory, report, 1000) ==
              SiteGroupSummaryResult::OK,
          "the assigned Group Leader summary should be accepted");
  const auto *summary = cache.find("group-0");
  require(summary != nullptr &&
              std::strcmp(summary->floorplan_id, "floorplan-0") == 0 &&
              summary->participants_expected == 7 &&
              summary->participants_online == 6 &&
              SiteGroupSummaryCache::lease_remaining_ms(*summary, 2000) ==
                  5000,
          "accepted summaries should retain only bounded Group metadata");

  report.participant_id = "other-leader";
  require(cache.accept(directory, report, 2000) ==
              SiteGroupSummaryResult::LEADER_MISMATCH,
          "a non-assigned sender must not replace the Group summary");
  report = report_for(0);
  report.term = 9;
  require(cache.accept(directory, report, 2000) ==
              SiteGroupSummaryResult::OK &&
              cache.find("group-0")->term == 9,
          "runtime Group term must be independent from assignment revision");
  report = report_for(0);
  report.assignment_revision = 2;
  report.term = 2;
  require(cache.accept(directory, report, 2000) ==
              SiteGroupSummaryResult::REVISION_MISMATCH,
          "a stale or future assignment must wait for the Site directory");
  report = report_for(0);
  report.participants_expected = 6;
  require(cache.accept(directory, report, 2000) ==
              SiteGroupSummaryResult::MEMBER_COUNT_MISMATCH,
          "the report cannot redefine the configured Group size");
}

void bounds_the_site_to_eight_live_group_summaries() {
  const auto directory =
      directory_with_groups(SiteDirectoryLimits::MAX_GROUPS);
  SiteGroupSummaryCache cache{};
  for (std::size_t index = 0; index < SiteDirectoryLimits::MAX_GROUPS;
       ++index) {
    require(cache.accept(directory, report_for(index), 1000) ==
                SiteGroupSummaryResult::OK,
            "all eight directory Groups should fit in the summary cache");
  }
  require(cache.size() == SiteDirectoryLimits::MAX_GROUPS,
          "the summary cache must stop at the Site Group limit");

  auto unknown = report_for(0);
  unknown.group_id = "group-8";
  unknown.participant_id = "leader-8";
  unknown.assignment_revision = 9;
  unknown.term = 9;
  require(cache.accept(directory, unknown, 1000) ==
              SiteGroupSummaryResult::UNKNOWN_GROUP,
          "a ninth undeclared Group must be rejected before allocation");
}

void expires_reports_and_clears_state_on_site_leader_handover() {
  const auto directory = directory_with_groups(1);
  SiteGroupSummaryCache cache{};
  require(cache.accept(directory, report_for(0), 1000) ==
              SiteGroupSummaryResult::OK,
          "expiry fixture should be accepted");
  cache.reconcile(&directory, true, 6999);
  require(cache.size() == 1,
          "a summary should remain live until its lease expires");
  cache.reconcile(&directory, true, 7000);
  require(cache.size() == 0 && cache.expired() == 1,
          "an expired Group summary must be removed from RAM");

  require(cache.accept(directory, report_for(0), 8000) ==
              SiteGroupSummaryResult::OK,
          "handover fixture should be accepted");
  cache.reconcile(&directory, false, 8001);
  require(cache.size() == 0,
          "a node that is no longer Site Leader must drop the cache");
}

SiteConfigurationSnapshot handover_configuration() {
  SiteConfigurationSnapshot configuration{};
  std::strcpy(configuration.site_id, "site-test");
  std::strcpy(configuration.configuration_owner_node_id, "leader-old");
  configuration.generation = 4;
  configuration.roster_revision = 3;
  configuration.roster_committed = true;
  const char *participants[] = {"leader-old", "leader-new"};
  for (std::size_t index = 0; index < 2; ++index) {
    auto &member = configuration.members[index];
    member.occupied = true;
    std::strcpy(member.participant_id, participants[index]);
    member.type = ParticipantType::NAMO_NODE;
    member.membership = ParticipantMembership::ACTIVE;
    member.leader_eligible = true;
    member.spatial_profile.committed = true;
    std::strcpy(member.spatial_profile.space_id, "space-0");
    std::strcpy(member.spatial_profile.group_id, "group-0");
  }
  return configuration;
}

void preserves_a_newer_authenticated_leader_until_directory_promotion() {
  auto directory = directory_with_groups(1);
  std::strcpy(directory.groups[0].leader_node_id, "leader-old");
  directory.groups[0].assignment_revision = 2;
  directory.groups[0].member_count = 2;
  const auto configuration = handover_configuration();

  SiteGroupSummaryReport report{};
  report.participant_id = "leader-new";
  report.group_id = "group-0";
  report.assignment_revision = 1;
  report.term = 1;
  report.lease_ms = 6000;
  report.participants_expected = 2;
  report.participants_online = 2;

  SiteGroupSummaryCache cache{};
  require(cache.accept_discovered(
              configuration, &directory, "leader-new", "floorplan-0",
              report, 1000) == SiteGroupSummaryResult::OK,
          "a newer deterministic Group Leader report should be admitted");
  cache.reconcile(&directory, true, 1001);
  const auto *pending = cache.find("group-0");
  require(pending != nullptr && pending->assignment_revision == 3 &&
              pending->term == 1 &&
              std::strcmp(pending->leader_node_id, "leader-new") == 0,
          "the Site Leader must assign a newer persistent revision to a "
          "restarted runtime Leader");

  std::strcpy(directory.groups[0].leader_node_id, "leader-new");
  directory.groups[0].assignment_revision = 3;
  cache.reconcile(&directory, true, 1002);
  require(cache.size() == 1,
          "the promoted Group Leader report should remain canonical");

  SiteGroupSummaryCache restarted{};
  report.assignment_revision = 1;
  report.term = 1;
  require(restarted.accept_discovered(
              configuration, &directory, "leader-new", "floorplan-0",
              report, 2000) == SiteGroupSummaryResult::OK,
          "the assigned Leader must be admitted after a full reboot");
  const auto *resumed = restarted.find("group-0");
  require(resumed != nullptr && resumed->assignment_revision == 3 &&
              resumed->term == 1,
          "a same-Leader reboot must retain the persistent assignment while "
          "accepting the restarted runtime term");
}

}  // namespace

void run_site_group_summary_tests() {
  accepts_only_the_directory_leader_and_exact_assignment();
  bounds_the_site_to_eight_live_group_summaries();
  expires_reports_and_clears_state_on_site_leader_handover();
  preserves_a_newer_authenticated_leader_until_directory_promotion();
}
