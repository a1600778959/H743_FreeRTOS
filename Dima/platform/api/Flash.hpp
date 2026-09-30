#pragma once

#include "PlatformTypes.hpp"

namespace dima::platform {

class CriticalSection;

class FlashTransactionManager {
public:
    virtual ~FlashTransactionManager() = default;
    virtual bool acquire(Timeout timeout) noexcept = 0;
    virtual void release() noexcept = 0;
};

class FlashTransaction final {
public:
    FlashTransaction(FlashTransactionManager &manager,
                     Timeout timeout) noexcept;
    ~FlashTransaction();

    explicit operator bool() const noexcept;
    FlashTransaction(const FlashTransaction &) = delete;
    FlashTransaction &operator=(const FlashTransaction &) = delete;

private:
    FlashTransactionManager *manager_{nullptr};
    bool acquired_{false};
};

class FlashPartition {
public:
    virtual ~FlashPartition() = default;
    virtual std::uintptr_t base() const noexcept = 0;
    virtual std::size_t size() const noexcept = 0;
    virtual std::size_t program_size() const noexcept = 0;
    virtual bool read(std::size_t offset, void *destination,
                      std::size_t length) noexcept = 0;
    // 支持连续多个 program_size 单元；返回 true 表示编程和写后回读均成功。
    // 上层仍保留介质记录 CRC 和最后提交标记，不再重复执行同一次写后的回读。
    virtual bool program(std::size_t offset, const void *source,
                         std::size_t length) noexcept = 0;
    virtual bool erase() noexcept = 0;
};

class ArmedFlashCoordinator final {
public:
    explicit ArmedFlashCoordinator(CriticalSection &critical) noexcept;

    bool try_arm(bool calibration = false) noexcept;
    void disarm() noexcept;
    bool begin_flash() noexcept;
    void end_flash() noexcept;
    bool begin_maintenance(bool calibration = false) noexcept;
    void end_maintenance() noexcept;
    bool armed() const noexcept;
    bool arming_blocked() const noexcept;
    // 自动校准单独锁住物理输出，不伪造 Disarmed。停波确认只由 PWM owner 写入。
    bool set_calibration_output_inhibited(bool inhibit) noexcept;
    bool calibration_output_inhibited() const noexcept;
    std::uint64_t calibration_output_transition() const noexcept;
    void confirm_calibration_output_stopped(bool stopped) noexcept;
    bool calibration_output_stopped() const noexcept;
    bool configuration_allowed() const noexcept;
    bool calibration_maintenance_recent(std::uint64_t now) const noexcept;
    bool begin_configuration_update() noexcept;
    void end_configuration_update() noexcept;

    ArmedFlashCoordinator(const ArmedFlashCoordinator &) = delete;
    ArmedFlashCoordinator &operator=(const ArmedFlashCoordinator &) = delete;

private:
    CriticalSection &critical_;
    bool armed_{false};
    bool flash_busy_{false};
    bool maintenance_busy_{false};
    bool calibration_maintenance_{false};
    bool calibration_output_inhibited_{false};
    bool calibration_output_stopped_{false};
    std::uint32_t configuration_users_{0U};
    std::uint64_t calibration_maintenance_end_us_{0U};
    std::uint64_t calibration_output_transition_us_{0U};
};

// 参数消费者的短租约阻止校准恢复运动和普通 Arm，覆盖检查到应用之间的抢占窗口。
class ConfigurationUpdateLease final {
public:
    explicit ConfigurationUpdateLease(ArmedFlashCoordinator &coordinator) noexcept;
    ~ConfigurationUpdateLease();
    explicit operator bool() const noexcept;
    ConfigurationUpdateLease(const ConfigurationUpdateLease &) = delete;
    ConfigurationUpdateLease &operator=(const ConfigurationUpdateLease &) = delete;
private:
    ArmedFlashCoordinator &coordinator_;
    bool acquired_;
};

class FlashWriteLease final {
public:
    explicit FlashWriteLease(ArmedFlashCoordinator &coordinator) noexcept;
    ~FlashWriteLease();

    explicit operator bool() const noexcept;
    FlashWriteLease(const FlashWriteLease &) = delete;
    FlashWriteLease &operator=(const FlashWriteLease &) = delete;

private:
    ArmedFlashCoordinator *coordinator_{nullptr};
    bool acquired_{false};
};

} // namespace dima::platform
