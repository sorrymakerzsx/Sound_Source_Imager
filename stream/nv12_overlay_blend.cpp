#include "nv12_overlay_blend.h"

#include <algorithm>

namespace {

static inline uint8_t clamp_u8(int value) {
    if (value < 0) {
        return 0;
    }
    if (value > 255) {
        return 255;
    }
    return static_cast<uint8_t>(value);
}

// alpha 合成一个通道：out = dst*(1-alpha) + src*alpha
static inline uint8_t blend_channel(uint8_t dst, uint8_t src, uint8_t alpha) {
    int out = (static_cast<int>(dst) * (255 - alpha) + static_cast<int>(src) * alpha + 127) / 255;
    return clamp_u8(out);
}

// ITU-R BT.601 RGB→YCbCr 矩阵系数
static inline uint8_t rgb_to_y(uint8_t r, uint8_t g, uint8_t b) {
    int value = (77 * r + 150 * g + 29 * b + 128) >> 8;
    return clamp_u8(value);
}

static inline uint8_t rgb_to_u(uint8_t r, uint8_t g, uint8_t b) {
    int value = ((-43 * r - 85 * g + 128 * b + 128) >> 8) + 128;
    return clamp_u8(value);
}

static inline uint8_t rgb_to_v(uint8_t r, uint8_t g, uint8_t b) {
    int value = ((128 * r - 107 * g - 21 * b + 128) >> 8) + 128;
    return clamp_u8(value);
}

}  // namespace

// 将 ARGB overlay alpha 混合到 NV12 帧，用于推流模式编码前处理。
// NV12 的 UV 以 2x2 块为单位，对 2x2 块内像素取平均再转换。
void blend_argb_overlay_to_nv12(uint8_t *nv12,
                                int width,
                                int height,
                                int stride,
                                const std::vector<uint32_t> &overlay_pixels,
                                int overlay_width,
                                int overlay_height) {
    if (!nv12 || overlay_pixels.empty() || overlay_width != width || overlay_height != height) {
        return;
    }

    uint8_t *y_plane = nv12;
    uint8_t *uv_plane = nv12 + stride * height;

    for (int y = 0; y < height; y += 2) {
        for (int x = 0; x < width; x += 2) {
            int sum_alpha = 0;
            int sum_r = 0;
            int sum_g = 0;
            int sum_b = 0;

            // Y 分量逐像素混合
            for (int dy = 0; dy < 2; ++dy) {
                for (int dx = 0; dx < 2; ++dx) {
                    int px = x + dx;
                    int py = y + dy;
                    if (px >= width || py >= height) {
                        continue;
                    }

                    uint32_t argb = overlay_pixels[static_cast<size_t>(py) * overlay_width + px];
                    uint8_t alpha = static_cast<uint8_t>((argb >> 24) & 0xFF);
                    if (alpha == 0) {
                        continue;
                    }

                    uint8_t r = static_cast<uint8_t>((argb >> 16) & 0xFF);
                    uint8_t g = static_cast<uint8_t>((argb >> 8) & 0xFF);
                    uint8_t b = static_cast<uint8_t>(argb & 0xFF);

                    y_plane[py * stride + px] =
                        blend_channel(y_plane[py * stride + px], rgb_to_y(r, g, b), alpha);

                    sum_alpha += alpha;
                    sum_r += r;
                    sum_g += g;
                    sum_b += b;
                }
            }

            if (sum_alpha == 0) {
                continue;
            }

            // UV 分量对 2x2 块取平均后混合（NV12 色度下采样）
            int samples = std::min(4, (width - x) * (height - y));
            if (samples <= 0) {
                continue;
            }
            uint8_t avg_alpha = static_cast<uint8_t>(sum_alpha / samples);
            uint8_t avg_r = static_cast<uint8_t>(sum_r / samples);
            uint8_t avg_g = static_cast<uint8_t>(sum_g / samples);
            uint8_t avg_b = static_cast<uint8_t>(sum_b / samples);

            int uv_index = (y / 2) * stride + x;
            uv_plane[uv_index] = blend_channel(uv_plane[uv_index], rgb_to_u(avg_r, avg_g, avg_b), avg_alpha);
            if (x + 1 < stride) {
                uv_plane[uv_index + 1] =
                    blend_channel(uv_plane[uv_index + 1], rgb_to_v(avg_r, avg_g, avg_b), avg_alpha);
            }
        }
    }
}
