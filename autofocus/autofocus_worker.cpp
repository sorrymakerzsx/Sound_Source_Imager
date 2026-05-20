#include "autofocus_worker.h"

#include "dw9763_vcm_sim.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <linux/videodev2.h>
#include <pthread.h>
#include <string>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vector>

namespace {

static const int kAfInitialStep = 8;
static const int kAfDeclineRestartThreshold = 3;

struct rk_cam_vcm_tim {
    // 驱动返回给应用层的本次马达移动耗时，单位微秒。
    unsigned int move_us;
    // 预留字段，方便后续扩展其他时间信息。
    unsigned int reserved;
};

#define RK_VIDIOC_VCM_TIMEINFO _IOR('V', BASE_VIDIOC_PRIVATE + 0, struct rk_cam_vcm_tim)

struct FocusControlContext {
    // 已打开的真实 focus 控制设备 fd；找不到时为 -1。
    int fd = -1;
    // 当前选中的设备节点路径，便于日志打印。
    std::string path;
    // 是否成功找到了支持 V4L2_CID_FOCUS_ABSOLUTE 的真实设备。
    bool use_ioctl = false;
};

struct HillClimbState {
    // 当前焦点位置，也是本次 sharpness 对应的位置。
    int current_pos = 32;
    // 当前爬山步长；越小表示已进入精细搜索阶段。
    int step = kAfInitialStep;
    // 搜索方向，1 表示朝增大方向，-1 表示朝减小方向。
    int direction = 1;
    // 历史上取得最佳清晰度时对应的焦点位置。
    int best_pos = 32;
    // 历史最佳清晰度值。
    int best_sharpness = 0;
    // 是否已经有有效的 best_pos/best_sharpness 记录。
    bool has_best = false;
    // 连续多少帧出现了 sharpness 下降，用于触发重新爬山。
    int decline_streak = 0;
    // 上一帧的清晰度值，用于判断本帧是否变差。
    int last_sharpness = 0;
    // 上一帧清晰度是否有效。
    bool has_last_sharpness = false;
    // 已经执行过多少次“重置步长重新爬山”。
    int restart_count = 0;
};

// 功能：
//   对一块灰度 ROI 计算基于 Sobel 梯度的清晰度分数。
// 参数：
//   roi: 中心 ROI 的亮度图，大小固定为 kAfRoiWidth * kAfRoiHeight。
// 返回值：
//   一个整数清晰度分数，值越大表示边缘越丰富、通常越清晰。
static int compute_sobel_sharpness(const std::array<uint8_t, kAfRoiWidth * kAfRoiHeight> &roi) {
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

// 功能：
//   扫描常见的 video/subdev 节点，寻找支持 V4L2_CID_FOCUS_ABSOLUTE 的设备。
// 参数：
//   无。
// 返回值：
//   FocusControlContext：
//   - use_ioctl=true 表示找到可直接下发焦点控件的真实设备；
//   - use_ioctl=false 表示当前只能退回应用层 dw9763 模拟控制。
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

// 功能：
//   关闭真实焦点控制设备。
// 参数：
//   ctx: 焦点控制上下文。
// 返回值：
//   无返回值。
static void close_focus_control_device(FocusControlContext *ctx) {
    if (!ctx || ctx->fd < 0) {
        return;
    }
    close(ctx->fd);
    ctx->fd = -1;
}

// 功能：
//   通过标准 V4L2 控件 ioctl 下发新的逻辑焦点位置。
// 参数：
//   fd: 支持 V4L2_CID_FOCUS_ABSOLUTE 的设备文件描述符。
//   logical_pos: 目标逻辑焦点位置，范围约定为 [0, 64]。
// 返回值：
//   true  表示 ioctl 调用成功；
//   false 表示 ioctl 失败，调用方应考虑退回模拟控制。
static bool set_focus_with_ioctl(int fd, int logical_pos) {
    struct v4l2_control ctrl;
    std::memset(&ctrl, 0, sizeof(ctrl));
    ctrl.id = V4L2_CID_FOCUS_ABSOLUTE;
    ctrl.value = logical_pos;
    return ioctl(fd, VIDIOC_S_CTRL, &ctrl) == 0;
}

// 功能：
//   读取 RK 私有 VCM 时间信息 ioctl，获取最近一次镜头移动耗时。
// 参数：
//   fd: 焦点控制设备文件描述符。
//   fallback_us: 若 ioctl 不支持时返回的默认耗时。
// 返回值：
//   成功时返回驱动报告的移动耗时；失败时返回 fallback_us。
static int query_move_time_us(int fd, int fallback_us) {
    struct rk_cam_vcm_tim info;
    std::memset(&info, 0, sizeof(info));
    if (fd >= 0 && ioctl(fd, RK_VIDIOC_VCM_TIMEINFO, &info) == 0 && info.move_us > 0) {
        return static_cast<int>(info.move_us);
    }
    return fallback_us;
}

// 功能：
//   统一下发新的焦点位置：优先使用真实 V4L2 ioctl，失败后退回 dw9763 模拟控制。
// 参数：
//   focus_ctx: 真实设备焦点控制上下文。
//   sim: 应用层 dw9763 模拟器。
//   logical_pos: 目标逻辑焦点位置。
//   move_time_us: 输出参数，返回本次移动预计耗时。
// 返回值：
//   true  表示至少有一种控制路径下发成功；
//   false 表示真实 ioctl 与模拟控制都失败。
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

// 功能：
//   丢弃旧的爬山历史，以当前镜头位置为锚点重新开始搜索。
// 参数：
//   hill: 待重置的爬山状态。
//   anchor_pos: 重新开始搜索时的镜头位置。
// 返回值：
//   无返回值。
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

// 功能：
//   根据相邻两帧的 Sobel 清晰度变化，统计“连续变差”的帧数。
// 参数：
//   hill: 爬山状态机，内部会记录上一帧清晰度和连续变差计数。
//   sharpness: 当前帧清晰度。
// 返回值：
//   true  表示已经连续多帧变差，建议重置步长重新爬山；
//   false 表示还不需要重启搜索。
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

// 功能：
//   根据当前清晰度结果执行一步爬山搜索，输出下一次要尝试的焦点位置。
// 参数：
//   hill: 爬山状态机，包括当前位置、方向、步长和历史最优点。
//   sharpness: 当前焦点位置对应的 Sobel 清晰度。
// 返回值：
//   下一次要写入 VCM 的目标逻辑焦点位置。
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

// 功能：
//   AF 线程主循环：
//   等待视频线程发布新 ROI -> 复制 ROI 到线程本地 -> 计算 Sobel 清晰度 ->
//   按 camera_app.c 的思路执行爬山搜索 -> 优先通过 V4L2_CID_FOCUS_ABSOLUTE
//   ioctl 下发真实焦点位置，失败时退回 dw9763 模拟控制接口。
// 参数：
//   state: AF 共享状态，包含 new_frame、ROI 数据、当前焦点和统计信息。
// 返回值：
//   无返回值；线程在 state->running 变为 false 后退出。
void run_autofocus_worker(AutofocusSharedState *state) {
    FocusControlContext focus_ctx = open_focus_control_device();
    Dw9763VcmSim vcm;
    std::array<uint8_t, kAfRoiWidth * kAfRoiHeight> local_roi{};
    HillClimbState hill;
    hill.current_pos = state->current_focus.load();
    hill.best_pos = hill.current_pos;

    state->current_step.store(hill.step);
    state->best_focus.store(hill.best_pos);
    state->decline_streak.store(0);
    state->restart_count.store(0);

    if (focus_ctx.use_ioctl) {
        std::cout << "[af] using focus ioctl device: " << focus_ctx.path << std::endl;
    } else {
        std::cout << "[af] no focus ioctl device found, use dw9763 simulation." << std::endl;
    }

    int initial_move_us = 30000;
    apply_focus_position(&focus_ctx, &vcm, hill.current_pos, &initial_move_us);
    state->last_move_us.store(initial_move_us);

    while (true) {
        bool restart_requested = false;
        {
            pthread_mutex_lock(&state->mutex);
            while (state->running && !state->new_frame) {
                pthread_cond_wait(&state->condition, &state->mutex);
            }
            if (!state->running) {
                pthread_mutex_unlock(&state->mutex);
                break;
            }
            std::copy(state->roi_y.begin(), state->roi_y.end(), local_roi.begin());
            restart_requested = state->restart_requested;
            state->restart_requested = false;
            state->new_frame = false;
            pthread_mutex_unlock(&state->mutex);
        }

        int sharpness = compute_sobel_sharpness(local_roi);
        state->last_sharpness.store(sharpness);

        bool decline_restart = false;
        if (!restart_requested) {
            decline_restart = update_decline_streak(&hill, sharpness);
        }

        if (restart_requested || decline_restart) {
            const int restart_anchor = state->current_focus.load();
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
        state->best_focus.store(hill.best_pos);
        state->current_step.store(hill.step);
        state->current_focus.store(next_pos);
        state->best_sharpness.store(hill.best_sharpness);
        state->decline_streak.store(hill.decline_streak);
        state->restart_count.store(hill.restart_count);

        int move_time_us = 30000;
        apply_focus_position(&focus_ctx, &vcm, next_pos, &move_time_us);
        state->last_move_us.store(move_time_us);

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
