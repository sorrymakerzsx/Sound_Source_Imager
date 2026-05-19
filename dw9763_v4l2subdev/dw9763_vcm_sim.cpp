#include "dw9763_vcm_sim.h"

#include <algorithm>
#include <iostream>
#include <unistd.h>

namespace {

static const unsigned int kDw9763MaxReg = 1023;

}  // namespace

// 功能：
//   构造一个应用层的 dw9763 模拟对象。
//   这里不访问真实硬件，只根据配置计算逻辑步进到寄存器值的映射关系。
// 参数：
//   无。
// 返回值：
//   无返回值。
Dw9763VcmSim::Dw9763VcmSim()
    : current_related_pos_(32),
      current_lens_pos_(0) {
    step_ = (config_.rated_current > config_.start_current)
                ? (config_.rated_current - config_.start_current) / 64
                : 1;
    if (step_ == 0) {
        step_ = 1;
    }
}

// 功能：
//   按 V4L2_CID_FOCUS_ABSOLUTE 的逻辑位置语义设置焦点。
//   逻辑位置范围参考 Rockchip 常见 AF 约定 [0, 64]，
//   再换算到模拟的 dw9763 寄存器/电流位置。
// 参数：
//   logical_pos: 逻辑焦点位置，数值越大通常表示镜头越往近处或远处移动，
//                具体方向由当前映射公式决定。
// 返回值：
//   0 表示设置成功；当前模拟实现不区分更多错误码。
int Dw9763VcmSim::set_focus_absolute(unsigned int logical_pos) {
    logical_pos = std::min(logical_pos, 64u);

    unsigned int position = 0;
    if (logical_pos >= 64) {
        position = config_.start_current;
    } else {
        position = config_.start_current + step_ * (64 - logical_pos);
    }

    if (position > kDw9763MaxReg) {
        position = kDw9763MaxReg;
    }

    current_related_pos_ = logical_pos;
    current_lens_pos_ = position;
    usleep(2000);

    std::cout << "[dw9763-sim] focus_abs=" << current_related_pos_
              << " reg=" << current_lens_pos_ << std::endl;
    return 0;
}

// 功能：
//   获取当前保存的逻辑焦点位置。
// 参数：
//   无。
// 返回值：
//   当前逻辑焦点位置，范围通常在 [0, 64]。
int Dw9763VcmSim::get_focus_absolute() const {
    return static_cast<int>(current_related_pos_);
}

// 功能：
//   获取当前模拟的镜头寄存器值。
// 参数：
//   无。
// 返回值：
//   当前换算后的“VCM 寄存器/电流位置”。
unsigned int Dw9763VcmSim::get_lens_reg() const {
    return current_lens_pos_;
}
