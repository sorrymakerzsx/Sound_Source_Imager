#ifndef V4L2_DRM_IMX415_OVERLAY_SHARED_STATE_H
#define V4L2_DRM_IMX415_OVERLAY_SHARED_STATE_H

#include <atomic>
#include <cstdint>
#include <pthread.h>
#include <vector>

// 云图线程与显示线程之间的共享区域。
// 当前实现里，云图线程只负责生成整张 ARGB overlay，
// 视频线程再把最新一版 overlay 拷到 DRM overlay buffer 上。
struct OverlaySharedState {
    // 保护 pixels/width/height/generation/ready 这些共享字段的互斥锁。
    pthread_mutex_t mutex;
    // 用于“尺寸已准备好”等事件通知的条件变量。
    pthread_cond_t condition;
    // 整张 ARGB overlay 的 CPU 像素数据。
    std::vector<uint32_t> pixels;
    // overlay 实际显示尺寸，和 DRM overlay plane 目标尺寸一致。
    int width = 0;
    int height = 0;
    // generation 用于判断“有没有新图”，避免视频线程重复拷贝同一张 overlay。
    uint64_t generation = 0;
    // ready 表示 DRM 线程已经把 overlay 尺寸准备好了，云图线程可以开始出图。
    bool ready = false;
    // 线程总运行标志，主线程退出时会把它清为 false。
    std::atomic<bool> running{true};
    // 视频 plane 如果被设置了 270 度旋转，云图也要跟着同样的显示方向做坐标映射。
    std::atomic<bool> rotate_270{false};
};

// 功能：
//   初始化云图共享状态中的 POSIX 锁与条件变量。
// 参数：
//   state: 待初始化的共享状态对象。
// 返回值：
//   true  表示初始化成功；
//   false 表示初始化失败。
inline bool init_overlay_shared_state(OverlaySharedState *state) {
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
//   销毁云图共享状态中的 POSIX 锁与条件变量。
// 参数：
//   state: 待销毁的共享状态对象。
// 返回值：
//   无返回值。
inline void destroy_overlay_shared_state(OverlaySharedState *state) {
    if (!state) {
        return;
    }
    pthread_cond_destroy(&state->condition);
    pthread_mutex_destroy(&state->mutex);
}

#endif
