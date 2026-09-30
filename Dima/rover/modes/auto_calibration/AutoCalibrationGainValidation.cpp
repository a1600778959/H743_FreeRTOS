#define MODULE_NAME "auto_cal"
#include "AutoCalibrationMode.hpp"
#include "logging/logging.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace dima::rover::modes {
namespace math = dima::lib::rover::calibration;

// —— VALIDATION 阶段 helper：闭环阶跃验证与停波后修复 ———————————————————
// 无倒退：速度组 exercise 0-2（半幅升/全幅升/全幅降）、角速度组 3-8
//（CW/CCW 各半幅与全幅），Heading 组两方向。段间返程经 WantReturn 上抛
//（resume_* 由本文件写好），段组完成经 WantStop 交调度器真实停波后推进
// cohort。修复只改候选并在 WaitStop 内应用，不再有任何状态跳转。

bool AutoCalibrationMode::start_validation(std::uint64_t now) noexcept
{
    status_.closed_loop = true;
    exercise_running_ = false;
    physical_speed_ = physical_rate_ = 0.0F;
    validation_passed_ = false;
    if (status_.gain_group == Status::GAIN_INNER) {
        if (exercise_ >= 9U) {
            return fail_flag(Status::FAILURE_GAIN_VALIDATION);
        }
        // 无倒退：速度组 0-2、偏航率组 3-8，不再有倒退验证组。
        if (exercise_ < 3U && !prepare_straight(now)) {
            return fail_flag(Status::FAILURE_FENCE_SPACE);
        }
        return true;
    } else if (status_.gain_group == Status::GAIN_HEADING) {
        if (!tuning_heading_.configure({status_.heading_p, tuning_config_.rate_limit})) {
            return fail_flag(Status::FAILURE_GAIN_VALIDATION);
        }
        exercise_ = 0U;
        return true;
    } else if (status_.gain_group == Status::GAIN_NAVIGATION) {
        if (!tuning_heading_.configure({tuning_config_.heading_p, tuning_config_.rate_limit}) ||
            !tuning_driving_.configure(tuning_config_.driving)) {
            return fail_flag(Status::FAILURE_GAIN_VALIDATION);
        }
        exercise_ = navigation_phase_ = navigation_directions_ = 0U;
        navigation_phase_started_ = now;
        exercise_heading_ = body_yaw();
        return true;
    }
    return reset_navigation_trial(now);
}

StepResult AutoCalibrationMode::validation_inner(std::uint64_t now, bool rate) noexcept
{
    // 计数只允许前进 0..2、CW 3..5、CCW 6..8；错序直接失败，避免无界复跑。
    if ((rate && (exercise_ < 3U || exercise_ >= 9U)) || (!rate && exercise_ >= 3U)) {
        return fail_step(Status::FAILURE_GAIN_VALIDATION);
    }
    const auto &feedback = control_feedback_sub_.get();
    if (!transaction_.generation_valid() ||
        feedback.parameter_update_instance != transaction_.generation()) {
        return fail_step(Status::FAILURE_GAIN_VALIDATION);
    }
    const unsigned phase = rate ? (exercise_ - 3U) % 3U : exercise_;
    const bool falling = phase == 2U;
    const float direction = rate && exercise_ >= 6U ? -1.0F : 1.0F;
    const float fraction = phase == 1U ? 1.0F : 0.5F;
    const float range = rate ? tuning_config_.rate_limit : tuning_config_.speed_limit;
    const float target = fraction * range * direction;
    if (!exercise_running_) {
        if (!falling) physical_speed_ = physical_rate_ = 0.0F;
        if (!falling && !stopped()) return StepResult::Busy;
        math::StepValidationConfig config{};
        config.sample_period_s = rate ? 0.02F : 0.1F;
        config.sample_period_tolerance_s = rate ? 0.012F : 0.04F;
        config.initial_output = rate ? feedback.yaw_rate_rad_s : feedback.speed_m_s;
        config.target_output = target;
        config.minimum_samples = rate ? 150U : 30U;
        config.steady_window_samples = rate ? 51U : 11U;
        const float step = std::fabs(config.target_output - config.initial_output);
        const float configured_deadband = rate ? tuning_config_.rate_threshold : tuning_config_.speed_threshold;
        // 验证只使用既有10%阶跃目标精度。运行死区只检查目标是否被
        // 控制器归零，不再把2倍死区/4倍噪声与30%上限拼成互斥准入条件。
        if (!std::isfinite(step) || step <= std::numeric_limits<float>::epsilon() ||
            std::fabs(target) <= configured_deadband) {
            PX4_WARN("[autocal] validation target inside configured deadband: target=%.3f deadband=%.3f",
                static_cast<double>(target), static_cast<double>(configured_deadband));
            return fail_step(Status::FAILURE_GAIN_VALIDATION);
        }
        config.absolute_steady_tolerance = 0.10F * step;
        if (!validator_.reset(config)) {
            return fail_step(Status::FAILURE_GAIN_VALIDATION);
        }
        // 验证包含实际 setpoint slew 与足够稳态时间；空间/截止预算不能满足
        // 声明速度时拒绝本组，不偷偷改小 RO_SPEED_LIM 来制造“验证通过”。
        const float limit = rate ? (falling ? tuning_config_.rate_deceleration : tuning_config_.rate_acceleration)
                                 : (falling ? tuning_config_.deceleration : tuning_config_.acceleration);
        const float rise = std::fabs(target - config.initial_output) / limit;
        const float base_duration = rise + std::max(3.0F, 3.0F * loop_time_[rate ? 1U : 0U]) + 1.0F;
        exercise_duration_ = base_duration;
        // 删除1.5×全幅范围×时长和重复停车距离预算；只检查本步目标是否
        // 在观测范围内及当前位置可执行。运动中持续按真实位移/留距停止。
        if (!std::isfinite(exercise_duration_) || exercise_duration_ <= 0.0F ||
            (!rate && (!std::isfinite(braking_distance(std::fabs(target))) || !fence_result(now).can_stop))) {
            return fail_step(Status::FAILURE_FENCE_SPACE);
        }
        exercise_started_ = now;
        tuning_sample_ = 0U;
        exercise_running_ = true;
    }
    physical_speed_ = rate ? 0.0F : target;
    // 实际位移持续复核；不能只在段首用预计时长证明空间充足。空间不足
    // 按原失败链回滚，不让返程或提前停车样本冒充阶跃验证完成。
    if (!rate && (!std::isfinite(braking_distance(ground_speed())) ||
        distance_to_start() + braking_distance(ground_speed()) + 0.72F * ground_speed() >= leg_distance_)) {
        return fail_step(Status::FAILURE_FENCE_SPACE);
    }
    // 速度阶跃仅验证纵向闭环；航向有独立实验，不在这里再造固定增益外环。
    physical_rate_ = rate ? target : 0.0F;
    if (take_tuning_sample(now, rate) && feedback.timestamp_sample >= exercise_started_) {
        // 验证实际速度响应，不以是否恰好采到两次设定值中间点裁定PID效果。
        if (!validator_.add_sample(feedback.timestamp_sample,
            rate ? feedback.yaw_rate_rad_s : feedback.speed_m_s, feedback.saturated)) {
            return fail_step(Status::FAILURE_GAIN_VALIDATION);
        }
    }
    if (now - exercise_started_ < static_cast<std::uint64_t>(exercise_duration_ * 1000000.0F)) return StepResult::Busy;
    const auto result = validator_.result();
    status_.validation_error = result.steady_state_error;
    // 验证器已有完整稳态样本窗，不另计一秒模式层稳定时间；当前输出须
    // 脱离保护遮蔽；设定值渐变的采样点数不再作为附加通过条件。
    if (!result.valid() ||
        !feedback_unmasked(result.valid()) || feedback.motor_slew_active) {
        return fail_step(Status::FAILURE_GAIN_VALIDATION);
    }
    // 完整幅值通过后保持当前非零命令，下一拍直接进入半幅下降验证。
    // 其余段先请求停车，避免把换向滑行混到另一方向的响应里。
    if (phase != 1U) physical_speed_ = physical_rate_ = 0.0F;
    exercise_running_ = false;
    ++exercise_;
    if ((!rate && exercise_ == 3U) || (rate && exercise_ == 9U)) {
        validation_passed_ = true;
        if (rate) return StepResult::WantStop;   // 原位停波后由调度器推进 cohort
        // 速度组完成：回入场点真实停波，调度器据此开启角速度组 Arm gate。
        session_.resume_substate = PhaseSubstate::WaitStop;
        return StepResult::WantReturn;
    }
    if (!rate && phase == 0U) {
        // 半幅段完成先回入场点；全幅与紧接的下降阶跃保持连续，不能插入
        // 停车而失去 RO_DECEL_LIM 的非零下降验证证据。
        session_.resume_substate = PhaseSubstate::Running;
        return StepResult::WantReturn;
    }
    return StepResult::Busy;
}

StepResult AutoCalibrationMode::validation_heading(std::uint64_t now) noexcept
{
    const auto &feedback = control_feedback_sub_.get();
    if (!exercise_running_) {
        physical_speed_ = physical_rate_ = 0.0F;
        if (!stopped()) return StepResult::Busy;
        exercise_start_yaw_ = body_yaw();
        const float angle = 30.0F * kRadians; // 沿用既有Heading阶跃，与转驱门限独立。
        const float target = exercise_ == 0U ? angle : -angle;
        exercise_heading_ = math::wrap_pi(exercise_start_yaw_ + target);
        math::StepValidationConfig config{};
        config.sample_period_tolerance_s = 0.04F;
        config.steady_window_samples = 11U;
        config.target_output = target;
        // Heading只验证本次阶跃跟踪精度，不修改或借用用户Driving转驱阈值。
        config.absolute_steady_tolerance = 0.10F * angle;
        if (!validator_.reset(config)) {
            return fail_step(Status::FAILURE_GAIN_VALIDATION);
        }
        tuning_heading_.reset();
        exercise_started_ = now;
        tuning_sample_ = 0U;
        exercise_running_ = true;
    }
    // 航向设定限速使用实际dt，不把10ms调度按20ms计算成双倍转向变化率。
    const float dt = control_interval_s(now);
    const auto heading = tuning_heading_.update(exercise_heading_, body_yaw(), dt);
    if (!heading.valid) {
        return fail_step(Status::FAILURE_GAIN_VALIDATION);
    }
    // Heading 也必须消费当前 provisional 代次；旧反馈只能等待下一拍，
    // 明确出现新代次不一致时立即终止，不能把旧参数响应计入新候选。
    // 来源、会话与鲜度已由公共验证入口检查；这里检查本候选的参数代次。
    if (feedback.parameter_update_instance != transaction_.generation()) {
        return fail_step(Status::FAILURE_GAIN_VALIDATION);
    }
    physical_speed_ = 0.0F;
    physical_rate_ = heading.yaw_rate_setpoint_rad_s;
    const float angle = math::wrap_pi(body_yaw() - exercise_start_yaw_);
    const float error = math::wrap_pi(exercise_heading_ - body_yaw());
    if (take_tuning_sample(now, false) && feedback.timestamp_sample >= exercise_started_) {
        // 本拍参数代次已在上方确认；采样失败仍交已有组失败链停车/回滚。
        if (!validator_.add_sample(feedback.timestamp_sample, angle, feedback.saturated)) {
            return fail_step(Status::FAILURE_GAIN_VALIDATION);
        }
    }
    // 以本次Heading验证精度确认到位；Driving门限只属于实际转驱逻辑。
    const bool converged = std::fabs(error) < validator_.result().allowed_steady_state_error &&
        std::fabs(yaw_rate()) < 0.05F && feedback_unmasked();
    const std::uint64_t heading_timeout = 30000000ULL;
    if (now - exercise_started_ > heading_timeout) {
        return fail_step(Status::FAILURE_GAIN_VALIDATION);
    }
    if (!converged || !validator_.result().valid()) return StepResult::Busy;
    status_.validation_error = std::fabs(error);
    physical_rate_ = 0.0F;
    exercise_running_ = false;
    ++exercise_;
    if (exercise_ == 2U) {
        validation_passed_ = true;
        return StepResult::WantStop;   // 原位停波后由调度器推进 cohort
    }
    return StepResult::Busy;
}

} // namespace dima::rover::modes
