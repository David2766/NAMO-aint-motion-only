#pragma once

#include "dashboard_handler.h"
#include "control_handler.h"
#include "control_state.h"
#include "device_config_cache.h"
#include "device_config_handler.h"
#include "diagnostic_handler.h"
#include "diagnostic_log.h"
#include "floorplan_handler.h"
#include "firmware_update_handler.h"
#include "group_coordinator.h"
#include "group_roster.h"
#include "legacy_presence.h"
#include "ld2410_stream_health.h"
#include "ld2450_startup_guard.h"
#include "local_observation.h"
#include "observation_transport.h"
#include "participant_credentials.h"
#include "participant_discovery.h"
#include "participant_enrollment.h"
#include "participant_pairing.h"
#include "participant_pairing_runtime.h"
#include "participant_readiness.h"
#include "platform_identity_store.h"
#include "platform_operation.h"
#include "platform_runtime_workspace.h"
#include "participant_registry_store.h"
#include "platform_handler.h"
#include "pairing_handler.h"
#include "presence_fusion.h"
#include "presence_tracker.h"
#include "presence_replay_log.h"
#include "radar_storage.h"
#include "setup_handler.h"
#include "site_coordinator.h"
#include "site_configuration.h"
#include "site_directory.h"
#include "site_heartbeat.h"
#include "site_replication.h"
#include "software_zone_evidence.h"
#include "spatial_production_fusion.h"
#include "spatial_shadow_runtime.h"
#include "state_json_builder.h"
#include "state_handler.h"
#include "stats_store.h"
#include "stats_handler.h"
#include "system_handler.h"
#include "timezone_catalog.h"
#include "esphome/core/component.h"
#include "esphome/components/ld2410/ld2410.h"
#include "esphome/components/ld2450/ld2450.h"
#include "esphome/components/uart/uart_component.h"
#include "esphome/components/web_server_base/web_server_base.h"

#include <functional>
#include <array>
#include <cstring>
#include <string>

namespace esphome {
namespace radar_api_server {

class RadarApiServer : public Component, public AsyncWebHandler {
 public:
  explicit RadarApiServer(web_server_base::WebServerBase *base);

  ~RadarApiServer() override;

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override;

  void set_ld2450(ld2450::LD2450Component *radar) { this->ld2450_ = radar; }
  void set_ld2410(ld2410::LD2410Component *radar) { this->ld2410_ = radar; }
  void set_ld2410_uart(uart::UARTComponent *uart) {
    this->ld2410_uart_ = uart;
  }
  bool ld2410_stream_fresh(uint32_t now_ms) const {
    if (this->ld2410_uart_ == nullptr)
      return false;
    const auto status = this->ld2410_stream_health_.snapshot(now_ms);
    return status.valid_frame_seen &&
           status.last_frame_age_ms < Ld2410StreamHealth::RUNTIME_STALE_MS;
  }
  bool ld2410_engineering_mode_active(uint32_t now_ms) const {
    if (!this->ld2410_stream_fresh(now_ms))
      return false;
    return this->ld2410_stream_health_.snapshot(now_ms).engineering_mode;
  }

  bool canHandle(AsyncWebServerRequest *request) const override;
  void handleRequest(AsyncWebServerRequest *request) override;
  void handleUpload(AsyncWebServerRequest *request,
                    const std::string &filename, size_t index, uint8_t *data,
                    size_t len, bool final) override;
  bool isRequestHandlerTrivial() const override { return false; }
  void record_httpd_stack_watermark(const char *endpoint,
                                    const char *phase = "handler");
  void record_internal_ram_watermark(const char *source,
                                     const char *phase = "complete");
  void record_internal_ram_checkpoint(const char *source,
                                      const char *phase);
  void copy_internal_ram_watermark(
      InternalRamWatermarkDiagnostics *output) const;
  const char *httpd_stack_lowest_endpoint() const {
    return this->httpd_stack_lowest_endpoint_;
  }
  const char *httpd_stack_lowest_phase() const {
    return this->httpd_stack_lowest_phase_;
  }
  uint32_t httpd_stack_lowest_endpoint_free_bytes() const {
    return this->httpd_stack_lowest_free_bytes_ == UINT32_MAX
               ? 0
               : this->httpd_stack_lowest_free_bytes_;
  }

  RadarStorage &storage() { return this->storage_; }
  StatePublisherDiagnostics state_publisher_diagnostics() const {
    return this->state_handler_.diagnostics();
  }
  TargetZonePolicy target_zone_policy(float target_x, float target_y) const {
    return compute_target_zone_policy(this->device_config_cache_, target_x,
                                      target_y);
  }
  bool tracker_assist_presence_enabled() const {
    const auto config = this->device_config_cache_.read();
    return config->tracker_assist_presence_enabled();
  }
  bool legacy_presence_fallback_enabled() const {
    const auto config = this->device_config_cache_.read();
    return config->legacy_presence_fallback_enabled();
  }
  void update_today_stats_json(const std::string &json) { this->stats_store_.update_today(json); }
  void record_heatmap_hit(float tx, float ty) { this->stats_store_.record_heatmap_hit(tx, ty); }
  bool save_finished_stats_day(const std::string &finished_day_json, const std::string &new_today_json) {
    return this->stats_store_.finish_day(finished_day_json, new_today_json);
  }
  bool reset_today_stats(const std::string &new_today_json) {
    return this->stats_store_.reset_today(new_today_json);
  }
  void update_device_state_json(DeviceStateJsonInput input);
  void update_presence_tracker(uint32_t now_ms, bool pir_motion, bool filter_blocked, bool stable_presence,
                               bool room_context_configured,
                               const StaticRadarEvidence &static_radar, float illuminance_lux,
                               float stationary_speed_cm_s,
                               bool target_1_valid, bool target_1_filter_blocked,
                               float target_1_x, float target_1_y,
                               float target_1_speed, float target_1_distance, bool target_1_reduced_zone,
                               int target_1_exit_zone_mask,
                               bool target_1_room_inside, bool target_1_room_outside,
                               float target_1_room_signed_distance,
                               bool target_2_valid, bool target_2_filter_blocked,
                               float target_2_x, float target_2_y, float target_2_speed,
                               float target_2_distance, bool target_2_reduced_zone, int target_2_exit_zone_mask,
                               bool target_2_room_inside, bool target_2_room_outside,
                               float target_2_room_signed_distance,
                               bool target_3_valid, bool target_3_filter_blocked,
                               float target_3_x, float target_3_y, float target_3_speed,
                               float target_3_distance, bool target_3_reduced_zone, int target_3_exit_zone_mask,
                               bool target_3_room_inside, bool target_3_room_outside,
                               float target_3_room_signed_distance);
  void update_legacy_presence(uint32_t now_ms, bool pir_motion, bool filter_blocked, int target_count,
                              int moving_target_count, int still_target_count, float stationary_speed_cm_s,
                              bool target_1_valid, float target_1_x, float target_1_y, float target_1_speed,
                              bool target_2_valid, float target_2_x, float target_2_y, float target_2_speed,
                              bool target_3_valid, float target_3_x, float target_3_y, float target_3_speed);
  void disable_legacy_presence() { this->legacy_presence_.disable(); }
  const LegacyPresenceOutput &legacy_presence_output() const { return this->legacy_presence_.output(); }
  void update_presence_replay_raw_targets(bool target_1_raw_valid, float target_1_x, float target_1_y,
                                          float target_1_speed, float target_1_distance, int target_1_filter_mode,
                                          bool target_1_filtered, bool target_1_range_valid,
                                          bool target_2_raw_valid, float target_2_x, float target_2_y,
                                          float target_2_speed, float target_2_distance, int target_2_filter_mode,
                                          bool target_2_filtered, bool target_2_range_valid,
                                          bool target_3_raw_valid, float target_3_x, float target_3_y,
                                          float target_3_speed, float target_3_distance, int target_3_filter_mode,
                                          bool target_3_filtered, bool target_3_range_valid);
  void update_software_zone_evidence(uint32_t now_ms, bool target_1_valid, float target_1_x, float target_1_y,
                                     bool target_2_valid, float target_2_x, float target_2_y,
                                     bool target_3_valid, float target_3_x, float target_3_y);
  SoftwareZoneEvidence software_zone_evidence_for_targets(uint32_t now_ms, bool target_1_valid, float target_1_x,
                                                          float target_1_y, bool target_2_valid, float target_2_x,
                                                          float target_2_y, bool target_3_valid, float target_3_x,
                                                          float target_3_y) const;
  const SoftwareZoneEvidence &software_zone_evidence() const { return this->software_zone_evidence_; }
  bool consume_ld2450_tracker_frame() { return this->ld2450_frame_gate_.consume_latest(); }
  std::string tracker_debug_json() const { return this->presence_tracker_.debug_json(); }
  const PresenceTrackerOutput &tracker_output() const { return this->presence_tracker_.output(); }
  const PresenceFusionOutput &presence_fusion_output() const { return this->presence_fusion_.output(); }
  const LocalObservation &local_observation() const { return this->local_observation_adapter_.output(); }
  bool platform_identity_ready() const {
    return this->platform_identity_store_result_ == PlatformIdentityStoreResult::LOADED ||
           this->platform_identity_store_result_ == PlatformIdentityStoreResult::CREATED ||
           this->platform_identity_store_result_ == PlatformIdentityStoreResult::MIGRATED ||
           this->platform_identity_store_result_ == PlatformIdentityStoreResult::MARKER_REPAIRED;
  }
  const PlatformIdentity &platform_identity() const { return this->platform_identity_; }
  const SiteMembership &platform_site_membership() const {
    return this->platform_site_membership_;
  }
  bool participant_registry_ready() const {
    return this->participant_registry_store_result_ == ParticipantRegistryStoreResult::LOADED ||
           this->participant_registry_store_result_ == ParticipantRegistryStoreResult::CREATED ||
           this->participant_registry_store_result_ == ParticipantRegistryStoreResult::MIGRATED ||
           this->participant_registry_store_result_ == ParticipantRegistryStoreResult::UPDATED ||
           this->participant_registry_store_result_ ==
               ParticipantRegistryStoreResult::COMMIT_REPAIRED;
  }
  const ParticipantRegistrySnapshot &participant_registry_snapshot() const {
    return this->platform_runtime_->participant_registry_snapshot;
  }
  ParticipantRegistrySnapshot &mutable_participant_registry_snapshot() {
    return this->platform_runtime_->participant_registry_snapshot;
  }
  ParticipantEnrollment &participant_enrollment() { return this->participant_enrollment_; }
  const ParticipantEnrollment &participant_enrollment() const {
    return this->participant_enrollment_;
  }
  PlatformParticipantOperation &participant_operation() {
    return this->participant_operation_;
  }
  bool participant_registry_persistence_ready() const {
    return this->participant_registry_ready() &&
           this->participant_registry_backend_ != nullptr &&
           this->participant_registry_store_ != nullptr;
  }
  ParticipantRegistryStorageBackend *participant_registry_backend() {
    return this->participant_registry_backend_;
  }
  ParticipantRegistryStore *participant_registry_store() {
    return this->participant_registry_store_;
  }
  bool site_configuration_ready() const {
    return this->site_configuration_store_result_ ==
               SiteConfigurationStoreResult::LOADED ||
           this->site_configuration_store_result_ ==
               SiteConfigurationStoreResult::CREATED ||
           this->site_configuration_store_result_ ==
               SiteConfigurationStoreResult::UPDATED ||
           this->site_configuration_store_result_ ==
               SiteConfigurationStoreResult::COMMIT_REPAIRED;
  }
  bool site_configuration_persistence_ready() const {
    return this->site_configuration_ready() &&
           this->site_configuration_backend_ != nullptr &&
           this->site_configuration_store_ != nullptr;
  }
  bool group_roster_ready() const {
    const bool stored =
        this->group_roster_store_result_ == GroupRosterStoreResult::LOADED ||
        this->group_roster_store_result_ == GroupRosterStoreResult::CREATED ||
        this->group_roster_store_result_ == GroupRosterStoreResult::MIGRATED ||
        this->group_roster_store_result_ == GroupRosterStoreResult::UPDATED ||
        this->group_roster_store_result_ ==
            GroupRosterStoreResult::COMMIT_REPAIRED;
    return stored && this->platform_runtime_ != nullptr &&
           this->platform_identity_ready() &&
           this->site_configuration_ready() &&
           group_roster_current_for_local_node(
               this->platform_runtime_->group_roster_snapshot,
               this->platform_runtime_->site_configuration_snapshot,
               this->platform_identity_.node_id);
  }
  bool site_directory_ready() const {
    return this->site_directory_stored_() &&
           this->site_configuration_ready() &&
           site_directory_matches_configuration_scope(
               this->platform_runtime_->site_configuration_snapshot,
               this->platform_runtime_->site_directory_snapshot);
  }
  bool canonical_site_directory_ready() const {
    return this->site_directory_stored_() &&
           this->site_configuration_ready() &&
           site_directory_is_canonical_for_configuration(
               this->platform_runtime_->site_configuration_snapshot,
               this->platform_runtime_->site_directory_snapshot);
  }
  const SiteConfigurationSnapshot &site_configuration_snapshot() const {
    return this->platform_runtime_->site_configuration_snapshot;
  }
  const GroupRosterSnapshot &group_roster_snapshot() const {
    return this->platform_runtime_->group_roster_snapshot;
  }
  GroupRosterStoreResult group_roster_store_result() const {
    return this->group_roster_store_result_;
  }
  SiteConfigurationSnapshot &mutable_site_configuration_snapshot() {
    return this->platform_runtime_->site_configuration_snapshot;
  }
  SiteConfigurationStorageBackend *site_configuration_backend() {
    return this->site_configuration_backend_;
  }
  SiteConfigurationStore *site_configuration_store() {
    return this->site_configuration_store_;
  }
  SiteConfigurationSnapshot &site_configuration_update_scratch() {
    return this->platform_runtime_->site_configuration_update_scratch;
  }
  bool refresh_owned_site_roster(
      const char *spatial_profile_participant_id = nullptr,
      bool resolve_automatic_group = false);
  bool clear_owned_floorplan_manifest();
  bool reset_platform_relationships();
  bool participant_readiness_ready() const {
    return this->participant_readiness_store_result_ ==
               ParticipantReadinessStoreResult::LOADED ||
           this->participant_readiness_store_result_ ==
               ParticipantReadinessStoreResult::CREATED ||
           this->participant_readiness_store_result_ ==
               ParticipantReadinessStoreResult::MIGRATED ||
           this->participant_readiness_store_result_ ==
               ParticipantReadinessStoreResult::UPDATED ||
           this->participant_readiness_store_result_ ==
               ParticipantReadinessStoreResult::COMMIT_REPAIRED;
  }
  const ParticipantReadinessSnapshot &participant_readiness_snapshot() const {
    return this->platform_runtime_->participant_readiness_snapshot;
  }
  ParticipantReadinessSnapshot &mutable_participant_readiness_snapshot() {
    return this->platform_runtime_->participant_readiness_snapshot;
  }
  bool participant_readiness_persistence_ready() const {
    return this->participant_readiness_ready() &&
           this->participant_readiness_backend_ != nullptr &&
           this->participant_readiness_store_ != nullptr;
  }
  ParticipantReadinessStorageBackend *participant_readiness_backend() {
    return this->participant_readiness_backend_;
  }
  ParticipantReadinessStore *participant_readiness_store() {
    return this->participant_readiness_store_;
  }
  bool participant_credentials_ready() const {
    return this->participant_credential_store_result_ ==
               ParticipantCredentialStoreResult::LOADED ||
           this->participant_credential_store_result_ ==
               ParticipantCredentialStoreResult::CREATED ||
           this->participant_credential_store_result_ ==
               ParticipantCredentialStoreResult::UPDATED ||
           this->participant_credential_store_result_ ==
               ParticipantCredentialStoreResult::COMMIT_REPAIRED;
  }
  bool participant_credential_persistence_ready() const {
    return this->participant_credentials_ready() &&
           this->participant_credential_backend_ != nullptr &&
           this->participant_credential_store_ != nullptr;
  }
  const ParticipantCredentialSnapshot &participant_credential_snapshot() const {
    return this->participant_credential_snapshot_;
  }
  ParticipantCredentialSnapshot &mutable_participant_credential_snapshot() {
    return this->participant_credential_snapshot_;
  }
  bool disable_participant_credential(const char *participant_id);
  bool enable_participant_credential(const char *participant_id);
  bool erase_participant_credential(const char *participant_id);
  bool restore_participant_credentials(
      const ParticipantCredentialRegistry &credentials);
  ParticipantPairingCommitResult commit_participant_pairing(
      const ParticipantPairingCredential &credential,
      ParticipantPairingRole local_role, const char *leader_site_id,
      uint32_t now_ms);
  StandaloneSiteReconciliationState standalone_site_reconciliation_state()
      const {
    return this->standalone_site_reconciliation_.state();
  }
  StandaloneSiteReconciliationResult standalone_site_reconciliation_result()
      const {
    return this->standalone_site_reconciliation_.result();
  }
  bool standalone_site_reconciliation_ready_for_pairing() const {
    return this->standalone_site_reconciliation_.ready_for_pairing();
  }
  ParticipantPairingRuntime &participant_pairing_runtime() {
    return this->participant_pairing_runtime_;
  }
  ParticipantDiscoveryRuntime &participant_discovery_runtime() {
    return this->participant_discovery_runtime_;
  }
  const GroupCoordinator &group_coordinator() const;
  const SiteCoordinator &site_coordinator() const;
  PlatformCoordinatorSnapshotLease acquire_coordinator_snapshot();
  void release_coordinator_snapshot(
      const PlatformCoordinatorSnapshotLease &lease);
  const SiteGroupSummaryCache &site_group_summaries() const {
    return this->platform_runtime_->site_group_summaries;
  }
  const SiteSpaceSummaryCache &site_space_summaries() const {
    return this->platform_runtime_->site_space_summaries;
  }
  const SiteDirectorySnapshot &site_directory_snapshot() const {
    return this->platform_runtime_->site_directory_snapshot;
  }
  const ParticipantEndpoint *participant_endpoint(
      const char *participant_id) const {
#ifdef USE_ESP32
    return this->site_heartbeat_ != nullptr
               ? this->site_heartbeat_->endpoints().find(participant_id)
               : nullptr;
#else
    (void) participant_id;
    return nullptr;
#endif
  }
  const SiteHeartbeatDiagnostics *site_heartbeat_diagnostics() const {
#ifdef USE_ESP32
    return this->site_heartbeat_ != nullptr
               ? &this->site_heartbeat_->diagnostics()
               : nullptr;
#else
    return nullptr;
#endif
  }
  const SiteReplicationDiagnostics *site_replication_diagnostics() const {
#ifdef USE_ESP32
    return this->site_replication_ != nullptr
               ? &this->site_replication_->diagnostics()
               : nullptr;
#else
    return nullptr;
#endif
  }
  const SiteReplicationSourceDiagnostics *
  site_replication_source_diagnostics() const {
#ifdef USE_ESP32
    return this->site_replication_ != nullptr
               ? &this->site_replication_->source_diagnostics()
               : nullptr;
#else
    return nullptr;
#endif
  }
  uint32_t site_replication_source_lease_remaining_ms(
      uint32_t now_ms) const {
#ifdef USE_ESP32
    return this->site_replication_ != nullptr
               ? this->site_replication_->source_lease_remaining_ms(now_ms)
               : 0;
#else
    (void) now_ms;
    return 0;
#endif
  }
  uint32_t auto_replication_pending_generation() const {
    return this->platform_runtime_ != nullptr
               ? this->platform_runtime_->site_authority_replication
                     .pending_generation()
               : 0;
  }
  uint32_t auto_replication_pending_authority_epoch() const {
    return this->platform_runtime_ != nullptr
               ? this->platform_runtime_->site_authority_replication
                     .pending_authority_epoch()
               : 0;
  }
  uint8_t auto_replication_retry_count() const {
    return this->platform_runtime_ != nullptr
               ? this->platform_runtime_->site_authority_replication
                     .retry_count()
               : 0;
  }
  uint32_t auto_replication_wait_remaining_ms(uint32_t now_ms) const {
    return this->platform_runtime_ != nullptr
               ? this->platform_runtime_->site_authority_replication
                     .wait_remaining_ms(now_ms)
               : 0;
  }
  bool auto_directory_replication_active() const {
    return this->auto_directory_replication_active_;
  }
  uint32_t auto_directory_replication_wait_remaining_ms(
      uint32_t now_ms) const {
    if (this->auto_directory_replication_due_ms_ == 0 ||
        static_cast<int32_t>(
            now_ms - this->auto_directory_replication_due_ms_) >= 0)
      return 0;
    return this->auto_directory_replication_due_ms_ - now_ms;
  }
  uint8_t auto_directory_replication_retry_count() const {
    return this->auto_directory_replication_retry_count_;
  }
  const AutomaticConfigurationHandoverTracker &
  automatic_configuration_handover() const {
    return this->automatic_configuration_handover_;
  }
  const SiteConfigurationMember *automatic_configuration_successor(
      uint32_t now_ms) const;
  const SiteAuthoritySnapshot &site_authority_snapshot(uint32_t now_ms);
  bool shared_site_configuration_writable(uint32_t now_ms);
  SiteReplicationResult start_site_replication(const char *owner_node_id);
  void cancel_site_replication() {
#ifdef USE_ESP32
    this->reset_configuration_replication_();
    if (this->site_replication_ != nullptr)
      this->site_replication_->cancel();
#endif
  }
#ifdef USE_ESP32
  ParticipantPairingCrypto &participant_pairing_crypto() {
    return this->participant_pairing_crypto_;
  }
#endif
  const ObservationTransportDiagnostics *observation_transport_diagnostics()
      const {
#ifdef USE_ESP32
    return this->observation_transport_ != nullptr
               ? &this->observation_transport_->diagnostics()
               : nullptr;
#else
    return nullptr;
#endif
  }
  const SpatialShadowRuntimeDiagnostics *spatial_shadow_runtime_diagnostics()
      const {
#ifdef USE_ESP32
    return this->spatial_shadow_runtime_ != nullptr
               ? &this->spatial_shadow_runtime_->diagnostics()
               : nullptr;
#else
    return nullptr;
#endif
  }
  const SpatialShadowFusionOutput *spatial_shadow_fusion_output() const {
#ifdef USE_ESP32
    return this->spatial_shadow_runtime_ != nullptr
               ? &this->spatial_shadow_runtime_->output()
               : nullptr;
#else
    return nullptr;
#endif
  }
  const SpatialProductionFusionOutput &spatial_production_fusion_output()
      const;
  const SpatialProductionFusionOutput &spatial_automation_fusion_output(
      uint32_t now_ms);
  const FusionAutomationSnapshot &fusion_automation_snapshot(
      uint32_t now_ms);
  bool production_fusion_build_enabled() const {
#ifdef NAMO_PRODUCTION_FUSION
    return true;
#else
    return false;
#endif
  }

  // Setup Wi-Fi uses ESP-IDF directly so provisioning can keep the setup AP alive
  // while validating the selected STA network. ESPHome is synced only at finish/handoff.
  void connect_setup_wifi_deferred(std::string ssid, std::string password);
  void pause_setup_sta_deferred();
  void close_setup_ap_deferred();
  void close_setup_ap_after(uint32_t delay_ms, std::function<void()> before_close = {});
  void finish_setup_scan_after(uint32_t delay_ms);
  void finish_setup_scan_and_open_ap_after(uint32_t delay_ms);
  void open_setup_ap_deferred();
  void schedule_setup_auto_reboot();
  void reboot_deferred();
  void reboot_after(uint32_t delay_ms);
  void complete_setup_handoff_after(uint32_t delay_ms);
  void reset_wifi_credentials_and_reboot_after(uint32_t delay_ms);
  void update_control_status(bool status_led_enabled, float led_blink_duration, bool environment_correction_enabled,
                             float temperature_offset, float humidity_offset, const std::string &timezone) {
    this->control_state_.status_led_enabled = status_led_enabled;
    this->control_state_.has_status_led_enabled = true;
    this->control_state_.led_blink_duration = led_blink_duration;
    this->control_state_.has_led_blink_duration = true;
    this->control_state_.environment_correction_enabled = environment_correction_enabled;
    this->control_state_.has_environment_correction_enabled = true;
    this->control_state_.temperature_offset = temperature_offset;
    this->control_state_.has_temperature_offset = true;
    this->control_state_.humidity_offset = humidity_offset;
    this->control_state_.has_humidity_offset = true;
    this->control_state_.timezone = timezone;
    this->control_state_.has_timezone = true;
  }
  void request_general_settings_reset() {
    this->control_state_.requested_status_led_enabled = true;
    this->control_state_.pending_status_led_enabled = true;
    this->control_state_.status_led_enabled = true;
    this->control_state_.has_status_led_enabled = true;
    this->control_state_.requested_led_blink_duration = 60.0f;
    this->control_state_.pending_led_blink_duration = true;
    this->control_state_.led_blink_duration = 60.0f;
    this->control_state_.has_led_blink_duration = true;
    this->control_state_.requested_environment_correction_enabled = true;
    this->control_state_.pending_environment_correction_enabled = true;
    this->control_state_.environment_correction_enabled = true;
    this->control_state_.has_environment_correction_enabled = true;
    this->control_state_.requested_temperature_offset = 0.0f;
    this->control_state_.pending_temperature_offset = true;
    this->control_state_.temperature_offset = 0.0f;
    this->control_state_.has_temperature_offset = true;
    this->control_state_.requested_humidity_offset = 0.0f;
    this->control_state_.pending_humidity_offset = true;
    this->control_state_.humidity_offset = 0.0f;
    this->control_state_.has_humidity_offset = true;
    this->control_state_.requested_timezone = "Asia/Seoul";
    this->control_state_.pending_timezone = true;
    this->control_state_.timezone = "Asia/Seoul";
    this->control_state_.has_timezone = true;
  }
  bool take_status_led_request(bool *enabled) {
    if (!this->control_state_.pending_status_led_enabled)
      return false;
    this->control_state_.pending_status_led_enabled = false;
    *enabled = this->control_state_.requested_status_led_enabled;
    return true;
  }
  bool take_led_blink_duration_request(float *seconds) {
    if (!this->control_state_.pending_led_blink_duration)
      return false;
    this->control_state_.pending_led_blink_duration = false;
    *seconds = this->control_state_.requested_led_blink_duration;
    return true;
  }
  bool take_environment_correction_request(bool *enabled) {
    if (!this->control_state_.pending_environment_correction_enabled)
      return false;
    this->control_state_.pending_environment_correction_enabled = false;
    *enabled = this->control_state_.requested_environment_correction_enabled;
    return true;
  }
  bool take_temperature_offset_request(float *value) {
    if (!this->control_state_.pending_temperature_offset)
      return false;
    this->control_state_.pending_temperature_offset = false;
    *value = this->control_state_.requested_temperature_offset;
    return true;
  }
  bool take_humidity_offset_request(float *value) {
    if (!this->control_state_.pending_humidity_offset)
      return false;
    this->control_state_.pending_humidity_offset = false;
    *value = this->control_state_.requested_humidity_offset;
    return true;
  }
  bool take_timezone_request(std::string *timezone) {
    if (!this->control_state_.pending_timezone)
      return false;
    this->control_state_.pending_timezone = false;
    *timezone = this->control_state_.requested_timezone;
    return true;
  }
  bool take_static_radar_tuning_session_request(uint32_t now_ms, bool *active) {
    constexpr uint32_t tuning_timeout_ms = 15000;
    if (this->control_state_.static_radar_tuning_requested_active &&
        now_ms - this->control_state_.static_radar_tuning_keepalive_ms >= tuning_timeout_ms) {
      this->control_state_.static_radar_tuning_requested_active = false;
      this->control_state_.static_radar_tuning_keepalive_ms = 0;
      this->control_state_.pending_static_radar_tuning_session = true;
      this->control_state_.static_radar_tuning_phase = StaticRadarTuningPhase::STOPPING;
      this->control_state_.static_radar_tuning_result = "session_expired";
    }
    if (!this->control_state_.pending_static_radar_tuning_session)
      return false;
    this->control_state_.pending_static_radar_tuning_session = false;
    *active = this->control_state_.static_radar_tuning_requested_active;
    return true;
  }
  bool take_static_radar_gate_request(uint8_t *gate, uint8_t *sensitivity) {
    return this->control_state_.take_static_radar_gate_request(gate, sensitivity);
  }
  bool request_static_radar_gate_write(uint8_t gate, uint8_t sensitivity,
                                       uint32_t now_ms);
  bool verify_static_radar_gate_readback(uint32_t now_ms, float move_threshold, float still_threshold) {
    return this->control_state_.verify_static_radar_gate_readback(now_ms, move_threshold, still_threshold);
  }
  bool static_radar_gate_verification_pending(uint8_t *gate) const {
    return this->control_state_.static_radar_gate_verification_pending(gate);
  }
  bool take_static_radar_factory_reset_request() {
    return this->control_state_.take_static_radar_factory_reset_request();
  }
  void update_static_radar_tuning_status(
      bool available, bool active, uint16_t resolution_mm,
      const std::array<float, ControlState::STATIC_RADAR_GATE_COUNT> &move_energy,
      const std::array<float, ControlState::STATIC_RADAR_GATE_COUNT> &still_energy,
      const std::array<float, ControlState::STATIC_RADAR_GATE_COUNT> &move_threshold,
      const std::array<float, ControlState::STATIC_RADAR_GATE_COUNT> &still_threshold) {
    this->control_state_.static_radar_available = available;
    if (this->ld2410_recovery_requested_ || this->ld2410_recovery_active_ ||
        this->ld2410_recovery_settling_) {
      this->control_state_.static_radar_recovering = true;
      this->control_state_.static_radar_tuning_active = false;
      this->control_state_.static_radar_tuning_phase =
          StaticRadarTuningPhase::FAILED;
      this->control_state_.static_radar_tuning_result = "sensor_recovering";
      return;
    }
    this->control_state_.static_radar_recovering = false;
    this->control_state_.static_radar_tuning_active = available && active;
    if (this->control_state_.static_radar_tuning_active) {
      this->control_state_.static_radar_tuning_phase =
          this->control_state_.static_radar_tuning_requested_active
              ? StaticRadarTuningPhase::ACTIVE
              : StaticRadarTuningPhase::STOPPING;
      this->control_state_.static_radar_tuning_result =
          this->control_state_.static_radar_tuning_requested_active
              ? "ok"
              : "pending";
    } else if (!this->control_state_.static_radar_tuning_requested_active &&
               this->control_state_.static_radar_tuning_phase !=
                   StaticRadarTuningPhase::FAILED) {
      this->control_state_.static_radar_tuning_phase = StaticRadarTuningPhase::IDLE;
      this->control_state_.static_radar_tuning_result = "idle";
    }
    this->control_state_.static_radar_gate_resolution_mm = resolution_mm;
    if (!this->control_state_.static_radar_tuning_active)
      return;
    for (size_t gate = 0; gate < ControlState::STATIC_RADAR_GATE_COUNT; gate++) {
      this->control_state_.static_radar_gates[gate] = {
          move_energy[gate], still_energy[gate], move_threshold[gate], still_threshold[gate]};
    }
  }
  void update_diagnostic_snapshot(bool presence, bool motion, bool pir_motion, int target_count,
                                  int moving_target_count, int still_target_count, int still_confidence,
                                  bool still_hold_active, int empty_samples, int range_suspect_count,
                                  int range_out_of_range_count, int range_remote_candidate_count,
                                  float illuminance_lux, const char *presence_reason,
                                  const char *presence_off_reason, const char *motion_reason,
                                  const char *still_state, const char *still_reason,
                                  const char *range_reason, bool exit_zone_active, int exit_zone_mask,
                                  int exit_target_count, int exit_last_seen_age_ms);

 protected:
  static constexpr uint32_t STANDALONE_SITE_RECONCILIATION_DELAY_MS = 1000;
  static constexpr uint32_t SPATIAL_READINESS_CHECK_INTERVAL_MS = 250;
  static constexpr uint32_t SITE_DIRECTORY_RECONCILIATION_INTERVAL_MS = 2500;
  static constexpr uint32_t SITE_DIRECTORY_REPLICATION_INTERVAL_MS = 30000;
  static constexpr uint32_t SITE_AUTHORITY_REFRESH_INTERVAL_MS = 250;
  void schedule_standalone_site_reconciliation_(bool cancel_pairing,
                                                uint32_t now_ms);
  void process_standalone_site_reconciliation_(uint32_t now_ms);
  void process_site_directory_reconciliation_(uint32_t now_ms);
  void process_automatic_site_directory_replication_(uint32_t now_ms);
  void process_automatic_credential_provisioning_(uint32_t now_ms);
  bool site_credential_identity_ready_() const;
  bool site_directory_stored_() const {
    return this->platform_runtime_ != nullptr &&
           (this->site_directory_store_result_ ==
                SiteDirectoryStoreResult::LOADED ||
            this->site_directory_store_result_ ==
                SiteDirectoryStoreResult::CREATED ||
            this->site_directory_store_result_ ==
                SiteDirectoryStoreResult::UPDATED ||
            this->site_directory_store_result_ ==
                SiteDirectoryStoreResult::COMMIT_REPAIRED);
  }
  bool reconcile_site_directory_scope_(const char *trigger);
  void reset_site_scoped_runtime_(const char *trigger);
  void process_spatial_readiness_invalidation_(uint32_t now_ms);
  void process_automatic_configuration_handover_(uint32_t now_ms);
  bool reconcile_group_roster_(const char *trigger);
  void refresh_site_authority_(uint32_t now_ms);
  void send_ld2410_recovery_command_(Ld2410RecoveryAction action);
  void send_ld2410_command_(uint8_t command, const uint8_t *payload,
                            uint8_t payload_size);
  bool process_ld2410_recovery_(uint32_t now_ms,
                                const Ld2410StreamSnapshot &status,
                                bool command_busy);
  void quarantine_ld2410_requests_();
  void process_ld2410_command_transaction_(
      uint32_t now_ms, const Ld2410StreamSnapshot &status);
  void finish_ld2410_command_transaction_(
      const Ld2410ManagedAction &action, uint32_t now_ms);
  bool reconcile_owned_spatial_assignments_(const char *trigger);
  void update_spatial_shadow_runtime_(uint32_t now_ms);
  void refresh_fusion_automation_(uint32_t now_ms);
  void refresh_shared_automation_areas_(const char *space_id);
  void publish_coordinator_snapshot_();
  bool allocate_platform_runtime_();
  void destroy_platform_runtime_();
  bool allocate_spatial_shadow_runtime_();
  void destroy_spatial_shadow_runtime_();
  bool automatic_handover_credentials_complete_() const;
  StandaloneSiteReconciliationResult reconcile_standalone_site_();
  void reset_configuration_replication_() {
    if (this->platform_runtime_ != nullptr)
      this->platform_runtime_->site_authority_replication.reset();
  }
  web_server_base::WebServerBase *base_;
  RadarStorage storage_;
  DeviceConfigCache device_config_cache_;
  StatsStore stats_store_;
  ControlState control_state_;
  DiagnosticLog diagnostic_log_;
  PresenceTracker presence_tracker_;
  PresenceFusion presence_fusion_;
  LocalObservationAdapter local_observation_adapter_;
  uart::UARTComponent *ld2410_uart_{nullptr};
  ld2410::LD2410Component *ld2410_{nullptr};
  Ld2410StreamHealth ld2410_stream_health_{};
  Ld2410CommandTransaction ld2410_command_transaction_{};
  Ld2410Watchdog ld2410_watchdog_{};
  uint32_t ld2410_startup_stable_since_ms_{0};
  uint32_t ld2410_startup_retry_after_ms_{0};
  bool ld2410_startup_sync_complete_{false};
  bool ld2410_tuning_request_pending_{false};
  bool ld2410_tuning_requested_active_{false};
  uint32_t ld2410_tuning_retry_after_ms_{0};
  uint8_t ld2410_tuning_retry_count_{0};
  bool ld2410_recovery_active_{false};
  bool ld2410_recovery_settling_{false};
  bool ld2410_recovery_requested_{false};
  uint32_t ld2410_recovery_settle_started_ms_{0};
  uint32_t ld2410_recovery_settle_frame_baseline_{0};
  ld2450::LD2450Component *ld2450_{nullptr};
  Ld2450StartupGuard ld2450_startup_guard_{};
  Ld2450FrameGate ld2450_frame_gate_{};
  PlatformIdentity platform_identity_{};
  SiteMembership platform_site_membership_{};
  PlatformIdentityStoreResult platform_identity_store_result_{PlatformIdentityStoreResult::EMPTY};
  ParticipantRegistryStoreResult participant_registry_store_result_{
      ParticipantRegistryStoreResult::EMPTY};
  ParticipantRegistryStorageBackend *participant_registry_backend_{nullptr};
  ParticipantRegistryStore *participant_registry_store_{nullptr};
  SiteConfigurationStoreResult site_configuration_store_result_{
      SiteConfigurationStoreResult::EMPTY};
  SiteConfigurationStorageBackend *site_configuration_backend_{nullptr};
  SiteConfigurationStore *site_configuration_store_{nullptr};
  GroupRosterStoreResult group_roster_store_result_{
      GroupRosterStoreResult::EMPTY};
  GroupRosterStorageBackend *group_roster_backend_{nullptr};
  GroupRosterStore *group_roster_store_{nullptr};
  SiteDirectoryStoreResult site_directory_store_result_{
      SiteDirectoryStoreResult::EMPTY};
  SiteDirectoryStorageBackend *site_directory_backend_{nullptr};
  SiteDirectoryStore *site_directory_store_{nullptr};
  SiteDirectoryResult site_directory_reconciliation_result_{
      SiteDirectoryResult::NOT_READY};
  PlatformRuntimeWorkspace *platform_runtime_{nullptr};
  bool platform_runtime_in_psram_{false};
  StandaloneSiteReconciliationTracker standalone_site_reconciliation_{};
  AutomaticConfigurationHandoverTracker
      automatic_configuration_handover_{};
  ParticipantReadinessStoreResult participant_readiness_store_result_{
      ParticipantReadinessStoreResult::EMPTY};
  ParticipantReadinessStorageBackend *participant_readiness_backend_{nullptr};
  ParticipantReadinessStore *participant_readiness_store_{nullptr};
  ParticipantCredentialSnapshot participant_credential_snapshot_{};
  ParticipantCredentialStoreResult participant_credential_store_result_{
      ParticipantCredentialStoreResult::EMPTY};
  ParticipantCredentialStorageBackend *participant_credential_backend_{nullptr};
  ParticipantCredentialStore *participant_credential_store_{nullptr};
  ParticipantEnrollment participant_enrollment_{};
  PlatformParticipantOperation participant_operation_{};
  ParticipantDiscoveryRuntime participant_discovery_runtime_{};
  ParticipantPairingRuntime participant_pairing_runtime_{};
  uint32_t coordinator_last_update_ms_{0};
  uint32_t spatial_shadow_last_update_ms_{0};
  uint32_t spatial_readiness_check_ms_{0};
  uint32_t site_directory_reconciliation_check_ms_{0};
  uint32_t credential_provisioning_check_ms_{0};
  uint32_t credential_provisioning_authority_epoch_{0};
  uint32_t credential_provisioning_roster_revision_{0};
  uint32_t credential_provisioning_attempted_source_mask_{0};
  char credential_provisioning_owner_node_id_[
      PlatformIdentity::MAX_ID_LENGTH + 1]{};
  bool credential_provisioning_verified_{false};
  bool credential_provisioning_refresh_pending_{false};
  CredentialProvisioningSourceReadiness credential_provisioning_wait_reason_{
      CredentialProvisioningSourceReadiness::READY};
  uint32_t auto_directory_replication_due_ms_{0};
  uint8_t auto_directory_replication_retry_count_{0};
  bool auto_directory_replication_active_{false};
  char auto_directory_replication_source_node_id_[
      ParticipantRecord::MAX_ID_LENGTH + 1]{};
  uint32_t floorplan_replica_check_ms_{0};
  bool local_floorplan_replica_ready_{false};
  bool local_floorplan_stored_{false};
  uint32_t local_floorplan_config_hash_{0};
  uint32_t local_floorplan_image_hash_{0};
#ifdef USE_ESP32
  Esp32ParticipantPairingCrypto participant_pairing_crypto_{};
  ObservationTransport *observation_transport_{nullptr};
  bool observation_transport_in_psram_{false};
  SpatialShadowRuntime *spatial_shadow_runtime_{nullptr};
  bool spatial_shadow_runtime_in_psram_{false};
  SiteHeartbeatTransport *site_heartbeat_{nullptr};
  bool site_heartbeat_in_psram_{false};
  SiteReplicationTransport *site_replication_{nullptr};
  bool site_replication_in_psram_{false};
#ifdef NAMO_SIGNED_OTA
  FirmwareUpdateHandler *firmware_update_handler_{nullptr};
  bool firmware_update_handler_in_psram_{false};
#endif
#endif
  LegacyPresence legacy_presence_;
  PresenceTrackerInput last_tracker_input_{};
  PresenceReplayRawInput last_replay_raw_input_{};
  PresenceReplayLog presence_replay_log_;
  SoftwareZoneEvidence software_zone_evidence_{};
  uint32_t exit_zone_last_seen_ms_{0};
  DashboardHandler dashboard_handler_;
  FloorplanHandler floorplan_handler_;
  DeviceConfigHandler device_config_handler_;
  StatsHandler stats_handler_;
  PlatformHandler platform_handler_;
  PairingHandler pairing_handler_;
  SystemHandler system_handler_;
  StateHandler state_handler_;
  DiagnosticHandler diagnostic_handler_;
  ControlHandler control_handler_;
  SetupHandler setup_handler_;
  // ESP-IDF's HTTP server dispatches handlers on one httpd task. Reuse one
  // URL buffer so the 513-byte decoded path does not remain on that task's
  // stack while an endpoint builds and sends its response.
  mutable std::array<char, AsyncWebServerRequest::URL_BUF_SIZE> request_url_buffer_{};
  uint32_t httpd_stack_lowest_free_bytes_{UINT32_MAX};
  const char *httpd_stack_lowest_endpoint_{"/unobserved"};
  const char *httpd_stack_lowest_phase_{"unobserved"};
  bool ha_setup_handoff_started_{false};
};

}  // namespace radar_api_server
}  // namespace esphome
