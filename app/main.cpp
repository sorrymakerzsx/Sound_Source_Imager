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
#include "key_select_listener.h"
#include "stream_mode.h"

namespace {

OverlaySharedState *g_overlay_state = nullptr;
AutofocusSharedState *g_af_state = nullptr;

struct DrmVideoThreadArg {
    // 指向云图共享状态，供视频线程读取最新 overlay。
    OverlaySharedState *shared_state;
    // 指向 AF 共享状态，供视频线程发布 ROI、退出时通知 AF。
    AutofocusSharedState *af_state;
    // 视频线程执行结束后，把返回值写回主线程。
    int *video_result;
};

struct StreamThreadArg {
    // 推流模式的运行配置，如 host/port/proto。
    const AppConfig *config;
    // 指向云图共享状态，供推流线程读取并混合 overlay。
    OverlaySharedState *shared_state;
    // 推流线程执行结束后，把返回值写回主线程。
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
// 信号处理：只改 running，线程自己轮询到就会退，不做 join / close。
void handle_signal(int) {
    if (g_overlay_state) {
        __atomic_store_n(&g_overlay_state->running, false, __ATOMIC_SEQ_CST);
    }
    if (g_af_state) {
        __atomic_store_n(&g_af_state->running, false, __ATOMIC_SEQ_CST);
    }
}

// 安装 SIGINT / SIGTERM 处理器。
void install_signal_handlers() {
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
}

void *audio_thread_entry(void *arg) {
    run_audio_overlay_worker(reinterpret_cast<OverlaySharedState *>(arg));
    return nullptr;
}

void *af_thread_entry(void *arg) {
    run_autofocus_worker(reinterpret_cast<AutofocusSharedState *>(arg));
    return nullptr;
}

void *key_thread_entry(void *arg) {
    run_key_select_listener(reinterpret_cast<AutofocusSharedState *>(arg));
    return nullptr;
}

// DRM 视频线程入口。线程退出后把 running 置 false，其他线程轮询到就退。
void *drm_video_thread_entry(void *arg) {
    DrmVideoThreadArg *ctx = reinterpret_cast<DrmVideoThreadArg *>(arg);
    *(ctx->video_result) = run_video_display_worker(ctx->shared_state, ctx->af_state);
    __atomic_store_n(&ctx->shared_state->running, false, __ATOMIC_SEQ_CST);
    __atomic_store_n(&ctx->af_state->running, false, __ATOMIC_SEQ_CST);
    return nullptr;
}

// stream 推流线程入口。退出后置 running，音频线程轮询到就退。
void *stream_thread_entry(void *arg) {
    StreamThreadArg *ctx = reinterpret_cast<StreamThreadArg *>(arg);
    *(ctx->stream_result) = run_stream_mode(*(ctx->config), ctx->shared_state);
    __atomic_store_n(&ctx->shared_state->running, false, __ATOMIC_SEQ_CST);
    return nullptr;
}

// 启动前探一下背光和 EEPROM，只打日志。
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

// ---------------------------------------------------------------------------
// 主入口：解析参数 → 按模式创建线程 → join 收尾
// ---------------------------------------------------------------------------
int main(int argc, char **argv) {
    AppConfig config;
    if (!parse_app_config(argc, argv, &config)) {
        return -1;
    }

    log_peripheral_state();

    if (config.mode == OutputMode::DRM) {
        // 四个线程：云图 / AF / 按键 / 视频显示
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
        pthread_t key_thread = 0;
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
            __atomic_store_n(&shared_state.running, false, __ATOMIC_SEQ_CST);
            pthread_join(audio_thread, nullptr);
            g_overlay_state = nullptr;
            g_af_state = nullptr;
            destroy_autofocus_shared_state(&af_state);
            destroy_overlay_shared_state(&shared_state);
            return -1;
        }
        if (pthread_create(&key_thread, nullptr, key_thread_entry, &af_state) != 0) {
            std::cerr << "Failed to create key thread." << std::endl;
            __atomic_store_n(&shared_state.running, false, __ATOMIC_SEQ_CST);
            __atomic_store_n(&af_state.running, false, __ATOMIC_SEQ_CST);
            pthread_join(audio_thread, nullptr);
            pthread_join(af_thread, nullptr);
            g_overlay_state = nullptr;
            g_af_state = nullptr;
            destroy_autofocus_shared_state(&af_state);
            destroy_overlay_shared_state(&shared_state);
            return -1;
        }

        DrmVideoThreadArg video_arg = {&shared_state, &af_state, &video_result};
        if (pthread_create(&video_thread, nullptr, drm_video_thread_entry, &video_arg) != 0) {
            std::cerr << "Failed to create video thread." << std::endl;
            __atomic_store_n(&shared_state.running, false, __ATOMIC_SEQ_CST);
            __atomic_store_n(&af_state.running, false, __ATOMIC_SEQ_CST);
            pthread_join(audio_thread, nullptr);
            pthread_join(af_thread, nullptr);
            pthread_join(key_thread, nullptr);
            g_overlay_state = nullptr;
            g_af_state = nullptr;
            destroy_autofocus_shared_state(&af_state);
            destroy_overlay_shared_state(&shared_state);
            return -1;
        }

        pthread_join(video_thread, nullptr);
        __atomic_store_n(&shared_state.running, false, __ATOMIC_SEQ_CST);
        __atomic_store_n(&af_state.running, false, __ATOMIC_SEQ_CST);
        pthread_join(audio_thread, nullptr);
        pthread_join(af_thread, nullptr);
        pthread_join(key_thread, nullptr);
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
            __atomic_store_n(&shared_state.running, false, __ATOMIC_SEQ_CST);
            pthread_join(audio_thread, nullptr);
            g_overlay_state = nullptr;
            destroy_overlay_shared_state(&shared_state);
            return -1;
        }

        pthread_join(stream_thread, nullptr);
        __atomic_store_n(&shared_state.running, false, __ATOMIC_SEQ_CST);
        pthread_join(audio_thread, nullptr);
        g_overlay_state = nullptr;
        destroy_overlay_shared_state(&shared_state);
        return stream_result;
    }

    std::cerr << "Unknown output mode." << std::endl;
    return -1;
}
