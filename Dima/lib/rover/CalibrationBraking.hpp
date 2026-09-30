#pragma once

namespace dima::lib::rover::calibration {

float stopping_distance(float speed_m_s, float deceleration_m_s2) noexcept;
float measured_braking_input_gain(float reverse_impulse_s, float squared_impulse_s,
    float motor_limit, float initial_speed_m_s, float stopped_speed_m_s) noexcept;
float proportional_braking_time(float initial_speed_m_s, float release_speed_m_s,
    float input_gain, float effective_time_s) noexcept;

} // namespace dima::lib::rover::calibration
