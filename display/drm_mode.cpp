#include "drm_mode.h"

#include "audio_cloud_overlay.h"
#include "autofocus_shared_state.h"
#include "common.h"
#include "overlay_shared_state.h"
#include "text_overlay.h"

#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>
#include <pthread.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

// 功能：
//   根据属性名字从 DRM 对象属性表中查找对应的 property id。
// 参数：
//   fd: DRM 设备文件描述符。
//   props: 通过 drmModeObjectGetProperties 得到的属性表。
//   name: 目标属性名称，如 "rotation"、"zpos"、"alpha"。
// 返回值：
//   找到则返回 property id，找不到返回 0。
// 说明：
//   在 DRM 里，rotation / zpos / alpha / pixel blend mode 等能力都通过 property 暴露。
static uint32_t get_property_id(int fd, drmModeObjectProperties *props, const char *name) {
    uint32_t id = 0;
    for (uint32_t i = 0; i < props->count_props; i++) {
        drmModePropertyRes *prop = drmModeGetProperty(fd, props->props[i]);
        if (prop) {
            if (std::strcmp(prop->name, name) == 0) {
                id = prop->prop_id;
            }
            drmModeFreeProperty(prop);
        }
        if (id) {
            break;
        }
    }
    return id;
}

// 功能：
//   按属性名称读取 DRM 对象当前的属性值。
// 参数：
//   fd: DRM 设备文件描述符。
//   props: 属性表。
//   name: 目标属性名称。
// 返回值：
//   找到则返回该属性当前值，找不到返回 0。
// 说明：
//   这里主要用于读取 plane 的 type，从而区分 Primary 和 Overlay。
static uint64_t get_property_value(int fd, drmModeObjectProperties *props, const char *name) {
    for (uint32_t i = 0; i < props->count_props; i++) {
        drmModePropertyRes *prop = drmModeGetProperty(fd, props->props[i]);
        if (prop) {
            bool matched = std::strcmp(prop->name, name) == 0;
            drmModeFreeProperty(prop);
            if (matched) {
                return props->prop_values[i];
            }
        }
    }
    return 0;
}

// 功能：
//   检查某个 DRM plane 是否支持指定像素格式。
// 参数：
//   plane: 待检查的 DRM plane。
//   format: DRM_FORMAT_* 定义的像素格式。
// 返回值：
//   true 表示支持；false 表示不支持。
// 说明：
//   本工程会用它分别筛选 NV12 视频 plane 和 ARGB8888 overlay plane。
static bool plane_supports_format(drmModePlane *plane, uint32_t format) {
    for (uint32_t j = 0; j < plane->count_formats; j++) {
        if (plane->formats[j] == format) {
            return true;
        }
    }
    return false;
}

// 功能：
//   把云图线程生成好的 ARGB 像素复制到真正用于显示的 dumb buffer。
// 参数：
//   dst: 最终要给 DRM overlay plane 使用的 dumb buffer。
//   width/height: 目标有效区域大小。
//   src_pixels: 云图线程生成的 ARGB 像素数组。
//   src_width/src_height: 源 overlay 尺寸。
// 返回值：
//   无返回值；若源尺寸与目标尺寸不一致，只会先清空目标图像然后直接返回。
// 说明：
//   当前实现里，云图线程只算图，不直接提交 DRM；视频线程在显示循环中消费最新结果。
static void copy_overlay_frame(DumbBuffer *dst,
                               int width,
                               int height,
                               const std::vector<uint32_t> &src_pixels,
                               int src_width,
                               int src_height) {
    clear_argb(dst, width, height, 0x00000000);
    if (src_width != width || src_height != height) {
        return;
    }

    uint32_t *dst_row = dst->pixels;
    const int dst_stride = static_cast<int>(dst->pitch / sizeof(uint32_t));
    for (int y = 0; y < height; ++y) {
        std::memcpy(dst_row + y * dst_stride,
                    &src_pixels[static_cast<size_t>(y) * src_width],
                    static_cast<size_t>(width) * sizeof(uint32_t));
    }
}

// 功能：
//   从当前 NV12 帧中复制中心 ROI 的 Y 分量，并发布给 AF 线程。
// 参数：
//   af_state: AF 共享状态；若为空则不发布。
//   buffer: 当前视频帧的 CPU 可访问缓冲区。
//   pitch: NV12 图像一行的字节跨度。
// 返回值：
//   无返回值。
// 说明：
//   这里使用 try_lock，如果 AF 线程正持锁处理上一帧，视频线程会跳过本帧 ROI 发布，
//   以保证显示链路不因 AF 而卡顿。
static void publish_af_roi_if_possible(AutofocusSharedState *af_state,
                                       const Buffer &buffer,
                                       uint32_t pitch) {
    if (!af_state || !buffer.start) {
        return;
    }

    if (pthread_mutex_trylock(&af_state->mutex) != 0) {
        return;
    }

    const uint8_t *src_y = reinterpret_cast<const uint8_t *>(buffer.start);
    const int roi_x = static_cast<int>((kFrameWidth - kAfRoiWidth) / 2);
    const int roi_y = static_cast<int>((kFrameHeight - kAfRoiHeight) / 2);
    for (int y = 0; y < kAfRoiHeight; ++y) {
        std::memcpy(&af_state->roi_y[y * kAfRoiWidth],
                    src_y + (roi_y + y) * pitch + roi_x,
                    kAfRoiWidth);
    }
    af_state->new_frame = true;
    af_state->published_frames++;
    pthread_mutex_unlock(&af_state->mutex);
    pthread_cond_signal(&af_state->condition);
}

// 功能：
//   DRM 模式的视频显示主线程。
//   完整流程包括：V4L2 采集、DRM 资源枚举、双 plane 显示、云图 overlay 消费、AF ROI 发布。
// 参数：
//   state: 云图共享状态；若非空，则从中读取最新 overlay 叠加到 ARGB overlay plane。
//   af_state: AF 共享状态；若非空，则每帧发布中心 ROI 给 AF 线程。
// 返回值：
//   0  表示主循环正常结束；
//   -1 表示初始化或运行过程中出现错误。
int run_video_display_worker(OverlaySharedState *state, AutofocusSharedState *af_state) {
    std::cout << "Selected mode: drm" << std::endl;
    std::cout << "Starting V4L2 + DRM Zero-Copy Display for IMX415..." << std::endl;

    // 第一步：初始化 V4L2 采集端，输出 NV12。
    // 这里的 NV12 将被同时用于：
    // 1. 直接导入 DRM 做本地显示；
    // 2. 提取中心 ROI 给 AF 线程。
    int v4l2_fd = open(kV4L2Device, O_RDWR | O_NONBLOCK, 0);
    if (v4l2_fd < 0) {
        std::cerr << "Failed to open " << kV4L2Device << std::endl;
        return -1;
    }

    struct v4l2_format fmt;
    std::memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.width = kFrameWidth;
    fmt.fmt.pix_mp.height = kFrameHeight;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    fmt.fmt.pix_mp.field = V4L2_FIELD_ANY;
    fmt.fmt.pix_mp.num_planes = 1;
    if (ioctl(v4l2_fd, VIDIOC_S_FMT, &fmt) < 0) {
        std::cerr << "Failed to set V4L2 format" << std::endl;
        close(v4l2_fd);
        return -1;
    }

    uint32_t af_pitch = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
    if (af_pitch == 0) {
        af_pitch = kFrameWidth;
    }

    // 第二步：初始化 DRM/KMS。
    // DRM 负责“找到显示器 -> 找到扫描输出路径 -> 找到可显示图层 plane”。
    int drm_fd = open(kDrmDevice, O_RDWR | O_CLOEXEC);
    if (drm_fd < 0) {
        std::cerr << "Failed to open " << kDrmDevice << std::endl;
        close(v4l2_fd);
        return -1;
    }

    if (drmSetMaster(drm_fd) < 0) {
        std::cerr << "Failed to set DRM master. Display may not update." << std::endl;
    }
    if (drmSetClientCap(drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) < 0) {
        std::cerr << "Failed to enable universal planes" << std::endl;
    }

    // DRM 资源树大致是：
    // connector -> encoder -> crtc -> plane
    // connector 对应外部显示接口，crtc 可理解为扫描输出控制器，plane 是可叠加图层。
    drmModeRes *res = drmModeGetResources(drm_fd);
    if (!res) {
        std::cerr << "Failed to get DRM resources" << std::endl;
        close(drm_fd);
        close(v4l2_fd);
        return -1;
    }

    drmModeConnector *conn = nullptr;
    for (int i = 0; i < res->count_connectors; i++) {
        conn = drmModeGetConnector(drm_fd, res->connectors[i]);
        if (conn && conn->connection == DRM_MODE_CONNECTED) {
            break;
        }
        drmModeFreeConnector(conn);
        conn = nullptr;
    }

    if (!conn) {
        std::cerr << "No connected DRM connector found" << std::endl;
        drmModeFreeResources(res);
        close(drm_fd);
        close(v4l2_fd);
        return -1;
    }

    drmModeEncoder *enc = drmModeGetEncoder(drm_fd, conn->encoder_id);
    if (!enc) {
        std::cerr << "Failed to get DRM encoder" << std::endl;
        drmModeFreeConnector(conn);
        drmModeFreeResources(res);
        close(drm_fd);
        close(v4l2_fd);
        return -1;
    }

    uint32_t crtc_id = enc->crtc_id;
    drmModeModeInfo mode = conn->modes[0];

    int crtc_index = -1;
    for (int i = 0; i < res->count_crtcs; i++) {
        if (res->crtcs[i] == crtc_id) {
            crtc_index = i;
            break;
        }
    }

    // 第三步：从所有 plane 里挑两个我们能用的图层。
    // - plane_id：主视频层，要求支持 NV12
    // - text_plane_id：叠加层，要求支持 ARGB8888，优先选 Overlay 类型
    drmModePlaneRes *plane_res = drmModeGetPlaneResources(drm_fd);
    uint32_t plane_id = 0;
    uint32_t text_plane_id = 0;

    if (plane_res) {
        for (uint32_t i = 0; i < plane_res->count_planes; i++) {
            drmModePlane *plane = drmModeGetPlane(drm_fd, plane_res->planes[i]);
            if (plane && plane->crtc_id == crtc_id && plane_supports_format(plane, DRM_FORMAT_NV12)) {
                plane_id = plane->plane_id;
                drmModeFreePlane(plane);
                break;
            }
            if (plane) {
                drmModeFreePlane(plane);
            }
        }

        if (plane_id == 0) {
            for (uint32_t i = 0; i < plane_res->count_planes; i++) {
                drmModePlane *plane = drmModeGetPlane(drm_fd, plane_res->planes[i]);
                if (plane && (plane->possible_crtcs & (1 << crtc_index)) &&
                    plane_supports_format(plane, DRM_FORMAT_NV12)) {
                    plane_id = plane->plane_id;
                    drmModeFreePlane(plane);
                    break;
                }
                if (plane) {
                    drmModeFreePlane(plane);
                }
            }
        }

        for (uint32_t i = 0; i < plane_res->count_planes; i++) {
            drmModePlane *plane = drmModeGetPlane(drm_fd, plane_res->planes[i]);
            if (plane && plane->plane_id != plane_id && (plane->possible_crtcs & (1 << crtc_index))) {
                bool supports_ar24 = plane_supports_format(plane, DRM_FORMAT_ARGB8888);
                bool prefer_overlay = false;
                if (supports_ar24) {
                    drmModeObjectProperties *plane_props =
                        drmModeObjectGetProperties(drm_fd, plane->plane_id, DRM_MODE_OBJECT_PLANE);
                    if (plane_props) {
                        uint64_t plane_type = get_property_value(drm_fd, plane_props, "type");
                        prefer_overlay = (plane_type == DRM_PLANE_TYPE_OVERLAY);
                        drmModeFreeObjectProperties(plane_props);
                    }
                }
                if (supports_ar24 && prefer_overlay) {
                    text_plane_id = plane->plane_id;
                    drmModeFreePlane(plane);
                    break;
                }
            }
            if (plane) {
                drmModeFreePlane(plane);
            }
        }

        if (text_plane_id == 0) {
            for (uint32_t i = 0; i < plane_res->count_planes; i++) {
                drmModePlane *plane = drmModeGetPlane(drm_fd, plane_res->planes[i]);
                if (plane && plane->plane_id != plane_id && (plane->possible_crtcs & (1 << crtc_index)) &&
                    plane_supports_format(plane, DRM_FORMAT_ARGB8888)) {
                    text_plane_id = plane->plane_id;
                    drmModeFreePlane(plane);
                    break;
                }
                if (plane) {
                    drmModeFreePlane(plane);
                }
            }
        }

        drmModeFreePlaneResources(plane_res);
    }

    if (plane_id == 0) {
        std::cerr << "No compatible plane found for CRTC " << crtc_id << std::endl;
        drmModeFreeEncoder(enc);
        drmModeFreeConnector(conn);
        drmModeFreeResources(res);
        close(drm_fd);
        close(v4l2_fd);
        return -1;
    }

    std::cout << "DRM Display Mode: " << mode.hdisplay << "x" << mode.vdisplay << std::endl;
    std::cout << "Using CRTC " << crtc_id << ", Video Plane " << plane_id << std::endl;
    if (text_plane_id) {
        std::cout << "Using Overlay Plane " << text_plane_id << std::endl;
    }

    // 第四步：为 overlay plane 创建一个 CPU 可写的 ARGB8888 dumb buffer。
    // dumb buffer 的优点是：创建简单、能 mmap，适合画文字/热点图。
    DumbBuffer text_buf;
    const int text_w = mode.hdisplay;
    const int text_h = mode.vdisplay;

    if (text_plane_id) {
        struct drm_mode_create_dumb create_arg = {0};
        create_arg.width = text_w;
        create_arg.height = text_h;
        create_arg.bpp = 32;
        if (drmIoctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &create_arg) == 0) {
            text_buf.handle = create_arg.handle;
            text_buf.pitch = create_arg.pitch;
            text_buf.size = create_arg.size;

            // 把 dumb buffer 注册成 framebuffer 后，plane 才能真正拿它去显示。
            if (drmModeAddFB(drm_fd, text_w, text_h, 24, 32, text_buf.pitch, text_buf.handle,
                             &text_buf.fb_id) == 0) {
                struct drm_mode_map_dumb map_arg = {0};
                map_arg.handle = text_buf.handle;
                if (drmIoctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &map_arg) == 0) {
                    text_buf.pixels = reinterpret_cast<uint32_t *>(
                        mmap(0, text_buf.size, PROT_READ | PROT_WRITE, MAP_SHARED, drm_fd, map_arg.offset));
                    if (text_buf.pixels != MAP_FAILED) {
                        clear_argb(&text_buf, text_w, text_h, 0x00000000);
                        // 把 overlay 尺寸告诉云图线程。
                        // 云图线程等到 ready 后，才开始按 LCD 实际尺寸生成热点图。
                        // ===== 与云图线程交互起点（初始化阶段）=====
                        // 这里把 overlay 的显示尺寸与 ready 状态发布到 OverlaySharedState，
                        // 用于唤醒 run_audio_overlay_worker() 的启动等待。
                        if (state) {
                            pthread_mutex_lock(&state->mutex);
                            state->width = text_w;
                            state->height = text_h;
                            state->ready = true;
                            pthread_mutex_unlock(&state->mutex);
                        }
                        if (state) {
                            pthread_cond_broadcast(&state->condition);
                        }
                        std::cout << "Created Dumb Buffer FB for sound source overlay on Plane "
                                  << text_plane_id << std::endl;
                    } else {
                        text_buf.pixels = nullptr;
                    }
                }
            }
        }
    } else {
        std::cerr << "No secondary ARGB plane found for FPS overlay!" << std::endl;
    }

    // 第五步：申请 V4L2 采集缓冲区。
    // 本工程选择 MMAP，这样既能让 CPU 访问 ROI，也能继续导出 DMA-BUF 给 DRM。
    struct v4l2_requestbuffers req;
    std::memset(&req, 0, sizeof(req));
    req.count = kBufferCount;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(v4l2_fd, VIDIOC_REQBUFS, &req) < 0) {
        std::cerr << "Failed to request V4L2 buffers" << std::endl;
        drmModeFreeEncoder(enc);
        drmModeFreeConnector(conn);
        drmModeFreeResources(res);
        close(drm_fd);
        close(v4l2_fd);
        return -1;
    }

    std::vector<Buffer> buffers(kBufferCount);
    for (uint32_t i = 0; i < kBufferCount; i++) {
        struct v4l2_plane planes[1];
        struct v4l2_buffer buf;
        std::memset(&buf, 0, sizeof(buf));
        std::memset(planes, 0, sizeof(planes));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        buf.m.planes = planes;
        buf.length = 1;

        if (ioctl(v4l2_fd, VIDIOC_QUERYBUF, &buf) < 0) {
            std::cerr << "Failed to query buffer " << i << std::endl;
            drmModeFreeEncoder(enc);
            drmModeFreeConnector(conn);
            drmModeFreeResources(res);
            close(drm_fd);
            close(v4l2_fd);
            return -1;
        }

        // 为 AF 线程额外 mmap 一份 CPU 可访问地址。
        // 这一步不影响后面 DMA-BUF 零拷贝显示，只是给应用层多一个读 ROI 的入口。
        buffers[i].length = buf.m.planes[0].length;
        buffers[i].start = mmap(NULL, buffers[i].length, PROT_READ | PROT_WRITE,
                                MAP_SHARED, v4l2_fd, buf.m.planes[0].m.mem_offset);
        if (buffers[i].start == MAP_FAILED) {
            buffers[i].start = nullptr;
            std::cerr << "Failed to mmap V4L2 buffer " << i << " for AF ROI" << std::endl;
            drmModeFreeEncoder(enc);
            drmModeFreeConnector(conn);
            drmModeFreeResources(res);
            close(drm_fd);
            close(v4l2_fd);
            return -1;
        }

        // 关键步骤：把 V4L2 buffer 导出成 DMA-BUF fd。
        // 这样这个 NV12 帧就可以跨子系统共享给 DRM，不用 CPU 再拷一遍整帧。
        struct v4l2_exportbuffer expbuf;
        std::memset(&expbuf, 0, sizeof(expbuf));
        expbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        expbuf.index = i;
        expbuf.plane = 0;
        expbuf.flags = O_CLOEXEC | O_RDWR;
        if (ioctl(v4l2_fd, VIDIOC_EXPBUF, &expbuf) < 0) {
            std::cerr << "Failed to export buffer " << i << std::endl;
            drmModeFreeEncoder(enc);
            drmModeFreeConnector(conn);
            drmModeFreeResources(res);
            close(drm_fd);
            close(v4l2_fd);
            return -1;
        }
        buffers[i].v4l2_fd = expbuf.fd;

        // DRM 侧把 DMA-BUF fd 导入成 GEM handle。
        // 之后再用 drmModeAddFB2 把它注册为一个真正的 DRM framebuffer。
        if (drmPrimeFDToHandle(drm_fd, buffers[i].v4l2_fd, &buffers[i].drm_handle) < 0) {
            std::cerr << "Failed to import DMA-BUF to DRM" << std::endl;
            drmModeFreeEncoder(enc);
            drmModeFreeConnector(conn);
            drmModeFreeResources(res);
            close(drm_fd);
            close(v4l2_fd);
            return -1;
        }

        uint32_t handles[4] = {buffers[i].drm_handle, buffers[i].drm_handle, 0, 0};
        uint32_t pitches[4] = {af_pitch, af_pitch, 0, 0};
        uint32_t offsets[4] = {0, af_pitch * kFrameHeight, 0, 0};
        // NV12 是双平面格式：
        // plane0 是 Y，plane1 是 UV。
        // 这里通过 handles/pitches/offsets 把同一个 DMA-BUF 描述成 NV12 framebuffer。
        if (drmModeAddFB2(drm_fd, kFrameWidth, kFrameHeight, DRM_FORMAT_NV12,
                          handles, pitches, offsets, &buffers[i].drm_fb_id, 0) < 0) {
            std::cerr << "Failed to add DRM FB" << std::endl;
            drmModeFreeEncoder(enc);
            drmModeFreeConnector(conn);
            drmModeFreeResources(res);
            close(drm_fd);
            close(v4l2_fd);
            return -1;
        }

        if (ioctl(v4l2_fd, VIDIOC_QBUF, &buf) < 0) {
            std::cerr << "Failed to queue buffer" << std::endl;
            drmModeFreeEncoder(enc);
            drmModeFreeConnector(conn);
            drmModeFreeResources(res);
            close(drm_fd);
            close(v4l2_fd);
            return -1;
        }
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (ioctl(v4l2_fd, VIDIOC_STREAMON, &type) < 0) {
        std::cerr << "Failed to start streaming" << std::endl;
        drmModeFreeEncoder(enc);
        drmModeFreeConnector(conn);
        drmModeFreeResources(res);
        close(drm_fd);
        close(v4l2_fd);
        return -1;
    }

    std::cout << "Streaming started. Press Ctrl+C to stop." << std::endl;

    struct timeval last_time, current_time;
    gettimeofday(&last_time, NULL);
    int frame_count = 0;
    bool first_frame = true;

    uint64_t applied_overlay_generation = 0;
    std::string fps_text = "FPS: --.-";

    // 第六步：进入显示主循环。
    // 每轮做的事是：
    // 1. 等待 V4L2 出一帧；
    // 2. DQBUF 取出这一帧；
    // 3. 发布中心 ROI 给 AF 线程；
    // 4. 把该帧对应的 DRM framebuffer 切到视频 plane；
    // 5. 如果云图线程生成了新 overlay，就同步刷新 overlay plane 的像素内容；
    // 6. QBUF 把采集缓冲还回驱动。
    while (!state || state->running.load()) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(v4l2_fd, &fds);
        struct timeval tv = {2, 0};
        int r = select(v4l2_fd + 1, &fds, NULL, NULL, &tv);
        if (r <= 0) {
            std::cerr << "Select timeout or error" << std::endl;
            break;
        }

        struct v4l2_plane planes[1];
        struct v4l2_buffer buf;
        std::memset(&buf, 0, sizeof(buf));
        std::memset(planes, 0, sizeof(planes));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.m.planes = planes;
        buf.length = 1;

        if (ioctl(v4l2_fd, VIDIOC_DQBUF, &buf) < 0) {
            std::cerr << "Failed to dequeue buffer" << std::endl;
            break;
        }

        // ===== 与 AF 线程交互起点（逐帧阶段）=====
        // ROI 发布在 DQBUF 之后、QBUF 之前完成，
        // 这样 AF 线程看到的一定是当前这张有效帧的中心区域。
        publish_af_roi_if_possible(af_state, buffers[buf.index], af_pitch);

        uint32_t dst_w = mode.hdisplay;
        uint32_t dst_h = mode.vdisplay;
        uint32_t dst_x = 0;
        uint32_t dst_y = 0;

        if (first_frame) {
            // 首帧时做一次比较完整的 plane 初始化：
            // - rotation
            // - zpos
            // - overlay alpha/blend mode
            // 后续帧只需要不断切 video plane 的 framebuffer 即可。
            drmModeObjectProperties *props =
                drmModeObjectGetProperties(drm_fd, plane_id, DRM_MODE_OBJECT_PLANE);
            if (props) {
                uint32_t rot_prop_id = get_property_id(drm_fd, props, "rotation");
                if (rot_prop_id) {
                    if (drmModeObjectSetProperty(drm_fd, plane_id, DRM_MODE_OBJECT_PLANE,
                                                 rot_prop_id, 1 << 3) == 0) {
                        if (state) {
                            state->rotate_270.store(true);
                        }
                        std::cout << "Successfully set plane rotation to 270 degrees (Counter-Clockwise 90)."
                                  << std::endl;
                    } else {
                        std::cerr << "Failed to set rotation property." << std::endl;
                    }
                }
                drmModeFreeObjectProperties(props);
            }

            // 第一次把 NV12 帧挂到视频 plane 上。
            // 这里 src 用 16.16 定点表示源裁剪矩形，dst 是 LCD 上的目标矩形。
            if (drmModeSetPlane(drm_fd, plane_id, crtc_id, buffers[buf.index].drm_fb_id, 0,
                                dst_x, dst_y, dst_w, dst_h,
                                0 << 16, 0 << 16, kFrameWidth << 16, kFrameHeight << 16) < 0) {
                std::cerr << "Failed to set DRM plane." << std::endl;
            }

            drmModeObjectProperties *v_props =
                drmModeObjectGetProperties(drm_fd, plane_id, DRM_MODE_OBJECT_PLANE);
            if (v_props) {
                uint32_t zpos_prop_id = get_property_id(drm_fd, v_props, "ZPOS");
                if (!zpos_prop_id) {
                    zpos_prop_id = get_property_id(drm_fd, v_props, "zpos");
                }
                if (zpos_prop_id) {
                    drmModeObjectSetProperty(drm_fd, plane_id, DRM_MODE_OBJECT_PLANE, zpos_prop_id, 1);
                }
                drmModeFreeObjectProperties(v_props);
            }

            if (text_plane_id && text_buf.pixels) {
                uint32_t text_dst_x = 0;
                uint32_t text_dst_y = 0;

                drmModeObjectProperties *t_props =
                    drmModeObjectGetProperties(drm_fd, text_plane_id, DRM_MODE_OBJECT_PLANE);
                if (t_props) {
                    uint32_t zpos_prop_id = get_property_id(drm_fd, t_props, "ZPOS");
                    if (!zpos_prop_id) {
                        zpos_prop_id = get_property_id(drm_fd, t_props, "zpos");
                    }
                    if (zpos_prop_id) {
                        drmModeObjectSetProperty(drm_fd, text_plane_id, DRM_MODE_OBJECT_PLANE,
                                                 zpos_prop_id, 2);
                    }
                    uint32_t alpha_prop_id = get_property_id(drm_fd, t_props, "alpha");
                    if (alpha_prop_id) {
                        drmModeObjectSetProperty(drm_fd, text_plane_id, DRM_MODE_OBJECT_PLANE,
                                                 alpha_prop_id, 65535);
                    }
                    uint32_t blend_prop_id = get_property_id(drm_fd, t_props, "pixel blend mode");
                    if (blend_prop_id) {
                        drmModeObjectSetProperty(drm_fd, text_plane_id, DRM_MODE_OBJECT_PLANE,
                                                 blend_prop_id, 1);
                    }
                    drmModeFreeObjectProperties(t_props);
                }

                // 再把 ARGB overlay framebuffer 挂到第二层 plane 上。
                // 视频层 zpos=1，overlay 层 zpos=2，因此能叠在视频上面。
                if (drmModeSetPlane(drm_fd, text_plane_id, crtc_id, text_buf.fb_id, 0,
                                    text_dst_x, text_dst_y, text_w, text_h,
                                    0 << 16, 0 << 16, text_w << 16, text_h << 16) < 0) {
                    std::cerr << "Failed to set DRM text plane." << std::endl;
                }
            }

            first_frame = false;
        } else {
            // 后续帧只切换视频 plane 指向的 framebuffer，就能完成流畅显示。
            drmModeSetPlane(drm_fd, plane_id, crtc_id, buffers[buf.index].drm_fb_id, 0,
                            dst_x, dst_y, dst_w, dst_h,
                            0 << 16, 0 << 16, kFrameWidth << 16, kFrameHeight << 16);
            usleep(10000);
        }

        frame_count++;
        gettimeofday(&current_time, NULL);
        double elapsed = (current_time.tv_sec - last_time.tv_sec) +
                         (current_time.tv_usec - last_time.tv_usec) / 1000000.0;

        if (elapsed >= 1.0) {
            float fps = frame_count / elapsed;
            char fps_str[32];
            std::snprintf(fps_str, sizeof(fps_str), "FPS: %.1f", fps);
            fps_text = fps_str;
            frame_count = 0;
            last_time = current_time;
        }

        // overlay 的“显示提交”目前仍由视频线程完成：
        // 视频线程读取云图线程生成的最新 ARGB 像素，再写进 text_buf。
        // 所以当前实现是“云图计算独立，但上屏提交通路还依附于视频线程”。
        // ===== 与云图线程交互起点（逐帧阶段）=====
        // 视频线程在显示循环里尝试读取 OverlaySharedState 的最新 generation/pixels，
        // 若有新图则更新 text_buf（overlay plane 的内容）。
        if (text_buf.pixels && state) {
            std::vector<uint32_t> overlay_pixels;
            int overlay_width = 0;
            int overlay_height = 0;
            uint64_t overlay_generation = 0;
            {
                if (pthread_mutex_trylock(&state->mutex) == 0) {
                    overlay_generation = state->generation;
                    if (overlay_generation != applied_overlay_generation && !state->pixels.empty()) {
                        overlay_pixels = state->pixels;
                        overlay_width = state->width;
                        overlay_height = state->height;
                    }
                    pthread_mutex_unlock(&state->mutex);
                }
            }
#if 1
            if (!overlay_pixels.empty() && overlay_generation != applied_overlay_generation) {
                copy_overlay_frame(&text_buf, text_w, text_h, overlay_pixels, overlay_width, overlay_height);
                draw_text_at(&text_buf, text_w, text_h, 22, 22, fps_text.c_str(), 0xDCFFFFFF);
                applied_overlay_generation = overlay_generation;
            }
#endif
        }

        if (ioctl(v4l2_fd, VIDIOC_QBUF, &buf) < 0) {
            std::cerr << "Failed to re-queue buffer" << std::endl;
            break;
        }
    }

    for (size_t i = 0; i < buffers.size(); ++i) {
        if (buffers[i].start) {
            munmap(buffers[i].start, buffers[i].length);
        }
        if (buffers[i].v4l2_fd >= 0) {
            close(buffers[i].v4l2_fd);
        }
    }

    return 0;
}
