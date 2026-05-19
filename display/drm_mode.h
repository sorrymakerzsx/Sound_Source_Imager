#ifndef V4L2_DRM_IMX415_DRM_MODE_H
#define V4L2_DRM_IMX415_DRM_MODE_H

// 这里只前向声明共享状态，避免头文件之间相互包含过深。
struct OverlaySharedState;
struct AutofocusSharedState;

// 功能：
//   运行 DRM 模式的视频显示主线程。
//   主要完成 V4L2 取帧、DRM/KMS 初始化、NV12 视频 plane 显示、
//   ARGB overlay plane 叠加，以及中心 ROI 发布给 AF 线程。
// 参数：
//   state: 云图共享状态；若非空，则读取最新 overlay 图层并显示。
//   af_state: AF 共享状态；若非空，则发布中心 ROI 给 AF 线程。
// 返回值：
//   0  表示正常结束；
//   -1 表示初始化或显示过程中出现错误。
int run_video_display_worker(OverlaySharedState *state, AutofocusSharedState *af_state);

#endif
