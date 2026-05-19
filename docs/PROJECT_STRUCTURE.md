# Project Structure And Usage Guide

## 1. Docs 目录作用

`docs/` 用来集中放这个工程的说明文档，主要面向两类需求：

- 工程结构说明：每个子目录、每个模式、每条数据链路分别放在哪里。
- 集成与使用说明：如何编译、如何运行 `drm` / `stream` 模式、PC 端怎么接收 UDP/TCP 推流。

当前 `docs/` 下文档建议这样理解：
- `PROJECT_STRUCTURE.md`
  - 讲当前工程目录结构、代码职责、调用方式和 PC 端命令。

## 2. 工程子目录作用

本工程按功能拆成了多个子目录，避免所有源码都堆在同一级目录。

- `app/`
  - 程序入口和线程启动逻辑。
  - 主要文件：
    - `main.cpp`
    - `peripheral_probe.cpp`
  - 作用：
    - 解析命令行参数；
    - 根据 `--mode=drm` 或 `--mode=stream` 启动对应线程；
    - 程序启动前打印背光 / EEPROM 状态。

- `config/`
  - 命令行解析与应用配置。
  - 主要文件：
    - `app_config.h`
    - `app_config.cpp`
  - 作用：
    - 解析 `--mode`、`--proto`、`--host`、`--port` 等参数。

- `display/`
  - 本地 LCD/HDMI 显示链路。
  - 主要文件：
    - `drm_mode.h`
    - `drm_mode.cpp`
    - `text_overlay.h`
    - `text_overlay.cpp`
  - 作用：
    - V4L2 取帧；
    - DMA-BUF 导入 DRM 做零拷贝显示；
    - 把云图和 FPS 文字写到 overlay plane；
    - 把中心 ROI 发布给 AF 线程。

- `audio/`
  - 声源定位云图生成线程。
  - 主要文件：
    - `audio_cloud_overlay.h`
    - `audio_cloud_overlay.cpp`
  - 作用：
    - 生成 ARGB 热点图；
    - 通过共享区把最新 overlay 发布给 `display/` 或 `stream/`。

- `stream/`
  - 网络推流和编码链路。
  - 主要文件：
    - `stream_mode.h`
    - `stream_mode.cpp`
    - `nv12_overlay_blend.h`
    - `nv12_overlay_blend.cpp`
  - 作用：
    - V4L2 采集 NV12；
    - 可选把 ARGB overlay 混到 NV12；
    - 调用 MPP 编码 H.264；
    - 使用 UDP/TCP socket 推流到 PC。

- `autofocus/`
  - 自动对焦线程与共享状态。
  - 主要文件：
    - `autofocus_shared_state.h`
    - `autofocus_worker.h`
    - `autofocus_worker.cpp`
  - 作用：
    - 接收视频线程发布的中心 ROI；
    - 做 Sobel 清晰度评价；
    - 执行爬山搜索；
    - 优先通过 `V4L2_CID_FOCUS_ABSOLUTE` 下发真实焦点位置，失败时退回模拟 VCM。

- `shared/`
  - 多线程共享结构和通用类型。
  - 主要文件：
    - `common.h`
    - `overlay_shared_state.h`
  - 作用：
    - 定义 `Buffer`、`DumbBuffer`、共享 overlay 状态等公共结构。

- `assets/`
  - 静态资源。
  - 主要文件：
    - `font.h`
  - 作用：
    - 提供简易位图字体，用于 overlay 画字。

- `dw9763_v4l2subdev/`
  - 应用层 `dw9763` 模拟控制封装。
  - 主要文件：
    - `dw9763_vcm_sim.h`
    - `dw9763_vcm_sim.cpp`
  - 作用：
    - 在没有真实 AF 镜头时，模拟 `dw9763` 的逻辑焦点到寄存器值映射。

- `peripherals/backlight_pwm/`
  - 用户态背光封装和参考驱动。
  - 作用：
    - 探测 `/sys/class/backlight`；
    - 读写亮度；
    - 提供背光 demo 驱动参考代码。

- `peripherals/eeprom_version/`
  - EEPROM 版本信息读写封装。
  - 作用：
    - 读取 `/dev/eeprom_ver` 或 `/dev/eeprom_ver_dev`；
    - 提供 EEPROM 参考驱动与测试程序。

- `peripherals/dw9763_v4l2subdev/`
  - `dw9763` 子设备参考驱动代码。
  - 作用：
    - 作为 V4L2 子设备驱动示例，展示 `probe/remove`、`ctrl ops`、`ioctl` 的写法。

## 3. 两种运行模式

当前工程主要有两种模式：

- `drm`
  - 本地显示模式。
  - 主链路：
    - V4L2 采集 NV12；
    - DMA-BUF 导入 DRM；
    - video plane 显示视频；
    - overlay plane 显示声源云图；
    - AF 线程消费 ROI。

- `stream`
  - 网络推流模式。
  - 主链路：
    - V4L2 采集 NV12；
    - 可选把云图混合进 NV12；
    - MPP 编码 H.264；
    - UDP/TCP 推流到 PC。

## 4. 对应模式的调用方法

### 4.1 DRM 本地显示模式

运行命令：

```bash
./v4l2_drm_imx415_cloud_img --mode=drm
```

调用路径：

- 程序入口：
  - `app/main.cpp`
- 参数解析后进入：
  - `run_video_display_worker()`
- 对应代码：
  - `display/drm_mode.cpp`

线程关系：

- 音频云图线程：
  - `run_audio_overlay_worker()`
- 视频显示线程：
  - `run_video_display_worker()`
- AF 线程：
  - `run_autofocus_worker()`

### 4.2 stream 推流模式

#### UDP 推流

板端运行：

```bash
./v4l2_drm_imx415_cloud_img --mode=stream --proto=udp --host=192.168.2.18 --port=5004
```

调用路径：

- 程序入口：
  - `app/main.cpp`
- 参数解析后进入：
  - `run_stream_mode()`
- 对应代码：
  - `stream/stream_mode.cpp`

线程关系：

- 音频云图线程：
  - `run_audio_overlay_worker()`
- 推流线程：
  - `run_stream_mode()`

#### TCP 推流

板端作为客户端，连到 PC：

```bash
./v4l2_drm_imx415_cloud_img --mode=stream --proto=tcp --host=192.168.2.18 --port=5000
```

或者让板端监听、PC 反向连接时，把 `host` 设为 `0.0.0.0`：

```bash
./v4l2_drm_imx415_cloud_img --mode=stream --proto=tcp --host=0.0.0.0 --port=5000
```

## 5. PC 端接收命令

### 5.1 UDP 接收命令

PC 端推荐先运行：

```bash
ffplay -f h264 -fflags nobuffer -flags low_delay udp://0.0.0.0:5004
```

如果想更稳一点，也可以试：

```bash
ffplay -f h264 -fflags nobuffer -flags low_delay -framedrop udp://0.0.0.0:5004
```

说明：

- `-f h264`
  - 告诉 `ffplay` 这是裸 H.264 码流。
- `udp://0.0.0.0:5004`
  - 表示在本机 `5004` 端口监听 UDP 数据。

### 5.2 TCP 接收命令

如果板端作为 TCP 客户端主动连接 PC，则 PC 端先监听：

```bash
ffplay -f h264 -fflags nobuffer -flags low_delay tcp://0.0.0.0:5000?listen
```

说明：

- `?listen`
  - 表示 `ffplay` 作为 TCP 服务端等待板端连接。

## 6. 关键调用关系速查

如果你只想快速找“某个模式主要由谁调用”，可以直接看这里：

- `main()`：
  - `app/main.cpp`
- `drm` 模式主线程入口：
  - `display/drm_mode.cpp` 中的 `run_video_display_worker()`
- `stream` 模式主线程入口：
  - `stream/stream_mode.cpp` 中的 `run_stream_mode()`
- 云图线程入口：
  - `audio/audio_cloud_overlay.cpp` 中的 `run_audio_overlay_worker()`
- AF 线程入口：
  - `autofocus/autofocus_worker.cpp` 中的 `run_autofocus_worker()`

## 7. 一句话理解整个工程

这个工程可以简化理解成两条主路径：

- `drm`：
  - `V4L2 -> DRM video plane`
  - `audio worker -> overlay plane`
  - `video thread -> ROI -> AF worker`

- `stream`：
  - `V4L2 -> overlay blend -> MPP H.264 encode -> UDP/TCP socket -> PC`
