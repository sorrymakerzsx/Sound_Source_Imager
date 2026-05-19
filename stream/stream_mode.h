#ifndef V4L2_DRM_IMX415_STREAM_MODE_H
#define V4L2_DRM_IMX415_STREAM_MODE_H

#include "common.h"

struct OverlaySharedState;

// 功能：
//   运行推流模式主流程：V4L2 采集、可选云图叠加、MPP 编码、UDP/TCP 发送。
// 参数：
//   config: 应用运行配置，包含协议、目标地址和端口。
//   state: 可选的云图共享状态；非空时会在编码前把 overlay 混到 NV12 帧里。
// 返回值：
//   0  表示正常结束；
//   -1 表示初始化、采集、编码或发送失败。
int run_stream_mode(const AppConfig &config, OverlaySharedState *state = nullptr);

#endif
