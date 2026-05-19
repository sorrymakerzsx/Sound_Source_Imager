#ifndef V4L2_DRM_IMX415_AUTOFOCUS_WORKER_H
#define V4L2_DRM_IMX415_AUTOFOCUS_WORKER_H

#include "autofocus_shared_state.h"

// 功能：
//   AF 线程入口，等待新 ROI、计算清晰度并驱动 dw9763 模拟焦点更新。
// 参数：
//   state: AF 共享状态，包含 ROI、条件变量、运行标志和焦点统计信息。
// 返回值：
//   无返回值；线程在 state->running 变为 false 后退出。
void run_autofocus_worker(AutofocusSharedState *state);

#endif
