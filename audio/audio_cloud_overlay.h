#ifndef V4L2_DRM_IMX415_AUDIO_CLOUD_OVERLAY_H
#define V4L2_DRM_IMX415_AUDIO_CLOUD_OVERLAY_H

#include "common.h"
#include "overlay_shared_state.h"

// 功能：
//   生成一张声源热点 ARGB overlay 图。
// 参数：
//   buf: 目标 dumb buffer。
//   width/height: overlay 尺寸。
//   fps: 预留给动态效果或调试显示的帧率参数，当前主要用于接口兼容。
//   rotate_270: 若视频最终显示做了 270 度旋转，则同步修正热点坐标。
// 返回值：
//   无返回值。
void draw_sound_source_overlay(DumbBuffer *buf, int width, int height, float fps, bool rotate_270);

// 功能：
//   云图线程入口，周期性生成最新 ARGB 声源热点并写入共享状态。
// 参数：
//   state: 云图共享状态，包含尺寸、像素缓存、代际号和运行标志。
// 返回值：
//   无返回值；当 state->running 为 false 时线程退出。
void run_audio_overlay_worker(OverlaySharedState *state);

#endif
