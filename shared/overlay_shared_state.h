#ifndef V4L2_DRM_IMX415_OVERLAY_SHARED_STATE_H
#define V4L2_DRM_IMX415_OVERLAY_SHARED_STATE_H

#include <cstdint>
#include <vector>

// 音频线程（生产者）←→ 视频/推流线程（消费者）共享区域。
// 音频线程生成 ARGB 云图，视频线程把它拷到 DRM overlay plane 上屏。
//
// 双缓冲无锁协议：
//   pixels[2] 两槽，write_idx 指示当前"活跃槽"（消费者只读它）。
//   生产者永远写 pixels[1 - write_idx]，写完再翻转 write_idx + 递增 generation。
//   两个线程永远不会同时碰同一个槽。
//
// 原子操作用 GCC __atomic_* 内置函数，不用 std::atomic。
struct OverlaySharedState {
    std::vector<uint32_t> pixels[2];      // 双缓冲：槽 0 / 槽 1
    int width;                            // overlay 宽高，由显示线程初始化后只读
    int height;
    int write_idx;                        // 生产者写完翻转，消费者用它定位活跃槽
    uint64_t generation;                  // 生产者每次发布 +1，消费者比对它判断有无新图
    bool ready;                           // 显示线程设 true，音频线程轮询等它就绪
    bool running;                         // 全局退出标志
    bool rotate_270;                      // 视频做 270° 旋转时，云图跟着转
};

inline bool init_overlay_shared_state(OverlaySharedState *state) {
    if (!state) {
        return false;
    }
    state->width = 0;
    state->height = 0;
    state->write_idx = 0;
    state->generation = 0;
    state->ready = false;
    state->running = true;
    state->rotate_270 = false;
    return true;
}

inline void destroy_overlay_shared_state(OverlaySharedState *state) {
    (void)state;
}

#endif
