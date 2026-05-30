#include "text_overlay.h"

#include "font.h"

#include <cstdint>

namespace {

// pitch 是字节跨度，这里是像素跨度
static inline int argb_stride(const DumbBuffer *buf) {
    return static_cast<int>(buf->pitch / sizeof(uint32_t));
}

static inline uint32_t &pixel_at(DumbBuffer *buf, int x, int y) {
    return buf->pixels[y * argb_stride(buf) + x];
}

}  // namespace

void clear_argb(DumbBuffer *buf, int width, int height, uint32_t color) {
    int stride = argb_stride(buf);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            buf->pixels[y * stride + x] = color;
        }
    }
}

// 用 8x8 位图字体（2x 缩放 → 16x16）在 ARGB 缓冲上渲染文本。
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

void draw_text(DumbBuffer *buf, int width, int height, const char *str) {
    clear_argb(buf, width, height, 0xFFFFFFFF);
    draw_text_at(buf, width, height, 10, 8, str, 0xFF000000);
}
