#include "CalibrationMath.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace dima::lib::rover::calibration {

float wrap_pi(float angle) noexcept
{
    if (!std::isfinite(angle)) return std::numeric_limits<float>::quiet_NaN();
    constexpr float pi = 3.14159265358979323846F;
    return std::remainder(angle, 2.0F * pi);
}

void CircularMean::add(float angle, float sample_weight) noexcept
{
    if (!std::isfinite(angle) || !std::isfinite(sample_weight) || sample_weight <= 0.0F) return;
    sine += sample_weight * std::sin(angle);
    cosine += sample_weight * std::cos(angle);
    weight += sample_weight;
    ++count;
}

bool CircularMean::result(float &angle, float minimum_concentration) const noexcept
{
    // 圆均值跨越 +/-pi 时连续；resultant/weight 衡量集中度，不能直接线性
    // 平均 179 与 -179 度。低集中度表示偏置不恒定（侧滑、错时或解跳变）。
    if (count < 30U || weight <= 0.0 ||
        std::hypot(sine, cosine) / weight < minimum_concentration) return false;
    angle = static_cast<float>(std::atan2(sine, cosine));
    return std::isfinite(angle);
}

bool baseline(float *samples, std::size_t count, float &length) noexcept
{
    if (count < 100U || count > 120U) return false;
    std::sort(samples, samples + count);
    length = samples[count / 2U];
    float deviation[120]{};
    for (std::size_t i = 0U; i < count; ++i) deviation[i] = std::fabs(samples[i] - length);
    std::sort(deviation, deviation + count);
    // MAD 对少量多路径离群值稳健；另用 5%/95% 分位间距拒绝双峰假收敛。
    return std::isfinite(length) && length >= 0.1F && length <= 10.0F &&
        deviation[count / 2U] <= std::fmax(0.005F, 0.01F * length) &&
        samples[count * 95U / 100U] - samples[count * 5U / 100U] <=
            std::fmax(0.02F, 0.03F * length);
}

void displacement(double latitude, double longitude, double origin_latitude,
                  double origin_longitude, float &north, float &east) noexcept
{
    // 40 m 以内使用局部球面近似，纬经差保持 double，避免 float 经纬度量化
    // 吞掉厘米级 RTK 位移；经差先 wrap，覆盖日期变更线。
    constexpr double radius_radians = 6371000.0 * 0.01745329251994329577;
    north = static_cast<float>((latitude - origin_latitude) * radius_radians);
    east = static_cast<float>(std::remainder(longitude - origin_longitude, 360.0) *
        radius_radians * std::cos(origin_latitude * 0.01745329251994329577));
}

} // namespace dima::lib::rover::calibration
