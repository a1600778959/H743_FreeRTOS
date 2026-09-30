#include "api/Flash.hpp"
#include "api/Services.hpp"
#include "RoverDifferential.hpp"

#include "rover/RoverModeContract.hpp"
#include "rover/CalibrationBraking.hpp"
#include "rover/CalibrationMath.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace dima::rover::control {
namespace {

constexpr float kUnavailable = std::numeric_limits<float>::quiet_NaN();

bool fresh(std::uint64_t timestamp, std::uint64_t now,
           std::uint64_t limit) noexcept
{
    return timestamp != 0U && timestamp <= now && now - timestamp <= limit;
}

// 正式制动观测只由公开诊断字段表达：直线族 straight_active + ACTIVE/SETTLE
// 制动相位（离开制动观测子状态即复位）；停车/保存窗口由 motion 字段与
// 两轮协议判据表达，外部安全模块不依赖阶段枚举。
bool braking_observation_active(const auto_calibration_status_s &status) noexcept
{
    return status.straight_active &&
        (status.braking_phase == auto_calibration_status_s::BRAKING_ACTIVE ||
         status.braking_phase == auto_calibration_status_s::BRAKING_SETTLE);
}

} // namespace

bool RoverDifferential::calibration_gains_applied(
    std::uint32_t instance, const float (&gains)[4]) const noexcept
{
    px4::AtomicTransaction transaction;
    // 回滚到四个零增益时 Navigation 会按设计变为不可用；这里只确认同一代
    // 参数和值已被真实控制器消费，不能把闭环 readiness 误作回滚成功条件。
    return applied_parameter_valid_ && applied_parameter_instance_ == instance &&
        parameters_.speed.proportional_gain == gains[0] &&
        parameters_.speed.integral_gain == gains[1] &&
        parameters_.yaw_rate.proportional_gain == gains[2] &&
        parameters_.yaw_rate.integral_gain == gains[3];
}

bool RoverDifferential::calibration_control_ready(
    std::uint32_t instance) const noexcept
{
    px4::AtomicTransaction transaction;
    return applied_parameter_valid_ && applied_parameter_instance_ == instance &&
           navigation_parameters_valid_;
}

bool RoverDifferential::calibration_yaw_control_ready(float minimum_target_rate) const noexcept
{
    px4::AtomicTransaction transaction;
    // 只判断共用控制器能否覆盖本次机动的最小目标；死区高于到位前目标会
    // 让车永远停在容差外。此结果只选择实验通路，不作为校准运动准入门。
    return applied_parameter_valid_ && !parameter_update_pending_ &&
        parameters_valid_ && yaw_rate_parameters_valid_ &&
        std::isfinite(minimum_target_rate) && minimum_target_rate > 0.0F &&
        parameters_.yaw_rate.measurement_threshold_rad_s < minimum_target_rate &&
        parameters_.yaw_rate.yaw_rate_limit_rad_s >= minimum_target_rate &&
        parameters_.yaw_rate.measurement_threshold_rad_s < parameters_.yaw_rate.yaw_rate_limit_rad_s;
}

bool RoverDifferential::calibration_deceleration_applied(std::uint32_t instance, float deceleration) const noexcept
{
    px4::AtomicTransaction transaction;
    // 初始 PI 可以尚未配置；这里只确认同代减速度已被控制器和停车模型消费。
    return applied_parameter_valid_ && applied_parameter_instance_ == instance &&
        parameters_.speed.deceleration_limit_m_s2 == deceleration &&
        calibration_braking_deceleration_ == deceleration && deceleration > 0.0F;
}

bool RoverDifferential::calibration_braking_model_applied(std::uint32_t session, std::uint8_t generation, float deceleration) const noexcept
{
    px4::AtomicTransaction transaction;
    return calibration_fence_valid_ && session == calibration_fence_session_id_ && generation != 0U &&
        generation == calibration_braking_generation_ && deceleration == calibration_braking_deceleration_ &&
        calibration_sub_.get().braking_input_gain == calibration_brake_input_gain_;
}

void RoverDifferential::refresh_calibration_fence(
    std::uint64_t now_us) noexcept
{
    const auto &status = calibration_sub_.get();
    if (status.session_id == 0U) return;

    if (!calibration_fence_latched_ ||
        status.session_id != calibration_fence_session_id_) {
        // 新会话只能在 Commander 的完整 Disarmed 同拍快照下锁定一次。阶段
        // Disarm 仍是同一 session，绝不能把车辆当前位置重新定义为圆心。
        if (!status.active || !(fresh_disarmed_snapshot(now_us) ||
            (active_snapshot_fresh(now_us) && safety_.vehicle_status.nav_state == vehicle_status_s::NAVIGATION_STATE_EXTERNAL1 &&
             dima::platform::services().armed_flash.calibration_output_stopped()))) return;
        dima::platform::ConfigurationUpdateLease lease{dima::platform::services().armed_flash};
        if (!lease) return;
        calibration_fence_latched_ = true;
        calibration_fence_session_id_ = status.session_id;
        calibration_fence_center_timestamp_ = status.fence_center_timestamp;
        calibration_fence_device_id_ = status.fence_device_id;
        const auto limits = dima::lib::rover::calibration::session_limits(parameters_.calibration_entry_cruise,
            parameters_.drive.throttle_max);
        calibration_entry_cruise_ = parameters_.calibration_entry_cruise;
        calibration_session_motor_limit_ = limits.motor_output;
        calibration_straight_length_m_ = parameters_.calibration_straight_distance_m;
        calibration_braking_deceleration_ = 0.0F;
        calibration_brake_input_gain_ = calibration_brake_input_ = 0.0F;
        calibration_speed_recovery_input_ = -1.0F;
        calibration_probe_run_ = 255U;
        calibration_braking_generation_ = 0U;
        calibration_brake_active_ = calibration_brake_released_ = false;
        calibration_active_phase_seen_ = false;
        calibration_speed_limit_active_ = false;
        calibration_brake_direction_at_ = 0U;
        calibration_fence_ = {status.fence_latitude_deg,
                              status.fence_longitude_deg,
                              status.fence_origin_error_m,
                              status.fence_radius_m,
                              status.entry_deceleration_m_s2, limits.speed_m_s};

        const auto origin = dima::lib::rover::calibration::evaluate_braking_probe(
            calibration_fence_, calibration_straight_length_m_, calibration_fence_.latitude_deg,
            calibration_fence_.longitude_deg,
            calibration_fence_.origin_error_m, 0.0F);
        calibration_fence_valid_ = limits.valid && status.fence_center_valid &&
            status.session_speed_limit_m_s == limits.speed_m_s &&
            status.session_motor_limit == limits.motor_output &&
            status.entry_cruise_speed_m_s == calibration_entry_cruise_ &&
            fresh(status.timestamp, now_us, 100000ULL) &&
            calibration_fence_center_timestamp_ != 0U &&
            calibration_fence_center_timestamp_ <= status.timestamp &&
            calibration_fence_device_id_ != 0U &&
            valid_calibration_ceiling_snapshot(parameters_) &&
            calibration_fence_.radius_m == parameters_.calibration_radius_m &&
            status.straight_distance_m == calibration_straight_length_m_ &&
            calibration_fence_.deceleration_m_s2 == parameters_.speed.deceleration_limit_m_s2 && origin.can_stop;
        return;
    }

    // 同一 session 的任一锁定字段或对应参数只要变化一次，本会话永久失效；
    // 即使稍后改回旧值也不能恢复，必须由新的显式校准会话重新取得圆心。
    if (!calibration_fence_status_unchanged())
        calibration_fence_valid_ = false;
    if (!calibration_fence_valid_ || !status.active || !fresh(status.timestamp, now_us, 100000ULL)) return;
    if (braking_observation_active(status) && status.braking_run != calibration_probe_run_) {
        // 内联两轮协议：观测从直行稳定平台触发，首轮 run=1 且代次未增；
        // 上一轮完成后 run 递增 1 且 generation 恰好等于 run-1（每轮观测
        // 只 bump 一次代次）。旧三轮 0/1/2 与临时模型协议已退役。
        if (status.braking_run > 2U || (calibration_probe_run_ == 255U ? status.braking_run != 1U
            : status.braking_run != calibration_probe_run_ + 1U || calibration_braking_generation_ + 1U != status.braking_run)) {
            calibration_fence_valid_ = false; return;
        }
        calibration_probe_run_ = status.braking_run;
    }
    // 只有当前轮次的制动停止窗口（SETTLE 相位）、真实停波和连续观测代次才
    // 接收 RAM 模型。每轮观测保存后授权恢复直行与下一平台观测；普通运动还
    // 要求最终保存。候选沿用 RO_DECEL_LIM 的有效正值域（至100 m/s²），
    // 不再依赖旧测量公式人为封顶的1.5，否则有效实测模型会无法完成交接。
    if (braking_observation_active(status) &&
        status.braking_model_generation == calibration_braking_generation_ + 1U &&
        status.braking_model_generation == calibration_probe_run_ &&
        status.braking_observations == calibration_probe_run_ && status.motion_inhibited && !status.motion_allowed &&
        dima::platform::services().armed_flash.calibration_output_stopped() &&
        std::isfinite(status.braking_deceleration_m_s2) && status.braking_deceleration_m_s2 > 0.0F &&
        status.braking_deceleration_m_s2 <= 100.0F &&
        std::isfinite(status.braking_input_gain) && status.braking_input_gain >= 0.0F) {
        calibration_braking_deceleration_ = status.braking_deceleration_m_s2;
        calibration_brake_input_gain_ = status.braking_input_gain;
        calibration_braking_generation_ = status.braking_model_generation;
    }
}

bool RoverDifferential::calibration_fence_status_unchanged() const noexcept
{
    const auto &status = calibration_sub_.get();
    return calibration_fence_latched_ && status.fence_center_valid &&
        status.session_id == calibration_fence_session_id_ &&
        status.fence_center_timestamp == calibration_fence_center_timestamp_ &&
        status.fence_device_id == calibration_fence_device_id_ &&
        status.fence_latitude_deg == calibration_fence_.latitude_deg &&
        status.fence_longitude_deg == calibration_fence_.longitude_deg &&
        status.fence_origin_error_m == calibration_fence_.origin_error_m &&
        status.fence_radius_m == calibration_fence_.radius_m &&
        status.entry_deceleration_m_s2 == calibration_fence_.deceleration_m_s2 &&
        status.straight_distance_m == calibration_straight_length_m_ &&
        status.session_speed_limit_m_s == calibration_fence_.speed_limit_m_s &&
        status.session_motor_limit == calibration_session_motor_limit_ &&
        status.entry_cruise_speed_m_s == calibration_entry_cruise_ &&
        parameters_.drive.throttle_max == calibration_session_motor_limit_ &&
        parameters_.calibration_radius_m == calibration_fence_.radius_m &&
        parameters_.calibration_straight_distance_m == calibration_straight_length_m_;
}

float RoverDifferential::calibration_motor_limit() const noexcept
{
    return calibration_session_motor_limit_;
}

bool RoverDifferential::yaw_rate_measurement(std::uint64_t now_us, float &yaw_rate_rad_s,
    std::uint64_t &timestamp_sample) const noexcept
{
    // 纯旋转用同一份 EKF 机体系角速度，不把天线平移速度当车体前进速度。
    // 只放开未参与控制的平移测量依赖，角速度本身仍要求新鲜且有限。
    if (!have_odometry_ || !fresh(vehicle_odometry_.timestamp, now_us, kEstimatorTimeoutUs) ||
        !fresh(vehicle_odometry_.timestamp_sample, now_us, kEstimatorTimeoutUs) ||
        vehicle_odometry_.timestamp_sample > vehicle_odometry_.timestamp ||
        !std::isfinite(vehicle_odometry_.angular_velocity[2])) return false;
    yaw_rate_rad_s = vehicle_odometry_.angular_velocity[2];
    timestamp_sample = vehicle_odometry_.timestamp_sample;
    return true;
}

bool RoverDifferential::control_measurement(
    std::uint64_t now_us, float &speed_m_s, float &yaw_rate_rad_s,
    std::uint64_t &timestamp_sample) const noexcept
{
    if (!navigation_estimator_valid(now_us)) return false;
    const auto speed = dima::lib::rover::measure_body_speed(
        vehicle_local_position_.vx, vehicle_local_position_.vy,
        vehicle_local_position_.heading,
        parameters_.speed.measurement_threshold_m_s);
    const float rate = vehicle_odometry_.angular_velocity[2];
    if (!speed.valid || !std::isfinite(speed.speed_m_s) ||
        !std::isfinite(rate)) return false;
    speed_m_s = speed.speed_m_s;
    yaw_rate_rad_s = rate;
    timestamp_sample = std::min(vehicle_local_position_.timestamp_sample,
                                vehicle_odometry_.timestamp_sample);
    return timestamp_sample != 0U;
}

bool RoverDifferential::execute_heading_target(const rover_motion_request_s &request,
    std::uint64_t now_us, ControlCycleFeedback &feedback) noexcept
{
    namespace math = dima::lib::rover::calibration;
    constexpr float pi = 3.14159265358979323846F;
    constexpr float tolerance = 3.0F * pi / 180.0F; // 沿用既有掉头角度容差，不引入目标转速。
    auto &turn = heading_maneuver_;
    const auto session = calibration_sub_.get().session_id;
    if (turn.request_timestamp != request.heading_request_timestamp || turn.session != session) {
        turn = {};
        turn.request_timestamp = request.heading_request_timestamp;
        turn.session = session;
        turn.target = request.heading_target_rad;
        turn.initial_direction = request.heading_direction;
        turn.result = rover_control_status_s::HEADING_RUNNING;
    }
    // 同一标识的目标不可被悄悄改写；后续阶段的新目标必须使用新的机动标识。
    if (turn.target != request.heading_target_rad || turn.initial_direction != request.heading_direction)
        turn.result = rover_control_status_s::HEADING_FAILED;
    feedback.longitudinal = feedback.steering = 0.0F;
    const auto &rtk = calibration_rtk_sub_.get();
    const auto &imu = heading_imu_sub_.get();
    if (!fresh(rtk.timestamp_sample, now_us, 300000ULL) || !rtk.solution_computed ||
        !rtk.integer_fixed || !rtk.velocity_aligned || rtk.device_id != calibration_fence_device_id_ ||
        !std::isfinite(rtk.array_heading_rad) || !fresh(imu.timestamp_sample, now_us, 100000ULL) ||
        imu.delta_angle_dt == 0U) return false;
    const float rate = imu.delta_angle[2] / (1.0e-6F * imu.delta_angle_dt);
    const float speed = std::hypot(rtk.velocity_north_m_s, rtk.velocity_east_m_s);
    if (!std::isfinite(rate) || !std::isfinite(speed)) return false;
    feedback.measurement_valid = true;
    feedback.speed_m_s = speed;
    feedback.yaw_rate_rad_s = rate;
    feedback.timestamp_sample = std::min(imu.timestamp_sample, rtk.timestamp_sample);
    const auto &output = heading_output_sub_.get();
    const bool released = fresh(output.timestamp, now_us, 100000ULL) &&
        fresh(output.timestamp_output, now_us, 100000ULL) && output.backend_ready &&
        !output.parameter_update_pending && output.timestamp_output >= turn.request_timestamp &&
        (output.state == actuator_output_status_s::STATE_ACTIVE || output.state == actuator_output_status_s::STATE_CONTROL_INHIBITED) &&
        std::isfinite(output.applied_right) && std::isfinite(output.applied_left) &&
        std::fabs(output.applied_right) < 1.0e-4F && std::fabs(output.applied_left) < 1.0e-4F;
    const bool stopped = speed < 0.08F && std::fabs(rate) < 0.05F;
    if (turn.result == rover_control_status_s::HEADING_FAILED ||
        turn.result == rover_control_status_s::HEADING_COMPLETE) return true;
    const std::uint64_t epoch = static_cast<std::uint64_t>(rtk.gps_week) * 604800000ULL + rtk.gps_milliseconds;
    if (!turn.started) {
        if (!stopped || !released) return true; // 先由公共停车链处理去程滑行，不带平移起转。
        turn.started = true;
        turn.last_heading = rtk.array_heading_rad;
        turn.last_heading_epoch = epoch;
        turn.remaining = math::wrap_pi(turn.target - turn.last_heading);
        if (turn.initial_direction > 0 && turn.remaining < -tolerance) turn.remaining += 2.0F * pi;
        if (turn.initial_direction < 0 && turn.remaining > tolerance) turn.remaining -= 2.0F * pi;
    } else if (epoch > turn.last_heading_epoch) {
        const float delta = math::wrap_pi(rtk.array_heading_rad - turn.last_heading);
        if (turn.remaining * (turn.remaining - delta) <= 0.0F) turn.stopping = true;
        turn.remaining -= delta;
        turn.last_heading = rtk.array_heading_rad;
        turn.last_heading_epoch = epoch;
    }
    if (std::fabs(turn.remaining) < tolerance) turn.stopping = true;
    if (turn.stopping) {
        // 到位或越过目标先撤力，确认实际停稳/轮端归零再报告完成或执行必要的小角度修正。
        if (turn.stop_requested_at == 0U) turn.stop_requested_at = now_us;
        turn.opposed_since = 0U;
        if (!stopped || !released || output.timestamp_output < turn.stop_requested_at) return true;
        if (std::fabs(turn.remaining) < tolerance) turn.result = rover_control_status_s::HEADING_COMPLETE;
        else { turn.stopping = false; turn.stop_requested_at = 0U; }
        return true;
    }
    const float direction = turn.remaining > 0.0F ? 1.0F : -1.0F;
    const bool opposed = rate * direction < -0.03F;
    if (opposed) {
        if (turn.opposed_since == 0U) { turn.opposed_since = now_us; turn.opposed_heading = rtk.array_heading_rad; }
        const float delta = math::wrap_pi(rtk.array_heading_rad - turn.opposed_heading);
        if (now_us - turn.opposed_since >= 300000ULL && delta * direction < 0.0F && std::fabs(delta) >= pi / 180.0F) {
            turn.result = rover_control_status_s::HEADING_FAILED;
            return true;
        }
    } else turn.opposed_since = 0U;
    // 用户指定固定幅度范围0.4～0.8，取中值0.6；不根据起转角速度保持或继续探测输入。
    // 这是混控前归一化转向轴，实际轮端仍由公共整形、换向等待和MOT_THR_MAX约束。
    feedback.steering = direction * 0.6F;
    return true;
}

bool RoverDifferential::publish_control_status(
    std::uint64_t now_us, const rover_motion_request_s *request,
    const ControlCycleFeedback &feedback) noexcept
{
    rover_control_status_s status{};
    status.timestamp = now_us;
    status.timestamp_sample = feedback.measurement_valid
        ? feedback.timestamp_sample : 0U;
    status.parameter_update_instance = applied_parameter_valid_
        ? applied_parameter_instance_ : 0U;
    status.source = request != nullptr ? request->source
                                       : rover_motion_request_s::SOURCE_MANUAL;
    status.session_id = request != nullptr &&
        request->source == rover_motion_request_s::SOURCE_CALIBRATION
        ? calibration_sub_.get().session_id : 0U;
    status.request_sequence = request != nullptr ? request->sequence : 0U;
    // 初探直接使用 RTK，不要求尚未完成校准的 EKF 速度反馈 valid。
    // 控制器先接管时必须通知模式锁住采样并完成零输出交还握手。
    status.braking_speed_limited = feedback.output_valid &&
        status.source == rover_motion_request_s::SOURCE_CALIBRATION &&
        (calibration_speed_limit_active_ || calibration_speed_recovery_input_ >= 0.0F);
    if (request != nullptr && request->mode == rover_motion_request_s::MODE_HEADING_TARGET &&
        heading_maneuver_.request_timestamp == request->heading_request_timestamp && heading_maneuver_.session == status.session_id) {
        status.heading_request_timestamp = heading_maneuver_.request_timestamp;
        status.heading_error_rad = heading_maneuver_.started ? heading_maneuver_.remaining : kUnavailable;
        status.heading_result = feedback.output_valid || heading_maneuver_.result == rover_control_status_s::HEADING_FAILED
            ? heading_maneuver_.result : rover_control_status_s::HEADING_IDLE;
    }
    status.closed_loop = feedback.closed_loop;
    status.valid = feedback.output_valid && feedback.measurement_valid;
    status.saturated = feedback.output_valid && feedback.saturated;
    status.input_limited = feedback.output_valid && feedback.input_limited;
    status.motor_slew_active = feedback.output_valid && feedback.motor_slew_active;
    status.mixing_limited = feedback.output_valid && feedback.mixing_limited;
    status.shaping_active = feedback.output_valid && feedback.shaping_active;
    status.arm_ramp_active = feedback.output_valid && feedback.arm_ramp_active;
    status.reversal_held = feedback.output_valid && feedback.reversal_held;
    status.safety_output_limited = feedback.output_valid && feedback.safety_output_limited;
    status.safety_slew_active = feedback.output_valid && feedback.safety_slew_active;
    status.speed_setpoint_m_s = feedback.closed_loop
        ? feedback.speed_setpoint_m_s : kUnavailable;
    status.yaw_rate_setpoint_rad_s = feedback.closed_loop
        ? feedback.yaw_rate_setpoint_rad_s : kUnavailable;
    status.speed_m_s = feedback.measurement_valid ? feedback.speed_m_s
                                                   : kUnavailable;
    const auto raw = dima::lib::rover::measure_body_speed(vehicle_local_position_.vx,
        vehicle_local_position_.vy, vehicle_local_position_.heading, 0.0F);
    // 只有偏航反馈有效时，平移字段保持 NaN，不能把旧位置数据伪装成新测速。
    // 航向机动使用RTK地速，不把这一有效性冒充为新鲜EKF机体系速度。
    const bool speed_valid = feedback.measurement_valid && std::isfinite(feedback.speed_m_s) && raw.valid &&
        (request == nullptr || request->mode != rover_motion_request_s::MODE_HEADING_TARGET);
    status.speed_raw_m_s = speed_valid ? raw.speed_m_s : kUnavailable;
    status.forward_raw_m_s = speed_valid ? raw.forward_m_s : kUnavailable;
    status.lateral_raw_m_s = speed_valid ? raw.lateral_m_s : kUnavailable;
    status.yaw_rate_rad_s = feedback.measurement_valid
        ? feedback.yaw_rate_rad_s : kUnavailable;
    status.longitudinal = feedback.output_valid ? feedback.longitudinal
                                                 : kUnavailable;
    status.steering = feedback.output_valid ? feedback.steering : kUnavailable;
    // applied_* 是由最终左右执行器归一化命令反算的轴量，供辨识核对整形映射；
    // 它们不是物理轮速、车速或角速度，不能替代 estimator measurement。
    status.applied_longitudinal = feedback.output_valid
        ? feedback.applied_longitudinal : kUnavailable;
    status.applied_steering = feedback.output_valid
        ? feedback.applied_steering : kUnavailable;
    status.speed_integral = feedback.closed_loop && feedback.output_valid
        ? feedback.speed_integral : kUnavailable;
    status.yaw_rate_integral = feedback.closed_loop && feedback.output_valid
        ? feedback.yaw_rate_integral : kUnavailable;
    return control_status_publication_.publish(status);
}

} // namespace dima::rover::control
