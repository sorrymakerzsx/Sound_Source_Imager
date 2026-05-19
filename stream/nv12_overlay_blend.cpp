#include "nv12_overlay_blend.h"

#include <algorithm>

namespace {

// 功能：
//   把整数裁剪到 8 位无符号像素范围。
// 参数：
//   value: 待裁剪数值。
// 返回值：
//   [0, 255] 范围内的 uint8_t。
static inline uint8_t clamp_u8(int value) {
    if (value < 0) {
        return 0;
    }
    if (value > 255) {
        return 255;
    }
    return static_cast<uint8_t>(value);
}

// 功能：
//   用 alpha 把一个颜色通道混合到目标通道中。
// 参数：
//   dst: 目标通道原值。
//   src: 源通道值。
//   alpha: 源像素 alpha，范围 0~255。
// 返回值：
//   混合后的通道值。
static inline uint8_t blend_channel(uint8_t dst, uint8_t src, uint8_t alpha) {
    int out = (static_cast<int>(dst) * (255 - alpha) + static_cast<int>(src) * alpha + 127) / 255;
    return clamp_u8(out);
}

// 功能：
//   把 RGB 转换为 NV12 所需的 Y 分量。
// 参数：
//   r/g/b: RGB 三个通道。
// 返回值：
//   对应的亮度值 Y。
static inline uint8_t rgb_to_y(uint8_t r, uint8_t g, uint8_t b) {
    int value = (77 * r + 150 * g + 29 * b + 128) >> 8;
    return clamp_u8(value);
}

// 功能：
//   把 RGB 转换为 NV12 所需的 U 分量。
// 参数：
//   r/g/b: RGB 三个通道。
// 返回值：
//   对应的色度 U。
static inline uint8_t rgb_to_u(uint8_t r, uint8_t g, uint8_t b) {
    int value = ((-43 * r - 85 * g + 128 * b + 128) >> 8) + 128;
    return clamp_u8(value);
}

// 功能：
//   把 RGB 转换为 NV12 所需的 V 分量。
// 参数：
//   r/g/b: RGB 三个通道。
// 返回值：
//   对应的色度 V。
static inline uint8_t rgb_to_v(uint8_t r, uint8_t g, uint8_t b) {
    int value = ((128 * r - 107 * g - 21 * b + 128) >> 8) + 128;
    return clamp_u8(value);
}

}  // namespace

// 功能：
//   把一张 ARGB overlay 按 alpha 混合到 NV12 图像上。
//   这个函数用于 stream 模式：先把云图叠到 NV12，再送给 MPP 编码。
// 参数：
//   nv12: 目标 NV12 帧首地址，内部包含连续的 Y 平面和 UV 平面。
//   width/height: NV12 图像有效尺寸。
//   stride: NV12 每行字节跨度。
//   overlay_pixels: ARGB8888 overlay 像素数组。
//   overlay_width/overlay_height: overlay 尺寸，必须与目标图像一致。
// 返回值：
//   无返回值；若输入为空或尺寸不匹配则直接返回，不做叠加。
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
