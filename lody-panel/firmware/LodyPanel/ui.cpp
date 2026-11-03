#include "ui.h"

#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <SPI.h>
#include <U8g2_for_Adafruit_GFX.h>

#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>

#include <stdio.h>
#include <string.h>

#include "config.h"
#include "provider_icons.h"
#include "status_font.h"

namespace {

Adafruit_ILI9341 tft(TFT_CS, TFT_DC, TFT_RST);
U8G2_FOR_ADAFRUIT_GFX u8g2;

uint8_t g_spinnerStep = 0;

char g_titleSource[TITLE_BUFFER_SIZE] = "";
char g_titleLines[2][TITLE_LINE_BUFFER] = {{0}, {0}};
bool g_titleValid = false;

uint16_t accentColor(PanelState state) {
  switch (state) {
    case PanelState::NeedsYou:
      return COLOR_PURPLE;
    case PanelState::Done:
      return COLOR_GREEN;
    case PanelState::Error:
      return COLOR_ORANGE;
    case PanelState::LinkLost:
      return COLOR_RED;
    case PanelState::Working:
    case PanelState::Quiet:
    default:
      return COLOR_CREAM;
  }
}

const char* statusEnglish(PanelState state) {
  switch (state) {
    case PanelState::Working:
      return "ON IT";
    case PanelState::NeedsYou:
      return "YOUR TURN";
    case PanelState::Done:
      return "ALL SET";
    case PanelState::Quiet:
      return "ALL QUIET";
    case PanelState::Error:
      return "CHECK";
    case PanelState::LinkLost:
    default:
      return "LINK LOST";
  }
}

// 行号对应 status_font.h 的 12 个字形：在跑 等你 完成 空闲 出错 断线。
uint8_t statusGlyphRow(PanelState state) {
  switch (state) {
    case PanelState::Working:
      return 0;
    case PanelState::NeedsYou:
      return 1;
    case PanelState::Done:
      return 2;
    case PanelState::Quiet:
      return 3;
    case PanelState::Error:
      return 4;
    case PanelState::LinkLost:
    default:
      return 5;
  }
}

const SessionView* featuredSession(const Snapshot& snapshot, int8_t index) {
  if (index < 0 || index >= snapshot.sessionCount) return nullptr;
  return &snapshot.sessions[index];
}

// ---------------------------------------------------------------------------
// 文本工具
// ---------------------------------------------------------------------------

void copyUtf8(char* dst, size_t cap, const char* src, size_t maxBytes) {
  if (dst == nullptr || cap == 0) return;
  if (src == nullptr) src = "";
  size_t limit = maxBytes;
  if (limit > cap - 1) limit = cap - 1;
  size_t len = strlen(src);
  if (len > limit) {
    len = limit;
    while (len > 0 && (static_cast<unsigned char>(src[len]) & 0xC0) == 0x80) --len;
  }
  memcpy(dst, src, len);
  dst[len] = '\0';
}

size_t utf8SeqLen(const char* text) {
  const unsigned char lead = static_cast<unsigned char>(*text);
  if (lead < 0x80) return 1;
  if ((lead & 0xE0) == 0xC0) return 2;
  if ((lead & 0xF0) == 0xE0) return 3;
  if ((lead & 0xF8) == 0xF0) return 4;
  return 1;
}

void trimLastUtf8(char* text) {
  size_t len = strlen(text);
  if (len == 0) return;
  size_t cut = len - 1;
  while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
  text[cut] = '\0';
}

void trimTrailingSpaces(char* text) {
  size_t len = strlen(text);
  while (len > 0 && text[len - 1] == ' ') text[--len] = '\0';
}

int16_t gfxTextWidth(const char* text) {
  int16_t x1 = 0;
  int16_t y1 = 0;
  uint16_t width = 0;
  uint16_t height = 0;
  tft.getTextBounds(text, 0, 0, &x1, &y1, &width, &height);
  return static_cast<int16_t>(width);
}

int16_t drawClippedText(int16_t x, int16_t y, const char* text, int16_t maxWidth,
                        uint16_t color) {
  tft.setTextColor(color);
  char buffer[64];
  copyUtf8(buffer, sizeof(buffer), text, sizeof(buffer) - 1);

  bool clipped = false;
  while (buffer[0] != '\0' && gfxTextWidth(buffer) > maxWidth) {
    trimLastUtf8(buffer);
    clipped = true;
  }
  if (clipped) {
    char probe[64];
    while (true) {
      const int written = snprintf(probe, sizeof(probe), "%s..", buffer);
      if (written < 0) break;
      if (gfxTextWidth(probe) <= maxWidth) {
        strncpy(buffer, probe, sizeof(buffer) - 1);
        buffer[sizeof(buffer) - 1] = '\0';
        break;
      }
      if (buffer[0] == '\0') break;
      trimLastUtf8(buffer);
    }
  }
  tft.setCursor(x, y);
  tft.print(buffer);
  return gfxTextWidth(buffer);
}


const uint16_t* providerIconByKey(const char* key) {
  if (key == nullptr || key[0] == '\0') return nullptr;
  for (uint8_t i = 0; i < PROVIDER_ICON_COUNT; ++i) {
    if (strcmp(key, PROVIDER_ICONS[i].key) == 0) {
      return PROVIDER_ICONS[i].pixels;
    }
  }
  return nullptr;
}

const uint16_t* providerIconFor(const char* provider) {
  const uint16_t* icon = providerIconByKey(provider);
  if (icon != nullptr) return icon;

  // bridge 可能使用短名/厂商名；统一回退到 lody。
  if (provider != nullptr) {
    if (strcmp(provider, "claude") == 0 || strcmp(provider, "anthropic") == 0 ||
        strcmp(provider, "claude-code") == 0) {
      return providerIconByKey("claudecode");
    }
    if (strcmp(provider, "gemini") == 0 || strcmp(provider, "gemini-cli") == 0 ||
        strcmp(provider, "google") == 0) {
      return providerIconByKey("geminicli");
    }
    if (strcmp(provider, "zhipu") == 0 || strcmp(provider, "chatglm") == 0) {
      return providerIconByKey("glm");
    }
    if (strcmp(provider, "xai") == 0) return providerIconByKey("grok");
    if (strcmp(provider, "moonshot") == 0) return providerIconByKey("kimi");
    if (strcmp(provider, "gpt") == 0) return providerIconByKey("openai");
    if (strcmp(provider, "alibaba") == 0) return providerIconByKey("qwen");
  }
  return PROVIDER_ICON_DATA[PROVIDER_ICON_LODY_INDEX];
}

void drawProvider(const SessionView& session) {
  tft.drawRGBBitmap(PROVIDER_ICON_X, PROVIDER_ICON_Y, providerIconFor(session.provider),
                    PROVIDER_ICON_SIZE, PROVIDER_ICON_SIZE);
  if (session.model[0] == '\0') return;

  tft.setFont(&FreeSans9pt7b);
  tft.setTextColor(COLOR_TOPBAR);
  drawClippedText(PROVIDER_TEXT_X, PROVIDER_TEXT_BASELINE, session.model,
                  PROVIDER_TEXT_RIGHT - PROVIDER_TEXT_X, COLOR_TOPBAR);
}

// ---------------------------------------------------------------------------
// 标题缓存与换行
// ---------------------------------------------------------------------------

bool titleProbeFits(const char* line, const char* token, size_t tokenLen) {
  char probe[TITLE_LINE_BUFFER];
  const size_t used = strlen(line);
  if (used + tokenLen >= sizeof(probe)) return false;
  memcpy(probe, line, used);
  memcpy(probe + used, token, tokenLen);
  probe[used + tokenLen] = '\0';
  return u8g2.getUTF8Width(probe) <= TITLE_MAX_W;
}

bool appendTitleBytes(char* line, const char* token, size_t tokenLen) {
  const size_t used = strlen(line);
  if (used + tokenLen >= TITLE_LINE_BUFFER) return false;
  memcpy(line + used, token, tokenLen);
  line[used + tokenLen] = '\0';
  return true;
}

void appendEllipsis(char* line) {
  char probe[TITLE_LINE_BUFFER];
  while (line[0] != '\0') {
    const size_t used = strlen(line);
    if (used + 2 < sizeof(probe)) {
      memcpy(probe, line, used);
      probe[used] = '.';
      probe[used + 1] = '.';
      probe[used + 2] = '\0';
      if (u8g2.getUTF8Width(probe) <= TITLE_MAX_W) {
        memcpy(line, probe, used + 3);
        return;
      }
    }
    trimLastUtf8(line);
  }
}

void wrapTitle(const char* text) {
  g_titleLines[0][0] = '\0';
  g_titleLines[1][0] = '\0';

  char line[TITLE_LINE_BUFFER] = "";
  uint8_t lineIndex = 0;
  bool truncated = false;

  auto commitLine = [&]() -> bool {
    trimTrailingSpaces(line);
    if (line[0] == '\0') return true;
    strncpy(g_titleLines[lineIndex], line, TITLE_LINE_BUFFER - 1);
    g_titleLines[lineIndex][TITLE_LINE_BUFFER - 1] = '\0';
    line[0] = '\0';
    if (lineIndex == 0) {
      lineIndex = 1;
      return true;
    }
    truncated = true;
    return false;
  };

  const char* cursor = text;
  while (*cursor != '\0' && !truncated) {
    const bool isSpace = (*cursor == ' ');
    size_t tokenLen = 0;
    if (isSpace) {
      tokenLen = 1;
    } else if (static_cast<unsigned char>(*cursor) < 0x80 &&
               static_cast<unsigned char>(*cursor) > 0x20) {
      const char* end = cursor;
      while (*end != '\0' && static_cast<unsigned char>(*end) < 0x80 &&
             static_cast<unsigned char>(*end) > 0x20) {
        ++end;
      }
      tokenLen = static_cast<size_t>(end - cursor);
    } else {
      tokenLen = utf8SeqLen(cursor);
      const size_t remaining = strlen(cursor);
      if (tokenLen > remaining) tokenLen = remaining;
    }

    size_t offset = 0;
    while (offset < tokenLen && !truncated) {
      if (isSpace && line[0] == '\0') {
        offset = tokenLen;  // 行首空格丢弃
        break;
      }

      size_t step = tokenLen - offset;
      if (!titleProbeFits(line, cursor + offset, step)) {
        if (line[0] == '\0') {
          // 单个 token 比整行还宽：按码点硬切。
          step = utf8SeqLen(cursor + offset);
          if (step > tokenLen - offset) step = tokenLen - offset;
        } else {
          if (!commitLine()) break;
          continue;  // 在新行重试同一个 token
        }
      }
      if (!appendTitleBytes(line, cursor + offset, step)) {
        truncated = true;
        break;
      }
      offset += step;
    }
    if (truncated) break;
    cursor += tokenLen;
  }

  if (line[0] != '\0') {
    trimTrailingSpaces(line);
    if (line[0] != '\0') {
      strncpy(g_titleLines[lineIndex], line, TITLE_LINE_BUFFER - 1);
      g_titleLines[lineIndex][TITLE_LINE_BUFFER - 1] = '\0';
    }
  }

  if (truncated && g_titleLines[1][0] != '\0') {
    appendEllipsis(g_titleLines[1]);
  }
}

void prepareTitle(const char* text) {
  if (text == nullptr) text = "";
  if (g_titleValid && strcmp(text, g_titleSource) == 0) return;
  copyUtf8(g_titleSource, sizeof(g_titleSource), text, MAX_TITLE_BYTES);
  u8g2.setFont(u8g2_font_wqy16_t_gb2312b);
  wrapTitle(g_titleSource);
  g_titleValid = true;
}

void drawTitle(const char* text) {
  u8g2.setFont(u8g2_font_wqy16_t_gb2312b);
  u8g2.setFontMode(1);
  u8g2.setForegroundColor(COLOR_CREAM);
  prepareTitle(text);
  const int16_t ascent = u8g2.getFontAscent();
  if (g_titleLines[0][0] != '\0') {
    u8g2.drawUTF8(TITLE_X, TITLE_TOP_1 + ascent, g_titleLines[0]);
  }
  if (g_titleLines[1][0] != '\0') {
    u8g2.drawUTF8(TITLE_X, TITLE_TOP_2 + ascent, g_titleLines[1]);
  }
}

// ---------------------------------------------------------------------------
// 左侧状态卡
// ---------------------------------------------------------------------------

void drawEmptyDots() {
  for (uint8_t i = 0; i < 3; ++i) {
    const int16_t x = DOT_RIGHT - static_cast<int16_t>(2 - i) * DOT_GAP;
    tft.fillCircle(x, DOT_CY, DOT_R, COLOR_DOT_IDLE);
  }
}

void drawDots(const Snapshot& snapshot, int8_t featuredIndex, uint8_t ringRadius) {
  uint8_t count = snapshot.sessionCount;
  if (count > DOT_LIMIT) count = DOT_LIMIT;
  if (count == 0) {
    drawEmptyDots();
    return;
  }
  for (uint8_t i = 0; i < count; ++i) {
    const int16_t x = DOT_RIGHT - static_cast<int16_t>(count - 1 - i) * DOT_GAP;
    const uint16_t color = accentColor(snapshot.sessions[i].state);
    tft.fillCircle(x, DOT_CY, DOT_R, color);
    if (i == featuredIndex) {
      tft.drawCircle(x, DOT_CY, ringRadius, color);
    }
  }
}

void drawSpark(uint16_t color) {
  tft.drawLine(SPARK_CX - SPARK_R, SPARK_CY, SPARK_CX + SPARK_R, SPARK_CY, color);
  tft.drawLine(SPARK_CX, SPARK_CY - SPARK_R, SPARK_CX, SPARK_CY + SPARK_R, color);
  tft.drawLine(SPARK_CX - 4, SPARK_CY - 4, SPARK_CX + 4, SPARK_CY + 4, color);
  tft.drawLine(SPARK_CX - 4, SPARK_CY + 4, SPARK_CX + 4, SPARK_CY - 4, color);
}

uint16_t blend565(uint16_t from, uint16_t to, uint8_t level) {
  const int32_t fromR = (from >> 11) & 0x1F;
  const int32_t fromG = (from >> 5) & 0x3F;
  const int32_t fromB = from & 0x1F;
  const int32_t toR = (to >> 11) & 0x1F;
  const int32_t toG = (to >> 5) & 0x3F;
  const int32_t toB = to & 0x1F;
  const int32_t r = fromR + ((toR - fromR) * level) / 255;
  const int32_t g = fromG + ((toG - fromG) * level) / 255;
  const int32_t b = fromB + ((toB - fromB) * level) / 255;
  return static_cast<uint16_t>((r << 11) | (g << 5) | b);
}

void drawLinkSparkImpl(uint8_t level) {
  const int16_t box = static_cast<int16_t>(SPARK_R + 2) * 2;
  tft.fillRect(SPARK_CX - SPARK_R - 2, SPARK_CY - SPARK_R - 2, box, box, COLOR_BG);
  drawSpark(blend565(COLOR_RED_DIM, COLOR_RED, level));
}

void drawWorkingScanImpl(uint8_t phase) {
  tft.fillRect(SCAN_X, SCAN_Y, SCAN_WIDTH, SCAN_HEIGHT, COLOR_BG);
  int16_t x = SCAN_X + static_cast<int16_t>(phase) * SCAN_STEP;
  if (x >= SCAN_X + SCAN_WIDTH) return;
  int16_t width = SCAN_SEGMENT;
  if (x + width > SCAN_X + SCAN_WIDTH) width = SCAN_X + SCAN_WIDTH - x;
  tft.fillRect(x, SCAN_Y, width, SCAN_HEIGHT, accentColor(PanelState::Working));
}

void drawDotPulseImpl(const Snapshot& snapshot, int8_t featuredIndex,
                      uint8_t ringRadius) {
  const int16_t areaWidth =
      static_cast<int16_t>(DOT_LIMIT - 1) * DOT_GAP + (DOT_RING_MAX + 2) * 2;
  const int16_t areaX = DOT_RIGHT - static_cast<int16_t>(DOT_LIMIT - 1) * DOT_GAP -
                        DOT_RING_MAX - 2;
  const int16_t areaY = DOT_CY - DOT_RING_MAX - 2;
  const int16_t areaHeight = (DOT_RING_MAX + 2) * 2;
  tft.fillRect(areaX, areaY, areaWidth, areaHeight, COLOR_BG);
  drawDots(snapshot, featuredIndex, ringRadius);
}

void drawQuietClockImpl(const Snapshot& snapshot, bool colonOn) {
  tft.fillRect(TITLE_X, QUIET_CLOCK_TOP, QUIET_CLOCK_WIDTH, QUIET_CLOCK_HEIGHT, COLOR_BG);
  tft.setFont(&FreeSansBold24pt7b);
  tft.setTextColor(COLOR_CREAM);

  const char* time = snapshot.valid ? snapshot.time : "--:--";
  if (strlen(time) == 5 && time[2] == ':') {
    char hours[3] = {time[0], time[1], '\0'};
    char minutes[3] = {time[3], time[4], '\0'};
    tft.setCursor(TITLE_X, QUIET_CLOCK_BASELINE);
    tft.print(hours);
    const int16_t hoursWidth = gfxTextWidth(hours);
    const int16_t colonX = TITLE_X + hoursWidth;
    const int16_t colonWidth = gfxTextWidth(":");
    if (colonOn) {
      tft.setCursor(colonX, QUIET_CLOCK_BASELINE);
      tft.print(":");
    }
    tft.setCursor(colonX + colonWidth, QUIET_CLOCK_BASELINE);
    tft.print(minutes);
    return;
  }

  tft.setCursor(TITLE_X, QUIET_CLOCK_BASELINE);
  tft.print(time);
}

void drawTopBarImpl(const Snapshot& snapshot, const SessionView* featured,
                    int8_t featuredIndex, PanelState shown) {
  tft.fillRect(0, 0, CARD_W, TOPBAR_HEIGHT, COLOR_BG);

  drawSpark(accentColor(shown));

  char elapsed[ELAPSED_TEXT_SIZE];
  snapshotElapsedText(snapshot, featured, shown, elapsed, sizeof(elapsed));
  tft.setFont(&FreeSans9pt7b);
  tft.setTextColor(COLOR_TOPBAR);
  tft.setCursor(32, TOPBAR_BASELINE);
  tft.print(elapsed);

  if (shown == PanelState::LinkLost) {
    drawEmptyDots();
  } else {
    drawDots(snapshot, featuredIndex, DOT_RING_RADIUS);
  }
}

void drawStatus(PanelState shown) {
  const uint16_t accent = accentColor(shown);
  const uint8_t row = statusGlyphRow(shown);

  tft.drawBitmap(STATUS_GLYPH_X1, STATUS_GLYPH_TOP, STATUS_GLYPHS[row * 2],
                 STATUS_GLYPH_W, STATUS_GLYPH_H, accent);
  tft.drawBitmap(STATUS_GLYPH_X2, STATUS_GLYPH_TOP, STATUS_GLYPHS[row * 2 + 1],
                 STATUS_GLYPH_W, STATUS_GLYPH_H, accent);

  tft.setFont(&FreeSansBold9pt7b);
  tft.setTextColor(accent);
  tft.setCursor(STATUS_EN_X, STATUS_EN_BASELINE);
  tft.print(statusEnglish(shown));
}

const int16_t SPOKE_OUT_X[8] = {0, 16, 22, 16, 0, -16, -22, -16};
const int16_t SPOKE_OUT_Y[8] = {-22, -16, 0, 16, 22, 16, 0, -16};
const int16_t SPOKE_IN_X[8] = {0, 8, 11, 8, 0, -8, -11, -8};
const int16_t SPOKE_IN_Y[8] = {-11, -8, 0, 8, 11, 8, 0, -8};

void drawSpinnerShape() {
  const int16_t box = static_cast<int16_t>(SPINNER_R + 2) * 2;
  tft.fillRect(SPINNER_CX - SPINNER_R - 2, SPINNER_CY - SPINNER_R - 2, box, box,
               COLOR_BG);

  const uint8_t lead = g_spinnerStep & 0x07;
  for (uint8_t i = 0; i < 8; ++i) {
    uint16_t color = COLOR_SPINNER_DIM;
    if (i == lead) {
      color = COLOR_PURPLE;
    } else if (i == ((lead + 7) & 0x07)) {
      color = COLOR_SPINNER_MID;
    }
    const int16_t x1 = SPINNER_CX + SPOKE_IN_X[i];
    const int16_t y1 = SPINNER_CY + SPOKE_IN_Y[i];
    const int16_t x2 = SPINNER_CX + SPOKE_OUT_X[i];
    const int16_t y2 = SPINNER_CY + SPOKE_OUT_Y[i];
    tft.drawLine(x1, y1, x2, y2, color);
    tft.drawLine(x1 + 1, y1, x2 + 1, y2, color);
  }
}

void drawTitleArea(const Snapshot& snapshot, const SessionView* featured,
                   PanelState shown) {
  if (shown == PanelState::Quiet) {
    drawQuietClockImpl(snapshot, true);
    return;
  }
  if (shown == PanelState::LinkLost) {
    drawTitle("Check computer");
    return;
  }
  if (shown == PanelState::Error) {
    drawTitle("Lody not reachable");
    return;
  }
  if (featured != nullptr) {
    const TitleBitmap* cached = titleCacheFind(featured->id);
    if (cached != nullptr) {
      tft.drawBitmap(TITLE_X, TITLE_TOP_1, cached->bitmap, cached->width, cached->height,
                     COLOR_CREAM);
      return;
    }
    drawTitle(featured->title);
    return;
  }
  drawTitle("waiting for frame");
}

void drawMeta(const char* left, const char* right, uint16_t leftColor) {
  tft.setFont(&FreeSans9pt7b);
  const int16_t width = drawClippedText(META_X, META_BASELINE, left, 150, leftColor);
  const int16_t dotX = META_X + width + 5;
  tft.fillCircle(dotX, META_BASELINE - 4, 1, COLOR_GRAY);
  tft.setTextColor(COLOR_GRAY);
  tft.setCursor(dotX + 7, META_BASELINE);
  tft.print(right);
}

void drawMetaFor(const Snapshot& /*snapshot*/, const SessionView* featured,
                 PanelState shown) {
  switch (shown) {
    case PanelState::LinkLost:
      drawMeta("lody", "no link", COLOR_GRAY);
      return;
    case PanelState::Error:
      drawMeta("lody", "error", COLOR_GRAY);
      return;
    case PanelState::Quiet:
      drawMeta("no sessions", "lody", COLOR_GRAY);
      return;
    default:
      break;
  }
  if (featured != nullptr) {
    drawMeta(featured->project, featured->local ? "local" : "remote", COLOR_TOPBAR);
  } else {
    drawMeta("waiting", "no session", COLOR_GRAY);
  }
}


#if ENABLE_BATTERY
int8_t readBatteryPercent() {
  analogSetPinAttenuation(BATTERY_PIN, ADC_11db);
  uint32_t millivolts = 0;
  for (uint8_t i = 0; i < 4; ++i) {
    millivolts += analogReadMilliVolts(BATTERY_PIN);
  }
  millivolts /= 4;
  const float volts = (millivolts / 1000.0f) * BATTERY_DIVIDER;
  if (volts < 2.5f) return -1;  // 没有分压/没有电池
  int percent = static_cast<int>((volts - 3.3f) / (4.2f - 3.3f) * 100.0f);
  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;
  return static_cast<int8_t>(percent);
}
#endif

void drawCountRow(uint8_t row, uint16_t value, const char* label, uint16_t color) {
  const int16_t baseline = RAIL_COUNT_BASELINE + static_cast<int16_t>(row) * RAIL_COUNT_GAP;
  char number[8];
  snprintf(number, sizeof(number), "%u", static_cast<unsigned>(value));

  tft.setFont(&FreeSansBold9pt7b);
  tft.setTextColor(value == 0 ? COLOR_GRAY : color);
  tft.setCursor(RAIL_COUNT_NUMBER_RIGHT - gfxTextWidth(number), baseline);
  tft.print(number);

  tft.setFont(&FreeSans9pt7b);
  tft.setTextColor(COLOR_GRAY);
  tft.setCursor(RAIL_COUNT_LABEL_X, baseline);
  tft.print(label);
}

void drawBattery() {
  int8_t percent = -1;
#if ENABLE_BATTERY
  percent = readBatteryPercent();
#endif

  uint8_t filled = 3;
  if (percent >= 0) {
    filled = static_cast<uint8_t>((percent + 24) / 25);
    if (filled > 4) filled = 4;
  }
  for (uint8_t i = 0; i < 4; ++i) {
    const int16_t x = RAIL_BATTERY_BARS_X +
                      static_cast<int16_t>(i) * (RAIL_BATTERY_BAR_W + RAIL_BATTERY_BAR_GAP);
    if (i < filled) {
      tft.fillRect(x, RAIL_BATTERY_BARS_Y, RAIL_BATTERY_BAR_W, RAIL_BATTERY_BAR_H,
                   COLOR_TOPBAR);
    } else {
      tft.drawRect(x, RAIL_BATTERY_BARS_Y, RAIL_BATTERY_BAR_W, RAIL_BATTERY_BAR_H,
                   COLOR_LINE);
    }
  }

  char label[8];
  if (percent >= 0) {
    snprintf(label, sizeof(label), "%d%%", percent);
  } else {
    strcpy(label, "USB");
  }
  tft.setFont(&FreeSans9pt7b);
  tft.setTextColor(COLOR_GRAY);
  tft.setCursor(RAIL_BATTERY_LABEL_X, RAIL_BATTERY_BASELINE);
  tft.print(label);
}

void drawRail(const Snapshot& snapshot, PanelState shown) {
  tft.setFont(&FreeSansBold9pt7b);
  tft.setTextColor(COLOR_CREAM);
  tft.setCursor(RAIL_TEXT_X, RAIL_MARK_BASELINE);
  tft.print("LODY");

  tft.setFont(&FreeSansBold12pt7b);
  tft.setTextColor(COLOR_CREAM);
  tft.setCursor(RAIL_TEXT_X, RAIL_CLOCK_BASELINE);
  tft.print(snapshot.valid ? snapshot.time : "--:--");

  tft.setFont(&FreeSans9pt7b);
  tft.setTextColor(COLOR_GRAY);
  tft.setCursor(RAIL_TEXT_X, RAIL_DATE_BASELINE);
  tft.print(snapshot.valid ? snapshot.date : "--");

  const bool lost = shown == PanelState::LinkLost;
  drawCountRow(0, lost ? 0 : snapshot.counts[COUNT_WORKING], "work", COLOR_CREAM);
  drawCountRow(1, lost ? 0 : snapshot.counts[COUNT_NEEDS], "yours", COLOR_PURPLE);
  drawCountRow(2, lost ? 0 : snapshot.counts[COUNT_DONE], "today", COLOR_GREEN);

  drawBattery();
}

}  // namespace

namespace ui {

void begin() {
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, LOW);

  SPI.begin(TFT_SCLK, TFT_MISO, TFT_MOSI, TFT_CS);
  tft.begin();
  tft.setRotation(SCREEN_ROTATION);
  tft.fillScreen(COLOR_BG);

  u8g2.begin(tft);
  u8g2.setFontMode(1);
  u8g2.setFontDirection(0);
  u8g2.setForegroundColor(COLOR_CREAM);
  u8g2.setBackgroundColor(COLOR_BG);

  digitalWrite(TFT_BL, HIGH);
}

void drawFull(const Snapshot& snapshot, int8_t featuredIndex, PanelState shown) {
  tft.setRotation(SCREEN_ROTATION);
  tft.fillScreen(COLOR_BG);

  const SessionView* featured = featuredSession(snapshot, featuredIndex);
  drawTopBarImpl(snapshot, featured, featuredIndex, shown);
  drawStatus(shown);
  if (shown == PanelState::Working) {
    drawSpinnerShape();
  }
  if (featured != nullptr && (shown == PanelState::Working ||
                              shown == PanelState::NeedsYou ||
                              shown == PanelState::Done)) {
    drawProvider(*featured);
  }
  drawTitleArea(snapshot, featured, shown);
  drawMetaFor(snapshot, featured, shown);

  tft.drawFastVLine(RAIL_X, 0, SCREEN_H, COLOR_LINE);
  drawRail(snapshot, shown);
}

void drawTopBar(const Snapshot& snapshot, int8_t featuredIndex, PanelState shown) {
  tft.setRotation(SCREEN_ROTATION);
  drawTopBarImpl(snapshot, featuredSession(snapshot, featuredIndex), featuredIndex, shown);
}

void drawSpinner(uint8_t step) {
  tft.setRotation(SCREEN_ROTATION);
  g_spinnerStep = step & 0x07;
  drawSpinnerShape();
}

void drawWorkingScan(uint8_t phase) {
  tft.setRotation(SCREEN_ROTATION);
  drawWorkingScanImpl(phase);
}

void drawDotPulse(const Snapshot& snapshot, int8_t featuredIndex, uint8_t ringRadius) {
  tft.setRotation(SCREEN_ROTATION);
  drawDotPulseImpl(snapshot, featuredIndex, ringRadius);
}

void drawQuietClock(const Snapshot& snapshot, bool colonOn) {
  tft.setRotation(SCREEN_ROTATION);
  drawQuietClockImpl(snapshot, colonOn);
}

void drawLinkSpark(uint8_t level) {
  tft.setRotation(SCREEN_ROTATION);
  drawLinkSparkImpl(level);
}

}  // namespace ui
