#include "stream_mode.h"

#include "nv12_overlay_blend.h"
#include "overlay_shared_state.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#define MODULE_TAG "v4l2_drm_imx415"
#include "mpp_buffer.h"
#include "mpp_frame.h"
#include "mpp_packet.h"
#include "rk_mpi.h"
#include "rk_venc_cfg.h"
#include "rk_venc_rc.h"

static uint32_t align_up(uint32_t value, uint32_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

// UDP 分片发送，按 1400 字节切分避免 IP 分片。
static bool send_udp_buffer(int sockfd, const struct sockaddr_in &addr, const uint8_t *data, size_t length) {
    const size_t kUdpChunkSize = 1400;

    while (length > 0) {
        size_t chunk = length > kUdpChunkSize ? kUdpChunkSize : length;
        ssize_t sent = sendto(sockfd, data, chunk, 0,
                              reinterpret_cast<const struct sockaddr *>(&addr), sizeof(addr));
        if (sent < 0 || static_cast<size_t>(sent) != chunk) {
            return false;
        }
        data += chunk;
        length -= chunk;
    }

    return true;
}

// TCP 阻塞发送，循环直到全部写完。MSG_NOSIGNAL 避免对端断开时触发 SIGPIPE。
static bool send_tcp_all(int sockfd, const uint8_t *data, size_t length) {
    while (length > 0) {
        ssize_t sent = send(sockfd, data, length, MSG_NOSIGNAL);
        if (sent <= 0) {
            return false;
        }
        data += static_cast<size_t>(sent);
        length -= static_cast<size_t>(sent);
    }
    return true;
}

// Stream 模式：V4L2 采集 → 可选云图叠加 → MPP H.264 编码 → UDP/TCP 推流。
int run_stream_mode(const AppConfig &config, OverlaySharedState *state) {
    std::cout << "Selected mode: stream" << std::endl;
    std::cout << "Streaming H.264 to " << config.host << ":" << config.port << std::endl;

    int v4l2_fd = open(kV4L2Device, O_RDWR | O_NONBLOCK, 0);
    if (v4l2_fd < 0) {
        std::cerr << "Failed to open " << kV4L2Device << std::endl;
        return -1;
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    int net_fd = -1;
    bool v4l2_streaming = false;
    std::vector<StreamBuffer> stream_buffers;
    MppCtx mpp_ctx = NULL;
    MppApi *mpp_mpi = NULL;
    MppEncCfg mpp_cfg = NULL;
    MppBuffer frame_buf = NULL;
    MppBuffer header_buf = NULL;

    auto cleanup = [&]() {
        if (v4l2_streaming) {
            ioctl(v4l2_fd, VIDIOC_STREAMOFF, &type);
        }
        for (size_t i = 0; i < stream_buffers.size(); ++i) {
            if (stream_buffers[i].start && stream_buffers[i].start != MAP_FAILED) {
                munmap(stream_buffers[i].start, stream_buffers[i].length);
            }
        }
        if (header_buf) {
            mpp_buffer_put(header_buf);
        }
        if (frame_buf) {
            mpp_buffer_put(frame_buf);
        }
        if (mpp_cfg) {
            mpp_enc_cfg_deinit(mpp_cfg);
        }
        if (mpp_ctx) {
            mpp_destroy(mpp_ctx);
        }
        if (net_fd >= 0) {
            close(net_fd);
        }
        close(v4l2_fd);
    };

    // ---- V4L2 采集初始化 ----
    struct v4l2_format fmt;
    std::memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.width = kFrameWidth;
    fmt.fmt.pix_mp.height = kFrameHeight;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    fmt.fmt.pix_mp.field = V4L2_FIELD_ANY;
    fmt.fmt.pix_mp.num_planes = 1;
    if (ioctl(v4l2_fd, VIDIOC_S_FMT, &fmt) < 0) {
        std::cerr << "Failed to set V4L2 format for stream mode" << std::endl;
        cleanup();
        return -1;
    }

    uint32_t hor_stride = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
    if (hor_stride == 0) {
        hor_stride = kFrameWidth;
    }
    uint32_t ver_stride = align_up(kFrameHeight, 16);
    size_t raw_frame_bytes = static_cast<size_t>(hor_stride) * kFrameHeight * 3 / 2;
    size_t enc_frame_bytes = static_cast<size_t>(hor_stride) * ver_stride * 3 / 2;

    // 通知云图线程 overlay 尺寸
    if (state) {
        state->width = kFrameWidth;
        state->height = kFrameHeight;
        __atomic_store_n(&state->rotate_270, false, __ATOMIC_SEQ_CST);
        __atomic_store_n(&state->ready, true, __ATOMIC_RELEASE);
    }

    struct v4l2_requestbuffers req;
    std::memset(&req, 0, sizeof(req));
    req.count = kBufferCount;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(v4l2_fd, VIDIOC_REQBUFS, &req) < 0) {
        std::cerr << "Failed to request V4L2 buffers for stream mode" << std::endl;
        cleanup();
        return -1;
    }

    stream_buffers.resize(req.count);
    for (uint32_t i = 0; i < req.count; ++i) {
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
            std::cerr << "Failed to query V4L2 buffer " << i << " for stream mode" << std::endl;
            cleanup();
            return -1;
        }

        stream_buffers[i].length = buf.m.planes[0].length;
        stream_buffers[i].start = mmap(NULL, stream_buffers[i].length, PROT_READ | PROT_WRITE,
                                       MAP_SHARED, v4l2_fd, buf.m.planes[0].m.mem_offset);
        if (stream_buffers[i].start == MAP_FAILED) {
            std::cerr << "Failed to mmap V4L2 buffer " << i << " for stream mode" << std::endl;
            cleanup();
            return -1;
        }

        if (ioctl(v4l2_fd, VIDIOC_QBUF, &buf) < 0) {
            std::cerr << "Failed to queue V4L2 buffer " << i << " for stream mode" << std::endl;
            cleanup();
            return -1;
        }
    }

    // ---- 网络 socket ----
    struct sockaddr_in peer_addr;
    std::memset(&peer_addr, 0, sizeof(peer_addr));
    peer_addr.sin_family = AF_INET;
    peer_addr.sin_port = htons(config.port);
    if (inet_pton(AF_INET, config.host.c_str(), &peer_addr.sin_addr) != 1) {
        std::cerr << "Invalid target IP: " << config.host << std::endl;
        cleanup();
        return -1;
    }

    if (config.proto == StreamProto::UDP) {
        net_fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (net_fd < 0) {
            std::cerr << "Failed to create UDP socket" << std::endl;
            cleanup();
            return -1;
        }
    } else if (config.proto == StreamProto::TCP) {
        net_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (net_fd < 0) {
            std::cerr << "Failed to create TCP socket" << std::endl;
            cleanup();
            return -1;
        }

        int one = 1;
        setsockopt(net_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        if (config.host == "0.0.0.0") {
            // 作为 TCP server：bind + listen + accept
            int opt = 1;
            setsockopt(net_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
            if (bind(net_fd, reinterpret_cast<const struct sockaddr *>(&peer_addr), sizeof(peer_addr)) < 0) {
                std::cerr << "Failed to bind TCP port " << config.port << std::endl;
                cleanup();
                return -1;
            }
            if (listen(net_fd, 1) < 0) {
                std::cerr << "Failed to listen TCP" << std::endl;
                cleanup();
                return -1;
            }
            std::cout << "Waiting for TCP connection on port " << config.port << "..." << std::endl;
            int client_fd = accept(net_fd, NULL, NULL);
            if (client_fd < 0) {
                std::cerr << "Failed to accept TCP connection" << std::endl;
                cleanup();
                return -1;
            }
            close(net_fd);
            net_fd = client_fd;
            setsockopt(net_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        } else {
            // 作为 TCP client：connect
            if (connect(net_fd, reinterpret_cast<const struct sockaddr *>(&peer_addr), sizeof(peer_addr)) != 0) {
                std::cerr << "Failed to connect TCP to " << config.host << ":" << config.port << std::endl;
                cleanup();
                return -1;
            }
        }
    } else {
        std::cerr << "Unsupported proto" << std::endl;
        cleanup();
        return -1;
    }

    // ---- MPP H.264 编码器初始化 ----
    MPP_RET ret = mpp_create(&mpp_ctx, &mpp_mpi);
    if (ret != MPP_OK) {
        std::cerr << "mpp_create failed: " << ret << std::endl;
        cleanup();
        return -1;
    }

    ret = mpp_init(mpp_ctx, MPP_CTX_ENC, MPP_VIDEO_CodingAVC);
    if (ret != MPP_OK) {
        std::cerr << "mpp_init encoder failed: " << ret << std::endl;
        cleanup();
        return -1;
    }

    ret = mpp_enc_cfg_init(&mpp_cfg);
    if (ret != MPP_OK) {
        std::cerr << "mpp_enc_cfg_init failed: " << ret << std::endl;
        cleanup();
        return -1;
    }

    const int fps = 30;
    const int target_bps = 4 * 1000 * 1000;
    mpp_enc_cfg_set_s32(mpp_cfg, "prep:width", kFrameWidth);
    mpp_enc_cfg_set_s32(mpp_cfg, "prep:height", kFrameHeight);
    mpp_enc_cfg_set_s32(mpp_cfg, "prep:hor_stride", hor_stride);
    mpp_enc_cfg_set_s32(mpp_cfg, "prep:ver_stride", ver_stride);
    mpp_enc_cfg_set_s32(mpp_cfg, "prep:format", MPP_FMT_YUV420SP);
    mpp_enc_cfg_set_s32(mpp_cfg, "prep:rotation", 0);

    mpp_enc_cfg_set_s32(mpp_cfg, "rc:mode", MPP_ENC_RC_MODE_CBR);
    mpp_enc_cfg_set_s32(mpp_cfg, "rc:fps_in_flex", 0);
    mpp_enc_cfg_set_s32(mpp_cfg, "rc:fps_in_num", fps);
    mpp_enc_cfg_set_s32(mpp_cfg, "rc:fps_in_denorm", 1);
    mpp_enc_cfg_set_s32(mpp_cfg, "rc:fps_out_flex", 0);
    mpp_enc_cfg_set_s32(mpp_cfg, "rc:fps_out_num", fps);
    mpp_enc_cfg_set_s32(mpp_cfg, "rc:fps_out_denorm", 1);
    mpp_enc_cfg_set_s32(mpp_cfg, "rc:gop", fps * 2);
    mpp_enc_cfg_set_s32(mpp_cfg, "rc:bps_target", target_bps);
    mpp_enc_cfg_set_s32(mpp_cfg, "rc:bps_max", target_bps * 17 / 16);
    mpp_enc_cfg_set_s32(mpp_cfg, "rc:bps_min", target_bps * 15 / 16);

    mpp_enc_cfg_set_s32(mpp_cfg, "codec:type", MPP_VIDEO_CodingAVC);
    mpp_enc_cfg_set_s32(mpp_cfg, "h264:profile", 100);
    mpp_enc_cfg_set_s32(mpp_cfg, "h264:level", 40);
    mpp_enc_cfg_set_s32(mpp_cfg, "h264:cabac_en", 1);
    mpp_enc_cfg_set_s32(mpp_cfg, "h264:cabac_idc", 0);
    mpp_enc_cfg_set_s32(mpp_cfg, "h264:trans8x8", 1);

    ret = mpp_mpi->control(mpp_ctx, MPP_ENC_SET_CFG, mpp_cfg);
    if (ret != MPP_OK) {
        std::cerr << "MPP_ENC_SET_CFG failed: " << ret << std::endl;
        cleanup();
        return -1;
    }

    ret = mpp_buffer_get(NULL, &frame_buf, enc_frame_bytes);
    if (ret != MPP_OK) {
        std::cerr << "mpp_buffer_get frame_buf failed: " << ret << std::endl;
        cleanup();
        return -1;
    }

    ret = mpp_buffer_get(NULL, &header_buf, 1024 * 1024);
    if (ret != MPP_OK) {
        std::cerr << "mpp_buffer_get header_buf failed: " << ret << std::endl;
        cleanup();
        return -1;
    }

    // 发送 H.264 SPS/PPS 头
    MppPacket header_packet = NULL;
    if (mpp_packet_init_with_buffer(&header_packet, header_buf) == MPP_OK) {
        mpp_packet_set_length(header_packet, 0);
        if (mpp_mpi->control(mpp_ctx, MPP_ENC_GET_HDR_SYNC, header_packet) == MPP_OK) {
            void *header_ptr = mpp_packet_get_pos(header_packet);
            size_t header_len = mpp_packet_get_length(header_packet);
            if (header_ptr && header_len > 0) {
                bool header_ok = false;
                if (config.proto == StreamProto::UDP) {
                    header_ok = send_udp_buffer(net_fd, peer_addr,
                                                reinterpret_cast<uint8_t *>(header_ptr), header_len);
                } else {
                    header_ok = send_tcp_all(net_fd, reinterpret_cast<uint8_t *>(header_ptr), header_len);
                }
                if (!header_ok) {
                    std::cerr << "Failed to send H.264 SPS/PPS header" << std::endl;
                    mpp_packet_deinit(&header_packet);
                    cleanup();
                    return -1;
                }
                std::cout << "Sent H.264 SPS/PPS header: " << header_len << " bytes" << std::endl;
            }
        }
        mpp_packet_deinit(&header_packet);
    }

    if (ioctl(v4l2_fd, VIDIOC_STREAMON, &type) < 0) {
        std::cerr << "Failed to start V4L2 streaming for stream mode" << std::endl;
        cleanup();
        return -1;
    }
    v4l2_streaming = true;

    std::cout << "Stream mode started. On PC run:" << std::endl;
    if (config.proto == StreamProto::UDP) {
        std::cout << "ffplay -f h264 -fflags nobuffer -flags low_delay udp://0.0.0.0:"
                  << config.port << std::endl;
    } else {
        std::cout << "ffplay -f h264 -fflags nobuffer -flags low_delay tcp://0.0.0.0:"
                  << config.port << "?listen" << std::endl;
    }

    // ---- 推流主循环 ----
    bool logged_first_packet = false;
    uint64_t applied_overlay_generation = 0;
    std::vector<uint32_t> overlay_pixels;
    int overlay_width = 0;
    int overlay_height = 0;

    while (!state || __atomic_load_n(&state->running, __ATOMIC_SEQ_CST)) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(v4l2_fd, &fds);
        struct timeval tv = {2, 0};
        int ready = select(v4l2_fd + 1, &fds, NULL, NULL, &tv);
        if (ready <= 0) {
            std::cerr << "Select timeout or error in stream mode" << std::endl;
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
            std::cerr << "Failed to dequeue V4L2 buffer in stream mode" << std::endl;
            break;
        }

        void *dst_ptr = mpp_buffer_get_ptr(frame_buf);
        if (!dst_ptr) {
            std::cerr << "Failed to get MPP frame buffer pointer" << std::endl;
            break;
        }
        std::memset(dst_ptr, 0, enc_frame_bytes);
        std::memcpy(dst_ptr, stream_buffers[buf.index].start, raw_frame_bytes);

        // overlay 的读取和 drm_mode 一样：generation 变 → 有新图 → 从活跃槽拷贝。
        // 音频线程同时在写 pixels[1 - write_idx]，不冲突。
        if (state) {
            uint64_t overlay_generation = __atomic_load_n(&state->generation, __ATOMIC_ACQUIRE);
            if (overlay_generation != applied_overlay_generation) {
                int idx = __atomic_load_n(&state->write_idx, __ATOMIC_ACQUIRE);
                overlay_pixels = state->pixels[idx];
                overlay_width = state->width;
                overlay_height = state->height;
                applied_overlay_generation = overlay_generation;
            }

            if (!overlay_pixels.empty()) {
                blend_argb_overlay_to_nv12(reinterpret_cast<uint8_t *>(dst_ptr),
                                           kFrameWidth,
                                           kFrameHeight,
                                           static_cast<int>(hor_stride),
                                           overlay_pixels,
                                           overlay_width,
                                           overlay_height);
            }
        }

        MppFrame frame = NULL;
        if (mpp_frame_init(&frame) != MPP_OK) {
            std::cerr << "mpp_frame_init failed" << std::endl;
            break;
        }

        mpp_frame_set_width(frame, kFrameWidth);
        mpp_frame_set_height(frame, kFrameHeight);
        mpp_frame_set_hor_stride(frame, hor_stride);
        mpp_frame_set_ver_stride(frame, ver_stride);
        mpp_frame_set_fmt(frame, MPP_FMT_YUV420SP);
        mpp_frame_set_eos(frame, 0);
        mpp_frame_set_buffer(frame, frame_buf);

        ret = mpp_mpi->encode_put_frame(mpp_ctx, frame);
        mpp_frame_deinit(&frame);
        if (ret != MPP_OK) {
            std::cerr << "encode_put_frame failed: " << ret << std::endl;
            break;
        }

        MppPacket packet = NULL;
        ret = mpp_mpi->encode_get_packet(mpp_ctx, &packet);
        if (ret != MPP_OK) {
            std::cerr << "encode_get_packet failed: " << ret << std::endl;
            break;
        }

        if (packet) {
            void *packet_ptr = mpp_packet_get_pos(packet);
            size_t packet_len = mpp_packet_get_length(packet);
            if (packet_ptr && packet_len > 0) {
                bool ok = false;
                if (config.proto == StreamProto::UDP) {
                    ok = send_udp_buffer(net_fd, peer_addr,
                                         reinterpret_cast<uint8_t *>(packet_ptr), packet_len);
                } else {
                    ok = send_tcp_all(net_fd, reinterpret_cast<uint8_t *>(packet_ptr), packet_len);
                }
                if (!ok) {
                    std::cerr << "Failed to send H.264 packet" << std::endl;
                    mpp_packet_deinit(&packet);
                    break;
                }
                if (!logged_first_packet) {
                    std::cout << "Sent first encoded H.264 packet: " << packet_len << " bytes" << std::endl;
                    logged_first_packet = true;
                }
            }
            mpp_packet_deinit(&packet);
        }

        if (ioctl(v4l2_fd, VIDIOC_QBUF, &buf) < 0) {
            std::cerr << "Failed to re-queue V4L2 buffer in stream mode" << std::endl;
            break;
        }
    }

    cleanup();
    return 0;
}
