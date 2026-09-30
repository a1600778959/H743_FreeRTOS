#define MODULE_NAME "auto_cal"
#include "AutoCalibrationMode.hpp"

#include <cmath>
#include <limits>

namespace dima::rover::modes {
namespace math = dima::lib::rover::calibration;

// —— 测量/安全门谓词（调度器所有；从 AutoCalibrationMode.cpp 原样迁入）—————
// fresh/safety_fresh/imu_quality/rtk_quality/rtk_yaw_fused/stopped/
// ground_speed/yaw_rate/motion_quality_failure；
// 已删除仅用于固定加速度门的滤波器，公共质量/停止判据保留。合同正文见
// Dima/rover/modes/README.md。

bool AutoCalibrationMode::fresh(std::uint64_t timestamp, std::uint64_t now,
    std::uint64_t limit) noexcept
{
    return timestamp != 0U && timestamp <= now && now - timestamp <= limit;
}

bool AutoCalibrationMode::safety_fresh(std::uint64_t now) const noexcept
{
    const auto &status = vehicle_status_sub_.get();
    const auto &control = control_sub_.get();
    const auto &armed = armed_sub_.get();
    // Commander 负责 RC、Kill、Failsafe、参数和执行器故障的最终处置；这里
    // 只观察三份状态是否新鲜、车型和 Armed 投影是否一致。三 Topic 不要求
    // 发布时刻完全相等，RoverDifferential 会在输出端等待同代快照，避免低优先
    // 级协调器把正常的跨队列更新窗口误报成 control loss。
    return fresh(status.timestamp, now, 750000ULL) &&
        fresh(control.timestamp, now, 750000ULL) &&
        fresh(armed.timestamp, now, 750000ULL) &&
        status.vehicle_type == vehicle_status_s::VEHICLE_TYPE_ROVER &&
        armed.armed == control.flag_armed && armed.armed == armed_.armed();
}

bool AutoCalibrationMode::imu_quality(std::uint64_t now) const noexcept
{
    const auto &imu = imu_sub_.get();
    const auto &att = attitude_sub_.get();
    // EKF/VehicleImu 已负责姿态归一化、设备选择和削波处理；模式层只保留
    // 本实验真正需要的新鲜姿态/IMU样本及有限值检查，避免不可能条件制造假失败。
    return fresh(imu.timestamp_sample, now, 100000ULL) && fresh(att.timestamp_sample, now, 100000ULL) &&
        std::isfinite(yaw_rate()) && std::isfinite(att.q[0]) && std::isfinite(att.q[1]) &&
        std::isfinite(att.q[2]) && std::isfinite(att.q[3]);
}

bool AutoCalibrationMode::rtk_quality(std::uint64_t now) const noexcept
{
    const auto &gps = gps_sub_.get();
    const auto &rtk = rtk_sub_.get();
    // RTK 固定解且测速有效时信任速度，不再用速度标准差上限拒绝低速车辆。
    // 标准差仍供停车余量等计算使用，只检查其为有限、非负的有效数值。
    return fresh(gps.timestamp_sample, now, 300000ULL) && fresh(rtk.timestamp_sample, now, 300000ULL) &&
        gps.device_id != 0U && gps.device_id == rtk.device_id &&
        (status_.fence_device_id == 0U || gps.device_id == status_.fence_device_id) &&
        gps.fix_type == sensor_gps_s::FIX_TYPE_RTK_FIXED && rtk.solution_computed && rtk.integer_fixed &&
        rtk.velocity_aligned &&
        std::isfinite(rtk.array_heading_rad) && std::isfinite(rtk.baseline_m) &&
        rtk.baseline_m >= 0.1F && rtk.baseline_m <= 10.0F &&
        std::isfinite(rtk.heading_accuracy_rad) &&
        rtk.heading_accuracy_rad <= kRadians && std::isfinite(ground_speed()) &&
        std::isfinite(rtk.speed_accuracy_m_s) && rtk.speed_accuracy_m_s >= 0.0F &&
        std::isfinite(gps.eph) && gps.eph <= 0.15F;
}

bool AutoCalibrationMode::rtk_yaw_fused(std::uint64_t now) const noexcept
{
    const auto &flags = flags_sub_.get();
    const auto &aid = yaw_aid_sub_.get();
    // 由EKF在融合时刻的innovation/test_ratio判断航向一致性。删除最新姿态
    // 与延迟RTK样本之间的5度直接比较；保留独立RTK质量、融合成功/故障和鲜度。
    return rtk_quality(now) && rtk_sub_.get().baseline_consistent &&
        imu_quality(now) && flags.cs_yaw_align && flags.cs_tilt_align && !flags.cs_mag_hdg && !flags.cs_mag_3d &&
        fresh(flags.timestamp, now, 1500000ULL) && flags.cs_gnss_yaw && !flags.cs_gnss_yaw_fault &&
        fresh(aid.timestamp, now, 1500000ULL) && fresh(aid.time_last_fuse, now, 500000ULL) &&
        aid.fused && !aid.innovation_rejected && std::isfinite(aid.test_ratio) && aid.test_ratio < 1.0F;
}

float AutoCalibrationMode::ground_speed() const noexcept
{
    return std::hypot(rtk_sub_.get().velocity_north_m_s, rtk_sub_.get().velocity_east_m_s);
}

std::uint64_t AutoCalibrationMode::velocity_epoch_us() const noexcept
{
    // AGRICA速度历元在唯一边界从GPS周/毫秒换算为微秒；采样去重与制动dt共用。
    const auto &rtk = rtk_sub_.get();
    return (static_cast<std::uint64_t>(rtk.velocity_gps_week) * 604800000ULL + rtk.velocity_gps_milliseconds) * 1000ULL;
}

float AutoCalibrationMode::control_interval_s(std::uint64_t now) const noexcept
{
    // 控制调度时间统一换算为秒；各动作保留自己的斜率积分上限，不改变测量采样周期。
    return last_run_ != 0U && now > last_run_ ? 1.0e-6F * static_cast<float>(now - last_run_) : 1.0e-6F * kIntervalUs;
}

float AutoCalibrationMode::yaw_rate() const noexcept
{
    const auto &imu = imu_sub_.get();
    return imu.delta_angle_dt == 0U ? std::numeric_limits<float>::quiet_NaN()
        : imu.delta_angle[2] / (1.0e-6F * imu.delta_angle_dt);
}

bool AutoCalibrationMode::stopped() const noexcept
{
    // 位置静止滑窗是独立于GNSS速度解的物理证据：速度解在急停后振铃~1s，
    // 停稳门不能被测量伪影阻塞；滑窗维护见 update_inputs()。杆臂旋转
    // （ω·r·0.5s 可小于位移阈值）会让滑窗单独误报静止，必须与 IMU 角
    // 速度（不受 GNSS 振铃污染）联合判定。
    return (position_quiet_ && std::fabs(yaw_rate()) < 0.05F) ||
        (ground_speed() < kStoppedSpeedMps && std::fabs(yaw_rate()) < 0.05F);
}

bool AutoCalibrationMode::motion_quality_failure(std::uint64_t now) const noexcept
{
    const auto &rtk = rtk_sub_.get();
    const bool baseline_bad = !std::isfinite(status_.rtk_baseline_m) ||
        !std::isfinite(rtk.baseline_m) ||
        std::fabs(rtk.baseline_m - status_.rtk_baseline_m) >
            std::fmax(0.02F, 0.05F * status_.rtk_baseline_m);
    // 融合检查已包含 IMU/RTK 质量；RTK 提交前才使用原始质量检查。
    const bool quality = (status_.provisional_validated_stages & Status::STAGE_RTK) != 0U
        ? rtk_yaw_fused(now) : imu_quality(now) && rtk_quality(now);
    return !quality || baseline_bad;
}

} // namespace dima::rover::modes
