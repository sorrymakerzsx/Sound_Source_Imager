#include "backlight_pwm.h"

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
/*
用法：
cat /sys/class/backlight/backlight/max_brightness
cat /sys/class/backlight/backlight/brightness
echo 64 > /sys/class/backlight/backlight/brightness
echo 255 > /sys/class/backlight/backlight/brightness
*/
namespace {

const char kBacklightClassPath[] = "/sys/class/backlight";

// 功能：
//   从指定 sysfs 文件读取一个整数值。
// 参数：
//   path: 目标文件路径，例如 /sys/class/backlight/backlight/brightness。
//   value: 输出参数，成功时写入读取到的整数。
// 返回值：
//   true 表示读取并解析成功；false 表示打开、读取或参数检查失败。
bool read_int_file(const std::string &path, int *value) {
    if (!value) {
        return false;
    }

    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }

    char buffer[32] = {0};
    ssize_t ret = read(fd, buffer, sizeof(buffer) - 1);
    close(fd);
    if (ret <= 0) {
        return false;
    }

    *value = std::atoi(buffer);
    return true;
}

}  // namespace

// 功能：
//   枚举 /sys/class/backlight 下的背光设备，找到第一路可访问的背光并读出
//   当前亮度和最大亮度，供主线程打印状态或后续调节亮度使用。
// 参数：
//   info: 输出参数，成功时写入背光名字、sysfs 根路径、当前亮度和最大亮度。
// 返回值：
//   true 表示至少找到一个可读的背光设备；false 表示目录不存在或没有可用节点。
bool probe_backlight_device(BacklightDeviceInfo *info) {
    if (!info) {
        return false;
    }

    info->available = false;
    info->name.clear();
    info->base_path.clear();
    info->brightness = -1;
    info->max_brightness = -1;

    DIR *dir = opendir(kBacklightClassPath);
    if (!dir) {
        return false;
    }

    bool found = false;
    struct dirent *entry = nullptr;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') {
            continue;
        }

        std::string base_path = std::string(kBacklightClassPath) + "/" + entry->d_name;
        int brightness = -1;
        int max_brightness = -1;
        if (!read_int_file(base_path + "/brightness", &brightness) ||
            !read_int_file(base_path + "/max_brightness", &max_brightness)) {
            continue;
        }

        info->available = true;
        info->name = entry->d_name;
        info->base_path = base_path;
        info->brightness = brightness;
        info->max_brightness = max_brightness;
        found = true;
        break;
    }

    closedir(dir);
    return found;
}

// 功能：
//   向指定背光设备的 brightness 节点写入目标亮度。
// 参数：
//   base_path: 背光设备的根目录，例如 /sys/class/backlight/backlight。
//   brightness: 要写入的亮度值，调用方应保证它不超过 max_brightness。
// 返回值：
//   true 表示亮度值完整写入；false 表示节点打开失败或写入字节数不正确。
bool set_backlight_brightness(const std::string &base_path, int brightness) {
    std::ostringstream oss;
    oss << brightness;
    std::string text = oss.str();

    int fd = open((base_path + "/brightness").c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }

    ssize_t ret = write(fd, text.c_str(), text.size());
    close(fd);
    return ret == static_cast<ssize_t>(text.size());
}
