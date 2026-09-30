#define MODULE_NAME "auto_cal"
#include "AutoCalibrationMode.hpp"
#include "logging/logging.hpp"

#include <climits>

namespace dima::rover::modes {

// —— 参数事务操作 ——————————————————————————————————————————————————
// 本文件只提供事务候选的建立（prepare/add/apply）：事务身份（kind/stages）
// 由apply_transaction()在写入前登记，前端确认与推进由
// transaction_frontend_confirmed()/poll_transaction() 按 TransactionKind
// 路由。这里不再有 commit/apply/restore 状态分支，也不读写 status_.state。

bool AutoCalibrationMode::begin_rtk_transaction(std::uint64_t now) noexcept
{
    // 事务操作：基线/偏置/GPS 控制候选一次性应用。
    return transaction_.prepare() &&
        transaction_.add_float(dima::params::GPS_YAW_BASELINE, status_.rtk_baseline_m) &&
        transaction_.add_float(dima::params::GPS_YAW_OFFSET, status_.rtk_yaw_offset_deg) &&
        transaction_.add_int(dima::params::EKF2_GPS_CTRL, config_.gps_control | (1 << 3)) &&
        apply_transaction(TransactionKind::Rtk, now);
}

bool AutoCalibrationMode::begin_dynamics_transaction(std::uint64_t now) noexcept
{
    // 事务操作：最大速度/角速度修正。最大速度出现在 yaw 前馈分母，
    // 两参数必须作为同一事务应用/回滚。
    return transaction_.prepare() &&
        transaction_.add_float(dima::params::RO_MAX_THR_SPEED, status_.maximum_speed_m_s) &&
        transaction_.add_float(dima::params::RO_YAW_RATE_CORR, status_.yaw_rate_correction) &&
        apply_transaction(TransactionKind::Dynamics, now, false, Status::STAGE_SPEED | Status::STAGE_YAW);
}

bool AutoCalibrationMode::begin_mag_transaction(std::uint64_t now, bool restore) noexcept
{
    // 事务操作：磁提交 / refine / 恢复。restore 撤销 bootstrap 的 provisional
    // 候选并确认回到旧值；false=撤销未成立（调用方保持互锁重试）。
    px4::AtomicTransaction atomic;
    if (restore) {
        transaction_.cancel(now);
        // cancel() 失败时可能仍停在 Provisional；不能把这种状态当成已进入
        // 回滚，否则事务机不会再推进恢复而会一直占用互锁。
        return transaction_.phase() == CalibrationParameters::Phase::Rollback;
    }
    if (transaction_.phase() == CalibrationParameters::Phase::Provisional) {
        // 磁偏置修订复用参数事务的候选槽与统一写入；确认后直接完成本组。
        return transaction_.revise_float(dima::params::CAL_MAG0_XOFF, candidate_mag_[0]) &&
            transaction_.revise_float(dima::params::CAL_MAG0_YOFF, candidate_mag_[1]) &&
            transaction_.revise_float(dima::params::CAL_MAG0_ZOFF, candidate_mag_[2]) &&
            apply_transaction(TransactionKind::Magnetic, now);
    }
    if (mag_device_id_ == 0U || mag_device_id_ > static_cast<std::uint32_t>(INT32_MAX)) return false;
    // WMM/EKF 参考法只观测硬铁 offset；磁设备 ID、scale 和 rotation 继续
    // 使用已有硬件配置，自动流程不借一次 yaw 圆锥激励改写不可观测量。
    // bootstrap 应用（!mag_ready_）保持 provisional，最终保存统一进 FINALIZE。
    return transaction_.prepare() &&
        transaction_.add_float(dima::params::CAL_MAG0_XOFF, candidate_mag_[0]) &&
        transaction_.add_float(dima::params::CAL_MAG0_YOFF, candidate_mag_[1]) &&
        transaction_.add_float(dima::params::CAL_MAG0_ZOFF, candidate_mag_[2]) &&
        apply_transaction(TransactionKind::Magnetic, now, !mag_ready_);
}

} // namespace dima::rover::modes
