// Lody Panel：ESP32 + ILI9341 状态小屏。
// 串口输入一行一个 JSON（见 lody-panel/docs/02-protocol.md），本文件只负责
// 收帧、超时、会话轮播和 spinner 调度；绘制在 ui.cpp。
#include <Arduino.h>

#include "config.h"

#if ENABLE_NEOPIXEL
#include <Adafruit_NeoPixel.h>
#endif

#include "snapshot.h"
#include "ui.h"

#if ENABLE_NEOPIXEL
Adafruit_NeoPixel g_pixels(NEOPIXEL_COUNT, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);
#endif

namespace {

Snapshot g_snapshot;
Snapshot g_drawn;

char g_line[LINE_BUFFER_SIZE + 1];
size_t g_lineLength = 0;
bool g_lineOverflow = false;

bool g_haveFrame = false;
uint32_t g_lastFrameAt = 0;

PanelState g_shown = PanelState::LinkLost;
int8_t g_featured = -1;
uint32_t g_rotateAt = 0;

bool g_haveDrawn = false;
PanelState g_drawnState = PanelState::LinkLost;
int8_t g_drawnFeatured = -1;

uint8_t g_spinnerStep = 0;
uint32_t g_spinnerAt = 0;

char g_elapsedDrawn[ELAPSED_TEXT_SIZE] = "";
uint32_t g_elapsedCheckedAt = 0;

uint8_t g_scanPhase = 0;
uint32_t g_scanAt = 0;
uint32_t g_pulseStart = 0;
uint32_t g_pulseAt = 0;
bool g_colonOn = true;
uint32_t g_colonAt = 0;
uint32_t g_breathStart = 0;
uint32_t g_breathAt = 0;
uint8_t g_breathLevel = 255;

void resetAnimations(uint32_t now) {
  g_scanPhase = 0;
  g_scanAt = now;
  g_pulseStart = now;
  g_pulseAt = now;
  g_colonOn = true;
  g_colonAt = now;
  g_breathStart = now;
  g_breathAt = now;
  g_breathLevel = 0;
}

PanelState effectiveState(uint32_t now) {
  if (!g_haveFrame) return PanelState::LinkLost;
  if (static_cast<uint32_t>(now - g_lastFrameAt) >= LINK_TIMEOUT_MS) {
    return PanelState::LinkLost;
  }
  return g_snapshot.state;
}

const SessionView* featuredNow() {
  if (g_featured < 0 || g_featured >= g_snapshot.sessionCount) return nullptr;
  return &g_snapshot.sessions[g_featured];
}

uint8_t visibleSessionCount() {
  const uint8_t count = g_snapshot.sessionCount;
  return count < DOT_LIMIT ? count : static_cast<uint8_t>(DOT_LIMIT);
}

void drawNow() {
  const int8_t index = visibleSessionCount() > 0 ? g_featured : static_cast<int8_t>(-1);
  ui::drawFull(g_snapshot, index, g_shown);
  if (g_shown == PanelState::Working) {
    ui::drawWorkingScan(g_scanPhase);
  } else if (g_shown == PanelState::Quiet) {
    ui::drawQuietClock(g_snapshot, g_colonOn);
  } else if (g_shown == PanelState::LinkLost) {
    ui::drawLinkSpark(g_breathLevel);
  }
  g_drawn = g_snapshot;
  g_drawnState = g_shown;
  g_drawnFeatured = g_featured;
  g_haveDrawn = true;
  snapshotElapsedText(g_snapshot, featuredNow(), g_shown, g_elapsedDrawn,
                      sizeof(g_elapsedDrawn));
  g_elapsedCheckedAt = millis();
}

bool viewChanged() {
  if (!g_haveDrawn) return true;
  if (g_drawnState != g_shown || g_drawnFeatured != g_featured) return true;
  if (strcmp(g_drawn.time, g_snapshot.time) != 0) return true;
  if (strcmp(g_drawn.date, g_snapshot.date) != 0) return true;
  for (uint8_t i = 0; i < 3; ++i) {
    if (g_drawn.counts[i] != g_snapshot.counts[i]) return true;
  }
  if (g_drawn.sessionCount != g_snapshot.sessionCount) return true;

  const uint8_t shared = g_drawn.sessionCount < g_snapshot.sessionCount
                            ? g_drawn.sessionCount
                            : g_snapshot.sessionCount;
  for (uint8_t i = 0; i < shared; ++i) {
    if (g_drawn.sessions[i].state != g_snapshot.sessions[i].state) return true;
  }

  const SessionView* current = featuredNow();
  const SessionView* drawn = nullptr;
  if (g_drawnFeatured >= 0 && g_drawnFeatured < g_drawn.sessionCount) {
    drawn = &g_drawn.sessions[g_drawnFeatured];
  }
  if ((current == nullptr) != (drawn == nullptr)) return true;
  if (current != nullptr && drawn != nullptr) {
    if (strcmp(current->id, drawn->id) != 0) return true;
    if (current->state != drawn->state || current->local != drawn->local) return true;
    if (strcmp(current->title, drawn->title) != 0) return true;
    if (strcmp(current->project, drawn->project) != 0) return true;
    if (strcmp(current->provider, drawn->provider) != 0) return true;
    if (strcmp(current->model, drawn->model) != 0) return true;
  }
  return false;
}

#if ENABLE_NEOPIXEL
uint32_t stateRgb(PanelState state) {
  switch (state) {
    case PanelState::NeedsYou:
      return 0x9D95FF;
    case PanelState::Done:
      return 0xABFF84;
    case PanelState::Error:
      return 0xFF8709;
    case PanelState::LinkLost:
      return 0xFF5F57;
    case PanelState::Working:
    case PanelState::Quiet:
    default:
      return 0xFFFCE1;
  }
}
#endif

void updateNeopixel() {
#if ENABLE_NEOPIXEL
  const uint32_t color = stateRgb(g_shown);
  for (uint16_t i = 0; i < NEOPIXEL_COUNT; ++i) {
    g_pixels.setPixelColor(i, color);
  }
  g_pixels.show();
#endif
}

void onFrame() {
  char titleId[SESSION_ID_BUFFER_SIZE];
  if (titleFrameParse(g_line, titleId, sizeof(titleId))) {
    const SessionView* current = featuredNow();
    if (titleId[0] != '\0' && current != nullptr && strcmp(current->id, titleId) == 0) {
      drawNow();
    }
    return;
  }

  char previousId[SESSION_ID_BUFFER_SIZE];
  const SessionView* previousFeatured = featuredNow();
  if (previousFeatured != nullptr) {
    strncpy(previousId, previousFeatured->id, sizeof(previousId) - 1);
    previousId[sizeof(previousId) - 1] = '\0';
  } else {
    previousId[0] = '\0';
  }

  Snapshot next = g_snapshot;
  if (!snapshotParseLine(next, g_line)) return;

  const uint32_t now = next.receivedAt;
  const PanelState previousShown = g_shown;
  const bool recovering =
      !g_haveFrame || previousShown == PanelState::LinkLost ||
      static_cast<uint32_t>(now - g_lastFrameAt) >= LINK_TIMEOUT_MS;

  g_snapshot = next;
  g_lastFrameAt = now;
  g_haveFrame = true;
  g_shown = effectiveState(now);

  if (g_shown == PanelState::NeedsYou) {
    g_featured = g_snapshot.sessionCount > 0 ? 0 : static_cast<int8_t>(-1);
  } else if (recovering || g_shown != previousShown) {
    g_featured = g_snapshot.sessionCount > 0 ? 0 : static_cast<int8_t>(-1);
  }
  if (g_featured >= visibleSessionCount()) {
    g_featured = visibleSessionCount() > 0 ? 0 : static_cast<int8_t>(-1);
  }

  const SessionView* currentFeatured = featuredNow();
  const bool featuredChanged =
      strcmp(previousId, currentFeatured != nullptr ? currentFeatured->id : "") != 0;
  if (recovering || g_shown != previousShown || featuredChanged) {
    resetAnimations(now);
  }

  if (recovering || g_shown != previousShown || g_snapshot.sessionCount <= 1) {
    g_rotateAt = now + SESSION_ROTATE_MS;
  }

  updateNeopixel();
  if (viewChanged()) drawNow();
}

void readSerial() {
  // 每轮最多处理 256 字节，避免长帧阻塞 spinner 动画。
  uint16_t budget = 256;
  while (budget-- > 0 && Serial.available() > 0) {
    const int value = Serial.read();
    if (value < 0) break;
    const char c = static_cast<char>(value);

    if (c == '\n' || c == '\r') {
      if (!g_lineOverflow && g_lineLength > 0) {
        g_line[g_lineLength] = '\0';
        onFrame();
      }
      g_lineLength = 0;
      g_lineOverflow = false;
      continue;
    }

    if (g_lineLength < LINE_BUFFER_SIZE) {
      g_line[g_lineLength++] = c;
    } else {
      g_lineOverflow = true;
    }
  }
}

void applyShownState(uint32_t now) {
  const PanelState next = effectiveState(now);
  if (next == g_shown) return;

  g_shown = next;
  if (next == PanelState::LinkLost) {
    g_featured = -1;
  } else if (next == PanelState::NeedsYou) {
    g_featured = g_snapshot.sessionCount > 0 ? 0 : static_cast<int8_t>(-1);
    g_rotateAt = now + SESSION_ROTATE_MS;
  } else if (g_featured >= visibleSessionCount()) {
    g_featured = visibleSessionCount() > 0 ? 0 : static_cast<int8_t>(-1);
  }
  resetAnimations(now);
  drawNow();
  updateNeopixel();
}

}  // namespace

void setup() {
  // 必须在 begin() 之前扩大 RX ring；core 2.0.5 默认只有 256 B，
  // 整卡重绘时最坏约 2 KB 的帧会丢字节。
  Serial.setRxBufferSize(4096);
  Serial.begin(SERIAL_BAUD);

#if ENABLE_NEOPIXEL
  g_pixels.begin();
  g_pixels.setBrightness(16);
  g_pixels.clear();
  g_pixels.show();
#endif

  snapshotReset(g_snapshot);
  snapshotReset(g_drawn);
  ui::begin();

  g_shown = PanelState::LinkLost;
  g_featured = -1;
  resetAnimations(millis());
  drawNow();
}

void loop() {
  readSerial();

  const uint32_t now = millis();
  applyShownState(now);

  if (g_shown == PanelState::Working) {
    if (static_cast<uint32_t>(now - g_spinnerAt) >= SPINNER_STEP_MS) {
      g_spinnerStep = (g_spinnerStep + 1) & 0x07;
      ui::drawSpinner(g_spinnerStep);
      g_spinnerAt = now;
    }
  } else {
    g_spinnerAt = now;
  }

  const uint8_t visible = visibleSessionCount();
  if ((g_shown == PanelState::Working || g_shown == PanelState::Done) &&
      visible > 1 && g_featured >= 0) {
    if (static_cast<int32_t>(now - g_rotateAt) >= 0) {
      g_featured = static_cast<int8_t>((g_featured + 1) % visible);
      g_rotateAt = now + SESSION_ROTATE_MS;
      resetAnimations(now);
      drawNow();
    }
  }

  if (g_shown == PanelState::Working) {
    if (static_cast<uint32_t>(now - g_scanAt) >= WORK_SCAN_STEP_MS) {
      g_scanPhase = static_cast<uint8_t>((g_scanPhase + 1) % SCAN_PHASES);
      g_scanAt = now;
      ui::drawWorkingScan(g_scanPhase);
    }
  } else if (g_shown == PanelState::NeedsYou) {
    if (static_cast<uint32_t>(now - g_pulseAt) >= DOT_PULSE_STEP_MS) {
      g_pulseAt = now;
      const uint32_t halfPeriod = DOT_PULSE_PERIOD_MS / 2;
      const uint32_t phase = (now - g_pulseStart) % DOT_PULSE_PERIOD_MS;
      const uint32_t ramp = phase < halfPeriod ? phase : DOT_PULSE_PERIOD_MS - phase;
      const uint8_t radius = static_cast<uint8_t>(
          DOT_RING_RADIUS + (ramp * 2 + halfPeriod - 1) / halfPeriod);
      ui::drawDotPulse(g_snapshot, g_featured, radius);
    }
  } else if (g_shown == PanelState::Quiet) {
    if (static_cast<uint32_t>(now - g_colonAt) >= QUIET_COLON_MS) {
      g_colonOn = !g_colonOn;
      g_colonAt = now;
      ui::drawQuietClock(g_snapshot, g_colonOn);
    }
  } else if (g_shown == PanelState::LinkLost) {
    if (static_cast<uint32_t>(now - g_breathAt) >= LINK_BREATH_STEP_MS) {
      g_breathAt = now;
      const uint32_t halfPeriod = LINK_BREATH_MS / 2;
      const uint32_t phase = (now - g_breathStart) % LINK_BREATH_MS;
      const uint32_t ramp = phase < halfPeriod ? phase : LINK_BREATH_MS - phase;
      g_breathLevel = static_cast<uint8_t>((ramp * 255 + halfPeriod / 2) / halfPeriod);
      ui::drawLinkSpark(g_breathLevel);
    }
  }

  if (g_haveDrawn &&
      static_cast<uint32_t>(now - g_elapsedCheckedAt) >= ELAPSED_REFRESH_MS) {
    g_elapsedCheckedAt = now;
    char elapsed[ELAPSED_TEXT_SIZE];
    snapshotElapsedText(g_snapshot, featuredNow(), g_shown, elapsed, sizeof(elapsed));
    if (strcmp(elapsed, g_elapsedDrawn) != 0) {
      const int8_t index =
          visibleSessionCount() > 0 ? g_featured : static_cast<int8_t>(-1);
      ui::drawTopBar(g_snapshot, index, g_shown);
      strncpy(g_elapsedDrawn, elapsed, sizeof(g_elapsedDrawn) - 1);
      g_elapsedDrawn[sizeof(g_elapsedDrawn) - 1] = '\0';
    }
  }
}
