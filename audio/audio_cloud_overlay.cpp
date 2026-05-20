#include "audio_cloud_overlay.h"

#include "text_overlay.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdint>
#include <iostream>
#include <pthread.h>
#include <unistd.h>
#include <vector>

namespace {

static const float kPi = 3.14159265358979323846f;

struct Vec2 {
    // 阵列平面内的横向坐标。
    float x;
    // 阵列平面内的纵向坐标。
    float y;
};

struct Vec3 {
    // 三维空间的 X 轴分量。
    float x;
    // 三维空间的 Y 轴分量。
    float y;
    // 三维空间的 Z 轴分量。
    float z;
};

struct Source {
    // 归一化图像横坐标，用于表示声源在画面中的水平位置。
    float u;
    // 归一化图像纵坐标，用于表示声源在画面中的垂直位置。
    float v;
    // 声源幅度，用于决定模拟观测信号强弱。
    float amplitude;
};

struct Peak {
    // 峰值在功率图或显示图中的横坐标。
    int x;
    // 峰值在功率图或显示图中的纵坐标。
    int y;
    // 峰值功率强度。
    float power;
};

struct PixelCoord {
    // 显示像素横坐标。
    int x;
    // 显示像素纵坐标。
    int y;
};

// 功能：
//   把浮点数限制在 [0, 1] 范围内。
// 参数：
//   value: 原始浮点数。
// 返回值：
//   裁剪后的结果。
static float clamp01(float value) {
    if (value < 0.0f) {
        return 0.0f;
    }
    if (value > 1.0f) {
        return 1.0f;
    }
    return value;
}

// 功能：
//   把 4 个 8 位通道打包成一个 ARGB8888 像素。
// 参数：
//   a/r/g/b: Alpha、红、绿、蓝通道。
// 返回值：
//   ARGB8888 格式的 32 位像素值。
static uint32_t make_argb(uint8_t a, uint8_t r, uint8_t g, uint8_t b) {
    return (static_cast<uint32_t>(a) << 24) |
           (static_cast<uint32_t>(r) << 16) |
           (static_cast<uint32_t>(g) << 8) |
           static_cast<uint32_t>(b);
}

// 功能：
//   按“source over”规则把一个 ARGB 像素叠加到目标像素上。
// 参数：
//   dst: 目标像素。
//   src: 源像素。
// 返回值：
//   混合后的 ARGB8888 像素。
static uint32_t blend_over(uint32_t dst, uint32_t src) {
    float src_a = static_cast<float>((src >> 24) & 0xFF) / 255.0f;
    float dst_a = static_cast<float>((dst >> 24) & 0xFF) / 255.0f;
    float out_a = src_a + dst_a * (1.0f - src_a);
    if (out_a < 1e-6f) {
        return 0;
    }

    float src_r = static_cast<float>((src >> 16) & 0xFF);
    float src_g = static_cast<float>((src >> 8) & 0xFF);
    float src_b = static_cast<float>(src & 0xFF);
    float dst_r = static_cast<float>((dst >> 16) & 0xFF);
    float dst_g = static_cast<float>((dst >> 8) & 0xFF);
    float dst_b = static_cast<float>(dst & 0xFF);

    float out_r = (src_r * src_a + dst_r * dst_a * (1.0f - src_a)) / out_a;
    float out_g = (src_g * src_a + dst_g * dst_a * (1.0f - src_a)) / out_a;
    float out_b = (src_b * src_a + dst_b * dst_a * (1.0f - src_a)) / out_a;

    return make_argb(static_cast<uint8_t>(out_a * 255.0f),
                     static_cast<uint8_t>(out_r),
                     static_cast<uint8_t>(out_g),
                     static_cast<uint8_t>(out_b));
}

// 功能：
//   计算 ARGB buffer 的行跨度，单位为像素个数。
// 参数：
//   buf: 目标 dumb buffer。
// 返回值：
//   每行像素数。
static inline int argb_stride(const DumbBuffer *buf) {
    return static_cast<int>(buf->pitch / sizeof(uint32_t));
}

// 功能：
//   返回指定像素位置的引用，便于读写热点图像素。
// 参数：
//   buf: 目标 dumb buffer。
//   x/y: 像素坐标。
// 返回值：
//   对应像素的引用。
static inline uint32_t &pixel_at(DumbBuffer *buf, int x, int y) {
    return buf->pixels[y * argb_stride(buf) + x];
}

// 功能：
//   把归一化图像坐标 (u,v) 映射到实际显示像素坐标。
// 参数：
//   u/v: 归一化图像坐标，范围通常在 [0,1]。
//   width/height: 目标显示尺寸。
//   rotate_270: 视频 plane 是否做了 270 度旋转；若为 true，需要同步修正坐标。
// 返回值：
//   对应的显示像素坐标。
static PixelCoord map_image_point_to_display(float u, float v, int width, int height, bool rotate_270) {
    float du = u;
    float dv = v;
    if (rotate_270) {
        du = v;
        dv = 1.0f - u;
    }
    du = clamp01(du);
    dv = clamp01(dv);
    PixelCoord coord;
    coord.x = std::min(width - 1, std::max(0, static_cast<int>(du * static_cast<float>(width - 1))));
    coord.y = std::min(height - 1, std::max(0, static_cast<int>(dv * static_cast<float>(height - 1))));
    return coord;
}

// 功能：
//   将三维向量归一化为单位向量。
// 参数：
//   value: 原始三维向量。
// 返回值：
//   归一化后的单位向量；若输入长度过小，返回朝前的默认向量。
static Vec3 normalize(const Vec3 &value) {
    float norm = std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
    if (norm < 1e-6f) {
        return {0.0f, 0.0f, 1.0f};
    }
    return {value.x / norm, value.y / norm, value.z / norm};
}

// 功能：
//   根据图像坐标估计其在摄像头视场中的三维方向。
// 参数：
//   u/v: 归一化图像坐标。
// 返回值：
//   对应的单位方向向量。
static Vec3 image_to_direction(float u, float v) {
    const float h_fov = 62.0f * kPi / 180.0f;
    const float v_fov = 39.0f * kPi / 180.0f;
    float az = (u - 0.5f) * h_fov;
    float el = (0.5f - v) * v_fov;
    Vec3 dir;
    dir.x = std::tan(az);
    dir.y = std::tan(el);
    dir.z = 1.0f;
    return normalize(dir);
}

// 功能：
//   构造一个模拟的 128 阵元阿基米德螺旋麦克风阵列几何。
// 参数：
//   无。
// 返回值：
//   每个麦克风在阵列平面中的二维坐标列表。
static std::vector<Vec2> build_archimedean_spiral_array() {
    const int mic_count = 128;
    const float max_radius = 0.095f;
    const float turns = 7.5f;
    std::vector<Vec2> positions(mic_count);

    for (int i = 0; i < mic_count; ++i) {
        float t = static_cast<float>(i) / static_cast<float>(mic_count - 1);
        float theta = turns * 2.0f * kPi * t;
        float radius = max_radius * std::sqrt(t);
        positions[i].x = radius * std::cos(theta);
        positions[i].y = radius * std::sin(theta);
    }
    return positions;
}

// 功能：
//   构造模拟声源列表。
// 参数：
//   无。
// 返回值：
//   声源列表；当前只放置一个主声源热点。
static std::vector<Source> build_sources() {
    std::vector<Source> sources;
    sources.push_back({0.63f, 0.42f, 1.0f});
    return sources;
}

// 功能：
//   在某个频点上，生成阵列对多个声源的复数观测快拍。
// 参数：
//   frequency_hz: 当前模拟频点。
//   mic_positions: 阵列几何坐标。
//   sources: 声源列表。
// 返回值：
//   每个阵元在该频点上的复数观测值。
static std::vector<std::complex<float> > build_snapshot_for_frequency(
    float frequency_hz,
    const std::vector<Vec2> &mic_positions,
    const std::vector<Source> &sources) {
    const float sound_speed = 343.0f;
    std::vector<std::complex<float> > snapshot(mic_positions.size(), std::complex<float>(0.0f, 0.0f));
    float wave_number = 2.0f * kPi * frequency_hz / sound_speed;

    for (size_t mic = 0; mic < mic_positions.size(); ++mic) {
        std::complex<float> value(0.0f, 0.0f);
        for (size_t source = 0; source < sources.size(); ++source) {
            Vec3 dir = image_to_direction(sources[source].u, sources[source].v);
            float projection = mic_positions[mic].x * dir.x + mic_positions[mic].y * dir.y;
            float phase = -wave_number * projection;
            std::complex<float> sample = std::polar(sources[source].amplitude, phase);
            value += sample;
        }

        float pseudo_noise = std::sin(static_cast<float>(mic) * 17.0f + frequency_hz * 0.003f);
        value += std::complex<float>(0.02f * pseudo_noise, 0.015f * std::cos(static_cast<float>(mic)));
        snapshot[mic] = value;
    }

    return snapshot;
}

// 功能：
//   对整个方向网格做波束扫描，生成一张二维响应功率图。
// 参数：
//   无。
// 返回值：
//   二维功率图，数值越大表示该方向越可能存在声源。
static std::vector<std::vector<float> > build_power_map() {
    const int map_w = 96;
    const int map_h = 96;
    const float sound_speed = 343.0f;
    const float freqs[3] = {1200.0f, 2200.0f, 3400.0f};

    std::vector<Vec2> mic_positions = build_archimedean_spiral_array();
    std::vector<Source> sources = build_sources();
    std::vector<std::vector<std::complex<float> > > snapshots;
    for (int i = 0; i < 3; ++i) {
        snapshots.push_back(build_snapshot_for_frequency(freqs[i], mic_positions, sources));
    }

    std::vector<std::vector<float> > power(map_h, std::vector<float>(map_w, 0.0f));
    float max_power = 1e-6f;
    for (int y = 0; y < map_h; ++y) {
        float v = static_cast<float>(y) / static_cast<float>(map_h - 1);
        for (int x = 0; x < map_w; ++x) {
            float u = static_cast<float>(x) / static_cast<float>(map_w - 1);
            Vec3 candidate = image_to_direction(u, v);
            float accumulated = 0.0f;
            for (int f = 0; f < 3; ++f) {
                float wave_number = 2.0f * kPi * freqs[f] / sound_speed;
                std::complex<float> beam(0.0f, 0.0f);
                for (size_t mic = 0; mic < mic_positions.size(); ++mic) {
                    float projection = mic_positions[mic].x * candidate.x + mic_positions[mic].y * candidate.y;
                    std::complex<float> steering = std::polar(1.0f, wave_number * projection);
                    beam += snapshots[f][mic] * steering;
                }
                accumulated += std::norm(beam);
            }
            power[y][x] = accumulated;
            if (accumulated > max_power) {
                max_power = accumulated;
            }
        }
    }

    for (int y = 0; y < map_h; ++y) {
        for (int x = 0; x < map_w; ++x) {
            float normalized = power[y][x] / max_power;
            power[y][x] = std::pow(clamp01(normalized), 2.2f);
        }
    }

    std::vector<std::vector<float> > smoothed = power;
    for (int iter = 0; iter < 2; ++iter) {
        std::vector<std::vector<float> > next = smoothed;
        for (int y = 1; y < map_h - 1; ++y) {
            for (int x = 1; x < map_w - 1; ++x) {
                float sum = 0.0f;
                for (int ky = -1; ky <= 1; ++ky) {
                    for (int kx = -1; kx <= 1; ++kx) {
                        sum += smoothed[y + ky][x + kx];
                    }
                }
                next[y][x] = sum / 9.0f;
            }
        }
        smoothed.swap(next);
    }

    return smoothed;
}

// 功能：
//   把归一化功率值映射成蓝色系 ARGB 颜色。
// 参数：
//   power: 归一化响应强度，范围期望在 [0,1]。
// 返回值：
//   对应的 ARGB8888 蓝色热点颜色。
static uint32_t heat_color(float power) {
    float t = clamp01(power);
    float edge = std::pow(t, 1.35f);
    float core = std::pow(t, 2.4f);
    uint8_t alpha = static_cast<uint8_t>(std::pow(t, 1.55f) * 170.0f);
    return make_argb(alpha,
                     static_cast<uint8_t>(12.0f + edge * 20.0f),
                     static_cast<uint8_t>(60.0f + edge * 90.0f),
                     static_cast<uint8_t>(180.0f + core * 75.0f));
}

// 功能：
//   从功率图中寻找局部峰值，并按强度排序后做近邻抑制。
// 参数：
//   map: 二维功率图。
// 返回值：
//   峰值列表；当前实现只保留最强的一个主峰。
static std::vector<Peak> find_local_peaks(const std::vector<std::vector<float> > &map) {
    int map_h = static_cast<int>(map.size());
    int map_w = static_cast<int>(map[0].size());
    std::vector<Peak> peaks;
    for (int y = 1; y < map_h - 1; ++y) {
        for (int x = 1; x < map_w - 1; ++x) {
            float center = map[y][x];
            if (center < 0.72f) {
                continue;
            }
            bool is_peak = true;
            for (int ky = -1; ky <= 1 && is_peak; ++ky) {
                for (int kx = -1; kx <= 1; ++kx) {
                    if (ky == 0 && kx == 0) {
                        continue;
                    }
                    if (map[y + ky][x + kx] > center) {
                        is_peak = false;
                        break;
                    }
                }
            }
            if (is_peak) {
                peaks.push_back({x, y, center});
            }
        }
    }

    std::sort(peaks.begin(), peaks.end(), [](const Peak &lhs, const Peak &rhs) {
        return lhs.power > rhs.power;
    });

    std::vector<Peak> filtered;
    for (size_t i = 0; i < peaks.size() && filtered.size() < 1; ++i) {
        bool too_close = false;
        for (size_t j = 0; j < filtered.size(); ++j) {
            int dx = peaks[i].x - filtered[j].x;
            int dy = peaks[i].y - filtered[j].y;
            if (dx * dx + dy * dy < 144) {
                too_close = true;
                break;
            }
        }
        if (!too_close) {
            filtered.push_back(peaks[i]);
        }
    }
    return filtered;
}

// 功能：
//   把功率图中的主峰渲染成一块“严格圆形边界”的稀疏蓝色热点。
// 参数：
//   buf: 目标 ARGB overlay buffer。
//   width/height: overlay 尺寸。
//   map: 二维功率图。
//   rotate_270: 是否按旋转后的视频方向映射热点。
// 返回值：
//   映射到显示坐标后的主峰位置与功率；若无峰值则返回 {-1,-1,0}。
static Peak draw_sparse_response_map(DumbBuffer *buf, int width, int height,
                                     const std::vector<std::vector<float> > &map,
                                     bool rotate_270) {
    int map_h = static_cast<int>(map.size());
    int map_w = static_cast<int>(map[0].size());
    std::vector<Peak> peaks = find_local_peaks(map);
    if (peaks.empty()) {
        return {-1, -1, 0.0f};
    }

    for (size_t i = 0; i < peaks.size(); ++i) {
        float peak_u = (static_cast<float>(peaks[i].x) + 0.5f) / static_cast<float>(map_w);
        float peak_v = (static_cast<float>(peaks[i].y) + 0.5f) / static_cast<float>(map_h);
        PixelCoord center = map_image_point_to_display(peak_u, peak_v, width, height, rotate_270);
        float cx = static_cast<float>(center.x);
        float cy = static_cast<float>(center.y);
        float radius = 35.0f + peaks[i].power * 150.0f;
        int x0 = std::max(0, static_cast<int>(cx - radius - 1));
        int y0 = std::max(0, static_cast<int>(cy - radius - 1));
        int x1 = std::min(width, static_cast<int>(cx + radius + 1));
        int y1 = std::min(height, static_cast<int>(cy + radius + 1));
        for (int py = y0; py < y1; ++py) {
            for (int px = x0; px < x1; ++px) {
                float dx = static_cast<float>(px) - cx;
                float dy = static_cast<float>(py) - cy;
                float dist2 = dx * dx + dy * dy;
                float radius2 = radius * radius;
                if (dist2 > radius2) {
                    continue;
                }
                float normalized = std::sqrt(dist2) / radius;
                float falloff = 1.0f - normalized;
                float sigma = radius * 0.42f;
                float gaussian = std::exp(-dist2 / (2.0f * sigma * sigma));
                gaussian *= std::pow(falloff, 0.8f);
                float local_strength = peaks[i].power * gaussian;
                if (local_strength < 0.04f) {
                    continue;
                }
                uint32_t color = heat_color(local_strength);
                pixel_at(buf, px, py) = blend_over(pixel_at(buf, px, py), color);
            }
        }

        int core_radius = std::max(4, static_cast<int>(radius * 0.14f));
        for (int py = std::max(0, center.y - core_radius); py <= std::min(height - 1, center.y + core_radius); ++py) {
            for (int px = std::max(0, center.x - core_radius); px <= std::min(width - 1, center.x + core_radius); ++px) {
                float dx = static_cast<float>(px - center.x);
                float dy = static_cast<float>(py - center.y);
                if (dx * dx + dy * dy <= core_radius * core_radius) {
                    pixel_at(buf, px, py) =
                        blend_over(pixel_at(buf, px, py), make_argb(175, 120, 210, 255));
                }
            }
        }
    }

    const Peak &main_peak = peaks[0];
    float peak_u = (static_cast<float>(main_peak.x) + 0.5f) / static_cast<float>(map_w);
    float peak_v = (static_cast<float>(main_peak.y) + 0.5f) / static_cast<float>(map_h);
    PixelCoord display_peak = map_image_point_to_display(peak_u, peak_v, width, height, rotate_270);
    static bool logged_peak = false;
    if (!logged_peak) {
        std::cout << "Sound source peak: u=" << peak_u
                  << ", v=" << peak_v
                  << ", display_x=" << display_peak.x
                  << ", display_y=" << display_peak.y
                  << ", power=" << main_peak.power << std::endl;
        logged_peak = true;
    }
    Peak mapped_peak = {display_peak.x, display_peak.y, main_peak.power};
    return mapped_peak;
}

}  // namespace

// 功能：
//   生成一整张声源定位 overlay：
//   先清空背景，再计算功率图和热点，最后可选叠加 FPS 文本。
// 参数：
//   buf: 目标 ARGB overlay buffer。
//   width/height: overlay 尺寸。
//   fps: 若大于 0，则在左上角绘制 FPS 文字；若小于等于 0，则不绘制。
//   rotate_270: 是否按视频旋转状态修正热点坐标。
// 返回值：
//   无返回值。
void draw_sound_source_overlay(DumbBuffer *buf, int width, int height, float fps, bool rotate_270) {
    clear_argb(buf, width, height, 0x00000000);

    std::vector<std::vector<float> > power_map = build_power_map();
    Peak main_peak = draw_sparse_response_map(buf, width, height, power_map, rotate_270);

    if (fps > 0.0f) {
        char fps_text[32];
        std::snprintf(fps_text, sizeof(fps_text), "FPS: %.1f", fps);
        draw_text_at(buf, width, height, 22, 22, fps_text, make_argb(220, 255, 255, 255));
    }

    static bool logged_pixels = false;
    if (!logged_pixels) {
        uint32_t top_left = pixel_at(buf, 0, 0);
        uint32_t top_right = pixel_at(buf, width - 1, 0);
        uint32_t bottom_left = pixel_at(buf, 0, height - 1);
        uint32_t bottom_right = pixel_at(buf, width - 1, height - 1);
        uint32_t peak_pixel = 0;
        if (main_peak.x >= 0 && main_peak.y >= 0) {
            peak_pixel = pixel_at(buf, main_peak.x, main_peak.y);
        }

        int nonzero_alpha = 0;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                if ((pixel_at(buf, x, y) >> 24) != 0) {
                    ++nonzero_alpha;
                }
            }
        }

        std::cout << "Overlay pixels: tl=0x" << std::hex << top_left
                  << " tr=0x" << top_right
                  << " bl=0x" << bottom_left
                  << " br=0x" << bottom_right
                  << " peak=0x" << peak_pixel
                  << std::dec << " stride=" << argb_stride(buf)
                  << " width=" << width
                  << " nonzero_alpha=" << nonzero_alpha
                  << "/" << (width * height)
                  << " rotate_270=" << (rotate_270 ? 1 : 0) << std::endl;
        logged_pixels = true;
    }
}

// 功能：
//   云图线程主循环。
//   等待显示尺寸准备好后，周期性生成一张完整 ARGB overlay，并发布给视频线程消费。
// 参数：
//   state: 云图共享状态，包含尺寸、像素数组、generation 和退出标志。
// 返回值：
//   无返回值；当 state->running 变为 false 时退出。
void run_audio_overlay_worker(OverlaySharedState *state) {
    int overlay_width = 0;
    int overlay_height = 0;
    {
        // 云图线程启动后先等待 DRM 线程把 overlay 尺寸准备好。
        // 因为只有知道 LCD/overlay plane 的实际宽高后，热点坐标映射才是正确的。
        pthread_mutex_lock(&state->mutex);
        while (state->running.load() && !state->ready) {
            pthread_cond_wait(&state->condition, &state->mutex);
        }
        if (!state->running.load()) {
            pthread_mutex_unlock(&state->mutex);
            return;
        }
        overlay_width = state->width;
        overlay_height = state->height;
        pthread_mutex_unlock(&state->mutex);
    }

    std::vector<uint32_t> local_pixels(static_cast<size_t>(overlay_width) * overlay_height, 0);

    DumbBuffer local_buf;
    local_buf.pitch = static_cast<uint32_t>(overlay_width * sizeof(uint32_t));
    local_buf.size = static_cast<uint32_t>(local_pixels.size() * sizeof(uint32_t));
    local_buf.pixels = local_pixels.data();

    while (state->running.load()) {
        bool rotate_270 = state->rotate_270.load();
        // 在本地缓冲区里生成一张完整的 ARGB overlay。
        // 这里不直接操作 DRM，只做“算图”。
        draw_sound_source_overlay(&local_buf, overlay_width, overlay_height, -1.0f, rotate_270);

        {
            // 发布最新一版 overlay 给视频线程消费。
            // 视频线程当前是主动轮询 generation 并 trylock 读取，
            pthread_mutex_lock(&state->mutex);
            state->pixels = local_pixels;
            state->generation++;
            pthread_mutex_unlock(&state->mutex);
        }

        usleep(80000);
    }
}
