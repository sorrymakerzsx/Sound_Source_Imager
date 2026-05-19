#include "eeprom_version.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <sstream>

namespace {

const char *kEepromCandidates[] = {
    "/dev/eeprom_ver",
    "/dev/eeprom_ver_dev",
};

// 功能：
//   把 EEPROM 读出的原始字节裁剪成便于显示的可打印字符串。
// 参数：
//   raw_bytes: 从字符设备读出的原始缓冲区，可能包含 '\0' 或不可打印字符。
// 返回值：
//   截止到第一个 '\0' 的可打印文本，末尾空白会被去掉。
std::string trim_printable_string(const std::vector<uint8_t> &raw_bytes) {
    std::string text;
    text.reserve(raw_bytes.size());
    for (size_t i = 0; i < raw_bytes.size(); ++i) {
        unsigned char ch = raw_bytes[i];
        if (ch == '\0') {
            break;
        }
        if (std::isprint(ch)) {
            text.push_back(static_cast<char>(ch));
        }
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.pop_back();
    }
    return text;
}

}  // namespace

// 功能：
//   打开指定 EEPROM 字符设备并读取一段版本信息数据。
// 参数：
//   device_path: 设备节点路径，例如 /dev/eeprom_ver。
//   info: 输出参数，成功时写入设备路径、文本版本和原始字节数组。
// 返回值：
//   true 表示读取成功；false 表示节点不存在、打开失败或读取长度非法。
bool read_eeprom_version(const std::string &device_path, EepromVersionInfo *info) {
    if (!info) {
        return false;
    }

    info->available = false;
    info->device_path.clear();
    info->text.clear();
    info->raw_bytes.clear();

    int fd = open(device_path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }

    std::vector<uint8_t> raw_bytes(64, 0);
    ssize_t ret = read(fd, raw_bytes.data(), raw_bytes.size());
    close(fd);
    if (ret <= 0) {
        return false;
    }

    raw_bytes.resize(static_cast<size_t>(ret));
    info->available = true;
    info->device_path = device_path;
    info->raw_bytes = raw_bytes;
    info->text = trim_printable_string(raw_bytes);
    return true;
}

// 功能：
//   依次探测工程约定的 EEPROM 设备节点，返回第一份成功读到的数据。
// 参数：
//   info: 输出参数，成功时写入命中的节点信息和内容。
// 返回值：
//   true 表示至少一个候选设备读取成功；false 表示所有候选路径都失败。
bool read_first_available_eeprom(EepromVersionInfo *info) {
    if (!info) {
        return false;
    }

    for (size_t i = 0; i < sizeof(kEepromCandidates) / sizeof(kEepromCandidates[0]); ++i) {
        if (read_eeprom_version(kEepromCandidates[i], info)) {
            return true;
        }
    }
    return false;
}

// 功能：
//   将 EEPROM 原始字节序列格式化成十六进制字符串，便于日志中直接查看。
// 参数：
//   raw_bytes: 原始字节数组。
// 返回值：
//   形如 "31 2E 30 2E 5A" 的十六进制文本。
std::string format_eeprom_hex(const std::vector<uint8_t> &raw_bytes) {
    std::ostringstream oss;
    for (size_t i = 0; i < raw_bytes.size(); ++i) {
        if (i != 0) {
            oss << ' ';
        }
        oss << std::uppercase << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<unsigned int>(raw_bytes[i]);
    }
    return oss.str();
}
