#include "RoverDifferential.hpp"

#include <algorithm>
#include <cmath>

namespace dima::rover::control {

bool RoverDifferential::calibration_braking_command(float &longitudinal, float &steering,
    std::uint64_t now, float dt_s) noexcept
{
    const auto &status = calibration_sub_.get();
    const bool probe = status.straight_active &&
        status.braking_phase != auto_calibration_status_s::BRAKING_ACCELERATE;
    const bool full_brake_phase = probe && status.braking_phase == auto_calibration_status_s::BRAKING_ACTIVE;
    const bool settle = probe && status.braking_phase == auto_calibration_status_s::BRAKING_SETTLE;
    const auto &rtk = calibration_rtk_sub_.get();
    const float speed = std::hypot(rtk.velocity_north_m_s, rtk.velocity_east_m_s);
    // 相邻GPS历元允许配对。速度落后时将该历元差计入年龄；速度领先时
    // 仍以较早的航向到达时间保守计龄，不向未来外推，也不钳成当前时间。
    // 此时间仅用于鲜度/方向证据，观测积分仍按真实GPS速度历元计算。
    const auto lag_ms = std::max<std::int64_t>(0,
        static_cast<std::int64_t>(rtk.gps_milliseconds) - rtk.velocity_gps_milliseconds);
    const auto lag_us = static_cast<std::uint64_t>(lag_ms) * 1000ULL;
    const auto velocity_sample = rtk.timestamp_sample > lag_us ? rtk.timestamp_sample - lag_us : 0U;
    const bool velocity_valid = velocity_sample > 0 && velocity_sample <= now &&
        now - velocity_sample <= 300000ULL && std::isfinite(speed) &&
        rtk.solution_computed && rtk.integer_fixed && rtk.velocity_aligned;
    const auto *request = active_request();
    // 正常有效零/零请求就是停车意图，不等到motion_allowed=false后再尝试
    // 制动（那时请求已经被上游安全检查撤销）。纯旋转由调用方独立排除。
    const bool stop = status.active && std::fabs(longitudinal) < 1.0e-6F && std::fabs(steering) < 1.0e-6F &&
        (!status.closed_loop || (request != nullptr && request->speed_m_s == 0.0F && request->yaw_rate_rad_s == 0.0F));
    const bool full_output_trial = status.braking_full_output && status.straight_active &&
        status.motion_allowed && !status.closed_loop;
    // 巡航限速接管只服务未标定的开环输入。闭环已有Speed PI处理速度误差，
    // 不能在PI计算之后再覆盖纵向/转向，否则候选验证混入另一套控制器。
    // probe和明确零目标stop仍沿原公共停车通路执行。
    // 回带宽度复用现有速度测量阈值/停速分辨率；真正超速立即接管，不加
    // 冷却计时。接管后须回到下边界才释放，避免在同一限速线上反复换向。
    const float release_speed = std::min(calibration_fence_.speed_limit_m_s,
        std::max(0.08F, calibration_fence_.speed_limit_m_s -
            std::max(0.08F, parameters_.speed.measurement_threshold_m_s)));
    const bool speed_limit = !status.closed_loop && !probe && !stop && !full_output_trial &&
        (velocity_valid ? speed > (calibration_speed_limit_active_ ? release_speed : calibration_fence_.speed_limit_m_s)
                        : calibration_speed_limit_active_);
    if (full_brake_phase && !calibration_active_phase_seen_) {
        // 只有新正式观测才在停车通路内重置。限速交接到停车不能解除撤力锁存；
        // 纯限速结束后重新行驶，由下方非停车分支复位，下一次超速才重新接管。
        calibration_brake_active_ = calibration_brake_released_ = false;
    }
    calibration_active_phase_seen_ = full_brake_phase;
    calibration_speed_limit_active_ = speed_limit;
    if (probe || stop || full_output_trial || status.closed_loop) calibration_speed_recovery_input_ = -1.0F;
    if (speed_limit) calibration_speed_recovery_input_ = 0.0F;
    if (!probe && !stop && !speed_limit) {
        calibration_brake_active_ = calibration_brake_released_ = false;
        if (calibration_speed_recovery_input_ >= 0.0F) {
            // 限速交还只恢复到本次请求，沿用实验0.15/s斜率；不能从负向
            // 制动直接跳回满档。正常实验和正式满输出观测不叠加这条恢复斜坡。
            calibration_speed_recovery_input_ = std::min(1.0F,
                calibration_speed_recovery_input_ + 0.15F * std::clamp(dt_s, 0.0F, 0.05F));
            if (longitudinal > calibration_speed_recovery_input_) longitudinal = calibration_speed_recovery_input_;
            else calibration_speed_recovery_input_ = -1.0F;
            // 换向等待期间轮端可能仍为零；只确认冻结方向上的真实前行，
            // 保持方向证据新鲜，不把回溜的航迹反过来当成前进方向。
            if (velocity_valid && rtk.velocity_north_m_s * calibration_brake_north_ +
                rtk.velocity_east_m_s * calibration_brake_east_ > 0.08F)
                calibration_brake_direction_at_ = velocity_sample;
        }
        if (velocity_valid && speed > 0.08F && calibration_right_ > 0.0F && calibration_left_ > 0.0F) {
            calibration_brake_north_ = rtk.velocity_north_m_s / speed;
            calibration_brake_east_ = rtk.velocity_east_m_s / speed;
            calibration_brake_direction_at_ = static_cast<std::uint64_t>(velocity_sample);
        }
        return false;
    }
    longitudinal = steering = 0.0F;
    // 测速失效是本次停车的撤力终点；恢复数据不得复活同一次反向制动。
    if (settle || !velocity_valid) calibration_brake_released_ = true;
    if (calibration_brake_released_) return true;
    if (!calibration_brake_active_) {
        // 方向来自此前已执行的双轮前进；停车过程中冻结，不能把过零后的
        // 倒退重新解释成前进。正式观测和普通停车共用同一撤力/换向入口。
        if (speed > 0.08F && calibration_right_ > 0.0F && calibration_left_ > 0.0F) {
            calibration_brake_north_ = rtk.velocity_north_m_s / speed;
            calibration_brake_east_ = rtk.velocity_east_m_s / speed;
            calibration_brake_direction_at_ = static_cast<std::uint64_t>(velocity_sample);
        }
        if (calibration_brake_direction_at_ == 0U || now < calibration_brake_direction_at_ ||
            now - calibration_brake_direction_at_ > 300000ULL) return true;
        calibration_brake_active_ = true;
        calibration_brake_input_ = 0.0F;
        calibration_brake_initial_speed_ = rtk.velocity_north_m_s * calibration_brake_north_ +
            rtk.velocity_east_m_s * calibration_brake_east_;
        return true;
    }
    const float forward = rtk.velocity_north_m_s * calibration_brake_north_ + rtk.velocity_east_m_s * calibration_brake_east_;
    const float target = speed_limit ? release_speed : 0.08F;
    if (!std::isfinite(forward) || forward <= target) calibration_brake_released_ = true;
    if (calibration_brake_released_) return true;
    if (calibration_braking_generation_ == 0U) {
        // 未辨识时从零沿用实验0.15/s渐进激励，未知车辆不直接施加30%反推。
        // 上限仅为本轮E包络；不是固定脉冲，近零/失鲜仍由原释放锁存撤力。
        calibration_brake_input_ = std::min(1.0F,
            calibration_brake_input_ + 0.15F * std::clamp(dt_s, 0.0F, 0.05F));
        // 探测幅值随剩余前向速度缩小，首轮不再速度盲爬升。
        if (!(calibration_brake_initial_speed_ > 0.0F)) return true;
        longitudinal = -calibration_brake_input_ * std::clamp(forward / calibration_brake_initial_speed_, 0.0F, 1.0F);
    } else {
        // 限速和停车都按零速误差产生制动力，分别在回带线/停速线撤力。
        // 第二轮及普通返程共用冻结g；g=0代表已测得无需反向力的滑停。
        calibration_brake_input_ = std::clamp(calibration_brake_input_gain_ * forward, 0.0F, 1.0F);
        longitudinal = -calibration_brake_input_;
    }
    return true;
}

} // namespace dima::rover::control
