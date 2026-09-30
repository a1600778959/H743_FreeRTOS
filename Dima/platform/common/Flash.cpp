#include "api/Flash.hpp"

#include "api/Execution.hpp"

namespace dima::platform {

FlashTransaction::FlashTransaction(FlashTransactionManager &manager,
                                   Timeout timeout) noexcept
    : manager_(&manager), acquired_(manager.acquire(timeout))
{
}

FlashTransaction::~FlashTransaction()
{
    if (acquired_) {
        manager_->release();
    }
}

ArmedFlashCoordinator::ArmedFlashCoordinator(
    CriticalSection &critical) noexcept
    : critical_(critical)
{
}

bool ArmedFlashCoordinator::try_arm(bool calibration) noexcept
{
    // 普通 Armed 与 Flash/维护互斥；自动校准只在锁存且后端确认停波时例外。
    // 临界区仅保护状态转移，不包围实际 Flash 操作，不屏蔽长时间中断。
    CriticalGuard guard{critical_};
    if ((flash_busy_ || maintenance_busy_ || configuration_users_ != 0U) &&
        !(calibration && calibration_output_inhibited_ && calibration_output_stopped_ &&
          configuration_users_ == 0U &&
          (!(flash_busy_ || maintenance_busy_) || calibration_maintenance_))) {
        return false;
    }
    armed_ = true;
    return true;
}

void ArmedFlashCoordinator::disarm() noexcept
{
    CriticalGuard guard{critical_};
    armed_ = false;
}

bool ArmedFlashCoordinator::begin_flash() noexcept
{
    // Armed 写入必须已经由校准维护 lease 持有物理停波证明；普通写入仍拒绝。
    // Flash lease 不能单独凭一个校准标志取得例外，也不能和另一个写事务重叠。
    CriticalGuard guard{critical_};
    if ((armed_ && !(calibration_maintenance_ && calibration_output_inhibited_ && calibration_output_stopped_)) || flash_busy_) {
        return false;
    }
    flash_busy_ = true;
    return true;
}

void ArmedFlashCoordinator::end_flash() noexcept
{
    CriticalGuard guard{critical_};
    flash_busy_ = false;
}

bool ArmedFlashCoordinator::begin_maintenance(bool calibration) noexcept
{
    CriticalGuard guard{critical_};
    if ((armed_ && !(calibration && calibration_output_inhibited_ && calibration_output_stopped_)) || flash_busy_ || maintenance_busy_) {
        return false;
    }
    maintenance_busy_ = true;
    calibration_maintenance_ = calibration;
    return true;
}

void ArmedFlashCoordinator::end_maintenance() noexcept
{
    CriticalGuard guard{critical_};
    if (calibration_maintenance_) calibration_maintenance_end_us_ = platform_time_us();
    maintenance_busy_ = false;
    calibration_maintenance_ = false;
}

bool ArmedFlashCoordinator::arming_blocked() const noexcept
{
    // 只读就绪投影；真正解锁仍须 try_arm 原子仲裁，不能用本查询代替取得锁。
    CriticalGuard guard{critical_};
    return (flash_busy_ || maintenance_busy_ || configuration_users_ != 0U) &&
        !(calibration_output_inhibited_ && calibration_output_stopped_ &&
          configuration_users_ == 0U &&
          (!(flash_busy_ || maintenance_busy_) || calibration_maintenance_));
}

bool ArmedFlashCoordinator::set_calibration_output_inhibited(bool inhibit) noexcept
{
    // 与维护/写入共用短原子门：任何事务未释放时均不允许恢复 PWM。
    // 重新申请时清掉上次确认，必须等 PWM owner 对本次锁存重新确认停波。
    CriticalGuard guard{critical_};
    if (!inhibit && (flash_busy_ || maintenance_busy_ || configuration_users_ != 0U)) return false;
    if (inhibit != calibration_output_inhibited_) {
        calibration_output_stopped_ = false;
        calibration_maintenance_end_us_ = 0U;
        calibration_output_transition_us_ = platform_time_us();
    }
    calibration_output_inhibited_ = inhibit;
    return true;
}

bool ArmedFlashCoordinator::calibration_output_inhibited() const noexcept
{
    CriticalGuard guard{critical_};
    return calibration_output_inhibited_;
}

std::uint64_t ArmedFlashCoordinator::calibration_output_transition() const noexcept
{
    CriticalGuard guard{critical_};
    return calibration_output_transition_us_;
}

void ArmedFlashCoordinator::confirm_calibration_output_stopped(bool stopped) noexcept
{
    CriticalGuard guard{critical_};
    calibration_output_stopped_ = calibration_output_inhibited_ && stopped;
}

bool ArmedFlashCoordinator::calibration_output_stopped() const noexcept
{
    CriticalGuard guard{critical_};
    return calibration_output_inhibited_ && calibration_output_stopped_;
}

bool ArmedFlashCoordinator::calibration_maintenance_recent(std::uint64_t now) const noexcept
{
    // 同步保存释放 lease 后，低优先级协调器还需一次调度才能刷新状态。
    // 交接宽限固定 200 ms，不能放开输出，也不能让旧状态无限续命。
    CriticalGuard guard{critical_};
    return calibration_output_inhibited_ && calibration_output_stopped_ &&
        ((calibration_maintenance_ && maintenance_busy_) ||
         (calibration_maintenance_end_us_ != 0U && now >= calibration_maintenance_end_us_ &&
          now - calibration_maintenance_end_us_ <= 200000ULL));
}

bool ArmedFlashCoordinator::configuration_allowed() const noexcept
{
    // Armed 只在已锁存且后端确认停波的校准窗口应用配置；所有普通行驶仍冻结。
    CriticalGuard guard{critical_};
    return !armed_ || (calibration_output_inhibited_ && calibration_output_stopped_);
}

bool ArmedFlashCoordinator::begin_configuration_update() noexcept
{
    CriticalGuard guard{critical_};
    if ((armed_ && !(calibration_output_inhibited_ && calibration_output_stopped_)) ||
        configuration_users_ == UINT32_MAX) return false;
    ++configuration_users_;
    return true;
}

void ArmedFlashCoordinator::end_configuration_update() noexcept
{
    CriticalGuard guard{critical_};
    if (configuration_users_ != 0U) --configuration_users_;
}

ConfigurationUpdateLease::ConfigurationUpdateLease(ArmedFlashCoordinator &coordinator) noexcept
    : coordinator_(coordinator), acquired_(coordinator.begin_configuration_update())
{}

ConfigurationUpdateLease::~ConfigurationUpdateLease()
{
    if (acquired_) coordinator_.end_configuration_update();
}

ConfigurationUpdateLease::operator bool() const noexcept
{ return acquired_; }

bool ArmedFlashCoordinator::armed() const noexcept
{
    CriticalGuard guard{critical_};
    return armed_;
}

FlashWriteLease::FlashWriteLease(
    ArmedFlashCoordinator &coordinator) noexcept
    /* RAII lease 确保所有返回路径都清除 flash_busy。 */
    : coordinator_(&coordinator), acquired_(coordinator.begin_flash())
{
}

FlashWriteLease::~FlashWriteLease()
{
    if (acquired_) {
        coordinator_->end_flash();
    }
}

} // namespace dima::platform


// 普通运行期实现从对应头文件移出；保持原状态、错误分支和计算顺序。

namespace dima::platform {

FlashTransaction::operator bool() const noexcept
{ return acquired_; }

FlashWriteLease::operator bool() const noexcept
{ return acquired_; }

} // namespace dima::platform
