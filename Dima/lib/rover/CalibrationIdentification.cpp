#include "CalibrationIdentification.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace dima::lib::rover::calibration {
namespace {

constexpr float kMicrosecondsToSeconds = 1.0e-6F;
constexpr float kMaximumOvershootRatio = 0.20F;
constexpr float kSteadyErrorRatio = 0.10F;
constexpr float kMinimumDivisor = 1.0e-9F;

bool finite(float value) noexcept
{
    return std::isfinite(value);
}

bool identification_config_valid(const IdentificationConfig &config) noexcept
{
    return finite(config.sample_period_s) && config.sample_period_s > 0.0F &&
        config.sample_period_s <= 1.0F && finite(config.sample_period_tolerance_s) &&
        config.sample_period_tolerance_s >= 0.0F &&
        config.sample_period_tolerance_s < config.sample_period_s &&
        finite(config.forgetting_time_constant_s) &&
        config.forgetting_time_constant_s > config.sample_period_s &&
        config.minimum_samples >= 16U && finite(config.maximum_absolute_input) &&
        config.maximum_absolute_input > 0.0F;
}

bool pi_limits_valid(const PiDesignLimits &limits) noexcept
{
    return finite(limits.requested_closed_loop_time_s) &&
        limits.requested_closed_loop_time_s > 0.0F &&
        finite(limits.maximum_proportional_gain) &&
        limits.maximum_proportional_gain > 0.0F && finite(limits.maximum_integral_gain) &&
        limits.maximum_integral_gain > 0.0F;
}

bool sample_time_valid(std::uint64_t timestamp_us, std::uint64_t last_timestamp_us,
                       float expected_period_s, float tolerance_s,
                       CalibrationAlgorithmFailure &failure) noexcept
{
    if (timestamp_us == 0U) {
        failure = CalibrationAlgorithmFailure::InvalidSample;
        return false;
    }
    if (last_timestamp_us == 0U) return true;
    if (timestamp_us == last_timestamp_us) {
        failure = CalibrationAlgorithmFailure::DuplicateTimestamp;
        return false;
    }
    if (timestamp_us < last_timestamp_us) {
        failure = CalibrationAlgorithmFailure::TimeRegression;
        return false;
    }
    const float period_s = static_cast<float>(timestamp_us - last_timestamp_us) *
        kMicrosecondsToSeconds;
    if (!finite(period_s) || std::fabs(period_s - expected_period_s) > tolerance_s) {
        failure = CalibrationAlgorithmFailure::SamplePeriodMismatch;
        return false;
    }
    return true;
}

template<std::size_t Delay>
void reset_bank(ArxRls<1U, 0U, Delay> &bank,
                const IdentificationConfig &config) noexcept
{
    bank.reset();
    bank.setForgettingFactor(config.forgetting_time_constant_s,
                             config.sample_period_s);
}

template<std::size_t Delay>
bool update_bank(ArxRls<1U, 0U, Delay> &bank, std::size_t index,
                 float input, float output, std::uint32_t sample_count,
                 const IdentificationConfig &config,
                 double (&residual_squared)[FirstOrderDelayIdentifier::kDelayBankSize],
                 std::uint32_t (&residual_count)[FirstOrderDelayIdentifier::kDelayBankSize]) noexcept
{
    bank.update(input, output);
    const auto &coefficients = bank.getCoefficients();
    const auto variances = bank.getVariances();
    if (!finite(coefficients(0)) || !finite(coefficients(1)) ||
        !finite(variances(0)) || !finite(variances(1)) ||
        !finite(bank.getInnovation())) return false;

    // RLS 初始大协方差会产生很大的暂态 innovation；残差统计只累计后半段，
    // 但系数与协方差仍从第一份唯一样本开始更新。
    const std::uint32_t warmup = std::max<std::uint32_t>(
        static_cast<std::uint32_t>(Delay + 3U), config.minimum_samples / 2U);
    if (sample_count > warmup) {
        const double innovation = bank.getInnovation();
        residual_squared[index] += innovation * innovation;
        ++residual_count[index];
    }
    return true;
}

bool evaluate_model(const matrix::Vector<float, 2U> &coefficients,
                    const matrix::Vector<float, 2U> &variances,
                    std::size_t delay, std::size_t index,
                    const IdentificationConfig &config, float output_variance,
                    const double (&residual_squared)[FirstOrderDelayIdentifier::kDelayBankSize],
                    const std::uint32_t (&residual_count)[FirstOrderDelayIdentifier::kDelayBankSize],
                    FirstOrderModel &model) noexcept
{
    // 六个延迟模型共用同一评估实体；调用方已检查残差样本数并按原顺序
    // 取得系数/方差。仅把 Delay 作为数据传入，不改公式、精度或短路门禁。
    const float a = coefficients(0);
    const float b = coefficients(1);
    const float pole = -a;
    // 一阶稳定实极点只要求0<pole<1；不再通过固定0.995给不同采样周期
    // 偷加不同响应时间上限。参数可保存范围在PI候选生成后独立检查。
    if (!finite(a) || !finite(b) || !finite(variances(0)) || !finite(variances(1)) ||
        variances(0) < 0.0F || variances(1) < 0.0F || pole <= 0.0F || pole >= 1.0F) return false;

    const float denominator = 1.0F + a;
    if (std::fabs(denominator) <= kMinimumDivisor) return false;
    const float dc_gain = b / denominator;
    const float time_constant_s = -config.sample_period_s / std::log(pole);
    if (!finite(dc_gain) || dc_gain <= 0.0F ||
        !finite(time_constant_s) || time_constant_s <= 0.0F) return false;

    // 残差只用于六个延迟模型的择优；删除无消费者的测量噪声缩放协方差，
    // RLS自身信息矩阵的有限/非负检查仍在上方保留。
    const float rms_residual = static_cast<float>(std::sqrt(
        residual_squared[index] / static_cast<double>(residual_count[index])));
    // fit已确认响应方差为正；以本批实测尺度归一化，不设置固定响应幅值地板。
    const float normalized_residual = rms_residual / std::sqrt(output_variance);
    // 有限残差用于模型排序，不用固定百分比提前否决候选。
    if (!finite(normalized_residual)) return false;

    model.valid = true;
    model.dc_gain = dc_gain;
    model.time_constant_s = time_constant_s;
    model.delay_s = static_cast<float>(delay) * config.sample_period_s;
    model.normalized_rms_residual = normalized_residual;
    return true;
}

template<std::size_t Delay>
bool evaluate_bank(const ArxRls<1U, 0U, Delay> &bank, std::size_t index,
                   const IdentificationConfig &config, float output_variance,
                   const double (&residual_squared)[FirstOrderDelayIdentifier::kDelayBankSize],
                   const std::uint32_t (&residual_count)[FirstOrderDelayIdentifier::kDelayBankSize],
                   FirstOrderModel &model) noexcept
{
    // 保留完整残差观察样本后再比较延迟模型，不因删除未使用诊断而改动采样范围。
    if (residual_count[index] <= 2U ||
        residual_count[index] < config.minimum_samples / 4U) return false;
    const auto &coefficients = bank.getCoefficients();
    const auto variances = bank.getVariances();
    return evaluate_model(coefficients, variances, Delay, index, config, output_variance, residual_squared,
                          residual_count, model);
}

bool step_config_valid(const StepValidationConfig &config) noexcept
{
    if (!finite(config.sample_period_s) || config.sample_period_s <= 0.0F ||
        config.sample_period_s > 1.0F || !finite(config.sample_period_tolerance_s) ||
        config.sample_period_tolerance_s < 0.0F ||
        config.sample_period_tolerance_s >= config.sample_period_s ||
        !finite(config.initial_output) || !finite(config.target_output) ||
        !finite(config.absolute_steady_tolerance) ||
        config.absolute_steady_tolerance < 0.0F ||
        config.minimum_samples < 2U || config.steady_window_samples < 2U ||
        config.steady_window_samples > StepResponseValidator::kMaximumSteadyWindowSamples ||
        config.minimum_samples < config.steady_window_samples) return false;
    const float step = std::fabs(config.target_output - config.initial_output);
    return finite(step) && config.absolute_steady_tolerance < step &&
        step > std::numeric_limits<float>::epsilon();
}

} // namespace

bool FirstOrderDelayIdentifier::configure(const IdentificationConfig &config) noexcept
{
    configured_ = identification_config_valid(config);
    config_ = configured_ ? config : IdentificationConfig{};
    reset();
    return configured_;
}

void FirstOrderDelayIdentifier::reset() noexcept
{
    reset_bank(delay_0_, config_);
    reset_bank(delay_1_, config_);
    reset_bank(delay_2_, config_);
    reset_bank(delay_3_, config_);
    reset_bank(delay_4_, config_);
    reset_bank(delay_5_, config_);
    for (std::size_t index = 0U; index < kDelayBankSize; ++index) {
        residual_squared_[index] = 0.0;
        residual_count_[index] = 0U;
    }
    input_mean_ = input_m2_ = output_mean_ = output_m2_ = 0.0;
    last_timestamp_us_ = 0U;
    sample_count_ = 0U;
    sample_failure_ = configured_ ? CalibrationAlgorithmFailure::None
                                  : CalibrationAlgorithmFailure::InvalidConfiguration;
}

bool FirstOrderDelayIdentifier::add_sample(std::uint64_t timestamp_us,
                                           float input_delta,
                                           float output_delta) noexcept
{
    if (!configured_ || sample_failure_ != CalibrationAlgorithmFailure::None) return false;
    // 开环输出是待辨识的实测响应，不能用期望车速/转速裁掉高增益样本。
    // 输入仍受执行器包络约束；输出检查有限值与采样时序，残差用于延迟模型择优。
    if (!finite(input_delta) || !finite(output_delta) ||
        std::fabs(input_delta) > config_.maximum_absolute_input) {
        sample_failure_ = CalibrationAlgorithmFailure::InvalidSample;
        return false;
    }
    if (!sample_time_valid(timestamp_us, last_timestamp_us_, config_.sample_period_s,
                           config_.sample_period_tolerance_s, sample_failure_)) return false;
    last_timestamp_us_ = timestamp_us;

    ++sample_count_;
    const double count = static_cast<double>(sample_count_);
    const double input_difference = static_cast<double>(input_delta) - input_mean_;
    input_mean_ += input_difference / count;
    input_m2_ += input_difference * (static_cast<double>(input_delta) - input_mean_);
    const double output_difference = static_cast<double>(output_delta) - output_mean_;
    output_mean_ += output_difference / count;
    output_m2_ += output_difference * (static_cast<double>(output_delta) - output_mean_);

    const bool updated =
        update_bank(delay_0_, 0U, input_delta, output_delta, sample_count_, config_,
                    residual_squared_, residual_count_) &&
        update_bank(delay_1_, 1U, input_delta, output_delta, sample_count_, config_,
                    residual_squared_, residual_count_) &&
        update_bank(delay_2_, 2U, input_delta, output_delta, sample_count_, config_,
                    residual_squared_, residual_count_) &&
        update_bank(delay_3_, 3U, input_delta, output_delta, sample_count_, config_,
                    residual_squared_, residual_count_) &&
        update_bank(delay_4_, 4U, input_delta, output_delta, sample_count_, config_,
                    residual_squared_, residual_count_) &&
        update_bank(delay_5_, 5U, input_delta, output_delta, sample_count_, config_,
                    residual_squared_, residual_count_);
    if (!updated) sample_failure_ = CalibrationAlgorithmFailure::ModelRejected;
    return updated;
}

IdentificationResult FirstOrderDelayIdentifier::fit(
    const PiDesignLimits &limits) const noexcept
{
    IdentificationResult result{};
    if (!configured_) return result;
    if (sample_failure_ != CalibrationAlgorithmFailure::None) {
        result.failure = sample_failure_;
        return result;
    }
    if (sample_count_ < config_.minimum_samples || sample_count_ < 2U) {
        result.failure = CalibrationAlgorithmFailure::InsufficientSamples;
        return result;
    }
    const float input_variance = static_cast<float>(
        input_m2_ / static_cast<double>(sample_count_ - 1U));
    const float output_variance = static_cast<float>(
        output_m2_ / static_cast<double>(sample_count_ - 1U));
    // 输入/响应必须实际变化才能辨识；不以2%输入比例或固定1e-4方差拒绝低速响应。
    if (!finite(input_variance) || !finite(output_variance) ||
        input_variance <= 0.0F || output_variance <= 0.0F) {
        result.failure = CalibrationAlgorithmFailure::InsufficientExcitation;
        return result;
    }

    FirstOrderModel candidate{};
    FirstOrderModel best{};
    float best_residual = std::numeric_limits<float>::infinity();
    const auto consider = [&](bool valid) {
        if (valid && candidate.normalized_rms_residual < best_residual) {
            best = candidate;
            best_residual = candidate.normalized_rms_residual;
        }
        candidate = {};
    };
    consider(evaluate_bank(delay_0_, 0U, config_, output_variance, residual_squared_, residual_count_, candidate));
    consider(evaluate_bank(delay_1_, 1U, config_, output_variance, residual_squared_, residual_count_, candidate));
    consider(evaluate_bank(delay_2_, 2U, config_, output_variance, residual_squared_, residual_count_, candidate));
    consider(evaluate_bank(delay_3_, 3U, config_, output_variance, residual_squared_, residual_count_, candidate));
    consider(evaluate_bank(delay_4_, 4U, config_, output_variance, residual_squared_, residual_count_, candidate));
    consider(evaluate_bank(delay_5_, 5U, config_, output_variance, residual_squared_, residual_count_, candidate));
    if (!best.valid) {
        result.failure = CalibrationAlgorithmFailure::ModelRejected;
        return result;
    }
    result.model = best;

    if (!pi_limits_valid(limits)) {
        result.failure = CalibrationAlgorithmFailure::PiRejected;
        return result;
    }
    // Skogestad, J. Process Control 13 (2003), 式(23)(24)：一阶带延迟
    // G(s)=K*exp(-theta*s)/(tau*s+1)的SIMC PI：Kp=tau/[K(lambda+theta)]，
    // Ti=min(tau,4*(lambda+theta))，Ki=Kp/Ti。lambda是显式设计输入，
    // 不再偷偷提高到3*tau、5*theta或另一固定下限；4来自该规则的阻尼推导。
    const float lambda = limits.requested_closed_loop_time_s;
    const float integral_time = std::min(best.time_constant_s, 4.0F * (lambda + best.delay_s));
    const float proportional = best.time_constant_s /
        (best.dc_gain * (lambda + best.delay_s));
    const float integral = proportional / integral_time;
    if (!finite(lambda) || !finite(integral_time) || integral_time <= 0.0F ||
        !finite(proportional) || !finite(integral) || proportional <= 0.0F ||
        integral <= 0.0F) {
        result.failure = CalibrationAlgorithmFailure::PiRejected;
        return result;
    }

    // 候选保持模型设计的P/I和lambda一致。删除“最大误差积分若干秒”的
    // 二次缩放；真实限幅与抗积分饱和由现有控制器执行，数值超参数范围则
    // 明确拒绝候选，不能悄悄缩小增益后继续声称原响应时间。
    if (proportional > limits.maximum_proportional_gain || integral > limits.maximum_integral_gain) {
        result.failure = CalibrationAlgorithmFailure::PiRejected;
        return result;
    }

    result.pi.valid = true;
    result.pi.proportional_gain = proportional;
    result.pi.integral_gain = integral;
    result.pi.integral_time_s = integral_time;
    result.pi.closed_loop_time_s = lambda;
    result.failure = CalibrationAlgorithmFailure::None;
    return result;
}

bool StepResponseValidator::reset(const StepValidationConfig &config) noexcept
{
    configured_ = step_config_valid(config);
    config_ = configured_ ? config : StepValidationConfig{};
    for (std::size_t index = 0U; index < kMaximumSteadyWindowSamples; ++index) {
        steady_measurements_[index] = 0.0F;
        steady_saturation_[index] = false;
    }
    steady_next_ = steady_count_ = steady_saturation_count_ = 0U;
    last_timestamp_us_ = 0U;
    sample_count_ = 0U;
    error_crossings_ = 0U;
    last_error_sign_ = 0;
    maximum_overshoot_ratio_ = 0.0F;
    sample_failure_ = configured_ ? CalibrationAlgorithmFailure::None
                                  : CalibrationAlgorithmFailure::InvalidConfiguration;
    return configured_;
}

bool StepResponseValidator::add_sample(std::uint64_t timestamp_us,
                                       float measurement,
                                       bool saturated) noexcept
{
    if (!configured_ || sample_failure_ != CalibrationAlgorithmFailure::None) return false;
    if (!finite(measurement)) {
        sample_failure_ = CalibrationAlgorithmFailure::InvalidSample;
        return false;
    }
    if (!sample_time_valid(timestamp_us, last_timestamp_us_, config_.sample_period_s,
                           config_.sample_period_tolerance_s, sample_failure_)) return false;
    last_timestamp_us_ = timestamp_us;
    ++sample_count_;

    const float signed_step = config_.target_output - config_.initial_output;
    const float step = std::fabs(signed_step);
    const float direction = signed_step > 0.0F ? 1.0F : -1.0F;
    const float progress = direction * (measurement - config_.initial_output);
    const float overshoot = std::max(0.0F, (progress - step) / step);
    maximum_overshoot_ratio_ = std::max(maximum_overshoot_ratio_, overshoot);
    if (maximum_overshoot_ratio_ > kMaximumOvershootRatio) {
        sample_failure_ = CalibrationAlgorithmFailure::Overshoot;
        return false;
    }

    // 振荡与最终跟踪误差使用同一验证容差，不另乘接收机噪声造接受边界。
    const float allowed_error = config_.absolute_steady_tolerance > 0.0F
        ? config_.absolute_steady_tolerance : kSteadyErrorRatio * step;
    const float error = config_.target_output - measurement;
    if (std::fabs(error) > allowed_error) {
        const std::int8_t sign = error > 0.0F ? 1 : -1;
        if (last_error_sign_ != 0 && sign != last_error_sign_) {
            if (error_crossings_ < std::numeric_limits<std::uint8_t>::max())
                ++error_crossings_;
            if (error_crossings_ > config_.maximum_error_crossings) {
                sample_failure_ = CalibrationAlgorithmFailure::SustainedOscillation;
                return false;
            }
        }
        last_error_sign_ = sign;
    }

    // 加速过程允许到顶，不再按固定1秒提前否决。最终观察窗仍检查
    // 到顶且误差超标，跟踪误差/超调/振荡验证保持原要求。
    const float tracking_tolerance = config_.absolute_steady_tolerance > 0.0F
        ? config_.absolute_steady_tolerance : kSteadyErrorRatio * step;
    const bool limited_error = saturated && std::fabs(error) > tracking_tolerance;

    const std::size_t window = config_.steady_window_samples;
    if (steady_count_ == window && steady_saturation_[steady_next_])
        --steady_saturation_count_;
    steady_measurements_[steady_next_] = measurement;
    steady_saturation_[steady_next_] = limited_error;
    if (limited_error) ++steady_saturation_count_;
    steady_next_ = (steady_next_ + 1U) % window;
    if (steady_count_ < window) ++steady_count_;
    return true;
}

StepValidationResult StepResponseValidator::result() const noexcept
{
    StepValidationResult output{};
    output.sample_count = sample_count_;
    output.error_crossings = error_crossings_;
    output.maximum_overshoot_ratio = maximum_overshoot_ratio_;
    const float step = std::fabs(config_.target_output - config_.initial_output);
    // 稳态目标精度由调用方给定（缺省沿用比例精度），不把近零死区或
    // 接收机单样本精度叠成另一套容差；振荡计数也使用该验证容差。
    output.allowed_steady_state_error = config_.absolute_steady_tolerance > 0.0F
        ? config_.absolute_steady_tolerance : kSteadyErrorRatio * step;
    if (!configured_) return output;
    if (sample_failure_ != CalibrationAlgorithmFailure::None) {
        output.failure = sample_failure_;
        return output;
    }
    if (sample_count_ < config_.minimum_samples ||
        steady_count_ < config_.steady_window_samples) {
        output.failure = CalibrationAlgorithmFailure::InsufficientSamples;
        return output;
    }
    // 整个最终窗口持续到顶且误差超标，不能把受限响应判成通过。
    if (steady_saturation_count_ == steady_count_) {
        output.failure = CalibrationAlgorithmFailure::SustainedSaturation;
        return output;
    }

    double sum = 0.0;
    for (std::size_t index = 0U; index < steady_count_; ++index)
        sum += steady_measurements_[index];
    const float mean = static_cast<float>(sum / static_cast<double>(steady_count_));
    output.steady_state_error = std::fabs(config_.target_output - mean);
    if (!finite(mean) || !finite(output.steady_state_error) ||
        output.steady_state_error > output.allowed_steady_state_error) {
        output.failure = CalibrationAlgorithmFailure::SteadyStateError;
        return output;
    }
    output.failure = CalibrationAlgorithmFailure::None;
    return output;
}

} // namespace dima::lib::rover::calibration


// 普通运行期实现从对应头文件移出；保持原状态、错误分支和计算顺序。

namespace dima::lib::rover::calibration {

bool IdentificationResult::valid() const noexcept
{
    return failure == CalibrationAlgorithmFailure::None && model.valid && pi.valid;
}

CalibrationAlgorithmFailure FirstOrderDelayIdentifier::sample_failure() const noexcept
{ return sample_failure_; }

std::uint32_t FirstOrderDelayIdentifier::sample_count() const noexcept
{ return sample_count_; }

bool StepValidationResult::valid() const noexcept
{ return failure == CalibrationAlgorithmFailure::None; }

CalibrationAlgorithmFailure StepResponseValidator::sample_failure() const noexcept
{ return sample_failure_; }

} // namespace dima::lib::rover::calibration
