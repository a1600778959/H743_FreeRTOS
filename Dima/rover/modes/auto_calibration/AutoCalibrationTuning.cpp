#define MODULE_NAME "auto_cal"
#include "AutoCalibrationMode.hpp"
#include "logging/logging.hpp"

#include <algorithm>
#include <cmath>

namespace dima::rover::modes {

// —— 整定链调度适配（Tuning 支撑）———————————————————————————————
// 状态区压平后本文件只保留调度器与阶段 helper 共用的无状态支撑：整定配置
// 读取、姿态解算、估计器代次校验、闭环反馈有效性与采样节流。阶段推进、
// 事务与子状态全部由 AutoCalibrationMode.cpp 的调度器（PhaseSubstate/
// TransactionKind）及各阶段文件的 StepResult helper 承担，这里不再有任何
// 状态跳转或事务判断。

bool AutoCalibrationMode::read_tuning_config() noexcept
{
    px4::AtomicTransaction atomic;
    const auto read = [](dima::params parameter, float &value) {
        const auto handle = param_handle(parameter);
        param_set_used(handle);
        return param_get(handle, &value) == 0;
    };
    TuningConfig next{};
    const bool loaded = read(dima::params::RO_SPEED_P, next.inner[0]) && read(dima::params::RO_SPEED_I, next.inner[1]) &&
        read(dima::params::RO_YAW_RATE_P, next.inner[2]) && read(dima::params::RO_YAW_RATE_I, next.inner[3]) &&
        read(dima::params::RO_SPEED_LIM, next.speed_limit) && read(dima::params::RO_SPEED_TH, next.speed_threshold) &&
        read(dima::params::RO_YAW_RATE_LIM, next.rate_limit) && read(dima::params::RO_YAW_RATE_TH, next.rate_threshold) &&
        read(dima::params::RO_ACCEL_LIM, next.acceleration) && read(dima::params::RO_DECEL_LIM, next.deceleration) &&
        read(dima::params::RO_YAW_ACCEL_LIM, next.rate_acceleration) && read(dima::params::RO_YAW_DECEL_LIM, next.rate_deceleration) &&
        read(dima::params::RO_JERK_LIM, next.jerk) && read(dima::params::RO_SPEED_RED, next.speed_reduction) &&
        read(dima::params::RO_YAW_P, next.heading_p) && read(dima::params::NAV_ACC_RAD, next.acceptance) &&
        read(dima::params::PP_LOOKAHD_GAIN, next.pursuit.lookahead_gain) &&
        read(dima::params::PP_LOOKAHD_MIN, next.pursuit.lookahead_min_m) &&
        read(dima::params::PP_LOOKAHD_MAX, next.pursuit.lookahead_max_m) &&
        read(dima::params::RD_TRANS_TRN_DRV, next.driving.turn_to_drive_yaw_error_rad) &&
        read(dima::params::RD_TRANS_DRV_TRN, next.driving.drive_to_turn_yaw_error_rad);
    if (!loaded) return false;
    // RO_YAW_* 的限制沿用 PX4 deg/s、deg/s^2；PI 本身使用 rad/s 误差，
    // 因此 P/I 不作角度换算。RD_TRANS_* 已是 rad，不能再转换一次。
    // 本轮实测范围仅约束实验目标，未写回用户巡航参数；重读前端时保留
    // 已选择的范围交集，避免把目标恢复到尚未观测/不可执行的更高速度。
    if (runtime_cohort_ && std::isfinite(tuning_config_.speed_limit) && tuning_config_.speed_limit > 0.0F)
        next.speed_limit = std::min(next.speed_limit, tuning_config_.speed_limit);
    next.rate_limit *= kRadians;
    next.rate_threshold *= kRadians;
    next.rate_acceleration *= kRadians;
    next.rate_deceleration *= kRadians;
    next.driving.stopped_speed_threshold_m_s = next.speed_threshold;
    tuning_config_ = next;
    return true;
}

float AutoCalibrationMode::body_yaw() const noexcept
{
    const auto &q = attitude_sub_.get().q;
    return std::atan2(2.0F * (q[0] * q[3] + q[1] * q[2]),
        1.0F - 2.0F * (q[2] * q[2] + q[3] * q[3]));
}

bool AutoCalibrationMode::tuning_estimator_valid(std::uint64_t now) const noexcept
{
    const auto &position = position_sub_.get();
    const auto &odometry = odometry_sub_.get();
    // GNSS yaw 融合不等于水平位置/速度融合。静态等待即检查真实闭环反馈的
    // 依赖，避免先允许 Arm 再发现只有航向而没有可用 EKF 速度。
    return fresh(position.timestamp, now, 200000ULL) && fresh(position.timestamp_sample, now, 200000ULL) &&
        position.timestamp_sample <= position.timestamp && fresh(odometry.timestamp, now, 200000ULL) &&
        fresh(odometry.timestamp_sample, now, 200000ULL) && odometry.timestamp_sample <= odometry.timestamp &&
        position.xy_valid && position.v_xy_valid && position.xy_global && position.heading_good_for_control &&
        !position.dead_reckoning && position.ref_timestamp != 0U && position.ref_timestamp <= position.timestamp &&
        (tuning_reference_ == 0U || (position.ref_timestamp == tuning_reference_ &&
            position.xy_reset_counter == tuning_xy_reset_ && position.vxy_reset_counter == tuning_vxy_reset_ &&
            position.heading_reset_counter == tuning_yaw_reset_ && odometry.reset_counter == tuning_odom_reset_)) &&
        std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.vx) && std::isfinite(position.vy) &&
        std::isfinite(position.heading) && std::isfinite(position.ref_lat) && std::fabs(position.ref_lat) < 85.0 &&
        std::isfinite(position.ref_lon) && std::fabs(position.ref_lon) <= 180.0 && std::isfinite(odometry.angular_velocity[2]);
}

bool AutoCalibrationMode::tuning_feedback(std::uint64_t now) const noexcept
{
    const auto &feedback = control_feedback_sub_.get();
    return feedback.valid && feedback.source == rover_motion_request_s::SOURCE_CALIBRATION &&
        feedback.session_id == status_.session_id && feedback.closed_loop == status_.closed_loop &&
        fresh(feedback.timestamp, now, 100000ULL) && fresh(feedback.timestamp_sample, now, 100000ULL) &&
        feedback.timestamp_sample >= arm_started_ && rtk_yaw_fused(now) && tuning_estimator_valid(now) &&
        std::isfinite(feedback.speed_m_s) && std::isfinite(feedback.speed_raw_m_s) &&
        std::isfinite(feedback.forward_raw_m_s) && std::isfinite(feedback.lateral_raw_m_s) && std::isfinite(feedback.yaw_rate_rad_s);
}

bool AutoCalibrationMode::take_tuning_sample(std::uint64_t now, bool rate) noexcept
{
    if (!tuning_feedback(now)) return false;
    const auto timestamp = control_feedback_sub_.get().timestamp_sample;
    const std::uint64_t interval = rate ? 20000U : 100000U;
    // 容差只判断实际采样抖动，不能作为提前采样许可；否则 20 ms 调度会把
    // 标称 100 ms 的速度模型稳定地采成 80 ms，导致时间常数与 PI 单位错误。
    if (timestamp <= tuning_sample_ || (tuning_sample_ != 0U && timestamp - tuning_sample_ < interval)) return false;
    tuning_sample_ = timestamp;
    return true;
}

} // namespace dima::rover::modes
