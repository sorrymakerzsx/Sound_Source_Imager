#ifndef V4L2_DRM_IMX415_AUTOFOCUS_SHARED_STATE_H
#define V4L2_DRM_IMX415_AUTOFOCUS_SHARED_STATE_H

#include <array>
#include <atomic>
#include <cstdint>
#include <pthread.h>

static const int kAfRoiWidth = 320;
static const int kAfRoiHeight = 240;
static const int kAfLogicalMax = 64;

// 视频线程与 AF 线程之间的共享区域。
// 只共享一块中心 ROI 的 Y 分量，而不是整帧 NV12，
// 这样拷贝量小，且 AF 线程可以只针对 Sobel 所需的数据工作。
struct AutofocusSharedState {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    // 共享 ROI，仅保存亮度分量，适合清晰度评价。
    std::array<uint8_t, kAfRoiWidth * kAfRoiHeight> roi_y{};
    // new_frame = 1 表示视频线程刚发布了一块新 ROI；
    // AF 线程复制走 ROI 后会把它清 0。
    bool new_frame = false;
    bool running = true;
    uint64_t published_frames = 0;
    // 下面这些值主要用于观察 AF 搜索状态。
    std::atomic<int> current_focus{32};
    std::atomic<int> best_focus{32};
    std::atomic<int> current_step{0};
    std::atomic<int> last_sharpness{0};
    std::atomic<int> best_sharpness{0};
    std::atomic<int> last_move_us{0};
};

// 功能：
//   初始化 AF 共享状态中的 POSIX 锁与条件变量。
// 参数：
//   state: 待初始化的共享状态对象。
// 返回值：
//   true  表示初始化成功；
//   false 表示初始化失败。
inline bool init_autofocus_shared_state(AutofocusSharedState *state) {
    if (!state) {
        return false;
    }
    if (pthread_mutex_init(&state->mutex, nullptr) != 0) {
        return false;
    }
    if (pthread_cond_init(&state->condition, nullptr) != 0) {
        pthread_mutex_destroy(&state->mutex);
        return false;
    }
    return true;
}

// 功能：
//   销毁 AF 共享状态中的 POSIX 锁与条件变量。
// 参数：
//   state: 待销毁的共享状态对象。
// 返回值：
//   无返回值。
inline void destroy_autofocus_shared_state(AutofocusSharedState *state) {
    if (!state) {
        return;
    }
    pthread_cond_destroy(&state->condition);
    pthread_mutex_destroy(&state->mutex);
}

#endif
