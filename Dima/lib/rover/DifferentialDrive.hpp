#pragma once

#include <cstdint>

namespace dima::lib::rover {

struct DifferentialDriveConfig {
    float steering_throttle_mix;
    float throttle_min;
    float throttle_max;
    float throttle_slew_rate;
    float reversal_delay_s;
    float throttle_expo;
    float thrust_asymmetry;
    float arm_ramp_s;
};

struct DifferentialDriveOutput {
    float right;
    float left;
    bool valid;
    bool motor_slew_active{};
    bool mixing_limited{};
    bool shaping_active{};
    bool arm_ramp_active{};
    bool reversal_held{};
};

/**
 * 固定存储的差速混控与电机保护核。
 *
 * manual_source=true 使用人工 steering/throttle 优先级；
 * 转向正值始终表示车头顺时针，前进与倒车使用同一转向符号。
 * Manual 先按 APM 的 |T|+|S| 同比缩放，再经过 slew 和逐轮换向等待。
 * Navigation 已在四环完成转向优先，零 longitudinal 立即清除旧 slew。
 * 最终轮端仍受本项目 MOT_THR_MAX 包络约束；Arm ramp仅限制驱动建立。
 */
class DifferentialDrive {
public:
    // Brake 是已限定的纵向制动力比例，不使用驱动起步补偿；保护链仍共用。
    enum class Purpose : std::uint8_t { Drive, Brake };
    bool configure(const DifferentialDriveConfig &config) noexcept;
    DifferentialDriveOutput update(float longitudinal, float steering,
                                   bool manual_source, bool armed,
                                   std::uint64_t now_us,
                                   float dt_s) noexcept;
    // 将 Manual 的额外正向输入地板与混控优先级分开：校准复用混控，
    // manual_throttle_floor=false 只跳过入口地板。Drive 使用 MIN/EXPO/ASYM；
    // Brake 直接按 E 缩放，沿用电机 slew/逐轮换向等待，跳过驱动 Arm ramp。
    DifferentialDriveOutput update(float longitudinal, float steering,
                                   bool manual_source, bool armed,
                                   std::uint64_t now_us, float dt_s,
                                   bool manual_throttle_floor,
                                   Purpose purpose = Purpose::Drive) noexcept;
    void reset() noexcept;

private:
    struct ReversalState {
        float last_nonzero{0.0F};
        std::uint64_t last_output_time_us{0U};
        bool have_output{false};
    };

    static bool finite(float value) noexcept;
    static float clamp(float value, float lower, float upper) noexcept;
    static float interpolate(float from, float to, float ratio) noexcept;
    static float signed_unit(float value) noexcept;
    static float expo_curve(float magnitude, float expo) noexcept;
    static bool valid_config(const DifferentialDriveConfig &config) noexcept;
    static void prioritize_axes(float &longitudinal, float &steering,
                                float priority,
                                float lower_motor_limit) noexcept;
    float shape_motor(float command) const noexcept;
    float apply_reversal_delay(float command, ReversalState &state,
                               std::uint64_t now_us) const noexcept;

    DifferentialDriveConfig config_{};
    ReversalState right_reversal_{};
    ReversalState left_reversal_{};
    float limited_longitudinal_{0.0F};
    std::uint64_t armed_since_us_{0U};
    bool configured_{false};
    bool armed_{false};
};

} // namespace dima::lib::rover
