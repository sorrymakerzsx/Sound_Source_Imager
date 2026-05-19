#ifndef V4L2_DRM_IMX415_NV12_OVERLAY_BLEND_H
#define V4L2_DRM_IMX415_NV12_OVERLAY_BLEND_H

#include <cstdint>
#include <vector>

// 功能：
//   把一张 ARGB overlay 按 alpha 混合到 NV12 帧中。
// 参数：
//   nv12: 目标 NV12 帧首地址，内存布局为 Y 平面后接 UV 平面。
//   width/height: NV12 图像尺寸。
//   stride: NV12 每行字节跨度。
//   overlay_pixels: ARGB8888 overlay 像素数组。
//   overlay_width/overlay_height: overlay 图尺寸，应与目标图像一致。
// 返回值：
//   无返回值；若输入为空或尺寸不匹配则直接返回。
void blend_argb_overlay_to_nv12(uint8_t *nv12,
                                int width,
                                int height,
                                int stride,
                                const std::vector<uint32_t> &overlay_pixels,
                                int overlay_width,
                                int overlay_height);

#endif
