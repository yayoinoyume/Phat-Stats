#include "snapshot.h"

#include <Arduino.h>
#include <ArduinoJson.h>

#include <stdio.h>
#include <string.h>

namespace {

// ArduinoJson 每帧都重建文档。用固定池避免每帧 malloc/free；池在下一帧前整体复位。
class FrameAllocator : public Allocator {
 public:
  void reset() { used_ = 0; }

  void* allocate(size_t size) override {
    if (size == 0) size = 1;
    const size_t total = align(size + sizeof(Header));
    if (used_ + total > sizeof(pool_)) return nullptr;
    Header* header = reinterpret_cast<Header*>(pool_ + used_);
    header->size = size;
    used_ += total;
    return header + 1;
  }

  void deallocate(void* /*ptr*/) override {
    // 池按帧复位，单帧内不做回收。
  }

  void* reallocate(void* ptr, size_t newSize) override {
    if (ptr == nullptr) return allocate(newSize);
    Header* header = static_cast<Header*>(ptr) - 1;
    void* next = allocate(newSize);
    if (next == nullptr) return nullptr;
    const size_t copy = header->size < newSize ? header->size : newSize;
    memcpy(next, ptr, copy);
    return next;
  }

 private:
  struct alignas(8) Header {
    size_t size;
  };

  static size_t align(size_t value) { return (value + 7) & ~static_cast<size_t>(7); }

  alignas(8) uint8_t pool_[JSON_POOL_SIZE];
  size_t used_ = 0;
};

FrameAllocator g_allocator;
Snapshot g_staging;

TitleBitmap g_titleCache[TITLE_CACHE_SLOTS];
uint32_t g_titleAge = 0;

int8_t base64Value(char c) {
  if (c >= 'A' && c <= 'Z') return static_cast<int8_t>(c - 'A');
  if (c >= 'a' && c <= 'z') return static_cast<int8_t>(c - 'a' + 26);
  if (c >= '0' && c <= '9') return static_cast<int8_t>(c - '0' + 52);
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

bool base64Decode(const char* src, uint8_t* dst, size_t dstCap, size_t& outLen) {
  outLen = 0;
  if (src == nullptr || dst == nullptr) return false;
  const size_t len = strlen(src);
  if (len == 0 || (len & 3) != 0) return false;

  uint32_t acc = 0;
  uint8_t bits = 0;
  bool padding = false;
  for (size_t i = 0; i < len; ++i) {
    const char c = src[i];
    if (c == '=') {
      padding = true;
      continue;
    }
    if (padding) return false;  // '=' 之后不能再有数据
    const int8_t value = base64Value(c);
    if (value < 0) return false;
    acc = (acc << 6) | static_cast<uint8_t>(value);
    bits = static_cast<uint8_t>(bits + 6);
    if (bits >= 8) {
      bits = static_cast<uint8_t>(bits - 8);
      if (outLen >= dstCap) return false;
      dst[outLen++] = static_cast<uint8_t>((acc >> bits) & 0xFF);
    }
  }
  return true;
}

void copyUtf8(char* dst, size_t cap, const char* src, size_t maxBytes) {
  if (dst == nullptr || cap == 0) return;
  if (src == nullptr) src = "";
  size_t limit = maxBytes;
  if (limit > cap - 1) limit = cap - 1;
  size_t len = strlen(src);
  if (len > limit) {
    len = limit;
    // 不要把 UTF-8 码点切成两半：若截断点落在续字节上，退到首字节之前。
    while (len > 0 && (static_cast<unsigned char>(src[len]) & 0xC0) == 0x80) --len;
  }
  memcpy(dst, src, len);
  dst[len] = '\0';
}

bool stateFromText(const char* text, PanelState& state) {
  if (text == nullptr) return false;
  if (strcmp(text, "working") == 0 || strcmp(text, "running") == 0) {
    state = PanelState::Working;
  } else if (strcmp(text, "needs_you") == 0 || strcmp(text, "needs") == 0) {
    state = PanelState::NeedsYou;
  } else if (strcmp(text, "done") == 0) {
    state = PanelState::Done;
  } else if (strcmp(text, "quiet") == 0) {
    state = PanelState::Quiet;
  } else if (strcmp(text, "error") == 0) {
    state = PanelState::Error;
  } else if (strcmp(text, "link_lost") == 0) {
    state = PanelState::LinkLost;
  } else {
    return false;
  }
  return true;
}

bool sessionStateFromText(const char* text, PanelState& state) {
  if (!stateFromText(text, state)) return false;
  return state == PanelState::Working || state == PanelState::NeedsYou ||
         state == PanelState::Done;
}

uint16_t clampCount(long value) {
  if (value < 0) return 0;
  if (value > 999) return 999;
  return static_cast<uint16_t>(value);
}

uint16_t countField(JsonObject counts, const char* key) {
  JsonVariant value = counts[key];
  if (!value.is<long>()) return 0;
  long parsed = value.as<long>();
  return clampCount(parsed);
}

void countSessions(const Snapshot& snapshot, uint16_t* counts) {
  counts[COUNT_WORKING] = 0;
  counts[COUNT_NEEDS] = 0;
  counts[COUNT_DONE] = 0;
  for (uint8_t i = 0; i < snapshot.sessionCount; ++i) {
    switch (snapshot.sessions[i].state) {
      case PanelState::Working:
        ++counts[COUNT_WORKING];
        break;
      case PanelState::NeedsYou:
        ++counts[COUNT_NEEDS];
        break;
      default:
        ++counts[COUNT_DONE];
        break;
    }
  }
}

PanelState deriveState(const uint16_t* counts) {
  if (counts[COUNT_NEEDS] > 0) return PanelState::NeedsYou;
  if (counts[COUNT_WORKING] > 0) return PanelState::Working;
  if (counts[COUNT_DONE] > 0) return PanelState::Done;
  return PanelState::Quiet;
}

void sortSessions(SessionView* sessions, uint8_t count) {
  for (uint8_t i = 1; i < count; ++i) {
    SessionView key = sessions[i];
    int8_t j = static_cast<int8_t>(i) - 1;
    while (j >= 0 && snapshotPriority(sessions[j].state) > snapshotPriority(key.state)) {
      sessions[j + 1] = sessions[j];
      --j;
    }
    sessions[j + 1] = key;
  }
}

void parseSessions(JsonArray array, Snapshot& target) {
  target.sessionCount = 0;
  for (JsonObject item : array) {
    if (target.sessionCount >= MAX_SESSIONS) break;
    SessionView& session = target.sessions[target.sessionCount];

    session.id[0] = '\0';
    session.provider[0] = '\0';
    session.model[0] = '\0';
    session.state = PanelState::Done;
    session.age = 0;
    session.local = false;
    copyUtf8(session.project, sizeof(session.project), "lody", MAX_PROJECT_BYTES);
    copyUtf8(session.title, sizeof(session.title), "untitled", MAX_TITLE_BYTES);

    const char* id = item["id"].as<const char*>();
    if (id != nullptr && id[0] != '\0') {
      copyUtf8(session.id, sizeof(session.id), id, SESSION_ID_BYTES);
    }

    const char* provider = item["provider"].as<const char*>();
    if (provider != nullptr && provider[0] != '\0') {
      copyUtf8(session.provider, sizeof(session.provider), provider, MAX_PROVIDER_BYTES);
    }

    const char* model = item["model"].as<const char*>();
    if (model != nullptr && model[0] != '\0') {
      copyUtf8(session.model, sizeof(session.model), model, MAX_MODEL_BYTES);
    }

    PanelState state;
    if (sessionStateFromText(item["state"].as<const char*>(), state)) {
      session.state = state;
    }

    JsonVariant age = item["age"];
    if (age.is<long>()) {
      long seconds = age.as<long>();
      if (seconds < 0) seconds = 0;
      if (seconds > 604800L) seconds = 604800L;
      session.age = static_cast<uint32_t>(seconds);
    }

    const char* project = item["project"].as<const char*>();
    if (project != nullptr && project[0] != '\0') {
      copyUtf8(session.project, sizeof(session.project), project, MAX_PROJECT_BYTES);
    }

    const char* title = item["title"].as<const char*>();
    if (title != nullptr && title[0] != '\0') {
      copyUtf8(session.title, sizeof(session.title), title, MAX_TITLE_BYTES);
    }

    const char* machine = item["machine"].as<const char*>();
    session.local = machine != nullptr && strcmp(machine, "local") == 0;

    ++target.sessionCount;
  }
  sortSessions(target.sessions, target.sessionCount);
}

void formatSpan(uint32_t seconds, char* out, size_t cap) {
  if (seconds < 60) {
    snprintf(out, cap, "%us", static_cast<unsigned>(seconds));
  } else if (seconds < 3600) {
    snprintf(out, cap, "%um", static_cast<unsigned>(seconds / 60));
  } else if (seconds < 86400) {
    snprintf(out, cap, "%uh", static_cast<unsigned>(seconds / 3600));
  } else {
    snprintf(out, cap, "%ud", static_cast<unsigned>(seconds / 86400));
  }
}

}  // namespace

void snapshotReset(Snapshot& snapshot) {
  snapshot.state = PanelState::Quiet;
  snapshot.counts[COUNT_WORKING] = 0;
  snapshot.counts[COUNT_NEEDS] = 0;
  snapshot.counts[COUNT_DONE] = 0;
  copyUtf8(snapshot.time, sizeof(snapshot.time), "--:--", sizeof(snapshot.time) - 1);
  copyUtf8(snapshot.date, sizeof(snapshot.date), "--", sizeof(snapshot.date) - 1);
  snapshot.sessionCount = 0;
  snapshot.receivedAt = 0;
  snapshot.valid = false;
}

bool snapshotParseLine(Snapshot& out, const char* line) {
  if (line == nullptr || line[0] == '\0') return false;

  g_allocator.reset();
  JsonDocument doc(&g_allocator);
  const DeserializationError error = deserializeJson(doc, line);
  if (error) {
#if DEBUG_SERIAL
    Serial.printf("frame: %s\n", error.c_str());
#endif
    return false;
  }

  JsonObject root = doc.as<JsonObject>();
  if (root.isNull()) return false;

  // 从上一帧开始，缺失字段保留旧值（02-protocol.md）。
  g_staging = out;

  JsonArray array = root["sessions"].as<JsonArray>();
  const bool hasSessions = !array.isNull();
  if (hasSessions) parseSessions(array, g_staging);

  JsonObject counts = root["counts"].as<JsonObject>();
  const bool hasCounts = !counts.isNull();
  if (hasCounts) {
    const uint16_t needs = countField(counts, "needs");
    g_staging.counts[COUNT_WORKING] = countField(counts, "working");
    g_staging.counts[COUNT_NEEDS] = needs != 0 ? needs : countField(counts, "needs_you");
    g_staging.counts[COUNT_DONE] = countField(counts, "done");
  } else if (hasSessions) {
    countSessions(g_staging, g_staging.counts);
  }

  PanelState parsed;
  if (stateFromText(root["state"].as<const char*>(), parsed)) {
    g_staging.state = parsed;
  } else if (hasCounts || hasSessions) {
    g_staging.state = deriveState(g_staging.counts);
  }

  const char* time = root["time"].as<const char*>();
  if (time != nullptr && time[0] != '\0') {
    copyUtf8(g_staging.time, sizeof(g_staging.time), time, sizeof(g_staging.time) - 1);
  }
  const char* date = root["date"].as<const char*>();
  if (date != nullptr && date[0] != '\0') {
    copyUtf8(g_staging.date, sizeof(g_staging.date), date, sizeof(g_staging.date) - 1);
  }

  g_staging.receivedAt = millis();
  g_staging.valid = true;
  out = g_staging;
  return true;
}

uint8_t snapshotPriority(PanelState state) {
  switch (state) {
    case PanelState::NeedsYou:
      return 0;
    case PanelState::Working:
      return 1;
    case PanelState::Done:
      return 2;
    default:
      return 3;
  }
}

void snapshotElapsedText(const Snapshot& snapshot, const SessionView* session,
                         PanelState shown, char* out, size_t cap) {
  if (out == nullptr || cap == 0) return;

  uint32_t seconds = 0;
  if (session != nullptr) {
    seconds = session->age;
    if (snapshot.valid) {
      const uint32_t local = (millis() - snapshot.receivedAt) / 1000;
      if (local < 604800UL) seconds += local;
    }
  }

  char span[12];
  formatSpan(seconds, span, sizeof(span));

  switch (shown) {
    case PanelState::Working:
      snprintf(out, cap, "%s so far", span);
      break;
    case PanelState::NeedsYou:
      snprintf(out, cap, "%s waiting", span);
      break;
    case PanelState::Done:
      snprintf(out, cap, "%s ago", span);
      break;
    case PanelState::Quiet:
      snprintf(out, cap, "nothing running");
      break;
    case PanelState::Error:
      snprintf(out, cap, "no data");
      break;
    case PanelState::LinkLost:
    default:
      snprintf(out, cap, "no link");
      break;
  }
}

bool titleFrameParse(const char* line, char* idOut, size_t idCap) {
  if (idOut != nullptr && idCap > 0) idOut[0] = '\0';
  if (line == nullptr || line[0] == '\0') return false;

  g_allocator.reset();
  JsonDocument doc(&g_allocator);
  if (deserializeJson(doc, line)) return false;

  JsonObject root = doc.as<JsonObject>();
  if (root.isNull()) return false;

  const char* kind = root["t"].as<const char*>();
  if (kind == nullptr || strcmp(kind, "title") != 0) return false;

  // 到这里已经确认是 title 帧；即使字段非法也返回 true，避免它被当成 snapshot。
  if (strlen(line) >= LINE_BUFFER_SIZE) return true;

  const char* id = root["id"].as<const char*>();
  if (id == nullptr || id[0] == '\0') return true;

  char key[SESSION_ID_BUFFER_SIZE];
  copyUtf8(key, sizeof(key), id, SESSION_ID_BYTES);

  const JsonVariant widthValue = root["w"];
  const JsonVariant heightValue = root["h"];
  if (!widthValue.is<long>() || !heightValue.is<long>()) return true;
  const long width = widthValue.as<long>();
  const long height = heightValue.as<long>();
  if (width != TITLE_BITMAP_W || height != TITLE_BITMAP_H) return true;

  const char* encoded = root["bmp"].as<const char*>();
  if (encoded == nullptr) return true;

  uint8_t decoded[TITLE_BITMAP_BYTES];
  size_t decodedLength = 0;
  if (!base64Decode(encoded, decoded, sizeof(decoded), decodedLength) ||
      decodedLength != TITLE_BITMAP_BYTES) {
    return true;
  }

  TitleBitmap* target = nullptr;
  TitleBitmap* oldest = nullptr;
  for (uint8_t i = 0; i < TITLE_CACHE_SLOTS; ++i) {
    TitleBitmap& entry = g_titleCache[i];
    if (!entry.valid) {
      if (target == nullptr) target = &entry;
      continue;
    }
    if (strcmp(entry.id, key) == 0) {
      target = &entry;
      break;
    }
    if (oldest == nullptr || entry.age < oldest->age) oldest = &entry;
  }
  if (target == nullptr) target = oldest;
  if (target == nullptr) return true;

  copyUtf8(target->id, sizeof(target->id), key, SESSION_ID_BYTES);
  target->width = static_cast<uint16_t>(width);
  target->height = static_cast<uint16_t>(height);
  target->age = ++g_titleAge;
  target->valid = true;
  memcpy(target->bitmap, decoded, TITLE_BITMAP_BYTES);

  copyUtf8(idOut, idCap, key, SESSION_ID_BYTES);
  return true;
}

const TitleBitmap* titleCacheFind(const char* id) {
  if (id == nullptr || id[0] == '\0') return nullptr;
  for (uint8_t i = 0; i < TITLE_CACHE_SLOTS; ++i) {
    const TitleBitmap& entry = g_titleCache[i];
    if (entry.valid && strcmp(entry.id, id) == 0) return &entry;
  }
  return nullptr;
}
