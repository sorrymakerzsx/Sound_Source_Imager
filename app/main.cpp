#include <iostream>
#include <csignal>
#include <cstring>
#include <pthread.h>

#include "app_config.h"
#include "audio_cloud_overlay.h"
#include "autofocus_shared_state.h"
#include "autofocus_worker.h"
#include "backlight_pwm.h"
#include "drm_mode.h"
#include "eeprom_version.h"
#include "overlay_shared_state.h"
#include "stream_mode.h"

namespace {

OverlaySharedState *g_overlay_state = nullptr;
AutofocusSharedState *g_af_state = nullptr;

struct DrmVideoThreadArg {
    OverlaySharedState *shared_state;
    AutofocusSharedState *af_state;
    int *video_result;
};

struct StreamThreadArg {
    const AppConfig *config;
    OverlaySharedState *shared_state;
    int *stream_result;
};

// 功能：
//   处理 SIGINT/SIGTERM，通知各工作线程退出。
// 参数：
//   无名 int 参数为信号编号，这里不关心具体值。
// 返回值：
//   无返回值。
// 说明：
//   这里只修改共享状态，不直接释放资源；真正的 join/close 由 main 统一收尾。
void handle_signal(int) {
    if (g_overlay_state) {
        g_overlay_state->running.store(false);
        pthread_cond_broadcast(&g_overlay_state->condition);
    }
    if (g_af_state) {
        pthread_mutex_lock(&g_af_state->mutex);
        g_af_state->running = false;
        pthread_mutex_unlock(&g_af_state->mutex);
        pthread_cond_broadcast(&g_af_state->condition);
    }
}

// 功能：
//   安装 POSIX 信号处理函数。
// 参数：
//   无。
// 返回值：
//   无返回值。
void install_signal_handlers() {
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
}

// 功能：
//   pthread 音频云图线程入口。
// 参数：
//   arg: OverlaySharedState*。
// 返回值：
//   固定返回 nullptr。
void *audio_thread_entry(void *arg) {
    run_audio_overlay_worker(reinterpret_cast<OverlaySharedState *>(arg));
    return nullptr;
}

// 功能：
//   pthread AF 线程入口。
// 参数：
//   arg: AutofocusSharedState*。
// 返回值：
//   固定返回 nullptr。
void *af_thread_entry(void *arg) {
    run_autofocus_worker(reinterpret_cast<AutofocusSharedState *>(arg));
    return nullptr;
}

// 功能：
//   pthread DRM 视频线程入口。
// 参数：
//   arg: DrmVideoThreadArg*。
// 返回值：
//   固定返回 nullptr。
void *drm_video_thread_entry(void *arg) {
    DrmVideoThreadArg *ctx = reinterpret_cast<DrmVideoThreadArg *>(arg);
    *(ctx->video_result) = run_video_display_worker(ctx->shared_state, ctx->af_state);
    ctx->shared_state->running.store(false);
    pthread_cond_broadcast(&ctx->shared_state->condition);
    pthread_mutex_lock(&ctx->af_state->mutex);
    ctx->af_state->running = false;
    pthread_mutex_unlock(&ctx->af_state->mutex);
    pthread_cond_broadcast(&ctx->af_state->condition);
    return nullptr;
}

// 功能：
//   pthread stream 推流线程入口。
// 参数：
//   arg: StreamThreadArg*。
// 返回值：
//   固定返回 nullptr。
void *stream_thread_entry(void *arg) {
    StreamThreadArg *ctx = reinterpret_cast<StreamThreadArg *>(arg);
    *(ctx->stream_result) = run_stream_mode(*(ctx->config), ctx->shared_state);
    ctx->shared_state->running.store(false);
    pthread_cond_broadcast(&ctx->shared_state->condition);
    return nullptr;
}

// 功能：
//   在主线程启动工作线程之前，打印当前板子的背光与 EEPROM 外设状态。
// 参数：
//   无。
// 返回值：
//   无返回值。
// 说明：
//   这是“主线程调用外设封装”的入口点：
//   1. 先调用 probe_backlight_device() 探测背光 sysfs 节点；
//   2. 再调用 read_first_available_eeprom() 尝试读取 EEPROM 版本信息；
//   3. 这里只做启动前状态探测和日志打印，不改变主业务线程模型。
void log_peripheral_state() {
    BacklightDeviceInfo backlight_info;
    if (probe_backlight_device(&backlight_info)) {
        std::cout << "Backlight device: " << backlight_info.name
                  << " brightness=" << backlight_info.brightness
                  << "/" << backlight_info.max_brightness
                  << " path=" << backlight_info.base_path << std::endl;
    } else {
        std::cout << "Backlight device not found." << std::endl;
    }

    EepromVersionInfo eeprom_info;
    if (read_first_available_eeprom(&eeprom_info)) {
        std::cout << "EEPROM version from " << eeprom_info.device_path
                  << ": " << (eeprom_info.text.empty() ? "<non-printable>" : eeprom_info.text)
                  << std::endl;
        std::cout << "EEPROM raw: " << format_eeprom_hex(eeprom_info.raw_bytes) << std::endl;
    } else {
        std::cout << "EEPROM version device not found." << std::endl;
    }
}

}  // namespace

// 功能：
//   程序入口，根据命令行选择 DRM 本地显示模式或 stream 推流模式，
//   并拉起对应的工作线程。
// 参数：
//   argc: 命令行参数个数。
//   argv: 命令行参数数组。
// 返回值：
//   0  表示执行成功结束；
//   -1 表示参数非法或初始化失败。
int main(int argc, char **argv) {
    AppConfig config;
    if (!parse_app_config(argc, argv, &config)) {
        return -1;
    }

    // 主线程在创建 DRM/stream/audio/AF 工作线程之前，先探测一次外设状态。
    // 所以如果你问“主循环里哪里调用了背光/EEPROM”，就是这里这一行。
    log_peripheral_state();

    if (config.mode == OutputMode::DRM) {
        // DRM 模式下有三个线程：
        // 1. 云图线程：生成声源热点 ARGB 图；
        // 2. AF 线程：消费 ROI，做 Sobel + 爬山；
        // 3. 视频线程：V4L2 取帧并通过 DRM 显示。
        OverlaySharedState shared_state;
        AutofocusSharedState af_state;
        if (!init_overlay_shared_state(&shared_state)) {
            std::cerr << "Failed to init overlay pthread state." << std::endl;
            return -1;
        }
        if (!init_autofocus_shared_state(&af_state)) {
            std::cerr << "Failed to init autofocus pthread state." << std::endl;
            destroy_overlay_shared_state(&shared_state);
            return -1;
        }
        g_overlay_state = &shared_state;
        g_af_state = &af_state;
        install_signal_handlers();

        int video_result = 0;
        pthread_t audio_thread = 0;
        pthread_t af_thread = 0;
        pthread_t video_thread = 0;

        if (pthread_create(&audio_thread, nullptr, audio_thread_entry, &shared_state) != 0) {
            std::cerr << "Failed to create audio thread." << std::endl;
            g_overlay_state = nullptr;
            g_af_state = nullptr;
            destroy_autofocus_shared_state(&af_state);
            destroy_overlay_shared_state(&shared_state);
            return -1;
        }
        if (pthread_create(&af_thread, nullptr, af_thread_entry, &af_state) != 0) {
            std::cerr << "Failed to create AF thread." << std::endl;
            shared_state.running.store(false);
            pthread_cond_broadcast(&shared_state.condition);
            pthread_join(audio_thread, nullptr);
            g_overlay_state = nullptr;
            g_af_state = nullptr;
            destroy_autofocus_shared_state(&af_state);
            destroy_overlay_shared_state(&shared_state);
            return -1;
        }

        DrmVideoThreadArg video_arg = {&shared_state, &af_state, &video_result};
        if (pthread_create(&video_thread, nullptr, drm_video_thread_entry, &video_arg) != 0) {
            std::cerr << "Failed to create video thread." << std::endl;
            shared_state.running.store(false);
            pthread_cond_broadcast(&shared_state.condition);
            pthread_mutex_lock(&af_state.mutex);
            af_state.running = false;
            pthread_mutex_unlock(&af_state.mutex);
            pthread_cond_broadcast(&af_state.condition);
            pthread_join(audio_thread, nullptr);
            pthread_join(af_thread, nullptr);
            g_overlay_state = nullptr;
            g_af_state = nullptr;
            destroy_autofocus_shared_state(&af_state);
            destroy_overlay_shared_state(&shared_state);
            return -1;
        }

        pthread_join(video_thread, nullptr);
        shared_state.running.store(false);
        pthread_cond_broadcast(&shared_state.condition);
        pthread_mutex_lock(&af_state.mutex);
        af_state.running = false;
        pthread_mutex_unlock(&af_state.mutex);
        pthread_cond_broadcast(&af_state.condition);
        pthread_join(audio_thread, nullptr);
        pthread_join(af_thread, nullptr);
        g_overlay_state = nullptr;
        g_af_state = nullptr;
        destroy_autofocus_shared_state(&af_state);
        destroy_overlay_shared_state(&shared_state);
        return video_result;
    } else if (config.mode == OutputMode::STREAM) {
        // stream 模式下没有 AF 线程，但仍保留云图线程；
        // 最新云图会在编码前叠加到 NV12，再送入 MPP 编码。
        OverlaySharedState shared_state;
        if (!init_overlay_shared_state(&shared_state)) {
            std::cerr << "Failed to init overlay pthread state." << std::endl;
            return -1;
        }
        g_overlay_state = &shared_state;
        g_af_state = nullptr;
        install_signal_handlers();

        int stream_result = 0;
        pthread_t audio_thread = 0;
        pthread_t stream_thread = 0;
        if (pthread_create(&audio_thread, nullptr, audio_thread_entry, &shared_state) != 0) {
            std::cerr << "Failed to create audio thread." << std::endl;
            g_overlay_state = nullptr;
            destroy_overlay_shared_state(&shared_state);
            return -1;
        }

        StreamThreadArg stream_arg = {&config, &shared_state, &stream_result};
        if (pthread_create(&stream_thread, nullptr, stream_thread_entry, &stream_arg) != 0) {
            std::cerr << "Failed to create stream thread." << std::endl;
            shared_state.running.store(false);
            pthread_cond_broadcast(&shared_state.condition);
            pthread_join(audio_thread, nullptr);
            g_overlay_state = nullptr;
            destroy_overlay_shared_state(&shared_state);
            return -1;
        }

        pthread_join(stream_thread, nullptr);
        shared_state.running.store(false);
        pthread_cond_broadcast(&shared_state.condition);
        pthread_join(audio_thread, nullptr);
        g_overlay_state = nullptr;
        destroy_overlay_shared_state(&shared_state);
        return stream_result;
    }

    std::cerr << "Unknown output mode." << std::endl;
    return -1;
}
