// SPDX-License-Identifier: BSD-3-Clause
#include "SegmentGuidance.hpp"

#include <cmath>
#include <cfloat>

namespace dima::lib::rover {

float segment_arrival_speed(Position2f start, Position2f target, Position2f next,
    bool final_waypoint, float cruise_speed, float maximum_speed,
    float turn_threshold, float reduction_gain) noexcept
{
    if (final_waypoint) return 0.0F;
    const float incoming = std::hypot(target.north_m - start.north_m, target.east_m - start.east_m);
    const float outgoing = std::hypot(next.north_m - target.north_m, next.east_m - target.east_m);
    if (!std::isfinite(incoming) || !std::isfinite(outgoing)) return NAN;
    if (incoming <= FLT_EPSILON || outgoing <= FLT_EPSILON) return 0.0F;
    constexpr float pi = 3.14159265358979323846F;
    const float bearing_delta = std::atan2(next.east_m - target.east_m, next.north_m - target.north_m) -
        std::atan2(target.east_m - start.east_m, target.north_m - start.north_m);
    float angle = std::fmod(bearing_delta + pi, 2.0F * pi);
    if (angle < 0.0F) angle += 2.0F * pi;
    angle = std::fabs(angle - pi);
    if (!std::isfinite(angle)) return NAN;
    // 原生产规则：大转角停车，小转角按同一降速函数带速通过。
    if (angle > turn_threshold) return 0.0F;
    return std::fmax(0.0F, reduce_speed_for_heading_error(
        cruise_speed, angle, maximum_speed, reduction_gain));
}

WaypointProgress update_waypoint_progress(bool inside, bool final_waypoint,
    float arrival_speed, float measured_speed, float stopped_threshold,
    bool &arrival_latched) noexcept
{
    // 首次进入到达半径即锁存，抖出半径不重新加速；最后点/停车点须实测停稳。
    arrival_latched = arrival_latched || inside;
    const bool stop = final_waypoint || arrival_speed <= stopped_threshold;
    return {arrival_latched && stop,
        arrival_latched && (!stop || std::fabs(measured_speed) <= stopped_threshold)};
}

SegmentGuidanceOutput update_segment(PurePursuit &pursuit,
                                      HeadingController &heading,
                                      DrivingStateMachine &driving,
                                      const SegmentGuidanceInput &input) noexcept
{
    SegmentGuidanceOutput output{};
    output.measured_speed = measure_body_speed(input.velocity_north,
        input.velocity_east, input.yaw, input.speed_threshold);
    if (!output.measured_speed.valid) return output;

    // Pure Pursuit 的 L=clamp(k*|ground_speed|,Lmin,Lmax) 必须使用水平模长，
    // 不能复用带前向符号的 speed_m_s，否则纯横向滑动会错误缩短前视距离。
    const float ground_speed = std::hypot(output.measured_speed.forward_m_s,
                                         output.measured_speed.lateral_m_s);
    if (!std::isfinite(ground_speed)) return output;
    output.pursuit = pursuit.update(input.start, input.target, input.position, ground_speed);
    if (!output.pursuit.valid) return output;

    // 保持原 AutoMode 的 [-pi,pi) 包角与求值顺序；Driving 使用未经过
    // Heading slew 的真实路径误差，第一拍便能识别侧后方目标并停车。
    constexpr float pi = 3.14159265358979323846F;
    float error = std::fmod(output.pursuit.target_bearing_rad - input.yaw + pi, 2.0F * pi);
    if (error < 0.0F) error += 2.0F * pi;
    output.heading_error = error - pi;
    if (!std::isfinite(output.heading_error)) return output;
    output.heading = heading.update(output.pursuit.target_bearing_rad, input.yaw, input.dt_s);
    if (!output.heading.valid) return output;

    output.speed_plan = plan_waypoint_speed(output.pursuit.distance_to_waypoint_m,
        input.acceptance_radius, input.cruise_speed, input.arrival_speed,
        input.cruise_speed, input.jerk, input.deceleration);
    if (!output.speed_plan.valid) return output;
    const float requested_speed = reduce_speed_for_heading_error(
        output.speed_plan.speed_setpoint_m_s, output.heading_error,
        input.maximum_speed, input.speed_reduction);
    const float state_speed = driving.state() == DrivingState::Driving ? requested_speed : 0.0F;
    output.driving = driving.update(output.heading_error, state_speed, output.measured_speed.speed_m_s);
    if (!output.driving.valid) return output;

    // 停车确认阶段同时抑制平移和航向控制，后续原地转向/恢复行驶沿用同一
    // DrivingStateMachine，不由校准模式另行模拟这组开关条件。
    output.speed_setpoint = output.driving.translation_enabled ? requested_speed : 0.0F;
    output.yaw_rate_setpoint = output.driving.heading_control_enabled
        ? output.heading.yaw_rate_setpoint_rad_s : 0.0F;
    output.valid = std::isfinite(output.speed_setpoint) &&
        std::isfinite(output.yaw_rate_setpoint) && std::isfinite(output.heading_error);
    return output;
}

} // namespace dima::lib::rover
