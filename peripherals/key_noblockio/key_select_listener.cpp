#include "key_select_listener.h"

#include "autofocus_shared_state.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <sys/select.h>
#include <unistd.h>
#include <vector>

namespace {

static const int kKeyPressValue = 0;
static const int kSelectTimeoutUs = 500000;

// 功能：
//   检查 AF 线程是否仍允许按键监听线程继续运行。
// 参数：
//   state: AF 共享状态。
// 返回值：
//   true  表示继续监听；
//   false 表示程序正在退出。
static bool is_listener_running(AutofocusSharedState *state) {
    if (!state) {
        return false;
    }

    pthread_mutex_lock(&state->mutex);
    const bool running = state->running;
    pthread_mutex_unlock(&state->mutex);
    return running;
}

// 功能：
//   轮询几个常见的字符设备节点，寻找参考驱动导出的按键设备。
// 参数：
//   device_path: 输出参数，返回成功打开的节点路径。
// 返回值：
//   >= 0 表示成功打开后的文件描述符；
//   -1  表示本轮没有找到可用按键设备。
static int open_key_device(std::string *device_path) {
    const std::vector<std::string> candidates = {
        "/dev/key",
        "/dev/key0",
        "/dev/af_key"
    };

    for (size_t i = 0; i < candidates.size(); ++i) {
        const int fd = open(candidates[i].c_str(), O_RDONLY | O_NONBLOCK);
        if (fd >= 0) {
            if (device_path) {
                *device_path = candidates[i];
            }
            return fd;
        }
    }

    if (device_path) {
        device_path->clear();
    }
    return -1;
}

// 功能：
//   向 AF 线程发送一次“重新对焦”请求。
// 参数：
//   state: AF 共享状态。
// 返回值：
//   无返回值。
static void request_af_restart(AutofocusSharedState *state) {
    pthread_mutex_lock(&state->mutex);
    state->restart_requested = true;
    pthread_mutex_unlock(&state->mutex);
    pthread_cond_signal(&state->condition);
}

}  // namespace

// 功能：
//   在独立线程中参考 14_noblockio 的用户态程序，通过 select + read
//   监听字符设备按键事件；检测到按下后通知 AF 线程重置步长并重新爬山。
// 参数：
//   state: AF 共享状态；用于设置 restart_requested 并唤醒 AF 线程。
// 返回值：
//   无返回值；线程会在 state->running 变为 false 后退出。
void run_key_select_listener(AutofocusSharedState *state) {
    if (!state) {
        return;
    }

    int key_fd = -1;
    std::string key_path;

    while (is_listener_running(state)) {
        if (key_fd < 0) {
            key_fd = open_key_device(&key_path);
            if (key_fd < 0) {
                usleep(500000);
                continue;
            }

            std::cout << "[key] listening on " << key_path << std::endl;
        }

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(key_fd, &readfds);

        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = kSelectTimeoutUs;

        const int ret = select(key_fd + 1, &readfds, nullptr, nullptr, &timeout);
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }

            std::cerr << "[key] select failed on " << key_path
                      << ": " << std::strerror(errno) << std::endl;
            close(key_fd);
            key_fd = -1;
            key_path.clear();
            continue;
        }

        if (ret == 0) {
            continue;
        }

        if (!FD_ISSET(key_fd, &readfds)) {
            continue;
        }

        int key_value = -1;
        const ssize_t read_ret = read(key_fd, &key_value, sizeof(key_value));
        if (read_ret == static_cast<ssize_t>(sizeof(key_value))) {
            if (key_value == kKeyPressValue) {
                request_af_restart(state);
                std::cout << "[key] press detected, request autofocus restart." << std::endl;
            }
            continue;
        }

        if (read_ret < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        }

        std::cerr << "[key] read failed on " << key_path
                  << ", reopen device." << std::endl;
        close(key_fd);
        key_fd = -1;
        key_path.clear();
    }

    if (key_fd >= 0) {
        close(key_fd);
    }
}
