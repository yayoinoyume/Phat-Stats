// 绘制层：左侧状态卡 + 右侧竖栏。只做渲染，不读串口、不管理状态。
#pragma once

#include <stdint.h>

#include "snapshot.h"

namespace ui {

void begin();

// 全量重绘。featuredIndex < 0 表示当前没有可显示会话。
void drawFull(const Snapshot& snapshot, int8_t featuredIndex, PanelState shown);

// 只重绘顶部 elapsed 区域（不触碰状态词/标题），用于秒级计时更新。
void drawTopBar(const Snapshot& snapshot, int8_t featuredIndex, PanelState shown);

// 只重绘 spinner 方块，供 80ms 动画使用。
void drawSpinner(uint8_t step);

// 局部动画：working 扫描线、needs_you 圆点脉冲、quiet 冒号、link_lost 呼吸。
void drawWorkingScan(uint8_t phase);
void drawDotPulse(const Snapshot& snapshot, int8_t featuredIndex, uint8_t ringRadius);
void drawQuietClock(const Snapshot& snapshot, bool colonOn);
void drawLinkSpark(uint8_t level);

}  // namespace ui
