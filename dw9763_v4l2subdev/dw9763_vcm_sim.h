#ifndef V4L2_DRM_IMX415_DW9763_VCM_SIM_H
#define V4L2_DRM_IMX415_DW9763_VCM_SIM_H

#include <cstdint>

struct Dw9763SimConfig {
    uint32_t max_current = 120;
    uint32_t start_current = 20;
    uint32_t rated_current = 90;
    uint32_t step_mode = 3;
    uint32_t t_src = 0x20;
    uint32_t t_div = 1;
};

class Dw9763VcmSim {
public:
    // 功能：
    //   构造一个 dw9763 VCM 应用层模拟对象。
    // 参数：
    //   无。
    // 返回值：
    //   无返回值。
    Dw9763VcmSim();

    // 功能：
    //   按逻辑焦点位置设置模拟镜头位置。
    // 参数：
    //   logical_pos: 逻辑焦点位置，范围通常为 [0, 64]。
    // 返回值：
    //   0 表示设置成功。
    int set_focus_absolute(unsigned int logical_pos);

    // 功能：
    //   获取当前逻辑焦点位置。
    // 参数：
    //   无。
    // 返回值：
    //   当前逻辑焦点位置。
    int get_focus_absolute() const;

    // 功能：
    //   获取当前换算后的模拟镜头寄存器值。
    // 参数：
    //   无。
    // 返回值：
    //   当前寄存器/电流位置。
    unsigned int get_lens_reg() const;

private:
    Dw9763SimConfig config_;
    unsigned int current_related_pos_;
    unsigned int current_lens_pos_;
    unsigned int step_;
};

#endif
