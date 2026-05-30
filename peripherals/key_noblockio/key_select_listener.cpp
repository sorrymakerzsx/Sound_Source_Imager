#include "key_select_listener.h"

#include "autofocus_shared_state.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/select.h>
#include <unistd.h>

namespace {

static const int kKeyPressValue = 0;
static const int kSelectTimeoutUs = 500000;

static bool is_listener_running(AutofocusSharedState *state) {
    if (!state) {
        return false;
    }
    return __atomic_load_n(&state->running, __ATOMIC_SEQ_CST);
}

// 轮询尝试打开 /dev/key /dev/key0 /dev/af_key
static int open_key_device(char *device_path, size_t path_size) {
    const char *candidates[] = {
        "/dev/key",
        "/dev/key0",
        "/dev/af_key",
        NULL
    };

    for (int i = 0; candidates[i] != NULL; ++i) {
        const int fd = open(candidates[i], O_RDONLY | O_NONBLOCK);
        if (fd >= 0) {
            if (device_path && path_size > 0) {
                strncpy(device_path, candidates[i], path_size - 1);
                device_path[path_size - 1] = '\0';
            }
            return fd;
        }
    }

    if (device_path && path_size > 0) {
        device_path[0] = '\0';
    }
    return -1;
}

// 直接 atomic store，AF 线程下一轮 exchange(false) 就读到了。
static void request_af_restart(AutofocusSharedState *state) {
    __atomic_store_n(&state->restart_requested, true, __ATOMIC_RELEASE);
}

}  // namespace

// 独立线程中 select 监听按键设备，检测到按下后通知 AF 重新爬山。
void run_key_select_listener(AutofocusSharedState *state) {
    if (!state) {
        return;
    }

    int key_fd = -1;
    char key_path[256] = {0};

    while (is_listener_running(state)) {
        if (key_fd < 0) {
            key_fd = open_key_device(key_path, sizeof(key_path));
            if (key_fd < 0) {
                usleep(500000);
                continue;
            }
            printf("[key] listening on %s\n", key_path);
        }

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(key_fd, &readfds);

        // 0.5s 超时，避免卡死在 select，周期性检查 running 标志
        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = kSelectTimeoutUs;

        const int ret = select(key_fd + 1, &readfds, nullptr, nullptr, &timeout);
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            fprintf(stderr, "[key] select failed on %s: %s\n", key_path, strerror(errno));
            close(key_fd);
            key_fd = -1;
            key_path[0] = '\0';
            continue;
        }

        if (ret == 0) {
            continue;
        }

        if (!FD_ISSET(key_fd, &readfds)) {
            continue;
        }

        // 驱动约定：read 返回 int，0 = 按下，1 = 松开
        int key_value = -1;
        const ssize_t read_ret = read(key_fd, &key_value, sizeof(key_value));
        if (read_ret == static_cast<ssize_t>(sizeof(key_value))) {
            if (key_value == kKeyPressValue) {
                request_af_restart(state);
                printf("[key] press detected, request autofocus restart.\n");
            }
            continue;
        }

        if (read_ret < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        }

        // read 失败则认为设备异常，关闭等下次重开
        fprintf(stderr, "[key] read failed on %s, reopen device.\n", key_path);
        close(key_fd);
        key_fd = -1;
        key_path[0] = '\0';
    }

    if (key_fd >= 0) {
        close(key_fd);
    }
}
