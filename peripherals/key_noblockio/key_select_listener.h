#ifndef V4L2_DRM_IMX415_KEY_SELECT_LISTENER_H
#define V4L2_DRM_IMX415_KEY_SELECT_LISTENER_H

struct AutofocusSharedState;

// 功能：
//   按 14_noblockio 的思路，在独立线程中通过 select 监听按键设备节点，
//   检测到“按下”事件后请求 AF 线程重新对焦。
// 参数：
//   state: AF 共享状态；用于设置 restart_requested 并唤醒 AF 线程。
// 返回值：
//   无返回值；线程会在 state->running 变为 false 后退出。
void run_key_select_listener(AutofocusSharedState *state);

#endif
