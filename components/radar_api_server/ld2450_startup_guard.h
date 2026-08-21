#pragma once

#include <cstdint>

namespace esphome {
namespace radar_api_server {

enum class Ld2450StartupAction : uint8_t {
  NONE = 0,
  STARTUP_HEALTHY,
  STARTUP_MISSING,
  STREAM_STALLED,
  STREAM_RESUMED,
};

class Ld2450FrameGate {
 public:
  void observe_frame() { this->observed_sequence_++; }

  bool consume_latest() {
    if (this->consumed_sequence_ == this->observed_sequence_)
      return false;
    this->consumed_sequence_ = this->observed_sequence_;
    return true;
  }

 private:
  uint32_t observed_sequence_{0};
  uint32_t consumed_sequence_{0};
};

class Ld2450StartupGuard {
 public:
  static constexpr uint32_t STARTUP_GRACE_MS = 5000;
  static constexpr uint32_t FRAME_FRESHNESS_MS = 1500;
  static constexpr uint32_t RUNTIME_STALE_MS = 5000;

  void reset(uint32_t now_ms) {
    this->deadline_ms_ = now_ms + STARTUP_GRACE_MS;
    this->last_valid_frame_ms_ = 0;
    this->valid_frame_count_ = 0;
    this->valid_frame_seen_ = false;
    this->finished_ = false;
    this->stream_ever_healthy_ = false;
    this->stream_stalled_reported_ = false;
  }

  void observe_valid_frame(uint32_t now_ms) {
    this->last_valid_frame_ms_ = now_ms;
    this->valid_frame_count_++;
    this->valid_frame_seen_ = true;
  }

  Ld2450StartupAction update(uint32_t now_ms) {
    if (!this->finished_) {
      if (!time_reached_(now_ms, this->deadline_ms_))
        return Ld2450StartupAction::NONE;
      if (this->valid_frame_seen_ &&
          now_ms - this->last_valid_frame_ms_ <= FRAME_FRESHNESS_MS) {
        this->finished_ = true;
        this->stream_ever_healthy_ = true;
        return Ld2450StartupAction::STARTUP_HEALTHY;
      }
      this->finished_ = true;
      this->stream_stalled_reported_ = true;
      return Ld2450StartupAction::STARTUP_MISSING;
    }

    const bool stream_fresh =
        this->valid_frame_seen_ &&
        now_ms - this->last_valid_frame_ms_ <= RUNTIME_STALE_MS;
    if (stream_fresh) {
      this->stream_ever_healthy_ = true;
      if (this->stream_stalled_reported_) {
        this->stream_stalled_reported_ = false;
        return Ld2450StartupAction::STREAM_RESUMED;
      }
      return Ld2450StartupAction::NONE;
    }

    if (this->stream_ever_healthy_ && !this->stream_stalled_reported_) {
      this->stream_stalled_reported_ = true;
      return Ld2450StartupAction::STREAM_STALLED;
    }
    return Ld2450StartupAction::NONE;
  }

  bool valid_frame_seen() const { return this->valid_frame_seen_; }
  uint32_t valid_frame_count() const { return this->valid_frame_count_; }
  uint32_t last_valid_frame_age_ms(uint32_t now_ms) const {
    return this->valid_frame_seen_ ? now_ms - this->last_valid_frame_ms_ : 0;
  }

 private:
  static bool time_reached_(uint32_t now_ms, uint32_t deadline_ms) {
    return static_cast<int32_t>(now_ms - deadline_ms) >= 0;
  }

  uint32_t deadline_ms_{0};
  uint32_t last_valid_frame_ms_{0};
  uint32_t valid_frame_count_{0};
  bool valid_frame_seen_{false};
  bool finished_{true};
  bool stream_ever_healthy_{false};
  bool stream_stalled_reported_{false};
};

}  // namespace radar_api_server
}  // namespace esphome
