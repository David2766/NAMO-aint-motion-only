#include "ld2410_stream_health.h"

#include <cstring>

namespace esphome {
namespace radar_api_server {

namespace {

constexpr uint8_t DATA_FRAME_HEADER[] = {0xF4, 0xF3, 0xF2, 0xF1};
constexpr uint8_t DATA_FRAME_FOOTER[] = {0xF8, 0xF7, 0xF6, 0xF5};
constexpr uint8_t ACK_FRAME_HEADER[] = {0xFD, 0xFC, 0xFB, 0xFA};
constexpr uint8_t ACK_FRAME_FOOTER[] = {0x04, 0x03, 0x02, 0x01};
constexpr uint8_t COMMAND_ENABLE_ENGINEERING = 0x62;
constexpr uint8_t COMMAND_DISABLE_ENGINEERING = 0x63;
constexpr uint8_t COMMAND_QUERY_PARAMETERS = 0x61;
constexpr uint8_t COMMAND_GATE_SENSITIVITY = 0x64;
constexpr uint8_t COMMAND_QUERY_VERSION = 0xA0;
constexpr uint8_t COMMAND_QUERY_MAC = 0xA5;
constexpr uint8_t COMMAND_QUERY_DISTANCE_RESOLUTION = 0xAB;
constexpr uint8_t COMMAND_QUERY_LIGHT_CONTROL = 0xAE;
constexpr uint8_t COMMAND_FACTORY_RESET = 0xA2;

constexpr std::size_t DATA_TYPE_INDEX = 6;
constexpr std::size_t DATA_HEADER_INDEX = 7;
constexpr std::size_t TARGET_STATE_INDEX = 8;
constexpr std::size_t MOVING_DISTANCE_LOW_INDEX = 9;
constexpr std::size_t MOVING_DISTANCE_HIGH_INDEX = 10;
constexpr std::size_t MOVING_ENERGY_INDEX = 11;
constexpr std::size_t STILL_DISTANCE_LOW_INDEX = 12;
constexpr std::size_t STILL_DISTANCE_HIGH_INDEX = 13;
constexpr std::size_t STILL_ENERGY_INDEX = 14;
constexpr std::size_t DETECTION_DISTANCE_LOW_INDEX = 15;
constexpr std::size_t DETECTION_DISTANCE_HIGH_INDEX = 16;

}  // namespace

const char *ld2410_managed_operation_name(
    Ld2410ManagedOperation operation) {
  switch (operation) {
    case Ld2410ManagedOperation::STARTUP_SYNC:
      return "startup_sync";
    case Ld2410ManagedOperation::TUNING_START:
      return "tuning_start";
    case Ld2410ManagedOperation::TUNING_STOP:
      return "tuning_stop";
    case Ld2410ManagedOperation::GATE_WRITE:
      return "gate_write";
    case Ld2410ManagedOperation::FACTORY_RESET:
      return "factory_reset";
    case Ld2410ManagedOperation::NONE:
    default:
      return "none";
  }
}

const char *ld2410_managed_failure_name(Ld2410ManagedFailure failure) {
  switch (failure) {
    case Ld2410ManagedFailure::ACK_REJECTED:
      return "ack_rejected";
    case Ld2410ManagedFailure::ACK_TIMEOUT:
      return "ack_timeout";
    case Ld2410ManagedFailure::UNEXPECTED_ACK:
      return "unexpected_ack";
    case Ld2410ManagedFailure::FRAME_TIMEOUT:
      return "frame_timeout";
    case Ld2410ManagedFailure::NONE:
    default:
      return "none";
  }
}

void Ld2410CommandTransaction::reset() {
  this->state_ = State::IDLE;
  this->operation_ = Ld2410ManagedOperation::NONE;
  this->pending_failure_ = Ld2410ManagedFailure::NONE;
  this->deadline_ms_ = 0;
  this->ack_cursor_ = 0;
  this->frame_baseline_ = 0;
  this->expected_command_ = 0;
  this->failure_command_ = 0;
  this->step_ = 0;
  this->gate_ = 0;
  this->sensitivity_ = 0;
  this->failure_ack_result_ = 0;
  this->ignored_ack_count_ = 0;
  this->last_ignored_command_ = 0;
  this->last_ignored_result_ = 0;
  this->aborting_ = false;
}

bool Ld2410CommandTransaction::start(
    Ld2410ManagedOperation operation, uint32_t now_ms,
    const Ld2410StreamSnapshot &stream, uint8_t gate,
    uint8_t sensitivity) {
  if (this->busy() || operation == Ld2410ManagedOperation::NONE || gate >= 9 ||
      sensitivity > 100) {
    return false;
  }
  this->operation_ = operation;
  this->state_ = State::SEND_NEXT;
  this->pending_failure_ = Ld2410ManagedFailure::NONE;
  this->deadline_ms_ = now_ms;
  this->ack_cursor_ = stream.ack_sequence;
  this->frame_baseline_ = stream.periodic_frames;
  this->expected_command_ = 0;
  this->step_ = 0;
  this->gate_ = gate;
  this->sensitivity_ = sensitivity;
  this->failure_command_ = 0;
  this->failure_ack_result_ = 0;
  this->ignored_ack_count_ = 0;
  this->last_ignored_command_ = 0;
  this->last_ignored_result_ = 0;
  this->aborting_ = false;
  return true;
}

Ld2410ManagedAction Ld2410CommandTransaction::update(
    uint32_t now_ms, const Ld2410StreamSnapshot &stream) {
  if (!this->busy())
    return {};

  if (this->state_ == State::SEND_NEXT ||
      this->state_ == State::SEND_ABORT_EXIT) {
    Ld2410ManagedAction action{};
    action.event = Ld2410ManagedEvent::SEND_COMMAND;
    action.operation = this->operation_;
    if (this->state_ == State::SEND_ABORT_EXIT) {
      action.command = LD2410_COMMAND_DISABLE_CONFIGURATION;
      this->aborting_ = true;
    } else if (!this->command_for_step_(this->step_, &action)) {
      return this->finish_(Ld2410ManagedEvent::FAILED,
                           Ld2410ManagedFailure::UNEXPECTED_ACK);
    }
    this->expected_command_ = action.command;
    this->ack_cursor_ = stream.ack_sequence;
    if (this->step_ + 1 >= this->sequence_length_() && !this->aborting_)
      this->frame_baseline_ = stream.periodic_frames;
    this->deadline_ms_ = now_ms + ACK_TIMEOUT_MS;
    this->state_ = State::WAIT_ACK;
    return action;
  }

  if (this->state_ == State::WAIT_ACK) {
    // A restart may reset the UART before its ACK reaches the host. A new
    // normal periodic frame is stronger evidence that the final restart
    // command completed successfully.
    if (this->operation_ == Ld2410ManagedOperation::FACTORY_RESET &&
        this->expected_command_ == LD2410_COMMAND_RESTART &&
        stream.periodic_frames > this->frame_baseline_ &&
        !stream.engineering_mode) {
      return this->finish_(Ld2410ManagedEvent::COMPLETED,
                           Ld2410ManagedFailure::NONE);
    }
    const Ld2410AckEvent *matching_ack = nullptr;
    for (uint8_t index = 0; index < stream.ack_event_count; index++) {
      const auto &event = stream.ack_events[index];
      if (!sequence_after_(event.sequence, this->ack_cursor_))
        continue;
      this->ack_cursor_ = event.sequence;
      if (event.command != this->expected_command_) {
        if (this->ignored_ack_count_ != UINT8_MAX)
          this->ignored_ack_count_++;
        this->last_ignored_command_ = event.command;
        this->last_ignored_result_ = event.result;
        continue;
      }
      matching_ack = &event;
      break;
    }
    if (matching_ack != nullptr) {
      if (matching_ack->result != 0) {
        if (this->aborting_) {
          this->state_ = State::REPORT_FAILURE;
        } else {
          this->begin_failure_(Ld2410ManagedFailure::ACK_REJECTED,
                               matching_ack->result);
        }
        return {};
      }
      if (this->aborting_) {
        this->state_ = State::REPORT_FAILURE;
        return {};
      }
      this->step_++;
      if (this->step_ >= this->sequence_length_()) {
        this->state_ = State::WAIT_FRAME;
        this->deadline_ms_ = now_ms +
            (this->operation_ == Ld2410ManagedOperation::FACTORY_RESET
                 ? RESTART_FRAME_TIMEOUT_MS
                 : FRAME_TIMEOUT_MS);
      } else {
        this->state_ = State::SEND_NEXT;
      }
      return {};
    }
    if (time_reached_(now_ms, this->deadline_ms_)) {
      if (this->aborting_)
        this->state_ = State::REPORT_FAILURE;
      else
        this->begin_failure_(Ld2410ManagedFailure::ACK_TIMEOUT);
    }
    return {};
  }

  if (this->state_ == State::WAIT_FRAME) {
    if (stream.periodic_frames > this->frame_baseline_ &&
        stream.engineering_mode == this->expected_engineering_mode_()) {
      return this->finish_(Ld2410ManagedEvent::COMPLETED,
                           Ld2410ManagedFailure::NONE);
    }
    if (time_reached_(now_ms, this->deadline_ms_))
      return this->finish_(Ld2410ManagedEvent::FAILED,
                           Ld2410ManagedFailure::FRAME_TIMEOUT);
    return {};
  }

  if (this->state_ == State::REPORT_FAILURE)
    return this->finish_(Ld2410ManagedEvent::FAILED,
                         this->pending_failure_);
  return {};
}

bool Ld2410CommandTransaction::time_reached_(uint32_t now_ms,
                                             uint32_t deadline_ms) {
  return static_cast<int32_t>(now_ms - deadline_ms) >= 0;
}

bool Ld2410CommandTransaction::sequence_after_(uint32_t sequence,
                                                uint32_t baseline) {
  return static_cast<int32_t>(sequence - baseline) > 0;
}

uint8_t Ld2410CommandTransaction::sequence_length_() const {
  switch (this->operation_) {
    case Ld2410ManagedOperation::STARTUP_SYNC:
      return 7;
    case Ld2410ManagedOperation::TUNING_START:
      return 4;
    case Ld2410ManagedOperation::TUNING_STOP:
      return 3;
    case Ld2410ManagedOperation::GATE_WRITE:
      return 4;
    case Ld2410ManagedOperation::FACTORY_RESET:
      return 3;
    case Ld2410ManagedOperation::NONE:
    default:
      return 0;
  }
}

bool Ld2410CommandTransaction::command_for_step_(
    uint8_t step, Ld2410ManagedAction *action) const {
  action->payload_size = 0;
  if (step == 0) {
    action->command = LD2410_COMMAND_ENABLE_CONFIGURATION;
    action->payload[0] = 0x01;
    action->payload[1] = 0x00;
    action->payload_size = 2;
    return true;
  }

  if (this->operation_ == Ld2410ManagedOperation::STARTUP_SYNC) {
    constexpr uint8_t commands[] = {
        COMMAND_QUERY_VERSION, COMMAND_QUERY_MAC,
        COMMAND_QUERY_DISTANCE_RESOLUTION, COMMAND_QUERY_LIGHT_CONTROL,
        COMMAND_QUERY_PARAMETERS, LD2410_COMMAND_DISABLE_CONFIGURATION};
    if (step > sizeof(commands))
      return false;
    action->command = commands[step - 1];
    if (action->command == COMMAND_QUERY_MAC) {
      action->payload[0] = 0x01;
      action->payload[1] = 0x00;
      action->payload_size = 2;
    }
    return true;
  }

  if (this->operation_ == Ld2410ManagedOperation::TUNING_START ||
      this->operation_ == Ld2410ManagedOperation::TUNING_STOP) {
    if (step == 1) {
      action->command =
          this->operation_ == Ld2410ManagedOperation::TUNING_START
              ? COMMAND_ENABLE_ENGINEERING
              : COMMAND_DISABLE_ENGINEERING;
      return true;
    }
    if (this->operation_ == Ld2410ManagedOperation::TUNING_START &&
        step == 2) {
      action->command = COMMAND_QUERY_PARAMETERS;
      return true;
    }
    if (step == this->sequence_length_() - 1) {
      action->command = LD2410_COMMAND_DISABLE_CONFIGURATION;
      return true;
    }
    return false;
  }

  if (this->operation_ == Ld2410ManagedOperation::GATE_WRITE) {
    if (step == 1) {
      const uint8_t threshold = static_cast<uint8_t>(100 - this->sensitivity_);
      const uint8_t payload[] = {
          0x00, 0x00, this->gate_, 0x00, 0x00, 0x00,
          0x01, 0x00, threshold, 0x00, 0x00, 0x00,
          0x02, 0x00, threshold, 0x00, 0x00, 0x00};
      std::memcpy(action->payload, payload, sizeof(payload));
      action->payload_size = sizeof(payload);
      action->command = COMMAND_GATE_SENSITIVITY;
      return true;
    }
    if (step == 2) {
      action->command = COMMAND_QUERY_PARAMETERS;
      return true;
    }
    if (step == 3) {
      action->command = LD2410_COMMAND_DISABLE_CONFIGURATION;
      return true;
    }
    return false;
  }

  if (this->operation_ == Ld2410ManagedOperation::FACTORY_RESET) {
    if (step == 1) {
      action->command = COMMAND_FACTORY_RESET;
      return true;
    }
    if (step == 2) {
      action->command = LD2410_COMMAND_RESTART;
      return true;
    }
  }
  return false;
}

bool Ld2410CommandTransaction::expected_engineering_mode_() const {
  return this->operation_ == Ld2410ManagedOperation::TUNING_START ||
         this->operation_ == Ld2410ManagedOperation::GATE_WRITE;
}

void Ld2410CommandTransaction::begin_failure_(
    Ld2410ManagedFailure failure, uint16_t ack_result) {
  this->pending_failure_ = failure;
  this->failure_command_ = this->expected_command_;
  this->failure_ack_result_ = ack_result;
  if (this->expected_command_ != LD2410_COMMAND_DISABLE_CONFIGURATION &&
      this->expected_command_ != LD2410_COMMAND_RESTART) {
    this->state_ = State::SEND_ABORT_EXIT;
  } else {
    this->state_ = State::REPORT_FAILURE;
  }
}

Ld2410ManagedAction Ld2410CommandTransaction::finish_(
    Ld2410ManagedEvent event, Ld2410ManagedFailure failure) {
  Ld2410ManagedAction action{};
  action.event = event;
  action.operation = this->operation_;
  action.failure = failure;
  action.command = failure == Ld2410ManagedFailure::NONE
                       ? this->expected_command_
                       : this->failure_command_;
  action.ack_result = this->failure_ack_result_;
  action.ignored_acks = this->ignored_ack_count_;
  action.last_ignored_command = this->last_ignored_command_;
  action.last_ignored_result = this->last_ignored_result_;
  this->reset();
  return action;
}

void Ld2410StreamHealth::reset(uint32_t now_ms) {
  this->reset_frame_();
  this->periodic_header_match_ = 0;
  this->ack_header_match_ = 0;
  this->boot_ms_ = now_ms;
  this->first_byte_ms_ = 0;
  this->first_valid_frame_ms_ = 0;
  this->last_byte_ms_ = 0;
  this->last_valid_frame_ms_ = 0;
  this->stall_started_ms_ = 0;
  this->last_stall_duration_ms_ = 0;
  this->last_unhealthy_report_ms_ = now_ms;
  this->rx_bytes_at_last_valid_frame_ = 0;
  this->rx_bytes_ = 0;
  this->periodic_frames_ = 0;
  this->ack_frames_ = 0;
  this->failed_ack_frames_ = 0;
  this->ack_sequence_ = 0;
  this->invalid_frames_ = 0;
  this->discarded_bytes_ = 0;
  this->moving_distance_cm_ = 0;
  this->still_distance_cm_ = 0;
  this->detection_distance_cm_ = 0;
  this->moving_energy_ = 0;
  this->still_energy_ = 0;
  this->target_state_ = 0;
  this->last_ack_command_ = 0;
  this->last_ack_result_ = 0;
  this->ack_event_head_ = 0;
  this->ack_event_count_ = 0;
  for (auto &event : this->ack_events_)
    event = {};
  this->engineering_mode_ = false;
  this->rx_seen_ = false;
  this->valid_frame_seen_ = false;
  this->startup_reported_ = false;
  this->healthy_ever_ = false;
  this->stalled_ = false;
  this->pending_rx_started_ = false;
  this->pending_first_valid_frame_ = false;
  this->pending_stream_resumed_ = false;
  this->ack_seen_ = false;
  this->configuration_mode_open_ = false;
}

void Ld2410StreamHealth::observe_rx_byte(uint8_t byte, uint32_t now_ms) {
  this->rx_bytes_++;
  this->last_byte_ms_ = now_ms;
  if (!this->rx_seen_) {
    this->rx_seen_ = true;
    this->first_byte_ms_ = now_ms;
    this->pending_rx_started_ = true;
  }

  if (this->frame_kind_ == FrameKind::NONE) {
    this->observe_idle_byte_(byte);
  } else {
    this->append_frame_byte_(byte, now_ms);
  }
}

void Ld2410StreamHealth::observe_ack(uint8_t command, uint16_t result,
                                     uint32_t now_ms) {
  this->ack_seen_ = true;
  this->last_byte_ms_ = now_ms;
  this->last_ack_command_ = command;
  this->last_ack_result_ = result;
  this->ack_sequence_++;
  if (this->ack_sequence_ == 0)
    this->ack_sequence_++;
  this->ack_events_[this->ack_event_head_] = {
      this->ack_sequence_, result, command};
  this->ack_event_head_ = static_cast<uint8_t>(
      (this->ack_event_head_ + 1) % LD2410_ACK_HISTORY_CAPACITY);
  if (this->ack_event_count_ < LD2410_ACK_HISTORY_CAPACITY)
    this->ack_event_count_++;
  if (result == 0) {
    this->ack_frames_++;
    if (command == LD2410_COMMAND_ENABLE_CONFIGURATION) {
      this->configuration_mode_open_ = true;
    } else if (command == LD2410_COMMAND_DISABLE_CONFIGURATION ||
               command == LD2410_COMMAND_RESTART) {
      this->configuration_mode_open_ = false;
    }
  } else {
    this->failed_ack_frames_++;
    this->invalid_frames_++;
  }
}

Ld2410StreamAction Ld2410StreamHealth::update(uint32_t now_ms) {
  if (this->pending_rx_started_) {
    this->pending_rx_started_ = false;
    return Ld2410StreamAction::RX_STARTED;
  }

  if (this->pending_first_valid_frame_) {
    this->pending_first_valid_frame_ = false;
    this->startup_reported_ = true;
    this->healthy_ever_ = true;
    if (this->stalled_) {
      this->last_stall_duration_ms_ = now_ms - this->stall_started_ms_;
      this->stalled_ = false;
    }
    return Ld2410StreamAction::STARTUP_HEALTHY;
  }

  if (this->pending_stream_resumed_) {
    this->pending_stream_resumed_ = false;
    this->last_stall_duration_ms_ = now_ms - this->stall_started_ms_;
    this->stalled_ = false;
    return Ld2410StreamAction::STREAM_RESUMED;
  }

  if (!this->startup_reported_ &&
      now_ms - this->boot_ms_ >= STARTUP_GRACE_MS) {
    this->startup_reported_ = true;
    this->stalled_ = true;
    this->stall_started_ms_ = now_ms;
    this->last_unhealthy_report_ms_ = now_ms;
    return this->rx_seen_ ? Ld2410StreamAction::STARTUP_NO_VALID_FRAME
                          : Ld2410StreamAction::STARTUP_NO_DATA;
  }

  if (this->healthy_ever_ && !this->stalled_ &&
      now_ms - this->last_valid_frame_ms_ >= RUNTIME_STALE_MS) {
    this->stalled_ = true;
    this->stall_started_ms_ = now_ms;
    this->last_unhealthy_report_ms_ = now_ms;
    const bool bytes_continue =
        now_ms - this->last_byte_ms_ < RUNTIME_STALE_MS &&
        this->rx_bytes_ > this->rx_bytes_at_last_valid_frame_;
    return bytes_continue ? Ld2410StreamAction::STREAM_STALLED_INVALID
                          : Ld2410StreamAction::STREAM_STALLED_SILENT;
  }

  if (this->stalled_ &&
      now_ms - this->last_unhealthy_report_ms_ >=
          UNHEALTHY_REPORT_INTERVAL_MS) {
    this->last_unhealthy_report_ms_ = now_ms;
    return Ld2410StreamAction::UNHEALTHY_SUMMARY;
  }

  return Ld2410StreamAction::NONE;
}

Ld2410StreamSnapshot Ld2410StreamHealth::snapshot(uint32_t now_ms) const {
  Ld2410StreamSnapshot out{};
  out.rx_bytes = this->rx_bytes_;
  out.periodic_frames = this->periodic_frames_;
  out.ack_frames = this->ack_frames_;
  out.failed_ack_frames = this->failed_ack_frames_;
  out.ack_sequence = this->ack_sequence_;
  out.invalid_frames = this->invalid_frames_;
  out.discarded_bytes = this->discarded_bytes_;
  out.first_byte_delay_ms =
      this->rx_seen_ ? this->first_byte_ms_ - this->boot_ms_ : 0;
  out.first_frame_delay_ms =
      this->valid_frame_seen_ ? this->first_valid_frame_ms_ - this->boot_ms_
                              : 0;
  out.last_byte_age_ms =
      this->rx_seen_ ? now_ms - this->last_byte_ms_ : now_ms - this->boot_ms_;
  out.last_frame_age_ms =
      this->valid_frame_seen_ ? now_ms - this->last_valid_frame_ms_
                              : now_ms - this->boot_ms_;
  out.stall_duration_ms = this->stalled_
                              ? now_ms - this->stall_started_ms_
                              : this->last_stall_duration_ms_;
  out.moving_distance_cm = this->moving_distance_cm_;
  out.still_distance_cm = this->still_distance_cm_;
  out.detection_distance_cm = this->detection_distance_cm_;
  out.moving_energy = this->moving_energy_;
  out.still_energy = this->still_energy_;
  out.target_state = this->target_state_;
  out.last_ack_command = this->last_ack_command_;
  out.last_ack_result = this->last_ack_result_;
  out.ack_event_count = this->ack_event_count_;
  const uint8_t oldest = static_cast<uint8_t>(
      (this->ack_event_head_ + LD2410_ACK_HISTORY_CAPACITY -
       this->ack_event_count_) % LD2410_ACK_HISTORY_CAPACITY);
  for (uint8_t index = 0; index < this->ack_event_count_; index++) {
    out.ack_events[index] = this->ack_events_[
        (oldest + index) % LD2410_ACK_HISTORY_CAPACITY];
  }
  out.engineering_mode = this->engineering_mode_;
  out.rx_seen = this->rx_seen_;
  out.valid_frame_seen = this->valid_frame_seen_;
  out.stalled = this->stalled_;
  out.ack_seen = this->ack_seen_;
  out.configuration_mode_open = this->configuration_mode_open_;
  return out;
}

void Ld2410StreamHealth::observe_idle_byte_(uint8_t byte) {
  this->periodic_header_match_ = advance_header_match_(
      DATA_FRAME_HEADER, this->periodic_header_match_, byte);
  this->ack_header_match_ =
      advance_header_match_(ACK_FRAME_HEADER, this->ack_header_match_, byte);

  if (this->periodic_header_match_ == HEADER_SIZE) {
    this->frame_kind_ = FrameKind::PERIODIC;
    std::memcpy(this->frame_, DATA_FRAME_HEADER, HEADER_SIZE);
    this->frame_size_ = HEADER_SIZE;
    this->periodic_header_match_ = 0;
    this->ack_header_match_ = 0;
  } else if (this->ack_header_match_ == HEADER_SIZE) {
    this->frame_kind_ = FrameKind::ACK;
    std::memcpy(this->frame_, ACK_FRAME_HEADER, HEADER_SIZE);
    this->frame_size_ = HEADER_SIZE;
    this->periodic_header_match_ = 0;
    this->ack_header_match_ = 0;
  } else if (this->periodic_header_match_ == 0 &&
             this->ack_header_match_ == 0) {
    this->discarded_bytes_++;
  }
}

void Ld2410StreamHealth::append_frame_byte_(uint8_t byte, uint32_t now_ms) {
  if (this->frame_size_ >= MAX_FRAME_SIZE) {
    this->invalid_frames_++;
    this->reset_frame_();
    this->observe_idle_byte_(byte);
    return;
  }

  this->frame_[this->frame_size_++] = byte;
  const uint8_t *footer = this->frame_kind_ == FrameKind::PERIODIC
                              ? DATA_FRAME_FOOTER
                              : ACK_FRAME_FOOTER;
  if (ends_with_(this->frame_, this->frame_size_, footer, HEADER_SIZE))
    this->complete_frame_(now_ms);
}

void Ld2410StreamHealth::complete_frame_(uint32_t now_ms) {
  if (this->frame_kind_ == FrameKind::PERIODIC) {
    const bool valid =
        this->frame_size_ >= 21 &&
        this->frame_[DATA_HEADER_INDEX] == 0xAA &&
        this->frame_[this->frame_size_ - 6] == 0x55 &&
        this->frame_[this->frame_size_ - 5] == 0x00;
    if (valid) {
      this->periodic_frames_++;
      this->last_valid_frame_ms_ = now_ms;
      this->rx_bytes_at_last_valid_frame_ = this->rx_bytes_;
      this->engineering_mode_ = this->frame_[DATA_TYPE_INDEX] == 0x01;
      this->target_state_ = this->frame_[TARGET_STATE_INDEX];
      this->moving_distance_cm_ =
          decode_u16_(this->frame_[MOVING_DISTANCE_LOW_INDEX],
                      this->frame_[MOVING_DISTANCE_HIGH_INDEX]);
      this->moving_energy_ = this->frame_[MOVING_ENERGY_INDEX];
      this->still_distance_cm_ =
          decode_u16_(this->frame_[STILL_DISTANCE_LOW_INDEX],
                      this->frame_[STILL_DISTANCE_HIGH_INDEX]);
      this->still_energy_ = this->frame_[STILL_ENERGY_INDEX];
      this->detection_distance_cm_ =
          decode_u16_(this->frame_[DETECTION_DISTANCE_LOW_INDEX],
                      this->frame_[DETECTION_DISTANCE_HIGH_INDEX]);
      if (!this->valid_frame_seen_) {
        this->valid_frame_seen_ = true;
        this->first_valid_frame_ms_ = now_ms;
        this->pending_first_valid_frame_ = true;
      } else if (this->stalled_) {
        this->pending_stream_resumed_ = true;
      }
    } else {
      this->invalid_frames_++;
    }
  } else {
    if (this->canonical_ack_source_) {
      this->reset_frame_();
      return;
    }
    const bool valid_envelope =
        this->frame_size_ >= 14 && this->frame_[7] == 0x01;
    if (!valid_envelope) {
      this->invalid_frames_++;
    } else {
      this->observe_ack(this->frame_[6],
                        decode_u16_(this->frame_[8], this->frame_[9]),
                        now_ms);
    }
  }
  this->reset_frame_();
}

void Ld2410StreamHealth::reset_frame_() {
  this->frame_size_ = 0;
  this->frame_kind_ = FrameKind::NONE;
}

uint8_t Ld2410StreamHealth::advance_header_match_(const uint8_t *header,
                                                  uint8_t matched,
                                                  uint8_t byte) {
  if (byte == header[matched])
    return matched + 1;
  return byte == header[0] ? 1 : 0;
}

bool Ld2410StreamHealth::ends_with_(const uint8_t *data, std::size_t size,
                                    const uint8_t *suffix,
                                    std::size_t suffix_size) {
  return size >= suffix_size &&
         std::memcmp(data + size - suffix_size, suffix, suffix_size) == 0;
}

uint16_t Ld2410StreamHealth::decode_u16_(uint8_t low, uint8_t high) {
  return static_cast<uint16_t>(low) |
         (static_cast<uint16_t>(high) << 8U);
}

void Ld2410Watchdog::reset(uint32_t now_ms) {
  this->state_ = State::MONITORING;
  (void) now_ms;
  this->deadline_ms_ = 0;
  this->periodic_baseline_ = 0;
  this->ack_cursor_ = 0;
  this->attempts_ = 0;
  this->restart_sent_ = false;
}

Ld2410RecoveryAction Ld2410Watchdog::update(
    uint32_t now_ms, const Ld2410StreamSnapshot &stream,
    bool command_busy, bool force_recovery) {
  if ((this->state_ == State::WAIT_FRAME ||
       (this->state_ == State::COOLDOWN && this->restart_sent_)) &&
      this->received_recovery_frame_(stream)) {
    this->state_ = State::MONITORING;
    this->restart_sent_ = false;
    return Ld2410RecoveryAction::RECOVERED;
  }

  if (this->state_ == State::MONITORING) {
    const uint32_t stale_after_ms = stream.valid_frame_seen
                                        ? RUNTIME_STALE_MS
                                        : STARTUP_STALE_MS;
    if ((!force_recovery && stream.last_frame_age_ms < stale_after_ms) ||
        command_busy)
      return Ld2410RecoveryAction::NONE;
    return this->begin_recovery_(now_ms, stream);
  }

  if (this->state_ == State::COOLDOWN) {
    if (!time_reached_(now_ms, this->deadline_ms_) || command_busy)
      return Ld2410RecoveryAction::NONE;
    return this->begin_recovery_(now_ms, stream);
  }

  if (this->state_ == State::WAIT_ENABLE_ACK) {
    if (this->received_successful_ack_(
            stream, LD2410_COMMAND_ENABLE_CONFIGURATION)) {
      this->state_ = State::WAIT_FRAME;
      this->deadline_ms_ = now_ms + RESTART_TIMEOUT_MS;
      this->restart_sent_ = true;
      return Ld2410RecoveryAction::RESTART;
    }
    if (!time_reached_(now_ms, this->deadline_ms_))
      return Ld2410RecoveryAction::NONE;
    this->state_ = State::COOLDOWN;
    this->deadline_ms_ = now_ms + RETRY_COOLDOWN_MS;
    return Ld2410RecoveryAction::GIVE_UP;
  }

  if (this->state_ == State::WAIT_FRAME &&
      time_reached_(now_ms, this->deadline_ms_)) {
    this->state_ = State::COOLDOWN;
    this->deadline_ms_ = now_ms + RETRY_COOLDOWN_MS;
    return Ld2410RecoveryAction::GIVE_UP;
  }
  return Ld2410RecoveryAction::NONE;
}

bool Ld2410Watchdog::time_reached_(uint32_t now_ms,
                                    uint32_t deadline_ms) {
  return static_cast<int32_t>(now_ms - deadline_ms) >= 0;
}

bool Ld2410Watchdog::sequence_after_(uint32_t sequence,
                                     uint32_t baseline) {
  return static_cast<int32_t>(sequence - baseline) > 0;
}

bool Ld2410Watchdog::received_successful_ack_(
    const Ld2410StreamSnapshot &stream, uint8_t command) {
  for (uint8_t index = 0; index < stream.ack_event_count; index++) {
    const auto &event = stream.ack_events[index];
    if (!sequence_after_(event.sequence, this->ack_cursor_))
      continue;
    this->ack_cursor_ = event.sequence;
    if (event.command == command)
      return event.result == 0;
  }
  return false;
}

bool Ld2410Watchdog::received_recovery_frame_(
    const Ld2410StreamSnapshot &stream) const {
  return stream.periodic_frames > this->periodic_baseline_;
}

Ld2410RecoveryAction Ld2410Watchdog::begin_recovery_(
    uint32_t now_ms, const Ld2410StreamSnapshot &stream) {
  this->attempts_++;
  this->periodic_baseline_ = stream.periodic_frames;
  this->ack_cursor_ = stream.ack_sequence;
  this->restart_sent_ = false;
  if (stream.configuration_mode_open) {
    this->state_ = State::WAIT_FRAME;
    this->deadline_ms_ = now_ms + RESTART_TIMEOUT_MS;
    this->restart_sent_ = true;
    return Ld2410RecoveryAction::RESTART;
  }
  this->state_ = State::WAIT_ENABLE_ACK;
  this->deadline_ms_ = now_ms + ACK_TIMEOUT_MS;
  return Ld2410RecoveryAction::ENTER_CONFIGURATION;
}

}  // namespace radar_api_server
}  // namespace esphome
