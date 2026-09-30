#define MODULE_NAME "auto_cal"
#include "AutoCalibrationMode.hpp"
#include "logging/logging.hpp"

#include <algorithm>
#include <cmath>

namespace dima::rover::modes {
namespace math = dima::lib::rover::calibration;

// —— IDENTIFICATION 阶段 helper：开环辨识实验与公式法 PI ————————————————
// 序列固定：exercise 0/1 = 去/返速度激励（中间掉头经 WantTurn 上抛，返程经
// WantReturn 回入场点停车），exercise 2/3 = CW/CCW 角速度激励（方向切换在
// helper 内部，结束后 WantStop 交调度器停车并计算候选）。阶段文件只返回
// StepResult；Arm gate、掉头、返程、停波与 Gains 事务全部由调度器承担。

bool AutoCalibrationMode::begin_identification() noexcept
{
    const auto &c = tuning_config_;
    const bool limits_valid = std::isfinite(c.speed_limit) && c.speed_limit > 0.0F && c.speed_limit <= fence_.speed_limit_m_s &&
        std::isfinite(c.rate_limit) && c.rate_limit > 0.0F &&
        std::isfinite(c.speed_threshold) && c.speed_threshold >= 0.0F &&
        std::isfinite(c.rate_threshold) && c.rate_threshold >= 0.0F &&
        std::isfinite(c.acceleration) && c.acceleration > 0.0F && std::isfinite(c.deceleration) && c.deceleration > 0.0F &&
        std::isfinite(c.rate_acceleration) && c.rate_acceleration > 0.0F && std::isfinite(c.rate_deceleration) && c.rate_deceleration > 0.0F;
    // 开环按实际输入/响应辨识，不要求跟踪指定速度；死区与目标幅值是否适合
    // 精度考核，由候选 PID 应用后的 validation_inner() 判定。
    if (!limits_valid) {
        return fail_flag(Status::FAILURE_MOTION_UNAVAILABLE);
    }
    // 本入口只校验配置并初始化实验；唯一调用方随后进入WaitArm，在正确的
    // IDENTIFICATION直线语义下检查实时融合、停车和围栏，不沿用PROFILE
    // 末段旋转语义抢先算一次围栏，也不把单拍未就绪直接变成组失败。
    leg_distance_ = config_.straight_distance;
    status_.closed_loop = false;
    status_.gain_group = Status::GAIN_INNER;
    for (auto &result : identification_results_) result = {};
    exercise_ = 0U;
    exercise_running_ = false;
    return true;
}

StepResult AutoCalibrationMode::identification_run(std::uint64_t now) noexcept
{
    const bool rate = exercise_ >= 2U;
    if (rate) longitudinal_ = 0.0F;
    if (exercise_ > 3U) return fail_step(Status::FAILURE_TIMEOUT);
    const auto &feedback = control_feedback_sub_.get();
    // 四段共用启动、采样和拟合；直线每腿90秒、旋转每方向75秒。
    // 子状态重入及CW→CCW切换各自重锚，不能共享掉头前的时间预算。
    if (state_started_ < session_.substate_started) state_started_ = now;
    if (now - state_started_ > (rate ? 75000000ULL : 90000000ULL)) {
        if (rate) steering_ = 0.0F;
        return fail_step(Status::FAILURE_TIMEOUT);
    }
    if (!exercise_running_) {
        longitudinal_ = steering_ = 0.0F;
        if (!stopped() || !tuning_feedback(now) || !feedback_unmasked() ||
            now - arm_started_ < static_cast<std::uint64_t>((tuning_arm_ramp_ + 0.25F) * 1000000.0F)) return StepResult::Busy;
        math::IdentificationConfig configuration{};
        if (rate) {
            configuration.sample_period_s = 0.02F;
            configuration.sample_period_tolerance_s = 0.012F;
            configuration.minimum_samples = 150U;
        } else configuration.sample_period_tolerance_s = 0.04F;
        // 上一段结果已保存；新段才重置共享工作区，并冻结本段输入/响应原点。
        if (!identifier_.configure(configuration)) return fail_step(Status::FAILURE_IDENTIFICATION);
        exercise_running_ = true;
        exercise_started_ = now;
        tuning_sample_ = 0U;
        identification_input_origin_ = rate ? feedback.steering : feedback.longitudinal;
        identification_output_origin_ = rate ? feedback.yaw_rate_rad_s : feedback.speed_raw_m_s;
    }
    const float direction = exercise_ == 3U ? -1.0F : 1.0F;
    const unsigned phase = static_cast<unsigned>((now - exercise_started_) / 4000000ULL) % 4U;
    const float fraction = phase == 1U ? 1.0F : phase == 3U ? 0.75F : 0.5F;
    const float desired = rate
        ? direction * fraction * tuning_config_.rate_limit * config_.track * status_.yaw_rate_correction /
            (2.0F * status_.maximum_speed_m_s)
        : config_.motor_maximum * fraction * tuning_config_.speed_limit / status_.maximum_speed_m_s;
    // 激励仍在公共混控整形前坐标；直线仅纵向、旋转仅转向，不叠加私有修正。
    // 增量按真实dt计算，两类实验保留各自原有斜率及采样周期。
    const float dt = control_interval_s(now);
    const float step = dt * (!rate && tuning_motor_slew_ > 0.0F
        ? std::min(0.05F, 0.5F * tuning_motor_slew_) : 0.05F);
    float &command = rate ? steering_ : longitudinal_;
    command += std::clamp(desired - command, -step, step);
    if (!rate) steering_ = 0.0F;
    if (take_tuning_sample(now, rate) && feedback.timestamp_sample >= exercise_started_) {
        // CCW同时翻转输入和响应坐标，保持模型增益定义；仅采本段启动后的样本。
        const float input = direction * ((rate ? feedback.steering : feedback.longitudinal) - identification_input_origin_);
        const float output = direction * ((rate ? feedback.yaw_rate_rad_s : feedback.speed_raw_m_s) - identification_output_origin_);
        if (!feedback_unmasked() || feedback.motor_slew_active || !std::isfinite(input) ||
            !identifier_.add_sample(feedback.timestamp_sample, input, output)) {
            return fail_step(Status::FAILURE_IDENTIFICATION);
        }
    }
    if (rate) {
        if (now - exercise_started_ < 16000000ULL) return StepResult::Busy;
    } else {
        const float distance = distance_to_start();
        const float speed = ground_speed();
        const auto &gps = gps_sub_.get();
        const float age = fresh(gps.timestamp_sample, now, 300000ULL)
            ? 1.0e-6F * static_cast<float>(now - gps.timestamp_sample) : NAN;
        // 与返程同口径：实际/配置输出等待在braking_distance内，位置与
        // 既有接收机/请求链路预算另计，不再把换向死区固定为0.72*v的一部分。
        const float stop = braking_distance(speed) + fence_.origin_error_m + gps.eph + speed * (0.42F + age);
        if (!std::isfinite(stop)) return abort_step(Status::FAILURE_MOTION_UNAVAILABLE);
        if (exercise_ == 0U ? distance + stop < leg_distance_ : distance > 0.5F + stop) return StepResult::Busy;
    }
    // 每段只拟合并保存一次；四段完成后再统一评估、计算及应用PI候选。
    identification_results_[exercise_] = identifier_.fit(math::PiDesignLimits{});
    longitudinal_ = steering_ = 0.0F;
    exercise_running_ = false;
    tuning_sample_ = 0U;
    ++exercise_;
    if (exercise_ == 1U) {
        // 去程完成，掉头后进入返程辨识；掉头动作本身不采样。
        turn_heading_ = heading_to_start();
        session_.resume_substate = PhaseSubstate::Running;
        return StepResult::WantTurn;
    }
    if (exercise_ == 2U) {
        // 返程辨识完成，回入场点真实停波后进入原地转向辨识。
        session_.resume_substate = PhaseSubstate::WaitStop;
        return StepResult::WantReturn;
    }
    if (exercise_ == 3U) {
        // CW结束先清零，下一段启动仍须经过stopped及实际输出确认。
        state_started_ = now;
        return StepResult::Busy;
    }
    return StepResult::WantStop;
}

bool AutoCalibrationMode::calculate_inner_gains() noexcept
{
    // 一阶模型按 P=τ/[K(λ+delay)] 设计 PI，候选保持原模型设计，输出约束由公共PI限幅/抗饱和处理。
    // 四段结果来自同一工作区的先后快照；此处仍统一判有效、合并双向候选，
    // 不提前提交参数，也不把后续段的数据混到上一段。
    const auto &fits = identification_results_;
    for (const auto &fit : fits) if (!fit.valid()) return false;
    for (unsigned axis = 0U; axis < 2U; ++axis) {
        const unsigned index = axis * 2U;
        // 两方向模型分别通过辨识质量检查后合并候选；坡度/摩擦不对称由
        // 后续真实双向闭环验证体现，不再用模型增益相差20%提前否决。
        gains_[index] = 0.5F * (fits[index].pi.proportional_gain + fits[index + 1U].pi.proportional_gain);
        gains_[index + 1U] = 0.5F * (fits[index].pi.integral_gain + fits[index + 1U].pi.integral_gain);
        loop_time_[axis] = std::max(fits[index].pi.closed_loop_time_s, fits[index + 1U].pi.closed_loop_time_s);
    }
    // 不再用场地/1.5反算一个更小的验证速度。具体阶跃沿用已有运动
    // 范围；执行期间由实时位移、实测停车留距及原围栏负责空间检查。
    return true;
}

} // namespace dima::rover::modes
