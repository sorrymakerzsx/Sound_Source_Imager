#include "audio_cloud_overlay.h"

#include "text_overlay.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdint>
#include <iostream>
#include <unistd.h>
#include <vector>

namespace {

static const float kPi = 3.14159265358979323846f;

struct Vec2 {
    float x;
    float y;
};

struct Vec3 {
    float x;
    float y;
    float z;
};

struct Source {
    float u;       // 归一化图像横坐标
    float v;       // 归一化图像纵坐标
    float amplitude;
};

struct Peak {
    int x;
    int y;
    float power;
};

struct PixelCoord {
    int x;
    int y;
};

static float clamp01(float value) {
    if (value < 0.0f) {
        return 0.0f;
    }
    if (value > 1.0f) {
        return 1.0f;
    }
    return value;
}

static uint32_t make_argb(uint8_t a, uint8_t r, uint8_t g, uint8_t b) {
    return (static_cast<uint32_t>(a) << 24) |
           (static_cast<uint32_t>(r) << 16) |
           (static_cast<uint32_t>(g) << 8) |
           static_cast<uint32_t>(b);
}

// "source over" alpha 混合
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

static inline int argb_stride(const DumbBuffer *buf) {
    return static_cast<int>(buf->pitch / sizeof(uint32_t));
}

static inline uint32_t &pixel_at(DumbBuffer *buf, int x, int y) {
    return buf->pixels[y * argb_stride(buf) + x];
}

// 归一化图像坐标 (u, v) → 显示像素坐标，自动处理 270° 旋转
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

static Vec3 normalize(const Vec3 &value) {
    float norm = std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
    if (norm < 1e-6f) {
        return {0.0f, 0.0f, 1.0f};
    }
    return {value.x / norm, value.y / norm, value.z / norm};
}

// 图像坐标 → 摄像头视场中的三维单位方向向量（假设 62°×39° FOV）
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

// 构造 128 阵元阿基米德螺旋麦克风阵列
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

// 模拟声源：一个主热点
static std::vector<Source> build_sources() {
    std::vector<Source> sources;
    sources.push_back({0.63f, 0.42f, 1.0f});
    return sources;
}

// 在指定频点生成阵列复数观测快拍（延迟求和 beamforming 输入）
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

// 三频点波束扫描 → 96x96 归一化功率图，2 轮平滑
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

// 归一化功率 → 蓝色系 ARGB 热点颜色
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

// 局部峰值检测 → 按强度排序 → 近邻抑制（只保留最强峰）
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

// 在 ARGB overlay 上以主峰为中心渲染高斯衰减蓝色热点圆
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

// 生成一整张声源定位热点图 overlay
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

// 云图线程：等显示线程告知尺寸 → 每 80ms 画一张 ARGB 热点图 → 双缓冲发布给显示/推流线程。
void run_audio_overlay_worker(OverlaySharedState *state) {
    int overlay_width = 0;
    int overlay_height = 0;
    {
        // 启动阶段轮询 5ms周期轮询ready，等显示线程把 LCD 尺寸写进来
        while (__atomic_load_n(&state->running, __ATOMIC_SEQ_CST) &&
               !__atomic_load_n(&state->ready, __ATOMIC_SEQ_CST)) {
            usleep(5000);
        }
        if (!__atomic_load_n(&state->running, __ATOMIC_SEQ_CST)) {
            return;
        }
        overlay_width = state->width;
        overlay_height = state->height;
    }

    std::vector<uint32_t> local_pixels(static_cast<size_t>(overlay_width) * overlay_height, 0);

    DumbBuffer local_buf;
    local_buf.pitch = static_cast<uint32_t>(overlay_width * sizeof(uint32_t));
    local_buf.size = static_cast<uint32_t>(local_pixels.size() * sizeof(uint32_t));
    local_buf.pixels = local_pixels.data();

    while (__atomic_load_n(&state->running, __ATOMIC_SEQ_CST)) {
        bool rotate_270 = __atomic_load_n(&state->rotate_270, __ATOMIC_SEQ_CST);    //云图是否旋转
        draw_sound_source_overlay(&local_buf, overlay_width, overlay_height, -1.0f, rotate_270);

        // 双缓冲发布：画到 pixels[1 - write_idx]（非活跃槽），再翻转 write_idx + 递增 generation。
        // 读取端（显示线程 / stream 线程）看到 generation 变了就会来拿 pixels[write_idx]。
        int next = 1 - __atomic_load_n(&state->write_idx, __ATOMIC_ACQUIRE);
        state->pixels[next] = local_pixels;
        __atomic_store_n(&state->write_idx, next, __ATOMIC_RELEASE);
        __atomic_fetch_add(&state->generation, 1, __ATOMIC_RELEASE);
    }
}
