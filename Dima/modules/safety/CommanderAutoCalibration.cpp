#define MODULE_NAME "commander"
#include "Commander.hpp"
#include "logging/logging.hpp"
#include "api/Time.hpp"

namespace dima::modules::safety {

bool Commander::auto_calibration_fresh(std::uint64_t now) const noexcept
{
    // 参数保存会占用低优先级协调器；只在已物理停波且实际持有校准维护锁时，
    // 接纳最长 20 s 的同一状态。运动阶段仍是 200 ms；锁不释放则 PWM 永不恢复。
    const std::uint64_t limit = armed_flash_.calibration_maintenance_recent(now) ? 20000000ULL : 200000ULL;
    return auto_calibration_status_.timestamp != 0U && auto_calibration_status_.timestamp <= now &&
        now - auto_calibration_status_.timestamp <= limit;
}

bool Commander::start_auto_calibration(std::uint64_t now) noexcept
{
    // 必须以新的显式模式动作在 Disarmed 进入；默认预留槽/启动恢复不会调用。
    // 选择模式不隐式 Arm；进入后立即接受正常人工授权，静态与提交阶段
    // 由独立停波锁约束电机，整个会话不因内部阶段改变 Armed。
    if (actuator_armed_.armed || !auto_calibration_fresh(now) ||
        !auto_calibration_status_.ready_for_entry || auto_calibration_status_.active ||
        !parameters_valid_ || actuator_armed_.kill || termination_latched_ || vehicle_status_.failsafe ||
        vehicle_status_.rc_calibration_in_progress || vehicle_status_.calibration_enabled || maintenance_.in_progress()) {
        PX4_WARN("Auto calibration rejected: disarmed readiness required");
        return false;
    }
    // 进入静态会话不是 Arm：无 RC/输出后端时仍可 Level；真正运动继续由
    // preflight_checks_pass 唯一核对 RC、Neutral 和完整安全条件。
    return change_navigation_state(vehicle_status_s::NAVIGATION_STATE_EXTERNAL1, now);
}

bool Commander::process_auto_calibration(std::uint64_t now) noexcept
{
    for (unsigned i = 0U; i < 8U && auto_calibration_sub_.update(); ++i)
        auto_calibration_status_ = auto_calibration_sub_.get();
    bool changed = false;
    auto_calibration_request_s request{};
    for (unsigned i = 0U; i < 4U && auto_calibration_request_sub_.copy(&request); ++i) {
        for (unsigned j = 0U; j < 8U && auto_calibration_sub_.update(); ++j)
            auto_calibration_status_ = auto_calibration_sub_.get();
        now = hrt_absolute_time();
        if (!auto_calibration_fresh(now) || request.session_id == 0U ||
            request.session_id != auto_calibration_status_.session_id || request.timestamp == 0U ||
            request.timestamp > now || now - request.timestamp > 200000ULL) continue;
        if (request.request == auto_calibration_request_s::REQUEST_EXIT) {
            if (vehicle_status_.nav_state == vehicle_status_s::NAVIGATION_STATE_EXTERNAL1) {
                // 责任分类按会话 failure_reason：真实会话故障（非 NONE 且非操作员
                // 取消）记 failure-detector，避免把围栏、失鲜、后端或事务故障伪装
                // 成正常退出；正常收尾/用户取消记内部命令。
                const auto reason =
                    auto_calibration_status_.failure_reason != auto_calibration_status_s::FAILURE_NONE &&
                    auto_calibration_status_.failure_reason != auto_calibration_status_s::FAILURE_OPERATOR_CANCEL
                    ? vehicle_status_s::ARM_DISARM_REASON_FAILURE_DETECTOR
                    : vehicle_status_s::ARM_DISARM_REASON_COMMAND_INTERNAL;
                if (auto_calibration_status_.active) {
                    // FINALIZE 握手（enter_finalize 的 EXIT，仍 active）：只解除
                    // Armed，不切回 Manual。若此刻切模式，协调器下一拍判“未选中”
                    // 会把收尾误标为 OPERATOR_CANCEL 并把已做的保存决策翻成回滚。
                    // 切回 Manual 推迟到 finish()（active=false）后的终态 EXIT；
                    // 除模式切换外与 Commander::disarm() 逐字段一致。
                    if (actuator_armed_.armed) {
                        actuator_armed_.armed = false;
                        vehicle_status_.arming_state = vehicle_status_s::ARMING_STATE_DISARMED;
                        vehicle_status_.latest_disarming_reason = reason;
                        vehicle_status_.armed_time = 0U;
                        vehicle_status_.takeoff_time = 0U;
                        armed_flash_.disarm();
                        PX4_INFO("Rover disarmed (auto-calibration finalize handshake)");
                        changed = true;
                    }
                } else {
                    // finish() 后的终态 EXIT：会话已闭合，解除 Armed 并切回 Manual；
                    // 协调器此时已不 active，“未选中”不会再触发取消路径。
                    changed = disarm(reason, now) == TransitionResult::Changed || changed;
                }
            }
        } else if (request.request == auto_calibration_request_s::REQUEST_LEVEL) {
            // 内部 Level 保持 Armed，但必须已取得独立物理停波证明；压平后
            // Level 事务只发生在 STATE_PREFLIGHT 阶段（请求与结果都在本阶段处理）。
            if (!armed_flash_.calibration_output_stopped() || vehicle_status_.nav_state != vehicle_status_s::NAVIGATION_STATE_EXTERNAL1 ||
                vehicle_status_.calibration_enabled || !auto_calibration_status_.active ||
                auto_calibration_status_.state != auto_calibration_status_s::STATE_PREFLIGHT) continue;
            sensor_calibration_request_s worker{};
            worker.timestamp = request.timestamp;
            worker.request = sensor_calibration_request_s::REQUEST_LEVEL;
            worker.feedback_owner = sensor_calibration_request_s::FEEDBACK_AUTO;
            worker.parameter_set_count = request.parameter_set_count;
            if (sensor_calibration_request_publication_.publish(worker)) {
                vehicle_status_.calibration_enabled = true;
                sensor_calibration_dispatch_time_ = now;
                auto_level_request_timestamp_ = request.timestamp;
                changed = true;
            }
        } else if (request.request == auto_calibration_request_s::REQUEST_CANCEL_LEVEL &&
                   auto_level_request_timestamp_ != 0U && sensor_calibration_status_.active &&
                   sensor_calibration_status_.request_timestamp == auto_level_request_timestamp_) {
            sensor_calibration_request_s worker{};
            worker.timestamp = now;
            worker.request = sensor_calibration_request_s::REQUEST_CANCEL;
            worker.feedback_owner = sensor_calibration_request_s::FEEDBACK_AUTO;
            (void)sensor_calibration_request_publication_.publish(worker);
        }
    }
    return changed;
}

bool Commander::evaluate_auto_calibration(std::uint64_t now) noexcept
{
    // 进入/Arm 的跨队列投影给一个固定短窗口；物理输出仍由下游完整同拍快照
    // 与请求 TTL 门控。窗口结束后协调器丢失或否定许可立即 Disarm。
    const bool entering = now >= vehicle_status_.nav_state_timestamp && now - vehicle_status_.nav_state_timestamp <= 250000ULL;
    const bool arming = actuator_armed_.armed && now >= vehicle_status_.armed_time && now - vehicle_status_.armed_time <= 250000ULL;
    const auto output_transition_at = armed_flash_.calibration_output_transition();
    const bool output_transition = auto_calibration_fresh(now) && auto_calibration_status_.active &&
        auto_calibration_status_.result == auto_calibration_status_s::RESULT_RUNNING &&
        output_transition_at != 0U && now >= output_transition_at && now - output_transition_at <= 250000ULL;
    // 恢复输出的原子锁先于下一帧状态发布；仅该真实边沿给固定交接窗口。
    if (entering || arming || output_transition) return false;
    const bool healthy = auto_calibration_fresh(now) && auto_calibration_status_.active &&
        auto_calibration_status_.result == auto_calibration_status_s::RESULT_RUNNING &&
        (!actuator_armed_.armed || auto_calibration_status_.motion_allowed ||
         auto_calibration_control_inhibit_expected(now));
    if (healthy) return false;
    return disarm(vehicle_status_s::ARM_DISARM_REASON_FAILURE_DETECTOR, now) == TransitionResult::Changed;
}

bool Commander::auto_calibration_control_inhibit_expected(std::uint64_t now) const noexcept
{
    // 只接纳同一模式的健康协调器主动停波；失鲜、终态或旧会话不构成例外。
    return vehicle_status_.nav_state == vehicle_status_s::NAVIGATION_STATE_EXTERNAL1 &&
        auto_calibration_fresh(now) && auto_calibration_status_.active &&
        auto_calibration_status_.result == auto_calibration_status_s::RESULT_RUNNING &&
        auto_calibration_status_.motion_inhibited && !auto_calibration_status_.motion_allowed &&
        armed_flash_.calibration_output_inhibited();
}

bool Commander::auto_calibration_output_stopped(std::uint64_t now) const noexcept
{
    // 解锁和维持 Armed 都要求后端证据，不能仅用模式的零命令/布尔许可代替。
    if (!auto_calibration_control_inhibit_expected(now) || !armed_flash_.calibration_output_stopped() ||
        !actuator_output_status_fresh(now) || !actuator_output_mapping_valid() ||
        !actuator_output_status_.backend_ready || !actuator_output_status_.drive_available ||
        actuator_output_status_.state != actuator_output_status_s::STATE_CONTROL_INHIBITED ||
        !actuator_output_status_.safe_off || actuator_output_status_.command_valid ||
        actuator_output_status_.active_output_mask != 0U) return false;
    for (const auto pulse : actuator_output_status_.pwm_us) if (pulse != 0U) return false;
    return true;
}

} // namespace dima::modules::safety
