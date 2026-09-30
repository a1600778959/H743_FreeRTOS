#include "DifferentialDrive.hpp"

#include <cmath>

namespace dima::lib::rover {
namespace {

constexpr float kZeroThreshold = 1.0e-6F;

} // namespace

bool DifferentialDrive::finite(float value) noexcept
{
    return std::isfinite(value);
}

float DifferentialDrive::clamp(float value, float lower, float upper) noexcept
{
    return value < lower ? lower : (value > upper ? upper : value);
}

float DifferentialDrive::interpolate(float from, float to,
                                     float ratio) noexcept
{
    return from + (to - from) * clamp(ratio, 0.0F, 1.0F);
}

float DifferentialDrive::signed_unit(float value) noexcept
{
    return value < 0.0F ? -1.0F : 1.0F;
}

float DifferentialDrive::expo_curve(float magnitude, float expo) noexcept
{
    const float input = clamp(magnitude, 0.0F, 1.0F);
    if (std::fabs(expo) <= kZeroThreshold) {
        return input;
    }

    const float one_minus_expo = 1.0F - expo;
    const float radicand = one_minus_expo * one_minus_expo +
                           4.0F * expo * input;
    if (radicand < 0.0F) {
        return input;
    }
    const float shaped =
        ((expo - 1.0F) + std::sqrt(radicand)) / (2.0F * expo);
    return clamp(shaped, 0.0F, 1.0F);
}

bool DifferentialDrive::valid_config(
    const DifferentialDriveConfig &config) noexcept
{
    return finite(config.steering_throttle_mix) &&
           config.steering_throttle_mix >= 0.0F &&
           config.steering_throttle_mix <= 1.0F &&
           finite(config.throttle_min) && config.throttle_min >= 0.0F &&
           finite(config.throttle_max) && config.throttle_max > 0.0F &&
           config.throttle_max <= 1.0F &&
           config.throttle_min <= config.throttle_max &&
           finite(config.throttle_slew_rate) &&
           config.throttle_slew_rate >= 0.0F &&
           config.throttle_slew_rate <= 10.0F &&
           finite(config.reversal_delay_s) &&
           config.reversal_delay_s >= 0.0F &&
           config.reversal_delay_s <= 1.0F &&
           finite(config.throttle_expo) &&
           config.throttle_expo >= -1.0F &&
           config.throttle_expo <= 1.0F &&
           finite(config.thrust_asymmetry) &&
           config.thrust_asymmetry >= 1.0F &&
           config.thrust_asymmetry <= 10.0F &&
           finite(config.arm_ramp_s) && config.arm_ramp_s >= 0.0F &&
           config.arm_ramp_s <= 5.0F;
}

void DifferentialDrive::prioritize_axes(float &longitudinal, float &steering,
                                        float priority,
                                        float lower_motor_limit) noexcept
{
    const float lower = clamp(lower_motor_limit, -1.0F, -0.1F);
    longitudinal = clamp(longitudinal, lower, 1.0F);
    steering = clamp(steering, -1.0F, 1.0F);
    // APM 非对称推力域为 [lower,1]，最宽转向截面位于两端中点。
    // 输入纵向低于该截面时，不允许靠额外增加前进量来换取更大转向范围。
    // 缺少这一步会在 ASYM>1、低油门急转时引入未请求的前进分量。
    const float midpoint = 0.5F * (1.0F + lower);
    const float steering_range = longitudinal < midpoint
        ? std::fmax(longitudinal, 0.0F) - lower : 1.0F - midpoint;
    steering = clamp(steering, -steering_range, steering_range);
    const float steering_magnitude = std::fabs(steering);
    const float upper_output = longitudinal + steering_magnitude;
    const float lower_output = longitudinal - steering_magnitude;
    const float saturation = std::fmax(upper_output, lower_output / lower);
    if (saturation <= 1.0F) {
        return;
    }

    const float fair_scale = 1.0F / saturation;
    const float fair_longitudinal = longitudinal * fair_scale;
    const float fair_steering = steering * fair_scale;

    const float throttle_priority_longitudinal = longitudinal;
    const float throttle_priority_range = std::fmax(
        0.0F,
        std::fmin(1.0F - longitudinal, longitudinal - lower));
    const float throttle_priority_steering = clamp(
        steering, -throttle_priority_range, throttle_priority_range);

    const float maximum_steering = 0.5F * (1.0F - lower);
    const float steering_priority_steering = clamp(
        steering, -maximum_steering, maximum_steering);
    const float steering_priority_magnitude =
        std::fabs(steering_priority_steering);
    const float steering_priority_longitudinal = clamp(
        longitudinal, lower + steering_priority_magnitude,
        1.0F - steering_priority_magnitude);

    const float bounded_priority = clamp(priority, 0.0F, 1.0F);
    if (bounded_priority <= 0.5F) {
        const float blend = bounded_priority * 2.0F;
        longitudinal = interpolate(throttle_priority_longitudinal,
                                   fair_longitudinal, blend);
        steering = interpolate(throttle_priority_steering, fair_steering,
                               blend);
    } else {
        const float blend = (bounded_priority - 0.5F) * 2.0F;
        longitudinal = interpolate(fair_longitudinal,
                                   steering_priority_longitudinal, blend);
        steering = interpolate(fair_steering, steering_priority_steering,
                               blend);
    }
}

bool DifferentialDrive::configure(
    const DifferentialDriveConfig &config) noexcept
{
    if (!valid_config(config)) {
        configured_ = false;
        reset();
        return false;
    }
    config_ = config;
    configured_ = true;
    reset();
    return true;
}

float DifferentialDrive::shape_motor(float command) const noexcept
{
    float bounded = clamp(command, -1.0F, 1.0F);
    if (std::fabs(bounded) <= kZeroThreshold) {
        return 0.0F;
    }

    if (bounded < 0.0F) {
        bounded = clamp(bounded * config_.thrust_asymmetry, -1.0F, 0.0F);
    }

    // APM 顺序为：负向补偿 -> 非零 MIN -> EXPO -> PWM。零命令在上方
    // 直接退出，MIN 不能令零输入自行起转。E 是本项目必须保留的轮端包络：
    // q=MIN/E+(1-MIN/E)*|u|，output=sign(u)*E*expo(q)。E=1 时与 APM
    // 百分比换算后的公式一致；E<1 时仍保证顶端为 E，不扩大校准安全包络。
    // 校准只跳过 Manual 入口的正向地板；此处仍对所有来源应用一次 MIN，
    // 避免重复抬高小请求，也保证校准所得映射适用于同一电机整形链。
    const float minimum_ratio = config_.throttle_min / config_.throttle_max;
    const float compensated = minimum_ratio + (1.0F - minimum_ratio) * std::fabs(bounded);
    const float magnitude = config_.throttle_max * expo_curve(compensated, config_.throttle_expo);
    return signed_unit(bounded) * clamp(magnitude, 0.0F, config_.throttle_max);
}

float DifferentialDrive::apply_reversal_delay(
    float command, ReversalState &state, std::uint64_t now_us) const noexcept
{
    if (std::fabs(command) <= kZeroThreshold) {
        return 0.0F;
    }
    if (now_us < state.last_output_time_us) {
        state = {};
        return 0.0F;
    }

    const std::uint64_t delay_us = static_cast<std::uint64_t>(
        config_.reversal_delay_s * 1000000.0F);
    const bool changes_direction = state.last_nonzero * command < 0.0F;
    if (changes_direction && state.have_output &&
        now_us - state.last_output_time_us < delay_us) {
        return 0.0F;
    }

    state.last_nonzero = command;
    state.last_output_time_us = now_us;
    state.have_output = true;
    return command;
}

DifferentialDriveOutput DifferentialDrive::update(
    float longitudinal, float steering, bool manual_source, bool armed,
    std::uint64_t now_us, float dt_s) noexcept
{
    return update(longitudinal, steering, manual_source, armed, now_us, dt_s,
                  true);
}

DifferentialDriveOutput DifferentialDrive::update(
    float longitudinal, float steering, bool manual_source, bool armed,
    std::uint64_t now_us, float dt_s, bool manual_throttle_floor, Purpose purpose) noexcept
{
    const bool braking = purpose == Purpose::Brake;
    if (!configured_ || !armed || !finite(longitudinal) ||
        !finite(steering) || !finite(dt_s) || dt_s <= 0.0F || dt_s > 0.05F ||
        (braking && (longitudinal > 0.0F || steering != 0.0F))) {
        reset();
        return {};
    }

    if (!armed_) {
        armed_ = true;
        armed_since_us_ = now_us;
        limited_longitudinal_ = 0.0F;
        right_reversal_ = {};
        left_reversal_ = {};
    }
    if (now_us < armed_since_us_) {
        reset();
        return {};
    }
    float target = clamp(longitudinal, -1.0F, 1.0F);
    float requested_steering = clamp(steering, -1.0F, 1.0F);
    bool manual_input_limited = false;
    if (manual_source) {
        // APM Manual 在电机混控前按 |T|+|S| 同比缩放，保持操作者两轴比例。
        // 不限制内轮方向；S>T 的前进急转仍允许内侧电机倒转。
        const float scale = std::fmax(1.0F, std::fabs(target) + std::fabs(requested_steering));
        manual_input_limited = scale > 1.0F;
        target /= scale;
        requested_steering /= scale;
        // 前进命令直接从 MOT_THR_MIN 起步：越过死区的正杆量输出不低于破摩阻
        // 下限；该额外输入地板仅由 Manual 启用。校准的连续 ramp 不在此
        // 再夹到 MIN，仍由 shape_motor() 施加一次共有的静摩擦补偿。
        if (manual_throttle_floor && target > 0.0F) target = std::fmax(target, config_.throttle_min);
    }
    if ((braking || !manual_source) && std::fabs(target) <= kZeroThreshold) {
        // Navigation/Calibration 零纵向是停车或原地转向的安全边界，立即
        // 清除旧 slew；Manual 按 APM 保留配置的 slew，设为 0 时直接跟随。
        limited_longitudinal_ = 0.0F;
    } else if (config_.throttle_slew_rate > 0.0F) {
        const float maximum_change = config_.throttle_slew_rate * dt_s;
        limited_longitudinal_ += clamp(target - limited_longitudinal_,
                                       -maximum_change, maximum_change);
    } else {
        limited_longitudinal_ = target;
    }
    // Brake 只对制动力增加保留 slew；减弱/撤力立即生效。历史负输出不能
    // 超过当前制动请求，历史正输出也不能延迟进入零起点的反向斜坡。
    if (braking) limited_longitudinal_ = clamp(limited_longitudinal_, target, 0.0F);

    const bool motor_slew_active = std::fabs(limited_longitudinal_ - target) > kZeroThreshold;
    const float before_projection = limited_longitudinal_;
    // 转向符号只表示车头旋转方向：正值始终顺时针，不随油门或 slew 历史
    // 跨过零点而翻转。轮端实际换向仍由下方逐轮等待与输出包络保护。
    float adjusted_steering = requested_steering;

    if (!manual_source) {
        // Speed PI 在进入本层前已按 1-|steering| 限制输出；MOT_SLEW_RATE 的历史
        // 状态也必须重新投影到本周期可行域，否则减速转弯时会再次饱和并挤占转向。
        const float longitudinal_limit =
            std::fmax(0.0F, 1.0F - std::fabs(adjusted_steering));
        limited_longitudinal_ = clamp(limited_longitudinal_,
                                      -longitudinal_limit,
                                      longitudinal_limit);
    }

    float mixed_longitudinal = limited_longitudinal_;
    const float before_mix_steering = adjusted_steering;
    const float lower_motor_limit = braking ? -1.0F : -1.0F / config_.thrust_asymmetry;
    // Manual 继续使用 RD_STR_THR_MIX；Navigation 固定 steering priority=1，
    // 因为 Heading/YawRate 闭环的抗扰稳定性不能被人工油门优先参数削弱。
    const float axis_priority = manual_source
                                    ? config_.steering_throttle_mix
                                    : 1.0F;
    prioritize_axes(mixed_longitudinal, adjusted_steering,
                     axis_priority, lower_motor_limit);

    // 车体 FRD/NED：纵向正值为前进，转向正值为顺时针（车头右转）。
    // 因此右轮 = longitudinal - steering，左轮 = longitudinal + steering；
    // 电机安装方向只在 PWM_Sx_REV 处理，左右侧映射不能代替单侧方向校准。
    // 主动制动 q=E*u，不叠加驱动 MIN/EXPO/ASYM。零附近连续撤力，避免
    // u=-0.30 被 MIN=0.20 抬成 -0.44；旧正向 slew 不能变成制动期间的前推。
    float right = braking ? config_.throttle_max * clamp(mixed_longitudinal, -1.0F, 0.0F)
                          : shape_motor(mixed_longitudinal - adjusted_steering);
    float left = braking ? right : shape_motor(mixed_longitudinal + adjusted_steering);
    // 各原因单独观测：正常曲线不是饱和，不能用一个 input_limited 把待辨识
    // 的 MIN/EXPO/ASYM 响应全部丢掉，也不能把安全 slew 冒充电机能力。
    const bool mixing_limited = manual_input_limited || std::fabs(mixed_longitudinal - before_projection) > kZeroThreshold ||
        std::fabs(adjusted_steering - before_mix_steering) > kZeroThreshold;
    const bool shaping_active = std::fabs(right - (mixed_longitudinal - adjusted_steering)) > kZeroThreshold ||
        std::fabs(left - (mixed_longitudinal + adjusted_steering)) > kZeroThreshold;

    float arm_scale = 1.0F;
    // Arm ramp限制驱动建立，不应削弱已获授权的停车制动力；Brake仍受
    // Armed许可、E包络、换向等待与末端安全保护约束。
    if (!braking && config_.arm_ramp_s > 0.0F) {
        const float elapsed_s = static_cast<float>(now_us - armed_since_us_) *
                                1.0e-6F;
        arm_scale = clamp(elapsed_s / config_.arm_ramp_s, 0.0F, 1.0F);
    }
    right *= arm_scale;
    left *= arm_scale;

    const float before_delay_right = right, before_delay_left = left;
    // 按 APM 左右独立执行换向等待，不因某一路等待而强制另一侧一起中立。
    // delay=0 时不产生软件换向等待；零命令不更新最后一次非零输出时间。
    right = apply_reversal_delay(right, right_reversal_, now_us);
    left = apply_reversal_delay(left, left_reversal_, now_us);
    return DifferentialDriveOutput{right, left, true, motor_slew_active, mixing_limited,
        shaping_active, arm_scale < 1.0F && (std::fabs(before_delay_right) > kZeroThreshold ||
        std::fabs(before_delay_left) > kZeroThreshold),
        right != before_delay_right || left != before_delay_left};
}

void DifferentialDrive::reset() noexcept
{
    right_reversal_ = {};
    left_reversal_ = {};
    limited_longitudinal_ = 0.0F;
    armed_since_us_ = 0U;
    armed_ = false;
}

} // namespace dima::lib::rover
