#include "group_coordinator.h"
#include "participant_readiness.h"
#include "participant_registry.h"
#include "site_configuration.h"
#include "site_coordinator.h"
#include "spatial_transform.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <winsock2.h>
#include <mstcpip.h>
#include <ws2tcpip.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#ifdef STRICT
#undef STRICT
#endif
using SocketHandle = SOCKET;
constexpr SocketHandle INVALID_SOCKET_HANDLE = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketHandle = int;
constexpr SocketHandle INVALID_SOCKET_HANDLE = -1;
#endif

using namespace esphome::radar_api_server;

namespace {

constexpr uint32_t WIRE_MAGIC = 0x4E414D4FU;
constexpr uint16_t WIRE_VERSION = 1;
constexpr const char *FLOORPLAN_ID = "floor-virtual";
constexpr const char *SPACE_ID = "space-virtual";
constexpr const char *GROUP_ID = "group-virtual";
constexpr uint32_t DIRECTORY_REVISION = 1;
constexpr uint32_t DIRECTORY_DIGEST = 0x4E414D4FU;
constexpr uint32_t DEFAULT_TIME_SCALE = 5;
constexpr uint32_t HEARTBEAT_WALL_INTERVAL_MS = 100;
constexpr uint32_t CONFIG_WALL_INTERVAL_MS = 500;

enum class WireKind : uint16_t {
  HEARTBEAT = 1,
  CONFIGURATION = 2,
};

#pragma pack(push, 1)
struct WireHeader {
  uint32_t magic{WIRE_MAGIC};
  uint16_t version{WIRE_VERSION};
  uint16_t kind{0};
  uint32_t payload_size{0};
};

struct WireHeartbeat {
  char participant_id[ParticipantRecord::MAX_ID_LENGTH + 1]{};
  char boot_id[GroupHeartbeat::MAX_BOOT_ID_LENGTH + 1]{};
  uint32_t site_sequence{0};
  uint32_t site_term{0};
  uint32_t site_election_view_digest{0};
  uint32_t group_sequence{0};
  uint32_t group_term{0};
  uint32_t group_election_view_digest{0};
  uint32_t configuration_generation{0};
  uint64_t configuration_digest{0};
  uint8_t group_present{0};
  uint8_t credentials_complete{1};
};

struct WireConfigurationPrefix {
  char participant_id[ParticipantRecord::MAX_ID_LENGTH + 1]{};
  uint32_t encoded_size{0};
};
#pragma pack(pop)

struct Options {
  std::string node_id;
  std::string site_id;
  std::string owner_id;
  std::vector<std::string> members;
  std::filesystem::path state_dir;
  uint16_t udp_base{47000};
  uint16_t http_base{48000};
  uint32_t time_scale{DEFAULT_TIME_SCALE};
};

void fail(const std::string &message) {
  std::fprintf(stderr, "virtual_node: %s\n", message.c_str());
  std::exit(2);
}

void copy_text(char *target, std::size_t capacity, const char *source) {
  std::strncpy(target, source, capacity - 1);
  target[capacity - 1] = '\0';
}

std::vector<std::string> split(const std::string &value, char delimiter) {
  std::vector<std::string> result;
  std::stringstream stream(value);
  std::string item;
  while (std::getline(stream, item, delimiter)) {
    if (!item.empty())
      result.push_back(item);
  }
  return result;
}

std::string json_escape(const std::string &value) {
  std::string output;
  output.reserve(value.size() + 8);
  for (const char character : value) {
    switch (character) {
      case '\\': output += "\\\\"; break;
      case '"': output += "\\\""; break;
      case '\n': output += "\\n"; break;
      case '\r': output += "\\r"; break;
      case '\t': output += "\\t"; break;
      default: output += character; break;
    }
  }
  return output;
}

bool set_nonblocking(SocketHandle socket) {
#ifdef _WIN32
  u_long enabled = 1;
  return ioctlsocket(socket, FIONBIO, &enabled) == 0;
#else
  const int flags = fcntl(socket, F_GETFL, 0);
  return flags >= 0 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

bool set_blocking(SocketHandle socket) {
#ifdef _WIN32
  u_long enabled = 0;
  return ioctlsocket(socket, FIONBIO, &enabled) == 0;
#else
  const int flags = fcntl(socket, F_GETFL, 0);
  return flags >= 0 && fcntl(socket, F_SETFL, flags & ~O_NONBLOCK) == 0;
#endif
}

void close_socket(SocketHandle socket) {
  if (socket == INVALID_SOCKET_HANDLE)
    return;
#ifdef _WIN32
  closesocket(socket);
#else
  close(socket);
#endif
}

bool would_block() {
#ifdef _WIN32
  const int error = WSAGetLastError();
  return error == WSAEWOULDBLOCK;
#else
  return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

Options parse_options(int argc, char **argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (index + 1 >= argc)
      fail("missing value for " + argument);
    const std::string value = argv[++index];
    if (argument == "--node-id")
      options.node_id = value;
    else if (argument == "--site-id")
      options.site_id = value;
    else if (argument == "--owner-id")
      options.owner_id = value;
    else if (argument == "--members")
      options.members = split(value, ',');
    else if (argument == "--state-dir")
      options.state_dir = value;
    else if (argument == "--udp-base")
      options.udp_base = static_cast<uint16_t>(std::stoul(value));
    else if (argument == "--http-base")
      options.http_base = static_cast<uint16_t>(std::stoul(value));
    else if (argument == "--time-scale")
      options.time_scale = std::stoul(value);
    else
      fail("unknown argument " + argument);
  }
  if (options.node_id.empty() || options.site_id.empty() ||
      options.owner_id.empty() || options.members.size() < 2 ||
      options.members.size() > MAX_AUTOMATIC_SPATIAL_GROUP_PARTICIPANTS ||
      options.state_dir.empty() || options.time_scale == 0)
    fail("invalid node options");
  if (std::find(options.members.begin(), options.members.end(),
                options.node_id) == options.members.end() ||
      std::find(options.members.begin(), options.members.end(),
                options.owner_id) == options.members.end())
    fail("node and owner must be present in members");
  return options;
}

class FileSiteConfigurationBackend : public SiteConfigurationStorageBackend {
 public:
  explicit FileSiteConfigurationBackend(std::filesystem::path directory)
      : directory_(std::move(directory)) {
    std::filesystem::create_directories(this->directory_);
  }

  bool load_site_configuration_slot(
      uint8_t slot, SiteConfigurationStorageBlob *blob) override {
    return slot < 2 && this->read_(this->slot_path_(slot), blob->bytes.data(),
                                  blob->bytes.size());
  }

  bool save_site_configuration_slot(
      uint8_t slot, const SiteConfigurationStorageBlob &blob) override {
    return slot < 2 && this->write_(this->slot_path_(slot), blob.bytes.data(),
                                   blob.bytes.size());
  }

  bool load_site_configuration_commit(
      SiteConfigurationCommitBlob *blob) override {
    return this->read_(this->directory_ / "site.commit", blob->bytes.data(),
                       blob->bytes.size());
  }

  bool save_site_configuration_commit(
      const SiteConfigurationCommitBlob &blob) override {
    return this->write_(this->directory_ / "site.commit", blob.bytes.data(),
                        blob.bytes.size());
  }

  bool sync() override { return true; }

 private:
  std::filesystem::path directory_;

  std::filesystem::path slot_path_(uint8_t slot) const {
    return this->directory_ /
           (slot == 0 ? "site.slot0" : "site.slot1");
  }

  static bool read_(const std::filesystem::path &path, uint8_t *data,
                    std::size_t size) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
      return false;
    input.read(reinterpret_cast<char *>(data),
               static_cast<std::streamsize>(size));
    return input.good() ||
           static_cast<std::size_t>(input.gcount()) == size;
  }

  static bool write_(const std::filesystem::path &path, const uint8_t *data,
                     std::size_t size) {
    const auto temporary = path.string() + ".tmp";
    {
      std::ofstream output(temporary,
                           std::ios::binary | std::ios::trunc);
      if (!output)
        return false;
      output.write(reinterpret_cast<const char *>(data),
                   static_cast<std::streamsize>(size));
      output.flush();
      if (!output)
        return false;
    }
    std::error_code error;
    std::filesystem::remove(path, error);
    error.clear();
    std::filesystem::rename(temporary, path, error);
    return !error;
  }
};

PlatformIdentity identity(const std::string &site_id,
                          const std::string &node_id) {
  PlatformIdentity value{};
  copy_text(value.site_id, sizeof(value.site_id), site_id.c_str());
  copy_text(value.node_id, sizeof(value.node_id), node_id.c_str());
  return value;
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

bool add_active_peer(ParticipantRegistry *registry,
                     ParticipantReadinessRegistry *readiness,
                     const std::string &participant_id) {
  return registry->discover(participant_id.c_str(),
                            ParticipantType::NAMO_NODE) ==
             ParticipantRegistryResult::OK &&
         registry->classify(participant_id.c_str(),
                            ParticipantTrust::OFFICIAL_VERIFIED) ==
             ParticipantRegistryResult::OK &&
         registry->activate(participant_id.c_str()) ==
             ParticipantRegistryResult::OK &&
         (readiness == nullptr ||
          readiness->configure(participant_id.c_str(), readiness_config()) ==
              ParticipantReadinessResult::OK);
}

SiteConfigurationSnapshot build_initial_configuration(
    const Options &options) {
  const auto owner = identity(options.site_id, options.owner_id);
  SiteConfigurationSnapshot initial{};
  if (initialize_site_configuration(owner, false, &initial) !=
      SiteConfigurationResult::OK)
    fail("failed to initialize Site Configuration");
  ParticipantRegistry roster{};
  for (const auto &member : options.members) {
    if (member != options.owner_id &&
        !add_active_peer(&roster, nullptr, member))
      fail("failed to build canonical member " + member);
  }
  SiteConfigurationSnapshot configuration{};
  if (commit_local_site_roster(owner, roster, initial, &configuration) !=
      SiteConfigurationResult::OK)
    fail("failed to commit canonical roster");
  configuration.floorplan.committed = true;
  copy_text(configuration.floorplan.floorplan_id,
            sizeof(configuration.floorplan.floorplan_id), FLOORPLAN_ID);
  configuration.floorplan.geometry_version = 1;
  configuration.floorplan.config_hash = 11;
  configuration.floorplan.image_hash = 22;
  for (auto &member : configuration.members) {
    if (!member.occupied)
      continue;
    member.spatial_profile.committed = true;
    member.spatial_profile.pose_version = 1;
    member.spatial_profile.calibration_version = 1;
    copy_text(member.spatial_profile.space_id,
              sizeof(member.spatial_profile.space_id), SPACE_ID);
    copy_text(member.spatial_profile.group_id,
              sizeof(member.spatial_profile.group_id), GROUP_ID);
    member.spatial_profile.transform = valid_transform();
  }
  return configuration;
}

uint32_t load_and_increment_boot_generation(
    const std::filesystem::path &directory) {
  std::filesystem::create_directories(directory);
  const auto path = directory / "boot-generation.txt";
  uint32_t generation = 0;
  {
    std::ifstream input(path);
    input >> generation;
  }
  ++generation;
  std::ofstream output(path, std::ios::trunc);
  output << generation << '\n';
  return generation;
}

class VirtualNode {
 public:
  explicit VirtualNode(Options options)
      : options_(std::move(options)),
        identity_(identity(this->options_.site_id, this->options_.node_id)),
        backend_(this->options_.state_dir) {
    this->index_ = this->member_index_(this->options_.node_id);
    for (const auto &member : this->options_.members) {
      if (member != this->options_.node_id &&
          !add_active_peer(&this->participants_, &this->readiness_, member))
        fail("failed to configure runtime peer " + member);
    }
    this->boot_generation_ =
        load_and_increment_boot_generation(this->options_.state_dir);
    std::snprintf(this->boot_id_, sizeof(this->boot_id_), "boot-%u-%u",
                  static_cast<unsigned>(this->index_),
                  static_cast<unsigned>(this->boot_generation_));
    const auto load_result = this->store_.load(this->backend_,
                                                &this->configuration_);
    if (load_result == SiteConfigurationStoreResult::EMPTY) {
      const auto initial = build_initial_configuration(this->options_);
      if (this->store_.create(this->backend_, initial,
                              &this->configuration_) !=
          SiteConfigurationStoreResult::CREATED)
        fail("failed to create Site Configuration storage");
    } else if (load_result != SiteConfigurationStoreResult::LOADED &&
               load_result !=
                   SiteConfigurationStoreResult::COMMIT_REPAIRED) {
      fail(std::string("failed to load Site Configuration: ") +
           site_configuration_store_result_to_string(load_result));
    }
    if (std::strcmp(this->configuration_.site_id,
                    this->identity_.site_id) != 0 ||
        this->configuration_.member_count() != this->options_.members.size())
      fail("persisted Site Configuration does not match cluster manifest");
    this->open_sockets_();
  }

  ~VirtualNode() {
    close_socket(this->udp_socket_);
    close_socket(this->http_socket_);
#ifdef _WIN32
    WSACleanup();
#endif
  }

  int run() {
    this->started_at_ = std::chrono::steady_clock::now();
    auto next_heartbeat = this->started_at_;
    auto next_configuration = this->started_at_;
    while (!this->stop_) {
      const auto now = std::chrono::steady_clock::now();
      const uint32_t now_ms = this->virtual_now_ms_(now);
      this->receive_udp_(now_ms);
      this->site_.set_local_directory_fence(DIRECTORY_REVISION,
                                            DIRECTORY_DIGEST);
      this->site_.update(this->identity_, this->participants_,
                         &this->configuration_, now_ms);
      this->group_.update(this->identity_, this->participants_,
                          this->readiness_, &this->configuration_,
                          this->site_.state().leader_node_id, now_ms);
      this->update_owner_handover_(now_ms);
      if (now >= next_heartbeat) {
        this->send_heartbeat_();
        next_heartbeat = now +
                         std::chrono::milliseconds(
                             HEARTBEAT_WALL_INTERVAL_MS);
      }
      if (now >= next_configuration) {
        if (std::strcmp(this->configuration_.configuration_owner_node_id,
                        this->identity_.node_id) == 0)
          this->send_configuration_();
        next_configuration = now +
                             std::chrono::milliseconds(
                                 CONFIG_WALL_INTERVAL_MS);
      }
      this->serve_http_();
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return 0;
  }

 private:
  Options options_;
  PlatformIdentity identity_{};
  ParticipantRegistry participants_{};
  ParticipantReadinessRegistry readiness_{};
  SiteConfigurationSnapshot configuration_{};
  SiteConfigurationStore store_{};
  FileSiteConfigurationBackend backend_;
  SiteCoordinator site_{};
  GroupCoordinator group_{};
  AutomaticConfigurationHandoverTracker handover_{};
  SocketHandle udp_socket_{INVALID_SOCKET_HANDLE};
  SocketHandle http_socket_{INVALID_SOCKET_HANDLE};
  std::chrono::steady_clock::time_point started_at_{};
  std::array<uint32_t, MAX_AUTOMATIC_SPATIAL_GROUP_PARTICIPANTS>
      last_seen_ms_{};
  std::array<bool, MAX_AUTOMATIC_SPATIAL_GROUP_PARTICIPANTS> blocked_{};
  std::array<uint8_t, 8192> packet_{};
  std::size_t index_{0};
  uint32_t boot_generation_{0};
  char boot_id_[GroupHeartbeat::MAX_BOOT_ID_LENGTH + 1]{};
  uint32_t site_sequence_{0};
  uint32_t group_sequence_{0};
  uint32_t received_packets_{0};
  uint32_t dropped_packets_{0};
  uint32_t invalid_packets_{0};
  uint32_t drop_every_{0};
  std::array<uint32_t, GroupRosterSnapshot::MAX_MEMBERS>
      received_from_peer_{};
  bool stop_{false};

  std::size_t member_index_(const std::string &participant_id) const {
    const auto position =
        std::find(this->options_.members.begin(),
                  this->options_.members.end(), participant_id);
    if (position == this->options_.members.end())
      fail("participant is not in cluster manifest");
    return static_cast<std::size_t>(position - this->options_.members.begin());
  }

  uint16_t udp_port_(std::size_t index) const {
    return static_cast<uint16_t>(this->options_.udp_base + index);
  }

  uint16_t http_port_(std::size_t index) const {
    return static_cast<uint16_t>(this->options_.http_base + index);
  }

  uint32_t virtual_now_ms_(
      std::chrono::steady_clock::time_point now) const {
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             now - this->started_at_)
                             .count();
    return static_cast<uint32_t>(elapsed * this->options_.time_scale + 1);
  }

  void open_sockets_() {
#ifdef _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
      fail("WSAStartup failed");
#endif
    this->udp_socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    this->http_socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (this->udp_socket_ == INVALID_SOCKET_HANDLE ||
        this->http_socket_ == INVALID_SOCKET_HANDLE)
      fail("socket creation failed");
#ifdef _WIN32
    BOOL report_udp_reset = FALSE;
    DWORD ioctl_bytes = 0;
    WSAIoctl(this->udp_socket_, SIO_UDP_CONNRESET, &report_udp_reset,
             sizeof(report_udp_reset), nullptr, 0, &ioctl_bytes, nullptr,
             nullptr);
#endif
    int reuse = 1;
    setsockopt(this->http_socket_, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char *>(&reuse), sizeof(reuse));
    sockaddr_in udp_address{};
    udp_address.sin_family = AF_INET;
    udp_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    udp_address.sin_port = htons(this->udp_port_(this->index_));
    sockaddr_in http_address = udp_address;
    http_address.sin_port = htons(this->http_port_(this->index_));
    if (bind(this->udp_socket_,
             reinterpret_cast<const sockaddr *>(&udp_address),
             sizeof(udp_address)) != 0 ||
        bind(this->http_socket_,
             reinterpret_cast<const sockaddr *>(&http_address),
             sizeof(http_address)) != 0 ||
        listen(this->http_socket_, 8) != 0 ||
        !set_nonblocking(this->udp_socket_) ||
        !set_nonblocking(this->http_socket_))
      fail("socket bind/listen failed");
  }

  void send_packet_(const uint8_t *data, std::size_t size) {
    for (std::size_t peer = 0; peer < this->options_.members.size(); ++peer) {
      if (peer == this->index_)
        continue;
      sockaddr_in address{};
      address.sin_family = AF_INET;
      address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      address.sin_port = htons(this->udp_port_(peer));
      sendto(this->udp_socket_, reinterpret_cast<const char *>(data),
             static_cast<int>(size), 0,
             reinterpret_cast<const sockaddr *>(&address), sizeof(address));
    }
  }

  void send_heartbeat_() {
    WireHeader header{};
    header.kind = static_cast<uint16_t>(WireKind::HEARTBEAT);
    header.payload_size = sizeof(WireHeartbeat);
    WireHeartbeat heartbeat{};
    copy_text(heartbeat.participant_id, sizeof(heartbeat.participant_id),
              this->identity_.node_id);
    copy_text(heartbeat.boot_id, sizeof(heartbeat.boot_id), this->boot_id_);
    heartbeat.site_sequence = ++this->site_sequence_;
    heartbeat.site_term = std::max<uint32_t>(1, this->site_.state().term);
    heartbeat.site_election_view_digest =
        this->site_.state().election_view_digest;
    const auto *group =
        this->group_.local_group(&this->configuration_,
                                 this->identity_.node_id);
    if (group != nullptr) {
      heartbeat.group_present = 1;
      heartbeat.group_sequence = ++this->group_sequence_;
      heartbeat.group_term = std::max<uint32_t>(1, group->term);
      heartbeat.group_election_view_digest = group->election_view_digest;
    }
    heartbeat.configuration_generation = this->configuration_.generation;
    heartbeat.configuration_digest =
        site_configuration_digest(this->configuration_);
    std::memcpy(this->packet_.data(), &header, sizeof(header));
    std::memcpy(this->packet_.data() + sizeof(header), &heartbeat,
                sizeof(heartbeat));
    this->send_packet_(this->packet_.data(),
                       sizeof(header) + sizeof(heartbeat));
  }

  void send_configuration_() {
    std::array<uint8_t, SiteConfigurationCodec::MAX_ENCODED_SIZE> encoded{};
    std::size_t encoded_size = 0;
    if (SiteConfigurationCodec::encode(
            this->configuration_, encoded.data(), encoded.size(),
            &encoded_size) != SiteConfigurationCodecResult::OK)
      return;
    WireHeader header{};
    header.kind = static_cast<uint16_t>(WireKind::CONFIGURATION);
    header.payload_size = static_cast<uint32_t>(
        sizeof(WireConfigurationPrefix) + encoded_size);
    WireConfigurationPrefix prefix{};
    copy_text(prefix.participant_id, sizeof(prefix.participant_id),
              this->identity_.node_id);
    prefix.encoded_size = static_cast<uint32_t>(encoded_size);
    const std::size_t packet_size =
        sizeof(header) + sizeof(prefix) + encoded_size;
    if (packet_size > this->packet_.size())
      return;
    std::memcpy(this->packet_.data(), &header, sizeof(header));
    std::memcpy(this->packet_.data() + sizeof(header), &prefix,
                sizeof(prefix));
    std::memcpy(this->packet_.data() + sizeof(header) + sizeof(prefix),
                encoded.data(), encoded_size);
    this->send_packet_(this->packet_.data(), packet_size);
  }

  void receive_udp_(uint32_t now_ms) {
    for (;;) {
      sockaddr_in source{};
#ifdef _WIN32
      int source_size = sizeof(source);
#else
      socklen_t source_size = sizeof(source);
#endif
      const int received = recvfrom(
          this->udp_socket_, reinterpret_cast<char *>(this->packet_.data()),
          static_cast<int>(this->packet_.size()), 0,
          reinterpret_cast<sockaddr *>(&source), &source_size);
      if (received < 0) {
        if (!would_block())
          ++this->invalid_packets_;
        break;
      }
      if (static_cast<std::size_t>(received) < sizeof(WireHeader)) {
        ++this->invalid_packets_;
        continue;
      }
      WireHeader header{};
      std::memcpy(&header, this->packet_.data(), sizeof(header));
      if (header.magic != WIRE_MAGIC || header.version != WIRE_VERSION ||
          header.payload_size !=
              static_cast<uint32_t>(received - sizeof(header))) {
        ++this->invalid_packets_;
        continue;
      }
      ++this->received_packets_;
      if (static_cast<WireKind>(header.kind) == WireKind::HEARTBEAT)
        this->accept_heartbeat_(this->packet_.data() + sizeof(header),
                                header.payload_size, now_ms);
      else if (static_cast<WireKind>(header.kind) ==
               WireKind::CONFIGURATION)
        this->accept_configuration_(this->packet_.data() + sizeof(header),
                                    header.payload_size);
      else
        ++this->invalid_packets_;
    }
  }

  bool fault_drops_(std::size_t peer) {
    if (peer >= this->options_.members.size() || this->blocked_[peer])
      return true;
    const uint32_t peer_sequence = ++this->received_from_peer_[peer];
    return this->drop_every_ != 0 && peer_sequence % this->drop_every_ == 0;
  }

  void accept_heartbeat_(const uint8_t *data, std::size_t size,
                         uint32_t now_ms) {
    if (size != sizeof(WireHeartbeat)) {
      ++this->invalid_packets_;
      return;
    }
    WireHeartbeat wire{};
    std::memcpy(&wire, data, sizeof(wire));
    const auto position =
        std::find(this->options_.members.begin(), this->options_.members.end(),
                  std::string(wire.participant_id));
    if (position == this->options_.members.end()) {
      ++this->invalid_packets_;
      return;
    }
    const std::size_t peer =
        static_cast<std::size_t>(position - this->options_.members.begin());
    if (this->fault_drops_(peer)) {
      ++this->dropped_packets_;
      return;
    }
    this->last_seen_ms_[peer] = now_ms;
    SiteLeaderHeartbeat site{};
    site.participant_id = wire.participant_id;
    site.boot_id = wire.boot_id;
    site.sequence = wire.site_sequence;
    site.term = wire.site_term;
    site.lease_ms = SiteCoordinator::DEFAULT_LEASE_MS;
    site.directory_revision = DIRECTORY_REVISION;
    site.directory_digest = DIRECTORY_DIGEST;
    site.election_view_digest = wire.site_election_view_digest;
    site.credentials_complete = wire.credentials_complete != 0;
    this->site_.accept_heartbeat(this->participants_, &this->configuration_,
                                 site, now_ms);
    if (wire.group_present != 0) {
      GroupHeartbeat group{};
      group.participant_id = wire.participant_id;
      group.group_id = GROUP_ID;
      group.boot_id = wire.boot_id;
      group.sequence = wire.group_sequence;
      group.term = wire.group_term;
      group.lease_ms = GroupCoordinator::DEFAULT_LEASE_MS;
      group.election_view_digest = wire.group_election_view_digest;
      group.credentials_complete = wire.credentials_complete != 0;
      this->group_.accept_heartbeat(
          this->participants_, this->readiness_, &this->configuration_, group,
          now_ms);
    }
  }

  void accept_configuration_(const uint8_t *data, std::size_t size) {
    if (size < sizeof(WireConfigurationPrefix)) {
      ++this->invalid_packets_;
      return;
    }
    WireConfigurationPrefix prefix{};
    std::memcpy(&prefix, data, sizeof(prefix));
    if (prefix.encoded_size != size - sizeof(prefix)) {
      ++this->invalid_packets_;
      return;
    }
    const std::size_t peer = this->member_index_(prefix.participant_id);
    if (this->fault_drops_(peer)) {
      ++this->dropped_packets_;
      return;
    }
    SiteConfigurationSnapshot remote{};
    if (SiteConfigurationCodec::decode(data + sizeof(prefix),
                                       prefix.encoded_size, &remote) !=
        SiteConfigurationCodecResult::OK) {
      ++this->invalid_packets_;
      return;
    }
    if (site_configuration_digest(remote) ==
        site_configuration_digest(this->configuration_))
      return;
    const bool owner_changed =
        std::strcmp(remote.configuration_owner_node_id,
                    this->configuration_.configuration_owner_node_id) != 0;
    const auto validation = validate_replicated_site_configuration(
        this->identity_, this->configuration_, remote,
        owner_changed
            ? SiteConfigurationReplicationMode::AUTOMATIC_OWNER_HANDOVER
            : SiteConfigurationReplicationMode::STRICT,
        prefix.participant_id);
    if (validation != SiteConfigurationReplicationResult::OK)
      return;
    if (this->store_.replace_with_newer(this->backend_, remote,
                                         &this->configuration_) ==
        SiteConfigurationStoreResult::UPDATED)
      this->handover_.reset();
  }

  bool owner_online_(uint32_t now_ms) const {
    if (std::strcmp(this->configuration_.configuration_owner_node_id,
                    this->identity_.node_id) == 0)
      return true;
    const std::size_t owner = this->member_index_(
        this->configuration_.configuration_owner_node_id);
    return this->last_seen_ms_[owner] != 0 &&
           static_cast<uint32_t>(now_ms - this->last_seen_ms_[owner]) <
               SiteCoordinator::DEFAULT_LEASE_MS;
  }

  const char *designated_successor_(uint32_t now_ms) const {
    const SiteConfigurationMember *selected = nullptr;
    uint8_t selected_penalty = 0;
    const auto *group =
        this->group_.local_group(&this->configuration_,
                                 this->identity_.node_id);
    for (const auto &member : this->configuration_.members) {
      if (!member.occupied ||
          member.membership != ParticipantMembership::ACTIVE ||
          member.type != ParticipantType::NAMO_NODE ||
          !member.leader_eligible ||
          std::strcmp(member.participant_id,
                      this->configuration_.configuration_owner_node_id) == 0)
        continue;
      const std::size_t index = this->member_index_(member.participant_id);
      const bool online =
          index == this->index_ ||
          (this->last_seen_ms_[index] != 0 &&
           static_cast<uint32_t>(now_ms - this->last_seen_ms_[index]) <
               SiteCoordinator::DEFAULT_LEASE_MS);
      if (!online)
        continue;
      uint8_t penalty =
          std::strcmp(member.participant_id,
                      this->site_.state().leader_node_id) == 0
              ? 2U
              : 0U;
      penalty += group != nullptr &&
                         std::strcmp(member.participant_id,
                                     group->leader_node_id) == 0
                     ? 1U
                     : 0U;
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

  void update_owner_handover_(uint32_t now_ms) {
    const char *successor_id = this->designated_successor_(now_ms);
    const auto condition = evaluate_automatic_configuration_handover(
        this->identity_, this->configuration_, this->owner_online_(now_ms),
        this->site_.state().quorum_available, true, true, successor_id);
    const uint64_t authority_fingerprint =
        automatic_configuration_handover_fingerprint(this->configuration_,
                                                     successor_id);
    if (!this->handover_.update(condition, authority_fingerprint, now_ms))
      return;
    SiteConfigurationSnapshot updated{};
    if (prepare_automatic_configuration_handover(
            this->identity_, this->configuration_, &updated, successor_id) !=
        SiteConfigurationResult::OK)
      return;
    if (this->store_.update(this->backend_, updated,
                            &this->configuration_) ==
        SiteConfigurationStoreResult::UPDATED) {
      this->handover_.reset();
      this->send_configuration_();
    }
  }

  std::string state_json_() const {
    const auto &site = this->site_.state();
    const auto *group =
        this->group_.local_group(&this->configuration_,
                                 this->identity_.node_id);
    std::ostringstream output;
    output << "{\"nodeId\":\"" << json_escape(this->identity_.node_id)
           << "\",\"bootId\":\"" << json_escape(this->boot_id_)
           << "\",\"ownerNodeId\":\""
           << json_escape(this->configuration_.configuration_owner_node_id)
           << "\",\"localOwner\":"
           << (std::strcmp(this->configuration_.configuration_owner_node_id,
                           this->identity_.node_id) == 0
                   ? "true"
                   : "false")
           << ",\"configurationGeneration\":"
           << this->configuration_.generation
           << ",\"configurationDigest\":\""
           << std::hex << site_configuration_digest(this->configuration_)
           << std::dec << "\",\"siteLeaderNodeId\":\""
           << json_escape(site.leader_node_id)
           << "\",\"siteTerm\":" << site.term
           << ",\"siteLocalLeader\":"
           << (site.local_leader ? "true" : "false")
           << ",\"siteQuorum\":"
           << (site.quorum_available ? "true" : "false")
           << ",\"siteExpected\":"
           << static_cast<unsigned>(site.participants_expected)
           << ",\"siteOnline\":"
           << static_cast<unsigned>(site.participants_online)
           << ",\"groupLeaderNodeId\":\""
           << json_escape(group != nullptr ? group->leader_node_id : "")
           << "\",\"groupTerm\":" << (group != nullptr ? group->term : 0)
           << ",\"groupLocalLeader\":"
           << (group != nullptr && group->local_leader ? "true" : "false")
           << ",\"groupQuorum\":"
           << (group != nullptr && group->quorum_available ? "true" : "false")
           << ",\"groupExpected\":"
           << static_cast<unsigned>(group != nullptr
                                        ? group->participants_expected
                                        : 0)
           << ",\"groupOnline\":"
           << static_cast<unsigned>(group != nullptr
                                        ? group->participants_online
                                        : 0)
           << ",\"receivedPackets\":" << this->received_packets_
           << ",\"droppedPackets\":" << this->dropped_packets_
           << ",\"invalidPackets\":" << this->invalid_packets_ << "}";
    return output.str();
  }

  static std::string query_value(const std::string &path,
                                 const std::string &key) {
    const auto query = path.find('?');
    if (query == std::string::npos)
      return "";
    for (const auto &part : split(path.substr(query + 1), '&')) {
      const auto equal = part.find('=');
      if (equal != std::string::npos && part.substr(0, equal) == key)
        return part.substr(equal + 1);
    }
    return "";
  }

  void serve_http_() {
    for (;;) {
      sockaddr_in source{};
#ifdef _WIN32
      int source_size = sizeof(source);
#else
      socklen_t source_size = sizeof(source);
#endif
      SocketHandle client = accept(
          this->http_socket_, reinterpret_cast<sockaddr *>(&source),
          &source_size);
      if (client == INVALID_SOCKET_HANDLE) {
        if (!would_block())
          ++this->invalid_packets_;
        break;
      }
      if (!set_blocking(client)) {
        close_socket(client);
        ++this->invalid_packets_;
        continue;
      }
      std::array<char, 4096> request{};
      std::size_t request_size = 0;
      while (request_size < request.size() - 1) {
        const int length = recv(
            client, request.data() + request_size,
            static_cast<int>(request.size() - 1 - request_size), 0);
        if (length <= 0)
          break;
        request_size += static_cast<std::size_t>(length);
        request[request_size] = '\0';
        if (std::strstr(request.data(), "\r\n\r\n") != nullptr)
          break;
      }
      std::string method;
      std::string path;
      if (request_size > 0) {
        std::istringstream line(std::string(request.data(), request_size));
        line >> method >> path;
      }
      int status = 200;
      std::string body;
      if (method == "GET" && path.rfind("/state", 0) == 0) {
        body = this->state_json_();
      } else if (method == "POST" &&
                 path.rfind("/control/block", 0) == 0) {
        const auto peer = query_value(path, "peer");
        const auto blocked = query_value(path, "blocked");
        const auto position = std::find(this->options_.members.begin(),
                                        this->options_.members.end(), peer);
        if (position == this->options_.members.end()) {
          status = 400;
          body = "{\"error\":\"unknown_peer\"}";
        } else {
          this->blocked_[static_cast<std::size_t>(
              position - this->options_.members.begin())] = blocked != "0";
          body = "{\"ok\":true}";
        }
      } else if (method == "POST" &&
                 path.rfind("/control/drop", 0) == 0) {
        const auto every = query_value(path, "every");
        this->drop_every_ = every.empty() ? 0 : std::stoul(every);
        body = "{\"ok\":true}";
      } else if (method == "POST" &&
                 path.rfind("/control/stop", 0) == 0) {
        this->stop_ = true;
        body = "{\"ok\":true}";
      } else {
        status = 404;
        body = "{\"error\":\"not_found\"}";
      }
      const char *reason = status == 200 ? "OK" :
                           status == 400 ? "Bad Request" : "Not Found";
      std::ostringstream response;
      response << "HTTP/1.1 " << status << ' ' << reason
               << "\r\nContent-Type: application/json\r\nContent-Length: "
               << body.size()
               << "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n"
               << body;
      const auto serialized = response.str();
      std::size_t sent = 0;
      while (sent < serialized.size()) {
        const int written = send(
            client, serialized.data() + sent,
            static_cast<int>(serialized.size() - sent), 0);
        if (written <= 0)
          break;
        sent += static_cast<std::size_t>(written);
      }
#ifdef _WIN32
      shutdown(client, SD_SEND);
#else
      shutdown(client, SHUT_WR);
#endif
      close_socket(client);
    }
  }
};

}  // namespace

int main(int argc, char **argv) {
  VirtualNode node(parse_options(argc, argv));
  return node.run();
}
