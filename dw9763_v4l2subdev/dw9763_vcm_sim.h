#ifndef V4L2_DRM_IMX415_DW9763_VCM_SIM_H
#define V4L2_DRM_IMX415_DW9763_VCM_SIM_H

#include <cstdint>

struct Dw9763SimConfig {
    // 模拟 VCM 的最大电流上限。
    uint32_t max_current = 120;
    // 镜头开始可运动时的起始电流。
    uint32_t start_current = 20;
    // 镜头额定电流，对应整个有效行程的上边界。
    uint32_t rated_current = 90;
    // 模拟步进模式参数。
    uint32_t step_mode = 3;
    // 模拟 VCM 的源时间寄存器参数。
    uint32_t t_src = 0x20;
    // 模拟 VCM 的时间分频参数。
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
    // 模拟器当前使用的配置参数集合。
    Dw9763SimConfig config_;
    // 当前逻辑焦点位置 [0,64] 对应的内部位置值。
    unsigned int current_related_pos_;
    // 当前换算后的镜头寄存器/电流位置。
    unsigned int current_lens_pos_;
    // 由配置推导出的单步电流增量。
    unsigned int step_;
};

#endif
