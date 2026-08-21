#include "participant_discovery.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace esphome::radar_api_server;

namespace {

void require(bool condition, const char *message) {
  if (condition)
    return;
  std::fprintf(stderr, "participant_discovery_tests: %s\n", message);
  std::exit(1);
}

bool consider(ParticipantDiscoveryResults &results, const char *participant_id,
              uint8_t protocol_minimum = 1, uint8_t protocol_maximum = 1) {
  return results.consider(participant_id, "namo_node",
                          "namo-spatial-sensor", "namo-peer",
                          "192.168.1.50", 80, protocol_minimum,
                          protocol_maximum, true);
}

void filters_untrusted_or_incompatible_advertisements() {
  ParticipantDiscoveryResults results;
  results.reset("namo-node-local");
  require(!consider(results, "namo-node-local"),
          "the local node must not discover itself");
  require(!results.consider("namo-node-peer", "satellite",
                            "namo-spatial-sensor", "peer", "192.168.1.2",
                            80, 1, 1, true),
          "non-NAMO participant types must be ignored");
  require(!results.consider("namo-node-peer", "namo_node", "other-product",
                            "peer", "192.168.1.2", 80, 1, 1, true),
          "wrong products must be ignored");
  require(!results.consider("namo-node-peer", "namo_node",
                            "namo-spatial-sensor", "peer", "192.168.1.2",
                            80, 2, 2, true),
          "incompatible protocol ranges must be ignored");
  require(!results.consider("namo-node-peer", "namo_node",
                            "namo-spatial-sensor", "peer", "192.168.1.2",
                            80, 1, 1, false),
          "identities that are not ready must be ignored");
  require(results.size() == 0,
          "rejected advertisements must not consume candidate slots");
}

void deduplicates_and_enforces_the_site_bound() {
  ParticipantDiscoveryResults results;
  results.reset("namo-node-local");
  char participant_id[32]{};
  for (std::size_t index = 0;
       index < ParticipantRegistry::MAX_REMOTE_PARTICIPANTS; ++index) {
    std::snprintf(participant_id, sizeof(participant_id), "namo-node-%02u",
                  static_cast<unsigned>(index));
    require(consider(results, participant_id),
            "every bounded remote slot should accept one compatible node");
  }
  require(results.size() == ParticipantRegistry::MAX_REMOTE_PARTICIPANTS,
          "discovery must use the same nineteen-remote-node limit as the Site");
  require(!consider(results, "namo-node-overflow"),
          "the twentieth remote discovery result must be rejected");

  results.reset("namo-node-local");
  require(consider(results, "namo-node-peer"),
          "a compatible peer should be accepted");
  require(!consider(results, "namo-node-peer"),
          "duplicate participant identities must not create duplicate buttons");
  require(results.size() == 1, "deduplication must preserve one candidate");
}

void updates_the_address_for_an_existing_stable_identity() {
  ParticipantDiscoveryResults results;
  results.reset("namo-node-local");
  require(consider(results, "namo-node-peer"),
          "the initial address should be accepted");
  require(results.consider("namo-node-peer", "namo_node",
                           "namo-spatial-sensor", "namo-peer-new",
                           "192.168.1.73", 80, 1, 1, true),
          "a DHCP address change should update the existing identity");
  require(results.size() == 1,
          "an address change must not create a second candidate");
  const auto *candidate = results.candidate_at(0);
  require(candidate != nullptr &&
              std::strcmp(candidate->address, "192.168.1.73") == 0 &&
              std::strcmp(candidate->hostname, "namo-peer-new") == 0,
          "the latest endpoint metadata should replace the old address");
}

void configures_advertisement_only_from_a_stable_identity() {
  ParticipantDiscoveryRuntime runtime;
  require(!runtime.advertisement_configured(),
          "discovery advertising must wait for platform identity");
  require(!runtime.configure_advertisement(""),
          "an empty platform identity must not be advertised");
  require(!runtime.configure_advertisement("namo node invalid"),
          "an invalid platform identity must not be advertised");
  require(runtime.configure_advertisement("namo-node-peer"),
          "a stable NAMO node identity should configure advertising");
  require(runtime.advertisement_configured(),
          "advertising should be ready after identity configuration");
  require(std::strcmp(runtime.advertised_participant_id(), "namo-node-peer") == 0,
          "advertising must retain the exact stable node identity");
  require(runtime.configure_advertisement("namo-node-peer"),
          "repeating the same identity should be idempotent");
  require(!runtime.configure_advertisement("namo-node-other"),
          "a configured runtime must not switch identities in place");
}

void scan_windows_are_bounded_and_retryable() {
  ParticipantDiscoveryRuntime runtime;
  require(runtime.configure_advertisement("namo-node-local"),
          "scan timing requires a configured local identity");
  require(runtime.queue_scan("namo-node-local"),
          "the first scan request should be accepted");
  require(runtime.queue_scan("namo-node-local"),
          "a repeated request during a scan should be idempotent");

  runtime.loop(100);
  require(runtime.snapshot().scanning,
          "a scan should remain open during its bounded collection window");
  runtime.loop(3099);
  require(runtime.snapshot().scanning,
          "a scan must not complete before its deadline");
  runtime.loop(3100);
  require(!runtime.snapshot().scanning,
          "a scan must clear its busy state at the deadline");
  require(runtime.snapshot().result == ParticipantDiscoveryResult::START_FAILED,
          "a host runtime without mDNS must report a bounded start failure");
  require(runtime.queue_scan("namo-node-local"),
          "a completed scan must allow a later retry");
}

void visible_scan_promotes_an_active_hidden_refresh() {
  ParticipantDiscoveryRuntime runtime;
  require(runtime.configure_advertisement("namo-node-local"),
          "hidden refresh promotion requires a configured identity");
  require(runtime.queue_refresh("namo-node-local"),
          "the hidden endpoint refresh should be queued");
  runtime.loop(100);
  require(!runtime.snapshot().scanning,
          "a hidden endpoint refresh must stay out of the UI");

  require(runtime.queue_scan("namo-node-local"),
          "a visible scan should join the active hidden refresh");
  require(runtime.snapshot().scanning &&
              runtime.snapshot().result == ParticipantDiscoveryResult::IDLE,
          "the joined refresh must immediately become visible");
  runtime.loop(3100);
  require(!runtime.snapshot().scanning &&
              runtime.snapshot().result ==
                  ParticipantDiscoveryResult::START_FAILED,
          "the promoted scan must publish its terminal result");
}

}  // namespace

void run_participant_discovery_tests() {
  filters_untrusted_or_incompatible_advertisements();
  deduplicates_and_enforces_the_site_bound();
  updates_the_address_for_an_existing_stable_identity();
  configures_advertisement_only_from_a_stable_identity();
  scan_windows_are_bounded_and_retryable();
  visible_scan_promotes_an_active_hidden_refresh();
}
