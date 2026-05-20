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

        // readfds 是 select() 使用的“读事件集合”。
        // 这里我们只监听一个按键设备 key_fd，所以流程是：
        // 1. 先把集合清空；
        // 2. 再把 key_fd 放进去；
        // 3. 告诉 select()：“只要这个 fd 变成可读，就返回给我”。
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(key_fd, &readfds);

        // timeout 指定本轮 select() 最多等待多久。
        // 这里没有用永久阻塞，而是设置了一个 0.5s 超时，
        // 这样即使设备一直没有按键事件，线程也会周期性醒来一次，
        // 重新检查 state->running，避免程序退出时长时间卡在 select() 里。
        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = kSelectTimeoutUs;

        // select() 的第 1 个参数不是“fd 个数”，而是“最大 fd + 1”。
        // 这里只监听 key_fd 一个描述符，所以传 key_fd + 1。
        //
        // 返回值含义：
        // < 0 : 调用出错；
        // = 0 : 本轮等到超时，也没有可读事件；
        // > 0 : 至少有一个 fd 就绪，这里通常就是 key_fd 可读。
        const int ret = select(key_fd + 1, &readfds, nullptr, nullptr, &timeout);
        if (ret < 0) {
            // EINTR 表示 select() 在等待期间被信号中断了。
            // 这不代表按键设备本身坏了，只是本轮等待被打断，
            // 所以直接 continue，下一轮重新监听即可。
            if (errno == EINTR) {
                continue;
            }

            // 走到这里说明是“真正的 select 错误”，例如设备异常。
            // 当前 fd 很可能已经不可用了，因此：
            // 1. 打日志；
            // 2. 关闭旧 fd；
            // 3. 清空路径；
            // 4. 下一轮 while 再重新 open 设备。
            std::cerr << "[key] select failed on " << key_path
                      << ": " << std::strerror(errno) << std::endl;
            close(key_fd);
            key_fd = -1;
            key_path.clear();
            continue;
        }

        // ret == 0 表示超时，没有按键事件到来。
        // 这里什么都不做，直接进入下一轮监听。
        if (ret == 0) {
            continue;
        }

        // 虽然当前只监听了一个 fd，但仍然保留 FD_ISSET 检查，
        // select()标准写法：确认 key_fd 真的被内核标记为“可读”。
        if (!FD_ISSET(key_fd, &readfds)) {
            continue;
        }

        // 设备可读后，再真正调用 read() 把驱动里的按键状态取出来。
        // 本工程约定：
        // - 读取到一个 int；
        // - 值为 0 表示按下；
        // - 值为 1 表示松开；
        // - 我们这里只关心“按下”这个事件。
        int key_value = -1;
        const ssize_t read_ret = read(key_fd, &key_value, sizeof(key_value));
        if (read_ret == static_cast<ssize_t>(sizeof(key_value))) {
            // 只有检测到“按下”才触发重新对焦。
            // 松开事件会被忽略，这样可以避免一次按键动作触发两次重爬山。
            if (key_value == kKeyPressValue) {
                request_af_restart(state);
                std::cout << "[key] press detected, request autofocus restart." << std::endl;
            }
            continue;
        }

        // EAGAIN / EWOULDBLOCK 表示：
        // 当前 fd 是非阻塞打开的，但这一次 read() 时驱动里暂时没有完整数据可取。
        // 这不是致命错误，所以直接继续下一轮 select() 即可。
        if (read_ret < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        }

        // 走到这里说明 read() 不是正常读到数据，也不是暂时无数据，
        // 更像是设备状态异常、节点失效，或者驱动读流程出问题。
        // 处理策略和 select 失败一致：关闭并丢弃当前 fd，等待下一轮重新打开。
        std::cerr << "[key] read failed on " << key_path
                  << ", reopen device." << std::endl;
        close(key_fd);
        key_fd = -1;
        key_path.clear();
    }

    // 线程退出前做最后一次资源清理，避免泄漏文件描述符。
    if (key_fd >= 0) {
        close(key_fd);
    }
}
