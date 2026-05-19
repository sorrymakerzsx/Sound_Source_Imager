#ifndef V4L2_DRM_IMX415_APP_CONFIG_H
#define V4L2_DRM_IMX415_APP_CONFIG_H

#include "common.h"

// 功能：
//   打印程序帮助信息。
// 参数：
//   program: 当前程序名，通常传入 argv[0]。
// 返回值：
//   无返回值。
void print_usage(const char *program);

// 功能：
//   解析命令行参数并填充运行配置。
// 参数：
//   argc: 命令行参数个数。
//   argv: 命令行参数数组。
//   config: 输出配置结构，解析成功后写入 mode/proto/host/port。
// 返回值：
//   true  表示解析成功；
//   false 表示参数非法或用户请求帮助。
bool parse_app_config(int argc, char **argv, AppConfig *config);

#endif
