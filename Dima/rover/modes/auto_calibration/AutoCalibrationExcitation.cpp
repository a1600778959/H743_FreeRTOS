#define MODULE_NAME "auto_cal"
#include "AutoCalibrationMode.hpp"
#include "logging/logging.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace dima::rover::modes {
namespace math = dima::lib::rover::calibration;

// —— PROFILE 阶段 helper：稳态响应激励（前进平台、CW/CCW 平台、六档+下降档）——
// 三个运动面共用同一序列：起步探测锁下界 → 六档保持输入 + 一档非零下降 →
// 真实停波确认。换档由请求到位/输入保持/有效样本驱动，不要求固定加速度；
// 序列推进（++profile_motion_）与 Runtime 事务由调度器的 WaitStop/Evaluate
// 子状态承担，本文件只返回 StepResult，不做任何状态跳转。

void AutoCalibrationMode::begin_profile_window(std::uint64_t now, bool rate) noexcept
{
    const auto &feedback = control_feedback_sub_.get();
    const float direction = profile_motion_ == 2U ? -1.0F : 1.0F;
    // 六档覆盖“已证实能动的下界→当前允许上界”，另加一档非零下降响应。
    // 弱动力起步之前不消耗平台档位，因此不会只剩两个可用的高档样本。
    const float fraction = profile_level_ < 6U ? static_cast<float>(profile_level_) / 5.0F : 0.5F;
    status_.excitation_target = profile_input_floor_ + fraction * (profile_input_ceiling_ - profile_input_floor_);
    status_.excitation_phase = Status::EXCITATION_RAMP;
    status_.excitation_level = profile_level_;
    status_.excitation_samples = 0U;
    profile_stable_since_ = 0U;
    response_sample_count_ = 0U;
    response_sample_interval_ = rate ? 20000ULL : 100000ULL;
    response_initial_ = direction * (rate ? feedback.yaw_rate_rad_s : feedback.speed_raw_m_s);
    reset_response_tail();
    const float current = std::fabs(rate ? steering_ : longitudinal_);
    const float delta = std::fabs(status_.excitation_target - current);
    float ramp_seconds = delta / (rate ? 0.04F : 0.05F);
    if (!rate && tuning_motor_slew_ > 0.0F)
        ramp_seconds = std::max(ramp_seconds, delta / (config_.motor_maximum * tuning_motor_slew_));
    // 预算只覆盖本段实际请求爬升与MOT_SLEW_RATE；不存在额外0.15/s末端缓升。
    // 沿用八秒观察预算，子窗口仍受本次运动截止约束。
    const float budget = std::min(90.0F, ramp_seconds + 8.0F);
    profile_phase_deadline_ = std::min(profile_motion_deadline_, now + static_cast<std::uint64_t>(budget * 1000000.0F));
}

void AutoCalibrationMode::record_response_sample(std::uint64_t timestamp, float value, bool usable) noexcept
{
    if (response_sample_count_ != 0U &&
        (timestamp <= response_samples_[response_sample_count_ - 1U].timestamp ||
         timestamp - response_samples_[response_sample_count_ - 1U].timestamp < response_sample_interval_)) return;
    constexpr std::size_t capacity = sizeof(response_samples_) / sizeof(response_samples_[0]);
    if (response_sample_count_ == capacity) {
        // 慢平台的过渡可能长于 256 个采样点。原位二倍抽稀并保留真实时间戳，
        // 不截掉起始段、不扩大 RAM，也不把抽稀后的数据冒充固定 20 ms。
        for (std::size_t i = 0U; i < capacity / 2U; ++i) response_samples_[i] = response_samples_[2U * i];
        response_sample_count_ = capacity / 2U;
        response_sample_interval_ *= 2U;
    }
    response_samples_[response_sample_count_++] = {timestamp, value, usable};
}

void AutoCalibrationMode::reset_response_tail() noexcept
{
    response_tail_.reset(); response_tail_timestamp_ = 0U;
    status_.excitation_samples = 0U;
    if (profile_level_ >= math::MotorResponseProfile::kLevels) return;
    const bool rate = profile_motion_ == 1U || profile_motion_ == 2U;
    auto &profile = rate ? response_rate_ : response_speed_;
    (void)profile.reset_plateau(profile_motion_ == 2U ? 1U : 0U, profile_level_);
}

void AutoCalibrationMode::finish_response_window(bool rate) noexcept
{
    // 唯一调用点已取得至少 10 个样本和 1.2 s 平台，不再重复设结束门。
    const float final = static_cast<float>(response_tail_.mean());
    const float change = final - response_initial_;
    // 用完整平台终值回看 20%..80% 区间；不以预估目标或含稳态平段的平均
    // 斜率冒充真实过渡。下降段仍为非零输入，不用最后停车替代减速辨识。
    // 过渡可观测性只看变化量，绝对平台信噪比由最终 FF 拟合判断。
    if (profile_level_ != 0U && std::fabs(change) > std::numeric_limits<float>::epsilon()) {
        math::TransientSlope fit;
        for (std::size_t i = 0U; i < response_sample_count_; ++i) {
            const auto &sample = response_samples_[i];
            const float progress = (sample.value - response_initial_) / change;
            if (sample.usable && progress >= 0.2F && progress <= 0.8F)
                (void)fit.add(sample.timestamp, sample.value);
        }
        const auto estimate = fit.measured_rate();
        if (estimate.valid() && fit.signed_rate() * change > 0.0F) {
            const unsigned index = (rate ? 2U : 0U) + (change < 0.0F ? 1U : 0U);
            // 同类有效窗口按真实观测时长加权，不把最慢窗口或置信下界当能力。
            const float duration = fit.duration_s();
            response_rate_sum_[index] += estimate.value * duration;
            response_rate_duration_[index] += duration;
        }
    }
    response_initial_ = final;
}

StepResult AutoCalibrationMode::profile_run(std::uint64_t now) noexcept
{
    const bool rate = profile_motion_ != 0U;
    const float direction = profile_motion_ == 2U ? -1.0F : 1.0F;
    const float maximum = rate ? 1.0F : config_.motor_maximum;
    const auto &f = control_feedback_sub_.get();
    if (!tuning_feedback(now)) {
        longitudinal_ = steering_ = 0.0F;
        if (now - arm_started_ > 250000ULL) {
            return abort_step(Status::FAILURE_SENSOR_STALE);   // 会话级：闭环反馈失鲜由终止链统一处置
        }
        return StepResult::Busy;
    }
    if (!profile_started_) {
        profile_started_ = true;
        profile_motion_deadline_ = now + 90000000ULL;
        profile_stable_since_ = drive_envelope_since_ = 0U;
        response_stop_started_ = tuning_sample_ = 0U;
        profile_level_ = 0U;
        status_.excitation_phase = Status::EXCITATION_PROBE;
        status_.excitation_level = 0U;
        status_.excitation_target = maximum;
        status_.excitation_samples = 0U;
        PX4_INFO("[autocal] profile startup probe; frozen motor envelope %.3f", static_cast<double>(config_.motor_maximum));
    }
    status_.excitation_remaining_s = now < profile_motion_deadline_
        ? static_cast<float>(profile_motion_deadline_ - now) * 1.0e-6F : 0.0F;
    const auto fence = fence_result(now);
    const float stop = braking_distance(ground_speed());
    if (now >= profile_motion_deadline_ || (!rate && !std::isfinite(stop)) || (!rate && distance_to_start() + stop + 0.72F * ground_speed() >= leg_distance_) ||
        (rate && fence.working_radius_m - fence.distance_m < std::max(0.5F, ground_speed())))
        profile_braking_ = true;
    if (profile_braking_) {
        status_.excitation_phase = Status::EXCITATION_BRAKE;
        status_.excitation_target = 0.0F;
        longitudinal_ = steering_ = 0.0F;
        if (response_stop_started_ == 0U) response_stop_started_ = now;
        // 先排空真实发布轮端的末端 slew，再交调度器确认停波；车体不动不是
        // 零输出证明。前进面先返场再统一停车推进序列；CW/CCW 面原位停车。
        if (stopped() && std::fabs(f.applied_longitudinal) < 1.0e-4F && std::fabs(f.applied_steering) < 1.0e-4F) {
            if (rate) return StepResult::WantStop;
            session_.resume_substate = PhaseSubstate::WaitStop;
            return StepResult::WantReturn;
        }
        if (now - response_stop_started_ > 15000000ULL) {
            return abort_step(Status::FAILURE_TIMEOUT);
        }
        return StepResult::Busy;
    }
    const float dt = last_run_ != 0U && now >= last_run_ ? std::min(0.1F, static_cast<float>(now - last_run_) * 1.0e-6F) : 0.02F;
    const float step = (rate ? 0.04F : 0.05F) * dt;
    const float measured = direction * (rate ? f.yaw_rate_rad_s : f.speed_raw_m_s);
    // 平路直行/原地转向只采公共执行链未遮蔽的响应，不加固定加速度、
    // 偏航速度或横向速度比例门；模型候选仍须经过后续闭环验证。
    const bool usable = feedback_unmasked();
    if (status_.excitation_phase == Status::EXCITATION_PROBE) {
        // Arm ramp 尚未结束时保持零请求，避免缓升掩盖起步阈值后继续叠加大输入。
        if (f.arm_ramp_active || now - arm_started_ < static_cast<std::uint64_t>((tuning_arm_ramp_ + 0.25F) * 1000000.0F)) {
            longitudinal_ = steering_ = 0.0F;
            return StepResult::Busy;
        }
        const bool moving = measured > (rate ? 0.03F : kStoppedSpeedMps);
        const float current = std::fabs(rate ? steering_ : longitudinal_);
        if (!moving && current >= 0.995F * maximum) {
            if (drive_envelope_since_ == 0U) drive_envelope_since_ = now;
            if (now - drive_envelope_since_ >= 8000000ULL) {
                PX4_WARN("[autocal] profile drive envelope exhausted; check MOT_THR_MIN/mechanics/battery");
                return fail_step(Status::FAILURE_DRIVE_ENVELOPE);
            }
        } else drive_envelope_since_ = 0U;
        if (moving) {
            // 起步探测只确认输入已生效且持续观测到运动，不要求已经停止加速。
            // 后续在输入保持窗口采集响应；确认期间保持当前输入。
            if (feedback_unmasked() && !f.motor_slew_active) {
                if (profile_stable_since_ == 0U) profile_stable_since_ = now;
                if (now - profile_stable_since_ >= 500000ULL) {
                    profile_input_floor_ = current;
                    // 起步点含MIN/EXPO，v/u不能外推整段输入范围。逐档探索已冻结
                    // 输出包络，由实际空间、输入保持及执行层巡航调节决定哪些窗口有效。
                    profile_input_ceiling_ = maximum;
                    if (!std::isfinite(profile_input_ceiling_) || profile_input_ceiling_ <= current) {
                        PX4_WARN("[autocal] profile has insufficient observable span above startup");
                        profile_braking_ = true;
                    } else begin_profile_window(now, rate);
                }
            } else profile_stable_since_ = 0U;
        } else {
            profile_stable_since_ = 0U;
            if (rate) steering_ = direction * std::min(maximum, current + step);
            else longitudinal_ = std::min(maximum, current + step);
        }
    } else {
        const float target = direction * status_.excitation_target;
        if (rate) steering_ += std::clamp(target - steering_, -step, step);
        else longitudinal_ += std::clamp(target - longitudinal_, -step, step);
        const float request = rate ? steering_ : longitudinal_ / config_.motor_maximum;
        const float expected = rate ? target : target / config_.motor_maximum;
        const bool at_target = std::fabs(request - expected) <= 1.0e-3F &&
            std::fabs((rate ? f.steering : f.longitudinal) - expected) <= 1.0e-3F;
        // 此处只证明输入到位并保持，沿用观察窗统计实际响应；不能把输入
        // 保持时间当作车速已稳态，也不因加速度越过固定0.10而清空整档。
        const bool input_held = at_target && usable && !f.motor_slew_active;
        if (now >= profile_phase_deadline_) {
            // 本档无法在有界时间内取得平台，保留此前合格档并停车；后续 FF
            // 至少三档的原门禁决定是否可继续，不用无效样本填充覆盖度。
            PX4_WARN("[autocal] profile window incomplete; retaining earlier plateaus");
            profile_braking_ = true;
            return StepResult::Busy;
        }
        if (!input_held) {
            profile_stable_since_ = 0U;
            if (status_.excitation_phase == Status::EXCITATION_COLLECT) reset_response_tail();
            status_.excitation_phase = at_target ? Status::EXCITATION_SETTLE : Status::EXCITATION_RAMP;
        } else if (status_.excitation_phase != Status::EXCITATION_COLLECT) {
            status_.excitation_phase = Status::EXCITATION_SETTLE;
            if (profile_stable_since_ == 0U) profile_stable_since_ = now;
            if (now - profile_stable_since_ >= 1000000ULL) status_.excitation_phase = Status::EXCITATION_COLLECT;
        }
        if (take_tuning_sample(now, rate)) {
            record_response_sample(f.timestamp_sample, measured, usable);
            if (response_tail_timestamp_ != 0U && f.timestamp_sample - response_tail_timestamp_ >
                (rate ? 40000ULL : 150000ULL)) {
                reset_response_tail();
                profile_stable_since_ = 0U;
                status_.excitation_phase = Status::EXCITATION_SETTLE;
            }
            if (status_.excitation_phase == Status::EXCITATION_COLLECT) {
                (void)response_tail_.add(f.timestamp_sample, measured);
                response_tail_timestamp_ = f.timestamp_sample;
                status_.excitation_samples = response_tail_.count();
                if (profile_level_ < 6U) {
                    auto &profile = rate ? response_rate_ : response_speed_;
                    if (!profile.add(direction > 0.0F ? 0U : 1U, profile_level_, f.timestamp_sample,
                        rate ? f.steering : f.longitudinal, rate ? f.applied_steering : f.applied_longitudinal,
                        rate ? f.yaw_rate_rad_s : f.speed_raw_m_s)) {
                        return fail_step(Status::FAILURE_PROFILE_UNOBSERVABLE);
                    }
                }
                if (!rate) {
                    status_.observed_forward_speed_m_s = std::max(status_.observed_forward_speed_m_s, measured);
                    float actual{};
                    if (motor_history_.forward_output(f.timestamp_sample, actual)) {
                        status_.observed_output_max = std::max(status_.observed_output_max, actual);
                        if (profile_level_ == 5U && !endpoint_reported_ && actual >= 0.995F * config_.motor_maximum) {
                            endpoint_reported_ = true;
                            PX4_INFO("[autocal] command endpoint reached: %.3f of envelope %.3f",
                                static_cast<double>(actual), static_cast<double>(config_.motor_maximum));
                        }
                    }
                }
            }
        }
        if (response_tail_.count() >= 10U && response_tail_.duration_s() >= 1.2F) {
            finish_response_window(rate);
            if (++profile_level_ >= 7U) profile_braking_ = true;
            else begin_profile_window(now, rate);
        }
    }
    if (rate) longitudinal_ = 0.0F;
    // 直线平台仅施加纵向输入。删除私有航向修正及为其追加的顶档死区；
    // 真实空间由既有围栏检查，样本有效性仍由轴向响应和模型检查负责。
    else steering_ = 0.0F;
    return StepResult::Busy;
}

} // namespace dima::rover::modes
