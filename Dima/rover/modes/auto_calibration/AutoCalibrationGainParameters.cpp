#define MODULE_NAME "auto_cal"
#include "AutoCalibrationMode.hpp"
#include "auto/AutoMode.hpp"
#include "control/RoverDifferential.hpp"
#include "logging/logging.hpp"

#include <algorithm>
#include <cmath>

namespace dima::rover::modes {

// —— 增益 cohort 事务操作（Gains 共用同一份 provisional cohort）——
// 本文件只剩事务操作与前端确认：cohort 的轮询、组推进与位结算统一由
// poll_active_transaction()/advance_cohort_validation() 及统一 FINALIZE 承担。
// 事务身份用 session_.kind（Gains）表达，
// 不再用 status_.state 判断事务类型。

bool AutoCalibrationMode::begin_gain_transaction(std::uint64_t now, std::uint8_t group) noexcept
{
    if (!maintenance_ready() || !runtime_cohort_ || transaction_.phase() != CalibrationParameters::Phase::Provisional) return false;
    status_.gain_group = group;
    status_.closed_loop = false;
    validation_passed_ = false;
    if (group == Status::GAIN_INNER) {
        // 四个内环增益共享一次快照、参数通知与回滚，不能只使速度环有效而
        // 留下不完整的 yaw-rate 控制配置。参数标识全部来自生成的 dima::params。
        status_.speed_p = gains_[0]; status_.speed_i = gains_[1];
        status_.yaw_rate_p = gains_[2]; status_.yaw_rate_i = gains_[3];
    }
    // 修订入既有 cohort 并重新应用；Provisional 确认、验证入口与失败回滚
    // 由调度器的 Evaluate 子状态事务机处理。
    return revise_runtime_candidates() && apply_transaction(TransactionKind::Gains, now, true);
}

bool AutoCalibrationMode::gain_frontend_confirmed() const noexcept
{
    if (!runtime_frontend_confirmed()) return false;
    const float expected[4]{transaction_.expected_float(dima::params::RO_SPEED_P),
        transaction_.expected_float(dima::params::RO_SPEED_I), transaction_.expected_float(dima::params::RO_YAW_RATE_P),
        transaction_.expected_float(dima::params::RO_YAW_RATE_I)};
    return drive_.calibration_gains_applied(transaction_.generation(), expected);
}

bool AutoCalibrationMode::gain_controller_ready() const noexcept
{
    // 内环、航向与转驱试验均直接复用差速层SI控制及本模式配置过的Heading/
    // Driving算法，不消费路径jerk。只有路径试验要求完整导航配置；所有组
    // 的参数代次/前端应用确认仍由gain_frontend_confirmed独立保留。
    return status_.gain_group == Status::GAIN_PATH
        ? navigation_.calibration_configuration_ready(transaction_.generation(), nav_.trial_speed, nav_.maximum_speed)
        : drive_.calibration_control_ready(transaction_.generation());
}

} // namespace dima::rover::modes
