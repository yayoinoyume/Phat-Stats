// Lody Panel 硬件与布局配置。
// 坐标以 05-ui.md 的 320x240 横屏（rotation 3）为基准。
#pragma once

#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// 功能开关
// ---------------------------------------------------------------------------
// GPIO34 电池分压默认不读取：未接分压时浮空会读出噪声，右栏只显示 USB。
#define ENABLE_BATTERY 0
// 板载 16 颗 NeoPixel 默认关闭，避免占用 RMT 或干扰主显示。
#define ENABLE_NEOPIXEL 0
// 打开后会把解析失败的行打印到串口；正常使用保持关闭，避免阻塞 loop。
#define DEBUG_SERIAL 0

// ---------------------------------------------------------------------------
// 屏幕与引脚（见 03-hardware.md）
// ---------------------------------------------------------------------------
constexpr uint8_t TFT_CS = 17;
constexpr uint8_t TFT_DC = 16;
constexpr uint8_t TFT_RST = 15;
constexpr uint8_t TFT_SCLK = 18;
constexpr uint8_t TFT_MOSI = 23;
constexpr uint8_t TFT_MISO = 19;  // 屏上没有 MISO，仅占位
constexpr uint8_t TFT_BL = 4;     // 背光，active HIGH
constexpr uint8_t SCREEN_ROTATION = 3;

constexpr int16_t SCREEN_W = 320;
constexpr int16_t SCREEN_H = 240;

constexpr uint8_t NEOPIXEL_PIN = 2;
constexpr uint16_t NEOPIXEL_COUNT = 16;

constexpr uint8_t BATTERY_PIN = 34;
constexpr float BATTERY_DIVIDER = 1.8f;  // 100k/100k 分压的实测系数

// ---------------------------------------------------------------------------
// 串口 / 协议（见 02-protocol.md）
// ---------------------------------------------------------------------------
constexpr uint32_t SERIAL_BAUD = 115200;
constexpr size_t LINE_BUFFER_SIZE = 2048;  // 一行 JSON 的上限，不含结尾换行
constexpr size_t JSON_POOL_SIZE = 16384;   // ArduinoJson 的静态解析池
constexpr uint8_t MAX_SESSIONS = 8;

constexpr uint32_t LINK_TIMEOUT_MS = 20000;
constexpr uint32_t SESSION_ROTATE_MS = 14000;
constexpr uint32_t SPINNER_STEP_MS = 80;
constexpr uint32_t ELAPSED_REFRESH_MS = 250;
constexpr uint32_t WORK_SCAN_STEP_MS = 80;
constexpr uint32_t DOT_PULSE_STEP_MS = 50;
constexpr uint32_t DOT_PULSE_PERIOD_MS = 600;
constexpr uint32_t QUIET_COLON_MS = 1000;
constexpr uint32_t LINK_BREATH_MS = 800;
constexpr uint32_t LINK_BREATH_STEP_MS = 50;

constexpr size_t MAX_TITLE_BYTES = 96;
constexpr size_t MAX_PROJECT_BYTES = 24;
constexpr size_t SESSION_ID_BYTES = 8;
constexpr size_t MAX_PROVIDER_BYTES = 11;
constexpr size_t MAX_MODEL_BYTES = 15;

// bridge 渲染的标题位图：208x44、1-bit、按行 26 字节。
constexpr uint16_t TITLE_BITMAP_W = 208;
constexpr uint16_t TITLE_BITMAP_H = 44;
constexpr uint16_t TITLE_BITMAP_STRIDE = (TITLE_BITMAP_W + 7) / 8;
constexpr uint16_t TITLE_BITMAP_BYTES = TITLE_BITMAP_STRIDE * TITLE_BITMAP_H;
constexpr uint8_t TITLE_CACHE_SLOTS = 8;

constexpr size_t TITLE_BUFFER_SIZE = MAX_TITLE_BYTES + 1;
constexpr size_t PROJECT_BUFFER_SIZE = MAX_PROJECT_BYTES + 1;
constexpr size_t SESSION_ID_BUFFER_SIZE = SESSION_ID_BYTES + 1;
constexpr size_t PROVIDER_BUFFER_SIZE = MAX_PROVIDER_BYTES + 1;
constexpr size_t MODEL_BUFFER_SIZE = MAX_MODEL_BYTES + 1;
constexpr size_t TIME_BUFFER_SIZE = 6;   // "21:10"
constexpr size_t DATE_BUFFER_SIZE = 6;   // "11-02"
constexpr size_t TITLE_LINE_BUFFER = 64; // 单行标题缓存（UTF-8 字节）
constexpr size_t ELAPSED_TEXT_SIZE = 24;

// ---------------------------------------------------------------------------
// 颜色（RGB565）
// ---------------------------------------------------------------------------
constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

constexpr uint16_t COLOR_BG = rgb565(0x0E, 0x10, 0x0F);
constexpr uint16_t COLOR_CREAM = rgb565(0xFF, 0xFC, 0xE1);
constexpr uint16_t COLOR_PURPLE = rgb565(0x9D, 0x95, 0xFF);
constexpr uint16_t COLOR_GREEN = rgb565(0xAB, 0xFF, 0x84);
constexpr uint16_t COLOR_ORANGE = rgb565(0xFF, 0x87, 0x09);
constexpr uint16_t COLOR_RED = rgb565(0xFF, 0x5F, 0x57);
constexpr uint16_t COLOR_RED_DIM = rgb565(0x4A, 0x18, 0x16);
constexpr uint16_t COLOR_GRAY = rgb565(0x96, 0x96, 0x8A);
constexpr uint16_t COLOR_TOPBAR = rgb565(0xCF, 0xCC, 0xB4);
constexpr uint16_t COLOR_LINE = rgb565(0x1B, 0x1E, 0x1A);
constexpr uint16_t COLOR_DOT_IDLE = rgb565(0x2A, 0x2D, 0x28);
constexpr uint16_t COLOR_SPINNER_DIM = rgb565(0x3F, 0x3B, 0x6B);
constexpr uint16_t COLOR_SPINNER_MID = rgb565(0x6B, 0x65, 0xB3);

// ---------------------------------------------------------------------------
// 左侧 240x240 状态卡
// ---------------------------------------------------------------------------
constexpr int16_t CARD_W = 240;
constexpr int16_t RAIL_X = CARD_W;

constexpr int16_t TOPBAR_X = 16;
constexpr int16_t TOPBAR_BASELINE = 24;
constexpr int16_t TOPBAR_HEIGHT = 32;
constexpr int16_t SPARK_CX = 22;
constexpr int16_t SPARK_CY = 20;
constexpr int16_t SPARK_R = 6;

constexpr int16_t DOT_CY = 20;
constexpr int16_t DOT_R = 2;
constexpr int16_t DOT_GAP = 10;
constexpr int16_t DOT_LIMIT = 6;
constexpr int16_t DOT_RIGHT = 224;
constexpr int16_t DOT_RING_RADIUS = DOT_R + 2;
constexpr int16_t DOT_RING_MAX = DOT_R + 4;

constexpr int16_t STATUS_GLYPH_X1 = 14;
constexpr int16_t STATUS_GLYPH_X2 = 74;
constexpr int16_t STATUS_GLYPH_TOP = 56;
constexpr int16_t STATUS_EN_X = 16;
constexpr int16_t STATUS_EN_BASELINE = 132;

constexpr int16_t PROVIDER_ICON_X = 16;
constexpr int16_t PROVIDER_ICON_Y = 146;
constexpr int16_t PROVIDER_ICON_SIZE = 16;
constexpr int16_t PROVIDER_TEXT_X = 36;
constexpr int16_t PROVIDER_TEXT_BASELINE = 158;
constexpr int16_t PROVIDER_TEXT_RIGHT = 224;

constexpr int16_t SCAN_X = 16;
constexpr int16_t SCAN_Y = 136;
constexpr int16_t SCAN_WIDTH = 208;
constexpr int16_t SCAN_HEIGHT = 2;
constexpr int16_t SCAN_SEGMENT = 64;
constexpr int16_t SCAN_STEP = 8;
constexpr uint8_t SCAN_PHASES =
    static_cast<uint8_t>((SCAN_WIDTH - SCAN_SEGMENT) / SCAN_STEP + 1);

constexpr int16_t SPINNER_CX = 196;
constexpr int16_t SPINNER_CY = 96;
constexpr int16_t SPINNER_R = 22;
constexpr int16_t SPINNER_RIN = 11;

constexpr int16_t TITLE_X = 16;
// 166/196 是文本顶部；绘制时加 wqy16 的 ascent 得到基线。
constexpr int16_t TITLE_TOP_1 = 166;
constexpr int16_t TITLE_TOP_2 = 196;
constexpr int16_t TITLE_MAX_W = 208;
constexpr int16_t QUIET_CLOCK_BASELINE = 182;
constexpr int16_t QUIET_CLOCK_TOP = 154;
constexpr int16_t QUIET_CLOCK_WIDTH = 100;
constexpr int16_t QUIET_CLOCK_HEIGHT = 38;

constexpr int16_t META_X = 16;
// 214 是文本顶部；FreeSans9pt7b ascent 12，故基线 226。
constexpr int16_t META_TOP = 214;
constexpr int16_t META_BASELINE = META_TOP + 12;  // FreeSans9pt7b ascent
constexpr int16_t META_MAX_W = 208;

// ---------------------------------------------------------------------------
// 右侧 80x240 竖栏
// ---------------------------------------------------------------------------
constexpr int16_t RAIL_TEXT_X = 250;
constexpr int16_t RAIL_MARK_BASELINE = 26;
constexpr int16_t RAIL_CLOCK_BASELINE = 54;
constexpr int16_t RAIL_DATE_BASELINE = 70;
constexpr int16_t RAIL_COUNT_BASELINE = 100;
constexpr int16_t RAIL_COUNT_GAP = 16;
constexpr int16_t RAIL_COUNT_NUMBER_RIGHT = 264;
constexpr int16_t RAIL_COUNT_LABEL_X = 270;

constexpr int16_t RAIL_BATTERY_BARS_X = 250;
constexpr int16_t RAIL_BATTERY_BARS_Y = 218;
constexpr int16_t RAIL_BATTERY_BAR_W = 5;
constexpr int16_t RAIL_BATTERY_BAR_H = 9;
constexpr int16_t RAIL_BATTERY_BAR_GAP = 2;
constexpr int16_t RAIL_BATTERY_LABEL_X = 282;
constexpr int16_t RAIL_BATTERY_BASELINE = 226;
