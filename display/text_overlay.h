#ifndef V4L2_DRM_IMX415_TEXT_OVERLAY_H
#define V4L2_DRM_IMX415_TEXT_OVERLAY_H

#include "common.h"

// 功能：
//   用统一颜色清空一张 ARGB 图层缓冲区。
// 参数：
//   buf: 目标 dumb buffer。
//   width/height: 需要清空的有效区域大小。
//   color: ARGB8888 填充值。
// 返回值：
//   无返回值。
void clear_argb(DumbBuffer *buf, int width, int height, uint32_t color);

// 功能：
//   从指定坐标开始在 ARGB 图层上绘制字符串。
// 参数：
//   buf: 目标 dumb buffer。
//   width/height: 有效绘制区域大小。
//   origin_x/origin_y: 字符串左上角起始坐标。
//   str: 待绘制字符串。
//   fg_color: 字体前景色，ARGB8888 格式。
// 返回值：
//   无返回值。
void draw_text_at(DumbBuffer *buf, int width, int height, int origin_x, int origin_y,
                  const char *str, uint32_t fg_color);

// 功能：
//   以默认样式快速绘制一行文本。
// 参数：
//   buf: 目标 dumb buffer。
//   width/height: 有效绘制区域大小。
//   str: 待显示字符串。
// 返回值：
//   无返回值。
void draw_text(DumbBuffer *buf, int width, int height, const char *str);

#endif
