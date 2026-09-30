#include "CalibrationBraking.hpp"

#include <algorithm>
#include <cmath>

namespace dima::lib::rover::calibration {


float stopping_distance(float speed, float deceleration) noexcept
{
    // 以本轮实测平均减速度建立名义停车模型：s=v²/(2a)。这里不是地面摩擦
    // 系数模型，也不把估算距离写成实测距离；历史制动初速不作为运行上限。
    if (!std::isfinite(speed) || speed < 0.0F ||
        !std::isfinite(deceleration) || deceleration <= 0.0F) return NAN;
    return 0.5F * speed * (speed / deceleration);
}

float measured_braking_input_gain(float impulse, float squared_impulse, float motor_limit,
    float initial, float stopped) noexcept
{
    if (!std::isfinite(impulse) || impulse < 0.0F || !std::isfinite(motor_limit) || motor_limit <= 0.0F ||
        !std::isfinite(initial) || initial <= 0.0F || !std::isfinite(stopped) || stopped < 0.0F || stopped >= initial ||
        !std::isfinite(squared_impulse) || squared_impulse < 0.0F) return NAN;
    // J=∫b dt，Q=∫b²dt。T_eff=J²/Q为输入加权的等效作用时长，零输出
    // 死区/换向等待/停稳确认均不贡献；低幅爬升的影响由实际输入权重决定。
    // K=Δv/(J/E)，g=1/(K*T_eff)=Q/(E*J*Δv)。Δv只取前向停车段，
    // 不再把负向回摆扩进分母。J=Q=0明确表示已测得滑停，无人为时间常数。
    if (impulse == 0.0F) return squared_impulse == 0.0F ? 0.0F : NAN;
    if (squared_impulse == 0.0F) return NAN;
    return squared_impulse / (motor_limit * impulse * (initial - stopped));
}

float proportional_braking_time(float initial, float release, float gain, float effective_time) noexcept
{
    if (!std::isfinite(initial) || initial < 0.0F || !std::isfinite(release) || release <= 0.0F ||
        !std::isfinite(gain) || gain <= 0.0F || !std::isfinite(effective_time) || effective_time <= 0.0F) return NAN;
    if (initial <= release) return 0.0F;
    // u=min(g*v,1)，K*g=1/T_eff：饱和段按恒定K减速，随后才是指数段。
    // t=T_eff*[g*max(v0-max(1/g,v_release),0)+ln(max(min(v0,1/g)/v_release,1))]。
    // 这里预测到既有撤力带的时间，不替代后端零输出/停稳证据，也不增加低速反推。
    const float transition = 1.0F / gain;
    const float linear = gain * std::max(0.0F, initial - std::max(transition, release));
    const float exponential = std::log(std::max(1.0F, std::min(initial, transition) / release));
    return effective_time * (linear + exponential);
}

} // namespace dima::lib::rover::calibration
