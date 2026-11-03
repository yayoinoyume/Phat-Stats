// 面板显示模型 + 串口 JSON 帧解析。
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "config.h"

enum class PanelState : uint8_t {
  Working,
  NeedsYou,
  Done,
  Quiet,
  Error,
  LinkLost,
};

struct SessionView {
  char id[SESSION_ID_BUFFER_SIZE];  // bridge 的 8 字符会话 id
  char provider[PROVIDER_BUFFER_SIZE];
  char model[MODEL_BUFFER_SIZE];
  PanelState state;
  uint32_t age;  // 帧内 age（秒），绘制时再加本地流逝时间
  bool local;
  char project[PROJECT_BUFFER_SIZE];
  char title[TITLE_BUFFER_SIZE];
};

struct Snapshot {
  PanelState state;
  uint16_t counts[3];  // 0=working 1=needs_you 2=done
  char time[TIME_BUFFER_SIZE];
  char date[DATE_BUFFER_SIZE];
  SessionView sessions[MAX_SESSIONS];
  uint8_t sessionCount;
  uint32_t receivedAt;  // millis()
  bool valid;
};

constexpr uint8_t COUNT_WORKING = 0;
constexpr uint8_t COUNT_NEEDS = 1;
constexpr uint8_t COUNT_DONE = 2;

// bridge 渲染的标题位图缓存，按会话 id 复用。
struct TitleBitmap {
  char id[SESSION_ID_BUFFER_SIZE];
  uint16_t width;
  uint16_t height;
  uint32_t age;  // 插入序号，越大越新
  bool valid;
  uint8_t bitmap[TITLE_BITMAP_BYTES];
};

void snapshotReset(Snapshot& snapshot);

// 解析一行 JSON。成功时更新 out 并返回 true；非法/超长帧返回 false 且不改动 out。
bool snapshotParseLine(Snapshot& out, const char* line);

// 解析 title 帧。返回 true 表示这一行是 title 帧（即使字段非法，也不应再当作
// snapshot 处理）；只有成功写入缓存时 idOut 才非空。
bool titleFrameParse(const char* line, char* idOut, size_t idCap);

// 按会话 id 查找有效标题位图；找不到返回 nullptr。
const TitleBitmap* titleCacheFind(const char* id);

// 排序优先级：needs_you < working < done < 其它。
uint8_t snapshotPriority(PanelState state);

// 顶部 "6m so far" 文本；session 为空时仍按 shown 状态给出占位文本。
void snapshotElapsedText(const Snapshot& snapshot, const SessionView* session,
                         PanelState shown, char* out, size_t cap);
