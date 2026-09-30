#define MODULE_NAME "auto_cal"
#include "AutoCalibrationMode.hpp"
#include "logging/logging.hpp"

#include <algorithm>
#include <cmath>

namespace dima::rover::modes {

// —— PROFILE 阶段 helper：入场检查与 WaitArm 就绪确认 ————————————————————
// 阶段文件只向调度器返回 StepResult；Arm gate、停车确认、平台序列推进与
// Runtime 事务由调度器的 PhaseSubstate/TransactionKind 承担。前进/CW/CCW
// 三个运动面共用本入场检查（序列 0=前进、1=CW、2=CCW，无倒退面）。

bool AutoCalibrationMode::feedback_unmasked(bool tracking_valid_at_limit) const noexcept
{
    const auto &f = control_feedback_sub_.get();
    // 普通非线性整形不属于保护介入；真正的限幅、等待和末端 slew 才使
    // 当前物理能力不可观。MOT 自身 slew 的活跃性另供策略比较使用。
    // publish_invalid会把限制标志清零、执行轴置NaN；不能将这种帧当作未受限。
    // 只要求执行轴有效，不把首次RTK直线采样绑到尚未就绪的EKF测量valid。
    return std::isfinite(f.longitudinal) && std::isfinite(f.steering) &&
        !f.braking_speed_limited && !f.mixing_limited && !f.arm_ramp_active && !f.reversal_held &&
        !f.safety_output_limited && !f.safety_slew_active && (!f.saturated || tracking_valid_at_limit);
}

StepResult AutoCalibrationMode::profile_prepare() noexcept
{
    // 响应采集入场检查+复位。RUNTIME 依赖门（调度器表）已由
    // continue_profile_entry 先行判定；这里只拒绝当前配置下不可观测的采集。
    // 准备只校验配置/预算；瞬时传感器未就绪交给现有 WaitArm 等待窗，
    // 不因阶段切换恰逢单帧重融合就直接废弃整组。
    if (!read_tuning_config() || !motion_configuration_valid()) {
        return fail_step(Status::FAILURE_PROFILE_UNOBSERVABLE);
    }
    px4::AtomicTransaction atomic;
    if (param_get(param_handle(dima::params::MOT_SLEW_RATE), &tuning_motor_slew_) != 0 ||
        param_get(param_handle(dima::params::MOT_ARM_RAMP), &tuning_arm_ramp_) != 0 ||
        !std::isfinite(tuning_motor_slew_) || tuning_motor_slew_ < 0.0F ||
        !std::isfinite(tuning_arm_ramp_) || tuning_arm_ramp_ < 0.0F) {
        return fail_step(Status::FAILURE_PARAMETER);
    }
    // 前进模式只辨识运行范围；全局 MIN/EXPO/ASYM/SLEW 不试改、不伪造反向证据。
    response_speed_.reset(); response_rate_.reset();
    for (float &value : response_rate_sum_) value = 0.0F;
    for (float &value : response_rate_duration_) value = 0.0F;
    profile_motion_ = profile_level_ = 0U;
    // 尚未确认本轮起步时返程应使用本轮直行证据，不能复用上一轮旋转的输入缓存。
    profile_input_floor_ = profile_input_ceiling_ = 0.0F;
    profile_started_ = profile_braking_ = false;
    response_stop_started_ = 0U;
    response_tail_timestamp_ = profile_motion_deadline_ = profile_phase_deadline_ = profile_stable_since_ = 0U;
    physical_speed_ = physical_rate_ = longitudinal_ = steering_ = 0.0F;
    status_.closed_loop = false;
    status_.gain_group = Status::GAIN_INNER;
    const auto &p = position_sub_.get();
    tuning_reference_ = p.ref_timestamp;
    tuning_xy_reset_ = p.xy_reset_counter; tuning_vxy_reset_ = p.vxy_reset_counter;
    tuning_yaw_reset_ = p.heading_reset_counter; tuning_odom_reset_ = odometry_sub_.get().reset_counter;
    return StepResult::Advance;
}

} // namespace dima::rover::modes
