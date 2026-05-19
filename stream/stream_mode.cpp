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
#include <pthread.h>

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

// 功能：
//   把数值按指定对齐粒度向上取整。
// 参数：
//   value: 原始值。
//   alignment: 对齐粒度，通常为 2 的幂。
// 返回值：
//   向上对齐后的结果。
static uint32_t align_up(uint32_t value, uint32_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

// 功能：
//   通过 UDP 发送一整块 H.264 数据。
//   为避免单个 UDP 包过大，这里会按 1400 字节左右切片发送。
// 参数：
//   sockfd: UDP socket。
//   addr: 对端地址。
//   data: 待发送数据首地址。
//   length: 待发送总字节数。
// 返回值：
//   true  表示所有分片都成功发送；
//   false 表示发送过程中出现错误或发送字节数异常。
static bool send_udp_buffer(int sockfd, const struct sockaddr_in &addr, const uint8_t *data, size_t length) {
    // 以太网 MTU 常见值是 1500 字节。扣掉 IP/UDP 头部后，单个 UDP 负载若过大，
    // 很容易在链路层发生分片。这里主动把 H.264 数据切成约 1400 字节的小块，
    // 目的是尽量避免 IP 分片，提高上位机接收稳定性。
    const size_t kUdpChunkSize = 1400;

    while (length > 0) {
        size_t chunk = length > kUdpChunkSize ? kUdpChunkSize : length;
        // sendto:
        //   1. sockfd 是一个 UDP socket；
        //   2. data/chunk 指向本次要发的一个分片；
        //   3. addr 指定目标 IP 和端口；
        //   4. UDP 是“无连接发送”，每次 sendto 都要带上目的地址。
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

// 功能：
//   通过 TCP 发送一整块数据，直到全部发送完成。
// 参数：
//   sockfd: 已连接的 TCP socket。
//   data: 待发送数据首地址。
//   length: 待发送总字节数。
// 返回值：
//   true  表示全部字节写出成功；
//   false 表示 send 失败或连接中断。
static bool send_tcp_all(int sockfd, const uint8_t *data, size_t length) {
    while (length > 0) {
        // send:
        //   用于已建立连接的 TCP socket。
        // TCP 是字节流协议，一次 send 不保证把 length 个字节全部发完，
        // 所以这里必须循环调用 send，直到剩余字节数变为 0。
        //
        // MSG_NOSIGNAL:
        //   当对端异常断开时，禁止内核给本进程发送 SIGPIPE。
        //   这样应用层能通过 send 返回值判断错误，而不会被信号直接打断退出。
        ssize_t sent = send(sockfd, data, length, MSG_NOSIGNAL);
        if (sent <= 0) {
            return false;
        }
        data += static_cast<size_t>(sent);
        length -= static_cast<size_t>(sent);
    }
    return true;
}

// 功能：
//   运行 stream 模式主流程：
//   V4L2 采集 NV12 -> 可选叠加 ARGB 云图 -> MPP 编码 H.264 -> UDP/TCP 发到上位机。
// 参数：
//   config: 应用层运行配置，包含协议、目标主机、端口等。
//   state: 云图共享状态；若非空，则在编码前把最新 overlay 混合进 NV12 帧。
// 返回值：
//   0  表示正常结束；
//   -1 表示初始化、采集、编码或网络发送过程中出现错误。
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

    if (state) {
        pthread_mutex_lock(&state->mutex);
        state->width = kFrameWidth;
        state->height = kFrameHeight;
        state->ready = true;
        state->rotate_270.store(false);
        pthread_mutex_unlock(&state->mutex);
    }
    if (state) {
        pthread_cond_broadcast(&state->condition);
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

    struct sockaddr_in peer_addr;
    std::memset(&peer_addr, 0, sizeof(peer_addr));
    // sockaddr_in 用来描述 IPv4 对端地址。
    // 这里统一填好目标主机和端口，后续 UDP 的 sendto 或 TCP 的 connect/bind 都会复用它。
    peer_addr.sin_family = AF_INET;
    // htons:
    //   端口号在网络上传输时要求网络字节序（大端），
    //   所以要把主机字节序的 config.port 转成网络字节序。
    peer_addr.sin_port = htons(config.port);
    // inet_pton:
    //   把 "192.168.x.x" 这样的点分十进制字符串转成二进制 IPv4 地址。
    //   如果返回值不是 1，说明用户输入的目标 IP 非法。
    if (inet_pton(AF_INET, config.host.c_str(), &peer_addr.sin_addr) != 1) {
        std::cerr << "Invalid target IP: " << config.host << std::endl;
        cleanup();
        return -1;
    }

    if (config.proto == StreamProto::UDP) {
        // socket(AF_INET, SOCK_DGRAM, 0):
        //   创建一个 IPv4 UDP socket。
        //   数据传输类型为SOCK_DGRAM报文式传输，对应UDP
        //   UDP 不需要像 TCP 那样先建立连接，后面直接 sendto 即可发包。
        net_fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (net_fd < 0) {
            std::cerr << "Failed to create UDP socket" << std::endl;
            cleanup();
            return -1;
        }
    } else if (config.proto == StreamProto::TCP) {
        // socket(AF_INET, SOCK_STREAM, 0):
        //   创建一个 IPv4 TCP socket。
        //   数据传输类型为SOCK_STREAM字节流式传输，对应TCP
        //   TCP 是面向连接的协议，后面必须经过 connect 或 listen/accept 才能真正收发数据。
        net_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (net_fd < 0) {
            std::cerr << "Failed to create TCP socket" << std::endl;
            cleanup();
            return -1;
        }

        int one = 1;
        // TCP_NODELAY:
        //   关闭 Nagle 算法，尽量让编码器产出的包立刻发出去，
        //   降低小包在发送端聚合等待的时延，更适合低延迟视频预览场景。
        setsockopt(net_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        if (config.host == "0.0.0.0") {
            int opt = 1;
            // SO_REUSEADDR:
            //   允许端口快速复用，避免程序刚退出时端口仍处于 TIME_WAIT，
            //   导致重新启动监听失败。
            setsockopt(net_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
            // bind:
            //   把本地监听 socket 绑定到指定端口上。
            //   这里 host=0.0.0.0 表示“本机作为 TCP 服务端”，等待上位机主动连过来。
            if (bind(net_fd, reinterpret_cast<const struct sockaddr *>(&peer_addr), sizeof(peer_addr)) < 0) {
                std::cerr << "Failed to bind TCP port " << config.port << std::endl;
                cleanup();
                return -1;
            }
            // listen:
            //   把普通 TCP socket 变成监听 socket，backlog=1 表示这里只接受一个上位机连接。
            if (listen(net_fd, 1) < 0) {
                std::cerr << "Failed to listen TCP" << std::endl;
                cleanup();
                return -1;
            }
            std::cout << "Waiting for TCP connection on port " << config.port << "..." << std::endl;
            // accept:
            //   阻塞等待客户端接入。
            //   成功后会返回一个新的已连接 socket(client_fd)，
            //   原 net_fd 仍然只是“监听 socket”。
            int client_fd = accept(net_fd, NULL, NULL);
            if (client_fd < 0) {
                std::cerr << "Failed to accept TCP connection" << std::endl;
                cleanup();
                return -1;
            }
            // 监听 socket 只负责握手，真正传输视频时只需要已连接的 client_fd。
            close(net_fd);
            net_fd = client_fd;
            // 对真正的传输 socket 再设置一次 TCP_NODELAY，确保低延迟策略生效在连接通道上。
            setsockopt(net_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        } else {
            // connect:
            //   当前板卡作为 TCP 客户端，主动连接到上位机监听端口。
            //   connect 成功后，后续就可以对 net_fd 直接调用 send 发送 H.264 码流。
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

    MppPacket header_packet = NULL;
    if (mpp_packet_init_with_buffer(&header_packet, header_buf) == MPP_OK) {
        mpp_packet_set_length(header_packet, 0);
        if (mpp_mpi->control(mpp_ctx, MPP_ENC_GET_HDR_SYNC, header_packet) == MPP_OK) {
            void *header_ptr = mpp_packet_get_pos(header_packet);
            size_t header_len = mpp_packet_get_length(header_packet);
            if (header_ptr && header_len > 0) {
                // 编码器初始化后先发一份 SPS/PPS 头。
                // 对端 ffplay/ffmpeg 只有拿到这份 AVC 头信息，才能正确解析后续裸 H.264 码流。
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

    bool logged_first_packet = false;
    uint64_t applied_overlay_generation = 0;
    std::vector<uint32_t> overlay_pixels;
    int overlay_width = 0;
    int overlay_height = 0;

    while (!state || state->running.load()) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(v4l2_fd, &fds);
        struct timeval tv = {2, 0};
        // select:
        //   等待 V4L2 设备变为“可读”，本质上就是等待采集驱动准备好一帧。
        //   这里不是网络 select，而是对视频采集 fd 做 I/O 多路复用等待，
        //   避免线程在没有新帧时忙轮询占满 CPU。
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

        if (state) {
            uint64_t overlay_generation = 0;
            {
                if (pthread_mutex_trylock(&state->mutex) == 0) {
                    overlay_generation = state->generation;
                    if (overlay_generation != applied_overlay_generation && !state->pixels.empty()) {
                        overlay_pixels = state->pixels;
                        overlay_width = state->width;
                        overlay_height = state->height;
                        applied_overlay_generation = overlay_generation;
                    }
                    pthread_mutex_unlock(&state->mutex);
                }
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

        // 这里按“一帧取一次输出包”的方式处理，避免首帧后整个推流线程停在编码器上。
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
                // 编码器每产出一个 H.264 NALU/码流包，就立刻按协议发给上位机：
                //   UDP: sendto 分片发送；
                //   TCP: send 循环发送直到当前包完全写出。
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
