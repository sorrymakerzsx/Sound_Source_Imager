#ifndef V4L2_DRM_IMX415_AUTOFOCUS_SHARED_STATE_H
#define V4L2_DRM_IMX415_AUTOFOCUS_SHARED_STATE_H

#include <cstdint>
#include <cstring>

static const int kAfRoiWidth = 320;
static const int kAfRoiHeight = 240;
static const int kAfLogicalMax = 64;

// 视频线程（生产者）←→ AF 线程（消费者）的共享区域。
// 只传中心 320×240 ROI 的 Y 分量，不传整帧 NV12，减少拷贝量。
//
// 双缓冲无锁协议（和 overlay 一样）：
//   roi_y[2] 两槽，视频线程写 roi_y[1 - write_idx]，写完翻转 write_idx + 置 new_frame。
//   AF 线程轮询 new_frame，拿 roi_y[write_idx] 拷贝到本地再清 new_frame。
//
// restart_requested 由按键线程直接 atomic store，AF 线程下一轮查出。
//
// 原子操作用 GCC __atomic_* 内置函数，不用 std::atomic。
struct AutofocusSharedState {
    // 双缓冲 ROI：视频线程写入 roi_y[1 - write_idx]，AF 线程读取 roi_y[write_idx]。
    // 仅保存亮度分量，适合清晰度评价。
    uint8_t roi_y[2][kAfRoiWidth * kAfRoiHeight];
    int write_idx;                       // 视频线程写完后翻转
    bool new_frame;                      // 视频线程置 true，AF 消费后清 false
    bool restart_requested;              // 按键线程置 true，AF 查出后 exchange(false)
    bool running;                        // 全局退出标志
    uint64_t published_frames;           // 视频线程累计成功发布给 AF 的 ROI 帧数

    // 下面都是 AF 线程写入的只读状态，别的线程随便 load 看
    int current_focus;
    int best_focus;
    int current_step;
    int last_sharpness;
    int best_sharpness;
    int last_move_us;
    int decline_streak;
    int restart_count;
};

inline bool init_autofocus_shared_state(AutofocusSharedState *state) {
    if (!state) {
        return false;
    }
    memset(state->roi_y, 0, sizeof(state->roi_y));
    state->write_idx = 0;
    state->new_frame = false;
    state->restart_requested = false;
    state->running = true;
    state->published_frames = 0;
    state->current_focus = 32;
    state->best_focus = 32;
    state->current_step = 0;
    state->last_sharpness = 0;
    state->best_sharpness = 0;
    state->last_move_us = 0;
    state->decline_streak = 0;
    state->restart_count = 0;
    return true;
}

inline void destroy_autofocus_shared_state(AutofocusSharedState *state) {
    (void)state;
}

#endif
