#include "CalibrationResponse.hpp"

#include <algorithm>
#include <cmath>

namespace dima::lib::rover::calibration {

bool MotorResponseProfile::reset_plateau(std::size_t direction, std::size_t level) noexcept
{
    if (direction >= kDirections || level >= kLevels || failure_ != ResponseFailure::None) return false;
    // 连续稳态被打断时只清当前档；全局去重时间保留，不能藉清窗重复喂旧样本。
    plateaus_[direction][level] = {};
    return true;
}

namespace {

constexpr std::uint32_t kMinimumPlateauSamples{10U};
constexpr double kMinimumDivisor{1.0e-12};

float unavailable() noexcept
{
    return std::numeric_limits<float>::quiet_NaN();
}

bool valid_timestamp(std::uint64_t timestamp, std::uint64_t previous,
                      ResponseFailure &failure) noexcept
{
    if (timestamp == 0U) failure = ResponseFailure::InvalidSample;
    else if (timestamp == previous) failure = ResponseFailure::DuplicateTimestamp;
    else if (timestamp < previous) failure = ResponseFailure::TimeRegression;
    return failure == ResponseFailure::None;
}

ResponseEstimate rejected(ResponseFailure failure) noexcept
{
    return {unavailable(), failure};
}

ResponseEstimate estimate(double value) noexcept
{
    const float result = static_cast<float>(value);
    return std::isfinite(result) ? ResponseEstimate{result, ResponseFailure::None}
                                 : rejected(ResponseFailure::UncertainResponse);
}

bool plateau_valid(const ResponsePlateau &plateau) noexcept
{
    const auto count = plateau.count();
    // add原子接收同一历元的输入/响应；全局failure已由查询入口检查，
    // 不为同一批样本重复保存轮端均值/方差及校验同一计数。
    return count >= kMinimumPlateauSamples && plateau.raw_speed.duration_s() >= 1.0F;
}

} // namespace

bool ResponseStatistics::add(std::uint64_t timestamp_us, float value) noexcept
{
    if (failure_ != ResponseFailure::None) return false;
    if (!std::isfinite(value)) failure_ = ResponseFailure::InvalidSample;
    if (count_ == UINT32_MAX) failure_ = ResponseFailure::CapacityExceeded;
    if (failure_ != ResponseFailure::None || !valid_timestamp(timestamp_us, last_timestamp_us_, failure_)) return false;
    if (count_ == 0U) {
        first_timestamp_us_ = timestamp_us;
    }
    last_timestamp_us_ = timestamp_us;
    ++count_;
    const double difference = value - mean_;
    mean_ += difference / count_;
    return true;
}

void ResponseStatistics::reset() noexcept { *this = {}; }

double ResponseStatistics::mean() const noexcept
{
    return count_ != 0U && failure_ == ResponseFailure::None ? mean_ : unavailable();
}

float ResponseStatistics::duration_s() const noexcept
{
    return count_ >= 2U && failure_ == ResponseFailure::None
        ? static_cast<float>(last_timestamp_us_ - first_timestamp_us_) * 1.0e-6F : 0.0F;
}

bool TransientSlope::add(std::uint64_t timestamp_us, float response) noexcept
{
    if (failure_ != ResponseFailure::None) return false;
    if (!std::isfinite(response)) failure_ = ResponseFailure::InvalidSample;
    if (count_ == UINT32_MAX) failure_ = ResponseFailure::CapacityExceeded;
    if (failure_ != ResponseFailure::None || !valid_timestamp(timestamp_us, last_timestamp_us_, failure_)) return false;
    if (count_ == 0U) {
        first_timestamp_us_ = timestamp_us;
        minimum_ = maximum_ = response;
    }
    const double seconds = static_cast<double>(timestamp_us - first_timestamp_us_) * 1.0e-6;
    last_timestamp_us_ = timestamp_us;
    minimum_ = std::min(minimum_, response);
    maximum_ = std::max(maximum_, response);
    ++count_;
    const double time_difference = seconds - mean_time_;
    const double response_difference = response - mean_response_;
    mean_time_ += time_difference / count_;
    mean_response_ += response_difference / count_;
    time_m2_ += time_difference * (seconds - mean_time_);
    time_response_m2_ += time_difference * (response - mean_response_);
    return true;
}

void TransientSlope::reset() noexcept { *this = {}; }

float TransientSlope::signed_rate() const noexcept
{
    if (failure_ != ResponseFailure::None || count_ < 2U || time_m2_ <= kMinimumDivisor) return unavailable();
    const float rate = static_cast<float>(time_response_m2_ / time_m2_);
    return std::isfinite(rate) ? rate : unavailable();
}

float TransientSlope::duration_s() const noexcept
{
    return count_ >= 2U && failure_ == ResponseFailure::None
        ? static_cast<float>(last_timestamp_us_ - first_timestamp_us_) * 1.0e-6F : 0.0F;
}

ResponseEstimate TransientSlope::measured_rate() const noexcept
{
    if (failure_ != ResponseFailure::None) return rejected(failure_);
    if (count_ < 8U) return rejected(ResponseFailure::InsufficientSamples);
    const double span = static_cast<double>(maximum_) - minimum_;
    if (span <= 0.0 || time_m2_ <= kMinimumDivisor)
        return rejected(ResponseFailure::InsufficientExcitation);
    const double slope = time_response_m2_ / time_m2_;
    // 最小二乘 a=cov(t,v)/var(t) 给出本窗口实测变化率，不另套20%残差拒绝线。
    if (!std::isfinite(slope) || slope == 0.0)
        return rejected(ResponseFailure::UncertainResponse);
    return estimate(std::fabs(slope));
}

bool MotorResponseProfile::add(std::size_t direction, std::size_t level,
                               std::uint64_t timestamp_us, float pre_input,
                               float applied_input, float raw_speed) noexcept
{
    if (failure_ != ResponseFailure::None) return false;
    if (direction >= kDirections || level >= kLevels) failure_ = ResponseFailure::CapacityExceeded;
    else if (!std::isfinite(pre_input) || !std::isfinite(applied_input) || !std::isfinite(raw_speed) ||
             std::fabs(pre_input) > 1.0F || std::fabs(applied_input) > 1.0F)
        failure_ = ResponseFailure::InvalidSample;
    const float sign = direction == kForward ? 1.0F : -1.0F;
    if (failure_ == ResponseFailure::None && (sign * pre_input < 0.0F || sign * applied_input < 0.0F))
        failure_ = ResponseFailure::DirectionMismatch;
    if (failure_ != ResponseFailure::None || !valid_timestamp(timestamp_us, last_timestamp_us_, failure_)) return false;
    last_timestamp_us_ = timestamp_us;
    auto &plateau = plateaus_[direction][level];
    const bool accepted = plateau.pre_input.add(timestamp_us, sign * pre_input) &&
        plateau.raw_speed.add(timestamp_us, sign * raw_speed);
    if (!accepted) failure_ = ResponseFailure::InvalidSample;
    return accepted;
}

void MotorResponseProfile::reset() noexcept { *this = {}; }

const ResponsePlateau *MotorResponseProfile::plateau(std::size_t direction,
                                                     std::size_t level) const noexcept
{
    return direction < kDirections && level < kLevels ? &plateaus_[direction][level] : nullptr;
}

ResponseEstimate MotorResponseProfile::measured_speed(std::size_t direction) const noexcept
{
    if (failure_ != ResponseFailure::None) return rejected(failure_);
    if (direction >= kDirections) return rejected(ResponseFailure::InvalidConfiguration);
    double best{};
    for (const auto &plateau : plateaus_[direction]) {
        if (!plateau_valid(plateau)) continue;
        best = std::max(best, plateau.raw_speed.mean());
    }
    return best > 0.0 ? estimate(best) : rejected(ResponseFailure::InsufficientExcitation);
}

} // namespace dima::lib::rover::calibration


// 普通运行期实现从对应头文件移出；保持原状态、错误分支和计算顺序。

namespace dima::lib::rover::calibration {

bool ResponseEstimate::valid() const noexcept
{ return failure == ResponseFailure::None; }

std::uint32_t ResponseStatistics::count() const noexcept
{ return count_; }

ResponseFailure ResponseStatistics::failure() const noexcept
{ return failure_; }

std::uint32_t TransientSlope::count() const noexcept
{ return count_; }

ResponseFailure TransientSlope::failure() const noexcept
{ return failure_; }

std::uint32_t ResponsePlateau::count() const noexcept
{ return raw_speed.count(); }

ResponseFailure MotorResponseProfile::failure() const noexcept
{ return failure_; }

bool better_trial(const TrialScore &a, const TrialScore &b) noexcept
{
    // 两个同量纲指标按调用方声明的优先级比较；不混合加权或扣除测量误差。
    return a.valid && (!b.valid || a.primary < b.primary ||
        (a.primary == b.primary && a.secondary < b.secondary));
}

bool BoundedParameterSearch::reset(float lower, float upper, float resolution, bool binary) noexcept
{
    *this = {};
    if (!std::isfinite(lower) || !std::isfinite(upper) || !std::isfinite(resolution) ||
        resolution <= 0.0F || upper - lower <= resolution) return false;
    lower_ = lower; upper_ = upper; resolution_ = resolution; binary_ = binary;
    constexpr float fraction = 0.61803398875F;
    remaining_ = static_cast<unsigned>(std::ceil(std::log((upper - lower) / resolution) /
        -std::log(binary ? 0.5F : fraction))) + 2U;
    points_[0] = binary ? (lower + upper) * 0.5F : upper - fraction * (upper - lower);
    points_[1] = lower + fraction * (upper - lower);
    return true;
}

float BoundedParameterSearch::candidate() const noexcept { return points_[slot_]; }

bool BoundedParameterSearch::observe(const TrialScore &score) noexcept
{
    if (remaining_ == 0U) return false;
    --remaining_;
    if (binary_) {
        if (score.valid) lower_ = points_[0]; else upper_ = points_[0];
        points_[0] = 0.5F * (lower_ + upper_);
    } else {
        scores_[slot_] = score;
        if (!paired_) { paired_ = true; slot_ = 1U; return remaining_ != 0U; }
        constexpr float fraction = 0.61803398875F;
        if (better_trial(scores_[1], scores_[0])) {
            lower_ = points_[0]; points_[0] = points_[1]; scores_[0] = scores_[1];
            points_[1] = lower_ + fraction * (upper_ - lower_); slot_ = 1U;
        } else {
            upper_ = points_[1]; points_[1] = points_[0]; scores_[1] = scores_[0];
            points_[0] = upper_ - fraction * (upper_ - lower_); slot_ = 0U;
        }
    }
    return remaining_ != 0U && upper_ - lower_ > resolution_;
}

} // namespace dima::lib::rover::calibration
