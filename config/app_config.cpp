#include "app_config.h"

#include <cstring>
#include <iostream>
#include <string>

// 功能：
//   打印命令行帮助信息。
// 参数：
//   program: 当前程序名，通常来自 argv[0]，用于在帮助中显示调用方式。
// 返回值：
//   无返回值。
void print_usage(const char *program) {
    std::cout << "Usage: " << program
              << " [--mode drm|stream] [--proto udp|tcp] [--host ip] [--port port]"
              << std::endl;
    std::cout << "  --mode drm       使用 DRM 在本地 LCD 上显示" << std::endl;
    std::cout << "  --mode stream    使用 H.264 推流到 PC" << std::endl;
    std::cout << "  --proto udp|tcp  推流协议，默认 udp" << std::endl;
    std::cout << "  --host ip        流媒体目标主机 IP，默认 192.168.1.100" << std::endl;
    std::cout << "  --port port      流媒体目标端口，默认 5000" << std::endl;
}

// 功能：
//   解析命令行参数，填充 AppConfig。
// 参数：
//   argc: 命令行参数个数。
//   argv: 命令行参数数组。
//   config: 输出参数，解析成功后写入运行模式、协议、主机和端口。
// 返回值：
//   true  表示参数有效，config 已可直接使用；
//   false 表示参数非法或请求打印帮助，此时调用者应结束程序。
bool parse_app_config(int argc, char **argv, AppConfig *config) {
    *config = AppConfig();

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return false;
        }

        if (arg == "drm" || arg == "--mode=drm") {
            config->mode = OutputMode::DRM;
            continue;
        }

        if (arg == "stream" || arg == "--mode=stream") {
            config->mode = OutputMode::STREAM;
            continue;
        }

        if (arg == "--mode") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value after --mode" << std::endl;
                print_usage(argv[0]);
                return false;
            }

            std::string value = argv[++i];
            if (value == "drm") {
                config->mode = OutputMode::DRM;
            } else if (value == "stream") {
                config->mode = OutputMode::STREAM;
            } else {
                std::cerr << "Unsupported mode: " << value << std::endl;
                print_usage(argv[0]);
                return false;
            }
            continue;
        }

        if (arg == "--proto") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value after --proto" << std::endl;
                print_usage(argv[0]);
                return false;
            }

            std::string value = argv[++i];
            if (value == "udp") {
                config->proto = StreamProto::UDP;
            } else if (value == "tcp") {
                config->proto = StreamProto::TCP;
            } else {
                std::cerr << "Unsupported proto: " << value << std::endl;
                print_usage(argv[0]);
                return false;
            }
            continue;
        }

        if (arg.rfind("--proto=", 0) == 0) {
            std::string value = arg.substr(std::strlen("--proto="));
            if (value == "udp") {
                config->proto = StreamProto::UDP;
            } else if (value == "tcp") {
                config->proto = StreamProto::TCP;
            } else {
                std::cerr << "Unsupported proto: " << value << std::endl;
                print_usage(argv[0]);
                return false;
            }
            continue;
        }

        if (arg == "--host") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value after --host" << std::endl;
                print_usage(argv[0]);
                return false;
            }

            config->host = argv[++i];
            continue;
        }

        if (arg.rfind("--host=", 0) == 0) {
            config->host = arg.substr(std::strlen("--host="));
            continue;
        }

        if (arg == "--port") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value after --port" << std::endl;
                print_usage(argv[0]);
                return false;
            }

            const char *p = argv[++i];
            int parsed_port = 0;
            while (*p >= '0' && *p <= '9') {
                parsed_port = parsed_port * 10 + (*p - '0');
                ++p;
            }

            config->port = parsed_port;
            if (config->port <= 0 || config->port > 65535) {
                std::cerr << "Invalid port: " << config->port << std::endl;
                return false;
            }
            continue;
        }

        if (arg.rfind("--port=", 0) == 0) {
            const char *p = arg.c_str() + std::strlen("--port=");
            int parsed_port = 0;
            while (*p >= '0' && *p <= '9') {
                parsed_port = parsed_port * 10 + (*p - '0');
                ++p;
            }

            config->port = parsed_port;
            if (config->port <= 0 || config->port > 65535) {
                std::cerr << "Invalid port: " << config->port << std::endl;
                return false;
            }
            continue;
        }

        std::cerr << "Unknown argument: " << arg << std::endl;
        print_usage(argv[0]);
        return false;
    }

    return true;
}
