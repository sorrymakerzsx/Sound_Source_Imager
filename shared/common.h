#ifndef V4L2_DRM_IMX415_COMMON_H
#define V4L2_DRM_IMX415_COMMON_H

#include <cstddef>
#include <cstdint>
#include <string>

// 一个最基础的 DRM dumb buffer。
// 用于 ARGB8888 叠加层，因为 dumb buffer 便于 CPU 直接 mmap 后写像素。
struct DumbBuffer {
    // DRM dumb buffer 的 GEM 句柄，供创建 framebuffer 和释放资源时使用。
    uint32_t handle = 0;
    // 每行字节数，通常会大于等于 width * 4。
    uint32_t pitch = 0;
    // 整块 buffer 的总字节数。
    uint32_t size = 0;
    // 与该 dumb buffer 绑定的 DRM framebuffer ID。
    uint32_t fb_id = 0;
    // mmap 后得到的 CPU 可访问像素首地址，格式ARGB8888。
    uint32_t *pixels = nullptr;
};

// 一个视频帧在本工程里的“双重身份”：
// 1. v4l2_fd / drm_handle / drm_fb_id 用于 V4L2 -> DMA-BUF -> DRM 零拷贝显示；
// 2. start / length 允许 CPU 直接访问该帧，用于 AF 线程提取 ROI。
struct Buffer {
    // 由 VIDIOC_EXPBUF 导出的 dma-buf fd，供 DRM 导入显示。
    int v4l2_fd = -1;
    // DRM 侧导入 dma-buf 后得到的 GEM handle。
    uint32_t drm_handle = 0;
    // 与当前视频帧绑定的 DRM framebuffer ID。
    uint32_t drm_fb_id = 0;
    // V4L2 MMAP 后的 CPU 虚拟地址，便于直接读取 NV12 数据。
    void *start = nullptr;
    // 当前视频缓冲区长度，单位字节。
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
    // 运行模式：本地 DRM 显示或网络推流。
    OutputMode mode = OutputMode::DRM;
    // stream 模式下使用的传输协议。
    StreamProto proto = StreamProto::UDP;
    // stream 模式下的目标主机 IP，作为服务端时也用于 bind 判断。
    std::string host = "192.168.1.100";
    // stream 模式下的目标端口号。
    int port = 5000;
};

struct StreamBuffer {
    // MPP 编码输出码流的首地址。
    void *start = nullptr;
    // 该段码流的有效长度，单位字节。
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
