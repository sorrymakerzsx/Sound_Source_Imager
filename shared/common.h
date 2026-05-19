#ifndef V4L2_DRM_IMX415_COMMON_H
#define V4L2_DRM_IMX415_COMMON_H

#include <cstddef>
#include <cstdint>
#include <string>

// 一个最基础的 DRM dumb buffer。
// 用于 ARGB8888 叠加层，因为 dumb buffer 便于 CPU 直接 mmap 后写像素。
struct DumbBuffer {
    uint32_t handle = 0;
    uint32_t pitch = 0;
    uint32_t size = 0;
    uint32_t fb_id = 0;
    uint32_t *pixels = nullptr;
};

// 一个视频帧在本工程里的“双重身份”：
// 1. v4l2_fd / drm_handle / drm_fb_id 用于 V4L2 -> DMA-BUF -> DRM 零拷贝显示；
// 2. start / length 允许 CPU 直接访问该帧，用于 AF 线程提取 ROI。
struct Buffer {
    int v4l2_fd = -1;
    uint32_t drm_handle = 0;
    uint32_t drm_fb_id = 0;
    void *start = nullptr;
    size_t length = 0;
};

enum class OutputMode {
    DRM,
    STREAM,
};

enum class StreamProto {
    UDP,
    TCP,
};

// 应用层运行参数：
// mode 决定走本地 DRM 显示还是网络推流；
// proto/host/port 只在 stream 模式下使用。
struct AppConfig {
    OutputMode mode = OutputMode::DRM;
    StreamProto proto = StreamProto::UDP;
    std::string host = "192.168.1.100";
    int port = 5000;
};

struct StreamBuffer {
    void *start = nullptr;
    size_t length = 0;
};

// 当前工程默认设备和图像参数。
// video-camera0 提供 NV12 视频帧，card0 提供 LCD 对应的 DRM/KMS 设备。
static const char kV4L2Device[] = "/dev/video-camera0";
static const char kDrmDevice[] = "/dev/dri/card0";
static const uint32_t kFrameWidth = 1920;
static const uint32_t kFrameHeight = 1080;
static const uint32_t kBufferCount = 4;

#endif
