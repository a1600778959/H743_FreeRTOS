#define MODULE_NAME "auto_cal"
#include "AutoCalibrationMode.hpp"
#include "auto/AutoMode.hpp"
#include "control/RoverDifferential.hpp"
#include "logging/logging.hpp"

#include <algorithm>
#include <cmath>

namespace dima::rover::modes {
namespace math = dima::lib::rover::calibration;
namespace {

bool origin_gain(const math::MotorResponseProfile &profile, unsigned direction, float &gain) noexcept
{
    if (profile.failure() != math::ResponseFailure::None) return false;
    double uu = 0.0, uv = 0.0;
    unsigned levels = 0U;
    for (unsigned i = 0U; i < math::MotorResponseProfile::kLevels; ++i) {
        const auto *p = profile.plateau(direction, i);
        if (p == nullptr || p->count() < 10U || p->raw_speed.duration_s() < 1.0F) continue;
        const double u = p->pre_input.mean();
        const double v = p->raw_speed.mean();
        // profile入口已检查每个执行值及同一历元的输入/响应，失败会锁存；
        // 拟合只需输入与响应均值，不保留另一份轮端统计或重复检查样本计数。
        if (!std::isfinite(u) || !std::isfinite(v) || u <= 0.0 || v <= 0.0) continue;
        const double w = p->count();
        uu += w * u * u; uv += w * u * v;
        ++levels;
    }
    if (levels < 3U || uu <= 1.0e-10) return false;
    gain = static_cast<float>(uv / uu);
    // 过原点最小二乘给出候选；不以固定10%残差拒绝，效果由后续闭环验证。
    return std::isfinite(gain) && gain > 0.0F && gain <= 1000.0F;

}

} // namespace

// —— RUNTIME 候选 cohort：纯算法候选 + 事务操作 ————————————————————————
// 事务身份在写入前登记为Runtime，前端确认与Provisional/Done/Failed推进在
// PROFILE Evaluate 子状态的事务机中；本文件不再写状态、不再做跳转。

bool AutoCalibrationMode::calculate_runtime_candidates(std::uint64_t now) noexcept
{
    (void)now;
    // 前进能力可辨识 FF/运行值；全局电机整形需要正反域，本模式明确跳过。
    float forward{}, clockwise{}, counterclockwise{};
    const bool rate_fit = origin_gain(response_rate_, 0U, clockwise) &&
        origin_gain(response_rate_, 1U, counterclockwise);
    // 两个方向各自的拟合必须有效，但路面阻力差不构成20%对称性准入条件。
    if (!origin_gain(response_speed_, 0U, forward) || !rate_fit) return false;
    const auto forward_speed = response_speed_.measured_speed(0U);
    const auto rate_cw = response_rate_.measured_speed(0U);
    const auto rate_ccw = response_rate_.measured_speed(1U);
    if (!forward_speed.valid() || !rate_cw.valid() || !rate_ccw.valid()) return false;
    const float yaw_correction = 2.0F * forward / (config_.track * 0.5F * (clockwise + counterclockwise));
    if (!std::isfinite(yaw_correction) || yaw_correction < 0.01F || yaw_correction > 100.0F || forward > 100.0F) return false;
    auto &c = tuning_config_;
    // 纵向只声明本次实际采到的前进范围；转向取两个方向都实际覆盖的范围。
    // speed_limit 在此仅是本轮实验目标，不写回用户 RO_SPEED_LIM，也不设置巡航速度折扣。
    // 实验范围是用户巡航范围与实际覆盖范围的交集；不对实测速度打折，
    // forward已在混控前归一化轴上拟合，不能再次乘电机包络E。
    c.speed_limit = std::min({fence_.speed_limit_m_s, forward_speed.value, forward});
    c.rate_limit = std::min(rate_cw.value, rate_ccw.value);
    if (!std::isfinite(c.speed_limit) || c.speed_limit <= 0.0F || !std::isfinite(c.rate_limit) || c.rate_limit <= 0.0F ||
        !std::isfinite(c.speed_threshold) || c.speed_threshold < 0.0F ||
        !std::isfinite(c.rate_threshold) || c.rate_threshold < 0.0F) return false;
    // 测量死区属于用户运行参数；校准期的可实现精度由验证器结合实测噪声
    // 计算，不在这里把死区改写成噪声倍数，也不因 10% 比例门限提前失败。
    float *limits[4]{&c.acceleration, &c.deceleration, &c.rate_acceleration, &c.rate_deceleration};
    runtime_fully_observed_ = true;
    if (!braking_model_ready()) return false;
    for (unsigned i = 0U; i < 4U; ++i) {
        if (i == 1U) {
            // 纵向减速度只来自正式全输出停车，非零平台下降不能覆盖它。
            *limits[i] = status_.braking_deceleration_m_s2;
        } else if (response_rate_duration_[i] > 0.0F) {
            // 实测斜率按窗口时长合并，不扣置信下界、不乘0.5或封顶1.5。
            *limits[i] = response_rate_sum_[i] / response_rate_duration_[i];
        } else {
            runtime_fully_observed_ = false;
        }
        const float maximum = i < 2U ? 100.0F : 10000.0F * kRadians;
        if (!std::isfinite(*limits[i]) || *limits[i] <= 0.0F || *limits[i] > maximum) return false;
    }
    // jerk没有独立观测，不从人为输入爬升时间制造数值；保留用户原值及
    // -1/0禁用语义，依赖jerk的路径试验按既有准备检查报告不可用。
    if (!std::isfinite(c.jerk) || c.jerk < -1.0F || c.jerk > 100.0F) return false;
    c.driving.stopped_speed_threshold_m_s = c.speed_threshold;
    // 三个全局整形量不能用局部正反比独立提交。当前倒车/空间无法覆盖时
    // 当前模式保留 Manual 共用电机参数，只提交随后可被真实 FF/PI/路径验证的运行值。
    // 全部计算有效后才更新候选；事务和后续增益修订共用这一份值。
    status_.maximum_speed_m_s = forward;
    status_.yaw_rate_correction = yaw_correction;
    return true;
}

bool AutoCalibrationMode::begin_runtime_transaction(std::uint64_t now) noexcept
{
    if (!maintenance_ready() || runtime_cohort_ || !transaction_.prepare()) return false;
    const auto set = [&](dima::params parameter, float value) {
        return transaction_.add_float(parameter, value);
    };
    const auto &c = tuning_config_;
    // 组成员只引用权威生成的参数标识；事务 32 槽保存关联运行/增益参数，最初
    // 旧值在后续 PI、Heading、路径候选更新时一直保留，直到整体保存或回滚。
    // 已在先前组出现过的参数由会话日志去重，不占用新增槽位。
    const bool added =
        set(dima::params::RO_MAX_THR_SPEED, status_.maximum_speed_m_s) &&
        set(dima::params::RO_YAW_RATE_CORR, status_.yaw_rate_correction) &&
        set(dima::params::RO_SPEED_P, c.inner[0]) && set(dima::params::RO_SPEED_I, c.inner[1]) &&
        set(dima::params::RO_YAW_RATE_P, c.inner[2]) && set(dima::params::RO_YAW_RATE_I, c.inner[3]) &&
        set(dima::params::RO_YAW_RATE_LIM, c.rate_limit / kRadians) &&
        set(dima::params::RO_ACCEL_LIM, c.acceleration) && set(dima::params::RO_DECEL_LIM, c.deceleration) &&
        set(dima::params::RO_YAW_ACCEL_LIM, c.rate_acceleration / kRadians) &&
        set(dima::params::RO_YAW_DECEL_LIM, c.rate_deceleration / kRadians) &&
        set(dima::params::RO_JERK_LIM, c.jerk) && set(dima::params::RO_SPEED_RED, c.speed_reduction) &&
        set(dima::params::RO_YAW_P, c.heading_p) &&
        set(dima::params::RD_TRANS_TRN_DRV, c.driving.turn_to_drive_yaw_error_rad) &&
        set(dima::params::RD_TRANS_DRV_TRN, c.driving.drive_to_turn_yaw_error_rad) &&
        set(dima::params::PP_LOOKAHD_GAIN, c.pursuit.lookahead_gain);
    runtime_cohort_ = true;
    if (!added || !apply_transaction(TransactionKind::Runtime, now, true)) return false;
    status_.closed_loop = false;
    return true;
}

bool AutoCalibrationMode::runtime_frontend_confirmed() const noexcept
{
    // 参数值确认同时核对前端代次，不再额外执行一次仅代次确认。
    return transaction_.generation_valid() &&
        drive_.calibration_parameters_applied(transaction_.generation(), transaction_.expected_float(dima::params::RO_MAX_THR_SPEED),
            transaction_.expected_float(dima::params::RO_YAW_RATE_CORR)) &&
        navigation_.calibration_parameters_applied(transaction_.generation(), transaction_.expected_float(dima::params::RO_YAW_P),
            transaction_.expected_float(dima::params::PP_LOOKAHD_GAIN),
            transaction_.expected_float(dima::params::RO_JERK_LIM), transaction_.expected_float(dima::params::RO_SPEED_RED));
}

bool AutoCalibrationMode::revise_runtime_candidates() noexcept
{
    if (status_.gain_group == Status::GAIN_INNER) {
        return transaction_.revise_float(dima::params::RO_SPEED_P, gains_[0]) &&
            transaction_.revise_float(dima::params::RO_SPEED_I, gains_[1]) &&
            transaction_.revise_float(dima::params::RO_YAW_RATE_P, gains_[2]) &&
            transaction_.revise_float(dima::params::RO_YAW_RATE_I, gains_[3]) &&
            transaction_.revise_float(dima::params::RO_YAW_RATE_LIM, tuning_config_.rate_limit / kRadians) &&
            transaction_.revise_float(dima::params::RO_MAX_THR_SPEED, status_.maximum_speed_m_s) &&
            transaction_.revise_float(dima::params::RO_YAW_RATE_CORR, status_.yaw_rate_correction);
    }
    if (status_.gain_group == Status::GAIN_HEADING)
        return transaction_.revise_float(dima::params::RO_YAW_P, status_.heading_p) &&
            transaction_.revise_float(dima::params::RD_TRANS_TRN_DRV, tuning_config_.driving.turn_to_drive_yaw_error_rad) &&
            transaction_.revise_float(dima::params::RD_TRANS_DRV_TRN, tuning_config_.driving.drive_to_turn_yaw_error_rad);
    if (status_.gain_group == Status::GAIN_NAVIGATION)
        return transaction_.revise_float(dima::params::RD_TRANS_TRN_DRV, tuning_config_.driving.turn_to_drive_yaw_error_rad) &&
            transaction_.revise_float(dima::params::RD_TRANS_DRV_TRN, tuning_config_.driving.drive_to_turn_yaw_error_rad);
    return transaction_.revise_float(dima::params::PP_LOOKAHD_GAIN, status_.lookahead_gain) &&
        transaction_.revise_float(dima::params::RO_JERK_LIM, tuning_config_.jerk) &&
        transaction_.revise_float(dima::params::RO_SPEED_RED, tuning_config_.speed_reduction);
}

} // namespace dima::rover::modes
