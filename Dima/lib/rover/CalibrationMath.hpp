#pragma once

#include <cstddef>
#include <cstdint>

namespace dima::lib::rover::calibration {

float wrap_pi(float angle) noexcept;
struct CircularMean {
    double sine{}, cosine{}, weight{};
    std::uint32_t count{};
    void add(float angle, float sample_weight) noexcept;
    bool result(float &angle, float minimum_concentration = 0.998F) const noexcept;
};

bool baseline(float *samples, std::size_t count, float &length) noexcept;
void displacement(double latitude, double longitude, double origin_latitude,
                  double origin_longitude, float &north, float &east) noexcept;

} // namespace dima::lib::rover::calibration
