#define MODULE_NAME "auto_cal"
#include "AutoCalibrationMode.hpp"
#include "logging/logging.hpp"
#include <uORB/topics/auto_calibration_status_labels.hpp>

#include <algorithm>
#include <cmath>

namespace dima::rover::modes {
namespace math = dima::lib::rover::calibration;

// —— Braking 阶段 helper ————————————————————————————————————————————
// 内联制动观测（PhaseSubstate::Braking）：停车意图交公共执行层。瞬时反向轮端
// 输出是制动观测证据，不是倒退校准——本会话没有前进/反向行驶辨识。观测
// 只返回 StepResult；停车确认与恢复/提交由调度器 WaitStop 处置。

float AutoCalibrationMode::braking_deceleration() const noexcept
{
    // 首轮为有界返程提供临时观测；第二轮验证新制动策略后使用其实际减速度。
    if (status_.braking_model_generation == 0U) return NAN;
    return status_.braking_deceleration_m_s2;
}

float AutoCalibrationMode::braking_distance(float speed) const noexcept
{
    // 所有校准停车规划共用名义距离与输出等待预算。配置MOT_REV_DELAY是
    // 已知等待，后端首次反向时间补充真实请求/执行等待；不能固定假设0.3s。
    // 平均a已含部分等待，额外v*delay属于明确的保守留距，不冒充实测位移。
    const float delay = status_.braking_model_generation != 0U && status_.braking_input_gain == 0.0F
        ? 0.0F : std::max(config_.motor_reversal_delay_s, braking_output_delay_s_);
    return math::stopping_distance(speed, braking_deceleration()) + speed * delay;
}

bool AutoCalibrationMode::braking_model_ready() const noexcept
{
    return (status_.provisional_validated_stages & Status::STAGE_DECELERATION) != 0U &&
        status_.braking_observations == 2U && std::isfinite(status_.braking_deceleration_m_s2) &&
        status_.braking_deceleration_m_s2 > 0.0F;
}

bool AutoCalibrationMode::braking_output_zero(std::uint64_t now) const noexcept
{
    const auto &output = output_sub_.get();
    // 零请求不等于末端输出归零；后端真实应用时间必须晚于本次停车请求。
    return fresh(output.timestamp, now, 100000ULL) && fresh(output.timestamp_output, now, 100000ULL) &&
        output.timestamp_output >= braking_command_at_ && output.backend_ready && !output.parameter_update_pending &&
        (output.state == actuator_output_status_s::STATE_ACTIVE || output.state == actuator_output_status_s::STATE_CONTROL_INHIBITED) &&
        std::isfinite(output.applied_right) && std::isfinite(output.applied_left) &&
        std::fabs(output.applied_right) < 1.0e-4F && std::fabs(output.applied_left) < 1.0e-4F;
}

bool AutoCalibrationMode::braking_stop_confirmed(std::uint64_t now) const noexcept
{
    // 制动停车确认：后端轮端归零 + (速度带静止 或 位置静止滑窗)。
    // GNSS速度解在急停后振铃~1s（±0.35m/s，实车ULG：轮端零、航向稳定），
    // 位置滑窗是独立物理证据；陀螺摆振语义保留在 stopped()。
    return (ground_speed() < kStoppedSpeedMps ||
        (position_quiet_ && std::fabs(yaw_rate()) < 0.05F)) && braking_output_zero(now);
}

bool AutoCalibrationMode::begin_braking_observation(std::uint64_t now) noexcept
{
    // 事务前观测准备（原 BRAKING_PROBE 入口）：初始化/递增轮次并冻结观测
    // 目标。进入 Braking 子状态、停车确认与恢复由调度器完成；false=组失败
    // 已登记（fail_braking_group），调用方必须走原地终止出口。
    const std::uint64_t epoch = velocity_epoch_us();
    // 本会话的轮次已表示是否首轮，不再维护另一份initialized镜像。
    if (status_.braking_run == 0U) {
        status_.braking_deceleration_m_s2 = NAN;
        status_.braking_input_gain = 0.0F;
        braking_output_delay_s_ = 0.0F;
    }
    // 两次观测分别在去程/返程满输出持续大于3秒后启动；第三入口是异常路径。
    if (++status_.braking_run > 2U) { fail_braking_group(now); return false; }
    // 首轮沿用去程90秒总预算的剩余时间，不再另设15秒制动截断，也不因
    // 切入Braking重置原航段截止。第二轮须在清空J/Q之前冻结预测预算。
    // T_eff=J²/Q而不是首轮总时长。预算只能扩展等待，不能延长失鲜制动力、
    // 放宽0.08m/s停速带或跳过500ms停稳确认；围栏与质量故障链仍独立生效。
    braking_budget_s_ = now >= session_.substate_started
        ? std::max(0.0F, 90.0F - 1.0e-6F * static_cast<float>(now - session_.substate_started)) : 0.0F;
    if (status_.braking_run == 2U) {
        const float speed = ground_speed();
        const float gain = status_.braking_input_gain;
        const float response = gain > 0.0F
            ? math::proportional_braking_time(speed, kStoppedSpeedMps, gain,
                braking_reverse_impulse_ * braking_reverse_impulse_ / braking_squared_impulse_)
            : std::max(0.0F, speed - kStoppedSpeedMps) / status_.braking_deceleration_m_s2;
        // 正向slew在制动入口清零；按本轮初始反向请求给出完整建立时间上界。
        // 纯滑停g=0不需要换向或反推建立预算。0.6秒沿用既有速度历元中断预算，
        // 为末次有效速度/后端确认留时；0.5秒是原停稳窗，不另造任意倍率。
        const float rise = gain > 0.0F && config_.motor_slew_rate > 0.0F
            ? std::min(1.0F, gain * speed) / config_.motor_slew_rate : 0.0F;
        const float delay = gain > 0.0F ? std::max(config_.motor_reversal_delay_s, braking_output_delay_s_) : 0.0F;
        if (!std::isfinite(response) || !std::isfinite(rise)) { fail_braking_group(now); return false; }
        braking_budget_s_ = std::max(15.0F, response + rise + delay + 0.6F + 0.5F);
        PX4_INFO_RAW("[autocal] brake budget %.2fs response %.2fs\n",
            static_cast<double>(braking_budget_s_), static_cast<double>(response));
    }
    braking_last_epoch_ = braking_start_epoch_ = braking_stopped_epoch_ = braking_command_at_ = 0U;
    status_.closed_loop = false;
    PX4_INFO_RAW("[autocal] braking observation %u: full-output %.2fm/s; measured response\n",
        static_cast<unsigned>(status_.braking_run), static_cast<double>(ground_speed()));
    request_measured_braking(now, epoch);
    return true;
}

void AutoCalibrationMode::fail_braking_group(std::uint64_t now) noexcept
{
    // 标记无效证据，调用方统一终止；不能恢复直行或沿用无效模型返场。
    longitudinal_ = steering_ = 0.0F;
    status_.unavailable_stages |= Status::STAGE_DECELERATION;
    if (status_.failure_reason == Status::FAILURE_NONE) status_.failure_reason = Status::FAILURE_DECELERATION;
    PX4_WARN("[autocal] braking experiment failed; stopping in place");
}

bool AutoCalibrationMode::complete_braking_observation(std::uint64_t now) noexcept
{
    // 首次前向停车点与最终停稳确认分开；过零回摆只作诊断（min_v），
    // 不再作为否决判据——GNSS速度解在急减速瞬间的振铃不是真实运动。
    const float distance = braking_stopped_distance_;
    const float duration = braking_stopped_epoch_ > braking_start_epoch_
        ? static_cast<float>(braking_stopped_epoch_ - braking_start_epoch_) * 1.0e-6F : NAN;
    // 唯一法则：位置法 a = v0²/(2·d_settled)，d 为制动入口到最终停稳的
    // GNSS定位位移沿冻结方向投影。时间法 Δv/Δt 已退役：速度解振铃使过零
    // 提前、结果系统性虚高 ~1.7×（两日实车 a_time≈2.2 vs 位置法≈1.3，
    // 且 D/T≈v0 的运动学矛盾证明 T 被污染）。位移必须超过噪声地板
    // max(0.05, 2×eph)，过短则位置法无法分辨，观测拒绝。
    const auto &gps = gps_sub_.get();
    float north{}, east{};
    math::displacement(gps.latitude_deg, gps.longitude_deg, braking_lat_, braking_lon_, north, east);
    const float settled_distance = std::max(0.0F,
        north * braking_forward_north_ + east * braking_forward_east_) +
        braking_entry_lag_distance_;
    const float distance_floor = std::max(0.05F, 2.0F * gps.eph);
    float candidate = NAN;
    if (settled_distance > distance_floor)
        candidate = braking_initial_speed_ * braking_initial_speed_ / (2.0F * settled_distance);
    // 时间用时与停止点前距离仅作诊断记录；域校验保持既有数值范围。
    if (!std::isfinite(candidate) || candidate <= 0.0F || candidate > 100.0F) {
        PX4_WARN("[autocal] braking observation rejected: candidate=%.3f initial=%.3f distance=%.3f duration=%.2f",
            static_cast<double>(candidate), static_cast<double>(braking_initial_speed_),
            static_cast<double>(distance), static_cast<double>(duration));
        return false;
    }
    if (status_.braking_run == 1U) {
        const float gain = math::measured_braking_input_gain(braking_reverse_impulse_, braking_squared_impulse_,
            config_.motor_maximum, braking_initial_speed_, braking_stopped_speed_);
        if (!std::isfinite(gain) || gain < 0.0F) return false;
        status_.braking_input_gain = gain;
        PX4_INFO_RAW("[autocal] brake response gain=%.4f min_v=%.3f\n",
            static_cast<double>(gain), static_cast<double>(braking_minimum_speed_));
    }
    // 首轮辨识、第二轮固定候选验证；最终a必须来自实际将用于返程的第二轮
    // 策略，不能把渐进探测和适配制动两种动作混合平均。没有人为减速度折扣。
    status_.braking_initial_speed_m_s = braking_initial_speed_;
    status_.braking_stop_distance_m = distance;
    status_.braking_stop_time_s = duration;
    // 历史最大速度/距离只作观测诊断，不作为后续运行门限；模型参数是减速度。
    status_.braking_distance_envelope_m = std::max(status_.braking_distance_envelope_m, distance);
    status_.braking_speed_envelope_m_s = std::max(status_.braking_speed_envelope_m_s, braking_observed_speed_);
    status_.braking_deceleration_m_s2 = candidate;
    status_.braking_observations = status_.braking_run;
    ++status_.braking_model_generation;
    status_.braking_full_speed_m_s = braking_initial_speed_;
    PX4_INFO_RAW("[autocal] braking stop %u: %.3f->%.3fm/s d=%.3fm t=%.2fs a=%.3fm/s2\n",
        static_cast<unsigned>(status_.braking_observations), static_cast<double>(braking_initial_speed_),
        static_cast<double>(braking_stopped_speed_), static_cast<double>(distance),
        static_cast<double>(duration), static_cast<double>(status_.braking_deceleration_m_s2));
    return true;
}

StepResult AutoCalibrationMode::braking_step(std::uint64_t now) noexcept
{
    // 既有两轮制动观测：首轮辨识响应，第二轮验证。Advance=本次观测完成
    //（轮次登记在 status_）；Failed=组失败，Abort=会话失败。
    // 入口已冻结制动方向和速度历元，停车确认与恢复由调度器处理。
    const auto &rtk = rtk_sub_.get();
    const float speed = ground_speed();
    // AGRICA 速度历元独立去重。
    const std::uint64_t epoch = velocity_epoch_us();
    // AGRICA 配置为 10 Hz；按真实历元去重，沿用 600 ms 中断退出边界。
    if (braking_last_epoch_ != 0U && (epoch < braking_last_epoch_ || epoch - braking_last_epoch_ > 600000ULL)) {
        return abort_step(Status::FAILURE_SENSOR_STALE);
    }
    const bool sample = epoch > braking_last_epoch_;
    const float forward = rtk.velocity_north_m_s * braking_forward_north_ + rtk.velocity_east_m_s * braking_forward_east_;
    // 模式只发停车意图；ACTIVE/SETTLE由已有公共执行层解释并制动。
    // 不在模式层计算反向轴，更不需要知道E坐标、PWM或电机反向配置。
    longitudinal_ = steering_ = 0.0F;
    if (1.0e-6F * static_cast<float>(now - braking_command_at_) > braking_budget_s_) {
        PX4_WARN("[autocal] brake timeout run=%u budget=%.2fs speed=%.3f",
            static_cast<unsigned>(status_.braking_run), static_cast<double>(braking_budget_s_), static_cast<double>(speed));
        fail_braking_group(now);
        return StepResult::Failed;
    }
    // 实际输出积分使用现有后端历史的应用时间，不用请求冒充已执行输入。
    // 每拍消费首次前向停止观测前的新后端区间；后续回摆不进入输入统计。
    const auto output_time = output_sub_.get().timestamp_output;
    if (braking_stopped_epoch_ == 0U && output_time > braking_impulse_time_) {
        float impulse{}, squared{};
        std::uint64_t reverse_at{};
        const bool history_available = output_time <= now &&
            motor_history_.reverse_impulse(braking_impulse_time_, output_time, impulse, squared, reverse_at);
        // 只有首轮用J/Q辨识g，必须有连续输入证据。第二轮冻结g，只验证
        // 速度响应与真实停稳；历史缺帧不再否决该观测，可用历史仍补充输出延迟。
        if (!history_available && status_.braking_run == 1U) {
            PX4_WARN("[autocal] brake output history unavailable");
            fail_braking_group(now);
            return StepResult::Failed;
        }
        if (history_available && status_.braking_run == 1U) {
            braking_reverse_impulse_ += impulse;
            braking_squared_impulse_ += squared;
        }
        if (history_available && braking_reverse_at_ == 0U && reverse_at != 0U) {
            braking_reverse_at_ = reverse_at;
            braking_output_delay_s_ = std::max(braking_output_delay_s_,
                1.0e-6F * static_cast<float>(reverse_at - braking_command_at_));
        }
        braking_impulse_time_ = output_time;
    }
    // 公共质量门的确认窗内也不能把无效/重复速度当成模型证据。
    if (!sample || !rtk_quality(now)) return StepResult::Busy;
    braking_minimum_speed_ = std::min(braking_minimum_speed_, forward);
    // 制动摆动监测：入口冻结航向在制动中持续偏移只作一次性诊断；
    // 观测投影仍严格使用入口冻结方向，不受车体摆动污染。
    const float heading_drift = math::wrap_pi(leg_heading_ - rtk.array_heading_rad);
    if (!braking_drift_reported_ && std::isfinite(heading_drift) &&
        std::fabs(heading_drift) > 15.0F * kRadians) {
        braking_drift_reported_ = true;
        PX4_WARN("[autocal] braking heading drift %.1f deg during stop; inspect alignment",
            static_cast<double>(heading_drift / kRadians));
    }
    const float dt = static_cast<float>(epoch - braking_last_epoch_) * 1.0e-6F;
    braking_observed_speed_ = std::max(braking_observed_speed_, forward);
    if (braking_stopped_epoch_ == 0U) {
        // 跨零两点线性插值：f=v_prev/(v_prev-v_now)，只积分到首次零速，
        // 不把整个采样周期当作正向三角形，也不把倒退距离抵销前向位移。
        const bool crossing = braking_previous_speed_ > 0.0F && forward <= 0.0F;
        const float fraction = crossing ? braking_previous_speed_ / (braking_previous_speed_ - forward) : 1.0F;
        const float end_speed = crossing ? 0.0F : std::max(0.0F, forward);
        braking_integral_distance_ += 0.5F * (std::max(0.0F, braking_previous_speed_) + end_speed) * dt * fraction;
        const auto &gps = gps_sub_.get();
        float north{}, east{};
        math::displacement(gps.latitude_deg, gps.longitude_deg, braking_lat_, braking_lon_, north, east);
        braking_stopped_distance_ = std::max(braking_stopped_distance_,
            std::max(braking_integral_distance_, north * braking_forward_north_ + east * braking_forward_east_));
        if (crossing || speed < kStoppedSpeedMps) {
            braking_stopped_epoch_ = braking_last_epoch_ +
                static_cast<std::uint64_t>(fraction * static_cast<float>(epoch - braking_last_epoch_));
            braking_stopped_speed_ = end_speed;
            status_.braking_phase = Status::BRAKING_SETTLE;
            PX4_INFO_RAW("[autocal] brake endpoint v=%.3f t=%.3fs\n", static_cast<double>(end_speed),
                static_cast<double>(braking_stopped_epoch_ - braking_start_epoch_) * 1.0e-6);
        }
    }
    braking_previous_speed_ = forward;
    braking_last_epoch_ = epoch;
    // 已冻结的前向终点不再因回摆被重写。500ms窗口只确认最终停稳与后端
    // 撤力，不扩大停止阈值，也不把该等待加进T或距离。
    if (braking_stop_confirmed(now)) {
        if (braking_settled_since_ == 0U) braking_settled_since_ = epoch;
        if (epoch - braking_settled_since_ >= 500000ULL) {
            if (complete_braking_observation(now)) return StepResult::Advance;
            fail_braking_group(now);
            return StepResult::Failed;
        }
    } else braking_settled_since_ = 0U;
    return StepResult::Busy;
}

void AutoCalibrationMode::report_braking_probe(std::uint64_t now) const noexcept
{
    const auto &output = output_sub_.get();
    const float age_ms = output.timestamp_output != 0U && output.timestamp_output <= now
        ? static_cast<float>(now - output.timestamp_output) * 1.0e-3F : -1.0F;
    // 同时保留请求与后端已应用的归一化轮端证据；模式不读取或解释脉宽，
    // 后端数值也不是实测轮速或电流。
    PX4_INFO_RAW("[autocal] probe run=%u phase=%s request=%.3f steer=%.3f\n",
        static_cast<unsigned>(status_.braking_run),
        dima::generated::uorb_labels::auto_calibration_status_braking_name(status_.braking_phase),
        static_cast<double>(longitudinal_), static_cast<double>(steering_));
    PX4_INFO_RAW("[autocal] probe output state=%u valid=%u age=%.0fms R=%.3f L=%.3f\n",
        static_cast<unsigned>(output.state), output.command_valid ? 1U : 0U, static_cast<double>(age_ms),
        static_cast<double>(output.applied_right), static_cast<double>(output.applied_left));
    const auto &rtk = rtk_sub_.get();
    const float forward = rtk.velocity_north_m_s * braking_forward_north_ + rtk.velocity_east_m_s * braking_forward_east_;
    PX4_INFO_RAW("[autocal] probe v=%.3f fwd=%.3f\n", static_cast<double>(ground_speed()), static_cast<double>(forward));
}

void AutoCalibrationMode::request_measured_braking(std::uint64_t now, std::uint64_t epoch) noexcept
{
    const auto &rtk = rtk_sub_.get();
    const auto &gps = gps_sub_.get();
    const float speed = ground_speed();
    // 制动入口不设最低车速；满输出但未运动也照常撤力，零速不作方向除法。
    braking_forward_north_ = speed > 0.0F ? rtk.velocity_north_m_s / speed : 0.0F;
    braking_forward_east_ = speed > 0.0F ? rtk.velocity_east_m_s / speed : 0.0F;
    const float heading_error = math::wrap_pi(leg_heading_ - rtk.array_heading_rad);
    if (std::isfinite(heading_error) && std::fabs(heading_error) > 15.0F * kRadians) {
        // 偏航不终止制动观测，但记录冻结方向已明显偏离，提醒操作者检查
        // 机械侧偏、轮端阻力和场地坡度；本次前向投影仍严格使用冻结方向。
        PX4_WARN("[autocal] braking heading drift %.1f deg; inspect vehicle alignment",
            static_cast<double>(heading_error / kRadians));
    }
    const float forward_speed = std::max(0.0F,
        rtk.velocity_north_m_s * braking_forward_north_ +
        rtk.velocity_east_m_s * braking_forward_east_);
    braking_initial_speed_ = forward_speed;
    braking_observed_speed_ = forward_speed;
    braking_previous_speed_ = braking_initial_speed_;
    braking_minimum_speed_ = braking_initial_speed_;
    braking_reverse_impulse_ = braking_squared_impulse_ = 0.0F;
    braking_reverse_at_ = 0U;
    braking_impulse_time_ = now;
    braking_integral_distance_ = 0.0F;
    braking_stopped_speed_ = braking_stopped_distance_ = 0.0F;
    braking_lat_ = gps.latitude_deg; braking_lon_ = gps.longitude_deg;
    // 入口定位滞后补偿：位置 topic 比速度历元晚约一个采样周期（实车
    // 0.78 m/s 下欠计 ~0.05-0.06 m，位置法虚高 ~40%）。按滞后时间把
    // 入口沿冻结方向前推 v0×lag；停稳端的滞后对静止车无影响。
    braking_entry_lag_distance_ = fresh(gps.timestamp_sample, now, 300000ULL) &&
        gps.timestamp_sample <= now
        ? forward_speed * static_cast<float>(now - gps.timestamp_sample) * 1.0e-6F
        : 0.0F;
    braking_start_epoch_ = epoch;
    braking_last_epoch_ = epoch;
    braking_command_at_ = now;
    braking_stopped_epoch_ = 0U;
    braking_settled_since_ = 0U;
    braking_drift_reported_ = false;
    status_.braking_phase = Status::BRAKING_ACTIVE;
    // 与每拍观测一致：只发布ACTIVE停车意图，制动力由公共执行层产生。
    longitudinal_ = 0.0F;
    steering_ = 0.0F;
}

bool AutoCalibrationMode::begin_deceleration_transaction(std::uint64_t now) noexcept
{
    // 事务操作：两次停车观测确认 RAM 候选（RO_DECEL_LIM）；后续 RTK/磁/增益
    // 失败不会撤销已验证的制动成果。未确认 RAM 候选则仍使用探测范围，禁止发布
    // 普通校准运动。事务身份与推进由调度器 bookkeeping/poll 维护。
    return transaction_.prepare() &&
        transaction_.add_float(dima::params::RO_DECEL_LIM, status_.braking_deceleration_m_s2) &&
        apply_transaction(TransactionKind::Dynamics, now, false, Status::STAGE_DECELERATION);
}

} // namespace dima::rover::modes
