#include "pairing_handler.h"

#include "http_response.h"
#include "radar_api_server.h"

#include "esphome/core/hal.h"

#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

namespace esphome {
namespace radar_api_server {

namespace {

constexpr uint32_t DEFAULT_SESSION_MS = 120000;
constexpr uint32_t DEFAULT_INVITATION_MS = 180000;

int hex_value(char value) {
  if (value >= '0' && value <= '9')
    return value - '0';
  if (value >= 'a' && value <= 'f')
    return value - 'a' + 10;
  if (value >= 'A' && value <= 'F')
    return value - 'A' + 10;
  return -1;
}

template<std::size_t Size>
bool parse_hex(const std::string &text, std::array<uint8_t, Size> *output) {
  if (output == nullptr || text.size() != Size * 2)
    return false;
  for (std::size_t index = 0; index < Size; ++index) {
    const int high = hex_value(text[index * 2]);
    const int low = hex_value(text[index * 2 + 1]);
    if (high < 0 || low < 0)
      return false;
    (*output)[index] = static_cast<uint8_t>((high << 4) | low);
  }
  return true;
}

template<std::size_t Size>
void print_hex(AsyncResponseStream *stream,
               const std::array<uint8_t, Size> &value) {
  static const char HEX[] = "0123456789abcdef";
  char pair[3]{};
  for (uint8_t byte : value) {
    pair[0] = HEX[byte >> 4U];
    pair[1] = HEX[byte & 0x0FU];
    stream->print(pair);
  }
}

bool parse_uint32(const std::string &text, uint32_t *value) {
  if (value == nullptr || text.empty() || text.size() > 10)
    return false;
  uint64_t parsed = 0;
  for (std::size_t index = 0; index < text.size(); ++index) {
    if (!std::isdigit(static_cast<unsigned char>(text[index])))
      return false;
    parsed = parsed * 10U + static_cast<uint32_t>(text[index] - '0');
    if (parsed > UINT32_MAX)
      return false;
  }
  *value = static_cast<uint32_t>(parsed);
  return true;
}

bool copy_identifier(const std::string &source, char *target,
                     std::size_t capacity) {
  if (target == nullptr || capacity == 0 || source.empty() ||
      source.size() >= capacity)
    return false;
  for (std::size_t index = 0; index < source.size(); ++index) {
    const char value = source[index];
    const bool alphanumeric =
        (value >= 'A' && value <= 'Z') ||
        (value >= 'a' && value <= 'z') ||
        (value >= '0' && value <= '9');
    if ((!alphanumeric && value != '-' && value != '_' && value != '.' &&
         value != ':') ||
        (index == 0 && !alphanumeric))
      return false;
  }
  std::memcpy(target, source.c_str(), source.size());
  target[source.size()] = '\0';
  return true;
}

void send_invalid(AsyncWebServerRequest *request, const char *code) {
  http_response::send_error_info(request, 400, code, code, "error", "{}");
}

void send_accepted(AsyncWebServerRequest *request, uint32_t operation_id) {
  char body[80];
  std::snprintf(body, sizeof(body),
                "{\"ok\":true,\"accepted\":true,\"operationId\":%u}",
                static_cast<unsigned>(operation_id));
  request->send(200, "application/json", body);
}

}  // namespace

bool PairingHandler::can_handle(AsyncWebServerRequest *request,
                                StringRef url) const {
  return (request->method() == HTTP_GET || request->method() == HTTP_POST) &&
         url == "/api/platform/pairing";
}

bool PairingHandler::handle(AsyncWebServerRequest *request, StringRef) {
  if (request->method() == HTTP_GET)
    this->handle_get_(request);
  else
    this->handle_post_(request);
  return true;
}

void PairingHandler::handle_get_(AsyncWebServerRequest *request) {
  const auto snapshot = this->server_->participant_pairing_runtime().snapshot(millis());
  auto *stream = request->beginResponseStream("application/json");
  stream->printf(
      "{\"ok\":true,\"supported\":true,\"busy\":%s,"
      "\"acceptedOperationId\":%u,\"completedOperationId\":%u,"
      "\"lastOperation\":\"%s\",\"operationResult\":\"%s\","
      "\"commitResult\":\"%s\",\"state\":\"%s\",\"role\":\"%s\","
      "\"remainingMs\":%u,\"prepareRateLimited\":%u,"
      "\"peerVerified\":%s,"
      "\"peerParticipantId\":\"%s\",\"leaderSiteId\":\"%s\"",
      snapshot.busy ? "true" : "false",
      static_cast<unsigned>(snapshot.accepted_operation_id),
      static_cast<unsigned>(snapshot.completed_operation_id),
      participant_pairing_operation_to_string(snapshot.last_operation),
      participant_pairing_result_to_string(snapshot.operation_result),
      participant_pairing_commit_result_to_string(snapshot.commit_result),
      participant_pairing_state_to_string(snapshot.state),
      participant_pairing_role_to_string(snapshot.role),
      static_cast<unsigned>(snapshot.remaining_ms),
      static_cast<unsigned>(snapshot.prepare_rate_limited),
      snapshot.peer_verified ? "true" : "false",
      snapshot.peer_participant_id, snapshot.leader_site_id);
  if (snapshot.has_offer) {
    stream->printf(
        ",\"offer\":{\"role\":\"%s\",\"participantId\":\"%s\","
        "\"siteId\":\"%s\",\"sessionId\":\"",
        participant_pairing_role_to_string(snapshot.offer.role),
        snapshot.offer.participant_id, snapshot.offer.site_id);
    print_hex(stream, snapshot.offer.session_id);
    stream->print("\",\"nonce\":\"");
    print_hex(stream, snapshot.offer.nonce);
    stream->print("\",\"publicKey\":\"");
    print_hex(stream, snapshot.offer.public_key);
    stream->print("\"}");
  } else {
    stream->print(",\"offer\":null");
  }
  if (snapshot.has_proof) {
    stream->print(",\"localProof\":\"");
    print_hex(stream, snapshot.proof);
    stream->printf("\",\"confirmationCode\":\"%06u\"",
                   static_cast<unsigned>(snapshot.confirmation_code));
  } else {
    stream->print(",\"localProof\":null,\"confirmationCode\":null");
  }
  if (snapshot.has_invitation) {
    stream->printf(
        ",\"invitation\":{\"state\":\"%s\",\"requesterAddress\":\"%s\","
        "\"remainingMs\":%u}",
        snapshot.invitation_rejected ? "rejected" : "pending",
        snapshot.invitation_requester_address,
        static_cast<unsigned>(snapshot.invitation_remaining_ms));
  } else {
    stream->print(",\"invitation\":null");
  }
  stream->print("}");
  request->send(stream);
}

void PairingHandler::handle_post_(AsyncWebServerRequest *request) {
  if (!this->server_->platform_identity_ready() ||
      !this->server_->participant_registry_persistence_ready() ||
      !this->server_->participant_credential_persistence_ready()) {
    http_response::send_error_info(request, 503, "pairing_unavailable",
                                   "pairing_unavailable", "error", "{}");
    return;
  }
  if (!request->hasArg("action")) {
    send_invalid(request, "action_required");
    return;
  }

  const std::string action = request->arg("action");
  const bool state_only_action =
      action == "cancel" || action == "rejectInvite" ||
      action == "dismissInvite";
  if (!state_only_action &&
      !this->server_->standalone_site_reconciliation_ready_for_pairing()) {
    const char *code =
        this->server_->standalone_site_reconciliation_state() ==
                StandaloneSiteReconciliationState::PENDING
            ? "site_reconciliation_pending"
            : "site_reconciliation_failed";
    http_response::send_error_info(request, 503, code, code, "error", "{}");
    return;
  }
  uint32_t operation_id = 0;
  bool queued = false;
  if (action == "invite") {
    char requester_address[64]{};
    if (!request->hasArg("requesterAddress") ||
        !copy_identifier(request->arg("requesterAddress"), requester_address,
                         sizeof(requester_address))) {
      send_invalid(request, "invalid_requester_address");
      return;
    }
    uint32_t duration_ms = DEFAULT_INVITATION_MS;
    if (request->hasArg("durationSeconds")) {
      uint32_t seconds = 0;
      if (!parse_uint32(request->arg("durationSeconds"), &seconds) ||
          seconds < 30 || seconds > 180) {
        send_invalid(request, "invalid_invitation_duration");
        return;
      }
      duration_ms = seconds * 1000U;
    }
    if (!this->server_->participant_pairing_runtime().set_invitation(
            requester_address, millis(), duration_ms)) {
      http_response::send_error_info(request, 409, "pairing_invitation_busy",
                                     "pairing_invitation_busy", "error", "{}");
      return;
    }
    request->send(200, "application/json",
                  "{\"ok\":true,\"invitationAccepted\":true}");
    return;
  } else if (action == "rejectInvite") {
    if (!this->server_->participant_pairing_runtime().reject_invitation(
            millis())) {
      http_response::send_error_info(request, 409, "pairing_invitation_expired",
                                     "pairing_invitation_expired", "error",
                                     "{}");
      return;
    }
    request->send(200, "application/json",
                  "{\"ok\":true,\"invitationRejected\":true}");
    return;
  } else if (action == "dismissInvite") {
    this->server_->participant_pairing_runtime().clear_invitation();
    request->send(200, "application/json",
                  "{\"ok\":true,\"invitationDismissed\":true}");
    return;
  } else if (action == "prepare") {
    if (!request->hasArg("role") ||
        !this->server_->participant_enrollment().window_open(millis())) {
      http_response::send_error_info(request, 409, "pairing_window_closed",
                                     "pairing_window_closed", "error", "{}");
      return;
    }
    const std::string role_text = request->arg("role");
    ParticipantPairingRole role{};
    if (role_text == "leader")
      role = ParticipantPairingRole::LEADER;
    else if (role_text == "joiner")
      role = ParticipantPairingRole::JOINER;
    else {
      send_invalid(request, "invalid_pairing_role");
      return;
    }
    uint32_t duration_ms = DEFAULT_SESSION_MS;
    if (request->hasArg("durationSeconds")) {
      uint32_t seconds = 0;
      if (!parse_uint32(request->arg("durationSeconds"), &seconds) ||
          seconds < 30 || seconds > 300) {
        send_invalid(request, "invalid_pairing_duration");
        return;
      }
      duration_ms = seconds * 1000U;
    }
    const uint32_t window_remaining =
        this->server_->participant_enrollment().window_remaining_ms(millis());
    if (duration_ms > window_remaining)
      duration_ms = window_remaining;
    queued = this->server_->participant_pairing_runtime().queue_prepare(
        role, duration_ms, millis(), &operation_id);
  } else if (action == "exchange") {
    const char *required[] = {"role", "participantId", "siteId", "sessionId",
                              "nonce", "publicKey"};
    for (const char *name : required) {
      if (!request->hasArg(name)) {
        send_invalid(request, "pairing_offer_required");
        return;
      }
    }
    ParticipantPairingOffer offer{};
    const std::string role = request->arg("role");
    if (role == "leader")
      offer.role = ParticipantPairingRole::LEADER;
    else if (role == "joiner")
      offer.role = ParticipantPairingRole::JOINER;
    else {
      send_invalid(request, "invalid_pairing_role");
      return;
    }
    if (!copy_identifier(request->arg("participantId"), offer.participant_id,
                         sizeof(offer.participant_id)) ||
        !copy_identifier(request->arg("siteId"), offer.site_id,
                         sizeof(offer.site_id)) ||
        !parse_hex(request->arg("sessionId"), &offer.session_id) ||
        !parse_hex(request->arg("nonce"), &offer.nonce) ||
        !parse_hex(request->arg("publicKey"), &offer.public_key)) {
      send_invalid(request, "invalid_pairing_offer");
      return;
    }
    queued = this->server_->participant_pairing_runtime().queue_exchange(
        offer, &operation_id);
  } else if (action == "verify") {
    std::array<uint8_t, ParticipantPairingOffer::PROOF_SIZE> proof{};
    if (!request->hasArg("proof") ||
        !parse_hex(request->arg("proof"), &proof)) {
      send_invalid(request, "invalid_pairing_proof");
      return;
    }
    queued = this->server_->participant_pairing_runtime().queue_verify(
        proof, &operation_id);
  } else if (action == "confirm") {
    uint32_t code = 0;
    if (!request->hasArg("confirmationCode") ||
        request->arg("confirmationCode").size() != 6 ||
        !parse_uint32(request->arg("confirmationCode"), &code)) {
      send_invalid(request, "invalid_confirmation_code");
      return;
    }
    queued = this->server_->participant_pairing_runtime().queue_confirm(
        code, &operation_id);
  } else if (action == "cancel") {
    queued = this->server_->participant_pairing_runtime().queue_cancel(
        &operation_id);
  } else {
    send_invalid(request, "invalid_action");
    return;
  }

  if (!queued) {
    http_response::send_error_info(request, 409, "pairing_operation_busy",
                                   "pairing_operation_busy", "error", "{}");
    return;
  }
  send_accepted(request, operation_id);
}

}  // namespace radar_api_server
}  // namespace esphome
