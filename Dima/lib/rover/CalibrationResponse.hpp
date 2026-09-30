#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace dima::lib::rover::calibration {

enum class ResponseFailure : std::uint8_t {
    None,
    InvalidConfiguration,
    InvalidSample,
    DuplicateTimestamp,
    TimeRegression,
    CapacityExceeded,
    InsufficientSamples,
    InsufficientExcitation,
    UncertainResponse,
    DirectionMismatch,
    IncompleteCoverage,
    NonmonotonicResponse,
    InconsistentDirections,
    MinimumUnobservable,
    ModelRejected,
};

struct ResponseEstimate {
    float value{std::numeric_limits<float>::quiet_NaN()};
    ResponseFailure failure{ResponseFailure::InsufficientSamples};
    bool valid() const noexcept;
};

// 时间窗实测均值；调用方负责输入保持、传感器设备和参数代次一致。
// 重复或回退时间锁存失败。已删除无消费者的方差、峰值以及轮端重复统计。
class ResponseStatistics final {
public:
    bool add(std::uint64_t timestamp_us, float value) noexcept;
    void reset() noexcept;
    std::uint32_t count() const noexcept;
    double mean() const noexcept;
    float duration_s() const noexcept;
    ResponseFailure failure() const noexcept;

private:
    double mean_{};
    std::uint64_t first_timestamp_us_{};
    std::uint64_t last_timestamp_us_{};
    std::uint32_t count_{};
    ResponseFailure failure_{ResponseFailure::None};
};

// 调用方只喂非零升/降阶跃的单调响应区间，不把最终零命令的自由停车当成
// RO_DECEL_LIM 生效证据。固定中心矩避免绝对时间戳使最小二乘发生相消。
class TransientSlope final {
public:
    bool add(std::uint64_t timestamp_us, float response) noexcept;
    void reset() noexcept;
    std::uint32_t count() const noexcept;
    ResponseFailure failure() const noexcept;
    float signed_rate() const noexcept;
    ResponseEstimate measured_rate() const noexcept;
    float duration_s() const noexcept;

private:
    double mean_time_{};
    double mean_response_{};
    double time_m2_{};
    double time_response_m2_{};
    float minimum_{};
    float maximum_{};
    std::uint64_t first_timestamp_us_{};
    std::uint64_t last_timestamp_us_{};
    std::uint32_t count_{};
    ResponseFailure failure_{ResponseFailure::None};
};

struct ResponsePlateau {
    // 输入/速度按direction归一化；轮端执行值只在采样入口校验。raw_speed
    // 不取绝对值，真实反向响应和零速噪声仍保留符号，不伪装成正响应。
    ResponseStatistics pre_input{};
    ResponseStatistics raw_speed{};
    std::uint32_t count() const noexcept;
};

class MotorResponseProfile final {
public:
    static constexpr std::size_t kDirections{2U};
    static constexpr std::size_t kLevels{6U};
    static constexpr std::size_t kForward{0U};
    static constexpr std::size_t kReverse{1U};

    bool add(std::size_t direction, std::size_t level,
             std::uint64_t timestamp_us, float pre_input,
             float applied_input, float raw_speed) noexcept;
    void reset() noexcept;
    const ResponsePlateau *plateau(std::size_t direction,
                                   std::size_t level) const noexcept;
    bool reset_plateau(std::size_t direction, std::size_t level) noexcept;
    ResponseFailure failure() const noexcept;

    // 速度取已测稳态平台的最大均值，不从测量值扣除噪声。
    ResponseEstimate measured_speed(std::size_t direction) const noexcept;

private:
    ResponsePlateau plateaus_[kDirections][kLevels]{};
    std::uint64_t last_timestamp_us_{};
    ResponseFailure failure_{ResponseFailure::None};
};

// 固定容量单参数搜索；分割比例只属于数值求解，不是控制增益折扣。
struct TrialScore {
    float primary{std::numeric_limits<float>::infinity()};
    float secondary{std::numeric_limits<float>::infinity()};
    bool valid{false};
};
bool better_trial(const TrialScore &a, const TrialScore &b) noexcept;
class BoundedParameterSearch final {
public:
    bool reset(float lower, float upper, float resolution, bool binary = false) noexcept;
    float candidate() const noexcept;
    bool observe(const TrialScore &score) noexcept;
private:
    float lower_{}, upper_{}, resolution_{}, points_[2]{};
    TrialScore scores_[2]{};
    unsigned remaining_{}, slot_{};
    bool binary_{}, paired_{};
};

} // namespace dima::lib::rover::calibration
