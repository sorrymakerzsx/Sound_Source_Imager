#ifndef V4L2_DRM_IMX415_EEPROM_VERSION_H_
#define V4L2_DRM_IMX415_EEPROM_VERSION_H_

#include <stdint.h>

#include <string>
#include <vector>

struct EepromVersionInfo {
    bool available = false;
    std::string device_path;
    std::string text;
    std::vector<uint8_t> raw_bytes;
};

// 功能：
//   从指定 EEPROM 字符设备读取版本字符串和原始字节。
// 参数：
//   device_path: 设备节点路径，例如 /dev/eeprom_ver。
//   info: 输出结果，成功时写入设备路径、文本和原始字节。
// 返回值：
//   true 表示读取成功；false 表示设备不存在或读取失败。
bool read_eeprom_version(const std::string &device_path, EepromVersionInfo *info);

// 功能：
//   依次尝试工程约定的 EEPROM 设备节点，读取第一份可用版本信息。
// 参数：
//   info: 输出结果。
// 返回值：
//   true 表示至少找到一个可用 EEPROM 设备并成功读取；false 表示未找到。
bool read_first_available_eeprom(EepromVersionInfo *info);

// 功能：
//   把原始字节格式化为便于打印的十六进制字符串。
// 参数：
//   raw_bytes: EEPROM 原始数据。
// 返回值：
//   形如 "31 2E 30 2E 5A" 的十六进制字符串。
std::string format_eeprom_hex(const std::vector<uint8_t> &raw_bytes);

#endif
