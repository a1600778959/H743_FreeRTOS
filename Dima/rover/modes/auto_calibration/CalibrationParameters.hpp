#pragma once

#include "api/Flash.hpp"
#include "parameter_update.hpp"
#include "parameters/param.h"
#include "uORB/SubscriptionData.hpp"

#include <cstddef>
#include <cstdint>

namespace dima::rover::modes {

// 固定容量的阶段事务；参数标识由调用方使用生成的 dima::params 指定。
// 生命周期是 Candidate -> Applying -> Provisional|Done；失败经 Rollback，
// 双重失败锁存 Fault 并保持 Arm 互锁。阶段事务只在 RAM 应用/恢复候选，
// 不写 Flash；掉电持久化只属于 FINALIZE 的 session_save/session_rollback。
// 无运行期动态分配。
class CalibrationParameters final {
public:
    enum class Phase : std::uint8_t { Idle, Applying, Provisional, Rollback, Done, Failed, Fault };
    explicit CalibrationParameters(dima::platform::ArmedFlashCoordinator &armed) noexcept;
    bool prepare() noexcept;
    void fault() noexcept;
    bool begin_session() noexcept;
    bool capture(dima::params parameter) noexcept;
    bool session_save(std::uint32_t &expected_set_count, bool commit) noexcept;
    bool session_rollback(std::uint32_t &expected_set_count) noexcept;
    bool session_active() const noexcept;
    bool session_committed() const noexcept;
    bool add_float(dima::params parameter, float value) noexcept;
    bool add_int(dima::params parameter, std::int32_t value) noexcept;
    bool apply(std::uint64_t now, std::uint32_t expected_set_count, bool provisional = false) noexcept;
    bool finalize_provisional(std::uint64_t now, std::uint32_t expected_set_count) noexcept;
    bool revise_float(dima::params parameter, float value) noexcept;
    bool apply_revisions(std::uint64_t now, std::uint32_t expected_set_count, bool provisional = true) noexcept;
    void poll(bool frontend_confirmed, bool validated, std::uint64_t now) noexcept;
    void cancel(std::uint64_t now) noexcept;
    Phase phase() const noexcept;
    bool active() const noexcept;
    bool rolling_back() const noexcept;
    bool generation_valid() const noexcept;
    std::uint32_t generation() const noexcept;
    std::uint64_t applied_at() const noexcept;
    std::uint32_t set_count_snapshot() const noexcept;
    float expected_float(dima::params parameter) const noexcept;
    std::int32_t expected_int(dima::params parameter) const noexcept;

private:
    // 会话回滚只需最初值；不为每个会话槽重复保存阶段候选/修订状态。
    struct Snapshot {
        param_t handle{PARAM_INVALID}; param_type_t type{}; param_value_u old{};
    };
    struct Entry : Snapshot {
        param_value_u next{}; float revised{}; bool revision_pending{};
    };
    bool add(dima::params parameter, param_value_u value, param_type_t type) noexcept;
    bool matches(bool original) const noexcept;
    bool write(bool original) noexcept;
    bool apply_candidate(std::uint64_t now, bool provisional) noexcept;
    void notify(std::uint64_t now) noexcept;
    void release() noexcept;

    dima::platform::ArmedFlashCoordinator &armed_;
    uORB::SubscriptionData<parameter_update_s> update_{ORB_ID(parameter_update)};
    // 关联整定保留同一份最初快照；容量有界，不在运行期扩容或建立第二份注册表。
    Entry entries_[32]{};
    std::size_t count_{0U};
    Phase phase_{Phase::Idle};
    std::uint64_t deadline_{0U};
    std::uint64_t applied_at_{0U};
    std::uint32_t generation_{0U};
    std::uint32_t set_count_{0U};
    bool generation_valid_{false};
    bool held_{false};
    bool rollback_{false};
    bool cancel_pending_{false};
    bool provisional_{false};
    bool session_active_{false};
    bool session_committed_{false};
    // 会话日志覆盖整场所有实验组的最初快照：入口 2（板级）+ 减速度 1 +
    // RTK 3 + 动力学 2 + 磁 3 + 观测级联 5 + IMU 6 + 运行/增益组新增 15，
    // 最坏 37 个不同参数；40 槽留余量，写入侧满槽仍按失败处理而非截断。
    Snapshot session_entries_[40]{};
    std::size_t session_count_{0U};
};

} // namespace dima::rover::modes
