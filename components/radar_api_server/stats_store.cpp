#include "stats_store.h"

#include <algorithm>
#include <cstring>

namespace esphome {
namespace radar_api_server {

void StatsStore::load(RadarStorage *storage) {
  this->storage_ = storage;
  this->today_json_.clear();
  this->daily_.clear();
  this->today_heatmap_cells_.fill(0);
  this->heatmap_daily_.clear();

  std::string data;
  if (storage != nullptr && storage->read_payload(RadarPayloadTarget::STATS, &data)) {
    this->today_json_ = extract_object_(data, "today");
    this->daily_ = extract_object_array_(data, "daily");
    const std::string heatmap = extract_object_(data, "heatmap");
    decode_heatmap_rle_(extract_string_(heatmap, "today"), &this->today_heatmap_cells_);
    this->heatmap_daily_ = extract_object_array_(heatmap, "daily");
    this->trim_daily_();
    this->trim_heatmap_daily_();
  }
}

void StatsStore::update_today(const std::string &today_json) { this->today_json_ = today_json; }

void StatsStore::record_heatmap_hit(float tx, float ty) {
  const int cell_mm = 300;
  const int x_min = -4800;
  const int y_min = 0;
  const int cols = 33;
  const int rows = 26;
  int col = static_cast<int>((tx - x_min) / cell_mm);
  int row = static_cast<int>((ty - y_min) / cell_mm);
  if (col < 0) col = 0;
  if (col >= cols) col = cols - 1;
  if (row < 0) row = 0;
  if (row >= rows) row = rows - 1;
  const int index = row * cols + col;
  if (this->today_heatmap_cells_[index] < 65535)
    this->today_heatmap_cells_[index]++;
}

bool StatsStore::finish_day(const std::string &finished_day_json, const std::string &new_today_json) {
  if (!finished_day_json.empty()) {
    const int finished_day = parse_totals_(finished_day_json).day;
    if (finished_day > 0) {
      const std::string finished_heatmap_rle = this->compact_today_heatmap_rle_();
      this->daily_.erase(std::remove_if(this->daily_.begin(), this->daily_.end(),
                                        [&](const std::string &entry) {
                                          return parse_totals_(entry).day == finished_day;
                                        }),
                         this->daily_.end());
      this->daily_.insert(this->daily_.begin(), finished_day_json);
      this->trim_daily_();
      this->heatmap_daily_.erase(std::remove_if(this->heatmap_daily_.begin(), this->heatmap_daily_.end(),
                                                [&](const std::string &entry) {
                                                  return parse_int_after_(entry, "\"d\":") == finished_day;
                                                }),
                                 this->heatmap_daily_.end());
      this->heatmap_daily_.insert(this->heatmap_daily_.begin(),
                                  "{\"d\":" + std::to_string(finished_day) + ",\"data\":" +
                                      json_string_(finished_heatmap_rle) + "}");
      this->trim_heatmap_daily_();
    }
  }
  this->today_json_ = new_today_json;
  this->today_heatmap_cells_.fill(0);
  return this->persist_();
}

bool StatsStore::replace_json(const std::string &json) {
  this->today_json_ = extract_object_(json, "today");
  this->daily_ = extract_object_array_(json, "daily");
  const std::string heatmap = extract_object_(json, "heatmap");
  decode_heatmap_rle_(extract_string_(heatmap, "today"), &this->today_heatmap_cells_);
  this->heatmap_daily_ = extract_object_array_(heatmap, "daily");
  this->trim_daily_();
  this->trim_heatmap_daily_();
  return this->persist_();
}

std::string StatsStore::current_json() const {
  const std::string today = this->today_json_.empty()
                                ? "{\"d\":0,\"f\":0,\"r\":0,\"fz\":[0,0,0,0],\"rz\":[0,0,0,0],\"sz\":[0,0,0,0,0,0]}"
                                : this->today_json_;
  const Totals today_totals = parse_totals_(today);
  const int today_serial = day_serial_(today_totals.day);

  auto build_summary = [&](int days) -> std::string {
    Totals result;
    if (today_serial > 0 && day_serial_(today_totals.day) >= today_serial - days + 1) {
      add_totals_(&result, today_totals);
    }
    if (today_serial > 0) {
      for (const auto &entry : this->daily_) {
        const Totals daily = parse_totals_(entry);
        if (daily.day == today_totals.day)
          continue;
        const int daily_serial = day_serial_(daily.day);
        if (daily_serial >= today_serial - days + 1 && daily_serial <= today_serial) {
          add_totals_(&result, daily);
        }
      }
    }
    return totals_json_(result, days);
  };

  std::string json = "{\"version\":1,\"today\":";
  json += today;
  json += ",\"daily\":[";
  for (size_t index = 0; index < this->daily_.size(); index++) {
    if (index > 0)
      json += ",";
    json += this->daily_[index];
  }
  json += "],\"summary\":{\"last3Days\":";
  json += build_summary(3);
  json += ",\"last7Days\":";
  json += build_summary(7);
  json += ",\"last15Days\":";
  json += build_summary(15);
  json += ",\"last30Days\":";
  json += build_summary(30);
  json += "},\"heatmap\":";
  json += "{\"version\":1,\"cols\":33,\"rows\":26,\"cellMm\":300,\"encoding\":\"rle\",\"today\":";
  json += json_string_(this->compact_today_heatmap_rle_());
  json += ",\"daily\":[";
  for (size_t index = 0; index < this->heatmap_daily_.size(); index++) {
    if (index > 0)
      json += ",";
    json += this->heatmap_daily_[index];
  }
  json += "]}";
  json += "}";
  return json;
}

std::vector<std::string> StatsStore::extract_object_array_(const std::string &json, const char *key) {
  std::vector<std::string> objects;
  const std::string marker = std::string("\"") + key + "\":[";
  const size_t start = json.find(marker);
  if (start == std::string::npos)
    return objects;

  size_t pos = start + marker.size();
  int depth = 0;
  size_t object_start = std::string::npos;
  for (; pos < json.size(); pos++) {
    const char ch = json[pos];
    if (ch == '{') {
      if (depth == 0)
        object_start = pos;
      depth++;
    } else if (ch == '}') {
      if (depth > 0)
        depth--;
      if (depth == 0 && object_start != std::string::npos) {
        objects.push_back(json.substr(object_start, pos - object_start + 1));
        object_start = std::string::npos;
      }
    } else if (ch == ']' && depth == 0) {
      break;
    }
  }
  return objects;
}

std::string StatsStore::extract_object_(const std::string &json, const char *key) {
  const std::string marker = std::string("\"") + key + "\":";
  size_t pos = json.find(marker);
  if (pos == std::string::npos)
    return "";
  pos += marker.size();
  while (pos < json.size() && json[pos] != '{')
    pos++;
  if (pos >= json.size())
    return "";

  const size_t object_start = pos;
  int depth = 0;
  for (; pos < json.size(); pos++) {
    if (json[pos] == '{') {
      depth++;
    } else if (json[pos] == '}') {
      depth--;
      if (depth == 0)
        return json.substr(object_start, pos - object_start + 1);
    }
  }
  return "";
}

std::string StatsStore::extract_string_(const std::string &json, const char *key) {
  const std::string marker = std::string("\"") + key + "\":";
  size_t pos = json.find(marker);
  if (pos == std::string::npos)
    return "";
  pos += marker.size();
  while (pos < json.size() && json[pos] != '"')
    pos++;
  if (pos >= json.size())
    return "";
  pos++;

  std::string value;
  for (; pos < json.size(); pos++) {
    const char ch = json[pos];
    if (ch == '"')
      return value;
    if (ch == '\\' && pos + 1 < json.size()) {
      pos++;
      value += json[pos];
    } else {
      value += ch;
    }
  }
  return "";
}

int StatsStore::parse_int_after_(const std::string &json, const char *key) {
  const size_t key_pos = json.find(key);
  if (key_pos == std::string::npos)
    return 0;
  size_t pos = key_pos + std::strlen(key);
  bool negative = false;
  if (pos < json.size() && json[pos] == '-') {
    negative = true;
    pos++;
  }
  int value = 0;
  while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
    value = value * 10 + (json[pos] - '0');
    pos++;
  }
  return negative ? -value : value;
}

void StatsStore::parse_int_array_(const std::string &json, const char *key, int *out, size_t count) {
  const size_t key_pos = json.find(key);
  if (key_pos == std::string::npos)
    return;
  size_t pos = key_pos + std::strlen(key);
  for (size_t index = 0; index < count && pos < json.size(); index++) {
    while (pos < json.size() && (json[pos] < '0' || json[pos] > '9') && json[pos] != '-')
      pos++;
    bool negative = false;
    if (pos < json.size() && json[pos] == '-') {
      negative = true;
      pos++;
    }
    int value = 0;
    while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
      value = value * 10 + (json[pos] - '0');
      pos++;
    }
    out[index] = negative ? -value : value;
  }
}

StatsStore::Totals StatsStore::parse_totals_(const std::string &json) {
  Totals totals;
  totals.day = parse_int_after_(json, "\"d\":");
  totals.f = parse_int_after_(json, "\"f\":");
  totals.r = parse_int_after_(json, "\"r\":");
  parse_int_array_(json, "\"fz\":[", totals.fz, 4);
  parse_int_array_(json, "\"rz\":[", totals.rz, 4);
  parse_int_array_(json, "\"sz\":[", totals.sz, 6);
  return totals;
}

int StatsStore::day_serial_(int day_key) {
  if (day_key <= 0)
    return 0;
  int year = day_key / 10000;
  int month = (day_key / 100) % 100;
  int day = day_key % 100;
  if (month <= 2) {
    year--;
    month += 12;
  }
  return 365 * year + year / 4 - year / 100 + year / 400 + (153 * (month - 3) + 2) / 5 + day - 1;
}

void StatsStore::add_totals_(Totals *target, const Totals &source) {
  target->f += source.f;
  target->r += source.r;
  for (int index = 0; index < 4; index++) {
    target->fz[index] += source.fz[index];
    target->rz[index] += source.rz[index];
  }
  for (int index = 0; index < 6; index++) {
    target->sz[index] += source.sz[index];
  }
}

std::string StatsStore::totals_json_(const Totals &totals, int days) {
  std::string json = "{\"days\":";
  json += std::to_string(days);
  json += ",\"f\":";
  json += std::to_string(totals.f);
  json += ",\"r\":";
  json += std::to_string(totals.r);
  json += ",\"fz\":[";
  for (int index = 0; index < 4; index++) {
    if (index > 0)
      json += ",";
    json += std::to_string(totals.fz[index]);
  }
  json += "],\"rz\":[";
  for (int index = 0; index < 4; index++) {
    if (index > 0)
      json += ",";
    json += std::to_string(totals.rz[index]);
  }
  json += "],\"sz\":[";
  for (int index = 0; index < 6; index++) {
    if (index > 0)
      json += ",";
    json += std::to_string(totals.sz[index]);
  }
  json += "]}";
  return json;
}

std::string StatsStore::default_heatmap_rle_() { return "0x858"; }

std::string StatsStore::json_string_(const std::string &value) {
  std::string out = "\"";
  for (const char ch : value) {
    if (ch == '"' || ch == '\\') {
      out += '\\';
      out += ch;
    } else if (ch == '\n') {
      out += "\\n";
    } else if (ch == '\r') {
      out += "\\r";
    } else if (ch == '\t') {
      out += "\\t";
    } else {
      out += ch;
    }
  }
  out += "\"";
  return out;
}

void StatsStore::decode_heatmap_rle_(const std::string &rle, std::array<uint16_t, 858> *cells) {
  if (cells == nullptr)
    return;
  cells->fill(0);
  if (rle.empty())
    return;

  size_t cursor = 0;
  size_t cell_index = 0;
  while (cursor < rle.size() && cell_index < cells->size()) {
    int value = 0;
    while (cursor < rle.size() && rle[cursor] >= '0' && rle[cursor] <= '9') {
      value = value * 10 + (rle[cursor] - '0');
      cursor++;
    }
    if (cursor >= rle.size() || rle[cursor] != 'x')
      break;
    cursor++;

    int run = 0;
    while (cursor < rle.size() && rle[cursor] >= '0' && rle[cursor] <= '9') {
      run = run * 10 + (rle[cursor] - '0');
      cursor++;
    }
    if (run <= 0)
      break;

    const uint16_t clamped_value = value < 0 ? 0 : value > 65535 ? 65535 : static_cast<uint16_t>(value);
    for (int count = 0; count < run && cell_index < cells->size(); count++) {
      (*cells)[cell_index++] = clamped_value;
    }

    if (cursor < rle.size() && rle[cursor] == ',')
      cursor++;
  }
}

std::string StatsStore::compact_today_heatmap_rle_() const {
  std::string out;
  uint16_t value = this->today_heatmap_cells_[0];
  int run = 1;
  for (size_t index = 1; index < this->today_heatmap_cells_.size(); index++) {
    const uint16_t next = this->today_heatmap_cells_[index];
    if (next == value) {
      run++;
      continue;
    }
    if (!out.empty())
      out += ",";
    out += std::to_string(value);
    out += "x";
    out += std::to_string(run);
    value = next;
    run = 1;
  }
  if (!out.empty())
    out += ",";
  out += std::to_string(value);
  out += "x";
  out += std::to_string(run);
  return out;
}

void StatsStore::trim_daily_() {
  std::sort(this->daily_.begin(), this->daily_.end(), [](const std::string &left, const std::string &right) {
    return parse_totals_(left).day > parse_totals_(right).day;
  });
  if (this->daily_.size() > MAX_DAILY_ENTRIES)
    this->daily_.resize(MAX_DAILY_ENTRIES);
}

void StatsStore::trim_heatmap_daily_() {
  std::sort(this->heatmap_daily_.begin(), this->heatmap_daily_.end(), [](const std::string &left, const std::string &right) {
    return parse_int_after_(left, "\"d\":") > parse_int_after_(right, "\"d\":");
  });
  if (this->heatmap_daily_.size() > MAX_DAILY_ENTRIES)
    this->heatmap_daily_.resize(MAX_DAILY_ENTRIES);
}

bool StatsStore::persist_() const {
  if (this->storage_ == nullptr)
    return false;
  const std::string json = this->current_json();
  return this->storage_->write_payload(RadarPayloadTarget::STATS, reinterpret_cast<const uint8_t *>(json.data()),
                                       json.size());
}

}  // namespace radar_api_server
}  // namespace esphome
