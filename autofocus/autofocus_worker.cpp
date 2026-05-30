#include "autofocus_worker.h"

#include "dw9763_vcm_sim.h"

#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <linux/videodev2.h>
#include <string>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vector>

namespace {

static const int kAfInitialStep = 8;
static const int kAfDeclineRestartThreshold = 3;

struct rk_cam_vcm_tim {
    unsigned int move_us;
    unsigned int reserved;
};

#define RK_VIDIOC_VCM_TIMEINFO _IOR('V', BASE_VIDIOC_PRIVATE + 0, struct rk_cam_vcm_tim)

struct FocusControlContext {
    int fd = -1;
    std::string path;
    bool use_ioctl = false;
};

struct HillClimbState {
    int current_pos = 32;
    int step = kAfInitialStep;
    int direction = 1;
    int best_pos = 32;
    int best_sharpness = 0;
    bool has_best = false;
    int decline_streak = 0;
    int last_sharpness = 0;
    bool has_last_sharpness = false;
    int restart_count = 0;
};

// Sobel 梯度清晰度评价
static int compute_sobel_sharpness(const uint8_t *roi) {
    long long total = 0;
    for (int y = 1; y < kAfRoiHeight - 1; ++y) {
        for (int x = 1; x < kAfRoiWidth - 1; ++x) {
            int p00 = roi[(y - 1) * kAfRoiWidth + (x - 1)];
            int p01 = roi[(y - 1) * kAfRoiWidth + x];
            int p02 = roi[(y - 1) * kAfRoiWidth + (x + 1)];
            int p10 = roi[y * kAfRoiWidth + (x - 1)];
            int p12 = roi[y * kAfRoiWidth + (x + 1)];
            int p20 = roi[(y + 1) * kAfRoiWidth + (x - 1)];
            int p21 = roi[(y + 1) * kAfRoiWidth + x];
            int p22 = roi[(y + 1) * kAfRoiWidth + (x + 1)];

            int gx = -p00 + p02 - 2 * p10 + 2 * p12 - p20 + p22;
            int gy = p00 + 2 * p01 + p02 - p20 - 2 * p21 - p22;
            total += std::abs(gx) + std::abs(gy);
        }
    }
    return static_cast<int>(total / ((kAfRoiWidth - 2) * (kAfRoiHeight - 2)));
}

// 扫描 /dev/v4l-subdev* / /dev/video*，找支持 V4L2_CID_FOCUS_ABSOLUTE 的设备
static FocusControlContext open_focus_control_device() {
    FocusControlContext ctx;
    std::vector<std::string> candidates;
    for (int i = 0; i < 8; ++i) {
        candidates.push_back("/dev/v4l-subdev" + std::to_string(i));
    }
    candidates.push_back("/dev/video0");
    candidates.push_back("/dev/video1");

    for (size_t i = 0; i < candidates.size(); ++i) {
        int fd = open(candidates[i].c_str(), O_RDWR | O_NONBLOCK, 0);
        if (fd < 0) {
            continue;
        }

        struct v4l2_queryctrl query;
        std::memset(&query, 0, sizeof(query));
        query.id = V4L2_CID_FOCUS_ABSOLUTE;
        if (ioctl(fd, VIDIOC_QUERYCTRL, &query) == 0) {
            ctx.fd = fd;
            ctx.path = candidates[i];
            ctx.use_ioctl = true;
            return ctx;
        }

        close(fd);
    }

    return ctx;
}

static void close_focus_control_device(FocusControlContext *ctx) {
    if (!ctx || ctx->fd < 0) {
        return;
    }
    close(ctx->fd);
    ctx->fd = -1;
}

static bool set_focus_with_ioctl(int fd, int logical_pos) {
    struct v4l2_control ctrl;
    std::memset(&ctrl, 0, sizeof(ctrl));
    ctrl.id = V4L2_CID_FOCUS_ABSOLUTE;
    ctrl.value = logical_pos;
    return ioctl(fd, VIDIOC_S_CTRL, &ctrl) == 0;
}

// 查询 RK 私有 VCM 移动耗时
static int query_move_time_us(int fd, int fallback_us) {
    struct rk_cam_vcm_tim info;
    std::memset(&info, 0, sizeof(info));
    if (fd >= 0 && ioctl(fd, RK_VIDIOC_VCM_TIMEINFO, &info) == 0 && info.move_us > 0) {
        return static_cast<int>(info.move_us);
    }
    return fallback_us;
}

// 下发焦点：优先 V4L2 ioctl，失败回退 dw9763 模拟
static bool apply_focus_position(FocusControlContext *focus_ctx,
                                 Dw9763VcmSim *sim,
                                 int logical_pos,
                                 int *move_time_us) {
    logical_pos = std::max(0, std::min(kAfLogicalMax, logical_pos));

    if (focus_ctx && focus_ctx->use_ioctl && focus_ctx->fd >= 0) {
        if (set_focus_with_ioctl(focus_ctx->fd, logical_pos)) {
            if (move_time_us) {
                *move_time_us = query_move_time_us(focus_ctx->fd, 30000);
            }
            return true;
        }

        std::cerr << "[af] focus ioctl failed on " << focus_ctx->path
                  << ", fallback to dw9763 simulation." << std::endl;
        close_focus_control_device(focus_ctx);
        focus_ctx->use_ioctl = false;
        focus_ctx->path.clear();
    }

    if (sim) {
        if (sim->set_focus_absolute(static_cast<unsigned int>(logical_pos)) == 0) {
            if (move_time_us) {
                *move_time_us = 30000;
            }
            return true;
        }
    }

    return false;
}

// 重置爬山状态，以 anchor_pos 为起点重新搜索
static void reset_hill_climb_state(HillClimbState *hill, int anchor_pos) {
    if (!hill) {
        return;
    }

    anchor_pos = std::max(0, std::min(kAfLogicalMax, anchor_pos));
    hill->restart_count++;
    hill->current_pos = anchor_pos;
    hill->step = kAfInitialStep;
    hill->direction = (hill->restart_count % 2 == 0) ? 1 : -1;
    hill->best_pos = anchor_pos;
    hill->best_sharpness = 0;
    hill->has_best = false;
    hill->decline_streak = 0;
    hill->last_sharpness = 0;
    hill->has_last_sharpness = false;
}

// 统计连续清晰度下降的帧数，达到阈值则建议重启搜索
static bool update_decline_streak(HillClimbState *hill, int sharpness) {
    if (!hill) {
        return false;
    }

    if (hill->has_last_sharpness && sharpness < hill->last_sharpness) {
        hill->decline_streak++;
    } else {
        hill->decline_streak = 0;
    }

    hill->last_sharpness = sharpness;
    hill->has_last_sharpness = true;
    return hill->decline_streak >= kAfDeclineRestartThreshold;
}

// 爬山状态转移：sharpness 变好继续同方向，变差则反转方向并减半步长
static int update_hill_climb_state(HillClimbState *hill, int sharpness) {
    if (!hill->has_best || sharpness >= hill->best_sharpness) {
        hill->best_sharpness = sharpness;
        hill->best_pos = hill->current_pos;
        hill->has_best = true;
        hill->current_pos += hill->direction * hill->step;
    } else {
        hill->direction = -hill->direction;
        hill->step /= 2;
        if (hill->step > 0) {
            hill->current_pos += hill->direction * hill->step;
        } else {
            hill->current_pos = hill->best_pos;
        }
    }

    if (hill->current_pos < 0) {
        hill->current_pos = 0;
    }
    if (hill->current_pos > kAfLogicalMax) {
        hill->current_pos = kAfLogicalMax;
    }
    return hill->current_pos;
}

}  // namespace

// AF 线程：轮询 new_frame → 拷贝 ROI → Sobel 清晰度评价 → 爬山搜索 → 下发焦点给镜头。
// 不再用 pthread_cond_wait 阻塞等，而是 0.5ms 轮询一次 new_frame。
void run_autofocus_worker(AutofocusSharedState *state) {
    FocusControlContext focus_ctx = open_focus_control_device();
    Dw9763VcmSim vcm;
    uint8_t local_roi[kAfRoiWidth * kAfRoiHeight] = {0};
    HillClimbState hill;
    hill.current_pos = __atomic_load_n(&state->current_focus, __ATOMIC_SEQ_CST);
    hill.best_pos = hill.current_pos;

    __atomic_store_n(&state->current_step, hill.step, __ATOMIC_SEQ_CST);
    __atomic_store_n(&state->best_focus, hill.best_pos, __ATOMIC_SEQ_CST);
    __atomic_store_n(&state->decline_streak, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&state->restart_count, 0, __ATOMIC_SEQ_CST);

    if (focus_ctx.use_ioctl) {
        std::cout << "[af] using focus ioctl device: " << focus_ctx.path << std::endl;
    } else {
        std::cout << "[af] no focus ioctl device found, use dw9763 simulation." << std::endl;
    }

    int initial_move_us = 30000;
    apply_focus_position(&focus_ctx, &vcm, hill.current_pos, &initial_move_us);
    __atomic_store_n(&state->last_move_us, initial_move_us, __ATOMIC_SEQ_CST);

    while (__atomic_load_n(&state->running, __ATOMIC_SEQ_CST)) {
        // 轮询 new_frame，等视频线程发布新 ROI。
        // 有了就从 roi_y[write_idx]（活跃槽）拷贝到 local_roi，再清 new_frame。
        if (!__atomic_load_n(&state->new_frame, __ATOMIC_ACQUIRE)) {
            usleep(500);
            continue;
        }
        int idx = __atomic_load_n(&state->write_idx, __ATOMIC_ACQUIRE);
        memcpy(local_roi, state->roi_y[idx], sizeof(local_roi));
        bool restart_requested = __atomic_exchange_n(&state->restart_requested, false, __ATOMIC_ACQ_REL);
        __atomic_store_n(&state->new_frame, false, __ATOMIC_RELEASE);

        int sharpness = compute_sobel_sharpness(local_roi);
        __atomic_store_n(&state->last_sharpness, sharpness, __ATOMIC_SEQ_CST);

        bool decline_restart = false;
        if (!restart_requested) {
            decline_restart = update_decline_streak(&hill, sharpness);
        }

        if (restart_requested || decline_restart) {
            const int restart_anchor = __atomic_load_n(&state->current_focus, __ATOMIC_SEQ_CST);
            reset_hill_climb_state(&hill, restart_anchor);
            hill.last_sharpness = sharpness;
            hill.has_last_sharpness = true;

            std::cout << "[af] restart hill climb: reason="
                      << (restart_requested ? "key" : "sharpness-decline")
                      << " anchor=" << restart_anchor
                      << " sharpness=" << sharpness
                      << " reset_count=" << hill.restart_count
                      << std::endl;
        }

        int next_pos = update_hill_climb_state(&hill, sharpness);
        __atomic_store_n(&state->best_focus, hill.best_pos, __ATOMIC_SEQ_CST);
        __atomic_store_n(&state->current_step, hill.step, __ATOMIC_SEQ_CST);
        __atomic_store_n(&state->current_focus, next_pos, __ATOMIC_SEQ_CST);
        __atomic_store_n(&state->best_sharpness, hill.best_sharpness, __ATOMIC_SEQ_CST);
        __atomic_store_n(&state->decline_streak, hill.decline_streak, __ATOMIC_SEQ_CST);
        __atomic_store_n(&state->restart_count, hill.restart_count, __ATOMIC_SEQ_CST);

        int move_time_us = 30000;
        apply_focus_position(&focus_ctx, &vcm, next_pos, &move_time_us);
        __atomic_store_n(&state->last_move_us, move_time_us, __ATOMIC_SEQ_CST);

        std::cout << "[af] sharpness=" << sharpness
                  << " current=" << next_pos
                  << " best=" << hill.best_pos
                  << " best_sharpness=" << hill.best_sharpness
                  << " step=" << hill.step
                  << " dir=" << hill.direction
                  << " move_us=" << move_time_us
                  << " backend=" << (focus_ctx.use_ioctl ? focus_ctx.path : std::string("dw9763-sim"))
                  << std::endl;

        if (move_time_us > 0) {
            usleep(static_cast<useconds_t>(move_time_us));
        }
    }

    close_focus_control_device(&focus_ctx);
}
