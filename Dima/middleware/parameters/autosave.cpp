/****************************************************************************
 *
 *   Copyright (c) 2023 PX4 Development Team. All rights reserved.
 *
 ****************************************************************************/
#define MODULE_NAME "param"
#include "autosave.h"

#include "events/events.hpp"
#include "logging/logging.hpp"
#include "param.h"
#include "param_internal.hpp"

#include <cerrno>

namespace {
constexpr uint32_t kDebounceUs = 300000U;
constexpr hrt_abstime kRateLimitUs = 2000000ULL;
constexpr uint32_t kWriteBlockedRetryUs = 1000000U;
constexpr uint32_t kStorageFullEventId = 0x50415201U;

void reportStorageFull() noexcept
{
    const uint32_t arguments[] = {static_cast<uint32_t>(ENOSPC)};
    (void)dima::events::report(kStorageFullEventId,
                               dima::events::Severity::Critical,
                               arguments, 1U);
}
} // namespace

ParamAutosave::ParamAutosave(
    dima::platform::ArmedFlashCoordinator &armed_flash) noexcept
    : ScheduledWorkItem("param-autosave", px4::wq_configurations::storage),
      _armed_flash(armed_flash)
{
}

void ParamAutosave::request(bool force) noexcept
{
    // 沿用 PX4：首次改参合并 300 ms，两笔保存开始时刻至少间隔 2 s。
    // 只合并一次请求，连续 PARAM_SET 不逐项触发 Flash 写入。
    px4::AtomicTransaction transaction;
    _force_save = _force_save || force;
    if (_scheduled.load() ||
        _disable_reason != DisableReason::None) {
        return;
    }

    hrt_abstime delay = kDebounceUs;
    if (_last_attempt_timestamp != 0U) {
        const hrt_abstime elapsed = hrt_elapsed_time(&_last_attempt_timestamp);
        if (elapsed < kRateLimitUs && kRateLimitUs > elapsed + delay) {
            delay = kRateLimitUs - elapsed;
        }
    }

    _scheduled.store(true);
    if (!ScheduleDelayed(static_cast<uint32_t>(delay))) {
        _scheduled.store(false);
    }
}

void ParamAutosave::enable() noexcept
{
    px4::AtomicTransaction transaction;
    _disable_reason = DisableReason::None;
    if (!ScheduleEnable()) {
        _disable_reason = DisableReason::Manual;
        return;
    }
    _retry_count = 0;
}

bool ParamAutosave::resume_after_storage_available() noexcept
{
    {
        px4::AtomicTransaction transaction;
        // 最终保存已修复介质时，旧的强制修复请求也不能再触发第二次写入。
        _force_save = false;
        if (_disable_reason != DisableReason::StorageFull) {
            return false;
        }
        if (!ScheduleEnable()) {
            return false;
        }
        _disable_reason = DisableReason::None;
        _retry_count = 0;
    }
    request();
    return pending();
}

void ParamAutosave::stop() noexcept
{
    {
        px4::AtomicTransaction transaction;
        _disable_reason = DisableReason::Manual;
        _scheduled.store(false);
    }
    ScheduleCancelAndDrain();
    px4::AtomicTransaction transaction;
    _last_attempt_timestamp = 0U;
    _last_success_timestamp = 0U;
    _retry_count = 0;
}

bool ParamAutosave::enabled() const noexcept
{
    px4::AtomicTransaction transaction;
    return _disable_reason == DisableReason::None;
}

hrt_abstime ParamAutosave::lastAutosave() const noexcept
{
    px4::AtomicTransaction transaction;
    return _last_success_timestamp;
}

void ParamAutosave::Run()
{
    {
        px4::AtomicTransaction transaction;
        if (_disable_reason != DisableReason::None) {
            _scheduled.store(false);
            return;
        }
        if (!writeAllowed()) {
            if (!ScheduleDelayed(kWriteBlockedRetryUs)) {
                _scheduled.store(false);
            }
            return;
        }
        // 最终显式保存已清空 dirty 位时，丢弃会话期间积压的 autosave；介质修复可强制写。
        if (!_force_save && dima::parameters::internal::g_unsaved.count() == 0U) {
            _scheduled.store(false);
            return;
        }
        _scheduled.store(false);
        _last_attempt_timestamp = hrt_absolute_time();
    }

    // 对齐 PX4 的单次后台保存：只在开始前清 scheduled，保存期间的新改参仍可
    // 请求下一笔；没有按 Flash 字或校验步骤重新调度造成的固定 10 ms 空等。
    const int result = param_save_default(false);
    if (result == 0) { px4::AtomicTransaction transaction; _force_save = false; }
    bool retry = false;
    bool exhausted = false;
    bool storage_full = false;
    bool storage_protected = false;
    bool snapshot_stale = false;
    {
        px4::AtomicTransaction transaction;
        if (result == 0) {
            _last_success_timestamp = hrt_absolute_time();
            _retry_count = 0;
        } else if (result == -ENOSPC) {
            // 满区挂起：后台 autosave 永不触发整区擦除——擦除发生在任意时刻
            // 会与用户操作竞争并短暂阻塞解锁。重建只在直接保存（自动校准的
            // COMMIT_LEVEL/事务 finalize）中进行，SD 先提交同代快照后擦除，
            // 成功后由 storage_save 经 resume_after_storage_available 唤醒。
            _disable_reason = DisableReason::StorageFull;
            _retry_count = 0;
            storage_full = true;
        } else if (result == -ENOTEMPTY) {
            // FlashFS 已验证存在其他 token 的有效记录并拒绝整区擦除。
            // 时间重试/SD 挂载无法解除保护；锁存暂停，使后台 resync/轮询
            // 不能无限重开三次重试。保留 unsaved，绝不把保存失败伪装成成功。
            _disable_reason = DisableReason::StorageProtected;
            _retry_count = 0;
            storage_protected = true;
        } else if (result == -EPERM) {
            _retry_count = 0;
            _scheduled.store(true);
            if (!ScheduleDelayed(kWriteBlockedRetryUs)) {
                _scheduled.store(false);
            }
        } else if (result == -ESTALE) {
            _retry_count = 0;
            retry = true;
            snapshot_stale = true;
        } else if (_retry_count < 3) {
            ++_retry_count;
            retry = true;
        } else {
            _retry_count = 0;
            exhausted = true;
        }
    }

    if (storage_full) {
        reportStorageFull();
        PX4_ERR("parameter storage full (%i), autosave suspended; recovery via explicit save", result);
    } else if (storage_protected) {
        PX4_ERR("param Flash contains other records (%i); autosave suspended", result);
    } else if (snapshot_stale) {
        PX4_INFO("parameters changed during save; scheduling fresh snapshot");
        request();
    } else if (retry) {
        PX4_INFO("param auto save unavailable (%i), retrying..", result);
        request();
    } else if (exhausted) {
        PX4_ERR("param auto save failed (%i)", result);
    }
}

// Upstream path: src/lib/parameters/autosave.cpp @ d6f12ad1


// 普通运行期实现从对应头文件移出；保持原状态、错误分支和计算顺序。

bool ParamAutosave::pending() const noexcept
{ return _scheduled.load(); }

bool ParamAutosave::writeAllowed() const noexcept
{ return _armed_flash.configuration_allowed() && !param_storage_paused(); }
