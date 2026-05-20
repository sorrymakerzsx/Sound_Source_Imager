#ifndef V4L2_DRM_IMX415_BACKLIGHT_PWM_H_
#define V4L2_DRM_IMX415_BACKLIGHT_PWM_H_

#include <string>

struct BacklightDeviceInfo {
    // 是否成功探测到可用背光设备。
    bool available = false;
    // 背光设备名称，对应 /sys/class/backlight/<name>。
    std::string name;
    // 背光设备在 sysfs 下的根路径。
    std::string base_path;
    // 当前亮度值。
    int brightness = -1;
    // 设备支持的最大亮度值。
    int max_brightness = -1;
};

// 功能：
//   探测系统中第一个可用的背光类设备，并读取当前亮度信息。
// 参数：
//   info: 输出结果，成功时写入背光名称、sysfs 路径和亮度值。
// 返回值：
//   true 表示找到可用背光；false 表示系统中没有可访问的背光设备。
bool probe_backlight_device(BacklightDeviceInfo *info);

// 功能：
//   向 sysfs brightness 节点写入指定亮度值。
// 参数：
//   base_path: 背光设备目录，例如 /sys/class/backlight/backlight。
//   brightness: 目标亮度，通常范围是 [0, max_brightness]。
// 返回值：
//   true 表示写入成功；false 表示打开或写节点失败。
bool set_backlight_brightness(const std::string &base_path, int brightness);

#endif
