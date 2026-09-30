/****************************************************************************
 *
 *   Copyright (c) 2023 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

#pragma once

#include "param.h"

#include "containers/atomic.h"
#include "api/Flash.hpp"
#include "api/Time.hpp"
#include "work_queue/ScheduledWorkItem.hpp"

#include <cstdint>

class ParamAutosave : public px4::ScheduledWorkItem
{
public:
    /* request 合并待保存标记，独立低优先级 storage WorkQueue 连续保存整份快照；
     * armed 时延期，MAVLink 回显和实时控制不等待介质写入。 */
    explicit ParamAutosave(
        dima::platform::ArmedFlashCoordinator &armed_flash) noexcept;
    void request(bool force = false) noexcept;
    void enable() noexcept;
    bool resume_after_storage_available() noexcept;
    void stop() noexcept;
    bool enabled() const noexcept;
    bool pending() const noexcept;
    hrt_abstime lastAutosave() const noexcept;

private:
    bool _force_save{false};
    enum class DisableReason : std::uint8_t {
        /* Manual 需显式 enable。StorageFull 满区挂起：后台 autosave 永不
         * 触发整区擦除；重建只在直接保存（自动校准提交/事务 finalize）中
         * 进行，成功后由 storage_save 唤醒本挂起。
         * StorageProtected 表示 Flash 内其他有效记录阻止擦除，换 SD 或
         * 周期 request 不能解决；修复存储后显式重新启用服务才恢复。 */
        None,
        Manual,
        StorageFull,
        StorageProtected,
    };

    void Run() override;
    bool writeAllowed() const noexcept;

    dima::platform::ArmedFlashCoordinator &_armed_flash;
    hrt_abstime _last_attempt_timestamp{0};
    hrt_abstime _last_success_timestamp{0};
    px4::atomic_bool _scheduled{false};
    int _retry_count{0};
    DisableReason _disable_reason{DisableReason::None};
};

// Upstream path: src/lib/parameters/autosave.h @ d6f12ad1
