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
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#include <cstdio>
#include <cstring>
#include <iostream>
#include <vector>

// 按名字查找 DRM property id（用于 rotation / zpos / alpha 等属性）。
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

// 按名字读取 DRM 对象属性值，主要用于读 plane type（区分 Primary / Overlay）。
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

// 检查 plane 是否支持指定格式（NV12 / ARGB8888）。
static bool plane_supports_format(drmModePlane *plane, uint32_t format) {
    for (uint32_t j = 0; j < plane->count_formats; j++) {
        if (plane->formats[j] == format) {
            return true;
        }
    }
    return false;
}

// 把云图 ARGB 像素拷贝到 DRM dumb buffer 上屏。
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

// 从 NV12 帧中心抠 320×240 Y 给 AF 线程。
// 写入 roi_y[1 - write_idx] 这个非活跃槽，写完翻转 write_idx + 置 new_frame。
// AF 线程跟视频线程永远不会踩同一个 roi_y 槽，所以不用锁。
static void publish_af_roi_if_possible(AutofocusSharedState *af_state,
                                       const Buffer &buffer,
                                       uint32_t pitch) {
    if (!af_state || !buffer.start) {
        return;
    }

    const int next = 1 - __atomic_load_n(&af_state->write_idx, __ATOMIC_ACQUIRE);
    const uint8_t *src_y = reinterpret_cast<const uint8_t *>(buffer.start);     //y分量首地址
    const int roi_x = static_cast<int>((kFrameWidth - kAfRoiWidth) / 2);        //中心 ROI 的 X 起点
    const int roi_y_off = static_cast<int>((kFrameHeight - kAfRoiHeight) / 2);  //中心ROI的Y起点
    for (int y = 0; y < kAfRoiHeight; ++y) {
        std::memcpy(&af_state->roi_y[next][y * kAfRoiWidth],
                    src_y + (roi_y_off + y) * pitch + roi_x,
                    kAfRoiWidth);
    }
    __atomic_store_n(&af_state->write_idx, next, __ATOMIC_RELEASE);     //翻转 write_idx，让 AF 线程知道新数据来了
    __atomic_store_n(&af_state->new_frame, true, __ATOMIC_RELEASE);
    __atomic_fetch_add(&af_state->published_frames, 1, __ATOMIC_RELAXED);
}

// DRM 模式主线程：V4L2 采集 → DRM 双 plane 显示（视频层 + 云图叠加层），
// 同时逐帧给 AF 线程喂中心 ROI。
int run_video_display_worker(OverlaySharedState *state, AutofocusSharedState *af_state) {
    std::cout << "Selected mode: drm" << std::endl;
    std::cout << "Starting V4L2 + DRM Zero-Copy Display for IMX415..." << std::endl;

    // ---- V4L2 采集初始化 ----
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

    uint32_t af_pitch = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;   //获取每行字节数（pitch），用于 AF ROI 计算
    if (af_pitch == 0) {
        af_pitch = kFrameWidth;
    }

    // ---- DRM/KMS 初始化 ----
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

    // DRM 拓扑：connector → encoder → crtc → plane
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

    // ---- 挑选 plane：NV12 视频层 + ARGB 叠加层 ----
    drmModePlaneRes *plane_res = drmModeGetPlaneResources(drm_fd);
    uint32_t plane_id = 0;
    uint32_t text_plane_id = 0;

    if (plane_res) {
        // 先找关联当前 crtc 且支持 NV12 的 plane
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

        // 再找支持 ARGB8888 的 overlay 层（优先 Overlay 类型）
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

        // 没 Overlay 类型就随便挑一个支持 ARGB 的
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

    // ---- overlay plane: 创建 CPU 可写的 ARGB dumb buffer ----
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

            if (drmModeAddFB(drm_fd, text_w, text_h, 24, 32, text_buf.pitch, text_buf.handle,
                             &text_buf.fb_id) == 0) {
                struct drm_mode_map_dumb map_arg = {0};
                map_arg.handle = text_buf.handle;
                if (drmIoctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &map_arg) == 0) {
                    text_buf.pixels = reinterpret_cast<uint32_t *>(
                        mmap(0, text_buf.size, PROT_READ | PROT_WRITE, MAP_SHARED, drm_fd, map_arg.offset));
                    if (text_buf.pixels != MAP_FAILED) {
                        clear_argb(&text_buf, text_w, text_h, 0x00000000);
                        // 告诉云图线程尺寸已到位，可以开始画图了
                        if (state) {
                            state->width = text_w;
                            state->height = text_h;
                            __atomic_store_n(&state->ready, true, __ATOMIC_RELEASE);
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

    // ---- V4L2 缓冲区申请（MMAP）----
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
    //初始化进程中的缓存区
    std::vector<Buffer> buffers(kBufferCount);      //数量4
    for (uint32_t i = 0; i < kBufferCount; i++) {
        //===========遍历 4 个缓冲区，逐个初始化。===========
        struct v4l2_plane planes[1];    //单平面数组（虽然 NV12 逻辑上有 Y 和 UV 两个平面，但这里用的是 multi-planar API，plane 0 代表整个缓冲区）
        struct v4l2_buffer buf;         //V4L2 缓冲区描述结构，用于 ioctl 与内核通信
        std::memset(&buf, 0, sizeof(buf));
        std::memset(planes, 0, sizeof(planes));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;  //视频捕获，多平面 API（mplane）
        buf.memory = V4L2_MEMORY_MMAP;  //MMAP映射方式
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
        // CPU 侧 mmap，供 AF ROI 读取
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

        // 导出 DMA-BUF，供 DRM 零拷贝显示
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

        // 导入 DRM → GEM handle → framebuffer（NV12 双平面）
        if (drmPrimeFDToHandle(drm_fd, buffers[i].v4l2_fd, &buffers[i].drm_handle) < 0) {   //把DMA-BUF描述符转换成DRM内部的GEN handle（整数句柄）
            std::cerr << "Failed to import DMA-BUF to DRM" << std::endl;
            drmModeFreeEncoder(enc);
            drmModeFreeConnector(conn);
            drmModeFreeResources(res);
            close(drm_fd);
            close(v4l2_fd);
            return -1;
        }
        //用导入的GEM handle创建DRM framebuffer对象，指定格式为NV12。DRM会把这个framebuffer和之前导入的GEM handle关联起来，后续显示时直接用这个framebuffer ID即可。
        uint32_t handles[4] = {buffers[i].drm_handle, buffers[i].drm_handle, 0, 0};
        uint32_t pitches[4] = {af_pitch, af_pitch, 0, 0};
        uint32_t offsets[4] = {0, af_pitch * kFrameHeight, 0, 0};
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
    char fps_text[32] = "FPS: --.-";

    // ---- 显示主循环 ----
    while (!state || __atomic_load_n(&state->running, __ATOMIC_SEQ_CST)) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(v4l2_fd, &fds);
        struct timeval tv = {2, 0};
        int r = select(v4l2_fd + 1, &fds, NULL, NULL, &tv); //等待 V4L2 采集到一帧数据，超时为 2 秒
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

        // 把中心 ROI 发给 AF 线程
        publish_af_roi_if_possible(af_state, buffers[buf.index], af_pitch);

        uint32_t dst_w = mode.hdisplay;
        uint32_t dst_h = mode.vdisplay;
        uint32_t dst_x = 0;
        uint32_t dst_y = 0;

        if (first_frame) {
            // 首帧做完整 plane 属性配置：rotation / zpos / alpha / blend mode
            drmModeObjectProperties *props =
                drmModeObjectGetProperties(drm_fd, plane_id, DRM_MODE_OBJECT_PLANE);
            if (props) {
                uint32_t rot_prop_id = get_property_id(drm_fd, props, "rotation");
                if (rot_prop_id) {
                    if (drmModeObjectSetProperty(drm_fd, plane_id, DRM_MODE_OBJECT_PLANE,
                                                 rot_prop_id, 1 << 3) == 0) {
                        if (state) {
                            __atomic_store_n(&state->rotate_270, true, __ATOMIC_SEQ_CST);
                        }
                        std::cout << "Successfully set plane rotation to 270 degrees (Counter-Clockwise 90)."
                                  << std::endl;
                    } else {
                        std::cerr << "Failed to set rotation property." << std::endl;
                    }
                }
                drmModeFreeObjectProperties(props);
            }

            // 视频 plane 挂 NV12 framebuffer
            if (drmModeSetPlane(drm_fd, plane_id, crtc_id, buffers[buf.index].drm_fb_id, 0,
                                dst_x, dst_y, dst_w, dst_h,
                                0 << 16, 0 << 16, kFrameWidth << 16, kFrameHeight << 16) < 0) {
                std::cerr << "Failed to set DRM plane." << std::endl;
            }

            // 视频层 zpos = 1
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

            // 叠加层 zpos = 2, alpha = 100%, blend = premultiplied
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

                if (drmModeSetPlane(drm_fd, text_plane_id, crtc_id, text_buf.fb_id, 0,
                                    text_dst_x, text_dst_y, text_w, text_h,
                                    0 << 16, 0 << 16, text_w << 16, text_h << 16) < 0) {
                    std::cerr << "Failed to set DRM text plane." << std::endl;
                }
            }

            first_frame = false;
        } else {
            // 后续帧只切视频 plane 的 framebuffer
            drmModeSetPlane(drm_fd, plane_id, crtc_id, buffers[buf.index].drm_fb_id, 0,
                            dst_x, dst_y, dst_w, dst_h,
                            0 << 16, 0 << 16, kFrameWidth << 16, kFrameHeight << 16);
            usleep(10000);
        }

        // FPS 统计
        frame_count++;
        gettimeofday(&current_time, NULL);
        double elapsed = (current_time.tv_sec - last_time.tv_sec) +
                         (current_time.tv_usec - last_time.tv_usec) / 1000000.0;

        if (elapsed >= 1.0) {
            float fps = frame_count / elapsed;
            snprintf(fps_text, sizeof(fps_text), "FPS: %.1f", fps);
            frame_count = 0;
            last_time = current_time;
        }

        // 无锁读取云图线程发布的最新 overlay ——
        // generation 变过说明有新图，从 pixels[write_idx]（活跃槽）拷贝，
        // 音频线程同时写的是 pixels[1 - write_idx]，不冲突。
        if (text_buf.pixels && state) {
            uint64_t overlay_generation = __atomic_load_n(&state->generation, __ATOMIC_ACQUIRE);
            if (overlay_generation != applied_overlay_generation) {
                int idx = __atomic_load_n(&state->write_idx, __ATOMIC_ACQUIRE);
                std::vector<uint32_t> overlay_pixels = state->pixels[idx];
                int overlay_width = state->width;
                int overlay_height = state->height;
                if (!overlay_pixels.empty()) {  //防止从还没初始化过的槽里拿空数据去渲染
                    copy_overlay_frame(&text_buf, text_w, text_h, overlay_pixels, overlay_width, overlay_height);
                    draw_text_at(&text_buf, text_w, text_h, 22, 22, fps_text, 0xDCFFFFFF);
                    applied_overlay_generation = overlay_generation;
                }
            }
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
