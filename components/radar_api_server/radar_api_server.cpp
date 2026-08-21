#include "radar_api_server.h"

#include "http_response.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cmath>
#include <new>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>

#ifdef USE_ESP32
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

namespace esphome {
namespace radar_api_server {

static const char *const TAG = "radar_api_server";

namespace {

bool has_non_revoked_participants(const ParticipantRegistry &participants) {
  return participants.connected_count() > 0;
}

bool http_path_starts_with(StringRef url, const char *prefix) {
  const std::size_t prefix_length = std::strlen(prefix);
  return url.size() >= prefix_length &&
         std::equal(url.begin(), url.begin() + prefix_length, prefix);
}

const char *classify_httpd_request(StringRef url) {
  if (url == "/dashboard" || url == "/dashboard/")
    return "/dashboard";
  if (url == "/dashboard/assets/dashboard.js")
    return "/dashboard/assets/dashboard.js";
  if (url == "/dashboard/assets/dashboard.css")
    return "/dashboard/assets/dashboard.css";
  if (url == "/" || url == "/setup" ||
      http_path_starts_with(url, "/api/setup/"))
    return "/setup";
  if (url == "/api/system/status")
    return "/api/system/status";
  if (http_path_starts_with(url, "/api/system/"))
    return "/api/system";
  if (url == "/api/state")
    return "/api/state";
  if (url == "/api/config")
    return "/api/config";
  if (http_path_starts_with(url, "/api/config/"))
    return "/api/config";
  if (url == "/api/floorplan/status")
    return "/api/floorplan/status";
  if (http_path_starts_with(url, "/api/floorplan"))
    return "/api/floorplan";
  if (url == "/api/platform/participants")
    return "/api/platform/participants";
  if (url == "/api/platform/groups")
    return "/api/platform/groups";
  if (url == "/api/platform/site-configuration")
    return "/api/platform/site-configuration";
  if (url == "/api/platform/pairing")
    return "/api/platform/pairing";
  if (url == "/api/platform/discovery")
    return "/api/platform/discovery";
  if (url == "/api/platform/descriptor")
    return "/api/platform/descriptor";
  if (url == "/api/stats" || http_path_starts_with(url, "/api/stats/"))
    return "/api/stats";
  if (http_path_starts_with(url, "/api/diagnostics/"))
    return "/api/diagnostics";
  if (http_path_starts_with(url, "/api/control/"))
    return "/api/control";
  return "/other";
}

#ifdef USE_ESP32
enum class RuntimeAllocationLogLevel : uint8_t {
  INFO = 0,
  WARN,
  ERROR,
};

portMUX_TYPE internal_ram_diagnostics_mux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE coordinator_snapshot_mux = portMUX_INITIALIZER_UNLOCKED;

uint8_t internal_ram_threshold_level(uint32_t free_bytes) {
  if (free_bytes <= 40U * 1024U)
    return 4;
  if (free_bytes <= 48U * 1024U)
    return 3;
  if (free_bytes <= 56U * 1024U)
    return 2;
  if (free_bytes <= 64U * 1024U)
    return 1;
  return 0;
}

bool site_replication_is_active(SiteReplicationState state) {
  return state != SiteReplicationState::IDLE &&
         state != SiteReplicationState::COMPLETE &&
         state != SiteReplicationState::CONFLICT &&
         state != SiteReplicationState::FAILED;
}

void log_runtime_allocation(const char *component, const char *region,
                            std::size_t size,
                            RuntimeAllocationLogLevel level) {
  const auto internal_free =
      heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const auto internal_largest = heap_caps_get_largest_free_block(
      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const auto psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  const auto psram_largest =
      heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
  if (level == RuntimeAllocationLogLevel::ERROR) {
    ESP_LOGE(TAG,
             "Runtime allocation component=%s region=%s size=%u "
             "internal_free=%u internal_largest=%u psram_free=%u "
             "psram_largest=%u",
             component, region, static_cast<unsigned>(size),
             static_cast<unsigned>(internal_free),
             static_cast<unsigned>(internal_largest),
             static_cast<unsigned>(psram_free),
             static_cast<unsigned>(psram_largest));
  } else if (level == RuntimeAllocationLogLevel::WARN) {
    ESP_LOGW(TAG,
             "Runtime allocation component=%s region=%s size=%u "
             "internal_free=%u internal_largest=%u psram_free=%u "
             "psram_largest=%u",
             component, region, static_cast<unsigned>(size),
             static_cast<unsigned>(internal_free),
             static_cast<unsigned>(internal_largest),
             static_cast<unsigned>(psram_free),
             static_cast<unsigned>(psram_largest));
  } else {
    ESP_LOGI(TAG,
             "Runtime allocation component=%s region=%s size=%u "
             "internal_free=%u internal_largest=%u psram_free=%u "
             "psram_largest=%u",
             component, region, static_cast<unsigned>(size),
             static_cast<unsigned>(internal_free),
             static_cast<unsigned>(internal_largest),
             static_cast<unsigned>(psram_free),
             static_cast<unsigned>(psram_largest));
  }
}

template<typename T, typename... Args>
T *allocate_runtime_object(const char *component, bool *in_psram,
                           Args &&...args) {
  if (in_psram == nullptr)
    return nullptr;
  *in_psram = false;
#ifdef USE_PSRAM
  void *memory =
      heap_caps_calloc(1, sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (memory != nullptr) {
    auto *object = new (memory) T(std::forward<Args>(args)...);
    *in_psram = true;
    log_runtime_allocation(component, "psram", sizeof(T),
                           RuntimeAllocationLogLevel::INFO);
    return object;
  }
  log_runtime_allocation(component, "psram_failed", sizeof(T),
                         RuntimeAllocationLogLevel::WARN);
#endif
  auto *object =
      new (std::nothrow) T(std::forward<Args>(args)...);
  if (object != nullptr) {
#ifdef USE_PSRAM
    log_runtime_allocation(component, "internal_fallback", sizeof(T),
                           RuntimeAllocationLogLevel::WARN);
#else
    log_runtime_allocation(component, "internal", sizeof(T),
                           RuntimeAllocationLogLevel::INFO);
#endif
    return object;
  }
  log_runtime_allocation(component, "allocation_failed", sizeof(T),
                         RuntimeAllocationLogLevel::ERROR);
  return nullptr;
}

template<typename T>
void destroy_runtime_object(T **object, bool *in_psram) {
  if (object == nullptr || *object == nullptr || in_psram == nullptr)
    return;
#ifdef USE_PSRAM
  if (*in_psram) {
    (*object)->~T();
    heap_caps_free(*object);
    *object = nullptr;
    *in_psram = false;
    return;
  }
#endif
  delete *object;
  *object = nullptr;
  *in_psram = false;
}
#endif

}  // namespace

RadarApiServer::RadarApiServer(web_server_base::WebServerBase *base)
    : base_(base),
      floorplan_handler_(this, &storage_),
      device_config_handler_(&storage_, &device_config_cache_),
      stats_handler_(&stats_store_, &storage_),
      platform_handler_(this),
      pairing_handler_(this),
      system_handler_(&storage_, &device_config_cache_, &stats_store_, this),
      diagnostic_handler_(&diagnostic_log_, &presence_replay_log_),
      control_handler_(&control_state_),
      setup_handler_(this) {
  // Template entities may read fusion output before this component's setup().
  this->allocate_platform_runtime_();
}

const GroupCoordinator &RadarApiServer::group_coordinator() const {
  return this->platform_runtime_->group_coordinator;
}

const SiteCoordinator &RadarApiServer::site_coordinator() const {
  return this->platform_runtime_->site_coordinator;
}

PlatformCoordinatorSnapshotLease
RadarApiServer::acquire_coordinator_snapshot() {
  PlatformCoordinatorSnapshotLease lease{};
  if (this->platform_runtime_ == nullptr)
    return lease;
  auto &store = this->platform_runtime_->coordinator_snapshots;
#ifdef USE_ESP32
  portENTER_CRITICAL(&coordinator_snapshot_mux);
#endif
  store.acquire(&lease);
#ifdef USE_ESP32
  portEXIT_CRITICAL(&coordinator_snapshot_mux);
#endif
  return lease;
}

void RadarApiServer::release_coordinator_snapshot(
    const PlatformCoordinatorSnapshotLease &lease) {
  if (this->platform_runtime_ == nullptr || !lease.valid() ||
      lease.buffer_index >= 2)
    return;
  auto &store = this->platform_runtime_->coordinator_snapshots;
#ifdef USE_ESP32
  portENTER_CRITICAL(&coordinator_snapshot_mux);
#endif
  store.release(lease);
#ifdef USE_ESP32
  portEXIT_CRITICAL(&coordinator_snapshot_mux);
#endif
}

void RadarApiServer::publish_coordinator_snapshot_() {
  if (this->platform_runtime_ == nullptr)
    return;
  auto &store = this->platform_runtime_->coordinator_snapshots;
  uint8_t buffer_index = 0;
#ifdef USE_ESP32
  portENTER_CRITICAL(&coordinator_snapshot_mux);
#endif
  if (!store.prepare_publish(&buffer_index)) {
#ifdef USE_ESP32
    portEXIT_CRITICAL(&coordinator_snapshot_mux);
#endif
    return;
  }
#ifdef USE_ESP32
  portEXIT_CRITICAL(&coordinator_snapshot_mux);
#endif

  auto &published = store.snapshots[buffer_index];
  published = {};
  published.site_generation =
      this->platform_runtime_->site_coordinator.generation();
  published.site = this->platform_runtime_->site_coordinator.state();
  published.group_generation =
      this->platform_runtime_->group_coordinator.generation();
  for (std::size_t slot = 0; slot < GroupCoordinator::MAX_GROUPS; ++slot) {
    const auto *group =
        this->platform_runtime_->group_coordinator.group_at(slot);
    if (group == nullptr)
      continue;
    published.groups[published.group_count++] = *group;
  }

#ifdef USE_ESP32
  portENTER_CRITICAL(&coordinator_snapshot_mux);
#endif
  store.commit_publish(buffer_index);
#ifdef USE_ESP32
  portEXIT_CRITICAL(&coordinator_snapshot_mux);
#endif
}

const SpatialProductionFusionOutput &
RadarApiServer::spatial_production_fusion_output() const {
  // Keep pre-setup reads safe even if both PSRAM and internal allocation fail.
  static const SpatialProductionFusionOutput unavailable{};
  if (this->platform_runtime_ == nullptr)
    return unavailable;
  return this->platform_runtime_->spatial_production_fusion.output();
}

const SpatialProductionFusionOutput &
RadarApiServer::spatial_automation_fusion_output(uint32_t now_ms) {
  this->refresh_fusion_automation_(now_ms);
  static const SpatialProductionFusionOutput unavailable{};
  return this->platform_runtime_ != nullptr
             ? this->platform_runtime_->fusion_automation.fusion_output()
             : unavailable;
}

const FusionAutomationSnapshot &RadarApiServer::fusion_automation_snapshot(
    uint32_t now_ms) {
  this->refresh_fusion_automation_(now_ms);
  static const FusionAutomationSnapshot unavailable{};
  return this->platform_runtime_ != nullptr
             ? this->platform_runtime_->fusion_automation.snapshot()
             : unavailable;
}

RadarApiServer::~RadarApiServer() {
#ifdef USE_ESP32
  this->participant_discovery_runtime_.shutdown();
#ifdef NAMO_SIGNED_OTA
  destroy_runtime_object(&this->firmware_update_handler_,
                         &this->firmware_update_handler_in_psram_);
#endif
  this->destroy_spatial_shadow_runtime_();
  destroy_runtime_object(&this->observation_transport_,
                         &this->observation_transport_in_psram_);
  destroy_runtime_object(&this->site_replication_,
                         &this->site_replication_in_psram_);
  destroy_runtime_object(&this->site_heartbeat_,
                         &this->site_heartbeat_in_psram_);
#endif
  this->destroy_platform_runtime_();
}

bool RadarApiServer::allocate_platform_runtime_() {
  if (this->platform_runtime_ != nullptr)
    return true;
#ifdef USE_ESP32
  this->platform_runtime_ =
      allocate_runtime_object<PlatformRuntimeWorkspace>(
          "platform_runtime", &this->platform_runtime_in_psram_);
#else
  this->platform_runtime_ = new (std::nothrow) PlatformRuntimeWorkspace();
  this->platform_runtime_in_psram_ = false;
#endif
  return this->platform_runtime_ != nullptr;
}

void RadarApiServer::destroy_platform_runtime_() {
#ifdef USE_ESP32
  destroy_runtime_object(&this->platform_runtime_,
                         &this->platform_runtime_in_psram_);
#else
  delete this->platform_runtime_;
  this->platform_runtime_ = nullptr;
  this->platform_runtime_in_psram_ = false;
#endif
}

void RadarApiServer::setup() {
#ifdef USE_ESP32
  if (!this->allocate_platform_runtime_()) {
    ESP_LOGE(TAG,
             "Platform runtime unavailable; radar API setup cannot continue");
    this->mark_failed();
    return;
  }
  this->record_internal_ram_watermark("setup", "platform_runtime");
#else
  if (!this->allocate_platform_runtime_())
    return;
#endif
  this->participant_operation_.setup(
      &this->platform_runtime_->participant_operation_workspace);
  if (this->ld2410_uart_ != nullptr) {
    const uint32_t now_ms = millis();
    this->ld2410_stream_health_.reset(now_ms);
    this->ld2410_command_transaction_.reset();
    this->ld2410_watchdog_.reset(now_ms);
    this->ld2410_startup_stable_since_ms_ = 0;
    this->ld2410_startup_retry_after_ms_ = 0;
    this->ld2410_startup_sync_complete_ = false;
    this->ld2410_tuning_request_pending_ = false;
    this->ld2410_tuning_requested_active_ = false;
    this->ld2410_tuning_retry_after_ms_ = 0;
    this->ld2410_tuning_retry_count_ = 0;
    this->ld2410_recovery_active_ = false;
    this->ld2410_recovery_settling_ = false;
    this->ld2410_recovery_requested_ = false;
    this->ld2410_recovery_settle_started_ms_ = 0;
    this->ld2410_recovery_settle_frame_baseline_ = 0;
    if (this->ld2410_ != nullptr) {
      this->ld2410_stream_health_.use_canonical_ack_source();
      this->ld2410_->add_on_ack_callback(
          [this](uint8_t command, uint16_t result) {
            this->ld2410_stream_health_.observe_ack(command, result,
                                                    millis());
            if (result == 0) {
              ESP_LOGD("ld2410_health",
                       "Canonical ACK command=0x%02X result=0",
                       static_cast<unsigned>(command));
            } else {
              ESP_LOGW("ld2410_health",
                       "Canonical ACK rejected command=0x%02X result=%u",
                       static_cast<unsigned>(command),
                       static_cast<unsigned>(result));
            }
          });
    }
#ifdef USE_UART_DEBUGGER
    this->ld2410_uart_->add_debug_callback(
        [this](uart::UARTDirection direction, uint8_t byte) {
          if (direction == uart::UART_DIRECTION_RX)
            this->ld2410_stream_health_.observe_rx_byte(byte, millis());
        });
#endif
  }
  if (this->ld2450_ != nullptr) {
    this->ld2450_startup_guard_.reset(millis());
    this->ld2450_->add_on_data_callback([this]() {
      this->ld2450_startup_guard_.observe_valid_frame(millis());
      this->ld2450_frame_gate_.observe_frame();
    });
  }
#ifdef USE_ESP32
  this->platform_identity_store_result_ =
      initialize_esp32_platform_identity(&this->platform_identity_,
                                         &this->platform_site_membership_);
  if (this->platform_identity_ready()) {
    ESP_LOGI(TAG, "Platform identity: %s",
             platform_identity_store_result_to_string(this->platform_identity_store_result_));
    if (!this->participant_discovery_runtime_.configure_advertisement(
            this->platform_identity_.node_id)) {
      ESP_LOGW(TAG, "Platform discovery advertisement could not be configured");
    }
  } else {
    ESP_LOGW(TAG, "Platform identity unavailable: %s; standalone runtime remains active",
             platform_identity_store_result_to_string(this->platform_identity_store_result_));
  }
  this->participant_registry_store_result_ =
      initialize_esp32_participant_registry(&this->storage_,
                                            &this->platform_runtime_->participant_registry_snapshot);
  if (!get_esp32_participant_registry_persistence(
          &this->participant_registry_backend_, &this->participant_registry_store_)) {
    this->participant_registry_store_result_ =
        ParticipantRegistryStoreResult::STORAGE_FAILED;
  }
  if (this->participant_registry_ready()) {
    ESP_LOGI(TAG, "Participant registry: %s (generation=%u, remote=%u, active=%u)",
             participant_registry_store_result_to_string(
                 this->participant_registry_store_result_),
             static_cast<unsigned>(this->platform_runtime_->participant_registry_snapshot.generation),
             static_cast<unsigned>(this->platform_runtime_->participant_registry_snapshot.registry.size()),
             static_cast<unsigned>(
                 this->platform_runtime_->participant_registry_snapshot.registry.active_count()));
  } else {
    ESP_LOGW(TAG, "Participant registry unavailable: %s; remote participation disabled",
             participant_registry_store_result_to_string(
                 this->participant_registry_store_result_));
  }
  if (this->platform_identity_ready() && this->participant_registry_ready()) {
    this->site_configuration_store_result_ =
        initialize_esp32_site_configuration(
            &this->storage_, this->platform_identity_,
            this->platform_runtime_->participant_registry_snapshot.registry,
            this->platform_site_membership_.mode == SiteMembershipMode::ENROLLED,
            &this->platform_runtime_->site_configuration_snapshot);
    if (!get_esp32_site_configuration_persistence(
            &this->site_configuration_backend_,
            &this->site_configuration_store_)) {
      this->site_configuration_store_result_ =
          SiteConfigurationStoreResult::STORAGE_FAILED;
    }
  }
  if (this->site_configuration_ready() &&
      this->platform_site_membership_.mode ==
          SiteMembershipMode::STANDALONE &&
      has_non_revoked_participants(
          this->platform_runtime_->participant_registry_snapshot.registry) &&
      local_node_is_enrolled_site_member(
          this->platform_identity_, this->platform_runtime_->site_configuration_snapshot)) {
    if (update_esp32_site_membership(
            this->platform_identity_.site_id,
            SiteMembershipMode::ENROLLED, &this->platform_identity_,
            &this->platform_site_membership_)) {
      ESP_LOGI(TAG, "Repaired enrolled Site membership mode");
    } else {
      ESP_LOGW(TAG,
               "Could not repair enrolled Site membership mode; "
               "runtime Site configuration remains authoritative");
    }
  }
  if (this->site_configuration_ready() &&
      !has_non_revoked_participants(
          this->platform_runtime_->participant_registry_snapshot.registry)) {
    this->schedule_standalone_site_reconciliation_(false, millis());
  } else {
    this->standalone_site_reconciliation_.mark_not_required();
  }
  if (this->site_configuration_ready()) {
    ESP_LOGI(TAG,
             "Site configuration: %s (generation=%u, roster=%u, members=%u)",
             site_configuration_store_result_to_string(
                 this->site_configuration_store_result_),
             static_cast<unsigned>(this->platform_runtime_->site_configuration_snapshot.generation),
             static_cast<unsigned>(
                 this->platform_runtime_->site_configuration_snapshot.roster_revision),
             static_cast<unsigned>(
                 this->platform_runtime_->site_configuration_snapshot.member_count()));
  } else {
    ESP_LOGW(TAG,
             "Site configuration unavailable: %s; shared configuration disabled",
             site_configuration_store_result_to_string(
                 this->site_configuration_store_result_));
  }
  if (this->site_configuration_ready()) {
    this->site_directory_store_result_ = initialize_esp32_site_directory(
        &this->storage_,
        this->platform_runtime_->site_configuration_snapshot,
        &this->platform_runtime_->site_directory_snapshot);
    if (!get_esp32_site_directory_persistence(
            &this->site_directory_backend_, &this->site_directory_store_)) {
      this->site_directory_store_result_ =
          SiteDirectoryStoreResult::STORAGE_FAILED;
    }
  }
  if (this->site_directory_store_result_ ==
          SiteDirectoryStoreResult::LOADED ||
      this->site_directory_store_result_ ==
          SiteDirectoryStoreResult::CREATED ||
      this->site_directory_store_result_ ==
          SiteDirectoryStoreResult::UPDATED ||
      this->site_directory_store_result_ ==
          SiteDirectoryStoreResult::COMMIT_REPAIRED) {
    ESP_LOGI(
        TAG,
        "Site directory: %s (generation=%u, floorplans=%u, groups=%u, "
        "compatibility_mirror=%s, legacy_projection=%s)",
        site_directory_store_result_to_string(
            this->site_directory_store_result_),
        static_cast<unsigned>(
            this->platform_runtime_->site_directory_snapshot.generation),
        static_cast<unsigned>(
            this->platform_runtime_->site_directory_snapshot.floorplan_count()),
        static_cast<unsigned>(
            this->platform_runtime_->site_directory_snapshot.group_count()),
        this->platform_runtime_->site_directory_snapshot.compatibility_mirror
            ? "true"
            : "false",
        this->platform_runtime_->site_directory_snapshot
                .legacy_single_floorplan_projection
            ? "true"
            : "false");
  } else {
    ESP_LOGW(TAG,
             "Site directory unavailable: %s; multi-Floorplan routing "
             "remains disabled",
             site_directory_store_result_to_string(
                 this->site_directory_store_result_));
  }
  this->participant_readiness_store_result_ =
      initialize_esp32_participant_readiness(&this->storage_,
                                             &this->platform_runtime_->participant_readiness_snapshot);
  if (!get_esp32_participant_readiness_persistence(
          &this->participant_readiness_backend_, &this->participant_readiness_store_)) {
    this->participant_readiness_store_result_ =
        ParticipantReadinessStoreResult::STORAGE_FAILED;
  }
  if (this->participant_readiness_ready()) {
    ESP_LOGI(TAG, "Participant readiness: %s (generation=%u, configured=%u)",
             participant_readiness_store_result_to_string(
                 this->participant_readiness_store_result_),
             static_cast<unsigned>(this->platform_runtime_->participant_readiness_snapshot.generation),
             static_cast<unsigned>(
                 this->platform_runtime_->participant_readiness_snapshot.registry.size()));
  } else {
    ESP_LOGW(TAG, "Participant readiness unavailable: %s; activation disabled",
             participant_readiness_store_result_to_string(
                 this->participant_readiness_store_result_));
  }
  this->participant_credential_store_result_ =
      initialize_esp32_participant_credentials(
          &this->storage_, &this->participant_credential_snapshot_);
  if (!get_esp32_participant_credential_persistence(
          &this->participant_credential_backend_,
          &this->participant_credential_store_)) {
    this->participant_credential_store_result_ =
        ParticipantCredentialStoreResult::STORAGE_FAILED;
  }
  if (this->participant_credentials_ready()) {
    ESP_LOGI(TAG, "Participant credentials: %s (generation=%u, provisioned=%u)",
             participant_credential_store_result_to_string(
                 this->participant_credential_store_result_),
             static_cast<unsigned>(
                 this->participant_credential_snapshot_.generation),
             static_cast<unsigned>(
                 this->participant_credential_snapshot_.registry.size()));
    if (this->participant_registry_ready() &&
        this->site_configuration_ready()) {
      const auto reconciled = reconcile_site_credential_registry(
          this->platform_runtime_->site_configuration_snapshot,
          this->platform_runtime_->participant_registry_snapshot.registry,
          &this->participant_credential_snapshot_.registry);
      if (reconciled.changed() &&
          this->participant_credential_store_->update(
              *this->participant_credential_backend_,
              this->participant_credential_snapshot_.registry,
              &this->participant_credential_snapshot_) !=
              ParticipantCredentialStoreResult::UPDATED) {
        ESP_LOGW(TAG,
                 "Participant credential reconciliation could not be persisted; "
                 "registry admission remains authoritative");
      } else if (reconciled.changed()) {
        ESP_LOGI(TAG,
                 "Participant credentials reconciled canonical=%u "
                 "orphans=%u revoked=%u",
                 static_cast<unsigned>(reconciled.preserved_canonical),
                 static_cast<unsigned>(reconciled.erased_orphans),
                 static_cast<unsigned>(reconciled.disabled_revoked));
      }
    } else {
      ESP_LOGW(TAG,
               "Participant topology unavailable; stored credentials preserved "
               "until canonical reconciliation is possible");
    }
  } else {
    ESP_LOGW(TAG,
             "Participant credentials unavailable: %s; authenticated remote "
             "transport disabled",
             participant_credential_store_result_to_string(
                 this->participant_credential_store_result_));
  }
  if (this->platform_identity_ready() && this->participant_credentials_ready()) {
    this->site_heartbeat_ = allocate_runtime_object<SiteHeartbeatTransport>(
        "site_heartbeat", &this->site_heartbeat_in_psram_);
    if (this->site_heartbeat_ == nullptr ||
        !this->site_heartbeat_->setup(
            &this->participant_credential_snapshot_.registry,
            &this->participant_discovery_runtime_, this->platform_identity_)) {
      ESP_LOGW(TAG,
               "Authenticated Site heartbeat unavailable; standalone runtime "
               "remains active");
      destroy_runtime_object(&this->site_heartbeat_,
                             &this->site_heartbeat_in_psram_);
    }
    if (this->site_heartbeat_ != nullptr) {
      this->site_heartbeat_->set_internal_ram_checkpoint(
          this, [](void *context, const char *source, const char *phase) {
            static_cast<RadarApiServer *>(context)
                ->record_internal_ram_checkpoint(source, phase);
          });
    }
    if (this->site_heartbeat_ != nullptr) {
      this->site_replication_ =
          allocate_runtime_object<SiteReplicationTransport>(
              "site_replication", &this->site_replication_in_psram_);
      if (this->site_replication_ == nullptr ||
          !this->site_replication_->setup(
              &this->storage_,
              &this->participant_credential_snapshot_,
              this->participant_credential_backend_,
              this->participant_credential_store_,
              &this->site_heartbeat_->endpoints())) {
        ESP_LOGW(TAG,
                 "Authenticated Site replication unavailable; local floorplan "
                 "remains active");
        destroy_runtime_object(&this->site_replication_,
                               &this->site_replication_in_psram_);
      }
    }
  }
  this->record_internal_ram_watermark("setup", "platform_state");
#if defined(NAMO_SHADOW_TRANSPORT_SMOKE) || defined(NAMO_PRODUCTION_FUSION)
  this->observation_transport_ =
      allocate_runtime_object<ObservationTransport>(
          "observation_transport", &this->observation_transport_in_psram_);
  bool transport_ready = false;
  if (this->observation_transport_ != nullptr &&
      this->participant_credentials_ready()) {
#ifdef NAMO_SHADOW_TRANSPORT_SMOKE
    transport_ready = this->observation_transport_->setup_smoke(
        &this->participant_credential_snapshot_.registry);
#else
    transport_ready = this->observation_transport_->setup(
        &this->participant_credential_snapshot_.registry);
#endif
  }
  if (!transport_ready) {
    ESP_LOGE(TAG,
             "Observation transport setup failed; standalone runtime remains "
             "active");
    destroy_runtime_object(&this->observation_transport_,
                           &this->observation_transport_in_psram_);
  } else if (!this->allocate_spatial_shadow_runtime_()) {
    ESP_LOGE(TAG,
             "Shadow fusion runtime allocation failed; transport diagnostics "
             "remain available");
  }
  if (this->observation_transport_ != nullptr) {
    this->observation_transport_->set_internal_ram_checkpoint(
        this, [](void *context, const char *source, const char *phase) {
          static_cast<RadarApiServer *>(context)
              ->record_internal_ram_checkpoint(source, phase);
        });
  }
#endif
#endif
  this->record_internal_ram_watermark("setup", "transports");
#if defined(USE_ESP32) && defined(NAMO_SIGNED_OTA)
  this->firmware_update_handler_ =
      allocate_runtime_object<FirmwareUpdateHandler>(
          "firmware_update", &this->firmware_update_handler_in_psram_, this);
  if (this->firmware_update_handler_ == nullptr) {
    ESP_LOGE(TAG, "Signed OTA handler allocation failed; OTA remains closed");
  } else if (!this->firmware_update_handler_in_psram_) {
    ESP_LOGE(TAG,
             "Signed OTA handler requires PSRAM; internal RAM fallback "
             "released and OTA remains closed");
    destroy_runtime_object(&this->firmware_update_handler_,
                           &this->firmware_update_handler_in_psram_);
  } else {
    this->firmware_update_handler_->setup();
  }
#endif
  this->storage_.ensure_partition();
  if (!this->storage_.prepare_group_roster_storage()) {
    ESP_LOGE(TAG,
             "Group roster storage reservation failed; Group roster "
             "persistence remains unavailable");
  }
  if (!this->stats_store_.setup()) {
    ESP_LOGE(TAG,
             "Statistics workspace allocation failed; statistics remain "
             "unavailable");
  }
  this->system_handler_.setup();
  this->platform_handler_.setup();
  this->state_handler_.setup();
  std::string config;
  if (this->storage_.read_payload(RadarPayloadTarget::DEVICE_CONFIG, &config)) {
    this->device_config_cache_.update(std::move(config));
  } else {
    this->device_config_cache_.clear();
  }
  bool local_pose_valid = false;
  {
    const auto config_read = this->device_config_cache_.read();
    local_pose_valid = config_read->floorplan_radar_pose().valid;
  }
  if (!local_pose_valid) {
    std::string floorplan_document;
    if (this->storage_.read_payload(RadarPayloadTarget::CONFIG,
                                    &floorplan_document) &&
        this->device_config_cache_.hydrate_floorplan_scale_from_document(
            floorplan_document)) {
      ESP_LOGI(TAG,
               "Restored local radar scale from the stored floorplan for "
               "direct-upgrade compatibility");
    }
  }
  this->reconcile_owned_spatial_assignments_("boot");
  this->stats_store_.load(&this->storage_);
  this->record_internal_ram_watermark("setup", "storage");
  if (this->platform_identity_ready() && this->participant_registry_ready() &&
      this->participant_readiness_ready()) {
    const auto *local_directory =
        this->canonical_site_directory_ready()
            ? &this->platform_runtime_->site_directory_snapshot
            : nullptr;
    this->platform_runtime_->site_coordinator.set_local_directory_fence(
        local_directory != nullptr ? local_directory->topology_revision : 0,
        local_directory != nullptr
            ? site_directory_payload_hash(*local_directory)
            : 0);
    const bool canonical_configuration = this->site_configuration_ready();
    const bool local_site_credentials_complete =
        !canonical_configuration ||
        site_credentials_complete_for_node(
            this->platform_identity_,
            this->platform_runtime_->site_configuration_snapshot,
            local_directory,
            this->platform_runtime_->site_coordinator.state().leader_node_id,
            this->participant_credential_snapshot_.registry);
    this->platform_runtime_->site_coordinator.update(
        this->platform_identity_, this->platform_runtime_->participant_registry_snapshot.registry,
        canonical_configuration
            ? &this->platform_runtime_->site_configuration_snapshot
            : nullptr,
        canonical_configuration ? local_directory : nullptr,
        local_site_credentials_complete,
        millis());
    this->platform_runtime_->group_coordinator.update(
        this->platform_identity_, this->platform_runtime_->participant_registry_snapshot.registry,
        this->platform_runtime_->participant_readiness_snapshot.registry,
        canonical_configuration
            ? &this->platform_runtime_->site_configuration_snapshot
            : nullptr,
        canonical_configuration
            ? &this->participant_credential_snapshot_.registry
            : nullptr,
        canonical_configuration && this->canonical_site_directory_ready()
            ? &this->platform_runtime_->site_directory_snapshot
            : nullptr,
        this->platform_runtime_->site_coordinator.state().leader_node_id,
        millis());
    this->reconcile_group_roster_("boot");
    if (this->group_roster_ready()) {
      this->platform_runtime_->group_coordinator.update(
          this->platform_identity_,
          this->platform_runtime_->participant_registry_snapshot.registry,
          this->platform_runtime_->participant_readiness_snapshot.registry,
          canonical_configuration
              ? &this->platform_runtime_->site_configuration_snapshot
              : nullptr,
          &this->platform_runtime_->group_roster_snapshot,
          canonical_configuration
              ? &this->participant_credential_snapshot_.registry
              : nullptr,
          canonical_configuration && this->canonical_site_directory_ready()
              ? &this->platform_runtime_->site_directory_snapshot
              : nullptr,
          this->platform_runtime_->site_coordinator.state().leader_node_id,
          millis());
    }
    this->publish_coordinator_snapshot_();
    this->coordinator_last_update_ms_ = millis();
  }
  this->base_->add_handler(this);
  this->record_internal_ram_watermark("setup", "complete");
  this->set_timeout("setup_prepare_access_point", 30 * 1000, [this]() {
    this->setup_handler_.prepare_setup_access_point();
  });
}

bool RadarApiServer::reconcile_group_roster_(const char *trigger) {
#ifdef USE_ESP32
  if (this->platform_runtime_ == nullptr || !this->site_configuration_ready())
    return false;

  const auto &configuration =
      this->platform_runtime_->site_configuration_snapshot;
  const auto *local_member =
      configuration.find(this->platform_identity_.node_id);
  if (local_member == nullptr ||
      local_member->membership != ParticipantMembership::ACTIVE ||
      local_member->type != ParticipantType::NAMO_NODE ||
      !local_member->spatial_profile.committed ||
      local_member->spatial_profile.group_id[0] == '\0')
    return false;
  const char *group_id = local_member->spatial_profile.group_id;

  uint8_t member_count = 0;
  for (const auto &member : configuration.members) {
    if (member.occupied &&
        member.membership == ParticipantMembership::ACTIVE &&
        member.type == ParticipantType::NAMO_NODE &&
        member.spatial_profile.committed &&
        std::strcmp(member.spatial_profile.group_id, group_id) == 0) {
      if (member_count == GroupRosterSnapshot::MAX_MEMBERS)
        return false;
      ++member_count;
    }
  }
  if (member_count == 0)
    return false;

  const GroupCoordinatorState *group = nullptr;
  for (std::size_t slot = 0; slot < GroupCoordinator::MAX_GROUPS; ++slot) {
    const auto *candidate =
        this->platform_runtime_->group_coordinator.group_at(slot);
    if (candidate != nullptr &&
        std::strcmp(candidate->group_id, group_id) == 0) {
      group = candidate;
      break;
    }
  }
  const char *directory_leader_node_id = nullptr;
  if (this->canonical_site_directory_ready()) {
    const auto *directory_group =
        this->platform_runtime_->site_directory_snapshot.find_group(group_id);
    if (directory_group != nullptr &&
        directory_group->assignment_revision != 0)
      directory_leader_node_id = directory_group->leader_node_id;
  }
  const char *runtime_leader_node_id =
      group != nullptr && group->configured &&
              group->leader_node_id[0] != '\0'
          ? group->leader_node_id
          : nullptr;
  const char *stored_leader_node_id =
      group_roster_matches_configuration_scope(
          this->platform_runtime_->group_roster_snapshot, configuration,
          group_id)
          ? this->platform_runtime_->group_roster_snapshot.leader_node_id
          : nullptr;
  const auto *selected_leader = select_group_roster_leader(
      configuration, group_id,
      this->platform_runtime_->site_coordinator.state().leader_node_id,
      directory_leader_node_id, runtime_leader_node_id,
      stored_leader_node_id);
  if (selected_leader == nullptr)
    return false;
  const char *leader_node_id = selected_leader->participant_id;

  const bool was_ready = this->group_roster_ready();

  if (this->group_roster_backend_ == nullptr ||
      this->group_roster_store_ == nullptr ||
      this->group_roster_store_result_ == GroupRosterStoreResult::EMPTY) {
    this->group_roster_store_result_ = initialize_esp32_group_roster(
        &this->storage_, configuration, group_id, leader_node_id,
        &this->platform_runtime_->group_roster_snapshot);
    if (!get_esp32_group_roster_persistence(&this->group_roster_backend_,
                                            &this->group_roster_store_)) {
      this->group_roster_store_result_ =
          GroupRosterStoreResult::STORAGE_FAILED;
      return false;
    }
    ESP_LOGI(TAG,
             "Group roster: %s trigger=%s group=%s generation=%u "
             "assignment=%u members=%u leader=%s",
             group_roster_store_result_to_string(
                 this->group_roster_store_result_),
             trigger != nullptr ? trigger : "unknown", group_id,
             static_cast<unsigned>(
                 this->platform_runtime_->group_roster_snapshot.generation),
             static_cast<unsigned>(this->platform_runtime_
                                       ->group_roster_snapshot
                                       .assignment_revision),
             static_cast<unsigned>(this->platform_runtime_
                                       ->group_roster_snapshot
                                       .member_count()),
             this->platform_runtime_->group_roster_snapshot.leader_node_id);
  }
  const bool stored_roster =
      this->group_roster_store_result_ == GroupRosterStoreResult::LOADED ||
      this->group_roster_store_result_ == GroupRosterStoreResult::CREATED ||
      this->group_roster_store_result_ == GroupRosterStoreResult::MIGRATED ||
      this->group_roster_store_result_ == GroupRosterStoreResult::UPDATED ||
      this->group_roster_store_result_ ==
          GroupRosterStoreResult::COMMIT_REPAIRED;
  if (!stored_roster ||
      !valid_group_roster(
          this->platform_runtime_->group_roster_snapshot))
    return false;

  const bool became_ready = !was_ready && this->group_roster_ready();

  auto &candidate =
      this->platform_runtime_->group_roster_update_scratch;
  candidate = {};
  const auto prepared = prepare_group_roster_from_site_configuration(
      configuration, group_id, leader_node_id, &candidate);
  if (prepared != GroupRosterResult::OK) {
    ESP_LOGW(TAG, "Group roster reconciliation rejected trigger=%s result=%s",
             trigger != nullptr ? trigger : "unknown",
             group_roster_result_to_string(prepared));
    return became_ready;
  }
  if (!group_roster_update_allowed(
          this->platform_runtime_->group_roster_snapshot, candidate))
    return became_ready;

  const auto updated = this->group_roster_store_->update(
      *this->group_roster_backend_, candidate,
      &this->platform_runtime_->group_roster_snapshot);
  if (updated != GroupRosterStoreResult::UPDATED &&
      updated != GroupRosterStoreResult::LOADED) {
    ESP_LOGW(TAG,
             "Group roster persistence failed trigger=%s result=%s; "
             "previous committed roster remains active",
             trigger != nullptr ? trigger : "unknown",
             group_roster_store_result_to_string(updated));
    return became_ready;
  }
  this->group_roster_store_result_ = updated;
  if (updated == GroupRosterStoreResult::UPDATED) {
    ESP_LOGI(TAG,
             "Group roster committed trigger=%s group=%s generation=%u "
             "assignment=%u members=%u leader=%s",
             trigger != nullptr ? trigger : "unknown",
             this->platform_runtime_->group_roster_snapshot.group_id,
             static_cast<unsigned>(
                 this->platform_runtime_->group_roster_snapshot.generation),
             static_cast<unsigned>(this->platform_runtime_
                                       ->group_roster_snapshot
                                       .assignment_revision),
             static_cast<unsigned>(this->platform_runtime_
                                       ->group_roster_snapshot
                                       .member_count()),
             this->platform_runtime_->group_roster_snapshot.leader_node_id);
  }
  return became_ready || updated == GroupRosterStoreResult::UPDATED;
#else
  (void) trigger;
  return false;
#endif
}

bool RadarApiServer::disable_participant_credential(
    const char *participant_id) {
#ifdef USE_ESP32
  if (!this->participant_credential_persistence_ready())
    return false;
  const auto *existing =
      this->participant_credential_snapshot_.registry.find(participant_id);
  if (existing == nullptr || !existing->enabled)
    return true;
  const ParticipantCredentialRecord backup = *existing;
  if (this->participant_credential_snapshot_.registry.set_enabled(
          participant_id, false) != ParticipantCredentialResult::OK)
    return false;
  if (this->participant_credential_store_->update(
          *this->participant_credential_backend_,
          this->participant_credential_snapshot_.registry,
          &this->participant_credential_snapshot_) ==
      ParticipantCredentialStoreResult::UPDATED)
    return true;
  this->participant_credential_snapshot_.registry.set(
      backup.participant_id, backup.participant_token, backup.key_version,
      backup.key.data(), backup.enabled);
#endif
  return false;
}

bool RadarApiServer::enable_participant_credential(
    const char *participant_id) {
#ifdef USE_ESP32
  if (!this->participant_credential_persistence_ready())
    return false;
  const auto *existing =
      this->participant_credential_snapshot_.registry.find(participant_id);
  if (existing == nullptr)
    return false;
  if (existing->enabled)
    return true;
  if (this->participant_credential_snapshot_.registry.set_enabled(
          participant_id, true) != ParticipantCredentialResult::OK)
    return false;
  if (this->participant_credential_store_->update(
          *this->participant_credential_backend_,
          this->participant_credential_snapshot_.registry,
          &this->participant_credential_snapshot_) ==
      ParticipantCredentialStoreResult::UPDATED)
    return true;
  this->participant_credential_snapshot_.registry.set_enabled(participant_id,
                                                               false);
#endif
  return false;
}

bool RadarApiServer::erase_participant_credential(const char *participant_id) {
#ifdef USE_ESP32
  if (!this->participant_credential_persistence_ready())
    return false;
  const auto *existing =
      this->participant_credential_snapshot_.registry.find(participant_id);
  if (existing == nullptr)
    return true;
  const ParticipantCredentialRecord backup = *existing;
  if (this->participant_credential_snapshot_.registry.erase(participant_id) !=
      ParticipantCredentialResult::OK)
    return false;
  if (this->participant_credential_store_->update(
          *this->participant_credential_backend_,
          this->participant_credential_snapshot_.registry,
          &this->participant_credential_snapshot_) ==
      ParticipantCredentialStoreResult::UPDATED)
    return true;
  this->participant_credential_snapshot_.registry.set(
      backup.participant_id, backup.participant_token, backup.key_version,
      backup.key.data(), backup.enabled);
#endif
  return false;
}

bool RadarApiServer::restore_participant_credentials(
    const ParticipantCredentialRegistry &credentials) {
#ifdef USE_ESP32
  if (!this->participant_credential_persistence_ready())
    return false;
  return this->participant_credential_store_->update(
             *this->participant_credential_backend_, credentials,
             &this->participant_credential_snapshot_) ==
         ParticipantCredentialStoreResult::UPDATED;
#else
  (void) credentials;
  return false;
#endif
}

bool RadarApiServer::refresh_owned_site_roster(
    const char *spatial_profile_participant_id,
    bool resolve_automatic_group) {
  if (!this->site_configuration_persistence_ready() ||
      !this->participant_registry_ready())
    return false;
  if (!has_non_revoked_participants(
          this->platform_runtime_->participant_registry_snapshot.registry)) {
    uint32_t operation_id = 0;
    this->participant_pairing_runtime_.queue_cancel(&operation_id);
    const auto result = this->reconcile_standalone_site_();
    this->standalone_site_reconciliation_.complete(result);
    if (result == StandaloneSiteReconciliationResult::OK)
      return true;
    if (result ==
        StandaloneSiteReconciliationResult::MEMBERSHIP_STORAGE_FAILED) {
      this->schedule_standalone_site_reconciliation_(false, millis());
      ESP_LOGW(TAG,
               "Standalone membership persistence deferred after canonical "
               "roster commit");
      return true;
    }
    ESP_LOGE(TAG, "Standalone Site reconciliation failed result=%s",
             standalone_site_reconciliation_result_to_string(result));
    return false;
  }
  this->standalone_site_reconciliation_.mark_not_required();
  if (std::strcmp(
          this->platform_runtime_->site_configuration_snapshot.configuration_owner_node_id,
          this->platform_identity_.node_id) != 0)
    return true;
  if (commit_local_site_roster(
          this->platform_identity_,
          this->platform_runtime_->participant_registry_snapshot.registry,
          this->platform_runtime_->site_configuration_snapshot,
          &this->platform_runtime_->site_configuration_update_scratch) !=
      SiteConfigurationResult::OK)
    return false;
  bool spatial_profiles_changed = false;
  bool automatic_group_ready = false;
  AutomaticSpatialGroupDecision group_decision{};
  if (resolve_automatic_group &&
      spatial_profile_participant_id != nullptr &&
      spatial_profile_participant_id[0] != '\0' &&
      this->participant_readiness_ready()) {
    const auto *readiness =
        this->platform_runtime_->participant_readiness_snapshot.registry.find(
            spatial_profile_participant_id);
    const auto group_result =
        readiness != nullptr
            ? resolve_automatic_spatial_group(
                  this->platform_identity_,
                  this->platform_runtime_->site_configuration_update_scratch,
                  spatial_profile_participant_id,
                  readiness->config.space_id,
                  readiness->config.group_id, &group_decision)
            : AutomaticSpatialGroupResult::NOT_READY;
    if (group_result != AutomaticSpatialGroupResult::OK) {
      ESP_LOGW(TAG,
               "Automatic spatial Group reconciliation participant=%s "
               "result=%s",
               spatial_profile_participant_id,
               automatic_spatial_group_result_to_string(group_result));
      return false;
    }
    automatic_group_ready = true;
  }
  FloorplanRadarPose local_pose{};
  {
    const auto config_read = this->device_config_cache_.read();
    local_pose = config_read->floorplan_radar_pose();
  }
  const auto local_transform = local_pose.spatial_transform();
  bool reconciled_profiles_changed = false;
  if (this->participant_readiness_ready() &&
      this->platform_runtime_->site_configuration_update_scratch.floorplan.committed &&
      local_pose.pose_version != 0 &&
      valid_spatial_transform(local_transform) &&
      reconcile_local_spatial_profiles(
          this->platform_identity_,
          this->platform_runtime_->participant_readiness_snapshot.registry,
          local_pose.pose_version, local_transform,
          this->platform_runtime_->site_configuration_update_scratch,
          spatial_profile_participant_id != nullptr
              ? SpatialProfileReconciliationMode::EXPLICIT_CALIBRATION
              : SpatialProfileReconciliationMode::MISSING_ONLY,
          spatial_profile_participant_id,
          &this->platform_runtime_->site_configuration_update_scratch,
          &reconciled_profiles_changed) != SiteConfigurationResult::OK)
    return false;
  spatial_profiles_changed =
      spatial_profiles_changed || reconciled_profiles_changed;
  if (automatic_group_ready) {
    bool group_changed = false;
    if (apply_automatic_spatial_group_assignment(
            this->platform_identity_, group_decision,
            this->platform_runtime_->site_configuration_update_scratch,
            &this->platform_runtime_->site_configuration_update_scratch,
            &group_changed) != SiteConfigurationResult::OK)
      return false;
    spatial_profiles_changed = spatial_profiles_changed || group_changed;
  }
  if (spatial_profiles_changed &&
      this->platform_runtime_->site_configuration_update_scratch.roster_revision ==
          this->platform_runtime_->site_configuration_snapshot.roster_revision) {
    if (this->platform_runtime_->site_configuration_update_scratch.roster_revision ==
        UINT32_MAX)
      return false;
    ++this->platform_runtime_->site_configuration_update_scratch.roster_revision;
  }
  if (this->platform_runtime_->site_configuration_update_scratch.roster_revision ==
          this->platform_runtime_->site_configuration_snapshot.roster_revision &&
      !spatial_profiles_changed)
    return true;
  return this->site_configuration_store_->update(
             *this->site_configuration_backend_,
             this->platform_runtime_->site_configuration_update_scratch,
             &this->platform_runtime_->site_configuration_snapshot) ==
         SiteConfigurationStoreResult::UPDATED;
}

bool RadarApiServer::reconcile_owned_spatial_assignments_(
    const char *trigger) {
  if (!this->site_configuration_ready() ||
      !this->participant_readiness_ready() ||
      std::strcmp(
          this->platform_runtime_->site_configuration_snapshot
              .configuration_owner_node_id,
          this->platform_identity_.node_id) != 0 ||
      !site_configuration_needs_spatial_profile_reconciliation(
          this->platform_runtime_->site_configuration_snapshot))
    return true;

  const char *source = trigger != nullptr ? trigger : "unknown";
  FloorplanRadarPose local_pose{};
  {
    const auto config_read = this->device_config_cache_.read();
    local_pose = config_read->floorplan_radar_pose();
  }
  if (!local_pose.valid || local_pose.pose_version == 0) {
    ESP_LOGW(TAG,
             "Canonical spatial assignment reconciliation trigger=%s "
             "result=deferred_local_pose",
             source);
    return false;
  }
  if (!this->refresh_owned_site_roster()) {
    ESP_LOGW(TAG,
             "Canonical spatial assignment reconciliation trigger=%s "
             "result=persistence_failed",
             source);
    return false;
  }
  if (site_configuration_needs_spatial_profile_reconciliation(
          this->platform_runtime_->site_configuration_snapshot)) {
    ESP_LOGW(TAG,
             "Canonical spatial assignment reconciliation trigger=%s "
             "result=deferred_readiness",
             source);
    return false;
  }

  this->site_directory_reconciliation_check_ms_ = 0;
  ESP_LOGI(TAG,
           "Canonical spatial assignment reconciliation trigger=%s "
           "result=ok transforms=preserved",
           source);
  return true;
}

void RadarApiServer::schedule_standalone_site_reconciliation_(
    bool cancel_pairing, uint32_t now_ms) {
  if (has_non_revoked_participants(
          this->platform_runtime_->participant_registry_snapshot.registry)) {
    this->standalone_site_reconciliation_.mark_not_required();
    return;
  }
  if (cancel_pairing) {
    uint32_t operation_id = 0;
    this->participant_pairing_runtime_.queue_cancel(&operation_id);
  }
  this->standalone_site_reconciliation_.schedule(
      now_ms, STANDALONE_SITE_RECONCILIATION_DELAY_MS);
  ESP_LOGI(TAG, "Standalone Site reconciliation queued");
}

void RadarApiServer::process_standalone_site_reconciliation_(uint32_t now_ms) {
  if (!this->standalone_site_reconciliation_.due(now_ms))
    return;
  if (has_non_revoked_participants(
          this->platform_runtime_->participant_registry_snapshot.registry)) {
    this->standalone_site_reconciliation_.mark_not_required();
    return;
  }
  const auto result = this->reconcile_standalone_site_();
  this->standalone_site_reconciliation_.complete(result);
  if (result == StandaloneSiteReconciliationResult::OK) {
    ESP_LOGI(TAG, "Standalone Site reconciliation completed");
  } else {
    ESP_LOGE(TAG,
             "Standalone Site reconciliation failed result=%s; pairing "
             "remains blocked until recovery succeeds",
             standalone_site_reconciliation_result_to_string(result));
  }
}

void RadarApiServer::process_site_directory_reconciliation_(uint32_t now_ms) {
  if (!this->site_configuration_ready() || !this->site_directory_stored_() ||
      this->site_directory_backend_ == nullptr ||
      this->site_directory_store_ == nullptr)
    return;
  if (!this->site_directory_ready() &&
      !this->reconcile_site_directory_scope_("periodic"))
    return;
  if (!this->site_authority_snapshot(now_ms).consistent() ||
      (this->site_directory_reconciliation_check_ms_ != 0 &&
       now_ms - this->site_directory_reconciliation_check_ms_ <
           SITE_DIRECTORY_RECONCILIATION_INTERVAL_MS))
    return;
  this->site_directory_reconciliation_check_ms_ = now_ms;

  const auto &current =
      this->platform_runtime_->site_directory_snapshot;
  const auto &site_leader =
      this->platform_runtime_->site_coordinator.state();
  if (!site_leader.local_leader ||
      std::strcmp(site_leader.leader_node_id,
                  this->platform_identity_.node_id) != 0)
    return;

  if (current.compatibility_mirror ||
      current.legacy_single_floorplan_projection) {
    if (current.compatibility_mirror &&
        this->platform_site_membership_.mode ==
            SiteMembershipMode::ENROLLED) {
      if (this->site_heartbeat_ == nullptr ||
          !site_directory_mirror_promotion_allowed(
              this->platform_identity_,
              this->platform_runtime_->site_configuration_snapshot, current,
              site_leader, this->site_heartbeat_->endpoints(),
              this->participant_credential_snapshot_.registry, now_ms)) {
        this->site_directory_reconciliation_result_ =
            SiteDirectoryResult::NOT_READY;
        return;
      }
    }
    bool projection_update_required = false;
    const auto projection_result =
        prepare_legacy_single_floorplan_projection(
            this->platform_runtime_->site_configuration_snapshot,
            this->platform_runtime_->group_coordinator, current,
            &this->platform_runtime_->site_directory_update_scratch,
            &projection_update_required);
    if (projection_result != SiteDirectoryResult::OK) {
      if (projection_result != SiteDirectoryResult::NOT_READY &&
          projection_result != this->site_directory_reconciliation_result_) {
        ESP_LOGW(TAG, "Site directory reconciliation blocked result=%s",
                 site_directory_result_to_string(projection_result));
      }
      this->site_directory_reconciliation_result_ = projection_result;
      return;
    }
    if (projection_update_required) {
      const auto store_result = this->site_directory_store_->update(
          *this->site_directory_backend_,
          this->platform_runtime_->site_directory_update_scratch,
          &this->platform_runtime_->site_directory_snapshot);
      if (store_result != SiteDirectoryStoreResult::UPDATED) {
        ESP_LOGE(TAG,
                 "Site directory projection persistence failed: %s",
                 site_directory_store_result_to_string(store_result));
        return;
      }
      this->site_directory_store_result_ = store_result;
      ESP_LOGI(TAG,
               "Site directory projection committed generation=%u "
               "floorplans=%u groups=%u",
               static_cast<unsigned>(
                   this->platform_runtime_->site_directory_snapshot.generation),
               static_cast<unsigned>(
                   this->platform_runtime_->site_directory_snapshot.floorplan_count()),
               static_cast<unsigned>(
                   this->platform_runtime_->site_directory_snapshot.group_count()));
      return;
    }
  }

  bool update_required = false;
  const auto build_result = prepare_site_directory_from_advertisements(
      this->platform_runtime_->site_configuration_snapshot, current,
      this->platform_runtime_->site_floorplan_owners,
      this->platform_runtime_->site_group_summaries,
      &this->platform_runtime_->site_directory_update_scratch,
      &update_required);
  if (build_result != SiteDirectoryResult::OK) {
    if (build_result != SiteDirectoryResult::NOT_READY &&
        build_result != this->site_directory_reconciliation_result_) {
      ESP_LOGW(TAG, "Site directory advertisement merge blocked result=%s",
               site_directory_result_to_string(build_result));
    }
    this->site_directory_reconciliation_result_ = build_result;
    return;
  }
  this->site_directory_reconciliation_result_ = SiteDirectoryResult::OK;
  if (!update_required)
    return;

  const auto store_result = this->site_directory_store_->update(
      *this->site_directory_backend_,
      this->platform_runtime_->site_directory_update_scratch,
      &this->platform_runtime_->site_directory_snapshot);
  if (store_result != SiteDirectoryStoreResult::UPDATED) {
    ESP_LOGE(TAG, "Site directory advertisement persistence failed: %s",
             site_directory_store_result_to_string(store_result));
    return;
  }
  this->site_directory_store_result_ = store_result;
  ESP_LOGI(TAG,
           "Site directory topology committed topology=%u floorplans=%u "
           "groups=%u",
           static_cast<unsigned>(this->platform_runtime_->
                                     site_directory_snapshot.topology_revision),
           static_cast<unsigned>(this->platform_runtime_->
                                     site_directory_snapshot.floorplan_count()),
           static_cast<unsigned>(this->platform_runtime_->
                                     site_directory_snapshot.group_count()));
}

void RadarApiServer::reset_site_scoped_runtime_(const char *trigger) {
  if (this->platform_runtime_ == nullptr)
    return;
#ifdef USE_ESP32
  if (this->site_replication_ != nullptr)
    this->site_replication_->cancel();
  if (this->site_heartbeat_ != nullptr)
    this->site_heartbeat_->reset_endpoint_authentication();
  if (this->spatial_shadow_runtime_ != nullptr)
    this->spatial_shadow_runtime_->clear();
#endif
  this->platform_runtime_->site_authority_snapshot = {};
  this->platform_runtime_->site_authority_replication.reset();
  this->platform_runtime_->site_coordinator.clear();
  this->platform_runtime_->group_coordinator.clear();
  this->platform_runtime_->site_group_summaries.clear();
  this->platform_runtime_->site_space_summaries.clear();
  this->platform_runtime_->site_floorplan_owners.clear();
  this->platform_runtime_->spatial_production_fusion.clear();
  this->platform_runtime_->fusion_automation.disable();
  this->platform_runtime_->shared_automation_areas.clear();
  this->site_directory_reconciliation_result_ = SiteDirectoryResult::NOT_READY;
  this->site_directory_reconciliation_check_ms_ = 0;
  this->auto_directory_replication_due_ms_ = 0;
  this->auto_directory_replication_retry_count_ = 0;
  this->auto_directory_replication_active_ = false;
  this->auto_directory_replication_source_node_id_[0] = '\0';
  this->coordinator_last_update_ms_ = 0;
  this->spatial_shadow_last_update_ms_ = 0;
  this->floorplan_replica_check_ms_ = 0;
  this->local_floorplan_replica_ready_ = false;
  this->local_floorplan_stored_ = false;
  this->local_floorplan_config_hash_ = 0;
  this->local_floorplan_image_hash_ = 0;
  ESP_LOGI(TAG, "Site-scoped runtime reset trigger=%s",
           trigger != nullptr ? trigger : "unknown");
}

bool RadarApiServer::reconcile_site_directory_scope_(const char *trigger) {
  if (!this->site_configuration_ready() || !this->site_directory_stored_() ||
      this->site_directory_backend_ == nullptr ||
      this->site_directory_store_ == nullptr)
    return false;
  const auto &configuration =
      this->platform_runtime_->site_configuration_snapshot;
  const auto &current = this->platform_runtime_->site_directory_snapshot;
  if (site_directory_matches_configuration_scope(configuration, current))
    return true;

  ESP_LOGW(TAG,
           "Site directory scope mismatch trigger=%s directory=%s "
           "configuration=%s; rebasing persisted topology",
           trigger != nullptr ? trigger : "unknown", current.site_id,
           configuration.site_id);
  bool update_required = false;
  const auto prepared = prepare_site_directory_compatibility_mirror(
      configuration, &current,
      &this->platform_runtime_->site_directory_update_scratch,
      &update_required);
  if (prepared != SiteDirectoryResult::OK || !update_required) {
    ESP_LOGE(TAG, "Site directory scope rebase rejected result=%s",
             site_directory_result_to_string(prepared));
    return false;
  }
  const auto stored = this->site_directory_store_->update(
      *this->site_directory_backend_,
      this->platform_runtime_->site_directory_update_scratch,
      &this->platform_runtime_->site_directory_snapshot);
  if (stored != SiteDirectoryStoreResult::UPDATED) {
    ESP_LOGE(TAG, "Site directory scope rebase persistence failed: %s",
             site_directory_store_result_to_string(stored));
    return false;
  }
  this->site_directory_store_result_ = stored;
  this->reset_site_scoped_runtime_(trigger);
  this->credential_provisioning_authority_epoch_ = 0;
  this->credential_provisioning_roster_revision_ = 0;
  this->credential_provisioning_attempted_source_mask_ = 0;
  this->credential_provisioning_owner_node_id_[0] = '\0';
  this->credential_provisioning_verified_ = false;
  this->credential_provisioning_refresh_pending_ = true;
  this->credential_provisioning_wait_reason_ =
      CredentialProvisioningSourceReadiness::READY;
  this->credential_provisioning_check_ms_ = 0;
  return this->site_directory_ready();
}

void RadarApiServer::process_automatic_site_directory_replication_(
    uint32_t now_ms) {
#ifdef USE_ESP32
  if (this->site_replication_ == nullptr || this->site_heartbeat_ == nullptr ||
      !this->site_configuration_ready() || !this->site_directory_ready())
    return;

  const auto &configuration =
      this->platform_runtime_->site_configuration_snapshot;
  const auto &directory = this->platform_runtime_->site_directory_snapshot;
  const auto &site = this->platform_runtime_->site_coordinator.state();
  const char *bootstrap_source = site_directory_bootstrap_source(
      this->platform_identity_, configuration, directory, site,
      this->site_heartbeat_->endpoints(),
      this->participant_credential_snapshot_.registry, now_ms);
  const bool bootstrap_pull = bootstrap_source != nullptr;
  const bool normal_pull =
      !bootstrap_pull &&
      this->site_authority_snapshot(now_ms).consistent() &&
      site_directory_replication_required(
          site, configuration, this->platform_identity_.node_id);
  const bool should_pull = bootstrap_pull || normal_pull;
  const char *source_node_id =
      bootstrap_pull ? bootstrap_source : site.leader_node_id;
  if (!should_pull) {
    if (this->auto_directory_replication_active_ &&
        this->site_replication_->diagnostics().target ==
            SiteReplicationTarget::SITE_DIRECTORY)
      this->site_replication_->cancel();
    this->auto_directory_replication_active_ = false;
    this->auto_directory_replication_due_ms_ = 0;
    this->auto_directory_replication_retry_count_ = 0;
    this->auto_directory_replication_source_node_id_[0] = '\0';
    return;
  }

  if (std::strcmp(this->auto_directory_replication_source_node_id_,
                  source_node_id) != 0) {
    if (this->auto_directory_replication_active_ &&
        this->site_replication_->diagnostics().target ==
            SiteReplicationTarget::SITE_DIRECTORY)
      this->site_replication_->cancel();
    std::strncpy(this->auto_directory_replication_source_node_id_,
                 source_node_id,
                 sizeof(this->auto_directory_replication_source_node_id_) - 1);
    this->auto_directory_replication_source_node_id_[
        sizeof(this->auto_directory_replication_source_node_id_) - 1] = '\0';
    uint32_t slot_delay = site_replication_source_slot_delay_ms(
        configuration, source_node_id, this->platform_identity_.node_id);
    if (slot_delay == std::numeric_limits<uint32_t>::max())
      slot_delay = SITE_REPLICATION_SLOT_MS;
    this->auto_directory_replication_due_ms_ = now_ms + slot_delay;
    this->auto_directory_replication_retry_count_ = 0;
    this->auto_directory_replication_active_ = false;
  }

  const auto &diagnostics = this->site_replication_->diagnostics();
  if (this->auto_directory_replication_active_) {
    if (diagnostics.target != SiteReplicationTarget::SITE_DIRECTORY) {
      this->auto_directory_replication_active_ = false;
    } else if (diagnostics.state == SiteReplicationState::COMPLETE) {
      this->auto_directory_replication_active_ = false;
      this->auto_directory_replication_retry_count_ = 0;
      this->auto_directory_replication_due_ms_ =
          now_ms + SITE_DIRECTORY_REPLICATION_INTERVAL_MS;
    } else if (diagnostics.state == SiteReplicationState::FAILED ||
               diagnostics.state == SiteReplicationState::CONFLICT) {
      this->auto_directory_replication_active_ = false;
      if (this->auto_directory_replication_retry_count_ <
          SITE_REPLICATION_MAX_SESSION_RETRIES)
        ++this->auto_directory_replication_retry_count_;
      this->auto_directory_replication_due_ms_ =
          now_ms + site_replication_retry_delay_ms(
                       this->auto_directory_replication_retry_count_);
    }
  }
  if (this->auto_directory_replication_active_)
    return;
  if (this->auto_directory_replication_due_ms_ == 0)
    this->auto_directory_replication_due_ms_ =
        now_ms + SITE_REPLICATION_SLOT_MS;
  if (static_cast<int32_t>(
          now_ms - this->auto_directory_replication_due_ms_) < 0 ||
      this->platform_runtime_->site_authority_replication.busy())
    return;

  const auto state = this->site_replication_->diagnostics().state;
  const bool transport_available =
      state == SiteReplicationState::IDLE ||
      state == SiteReplicationState::COMPLETE ||
      state == SiteReplicationState::FAILED ||
      state == SiteReplicationState::CONFLICT;
  const auto *endpoint = this->site_heartbeat_->endpoints().find(
      this->auto_directory_replication_source_node_id_);
  if (!transport_available ||
      !ParticipantEndpointCache::is_online(endpoint, now_ms))
    return;

  const auto start_result = this->site_replication_->start_directory(
      this->auto_directory_replication_source_node_id_, now_ms,
      bootstrap_pull
          ? SiteDirectoryReplicationMode::INITIAL_BOOTSTRAP
          : SiteDirectoryReplicationMode::STANDARD);
  if (start_result == SiteReplicationResult::OK) {
    this->auto_directory_replication_active_ = true;
    this->auto_directory_replication_due_ms_ = 0;
    if (bootstrap_pull) {
      ESP_LOGI(TAG,
               "Initial Site Directory bootstrap started source=%s "
               "local_mirror=true",
               this->auto_directory_replication_source_node_id_);
    }
  } else {
    if (this->auto_directory_replication_retry_count_ <
        SITE_REPLICATION_MAX_SESSION_RETRIES)
      ++this->auto_directory_replication_retry_count_;
    this->auto_directory_replication_due_ms_ =
        now_ms + site_replication_retry_delay_ms(
                     this->auto_directory_replication_retry_count_);
  }
#else
  (void) now_ms;
#endif
}

void RadarApiServer::process_automatic_credential_provisioning_(
    uint32_t now_ms) {
#ifdef USE_ESP32
  if (this->site_replication_ == nullptr || this->site_heartbeat_ == nullptr ||
      !this->site_configuration_ready() || !this->site_directory_ready() ||
      !this->participant_credentials_ready())
    return;
  const auto &configuration =
      this->platform_runtime_->site_configuration_snapshot;
  const auto context_change = credential_provisioning_context_change(
      this->credential_provisioning_authority_epoch_,
      this->credential_provisioning_roster_revision_,
      this->credential_provisioning_owner_node_id_, configuration);
  if (context_change != CredentialProvisioningContextChange::NONE) {
    this->credential_provisioning_authority_epoch_ =
        configuration.authority_epoch;
    this->credential_provisioning_roster_revision_ =
        configuration.roster_revision;
    std::strncpy(this->credential_provisioning_owner_node_id_,
                 configuration.configuration_owner_node_id,
                 sizeof(this->credential_provisioning_owner_node_id_) - 1);
    this->credential_provisioning_owner_node_id_[
        sizeof(this->credential_provisioning_owner_node_id_) - 1] = '\0';
    this->credential_provisioning_refresh_pending_ = true;
    this->credential_provisioning_attempted_source_mask_ = 0;
    this->credential_provisioning_check_ms_ = 0;
    if (context_change ==
        CredentialProvisioningContextChange::AUTHORITY_REFRESH) {
      this->credential_provisioning_verified_ = false;
      const auto &replication = this->site_replication_->diagnostics();
      if (replication.target == SiteReplicationTarget::CREDENTIAL_BUNDLE &&
          site_replication_is_active(replication.state))
        this->site_replication_->cancel();
      ESP_LOGI(TAG,
               "Credential authority changed owner=%s epoch=%u roster=%u; "
               "bundle refresh queued with Site/Group runtime preserved",
               configuration.configuration_owner_node_id,
               static_cast<unsigned>(configuration.authority_epoch),
               static_cast<unsigned>(configuration.roster_revision));
    } else {
      ESP_LOGI(TAG,
               "Credential roster changed owner=%s epoch=%u roster=%u; "
               "bundle refresh queued without leader reset",
               configuration.configuration_owner_node_id,
               static_cast<unsigned>(configuration.authority_epoch),
               static_cast<unsigned>(configuration.roster_revision));
    }
  }
  if (std::strcmp(configuration.configuration_owner_node_id,
                  this->platform_identity_.node_id) == 0) {
    this->credential_provisioning_verified_ = true;
    this->credential_provisioning_refresh_pending_ = false;
    this->credential_provisioning_attempted_source_mask_ = 0;
    return;
  }
  if (site_credential_bootstrap_ready_for_node(
          this->platform_identity_, configuration,
          this->platform_runtime_->participant_registry_snapshot.registry,
          this->participant_credential_snapshot_.registry)) {
    this->credential_provisioning_check_ms_ = 0;
    return;
  }
  const auto &diagnostics = this->site_replication_->diagnostics();
  if (diagnostics.target == SiteReplicationTarget::CREDENTIAL_BUNDLE &&
      diagnostics.state == SiteReplicationState::COMPLETE &&
      diagnostics.remote_generation == configuration.generation) {
    this->credential_provisioning_verified_ = true;
    this->credential_provisioning_refresh_pending_ = false;
    this->credential_provisioning_attempted_source_mask_ = 0;
  }
  const bool credentials_complete = site_credentials_complete_for_node(
      this->platform_identity_, configuration,
      &this->platform_runtime_->site_directory_snapshot,
      this->platform_runtime_->site_coordinator.state().leader_node_id,
      this->participant_credential_snapshot_.registry);
  if (this->credential_provisioning_verified_ && credentials_complete &&
      !this->credential_provisioning_refresh_pending_) {
    this->credential_provisioning_attempted_source_mask_ = 0;
    this->credential_provisioning_check_ms_ = 0;
    return;
  }
  if (this->credential_provisioning_check_ms_ != 0 &&
      static_cast<int32_t>(now_ms - this->credential_provisioning_check_ms_) <
          0)
    return;
  if (diagnostics.state != SiteReplicationState::IDLE &&
      diagnostics.state != SiteReplicationState::COMPLETE &&
      diagnostics.state != SiteReplicationState::FAILED &&
      diagnostics.state != SiteReplicationState::CONFLICT)
    return;
  const auto &site = this->platform_runtime_->site_coordinator.state();
  const bool previous_source_failed =
      diagnostics.target == SiteReplicationTarget::CREDENTIAL_BUNDLE &&
      (diagnostics.state == SiteReplicationState::FAILED ||
       diagnostics.state == SiteReplicationState::CONFLICT);
  if (previous_source_failed) {
    this->credential_provisioning_attempted_source_mask_ |=
        credential_provisioning_source_bit(configuration,
                                           diagnostics.owner_node_id);
  }
  const auto *source_endpoint = select_credential_provisioning_source(
      this->platform_identity_, configuration,
      this->site_heartbeat_->endpoints(),
      this->participant_credential_snapshot_.registry, site.leader_node_id,
      this->credential_provisioning_attempted_source_mask_, now_ms,
      this->credential_provisioning_refresh_pending_ &&
          !this->credential_provisioning_verified_);
  const auto *owner_endpoint = this->site_heartbeat_->endpoints().find(
      configuration.configuration_owner_node_id);
  const auto wait_reason = credential_provisioning_source_readiness(
      owner_endpoint, configuration, now_ms);
  if (source_endpoint == nullptr) {
    this->credential_provisioning_check_ms_ =
        now_ms + (previous_source_failed ? 10000U : 2000U);
    if (previous_source_failed) {
      ESP_LOGW(TAG,
               "Credential provisioning sources exhausted last_source=%s "
               "last_result=%s detail=%s attempted_mask=0x%08x "
               "retry_in=10000ms",
               diagnostics.owner_node_id,
               site_replication_result_to_string(diagnostics.result),
               site_replication_failure_detail_to_string(
                   diagnostics.failure_detail),
               static_cast<unsigned>(
                   this->credential_provisioning_attempted_source_mask_));
      this->site_replication_->cancel();
      this->credential_provisioning_attempted_source_mask_ = 0;
      return;
    }
    if (this->credential_provisioning_wait_reason_ != wait_reason) {
      const uint64_t local_configuration_digest =
          site_configuration_digest(configuration);
      const uint64_t local_topology_digest = site_topology_digest(configuration);
      ESP_LOGW(
          TAG,
          "Credential provisioning waiting reason=%s owner=%s "
          "retry_in=2000ms",
          credential_provisioning_source_readiness_to_string(wait_reason),
          configuration.configuration_owner_node_id);
      ESP_LOGW(
          TAG,
          "Credential owner configuration local_epoch=%u remote_epoch=%u "
          "local_generation=%u remote_generation=%u "
          "local_digest=%08x%08x remote_digest=%08x%08x",
          static_cast<unsigned>(configuration.authority_epoch),
          static_cast<unsigned>(
              owner_endpoint != nullptr
                  ? owner_endpoint->site_configuration_authority_epoch
                  : 0),
          static_cast<unsigned>(configuration.generation),
          static_cast<unsigned>(
              owner_endpoint != nullptr
                  ? owner_endpoint->site_configuration_generation
                  : 0),
          static_cast<unsigned>(local_configuration_digest >> 32U),
          static_cast<unsigned>(local_configuration_digest & 0xFFFFFFFFULL),
          static_cast<unsigned>(
              owner_endpoint != nullptr
                  ? owner_endpoint->site_configuration_digest >> 32U
                  : 0),
          static_cast<unsigned>(
              owner_endpoint != nullptr
                  ? owner_endpoint->site_configuration_digest & 0xFFFFFFFFULL
                  : 0));
      ESP_LOGW(
          TAG,
          "Credential owner topology local_revision=%u "
          "remote_revision=%u local_digest=%08x%08x "
          "remote_digest=%08x%08x remote_wire=%u "
          "remote_credentials_complete=%s",
          static_cast<unsigned>(configuration.roster_revision),
          static_cast<unsigned>(owner_endpoint != nullptr
                                    ? owner_endpoint->site_topology_revision
                                    : 0),
          static_cast<unsigned>(local_topology_digest >> 32U),
          static_cast<unsigned>(local_topology_digest & 0xFFFFFFFFULL),
          static_cast<unsigned>(
              owner_endpoint != nullptr
                  ? owner_endpoint->site_topology_digest >> 32U
                  : 0),
          static_cast<unsigned>(
              owner_endpoint != nullptr
                  ? owner_endpoint->site_topology_digest & 0xFFFFFFFFULL
                  : 0),
          static_cast<unsigned>(owner_endpoint != nullptr
                                    ? owner_endpoint->heartbeat_wire_version
                                    : 0),
          owner_endpoint != nullptr &&
                  owner_endpoint->site_credentials_complete
              ? "true"
              : "false");
      this->credential_provisioning_wait_reason_ = wait_reason;
    }
    return;
  }
  if (this->credential_provisioning_wait_reason_ !=
      CredentialProvisioningSourceReadiness::READY) {
    ESP_LOGI(TAG,
             "Credential provisioning source ready source=%s owner=%s "
             "generation=%u roster=%u previous_wait=%s",
             source_endpoint->participant_id,
             configuration.configuration_owner_node_id,
             static_cast<unsigned>(configuration.generation),
             static_cast<unsigned>(configuration.roster_revision),
             credential_provisioning_source_readiness_to_string(
                 this->credential_provisioning_wait_reason_));
    this->credential_provisioning_wait_reason_ =
        CredentialProvisioningSourceReadiness::READY;
  }
  const bool authority_bootstrap =
      credential_provisioning_source_readiness(source_endpoint, configuration,
                                               now_ms) !=
      CredentialProvisioningSourceReadiness::READY;
  const auto result = this->site_replication_->start_credentials(
      source_endpoint->participant_id, now_ms, authority_bootstrap);
  this->credential_provisioning_check_ms_ =
      now_ms + (result == SiteReplicationResult::OK ? 5000U : 10000U);
  if (result == SiteReplicationResult::OK) {
    ESP_LOGI(TAG,
             "Credential provisioning queued source=%s owner=%s "
             "generation=%u roster=%u local_credentials_complete=%s mode=%s",
             source_endpoint->participant_id,
             configuration.configuration_owner_node_id,
             static_cast<unsigned>(configuration.generation),
             static_cast<unsigned>(configuration.roster_revision),
             credentials_complete ? "true" : "false",
             authority_bootstrap ? "authority_bootstrap" : "authenticated");
  } else {
    ESP_LOGW(
        TAG,
        "Credential provisioning deferred source=%s owner=%s start_result=%s "
        "active_operation=%s active_state=%s active_result=%s "
        "active_detail=%s retry_in=10000ms",
        source_endpoint->participant_id,
        configuration.configuration_owner_node_id,
        site_replication_result_to_string(result),
        site_replication_target_to_string(diagnostics.target),
        site_replication_state_to_string(diagnostics.state),
        site_replication_result_to_string(diagnostics.result),
        site_replication_failure_detail_to_string(diagnostics.failure_detail));
  }
#else
  (void) now_ms;
#endif
}

bool RadarApiServer::site_credential_identity_ready_() const {
  if (!this->site_configuration_ready())
    return false;
  const auto &configuration =
      this->platform_runtime_->site_configuration_snapshot;
  if (std::strcmp(configuration.configuration_owner_node_id,
                  this->platform_identity_.node_id) == 0)
    return true;
  const bool provisioned_identity =
      this->credential_provisioning_verified_ &&
      this->credential_provisioning_authority_epoch_ ==
          configuration.authority_epoch &&
      this->credential_provisioning_roster_revision_ ==
          configuration.roster_revision &&
      std::strcmp(this->credential_provisioning_owner_node_id_,
                  configuration.configuration_owner_node_id) == 0;
  const bool committed_identity = site_credentials_complete_for_node(
      this->platform_identity_, configuration,
      this->canonical_site_directory_ready()
          ? &this->platform_runtime_->site_directory_snapshot
          : nullptr,
      this->platform_runtime_->site_coordinator.state().leader_node_id,
      this->participant_credential_snapshot_.registry);
  return provisioned_identity || committed_identity ||
         site_credential_bootstrap_ready_for_node(
             this->platform_identity_, configuration,
             this->platform_runtime_->participant_registry_snapshot.registry,
             this->participant_credential_snapshot_.registry);
}

StandaloneSiteReconciliationResult
RadarApiServer::reconcile_standalone_site_() {
  if (!this->site_configuration_persistence_ready() ||
      !this->participant_registry_ready() ||
      has_non_revoked_participants(
          this->platform_runtime_->participant_registry_snapshot.registry))
    return StandaloneSiteReconciliationResult::INVALID_STATE;

  if (!is_local_standalone_site_configuration(
          this->platform_identity_,
          this->platform_runtime_->participant_registry_snapshot.registry,
          this->platform_runtime_->site_configuration_snapshot)) {
    const bool replace_site =
        std::strcmp(this->platform_runtime_->site_configuration_snapshot.site_id,
                    this->platform_identity_.site_id) != 0;
    if (reconcile_local_standalone_site_configuration(
            this->platform_identity_,
            this->platform_runtime_->participant_registry_snapshot.registry,
            this->platform_runtime_->site_configuration_snapshot,
            &this->platform_runtime_->site_configuration_update_scratch) !=
            SiteConfigurationResult::OK)
      return StandaloneSiteReconciliationResult::BUILD_FAILED;
    this->storage_.reset_platform_blob_write_diagnostic();
    const auto store_result =
        replace_site
            ? this->site_configuration_store_->replace_for_local_reset(
                  *this->site_configuration_backend_,
                  this->platform_runtime_->site_configuration_update_scratch,
                  &this->platform_runtime_->site_configuration_snapshot)
            : this->site_configuration_store_->update(
                  *this->site_configuration_backend_,
                  this->platform_runtime_->site_configuration_update_scratch,
                  &this->platform_runtime_->site_configuration_snapshot);
    if (store_result != SiteConfigurationStoreResult::UPDATED) {
      const auto &diagnostic =
          this->storage_.last_platform_blob_write_diagnostic();
      ESP_LOGE(
          TAG,
          "Standalone Site persistence failed store=%s flash=%s target=%u "
          "offset=%u error=%d",
          site_configuration_store_result_to_string(store_result),
          radar_platform_blob_write_result_to_string(diagnostic.result),
          static_cast<unsigned>(diagnostic.target),
          static_cast<unsigned>(diagnostic.offset),
          static_cast<int>(diagnostic.error));
      return StandaloneSiteReconciliationResult::SITE_STORAGE_FAILED;
    }
    this->site_configuration_store_result_ =
        SiteConfigurationStoreResult::UPDATED;
    this->reset_configuration_replication_();
  }

  if (!this->reconcile_site_directory_scope_("standalone"))
    return StandaloneSiteReconciliationResult::SITE_STORAGE_FAILED;

#ifdef USE_ESP32
  if (this->platform_site_membership_.mode !=
      SiteMembershipMode::STANDALONE) {
    if (!update_esp32_site_membership(
            this->platform_identity_.site_id,
            SiteMembershipMode::STANDALONE, &this->platform_identity_,
            &this->platform_site_membership_))
      return StandaloneSiteReconciliationResult::
          MEMBERSHIP_STORAGE_FAILED;
  }
#endif
  return StandaloneSiteReconciliationResult::OK;
}

bool RadarApiServer::clear_owned_floorplan_manifest() {
  if (!this->site_configuration_persistence_ready())
    return false;
  if (std::strcmp(
          this->platform_runtime_->site_configuration_snapshot.configuration_owner_node_id,
          this->platform_identity_.node_id) != 0)
    return true;
  if (!this->platform_runtime_->site_configuration_snapshot.floorplan.committed)
    return true;
  if (clear_local_floorplan_manifest(
          this->platform_identity_, this->platform_runtime_->site_configuration_snapshot,
          &this->platform_runtime_->site_configuration_update_scratch) !=
      SiteConfigurationResult::OK)
    return false;
  return this->site_configuration_store_->update(
             *this->site_configuration_backend_,
             this->platform_runtime_->site_configuration_update_scratch,
             &this->platform_runtime_->site_configuration_snapshot) ==
         SiteConfigurationStoreResult::UPDATED;
}

bool RadarApiServer::reset_platform_relationships() {
#ifdef USE_ESP32
  if (!this->platform_identity_ready() ||
      !this->participant_registry_persistence_ready() ||
      !this->site_configuration_persistence_ready() ||
      !this->site_directory_stored_() ||
      this->site_directory_backend_ == nullptr ||
      this->site_directory_store_ == nullptr ||
      !this->participant_readiness_persistence_ready() ||
      !this->participant_credential_persistence_ready())
    return false;

  uint8_t mac[6]{};
  uint8_t site_random[16]{};
  get_mac_address_raw(mac);
  if (!random_bytes(site_random, sizeof(site_random)))
    return false;

  PlatformIdentity standalone_identity{};
  if (create_initial_platform_identity(mac, site_random,
                                       &standalone_identity) !=
          PlatformIdentityCodecResult::OK ||
      std::strcmp(standalone_identity.node_id,
                  this->platform_identity_.node_id) != 0)
    return false;

  if (initialize_site_configuration(standalone_identity, false,
                                    &this->platform_runtime_->site_configuration_update_scratch) !=
      SiteConfigurationResult::OK)
    return false;
  if (migrate_site_configuration_to_directory(
          this->platform_runtime_->site_configuration_update_scratch,
          &this->platform_runtime_->site_directory_update_scratch) !=
      SiteDirectoryResult::OK)
    return false;
  this->platform_runtime_->site_directory_update_scratch.generation =
      this->platform_runtime_->site_directory_snapshot.generation;

  this->participant_credential_snapshot_.registry.clear();
  if (this->participant_credential_store_->update(
          *this->participant_credential_backend_,
          this->participant_credential_snapshot_.registry,
          &this->participant_credential_snapshot_) !=
      ParticipantCredentialStoreResult::UPDATED)
    return false;

  this->platform_runtime_->participant_readiness_snapshot.registry.clear();
  if (this->participant_readiness_store_->update(
          *this->participant_readiness_backend_,
          this->platform_runtime_->participant_readiness_snapshot.registry,
          &this->platform_runtime_->participant_readiness_snapshot) !=
      ParticipantReadinessStoreResult::UPDATED)
    return false;

  this->platform_runtime_->participant_registry_snapshot.registry.clear();
  if (this->participant_registry_store_->update(
          *this->participant_registry_backend_,
          this->platform_runtime_->participant_registry_snapshot.registry,
          &this->platform_runtime_->participant_registry_snapshot) !=
      ParticipantRegistryStoreResult::UPDATED)
    return false;

  if (!update_esp32_site_membership(
          standalone_identity.site_id, SiteMembershipMode::STANDALONE,
          &this->platform_identity_, &this->platform_site_membership_))
    return false;

  if (this->site_configuration_store_->replace_for_local_reset(
          *this->site_configuration_backend_,
          this->platform_runtime_->site_configuration_update_scratch,
          &this->platform_runtime_->site_configuration_snapshot) !=
      SiteConfigurationStoreResult::UPDATED)
    return false;

  if (this->site_directory_store_->update(
          *this->site_directory_backend_,
          this->platform_runtime_->site_directory_update_scratch,
          &this->platform_runtime_->site_directory_snapshot) !=
      SiteDirectoryStoreResult::UPDATED)
    return false;
  this->site_directory_store_result_ = SiteDirectoryStoreResult::UPDATED;
  this->site_directory_reconciliation_result_ =
      SiteDirectoryResult::NOT_READY;
  this->site_directory_reconciliation_check_ms_ = 0;
  this->reset_site_scoped_runtime_("platform_reset");

  uint32_t operation_id = 0;
  this->participant_pairing_runtime_.queue_cancel(&operation_id);
  this->participant_enrollment_.close_window();
  this->standalone_site_reconciliation_.mark_not_required();
  this->reset_configuration_replication_();
  this->local_floorplan_replica_ready_ = false;
  return true;
#else
  return false;
#endif
}

ParticipantPairingCommitResult RadarApiServer::commit_participant_pairing(
    const ParticipantPairingCredential &credential,
    ParticipantPairingRole local_role, const char *leader_site_id,
    uint32_t now_ms) {
#ifdef USE_ESP32
  if (credential.participant_id[0] == '\0' ||
      credential.participant_token == 0 || credential.key_version == 0 ||
      leader_site_id == nullptr || leader_site_id[0] == '\0')
    return ParticipantPairingCommitResult::INVALID_ARGUMENT;
  if (!this->participant_registry_persistence_ready())
    return ParticipantPairingCommitResult::REGISTRY_UNAVAILABLE;
  if (!this->participant_credential_persistence_ready())
    return ParticipantPairingCommitResult::CREDENTIAL_STORAGE_UNAVAILABLE;
  if (!this->standalone_site_reconciliation_.ready_for_pairing())
    return ParticipantPairingCommitResult::INVALID_STATE;

  const auto *existing_participant =
      this->platform_runtime_->participant_registry_snapshot.registry.find(
          credential.participant_id);
  const bool retrying_same_peer =
      existing_participant != nullptr &&
      existing_participant->type == ParticipantType::NAMO_NODE &&
      existing_participant->trust == ParticipantTrust::COMMUNITY_UNVERIFIED &&
      existing_participant->membership == ParticipantMembership::QUARANTINED;
  if (existing_participant != nullptr && !retrying_same_peer)
    return ParticipantPairingCommitResult::INVALID_STATE;
  const bool joiner_site_already_converged =
      local_role == ParticipantPairingRole::JOINER && retrying_same_peer &&
      this->site_configuration_persistence_ready() &&
      pairing_joiner_site_already_converged(
          this->platform_identity_,
          this->platform_runtime_->site_configuration_snapshot,
          leader_site_id, credential.participant_id);
  if (local_role == ParticipantPairingRole::JOINER &&
      this->site_configuration_persistence_ready() &&
      this->platform_runtime_->site_configuration_snapshot
              .configuration_owner_node_id[0] != '\0' &&
      std::strcmp(this->platform_runtime_->site_configuration_snapshot
                      .configuration_owner_node_id,
                  this->platform_identity_.node_id) != 0 &&
      !joiner_site_already_converged)
    return ParticipantPairingCommitResult::SITE_CONFIGURATION_OWNER_REQUIRED;
  if (local_role == ParticipantPairingRole::JOINER &&
      std::strcmp(this->platform_site_membership_.site_id, leader_site_id) != 0) {
    if (!can_move_site_without_orphaning_participants(
            this->platform_runtime_->participant_registry_snapshot.registry,
            this->platform_runtime_->site_configuration_snapshot,
            retrying_same_peer ? credential.participant_id : nullptr))
      return ParticipantPairingCommitResult::SITE_CONFLICT;
  }
  if (local_role == ParticipantPairingRole::LEADER &&
      this->site_configuration_persistence_ready()) {
    const auto preflight_result = prepare_local_pairing_site(
        this->platform_identity_,
        this->platform_runtime_->participant_registry_snapshot.registry,
        this->platform_runtime_->site_configuration_snapshot, credential.participant_id,
        &this->platform_runtime_->site_configuration_update_scratch);
    if (preflight_result == SiteConfigurationResult::NOT_OWNER)
      return ParticipantPairingCommitResult::SITE_CONFIGURATION_OWNER_REQUIRED;
    if (preflight_result == SiteConfigurationResult::FULL)
      return ParticipantPairingCommitResult::REGISTRY_FULL;
    if (preflight_result != SiteConfigurationResult::OK)
      return ParticipantPairingCommitResult::SITE_STATE_INVALID;
  }

  const SiteMembership previous_site_membership =
      this->platform_site_membership_;
  bool site_membership_changed = false;

  if (!retrying_same_peer) {
    auto &enrollment = this->participant_enrollment_;
    if (!enrollment.window_open(now_ms))
      return ParticipantPairingCommitResult::INVALID_STATE;
    auto result = enrollment.observe_candidate(
        this->platform_runtime_->participant_registry_snapshot, credential.participant_id,
        ParticipantType::NAMO_NODE, now_ms);
    if (result == ParticipantEnrollmentResult::REGISTRY_FULL)
      return ParticipantPairingCommitResult::REGISTRY_FULL;
    if (result != ParticipantEnrollmentResult::OK)
      return ParticipantPairingCommitResult::INVALID_STATE;
    result = enrollment.approve_candidate(
        *this->participant_registry_backend_, *this->participant_registry_store_,
        &this->platform_runtime_->participant_registry_snapshot,
        ParticipantTrust::COMMUNITY_UNVERIFIED, now_ms);
    if (result == ParticipantEnrollmentResult::REGISTRY_FULL)
      return ParticipantPairingCommitResult::REGISTRY_FULL;
    if (result != ParticipantEnrollmentResult::OK)
      return ParticipantPairingCommitResult::PERSISTENCE_FAILED;
  }

  const auto *existing_credential =
      this->participant_credential_snapshot_.registry.find(
          credential.participant_id);
  const bool credential_matches =
      existing_credential != nullptr &&
      existing_credential->participant_token == credential.participant_token &&
      existing_credential->key_version == credential.key_version &&
      existing_credential->key == credential.key && existing_credential->enabled;
  if (!credential_matches) {
    ParticipantCredentialRecord backup{};
    const bool had_backup = existing_credential != nullptr;
    if (had_backup)
      backup = *existing_credential;
    const auto set_result = this->participant_credential_snapshot_.registry.set(
        credential.participant_id, credential.participant_token,
        credential.key_version, credential.key.data(), true);
    if (set_result == ParticipantCredentialResult::FULL)
      return ParticipantPairingCommitResult::REGISTRY_FULL;
    if (set_result != ParticipantCredentialResult::OK)
      return ParticipantPairingCommitResult::INVALID_ARGUMENT;
    if (this->participant_credential_store_->update(
            *this->participant_credential_backend_,
            this->participant_credential_snapshot_.registry,
            &this->participant_credential_snapshot_) !=
        ParticipantCredentialStoreResult::UPDATED) {
      if (had_backup) {
        this->participant_credential_snapshot_.registry.set(
            backup.participant_id, backup.participant_token,
            backup.key_version, backup.key.data(), backup.enabled);
      } else {
        this->participant_credential_snapshot_.registry.erase(
            credential.participant_id);
      }
      return ParticipantPairingCommitResult::PERSISTENCE_FAILED;
    }
  }

  if (local_role == ParticipantPairingRole::JOINER &&
      !site_membership_matches_target(this->platform_site_membership_,
                                      leader_site_id,
                                      SiteMembershipMode::ENROLLED)) {
    if (!update_esp32_site_membership(
            leader_site_id, SiteMembershipMode::ENROLLED,
            &this->platform_identity_, &this->platform_site_membership_))
      return ParticipantPairingCommitResult::PERSISTENCE_FAILED;
    site_membership_changed = true;
  }
  if (this->site_configuration_persistence_ready()) {
    if (local_role == ParticipantPairingRole::JOINER &&
        !joiner_site_already_converged) {
      if (initialize_site_configuration(
              this->platform_identity_, true,
              &this->platform_runtime_->site_configuration_update_scratch) !=
          SiteConfigurationResult::OK) {
        if (site_membership_changed)
          update_esp32_site_membership(
              previous_site_membership.site_id,
              previous_site_membership.mode, &this->platform_identity_,
              &this->platform_site_membership_);
        return ParticipantPairingCommitResult::PERSISTENCE_FAILED;
      }
      if (this->site_configuration_store_->replace_for_enrollment(
              *this->site_configuration_backend_,
              this->platform_runtime_->site_configuration_update_scratch,
              &this->platform_runtime_->site_configuration_snapshot) !=
          SiteConfigurationStoreResult::UPDATED) {
        if (site_membership_changed)
          update_esp32_site_membership(
              previous_site_membership.site_id,
              previous_site_membership.mode, &this->platform_identity_,
              &this->platform_site_membership_);
        return ParticipantPairingCommitResult::PERSISTENCE_FAILED;
      }
    } else if (local_role == ParticipantPairingRole::LEADER) {
      const auto roster_result = prepare_local_pairing_site(
          this->platform_identity_,
          this->platform_runtime_->participant_registry_snapshot.registry,
          this->platform_runtime_->site_configuration_snapshot, credential.participant_id,
          &this->platform_runtime_->site_configuration_update_scratch);
      if (roster_result == SiteConfigurationResult::NOT_OWNER)
        return ParticipantPairingCommitResult::SITE_CONFIGURATION_OWNER_REQUIRED;
      if (roster_result == SiteConfigurationResult::FULL)
        return ParticipantPairingCommitResult::REGISTRY_FULL;
      if (roster_result != SiteConfigurationResult::OK)
        return ParticipantPairingCommitResult::SITE_STATE_INVALID;
      if (this->site_configuration_store_->update(
              *this->site_configuration_backend_,
              this->platform_runtime_->site_configuration_update_scratch,
              &this->platform_runtime_->site_configuration_snapshot) !=
              SiteConfigurationStoreResult::UPDATED)
        return ParticipantPairingCommitResult::PERSISTENCE_FAILED;
    }
  }
  if (local_role == ParticipantPairingRole::JOINER &&
      !this->reconcile_site_directory_scope_("pairing_joiner")) {
    // Pairing and the enrollment Configuration are already durable. Directory
    // reconciliation is a recoverable bootstrap phase and is retried by the
    // normal loop; reporting the whole pairing commit as failed here leaves a
    // successful direct pair looking like a failed transaction to the UI.
    ESP_LOGW(TAG,
             "Pairing committed; Site Directory bootstrap remains pending "
             "participant=%s",
             credential.participant_id);
    this->site_directory_reconciliation_check_ms_ = 0;
  }
  this->floorplan_replica_check_ms_ = 0;
  this->standalone_site_reconciliation_.mark_not_required();
  return ParticipantPairingCommitResult::OK;
#else
  (void) credential;
  (void) local_role;
  (void) leader_site_id;
  (void) now_ms;
  return ParticipantPairingCommitResult::PERSISTENCE_FAILED;
#endif
}

void RadarApiServer::send_ld2410_recovery_command_(
    Ld2410RecoveryAction action) {
  if (this->ld2410_ == nullptr)
    return;
  if (action == Ld2410RecoveryAction::ENTER_CONFIGURATION) {
    constexpr uint8_t payload[] = {0x01, 0x00};
    this->ld2410_->send_managed_command(
        LD2410_COMMAND_ENABLE_CONFIGURATION, payload, sizeof(payload));
  } else if (action == Ld2410RecoveryAction::RESTART) {
    this->ld2410_->send_managed_command(LD2410_COMMAND_RESTART, nullptr, 0);
  }
}

void RadarApiServer::send_ld2410_command_(uint8_t command,
                                          const uint8_t *payload,
                                          uint8_t payload_size) {
  if (this->ld2410_ == nullptr ||
      payload_size > Ld2410ManagedAction::MAX_PAYLOAD_SIZE)
    return;
  this->ld2410_->send_managed_command(command, payload, payload_size);
}

void RadarApiServer::quarantine_ld2410_requests_() {
  this->ld2410_tuning_request_pending_ = false;
  this->ld2410_tuning_requested_active_ = false;
  this->ld2410_tuning_retry_after_ms_ = 0;
  this->ld2410_tuning_retry_count_ = 0;
  this->control_state_.pending_static_radar_tuning_session = false;
  this->control_state_.static_radar_recovering = true;
  this->control_state_.static_radar_tuning_requested_active = false;
  this->control_state_.static_radar_tuning_keepalive_ms = 0;
  this->control_state_.static_radar_tuning_active = false;
  this->control_state_.static_radar_tuning_phase =
      StaticRadarTuningPhase::FAILED;
  this->control_state_.static_radar_tuning_result = "sensor_recovering";
  this->control_state_.pending_static_radar_factory_reset = false;
  this->control_state_.fail_static_radar_gate_operation(
      "sensor_recovering");
}

bool RadarApiServer::process_ld2410_recovery_(
    uint32_t now_ms, const Ld2410StreamSnapshot &status,
    bool command_busy) {
  constexpr uint32_t settle_time_ms = 1500;
  constexpr uint32_t settle_frames = 10;

  const auto recovery_action = this->ld2410_watchdog_.update(
      now_ms, status, command_busy, this->ld2410_recovery_requested_);
  if (recovery_action == Ld2410RecoveryAction::ENTER_CONFIGURATION ||
      recovery_action == Ld2410RecoveryAction::RESTART) {
    if (!this->ld2410_recovery_active_) {
      this->quarantine_ld2410_requests_();
      ESP_LOGW("ld2410_health",
               "Command manager entering recovery quarantine frames=%u "
               "ack_sequence=%u",
               static_cast<unsigned>(status.periodic_frames),
               static_cast<unsigned>(status.ack_sequence));
    }
    this->ld2410_recovery_requested_ = false;
    this->ld2410_recovery_active_ = true;
    this->ld2410_recovery_settling_ = false;
    ESP_LOGW("ld2410_health",
             "Watchdog recovery command=%s frames=%u ack=%u "
             "ack_failed=%u config_open=%s",
             recovery_action == Ld2410RecoveryAction::RESTART
                 ? "restart"
                 : "enter_configuration",
             static_cast<unsigned>(status.periodic_frames),
             static_cast<unsigned>(status.ack_frames),
             static_cast<unsigned>(status.failed_ack_frames),
             status.configuration_mode_open ? "yes" : "no");
    this->send_ld2410_recovery_command_(recovery_action);
  } else if (recovery_action == Ld2410RecoveryAction::RECOVERED) {
    this->ld2410_recovery_requested_ = false;
    this->ld2410_recovery_active_ = false;
    this->ld2410_recovery_settling_ = true;
    this->ld2410_recovery_settle_started_ms_ = now_ms;
    this->ld2410_recovery_settle_frame_baseline_ = status.periodic_frames;
    this->ld2410_startup_sync_complete_ = false;
    this->ld2410_startup_stable_since_ms_ = 0;
    ESP_LOGI("ld2410_health",
             "Command manager recovery frame received; settling starts "
             "frames=%u",
             static_cast<unsigned>(status.periodic_frames));
  } else if (recovery_action == Ld2410RecoveryAction::GIVE_UP) {
    this->ld2410_recovery_active_ = true;
    this->ld2410_recovery_settling_ = false;
    ESP_LOGE("ld2410_health",
             "Watchdog recovery attempt timed out; quarantine remains "
             "for %ums last_ack=0x%02X result=%u",
             static_cast<unsigned>(Ld2410Watchdog::RETRY_COOLDOWN_MS),
             static_cast<unsigned>(status.last_ack_command),
             static_cast<unsigned>(status.last_ack_result));
  }

  if (this->ld2410_recovery_settling_) {
    const bool stream_stable = status.valid_frame_seen && !status.stalled &&
        status.last_frame_age_ms < Ld2410StreamHealth::RUNTIME_STALE_MS;
    if (!stream_stable) {
      this->ld2410_recovery_settling_ = false;
      this->ld2410_recovery_active_ = true;
      this->ld2410_recovery_requested_ = true;
      ESP_LOGW("ld2410_health",
               "Command manager settling interrupted; recovery requested");
    } else if (now_ms - this->ld2410_recovery_settle_started_ms_ >=
                   settle_time_ms &&
               status.periodic_frames -
                       this->ld2410_recovery_settle_frame_baseline_ >=
                   settle_frames) {
      this->ld2410_recovery_settling_ = false;
      this->ld2410_stream_health_.observe_restart_settled();
      this->control_state_.static_radar_recovering = false;
      this->control_state_.static_radar_tuning_phase =
          StaticRadarTuningPhase::IDLE;
      this->control_state_.static_radar_tuning_result = "idle";
      this->ld2410_startup_retry_after_ms_ = now_ms + 1000;
      ESP_LOGI("ld2410_health",
               "Command manager recovery settled frames=%u elapsed=%ums",
               static_cast<unsigned>(status.periodic_frames -
                                     this->ld2410_recovery_settle_frame_baseline_),
               static_cast<unsigned>(now_ms -
                                     this->ld2410_recovery_settle_started_ms_));
      return true;
    }
  }

  const bool blocked = this->ld2410_recovery_requested_ ||
                       this->ld2410_recovery_active_ ||
                       this->ld2410_recovery_settling_;
  if (blocked)
    this->quarantine_ld2410_requests_();
  return blocked;
}

bool RadarApiServer::request_static_radar_gate_write(
    uint8_t gate, uint8_t sensitivity, uint32_t now_ms) {
  const auto status = this->ld2410_stream_health_.snapshot(now_ms);
  const bool ready = this->ld2410_ != nullptr &&
                     !this->ld2410_recovery_requested_ &&
                     !this->ld2410_recovery_active_ &&
                     !this->ld2410_recovery_settling_ &&
                     status.valid_frame_seen && !status.stalled &&
                     status.last_frame_age_ms <
                         Ld2410StreamHealth::RUNTIME_STALE_MS &&
                     status.engineering_mode;
  if (!ready || this->ld2410_command_transaction_.busy() ||
      !this->ld2410_command_transaction_.start(
          Ld2410ManagedOperation::GATE_WRITE, now_ms, status, gate,
          sensitivity)) {
    this->control_state_.fail_static_radar_gate_operation(
        ready ? "command_busy" : "tuning_inactive");
    return false;
  }
  ESP_LOGI("ld2410_health",
           "Command transaction started operation=gate_write gate=%u "
           "sensitivity=%u",
           static_cast<unsigned>(gate), static_cast<unsigned>(sensitivity));
  return true;
}

void RadarApiServer::finish_ld2410_command_transaction_(
    const Ld2410ManagedAction &action, uint32_t now_ms) {
  const bool completed = action.event == Ld2410ManagedEvent::COMPLETED;
  const char *result = completed ? "ok"
                                 : ld2410_managed_failure_name(action.failure);
  if (completed) {
    ESP_LOGI("ld2410_health",
             "Command transaction completed operation=%s ignored_acks=%u",
             ld2410_managed_operation_name(action.operation),
             static_cast<unsigned>(action.ignored_acks));
  } else {
    ESP_LOGE("ld2410_health",
             "Command transaction failed operation=%s reason=%s "
             "command=0x%02X ack_result=%u ignored_acks=%u "
             "last_ignored=0x%02X/%u",
             ld2410_managed_operation_name(action.operation), result,
             static_cast<unsigned>(action.command),
             static_cast<unsigned>(action.ack_result),
             static_cast<unsigned>(action.ignored_acks),
             static_cast<unsigned>(action.last_ignored_command),
             static_cast<unsigned>(action.last_ignored_result));
    this->ld2410_recovery_requested_ = true;
  }

  switch (action.operation) {
    case Ld2410ManagedOperation::STARTUP_SYNC:
      this->ld2410_startup_sync_complete_ = completed;
      this->ld2410_startup_retry_after_ms_ =
          completed ? 0 : now_ms + Ld2410Watchdog::RETRY_COOLDOWN_MS;
      this->ld2410_startup_stable_since_ms_ = 0;
      break;
    case Ld2410ManagedOperation::TUNING_START:
    case Ld2410ManagedOperation::TUNING_STOP: {
      const bool operation_active =
          action.operation == Ld2410ManagedOperation::TUNING_START;
      if (completed) {
        this->control_state_.static_radar_tuning_active = operation_active;
        this->ld2410_tuning_retry_count_ = 0;
        this->ld2410_tuning_retry_after_ms_ = 0;
        if (this->ld2410_tuning_requested_active_ == operation_active) {
          this->control_state_.static_radar_tuning_phase =
              operation_active ? StaticRadarTuningPhase::ACTIVE
                               : StaticRadarTuningPhase::IDLE;
          this->control_state_.static_radar_tuning_result =
              operation_active ? "ok" : "idle";
        }
      } else if (this->ld2410_tuning_requested_active_ == operation_active) {
        this->ld2410_tuning_request_pending_ = false;
        this->ld2410_tuning_retry_after_ms_ = 0;
        this->control_state_.static_radar_tuning_phase =
            StaticRadarTuningPhase::FAILED;
        this->control_state_.static_radar_tuning_result = result;
      }
      break;
    }
    case Ld2410ManagedOperation::GATE_WRITE:
      if (!completed)
        this->control_state_.fail_static_radar_gate_operation(result);
      break;
    case Ld2410ManagedOperation::FACTORY_RESET:
      this->ld2410_tuning_request_pending_ = false;
      this->ld2410_tuning_retry_after_ms_ = 0;
      this->ld2410_tuning_retry_count_ = 0;
      this->control_state_.static_radar_tuning_active = false;
      this->control_state_.static_radar_tuning_phase =
          completed ? StaticRadarTuningPhase::IDLE
                    : StaticRadarTuningPhase::FAILED;
      this->control_state_.static_radar_tuning_result =
          completed ? "factory_reset_complete" : result;
      this->ld2410_startup_sync_complete_ = false;
      this->ld2410_startup_stable_since_ms_ = 0;
      this->ld2410_startup_retry_after_ms_ = completed ? now_ms + 1000 :
          now_ms + Ld2410Watchdog::RETRY_COOLDOWN_MS;
      break;
    case Ld2410ManagedOperation::NONE:
    default:
      break;
  }

  // A failed command always hands ownership to recovery. Apply this after the
  // operation-specific result handling so stale UI state cannot overwrite the
  // recovery quarantine with the original timeout or ACK error.
  if (!completed)
    this->quarantine_ld2410_requests_();
}

void RadarApiServer::process_ld2410_command_transaction_(
    uint32_t now_ms, const Ld2410StreamSnapshot &status) {
  bool requested_tuning_active = false;
  if (this->take_static_radar_tuning_session_request(
          now_ms, &requested_tuning_active)) {
    this->ld2410_tuning_request_pending_ = true;
    this->ld2410_tuning_requested_active_ = requested_tuning_active;
    this->ld2410_tuning_retry_after_ms_ = now_ms;
    this->ld2410_tuning_retry_count_ = 0;
    this->ld2410_startup_stable_since_ms_ = 0;
  }

  const bool stream_ready = status.valid_frame_seen && !status.stalled &&
      status.last_frame_age_ms < Ld2410StreamHealth::RUNTIME_STALE_MS;
  if (!this->ld2410_command_transaction_.busy()) {
    if (this->ld2410_tuning_request_pending_ && stream_ready &&
        static_cast<int32_t>(now_ms -
                             this->ld2410_tuning_retry_after_ms_) >= 0) {
      const bool already_in_requested_mode =
          status.engineering_mode == this->ld2410_tuning_requested_active_;
      if (already_in_requested_mode) {
        this->ld2410_tuning_request_pending_ = false;
        this->control_state_.static_radar_tuning_active =
            this->ld2410_tuning_requested_active_;
        this->control_state_.static_radar_tuning_phase =
            this->ld2410_tuning_requested_active_
                ? StaticRadarTuningPhase::ACTIVE
                : StaticRadarTuningPhase::IDLE;
        this->control_state_.static_radar_tuning_result =
            this->ld2410_tuning_requested_active_ ? "ok" : "idle";
        this->ld2410_tuning_retry_after_ms_ = 0;
        this->ld2410_tuning_retry_count_ = 0;
      } else if (this->ld2410_command_transaction_.start(
                     this->ld2410_tuning_requested_active_
                         ? Ld2410ManagedOperation::TUNING_START
                         : Ld2410ManagedOperation::TUNING_STOP,
                     now_ms, status)) {
        this->ld2410_tuning_request_pending_ = false;
        if (this->ld2410_tuning_retry_count_ != UINT8_MAX)
          this->ld2410_tuning_retry_count_++;
        ESP_LOGI("ld2410_health",
                 "Command transaction started operation=%s attempt=%u",
                 this->ld2410_tuning_requested_active_ ? "tuning_start"
                                                       : "tuning_stop",
                 static_cast<unsigned>(this->ld2410_tuning_retry_count_));
      }
    } else if (stream_ready &&
               this->take_static_radar_factory_reset_request()) {
      this->ld2410_tuning_request_pending_ = false;
      this->ld2410_tuning_requested_active_ = false;
      this->ld2410_tuning_retry_after_ms_ = 0;
      this->ld2410_tuning_retry_count_ = 0;
      if (this->ld2410_command_transaction_.start(
              Ld2410ManagedOperation::FACTORY_RESET, now_ms, status)) {
        ESP_LOGW("ld2410_health",
                 "Command transaction started operation=factory_reset");
      }
    } else if (!this->ld2410_startup_sync_complete_ && stream_ready &&
               !status.engineering_mode &&
               !this->control_state_.static_radar_tuning_requested_active &&
               !this->ld2410_tuning_request_pending_ &&
               static_cast<int32_t>(now_ms -
                                    this->ld2410_startup_retry_after_ms_) >= 0) {
      if (this->ld2410_startup_stable_since_ms_ == 0) {
        this->ld2410_startup_stable_since_ms_ = now_ms;
      } else if (now_ms - this->ld2410_startup_stable_since_ms_ >= 1000 &&
                 this->ld2410_command_transaction_.start(
                     Ld2410ManagedOperation::STARTUP_SYNC, now_ms, status)) {
        ESP_LOGI("ld2410_health",
                 "Command transaction started operation=startup_sync "
                 "frames=%u",
                 static_cast<unsigned>(status.periodic_frames));
      }
    } else if (!stream_ready) {
      this->ld2410_startup_stable_since_ms_ = 0;
    }
  }

  const auto action =
      this->ld2410_command_transaction_.update(now_ms, status);
  if (action.event == Ld2410ManagedEvent::SEND_COMMAND) {
    ESP_LOGD("ld2410_health",
             "Command transaction send operation=%s command=0x%02X",
             ld2410_managed_operation_name(action.operation),
             static_cast<unsigned>(action.command));
    this->send_ld2410_command_(action.command, action.payload,
                              action.payload_size);
    if (action.operation == Ld2410ManagedOperation::GATE_WRITE &&
        action.command == 0x61) {
      this->control_state_.mark_static_radar_gate_readback_pending(now_ms);
    }
  } else if (action.event == Ld2410ManagedEvent::COMPLETED ||
             action.event == Ld2410ManagedEvent::FAILED) {
    this->finish_ld2410_command_transaction_(action, now_ms);
  }
}

void RadarApiServer::loop() {
  if (this->ld2410_uart_ != nullptr) {
    const uint32_t ld2410_now_ms = millis();
    const auto action = this->ld2410_stream_health_.update(ld2410_now_ms);
    const auto status =
        this->ld2410_stream_health_.snapshot(ld2410_now_ms);
    if (action != Ld2410StreamAction::NONE) {
      if (action == Ld2410StreamAction::RX_STARTED) {
        ESP_LOGI(
            "ld2410_health",
            "UART RX started after %ums",
            static_cast<unsigned>(status.first_byte_delay_ms));
      } else if (action == Ld2410StreamAction::STARTUP_HEALTHY) {
        ESP_LOGI(
            "ld2410_health",
            "Periodic stream healthy first_frame=%ums frames=%u ack=%u "
            "ack_failed=%u invalid=%u",
            static_cast<unsigned>(status.first_frame_delay_ms),
            static_cast<unsigned>(status.periodic_frames),
            static_cast<unsigned>(status.ack_frames),
            static_cast<unsigned>(status.failed_ack_frames),
            static_cast<unsigned>(status.invalid_frames));
      } else if (action == Ld2410StreamAction::STARTUP_NO_DATA) {
        ESP_LOGW(
            "ld2410_health",
            "No UART RX data after startup grace rx_bytes=0 "
            "last_byte_age=%ums",
            static_cast<unsigned>(status.last_byte_age_ms));
      } else if (action ==
                 Ld2410StreamAction::STARTUP_NO_VALID_FRAME) {
        ESP_LOGW(
            "ld2410_health",
            "UART RX data present but no valid periodic frame rx_bytes=%u "
            "ack=%u ack_failed=%u last_ack=0x%02X result=%u "
            "config_open=%s invalid=%u discarded=%u last_byte_age=%ums",
            static_cast<unsigned>(status.rx_bytes),
            static_cast<unsigned>(status.ack_frames),
            static_cast<unsigned>(status.failed_ack_frames),
            static_cast<unsigned>(status.last_ack_command),
            static_cast<unsigned>(status.last_ack_result),
            status.configuration_mode_open ? "yes" : "no",
            static_cast<unsigned>(status.invalid_frames),
            static_cast<unsigned>(status.discarded_bytes),
            static_cast<unsigned>(status.last_byte_age_ms));
      } else if (action ==
                 Ld2410StreamAction::STREAM_STALLED_SILENT) {
        ESP_LOGW(
            "ld2410_health",
            "Periodic stream stalled with silent UART last_byte_age=%ums "
            "last_frame_age=%ums frames=%u",
            static_cast<unsigned>(status.last_byte_age_ms),
            static_cast<unsigned>(status.last_frame_age_ms),
            static_cast<unsigned>(status.periodic_frames));
      } else if (action ==
                 Ld2410StreamAction::STREAM_STALLED_INVALID) {
        ESP_LOGW(
            "ld2410_health",
            "Periodic stream stalled while UART bytes continue "
            "last_byte_age=%ums last_frame_age=%ums rx_bytes=%u invalid=%u "
            "discarded=%u",
            static_cast<unsigned>(status.last_byte_age_ms),
            static_cast<unsigned>(status.last_frame_age_ms),
            static_cast<unsigned>(status.rx_bytes),
            static_cast<unsigned>(status.invalid_frames),
            static_cast<unsigned>(status.discarded_bytes));
      } else if (action == Ld2410StreamAction::STREAM_RESUMED) {
        ESP_LOGI(
            "ld2410_health",
            "Periodic stream resumed downtime=%ums frames=%u invalid=%u "
            "target=%u distance=%ucm",
            static_cast<unsigned>(status.stall_duration_ms),
            static_cast<unsigned>(status.periodic_frames),
            static_cast<unsigned>(status.invalid_frames),
            static_cast<unsigned>(status.target_state),
            static_cast<unsigned>(status.detection_distance_cm));
      } else if (action ==
                 Ld2410StreamAction::UNHEALTHY_SUMMARY) {
        ESP_LOGW(
            "ld2410_health",
            "Stream remains unhealthy stalled=%ums rx_bytes=%u frames=%u "
            "ack=%u ack_failed=%u last_ack=0x%02X result=%u "
            "config_open=%s invalid=%u discarded=%u last_byte_age=%ums "
            "last_frame_age=%ums",
            static_cast<unsigned>(status.stall_duration_ms),
            static_cast<unsigned>(status.rx_bytes),
            static_cast<unsigned>(status.periodic_frames),
            static_cast<unsigned>(status.ack_frames),
            static_cast<unsigned>(status.failed_ack_frames),
            static_cast<unsigned>(status.last_ack_command),
            static_cast<unsigned>(status.last_ack_result),
            status.configuration_mode_open ? "yes" : "no",
            static_cast<unsigned>(status.invalid_frames),
            static_cast<unsigned>(status.discarded_bytes),
            static_cast<unsigned>(status.last_byte_age_ms),
            static_cast<unsigned>(status.last_frame_age_ms));
      }
    }

    const bool command_busy = this->ld2410_command_transaction_.busy();
    const bool recovery_blocks_commands = this->process_ld2410_recovery_(
        ld2410_now_ms, status, command_busy);
    if (!recovery_blocks_commands)
      this->process_ld2410_command_transaction_(ld2410_now_ms, status);
  }
  if (this->ld2450_ != nullptr) {
    const uint32_t ld2450_now_ms = millis();
    const auto startup_action =
        this->ld2450_startup_guard_.update(ld2450_now_ms);
    if (startup_action == Ld2450StartupAction::STARTUP_HEALTHY) {
      ESP_LOGI(TAG,
               "LD2450 periodic stream healthy after startup frames=%u "
               "last_frame_age=%ums",
               static_cast<unsigned>(
                   this->ld2450_startup_guard_.valid_frame_count()),
               static_cast<unsigned>(
                   this->ld2450_startup_guard_.last_valid_frame_age_ms(
                       ld2450_now_ms)));
    } else if (startup_action == Ld2450StartupAction::STARTUP_MISSING) {
      ESP_LOGW(
          TAG,
          "LD245x produced no valid periodic frame after startup "
          "rx_bytes=%u valid=%u invalid=%u parser_resets=%u "
          "bounded_recovery_started=%s",
          static_cast<unsigned>(this->ld2450_->raw_rx_bytes()),
          static_cast<unsigned>(this->ld2450_->valid_periodic_frames()),
          static_cast<unsigned>(this->ld2450_->invalid_periodic_frames()),
          static_cast<unsigned>(this->ld2450_->parser_resets()),
          this->ld2450_->startup_started_without_frame() ? "yes" : "no");
    } else if (startup_action == Ld2450StartupAction::STREAM_STALLED) {
      ESP_LOGW(TAG,
               "LD2450 periodic stream stalled last_frame_age=%ums frames=%u; "
               "diagnostics only, no automatic recovery",
               static_cast<unsigned>(
                   this->ld2450_startup_guard_.last_valid_frame_age_ms(
                       ld2450_now_ms)),
               static_cast<unsigned>(
                   this->ld2450_startup_guard_.valid_frame_count()));
    } else if (startup_action == Ld2450StartupAction::STREAM_RESUMED) {
      ESP_LOGI(TAG,
               "LD2450 periodic stream resumed frames=%u "
               "last_frame_age=%ums",
               static_cast<unsigned>(
                   this->ld2450_startup_guard_.valid_frame_count()),
               static_cast<unsigned>(
                   this->ld2450_startup_guard_.last_valid_frame_age_ms(
                       ld2450_now_ms)));
    }
  }
#ifdef USE_ESP32
  const uint32_t now_ms = millis();
  this->process_standalone_site_reconciliation_(now_ms);
  this->participant_discovery_runtime_.loop(now_ms);
  {
    auto operation_lease = this->participant_operation_.try_acquire();
    if (operation_lease.acquired()) {
      ParticipantPairingPendingCommit pending_commit{};
      if (this->participant_pairing_runtime_.process(
              this->participant_pairing_crypto_, this->platform_identity_,
              now_ms, &pending_commit)) {
        const auto result = this->commit_participant_pairing(
            pending_commit.credential, pending_commit.local_role,
            pending_commit.leader_site_id, now_ms);
        this->participant_pairing_runtime_.complete_commit(result, now_ms);
      }
    }
  }
  this->participant_pairing_runtime_.expire(now_ms);
  if (this->platform_identity_ready() && this->participant_registry_ready() &&
      this->participant_readiness_ready() &&
      now_ms - this->coordinator_last_update_ms_ >= 500U) {
    const auto *local_directory =
        this->canonical_site_directory_ready()
            ? &this->platform_runtime_->site_directory_snapshot
            : nullptr;
    this->platform_runtime_->site_coordinator.set_local_directory_fence(
        local_directory != nullptr ? local_directory->topology_revision : 0,
        local_directory != nullptr
            ? site_directory_payload_hash(*local_directory)
            : 0);
    const bool canonical_configuration = this->site_configuration_ready();
    const bool local_site_credentials_complete =
        !canonical_configuration ||
        site_credentials_complete_for_node(
            this->platform_identity_,
            this->platform_runtime_->site_configuration_snapshot,
            local_directory,
            this->platform_runtime_->site_coordinator.state().leader_node_id,
            this->participant_credential_snapshot_.registry);
    this->platform_runtime_->site_coordinator.update(
        this->platform_identity_, this->platform_runtime_->participant_registry_snapshot.registry,
        canonical_configuration
            ? &this->platform_runtime_->site_configuration_snapshot
            : nullptr,
        canonical_configuration ? local_directory : nullptr,
        local_site_credentials_complete,
        now_ms);
    this->platform_runtime_->group_coordinator.update(
        this->platform_identity_, this->platform_runtime_->participant_registry_snapshot.registry,
        this->platform_runtime_->participant_readiness_snapshot.registry,
        canonical_configuration
            ? &this->platform_runtime_->site_configuration_snapshot
            : nullptr,
        this->group_roster_ready()
            ? &this->platform_runtime_->group_roster_snapshot
            : nullptr,
        canonical_configuration
            ? &this->participant_credential_snapshot_.registry
            : nullptr,
        canonical_configuration && this->canonical_site_directory_ready()
            ? &this->platform_runtime_->site_directory_snapshot
            : nullptr,
        this->platform_runtime_->site_coordinator.state().leader_node_id,
        now_ms);
    const bool group_roster_changed =
        this->reconcile_group_roster_("coordinator");
    if (group_roster_changed && this->group_roster_ready()) {
      this->platform_runtime_->group_coordinator.update(
          this->platform_identity_,
          this->platform_runtime_->participant_registry_snapshot.registry,
          this->platform_runtime_->participant_readiness_snapshot.registry,
          canonical_configuration
              ? &this->platform_runtime_->site_configuration_snapshot
              : nullptr,
          &this->platform_runtime_->group_roster_snapshot,
          canonical_configuration
              ? &this->participant_credential_snapshot_.registry
              : nullptr,
          canonical_configuration && this->canonical_site_directory_ready()
              ? &this->platform_runtime_->site_directory_snapshot
              : nullptr,
          this->platform_runtime_->site_coordinator.state().leader_node_id,
          now_ms);
    }
    this->publish_coordinator_snapshot_();
    this->coordinator_last_update_ms_ = now_ms;
    this->record_internal_ram_watermark("site_coordination", "update");
  }
  this->process_site_directory_reconciliation_(now_ms);
  if (this->site_heartbeat_ != nullptr && this->platform_identity_ready() &&
      this->participant_registry_ready() &&
      this->participant_readiness_ready()) {
    const uint32_t group_summary_generation_before =
        this->platform_runtime_->site_group_summaries.generation();
    const uint32_t authenticated_before =
        this->site_heartbeat_->diagnostics().authenticated;
    const uint32_t received_before =
        this->site_heartbeat_->diagnostics().received;
    const uint32_t sent_before = this->site_heartbeat_->diagnostics().sent;
    if (this->floorplan_replica_check_ms_ == 0 ||
        now_ms - this->floorplan_replica_check_ms_ >=
            SiteHeartbeatTransport::HEARTBEAT_INTERVAL_MS) {
      const auto storage_status = this->storage_.status();
      this->local_floorplan_stored_ =
          storage_status.ok && storage_status.header.config_size > 0 &&
          storage_status.header.image_size > 0;
      this->local_floorplan_config_hash_ =
          this->local_floorplan_stored_
              ? storage_status.header.config_hash
              : 0;
      this->local_floorplan_image_hash_ =
          this->local_floorplan_stored_
              ? storage_status.header.image_hash
              : 0;
      this->local_floorplan_replica_ready_ =
          this->site_configuration_ready() &&
          this->platform_runtime_->site_configuration_snapshot.floorplan.committed &&
          storage_status.ok && storage_status.header.config_size > 0 &&
          storage_status.header.image_size > 0 &&
          storage_status.header.config_hash ==
              this->platform_runtime_->site_configuration_snapshot.floorplan.config_hash &&
          storage_status.header.image_hash ==
              this->platform_runtime_->site_configuration_snapshot.floorplan.image_hash;
      this->floorplan_replica_check_ms_ = now_ms;
    }
    uint32_t local_pose_version = 0;
    {
      const auto config_read = this->device_config_cache_.read();
      local_pose_version = config_read->floorplan_pose_version();
    }
    this->record_internal_ram_checkpoint("site_heartbeat", "entry");
    this->site_heartbeat_->loop(
        this->platform_identity_, this->platform_site_membership_,
        this->platform_runtime_->participant_registry_snapshot.registry,
        this->platform_runtime_->participant_readiness_snapshot.registry,
        this->site_configuration_ready()
            ? &this->platform_runtime_->site_configuration_snapshot
            : nullptr,
        this->canonical_site_directory_ready()
            ? &this->platform_runtime_->site_directory_snapshot
            : nullptr,
        this->site_configuration_ready()
            ? &this->platform_runtime_->site_configuration_snapshot.floorplan
            : nullptr,
        this->site_configuration_ready()
            ? this->platform_runtime_->site_configuration_snapshot.generation
            : 0,
        local_pose_version,
        this->local_floorplan_replica_ready_,
        this->site_credential_identity_ready_(),
        &this->platform_runtime_->site_coordinator,
        &this->platform_runtime_->group_coordinator,
        &this->platform_runtime_->site_group_summaries,
        &this->platform_runtime_->site_floorplan_owners, now_ms);
    this->record_internal_ram_checkpoint("site_heartbeat", "loop_exit");
    if (this->site_heartbeat_->diagnostics().authenticated !=
        authenticated_before)
      this->refresh_site_authority_(now_ms);
    if (this->site_heartbeat_->diagnostics().received != received_before ||
        this->site_heartbeat_->diagnostics().sent != sent_before)
      this->record_internal_ram_watermark("site_heartbeat", "packet");
    if (this->platform_runtime_->site_coordinator.state().local_leader &&
        this->platform_runtime_->site_group_summaries.size() != 0 &&
        this->platform_runtime_->site_group_summaries.generation() !=
            group_summary_generation_before) {
      this->site_directory_reconciliation_check_ms_ = 0;
      this->process_site_directory_reconciliation_(now_ms);
    }
  }
  this->process_spatial_readiness_invalidation_(now_ms);
  this->process_automatic_configuration_handover_(now_ms);
  if (this->site_replication_ != nullptr &&
      this->platform_identity_ready() && this->participant_registry_ready() &&
      this->site_configuration_ready()) {
    this->record_internal_ram_checkpoint("site_replication", "entry");
    const auto &local_configuration =
        this->platform_runtime_->site_configuration_snapshot;
    auto &replication_tracker =
        this->platform_runtime_->site_authority_replication;
    auto candidate = select_enrollment_replication_candidate(
        this->platform_identity_, local_configuration,
        this->platform_runtime_->participant_registry_snapshot.registry,
        this->participant_credential_snapshot_.registry,
        this->site_heartbeat_ != nullptr
            ? &this->site_heartbeat_->endpoints()
            : nullptr,
        now_ms);
    const bool enrollment_candidate_available = candidate.available;
    if (enrollment_candidate_available) {
      const auto *source_endpoint =
          this->site_heartbeat_ != nullptr
              ? this->site_heartbeat_->endpoints().find(
                    candidate.source_node_id)
              : nullptr;
      if (!automatic_enrollment_floorplan_replication_allowed(
              candidate, source_endpoint, this->local_floorplan_stored_,
              this->local_floorplan_config_hash_,
              this->local_floorplan_image_hash_))
        candidate = {};
    }
    if (!candidate.available && !enrollment_candidate_available) {
      candidate = select_site_replication_candidate(
          this->platform_identity_, local_configuration,
          this->site_directory_ready()
              ? &this->platform_runtime_->site_directory_snapshot
              : nullptr,
          this->site_heartbeat_ != nullptr
              ? &this->site_heartbeat_->endpoints()
              : nullptr,
          now_ms);
    }
    replication_tracker.observe_candidate(candidate, now_ms);

    const auto replication_state =
        this->site_replication_->diagnostics().state;
    replication_tracker.observe_transport(local_configuration,
                                          replication_state, now_ms);
    const auto *scheduled_endpoint =
        this->site_heartbeat_ != nullptr
            ? this->site_heartbeat_->endpoints().find(
                  replication_tracker.source_node_id())
            : nullptr;
    if (replication_tracker.start_due(now_ms, replication_state) &&
        ParticipantEndpointCache::is_online(scheduled_endpoint, now_ms)) {
      const bool candidate_matches_pending =
          candidate.available &&
          candidate.authority_epoch ==
              replication_tracker.pending_authority_epoch() &&
          candidate.generation == replication_tracker.pending_generation() &&
          candidate.mode == replication_tracker.replication_mode() &&
          std::strcmp(candidate.source_node_id,
                      replication_tracker.source_node_id()) == 0;
      const auto *authority_evidence =
          candidate_matches_pending && candidate.evidence.confirmed()
              ? &candidate.evidence
              : nullptr;
      if (replication_tracker.replication_mode() !=
              SiteConfigurationReplicationMode::QUORUM_AUTHORITY_REJOIN ||
          authority_evidence != nullptr) {
        const auto start_result = this->site_replication_->start(
            replication_tracker.source_node_id(), now_ms,
            replication_tracker.replication_mode(),
            replication_tracker.route(), authority_evidence);
        replication_tracker.complete_start(start_result, now_ms);
      }
    }
    this->process_automatic_site_directory_replication_(now_ms);
    this->process_automatic_credential_provisioning_(now_ms);
    this->record_internal_ram_checkpoint("site_replication", "scheduled");
    const uint32_t authority_epoch_before_replication =
        this->platform_runtime_->site_configuration_snapshot.authority_epoch;
    const uint32_t generation_before_replication =
        this->platform_runtime_->site_configuration_snapshot.generation;
    this->record_internal_ram_checkpoint("site_replication", "loop_entry");
    this->site_replication_->loop(
        this->platform_identity_,
        this->platform_runtime_->participant_registry_snapshot.registry,
        &this->platform_runtime_->site_configuration_snapshot,
        this->site_configuration_backend_, this->site_configuration_store_,
        &this->platform_runtime_->site_directory_snapshot,
        this->site_directory_backend_, this->site_directory_store_,
        this->platform_runtime_->site_coordinator.state(), now_ms);
    this->record_internal_ram_checkpoint("site_replication", "loop_exit");
    const auto &replication_after = this->site_replication_->diagnostics();
    if (site_replication_is_active(replication_after.state) ||
        this->site_replication_->source_diagnostics().active)
      this->record_internal_ram_watermark("site_replication", "active");
    if ((this->platform_runtime_->site_configuration_snapshot.authority_epoch !=
             authority_epoch_before_replication ||
         this->platform_runtime_->site_configuration_snapshot.generation !=
             generation_before_replication) &&
        this->platform_site_membership_.mode ==
            SiteMembershipMode::STANDALONE &&
        local_node_is_enrolled_site_member(
            this->platform_identity_, this->platform_runtime_->site_configuration_snapshot)) {
      if (update_esp32_site_membership(
              this->platform_identity_.site_id,
              SiteMembershipMode::ENROLLED, &this->platform_identity_,
              &this->platform_site_membership_)) {
        ESP_LOGI(TAG,
                 "Updated Site membership after configuration handover");
      } else {
        ESP_LOGW(TAG,
                 "Could not update Site membership after configuration "
                 "handover");
      }
    }
  }
  if (this->observation_transport_ != nullptr &&
      this->platform_identity_ready() && this->participant_registry_ready() &&
      this->participant_readiness_ready()) {
    const uint32_t shadow_now_ms = millis();
    this->record_internal_ram_checkpoint("observation_cycle", "entry");
    this->observation_transport_->loop(
        this->platform_runtime_->participant_registry_snapshot.registry,
        this->platform_runtime_->participant_readiness_snapshot.registry,
        this->platform_runtime_->group_coordinator,
        this->site_configuration_ready()
            ? &this->platform_runtime_->site_configuration_snapshot
            : nullptr,
        this->group_roster_ready()
            ? &this->platform_runtime_->group_roster_snapshot
            : nullptr,
        this->platform_runtime_->site_coordinator.state(),
        &this->platform_runtime_->site_space_summaries,
        this->platform_identity_.node_id,
        this->platform_site_membership_.site_id,
        shadow_now_ms);
    this->record_internal_ram_checkpoint("observation_cycle", "receive");
    const bool outbound_identity_ready =
        this->site_credential_identity_ready_();
    if (this->site_heartbeat_ != nullptr && outbound_identity_ready) {
      uint32_t local_pose_version = 0;
      {
        const auto config_read = this->device_config_cache_.read();
        local_pose_version = config_read->floorplan_pose_version();
      }
      this->observation_transport_->send(
          this->platform_identity_,
          this->platform_runtime_->participant_registry_snapshot.registry,
          this->platform_runtime_->participant_readiness_snapshot.registry,
          this->site_heartbeat_->endpoints(),
          this->platform_runtime_->group_coordinator,
          this->site_configuration_ready()
              ? &this->platform_runtime_->site_configuration_snapshot
              : nullptr,
          this->group_roster_ready()
              ? &this->platform_runtime_->group_roster_snapshot
              : nullptr,
          this->local_observation_adapter_.output(),
          local_pose_version,
          this->platform_site_membership_.site_id, shadow_now_ms);
      this->record_internal_ram_checkpoint("observation_cycle",
                                           "observation_send");
    }
    if (this->spatial_shadow_runtime_ != nullptr &&
        (this->spatial_shadow_last_update_ms_ == 0 ||
         shadow_now_ms - this->spatial_shadow_last_update_ms_ >= 50)) {
      this->spatial_shadow_last_update_ms_ = shadow_now_ms;
      this->record_internal_ram_checkpoint("observation_cycle",
                                           "fusion_entry");
      this->update_spatial_shadow_runtime_(shadow_now_ms);
      this->record_internal_ram_checkpoint("observation_cycle",
                                           "fusion_update");
      if (this->site_heartbeat_ != nullptr && outbound_identity_ready) {
        this->observation_transport_->send_fusion_snapshot(
            this->platform_identity_,
            this->platform_runtime_->participant_registry_snapshot.registry,
            this->platform_runtime_->participant_readiness_snapshot.registry,
            this->site_heartbeat_->endpoints(),
            this->platform_runtime_->group_coordinator,
            this->site_configuration_ready()
                ? &this->platform_runtime_->site_configuration_snapshot
                : nullptr,
            this->group_roster_ready()
                ? &this->platform_runtime_->group_roster_snapshot
                : nullptr,
            this->spatial_shadow_runtime_->output(),
            this->platform_runtime_->spatial_production_fusion.output(),
            this->platform_site_membership_.site_id, shadow_now_ms);
        this->record_internal_ram_checkpoint("observation_cycle",
                                             "snapshot_send");
        this->observation_transport_->send_site_space_summary(
            this->platform_identity_, this->site_heartbeat_->endpoints(),
            this->platform_runtime_->group_coordinator,
            this->platform_runtime_->site_coordinator.state(),
            this->site_configuration_ready()
                ? &this->platform_runtime_->site_configuration_snapshot
                : nullptr,
            this->platform_runtime_->spatial_production_fusion.output(),
            this->platform_site_membership_.site_id,
            &this->platform_runtime_->site_space_summaries, shadow_now_ms);
        this->record_internal_ram_checkpoint("observation_cycle",
                                             "summary_send");
      }
      this->record_internal_ram_watermark("observation_fusion", "cycle");
    }
  }
#endif
}

SiteReplicationResult RadarApiServer::start_site_replication(
    const char *owner_node_id) {
#ifdef USE_ESP32
  if (this->site_replication_ == nullptr)
    return SiteReplicationResult::SOCKET_ERROR;
  if (!this->platform_identity_ready() ||
      !this->site_configuration_ready())
    return SiteReplicationResult::INVALID_CONFIGURATION;

  if (authenticated_enrollment_source_allowed(
          this->platform_identity_,
          this->platform_runtime_->site_configuration_snapshot,
          this->platform_runtime_->participant_registry_snapshot.registry,
          this->participant_credential_snapshot_.registry, owner_node_id)) {
    this->reset_configuration_replication_();
    if (this->site_replication_->diagnostics().target ==
            SiteReplicationTarget::SITE_DIRECTORY &&
        this->site_replication_->diagnostics().state !=
            SiteReplicationState::IDLE)
      this->site_replication_->cancel();
    this->auto_directory_replication_active_ = false;
    this->auto_directory_replication_due_ms_ =
        millis() + SITE_DIRECTORY_REPLICATION_INTERVAL_MS;
    return this->site_replication_->start(
        owner_node_id, millis(),
        SiteConfigurationReplicationMode::AUTHENTICATED_ENROLLMENT);
  }

  const bool pairing_owner_requested =
      owner_node_id != nullptr && owner_node_id[0] != '\0';
  const auto *pairing_participant =
      pairing_owner_requested
          ? this->platform_runtime_->participant_registry_snapshot.registry
                .find(owner_node_id)
          : nullptr;
  const auto *pairing_credential =
      pairing_owner_requested
          ? this->participant_credential_snapshot_.registry.find(
                owner_node_id)
          : nullptr;
  const auto *pairing_endpoint =
      pairing_owner_requested && this->site_heartbeat_ != nullptr
          ? this->site_heartbeat_->endpoints().find(owner_node_id)
          : nullptr;
  const auto pairing_sync = pairing_site_sync_decision(
      this->platform_runtime_->site_configuration_snapshot,
      pairing_participant, pairing_credential, pairing_endpoint,
      this->platform_identity_.node_id, owner_node_id,
      this->local_floorplan_replica_ready_, millis());
  if (pairing_sync == PairingSiteSyncDecision::ALREADY_CURRENT)
    return SiteReplicationResult::OK;
  if (pairing_sync == PairingSiteSyncDecision::OWNER_OFFLINE)
    return SiteReplicationResult::OWNER_OFFLINE;
  if (pairing_sync == PairingSiteSyncDecision::START_STRICT) {
    this->reset_configuration_replication_();
    if (this->site_replication_->diagnostics().target ==
            SiteReplicationTarget::SITE_DIRECTORY &&
        this->site_replication_->diagnostics().state !=
            SiteReplicationState::IDLE)
      this->site_replication_->cancel();
    this->auto_directory_replication_active_ = false;
    this->auto_directory_replication_due_ms_ =
        millis() + SITE_DIRECTORY_REPLICATION_INTERVAL_MS;
    return this->site_replication_->start(
        owner_node_id, millis(), SiteConfigurationReplicationMode::STRICT);
  }

  SiteReplicationRoute route{};
  const auto route_result = resolve_site_replication_route(
      this->site_directory_ready()
          ? &this->platform_runtime_->site_directory_snapshot
          : nullptr,
      this->platform_runtime_->site_configuration_snapshot,
      this->platform_identity_.node_id, owner_node_id, &route);
  if (route_result != SiteReplicationRouteResult::OK &&
      route_result != SiteReplicationRouteResult::LEGACY_FALLBACK) {
    return route_result == SiteReplicationRouteResult::LOCAL_OWNER ||
                   route_result == SiteReplicationRouteResult::OWNER_MISMATCH
               ? SiteReplicationResult::INVALID_OWNER
               : SiteReplicationResult::INVALID_CONFIGURATION;
  }

  this->reset_configuration_replication_();
  if (this->site_replication_->diagnostics().target ==
          SiteReplicationTarget::SITE_DIRECTORY &&
      this->site_replication_->diagnostics().state !=
          SiteReplicationState::IDLE)
    this->site_replication_->cancel();
  this->auto_directory_replication_active_ = false;
  this->auto_directory_replication_due_ms_ =
      millis() + SITE_DIRECTORY_REPLICATION_INTERVAL_MS;
  return this->site_replication_->start(
      route.floorplan.owner_node_id, millis(),
      SiteConfigurationReplicationMode::STRICT,
      route_result == SiteReplicationRouteResult::OK ? &route : nullptr);
#else
  (void) owner_node_id;
  return SiteReplicationResult::SOCKET_ERROR;
#endif
}

void RadarApiServer::update_device_state_json(DeviceStateJsonInput input) {
#ifdef USE_ESP32
  this->refresh_fusion_automation_(input.now_ms);
  if (this->platform_runtime_ != nullptr) {
    const auto &automation =
        this->platform_runtime_->fusion_automation.snapshot();
    input.automation = &automation;
    input.shared_automation_areas =
        &this->platform_runtime_->shared_automation_areas;
    input.fusion_build_enabled = automation.build_enabled;
    input.fusion_authoritative = automation.authoritative;
    input.fusion_state =
        !automation.build_enabled
            ? "disabled"
            : (automation.authoritative ? "active" : "degraded");
    if (automation.authoritative) {
      input.presence = input.presence || automation.presence;
      input.motion = input.motion || automation.motion;
      input.target_count = automation.fusion.target_count;
      input.moving_target_count = 0;
      input.still_target_count = 0;
      for (auto &target : input.targets) {
        target.active = false;
        target.fused = false;
        target.source_count = 0;
      }
      for (std::size_t index = 0;
           index < automation.fusion.targets.size(); ++index) {
        const auto &source = automation.fusion.targets[index];
        if (!source.valid)
          continue;
        auto &target = input.targets[index];
        target.x = source.x_mm;
        target.y = source.y_mm;
        target.active = true;
        target.source_count = source.source_count;
        target.fused = source.source_count >= 2;
        if (std::hypot(source.vx_mm_s, source.vy_mm_s) > 100.0f)
          ++input.moving_target_count;
        else
          ++input.still_target_count;
      }
    }
  }
#endif
  const StateUpdateLease state_update = this->state_handler_.begin_update();
  if (!state_update.valid())
    return;

  FixedTextBuffer tracker_json(state_update.tracker,
                               state_update.tracker_capacity);
  if (!this->presence_tracker_.write_debug_json(tracker_json)) {
    ESP_LOGW(TAG, "State tracker JSON exceeded PSRAM workspace capacity=%u",
             static_cast<unsigned>(state_update.tracker_capacity));
    return;
  }

  FixedTextBuffer state_json(state_update.state, state_update.state_capacity);
  if (!build_device_state_json(state_json, this->device_config_cache_,
                               this->software_zone_evidence_,
                               tracker_json.data(), input)) {
    ESP_LOGW(TAG, "State JSON exceeded PSRAM workspace capacity=%u",
             static_cast<unsigned>(state_update.state_capacity));
    return;
  }
  if (!this->state_handler_.commit_update(state_update, state_json.size()))
    ESP_LOGW(TAG, "State JSON publish failed");
}

void RadarApiServer::refresh_fusion_automation_(uint32_t now_ms) {
#ifdef USE_ESP32
  if (this->platform_runtime_ == nullptr)
    return;
  auto &automation = this->platform_runtime_->fusion_automation;
  const bool build_enabled = this->production_fusion_build_enabled();
  if (!this->site_configuration_ready()) {
    std::string room_id;
    {
      const auto config_read = this->device_config_cache_.read();
      room_id = config_read->floorplan_room().id;
    }
    this->refresh_shared_automation_areas_(room_id.c_str());
    if (build_enabled)
      automation.mark_unavailable(this->platform_site_membership_.site_id,
                                  "", "", "", 0, false);
    else
      automation.disable(this->platform_site_membership_.site_id);
    return;
  }

  const GroupCoordinatorState *selected_group =
      this->platform_runtime_->group_coordinator.local_group(
          &this->platform_runtime_->site_configuration_snapshot,
          this->platform_identity_.node_id);
  const auto *local_member =
      this->group_roster_ready()
          ? this->platform_runtime_->group_roster_snapshot.find(
                this->platform_identity_.node_id)
          : nullptr;
  if (local_member == nullptr)
    local_member = this->platform_runtime_->site_configuration_snapshot.find(
        this->platform_identity_.node_id);
  const char *space_id =
      local_member != nullptr ? local_member->spatial_profile.space_id : "";
  this->refresh_shared_automation_areas_(space_id);
  if (selected_group == nullptr || local_member == nullptr ||
      !local_member->spatial_profile.committed || space_id[0] == '\0') {
    if (build_enabled)
      automation.mark_unavailable(this->platform_site_membership_.site_id,
                                  space_id, "", "", 0, false);
    else
      automation.disable(this->platform_site_membership_.site_id, space_id);
    return;
  }

  if (!build_enabled) {
    automation.disable(this->platform_site_membership_.site_id, space_id,
                       selected_group->group_id,
                       selected_group->leader_node_id, selected_group->term);
    return;
  }

  uint32_t local_pose_version = 0;
  {
    const auto config_read = this->device_config_cache_.read();
    local_pose_version = config_read->floorplan_pose_version();
  }
  if (!valid_spatial_transform(local_member->spatial_profile.transform) ||
      local_member->spatial_profile.pose_version == 0 ||
      local_member->spatial_profile.pose_version !=
          local_pose_version) {
    automation.mark_unavailable(
        this->platform_site_membership_.site_id, space_id,
        selected_group->group_id, selected_group->leader_node_id,
        selected_group->term, true);
    return;
  }

  const bool canonical_group_ready =
      selected_group->assignment_revision == 0 ||
      (selected_group->canonical_assignment &&
       selected_group->credentials_complete);
  if (!canonical_group_ready) {
    automation.preserve_continuity_or_mark_unavailable(
        this->platform_site_membership_.site_id, space_id,
        selected_group->group_id, selected_group->leader_node_id,
        selected_group->term, now_ms, selected_group->local_leader);
    return;
  }

  if (selected_group->local_leader) {
    if (!automation.update_local(
            this->platform_site_membership_.site_id, space_id,
            selected_group->group_id, this->platform_identity_.node_id,
            this->observation_transport_ != nullptr
                ? this->observation_transport_->boot_id()
                : "",
            selected_group->term,
            this->platform_runtime_->spatial_production_fusion.output(),
            now_ms)) {
      automation.preserve_continuity_or_mark_unavailable(
          this->platform_site_membership_.site_id, space_id,
          selected_group->group_id, selected_group->leader_node_id,
          selected_group->term, now_ms, true);
    }
    return;
  }

  auto &snapshot = this->platform_runtime_->fusion_snapshot_scratch;
  snapshot = FusionSnapshot{};
  if (this->observation_transport_ == nullptr ||
      !this->observation_transport_->copy_fusion_snapshot(now_ms, &snapshot) ||
      std::strcmp(snapshot.group_id, selected_group->group_id) != 0 ||
      std::strcmp(snapshot.leader_node_id,
                  selected_group->leader_node_id) != 0 ||
      snapshot.group_term != selected_group->term ||
      !automation.update_remote(snapshot, space_id,
                                local_member->spatial_profile.transform,
                                true)) {
    automation.preserve_continuity_or_mark_unavailable(
        this->platform_site_membership_.site_id, space_id,
        selected_group->group_id, selected_group->leader_node_id,
        selected_group->term, now_ms, false);
    return;
  }
#else
  (void) now_ms;
#endif
}

void RadarApiServer::refresh_shared_automation_areas_(const char *space_id) {
#ifdef USE_ESP32
  if (this->platform_runtime_ == nullptr)
    return;
  auto &cache = this->platform_runtime_->shared_automation_areas;
  const auto status = this->storage_.status();
  FloorplanRadarPose pose{};
  {
    const auto config_read = this->device_config_cache_.read();
    pose = config_read->floorplan_radar_pose();
  }
  const char *floorplan_id =
      this->site_configuration_ready() &&
              this->platform_runtime_->site_configuration_snapshot.floorplan
                  .committed
          ? this->platform_runtime_->site_configuration_snapshot.floorplan
                .floorplan_id
          : "";
  if (cache.matches(floorplan_id, space_id, status.header.config_hash,
                    pose.pose_version))
    return;
  if (!status.ok || status.header.config_size == 0 ||
      status.header.config_hash == 0 || floorplan_id[0] == '\0' ||
      space_id == nullptr || space_id[0] == '\0') {
    cache.clear();
    return;
  }

  uint32_t config_offset = 0;
  uint32_t config_size = 0;
  if (!this->storage_.payload_info(RadarPayloadTarget::CONFIG, &config_offset,
                                   &config_size) ||
      config_size == 0) {
    cache.clear();
    return;
  }

#ifdef USE_PSRAM
  auto *document = static_cast<char *>(heap_caps_malloc(
      config_size + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (document == nullptr) {
    ESP_LOGE(TAG,
             "Automation area CONFIG allocation failed region=psram size=%u",
             static_cast<unsigned>(config_size + 1));
    cache.clear();
    return;
  }
  const bool read = this->storage_.read_storage_range(
      config_offset, reinterpret_cast<uint8_t *>(document), config_size);
  document[config_size] = '\0';
  const bool valid =
      read && cache.update(document, config_size, floorplan_id, space_id,
                           status.header.config_hash, pose);
  heap_caps_free(document);
#else
  std::unique_ptr<char[]> document(new (std::nothrow) char[config_size + 1]);
  if (!document) {
    cache.clear();
    return;
  }
  const bool read = this->storage_.read_storage_range(
      config_offset, reinterpret_cast<uint8_t *>(document.get()), config_size);
  document[config_size] = '\0';
  const bool valid =
      read && cache.update(document.get(), config_size, floorplan_id, space_id,
                           status.header.config_hash, pose);
#endif
  ESP_LOGI(TAG,
           "Automation area cache refreshed floorplan=%s space=%s hash=%u "
           "revision=%u areas=%u valid=%s",
           floorplan_id, space_id, static_cast<unsigned>(status.header.config_hash),
           static_cast<unsigned>(cache.revision()),
           static_cast<unsigned>(cache.size()), valid ? "true" : "false");
#else
  (void) space_id;
#endif
}

void RadarApiServer::update_spatial_shadow_runtime_(uint32_t now_ms) {
#ifdef USE_ESP32
  if (this->spatial_shadow_runtime_ == nullptr)
    return;
  const int64_t started_us = esp_timer_get_time();
  auto &input = this->platform_runtime_->spatial_shadow_input_scratch;
  input = SpatialShadowRuntimeInput{};
  input.enabled = this->observation_transport_ != nullptr &&
                  this->site_authority_snapshot(now_ms).spatially_consistent();
  input.local_participant_id = this->platform_identity_.node_id;
  input.local_observation = &this->local_observation_adapter_.output();
  {
    const auto config_read = this->device_config_cache_.read();
    input.local_pose_version =
        config_read->floorplan_radar_pose().pose_version;
  }
  const SiteConfigurationMember *local_member =
      this->group_roster_ready()
          ? this->platform_runtime_->group_roster_snapshot.find(
                this->platform_identity_.node_id)
          : nullptr;
  if (local_member == nullptr && this->site_configuration_ready())
    local_member = this->platform_runtime_->site_configuration_snapshot.find(
        this->platform_identity_.node_id);
  input.local_transform_authoritative =
      local_member != nullptr &&
      local_member->spatial_profile.committed;
  if (input.local_transform_authoritative) {
    input.expected_local_pose_version =
        local_member->spatial_profile.pose_version;
    input.local_transform = local_member->spatial_profile.transform;
  }

  const GroupCoordinatorState *selected_group =
      this->platform_runtime_->group_coordinator.local_group(
          this->site_configuration_ready()
              ? &this->platform_runtime_->site_configuration_snapshot
              : nullptr,
          this->platform_identity_.node_id);
  const GroupMembershipView *membership =
      this->platform_runtime_->group_coordinator.local_membership();
  if (selected_group != nullptr) {
    input.group_configured = true;
    input.local_group_leader =
        selected_group->local_leader &&
        (selected_group->assignment_revision == 0 ||
         (selected_group->canonical_assignment &&
          selected_group->credentials_complete));
    input.group_id = selected_group->group_id;
  }

  auto &remote_participants =
      this->platform_runtime_->spatial_remote_participant_scratch;
  auto &remote_readiness =
      this->platform_runtime_->spatial_remote_readiness_scratch;
  remote_participants = {};
  remote_readiness = {};
  std::size_t selected_source_count = 0;
  std::size_t source_count = 0;
  for (auto &observation :
       this->platform_runtime_->remote_observation_scratch)
    observation = ObservationTransportSample{};

  const bool canonical_membership =
      selected_group != nullptr && membership != nullptr &&
      membership->valid && membership->local_admitted &&
      (this->group_roster_ready() || this->site_configuration_ready());
  if (canonical_membership) {
    for (const auto &member : membership->members) {
      if (!member.occupied ||
          std::strcmp(member.participant_id,
                      this->platform_identity_.node_id) == 0)
        continue;
      const auto *direct =
          this->platform_runtime_->participant_registry_snapshot.registry.find(
              member.participant_id);
      if (direct != nullptr &&
          direct->membership == ParticipantMembership::REVOKED)
        continue;
      const auto *remote_member =
          this->group_roster_ready()
              ? this->platform_runtime_->group_roster_snapshot.find(
                    member.participant_id)
              : nullptr;
      if (remote_member == nullptr && this->site_configuration_ready())
        remote_member =
            this->platform_runtime_->site_configuration_snapshot.find(
                member.participant_id);
      if (remote_member == nullptr ||
          !remote_member->spatial_profile.committed ||
          std::strcmp(remote_member->spatial_profile.group_id,
                      selected_group->group_id) != 0)
        continue;
      ++source_count;
      if (input.remote_count >= input.remotes.size())
        continue;
      const auto *credential =
          this->participant_credential_snapshot_.registry.find(
              member.participant_id);
      if (credential == nullptr || !credential->enabled)
        continue;
      auto &observation =
          this->platform_runtime_
              ->remote_observation_scratch[input.remote_count];
      if (!this->observation_transport_->copy_latest(
              member.participant_id, now_ms, &observation))
        continue;

      auto &remote = input.remotes[input.remote_count++];
      remote.participant_id = member.participant_id;
      remote.participant_token = credential->participant_token;
      remote.expected_pose_version =
          remote_member->spatial_profile.pose_version;
      remote.expected_calibration_version =
          remote_member->spatial_profile.calibration_version;
      remote.transform = remote_member->spatial_profile.transform;
      remote.observation = &observation;
    }
  } else if (selected_group != nullptr) {
    for (std::size_t slot = 0;
         slot < ParticipantRegistry::MAX_REMOTE_PARTICIPANTS; ++slot) {
      const auto *participant =
          this->platform_runtime_->participant_registry_snapshot.registry.record_at(slot);
      if (participant == nullptr ||
          participant->membership != ParticipantMembership::ACTIVE)
        continue;
      if (membership != nullptr &&
          !membership->contains(participant->participant_id))
        continue;
      const auto *readiness =
          this->platform_runtime_->participant_readiness_snapshot.registry.find(
              participant->participant_id);
      const auto *remote_member =
          this->group_roster_ready()
              ? this->platform_runtime_->group_roster_snapshot.find(
                    participant->participant_id)
              : nullptr;
      if (remote_member == nullptr && this->site_configuration_ready())
        remote_member =
            this->platform_runtime_->site_configuration_snapshot.find(
                participant->participant_id);
      const char *participant_group_id =
          remote_member != nullptr &&
                  remote_member->spatial_profile.committed &&
                  remote_member->spatial_profile.group_id[0] != '\0'
              ? remote_member->spatial_profile.group_id
              : (readiness != nullptr ? readiness->config.group_id : nullptr);
      if (!ParticipantReadinessRegistry::ready(readiness) ||
          participant_group_id == nullptr ||
          std::strcmp(participant_group_id, selected_group->group_id) != 0 ||
          (readiness->config.capability_flags &
           PARTICIPANT_CAP_POINT_TRACKS) == 0)
        continue;
      ++source_count;
      std::size_t insertion = 0;
      while (insertion < selected_source_count &&
             std::strcmp(remote_participants[insertion]->participant_id,
                         participant->participant_id) < 0)
        ++insertion;
      if (insertion >= remote_participants.size())
        continue;
      const std::size_t new_count =
          std::min(remote_participants.size(), selected_source_count + 1);
      for (std::size_t shift = new_count - 1; shift > insertion; --shift) {
        remote_participants[shift] = remote_participants[shift - 1];
        remote_readiness[shift] = remote_readiness[shift - 1];
      }
      remote_participants[insertion] = participant;
      remote_readiness[insertion] = readiness;
      selected_source_count = new_count;
    }
  }
  input.source_count_supported =
      selected_group != nullptr &&
      source_count <= SpatialShadowRuntimeInput::MAX_REMOTES;

  if (input.source_count_supported && !canonical_membership) {
    for (std::size_t index = 0; index < selected_source_count; ++index) {
      const auto *remote_participant = remote_participants[index];
      const auto *readiness = remote_readiness[index];
      if (remote_participant == nullptr || readiness == nullptr)
        continue;
      const auto *remote_member =
          this->group_roster_ready()
              ? this->platform_runtime_->group_roster_snapshot.find(
                    remote_participant->participant_id)
              : nullptr;
      if (remote_member == nullptr && this->site_configuration_ready())
        remote_member =
            this->platform_runtime_->site_configuration_snapshot.find(
                remote_participant->participant_id);
      const auto *credential =
          this->participant_credential_snapshot_.registry.find(
              remote_participant->participant_id);
      if (remote_member == nullptr ||
          !remote_member->spatial_profile.committed ||
          credential == nullptr || !credential->enabled)
        continue;

      auto &observation =
          this->platform_runtime_
              ->remote_observation_scratch[input.remote_count];
      if (!this->observation_transport_->copy_latest(
              remote_participant->participant_id, now_ms, &observation))
        continue;

      auto &remote = input.remotes[input.remote_count++];
      remote.participant_id = remote_participant->participant_id;
      remote.participant_token = credential->participant_token;
      remote.expected_pose_version =
          remote_member->spatial_profile.pose_version;
      remote.expected_calibration_version =
          remote_member->spatial_profile.calibration_version;
      remote.transform = remote_member->spatial_profile.transform;
      remote.observation = &observation;
    }
  }

  this->spatial_shadow_runtime_->update(now_ms, input);
#ifdef NAMO_PRODUCTION_FUSION
  constexpr bool production_enabled = true;
#else
  constexpr bool production_enabled = false;
#endif
  this->platform_runtime_->spatial_production_fusion.update(
      production_enabled, &this->spatial_shadow_runtime_->diagnostics(),
      &this->spatial_shadow_runtime_->output(), input.local_transform);
  const uint32_t cycle_us = static_cast<uint32_t>(std::max<int64_t>(
      0, esp_timer_get_time() - started_us));
  this->spatial_shadow_runtime_->record_cycle_us(cycle_us);
#else
  (void) now_ms;
#endif
}

bool RadarApiServer::allocate_spatial_shadow_runtime_() {
#ifdef USE_ESP32
  if (this->spatial_shadow_runtime_ != nullptr)
    return true;
  this->spatial_shadow_runtime_ =
      allocate_runtime_object<SpatialShadowRuntime>(
          "spatial_shadow_runtime",
          &this->spatial_shadow_runtime_in_psram_);
  return this->spatial_shadow_runtime_ != nullptr;
#else
  return false;
#endif
}

void RadarApiServer::destroy_spatial_shadow_runtime_() {
#ifdef USE_ESP32
  destroy_runtime_object(&this->spatial_shadow_runtime_,
                         &this->spatial_shadow_runtime_in_psram_);
#endif
}

bool RadarApiServer::automatic_handover_credentials_complete_() const {
#ifdef USE_ESP32
  if (!this->site_configuration_ready() ||
      !this->canonical_site_directory_ready() ||
      !this->participant_credential_persistence_ready())
    return false;
  return site_credentials_complete_for_node(
      this->platform_identity_,
      this->platform_runtime_->site_configuration_snapshot,
      &this->platform_runtime_->site_directory_snapshot,
      this->platform_runtime_->site_coordinator.state().leader_node_id,
      this->participant_credential_snapshot_.registry);
#else
  return false;
#endif
}

void RadarApiServer::refresh_site_authority_(uint32_t now_ms) {
  if (this->platform_runtime_ == nullptr)
    return;
  const PlatformIdentity *identity =
      this->platform_identity_ready() ? &this->platform_identity_ : nullptr;
  const SiteConfigurationSnapshot *configuration =
      this->site_configuration_ready()
          ? &this->platform_runtime_->site_configuration_snapshot
          : nullptr;
  const ParticipantEndpointCache *endpoints = nullptr;
#ifdef USE_ESP32
  if (this->site_heartbeat_ != nullptr)
    endpoints = &this->site_heartbeat_->endpoints();
#endif
  this->platform_runtime_->site_authority_snapshot = evaluate_site_authority(
      identity, configuration, endpoints, now_ms);
}

const SiteAuthoritySnapshot &RadarApiServer::site_authority_snapshot(
    uint32_t now_ms) {
  static const SiteAuthoritySnapshot unavailable{};
  if (this->platform_runtime_ == nullptr)
    return unavailable;
  auto &authority = this->platform_runtime_->site_authority_snapshot;
  const bool local_configuration_changed =
      !this->site_configuration_ready() ||
      !authority.describes_revision(
          this->platform_runtime_->site_configuration_snapshot);
  if (!authority.evaluated || local_configuration_changed ||
      now_ms - authority.evaluated_at_ms >=
          SITE_AUTHORITY_REFRESH_INTERVAL_MS)
    this->refresh_site_authority_(now_ms);
  return this->platform_runtime_->site_authority_snapshot;
}

const SiteConfigurationMember *
RadarApiServer::automatic_configuration_successor(uint32_t now_ms) const {
  if (this->platform_runtime_ == nullptr || !this->site_configuration_ready())
    return nullptr;
  const auto &configuration =
      this->platform_runtime_->site_configuration_snapshot;
  const auto &site = this->platform_runtime_->site_coordinator.state();
  if (!site.election_fenced || this->site_heartbeat_ == nullptr)
    return radar_api_server::automatic_configuration_successor(configuration);

  const SiteConfigurationMember *selected = nullptr;
  uint8_t selected_penalty = 0;
  for (const auto &member : configuration.members) {
    if (!member.occupied ||
        member.membership != ParticipantMembership::ACTIVE ||
        member.type != ParticipantType::NAMO_NODE || !member.leader_eligible ||
        std::strcmp(member.participant_id,
                    configuration.configuration_owner_node_id) == 0)
      continue;

    bool ready = false;
    if (std::strcmp(member.participant_id,
                    this->platform_identity_.node_id) == 0) {
      const auto *directory = this->canonical_site_directory_ready()
                                  ? &this->platform_runtime_->site_directory_snapshot
                                  : nullptr;
      ready = site_credentials_complete_for_node(
          this->platform_identity_, configuration, directory,
          site.leader_node_id, this->participant_credential_snapshot_.registry);
    } else {
      const auto *endpoint =
          this->site_heartbeat_->endpoints().find(member.participant_id);
      ready = ParticipantEndpointCache::is_online(endpoint, now_ms) &&
              participant_endpoint_matches_configuration(endpoint,
                                                         configuration) &&
              endpoint->heartbeat_wire_version >=
                  SiteHeartbeatEnvelope::CURRENT_WIRE_VERSION &&
              endpoint->site_credentials_complete;
    }
    if (!ready)
      continue;

    uint8_t penalty =
        std::strcmp(member.participant_id, site.leader_node_id) == 0 ? 2U : 0U;
    if (this->canonical_site_directory_ready()) {
      for (const auto &group :
           this->platform_runtime_->site_directory_snapshot.groups) {
        if (group.occupied &&
            std::strcmp(group.leader_node_id, member.participant_id) == 0) {
          ++penalty;
          break;
        }
      }
    }
    if (selected == nullptr ||
        leader_candidate_preferred(member.participant_id, penalty,
                                   selected->participant_id,
                                   selected_penalty)) {
      selected = &member;
      selected_penalty = penalty;
    }
  }
  return selected;
}

bool RadarApiServer::shared_site_configuration_writable(uint32_t now_ms) {
#ifdef USE_ESP32
  if (!this->site_configuration_ready() ||
      std::strcmp(
          this->platform_runtime_->site_configuration_snapshot.configuration_owner_node_id,
          this->platform_identity_.node_id) != 0)
    return false;
  if (this->platform_runtime_->site_configuration_snapshot.active_member_count() <= 1)
    return true;
  if (!this->platform_runtime_->site_coordinator.state().quorum_available ||
      this->site_heartbeat_ == nullptr ||
      !this->site_authority_snapshot(now_ms).consistent())
    return false;
  const auto *successor = this->automatic_configuration_successor(now_ms);
  if (successor == nullptr) {
    const bool configured_successor_exists =
        radar_api_server::automatic_configuration_successor(
            this->platform_runtime_->site_configuration_snapshot) != nullptr;
    // A consistent Site may deliberately have no opted-in backup. Keep the
    // current Owner writable in that emergency concentrated-role mode, while
    // still fencing writes when a configured successor is merely unavailable.
    return !this->platform_runtime_->site_coordinator.state().election_fenced ||
           !configured_successor_exists;
  }
  const auto *endpoint =
      this->site_heartbeat_->endpoints().find(successor->participant_id);
  if (!ParticipantEndpointCache::is_online(endpoint, now_ms) ||
      !participant_endpoint_matches_configuration(
          endpoint,
          this->platform_runtime_->site_configuration_snapshot) ||
      endpoint->heartbeat_wire_version <
          SiteHeartbeatEnvelope::CURRENT_WIRE_VERSION ||
      !endpoint->site_credentials_complete)
    return false;
  if (!this->platform_runtime_->site_configuration_snapshot.floorplan.committed)
    return true;
  return endpoint->floorplan_replica_ready &&
         endpoint->floorplan_geometry_version ==
             this->platform_runtime_->site_configuration_snapshot.floorplan.geometry_version &&
         endpoint->floorplan_config_hash ==
             this->platform_runtime_->site_configuration_snapshot.floorplan.config_hash &&
         endpoint->floorplan_image_hash ==
             this->platform_runtime_->site_configuration_snapshot.floorplan.image_hash;
#else
  (void) now_ms;
  return false;
#endif
}

void RadarApiServer::process_automatic_configuration_handover_(
    uint32_t now_ms) {
#ifdef USE_ESP32
  if (!this->platform_identity_ready() ||
      !this->site_configuration_persistence_ready() ||
      this->site_heartbeat_ == nullptr) {
    this->automatic_configuration_handover_.reset();
    return;
  }
  if (this->site_directory_ready() &&
      !this->platform_runtime_->site_directory_snapshot.compatibility_mirror &&
      !this->platform_runtime_->site_directory_snapshot.
          legacy_single_floorplan_projection &&
      this->platform_runtime_->site_directory_snapshot.floorplan_count() > 1) {
    this->automatic_configuration_handover_.reset();
    return;
  }
  if (!this->site_authority_snapshot(now_ms).consistent()) {
    this->automatic_configuration_handover_.update(
        AutomaticConfigurationHandoverCondition::CONFIGURATION_CONFLICT,
        0, now_ms);
    return;
  }
  const char *owner =
      this->platform_runtime_->site_configuration_snapshot.configuration_owner_node_id;
  const auto *owner_endpoint =
      owner[0] != '\0' &&
              std::strcmp(owner, this->platform_identity_.node_id) != 0
          ? this->site_heartbeat_->endpoints().find(owner)
          : nullptr;
  const bool owner_online =
      ParticipantEndpointCache::is_online(owner_endpoint, now_ms);
  const auto &coordinator = this->platform_runtime_->site_coordinator.state();
  const auto *successor = this->automatic_configuration_successor(now_ms);
  const auto condition = evaluate_automatic_configuration_handover(
      this->platform_identity_, this->platform_runtime_->site_configuration_snapshot,
       owner_online, coordinator.quorum_available,
       this->local_floorplan_replica_ready_,
       this->automatic_handover_credentials_complete_(),
       successor != nullptr
           ? successor->participant_id
           : (coordinator.election_fenced ? "" : nullptr));
  const uint64_t handover_fingerprint =
      automatic_configuration_handover_fingerprint(
          this->platform_runtime_->site_configuration_snapshot,
          successor != nullptr ? successor->participant_id : nullptr);
  if (!this->automatic_configuration_handover_.update(
          condition, handover_fingerprint, now_ms))
    return;

  if (prepare_automatic_configuration_handover(
          this->platform_identity_, this->platform_runtime_->site_configuration_snapshot,
          &this->platform_runtime_->site_configuration_update_scratch,
          successor != nullptr ? successor->participant_id : nullptr) !=
      SiteConfigurationResult::OK) {
    this->automatic_configuration_handover_.complete(false, now_ms);
    ESP_LOGE(TAG, "Automatic configuration handover build failed");
    return;
  }
  const auto result = this->site_configuration_store_->update(
      *this->site_configuration_backend_,
      this->platform_runtime_->site_configuration_update_scratch,
      &this->platform_runtime_->site_configuration_snapshot);
  if (result != SiteConfigurationStoreResult::UPDATED) {
    this->automatic_configuration_handover_.complete(false, now_ms);
    ESP_LOGE(TAG, "Automatic configuration handover persistence failed: %s",
             site_configuration_store_result_to_string(result));
    return;
  }
  this->site_configuration_store_result_ =
      SiteConfigurationStoreResult::UPDATED;
  this->automatic_configuration_handover_.complete(true, now_ms);
  this->reset_configuration_replication_();
  if (this->site_replication_ != nullptr)
    this->site_replication_->cancel();
  ESP_LOGI(TAG, "Automatic configuration handover completed owner=%s",
           this->platform_identity_.node_id);
  this->reconcile_owned_spatial_assignments_("automatic_handover");
#else
  (void) now_ms;
#endif
}

void RadarApiServer::process_spatial_readiness_invalidation_(uint32_t now_ms) {
  if (!this->participant_registry_persistence_ready() ||
      !this->participant_readiness_persistence_ready() ||
      !this->site_configuration_ready() ||
      this->participant_pairing_runtime_.snapshot(now_ms).busy ||
      (this->spatial_readiness_check_ms_ != 0 &&
       now_ms - this->spatial_readiness_check_ms_ <
           SPATIAL_READINESS_CHECK_INTERVAL_MS))
    return;
  {
    auto reconciliation_lease = this->participant_operation_.try_acquire();
    if (!reconciliation_lease.acquired())
      return;
    this->spatial_readiness_check_ms_ = now_ms;

    auto *readiness_scratch = this->participant_operation_.readiness_scratch();
    if (readiness_scratch == nullptr)
      return;
    bool readiness_reconciled = false;
    const auto reconciliation_result = reconcile_direct_spatial_readiness(
        this->platform_identity_,
        this->platform_runtime_->participant_registry_snapshot.registry,
        this->platform_runtime_->site_configuration_snapshot,
        this->platform_runtime_->participant_readiness_snapshot.registry,
        readiness_scratch, &readiness_reconciled);
    if (reconciliation_result != SiteConfigurationResult::OK) {
      ESP_LOGE(TAG, "Direct spatial readiness reconciliation failed result=%u",
               static_cast<unsigned>(reconciliation_result));
      return;
    }
    if (readiness_reconciled) {
      const auto store_result = this->participant_readiness_store_->update(
          *this->participant_readiness_backend_, *readiness_scratch,
          &this->platform_runtime_->participant_readiness_snapshot);
      if (store_result != ParticipantReadinessStoreResult::UPDATED) {
        ESP_LOGE(TAG,
                 "Direct spatial readiness reconciliation persistence failed "
                 "result=%s",
                 participant_readiness_store_result_to_string(store_result));
        return;
      }
      ESP_LOGI(
          TAG,
          "Direct spatial readiness reconciled from canonical Site profiles");
    }
  }

  const auto &floorplan = this->platform_runtime_->site_configuration_snapshot.floorplan;
  uint32_t local_pose_version = 0;
  {
    const auto config_read = this->device_config_cache_.read();
    local_pose_version = config_read->floorplan_pose_version();
  }
  for (std::size_t slot = 0;
       slot < ParticipantRegistry::MAX_REMOTE_PARTICIPANTS; ++slot) {
    const auto *participant =
        this->platform_runtime_->participant_registry_snapshot.registry.record_at(slot);
    if (participant == nullptr ||
        participant->membership != ParticipantMembership::ACTIVE)
      continue;
    const auto *profile =
        this->platform_runtime_->participant_readiness_snapshot.registry.find(
            participant->participant_id);
    if (profile == nullptr ||
        (profile->config.capability_flags & PARTICIPANT_CAP_POINT_TRACKS) == 0)
      continue;

    const auto *endpoint =
        this->participant_endpoint(participant->participant_id);
    const auto mismatch = spatial_readiness_mismatch(
        profile, floorplan.committed, floorplan.floorplan_id,
        floorplan.geometry_version, local_pose_version,
        endpoint != nullptr ? endpoint->radar_pose_version : 0);
    if (mismatch == SpatialReadinessMismatch::NONE)
      continue;
    const char *reason = spatial_readiness_mismatch_to_string(mismatch);

    char participant_id[ParticipantRecord::MAX_ID_LENGTH + 1]{};
    std::strncpy(participant_id, participant->participant_id,
                 sizeof(participant_id) - 1);
    ParticipantReadinessConfig invalidated = profile->config;
    invalidated.calibration_version = 0;
    invalidated.transform = {};
    PlatformParticipantOperationRequest operation{};
    operation.action =
        PlatformParticipantOperationAction::INVALIDATE_READINESS;
    operation.participant_id = participant_id;
    operation.configuration = &invalidated;
    operation.now_ms = now_ms;
    const auto outcome =
        this->participant_operation_.execute(*this, operation);

    if (outcome.ok()) {
      ESP_LOGW(TAG, "Spatial readiness invalidated participant=%s reason=%s",
               participant_id, reason);
    } else {
      ESP_LOGE(TAG,
               "Spatial readiness invalidation failed participant=%s reason=%s "
               "operation=%s result=%s rollback=%s",
               participant_id, reason,
               platform_participant_operation_result_to_string(outcome.result),
               participant_enrollment_result_to_string(
                   outcome.enrollment_result),
               outcome.rollback_attempted
                   ? (outcome.rollback_succeeded ? "restored" : "failed")
                   : "not_required");
    }
    break;
  }
}

void RadarApiServer::dump_config() {
  ESP_LOGCONFIG(TAG, "Radar API Server:");
  ESP_LOGCONFIG(TAG, "  LD2410C UART health logging: %s",
                this->ld2410_uart_ != nullptr ? "enabled" : "disabled");
  ESP_LOGCONFIG(TAG, "  LD2410C bounded stream watchdog: %s",
                this->ld2410_uart_ != nullptr ? "enabled" : "disabled");
  ESP_LOGCONFIG(TAG, "  Setup: /setup");
  ESP_LOGCONFIG(TAG, "  Setup API: GET /api/setup/status|networks, POST /api/setup/prepare|apply-wifi|finish");
  ESP_LOGCONFIG(TAG, "  Dashboard: /dashboard");
  ESP_LOGCONFIG(TAG, "  Floorplan Status API: /api/floorplan/status");
  ESP_LOGCONFIG(TAG, "  Floorplan Config API: GET /api/floorplan");
  ESP_LOGCONFIG(TAG, "  Floorplan Image API: GET /api/floorplan/image");
  ESP_LOGCONFIG(TAG, "  Floorplan Patch API: POST /api/floorplan/radar|room-name|occlusion|objects");
  ESP_LOGCONFIG(TAG, "  Device Config Status API: /api/config/status");
  ESP_LOGCONFIG(TAG, "  Device Config API: GET /api/config");
  ESP_LOGCONFIG(TAG, "  Device Config Upload API: POST /api/config/upload/start|chunk|commit");
  ESP_LOGCONFIG(TAG, "  State API: GET /api/state");
  ESP_LOGCONFIG(TAG, "  Stats API: GET /api/stats");
  ESP_LOGCONFIG(TAG, "  Stats Upload API: POST /api/stats/upload/start|chunk|commit");
  ESP_LOGCONFIG(TAG, "  System API: GET /api/system/status, POST /api/system/reset");
#ifdef NAMO_SIGNED_OTA
  ESP_LOGCONFIG(TAG,
                "  Signed Firmware API: GET /api/firmware/trust, POST "
                "/api/firmware/alpha-permission, /api/firmware/update/*");
#endif
  ESP_LOGCONFIG(TAG, "  Platform Descriptor API: GET /api/platform/descriptor");
  ESP_LOGCONFIG(TAG, "  Platform Discovery API: GET|POST /api/platform/discovery");
  ESP_LOGCONFIG(TAG, "  Platform Participants API: GET|POST /api/platform/participants");
  ESP_LOGCONFIG(TAG, "  Platform Groups API: GET /api/platform/groups");
  ESP_LOGCONFIG(TAG, "  Platform Pairing API: GET|POST /api/platform/pairing");
  ESP_LOGCONFIG(TAG, "  Control Status API: GET /api/control/status");
  ESP_LOGCONFIG(TAG, "  Control API: POST /api/control/status-led|led-duration|environment-correction|temperature-offset|humidity-offset|timezone");
  ESP_LOGCONFIG(TAG, "  Static Radar Tuning API: GET status, POST session|gate");
  ESP_LOGCONFIG(TAG, "  Diagnostics API: GET /api/diagnostics/events|events.txt|replay.ndjson");
  ESP_LOGCONFIG(TAG, "  Floorplan Upload API: POST /api/floorplan/upload/start|chunk|commit");
}

float RadarApiServer::get_setup_priority() const { return setup_priority::DATA + 1.0f; }

bool RadarApiServer::canHandle(AsyncWebServerRequest *request) const {
  const auto url = request->url_to(this->request_url_buffer_);
  return
#if defined(USE_ESP32) && defined(NAMO_SIGNED_OTA)
         FirmwareUpdateHandler::can_handle(request, url) ||
#endif
         this->setup_handler_.can_handle(request, url) ||
         this->dashboard_handler_.can_handle(request, url) ||
         this->floorplan_handler_.can_handle(request, url) ||
         this->device_config_handler_.can_handle(request, url) ||
         this->stats_handler_.can_handle(request, url) ||
         this->pairing_handler_.can_handle(request, url) ||
         this->platform_handler_.can_handle(request, url) ||
         this->system_handler_.can_handle(request, url) ||
         this->state_handler_.can_handle(request, url) ||
         this->diagnostic_handler_.can_handle(request, url) ||
         this->control_handler_.can_handle(request, url);
}

void RadarApiServer::record_httpd_stack_watermark(const char *endpoint,
                                                  const char *phase) {
#ifdef USE_ESP32
  const uint32_t free_bytes =
      static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr)) *
      sizeof(StackType_t);
  if (free_bytes < this->httpd_stack_lowest_free_bytes_) {
    this->httpd_stack_lowest_free_bytes_ = free_bytes;
    this->httpd_stack_lowest_endpoint_ = endpoint;
    this->httpd_stack_lowest_phase_ = phase;
  }
  this->record_internal_ram_watermark(endpoint, phase);
#else
  (void) endpoint;
  (void) phase;
#endif
}

void RadarApiServer::record_internal_ram_watermark(const char *source,
                                                   const char *phase) {
#ifdef USE_ESP32
  if (this->platform_runtime_ == nullptr || source == nullptr ||
      phase == nullptr)
    return;
  const uint32_t minimum_free = static_cast<uint32_t>(
      heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL |
                                      MALLOC_CAP_8BIT));
  const uint32_t largest_free = static_cast<uint32_t>(
      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL |
                                       MALLOC_CAP_8BIT));
  const uint8_t threshold_level =
      internal_ram_threshold_level(minimum_free);
  const uint32_t observed_at_ms = millis();
  bool log_threshold = false;
  portENTER_CRITICAL(&internal_ram_diagnostics_mux);
  auto &diagnostics = this->platform_runtime_->internal_ram_watermark;
  if (!diagnostics.threshold_recorded && minimum_free < 48U * 1024U) {
    diagnostics.threshold_source = source;
    diagnostics.threshold_phase = phase;
    diagnostics.threshold_free_bytes = minimum_free;
    diagnostics.threshold_observed_at_ms = observed_at_ms;
    diagnostics.threshold_recorded = true;
  }
  if (!diagnostics.initialized ||
      minimum_free < diagnostics.minimum_free_bytes) {
    diagnostics.source = source;
    diagnostics.phase = phase;
    diagnostics.minimum_free_bytes = minimum_free;
    diagnostics.largest_free_block_bytes = largest_free;
    diagnostics.observed_at_ms = observed_at_ms;
    diagnostics.event_count++;
    diagnostics.initialized = true;
    if (threshold_level > diagnostics.logged_threshold_level) {
      diagnostics.logged_threshold_level = threshold_level;
      log_threshold = threshold_level > 0;
    }
  }
  portEXIT_CRITICAL(&internal_ram_diagnostics_mux);
  if (log_threshold) {
    ESP_LOGW(TAG,
             "Internal RAM low watermark source=%s phase=%s minimum=%u "
             "largest=%u threshold=%uKiB",
             source, phase, static_cast<unsigned>(minimum_free),
             static_cast<unsigned>(largest_free),
             static_cast<unsigned>((72U - threshold_level * 8U)));
  }
#else
  (void) source;
  (void) phase;
#endif
}

void RadarApiServer::record_internal_ram_checkpoint(const char *source,
                                                    const char *phase) {
#ifdef USE_ESP32
  if (this->platform_runtime_ == nullptr)
    return;
  bool needed = false;
  portENTER_CRITICAL(&internal_ram_diagnostics_mux);
  const auto &diagnostics =
      this->platform_runtime_->internal_ram_watermark;
  needed = !diagnostics.threshold_recorded;
  portEXIT_CRITICAL(&internal_ram_diagnostics_mux);
  if (needed)
    this->record_internal_ram_watermark(source, phase);
#else
  (void) source;
  (void) phase;
#endif
}

void RadarApiServer::copy_internal_ram_watermark(
    InternalRamWatermarkDiagnostics *output) const {
  if (output == nullptr)
    return;
#ifdef USE_ESP32
  portENTER_CRITICAL(&internal_ram_diagnostics_mux);
  if (this->platform_runtime_ != nullptr)
    *output = this->platform_runtime_->internal_ram_watermark;
  else
    *output = InternalRamWatermarkDiagnostics{};
  portEXIT_CRITICAL(&internal_ram_diagnostics_mux);
#else
  *output = InternalRamWatermarkDiagnostics{};
#endif
}

void RadarApiServer::handleRequest(AsyncWebServerRequest *request) {
  const auto url = request->url_to(this->request_url_buffer_);
  const char *request_endpoint = classify_httpd_request(url);
  this->record_httpd_stack_watermark(request_endpoint, "entry");
#if defined(USE_ESP32) && defined(NAMO_SIGNED_OTA)
  if (FirmwareUpdateHandler::can_handle(request, url)) {
    if (this->firmware_update_handler_ != nullptr) {
      this->firmware_update_handler_->handle(request, url);
    } else {
      http_response::send_error_info(request, 503,
                                     "firmware_trust_unavailable", "error");
    }
    this->record_httpd_stack_watermark(request_endpoint);
    return;
  }
#endif
  if (this->setup_handler_.can_handle(request, url)) {
    this->setup_handler_.handle(request, url);
    this->record_httpd_stack_watermark(request_endpoint);
    return;
  }
  if (this->dashboard_handler_.can_handle(request, url)) {
    this->dashboard_handler_.handle(request, url);
    this->record_httpd_stack_watermark(request_endpoint);
    return;
  }
  if (this->floorplan_handler_.can_handle(request, url)) {
    this->floorplan_handler_.handle(request, url);
    this->record_httpd_stack_watermark(request_endpoint);
    return;
  }
  if (this->device_config_handler_.can_handle(request, url)) {
    this->device_config_handler_.handle(request, url);
    this->record_httpd_stack_watermark(request_endpoint);
    return;
  }
  if (this->stats_handler_.can_handle(request, url)) {
    this->stats_handler_.handle(request, url);
    this->record_httpd_stack_watermark(request_endpoint);
    return;
  }
  if (this->pairing_handler_.can_handle(request, url)) {
    this->pairing_handler_.handle(request, url);
    this->record_httpd_stack_watermark(request_endpoint);
    return;
  }
  if (this->platform_handler_.can_handle(request, url)) {
    this->platform_handler_.handle(request, url);
    this->record_httpd_stack_watermark(request_endpoint);
    return;
  }
  if (this->system_handler_.can_handle(request, url)) {
    this->system_handler_.handle(request, url);
    this->record_httpd_stack_watermark(request_endpoint);
    return;
  }
  if (this->state_handler_.can_handle(request, url)) {
    this->state_handler_.handle(request, url);
    this->record_httpd_stack_watermark(request_endpoint);
    return;
  }
  if (this->diagnostic_handler_.can_handle(request, url)) {
    this->diagnostic_handler_.handle(request, url);
    this->record_httpd_stack_watermark(request_endpoint);
    return;
  }
  if (this->control_handler_.can_handle(request, url)) {
    this->control_handler_.handle(request, url);
    this->record_httpd_stack_watermark(request_endpoint);
    return;
  }

  http_response::send_error_info(request, 404, "not_found", "not_found",
                                 "error", "{}");
  this->record_httpd_stack_watermark(request_endpoint);
}

void RadarApiServer::handleUpload(AsyncWebServerRequest *request,
                                  const std::string &filename, size_t index,
                                  uint8_t *data, size_t len, bool final) {
#if defined(USE_ESP32) && defined(NAMO_SIGNED_OTA)
  const auto url = request->url_to(this->request_url_buffer_);
  if (this->firmware_update_handler_ != nullptr &&
      FirmwareUpdateHandler::can_handle_upload(request, url)) {
    this->firmware_update_handler_->handle_upload(
        request, url, filename, index, data, len, final);
  }
#else
  (void) request;
  (void) filename;
  (void) index;
  (void) data;
  (void) len;
  (void) final;
#endif
}

void RadarApiServer::update_presence_tracker(uint32_t now_ms, bool pir_motion, bool filter_blocked,
                                             bool stable_presence,
                                             bool room_context_configured,
                                             const StaticRadarEvidence &static_radar, float illuminance_lux,
                                             float stationary_speed_cm_s,
                                             bool target_1_valid, bool target_1_filter_blocked,
                                             float target_1_x, float target_1_y,
                                             float target_1_speed, float target_1_distance,
                                             bool target_1_reduced_zone, int target_1_exit_zone_mask,
                                             bool target_1_room_inside,
                                             bool target_1_room_outside, float target_1_room_signed_distance,
                                             bool target_2_valid, bool target_2_filter_blocked,
                                             float target_2_x, float target_2_y,
                                             float target_2_speed, float target_2_distance,
                                             bool target_2_reduced_zone, int target_2_exit_zone_mask,
                                             bool target_2_room_inside,
                                             bool target_2_room_outside, float target_2_room_signed_distance,
                                             bool target_3_valid, bool target_3_filter_blocked,
                                             float target_3_x, float target_3_y,
                                             float target_3_speed, float target_3_distance,
                                             bool target_3_reduced_zone, int target_3_exit_zone_mask,
                                             bool target_3_room_inside,
                                             bool target_3_room_outside, float target_3_room_signed_distance) {
  PresenceTrackerInput input;
  input.now_ms = now_ms;
  input.pir_motion = pir_motion;
  input.filter_blocked = filter_blocked;
  input.room_context_configured = room_context_configured;
  input.illuminance_lux = illuminance_lux;
  input.stationary_speed_cm_s = stationary_speed_cm_s;
  input.targets[0] = {target_1_valid, target_1_x, target_1_y, target_1_speed, target_1_distance,
                      target_1_reduced_zone,
                      target_1_exit_zone_mask, target_1_room_inside, target_1_room_outside,
                      target_1_room_signed_distance, target_1_filter_blocked};
  input.targets[1] = {target_2_valid, target_2_x, target_2_y, target_2_speed, target_2_distance,
                      target_2_reduced_zone,
                      target_2_exit_zone_mask, target_2_room_inside, target_2_room_outside,
                      target_2_room_signed_distance, target_2_filter_blocked};
  input.targets[2] = {target_3_valid, target_3_x, target_3_y, target_3_speed, target_3_distance,
                      target_3_reduced_zone,
                      target_3_exit_zone_mask, target_3_room_inside, target_3_room_outside,
                      target_3_room_signed_distance, target_3_filter_blocked};
  this->last_tracker_input_ = input;
  this->presence_tracker_.update(input);
  PresenceFusionInput fusion_input;
  fusion_input.now_ms = now_ms;
  fusion_input.pir_motion = pir_motion;
  fusion_input.filter_blocked = filter_blocked;
  fusion_input.stable_presence = stable_presence;
  fusion_input.static_radar = static_radar;
  this->presence_fusion_.update(fusion_input, this->presence_tracker_.output());
  this->local_observation_adapter_.update(now_ms, pir_motion, filter_blocked,
                                          this->presence_tracker_.output(),
                                          this->presence_fusion_.output());
}

void RadarApiServer::update_legacy_presence(uint32_t now_ms, bool pir_motion, bool filter_blocked, int target_count,
                                            int moving_target_count, int still_target_count,
                                            float stationary_speed_cm_s, bool target_1_valid, float target_1_x,
                                            float target_1_y, float target_1_speed, bool target_2_valid,
                                            float target_2_x, float target_2_y, float target_2_speed,
                                            bool target_3_valid, float target_3_x, float target_3_y,
                                            float target_3_speed) {
  LegacyPresenceInput input;
  input.now_ms = now_ms;
  input.pir_motion = pir_motion;
  input.filter_blocked = filter_blocked;
  input.target_count = target_count;
  input.moving_target_count = moving_target_count;
  input.still_target_count = still_target_count;
  input.stationary_speed_cm_s = stationary_speed_cm_s;
  input.targets[0] = {target_1_valid, target_1_x, target_1_y, target_1_speed};
  input.targets[1] = {target_2_valid, target_2_x, target_2_y, target_2_speed};
  input.targets[2] = {target_3_valid, target_3_x, target_3_y, target_3_speed};
  this->legacy_presence_.update(input);
}

void RadarApiServer::update_presence_replay_raw_targets(
    bool target_1_raw_valid, float target_1_x, float target_1_y, float target_1_speed, float target_1_distance,
    int target_1_filter_mode, bool target_1_filtered, bool target_1_range_valid,
    bool target_2_raw_valid, float target_2_x, float target_2_y, float target_2_speed, float target_2_distance,
    int target_2_filter_mode, bool target_2_filtered, bool target_2_range_valid,
    bool target_3_raw_valid, float target_3_x, float target_3_y, float target_3_speed, float target_3_distance,
    int target_3_filter_mode, bool target_3_filtered, bool target_3_range_valid) {
  this->last_replay_raw_input_.targets[0] = {target_1_raw_valid, target_1_x, target_1_y, target_1_speed,
                                             target_1_distance, target_1_filter_mode, target_1_filtered,
                                             target_1_range_valid};
  this->last_replay_raw_input_.targets[1] = {target_2_raw_valid, target_2_x, target_2_y, target_2_speed,
                                             target_2_distance, target_2_filter_mode, target_2_filtered,
                                             target_2_range_valid};
  this->last_replay_raw_input_.targets[2] = {target_3_raw_valid, target_3_x, target_3_y, target_3_speed,
                                             target_3_distance, target_3_filter_mode, target_3_filtered,
                                             target_3_range_valid};
}

void RadarApiServer::update_software_zone_evidence(uint32_t now_ms, bool target_1_valid, float target_1_x,
                                                   float target_1_y, bool target_2_valid, float target_2_x,
                                                   float target_2_y, bool target_3_valid, float target_3_x,
                                                   float target_3_y) {
  const SoftwareZoneTarget targets[3] = {
      {target_1_valid, target_1_x, target_1_y},
      {target_2_valid, target_2_x, target_2_y},
      {target_3_valid, target_3_x, target_3_y},
  };
  this->software_zone_evidence_ =
      compute_software_zone_evidence(this->device_config_cache_, now_ms, targets, &this->exit_zone_last_seen_ms_);
}

SoftwareZoneEvidence RadarApiServer::software_zone_evidence_for_targets(uint32_t now_ms, bool target_1_valid,
                                                                        float target_1_x, float target_1_y,
                                                                        bool target_2_valid, float target_2_x,
                                                                        float target_2_y, bool target_3_valid,
                                                                        float target_3_x, float target_3_y) const {
  const SoftwareZoneTarget targets[3] = {
      {target_1_valid, target_1_x, target_1_y},
      {target_2_valid, target_2_x, target_2_y},
      {target_3_valid, target_3_x, target_3_y},
  };
  uint32_t scratch_exit_last_seen_ms = 0;
  return compute_software_zone_evidence(this->device_config_cache_, now_ms, targets, &scratch_exit_last_seen_ms);
}

void RadarApiServer::update_diagnostic_snapshot(bool presence, bool motion, bool pir_motion, int target_count,
                                                int moving_target_count, int still_target_count,
                                                int still_confidence, bool still_hold_active, int empty_samples,
                                                int range_suspect_count, int range_out_of_range_count,
                                                int range_remote_candidate_count, float illuminance_lux,
                                                const char *presence_reason, const char *presence_off_reason,
                                                const char *motion_reason, const char *still_state,
                                                const char *still_reason, const char *range_reason,
                                                bool exit_zone_active, int exit_zone_mask, int exit_target_count,
                                                int exit_last_seen_age_ms) {
  DiagnosticSnapshot snapshot;
  snapshot.ms = millis();
  snapshot.presence = presence;
  snapshot.motion = motion;
  snapshot.pir_motion = pir_motion;
  snapshot.target_count = target_count;
  snapshot.moving_target_count = moving_target_count;
  snapshot.still_target_count = still_target_count;
  snapshot.still_confidence = still_confidence;
  snapshot.still_hold_active = still_hold_active;
  snapshot.empty_samples = empty_samples;
  snapshot.range_suspect_count = range_suspect_count;
  snapshot.range_out_of_range_count = range_out_of_range_count;
  snapshot.range_remote_candidate_count = range_remote_candidate_count;
  snapshot.illuminance_lux = illuminance_lux;
  snapshot.presence_reason = presence_reason;
  snapshot.presence_off_reason = presence_off_reason;
  snapshot.motion_reason = motion_reason;
  snapshot.still_state = still_state;
  snapshot.still_reason = still_reason;
  snapshot.range_reason = range_reason;
  snapshot.exit_zone_active = exit_zone_active;
  snapshot.exit_zone_mask = exit_zone_mask;
  snapshot.exit_target_count = exit_target_count;
  snapshot.exit_last_seen_age_ms = exit_last_seen_age_ms;
  const auto &tracker = this->presence_tracker_.output();
  snapshot.tracker_presence = tracker.presence;
  snapshot.tracker_motion = tracker.motion;
  snapshot.tracker_track_score = tracker.track_score;
  snapshot.tracker_input_detection_count = tracker.input_detection_count;
  snapshot.tracker_active_track_count = tracker.active_track_count;
  snapshot.tracker_tentative_track_count = tracker.tentative_track_count;
  snapshot.tracker_confirmed_track_count = tracker.confirmed_track_count;
  snapshot.tracker_coasting_track_count = tracker.coasting_track_count;
  snapshot.tracker_moving_track_count = tracker.moving_track_count;
  snapshot.tracker_still_track_count = tracker.still_track_count;
  snapshot.tracker_state = tracker.state;
  snapshot.tracker_reason = tracker.reason;
  const auto &fusion = this->presence_fusion_.output();
  snapshot.static_radar = fusion.static_radar;
  snapshot.pir_evidence = fusion.pir_evidence;
  snapshot.tracker_evidence = fusion.tracker_evidence;
  snapshot.static_assist_armed = fusion.static_assist_armed;
  snapshot.static_assist_active = fusion.static_assist_active;
  snapshot.static_assist_arm_pending = fusion.static_assist_arm_pending;
  snapshot.static_assist_exit_veto = fusion.static_assist_exit_veto;
  snapshot.static_assist_arm_elapsed_ms = fusion.static_assist_arm_elapsed_ms;
  this->diagnostic_log_.update(snapshot);
  this->presence_replay_log_.update(this->last_tracker_input_, this->last_replay_raw_input_, snapshot);
}

}  // namespace radar_api_server
}  // namespace esphome
