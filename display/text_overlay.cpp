#include "text_overlay.h"

#include "font.h"

#include <cstdint>

namespace {

// 功能：
//   计算 ARGB 缓冲区一行有多少个 32 位像素。
// 参数：
//   buf: 目标 dumb buffer。
// 返回值：
//   以像素为单位的行跨度，而不是字节数。
static inline int argb_stride(const DumbBuffer *buf) {
    return static_cast<int>(buf->pitch / sizeof(uint32_t));
}

// 功能：
//   返回指定像素位置的引用，便于直接读写 ARGB 像素。
// 参数：
//   buf: 目标 dumb buffer。
//   x/y: 像素坐标。
// 返回值：
//   指向指定像素的引用。
static inline uint32_t &pixel_at(DumbBuffer *buf, int x, int y) {
    return buf->pixels[y * argb_stride(buf) + x];
}

}  // namespace

// 功能：
//   用统一颜色清空一张 ARGB 图。
// 参数：
//   buf: 目标 dumb buffer。
//   width/height: 有效绘制区域大小。
//   color: ARGB8888 颜色值。
// 返回值：
//   无返回值。
void clear_argb(DumbBuffer *buf, int width, int height, uint32_t color) {
    int stride = argb_stride(buf);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            buf->pixels[y * stride + x] = color;
        }
    }
}

// 功能：
//   在 ARGB 图上从指定起点绘制字符串。
// 参数：
//   buf: 目标 dumb buffer。
//   width/height: 有效绘制区域大小。
//   origin_x/origin_y: 文本左上角起始位置。
//   str: 要绘制的字符串。
//   fg_color: 前景色，ARGB8888 格式。
// 返回值：
//   无返回值。
void draw_text_at(DumbBuffer *buf, int width, int height, int origin_x, int origin_y,
                  const char *str, uint32_t fg_color) {
    int cursor_x = origin_x;
    int cursor_y = origin_y;
    while (*str) {
        char c = *str++;
        if (c < 32 || c > 127) {
            c = '?';
        }

        const uint8_t *glyph = font8x8[c - 32];
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 8; ++x) {
                if (glyph[y] & (1 << (7 - x))) {
                    int px = cursor_x + x * 2;
                    int py = cursor_y + y * 2;
                    if (px < width - 1 && py < height - 1) {
                        pixel_at(buf, px, py) = fg_color;
                        pixel_at(buf, px + 1, py) = fg_color;
                        pixel_at(buf, px, py + 1) = fg_color;
                        pixel_at(buf, px + 1, py + 1) = fg_color;
                    }
                }
            }
        }

        cursor_x += 16;
        if (cursor_x + 16 > width) {
            break;
        }
    }
}

// 功能：
//   以“白底黑字”的方式快速绘制一行文本。
// 参数：
//   buf: 目标 dumb buffer。
//   width/height: 有效绘制区域大小。
//   str: 要显示的字符串。
// 返回值：
//   无返回值。
void draw_text(DumbBuffer *buf, int width, int height, const char *str) {
    clear_argb(buf, width, height, 0xFFFFFFFF);
    draw_text_at(buf, width, height, 10, 8, str, 0xFF000000);
}
