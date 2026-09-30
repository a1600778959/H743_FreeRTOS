#define MODULE_NAME "auto_cal"
#include "AutoCalibrationMode.hpp"
#include "imu/VehicleImu.hpp"

#include <algorithm>
#include <cmath>
#include <climits>

namespace dima::rover::modes {
namespace {

bool finite_bias(const float (&bias)[3], const float (&variance)[3]) noexcept
{
    // 收敛由EKF自身 valid/stable 判据负责；模式不另设3σ、轴间方差比或改善比例。
    for (unsigned i = 0U; i < 3U; ++i)
        if (!std::isfinite(bias[i]) || !std::isfinite(variance[i]) || variance[i] < 0.0F) return false;
    return true;
}

} // namespace

// —— IMU 阶段 helper ————————————————————————————————————————————————
// EKF 偏置只做 offset-only 事务：候选经 begin_imu_bias 建立（provisional
// 应用），前端确认与残差重锁判据供调度器的 TransactionKind::Imu 事务机
// 路由；应用成功后的PROFILE交接在本入口统一执行。

bool AutoCalibrationMode::begin_imu_bias(std::uint64_t now) noexcept
{
    // 入场前已完成六面校准并绑定设备；这里只修正 offset，不改 ID/scale/rotation。
    // 返回 false=本会话不发起
    //（已尝试过、证据不足或准备失败）；事务身份在应用入口写入前登记。
    if (imu_bias_attempted_) return false;
    imu_bias_attempted_ = true;
    const auto &bias = bias_sub_.get();
    std::int32_t enabled{};
    float accel[3]{}, gyro[3]{}, matrix[9]{}, active_accel[6]{};
    px4::AtomicTransaction atomic;
    // offset使用同设备稳定EKF偏置和传感器旋转，不依赖GNSS航向融合状态。
    if (!maintenance_ready() || !stopped() || !imu_quality(now) || !rtk_quality(now) ||
        param_get(param_handle(dima::params::SENS_IMU_AUTOCAL), &enabled) != 0 || enabled == 0 ||
        !fresh(bias.timestamp, now, 1000000ULL) || bias.timestamp_sample <= session_started_ ||
        !fresh(bias.timestamp_sample, now, 1500000ULL) ||
        !imu_frontend_.calibration_snapshot(imu_sub_.get().accel_device_id, imu_sub_.get().gyro_device_id,
            active_accel, gyro, matrix)) return false;
    const bool accel_valid = bias.accel_bias_valid && bias.accel_bias_stable &&
        bias.accel_device_id == imu_sub_.get().accel_device_id && bias.accel_device_id != 0U && bias.accel_device_id <= INT32_MAX &&
        finite_bias(bias.accel_bias, bias.accel_bias_variance);
    const bool gyro_valid = bias.gyro_bias_valid && bias.gyro_bias_stable &&
        bias.gyro_device_id == imu_sub_.get().gyro_device_id && bias.gyro_device_id != 0U && bias.gyro_device_id <= INT32_MAX &&
        finite_bias(bias.gyro_bias, bias.gyro_bias_variance);
    // IMU bias 是一个共同代次；不能只提交 accel 或 gyro 后留下未标记的
    // 部分校准值。任一轴缺少稳定证据时整组跳过/回滚。
    if (!accel_valid || !gyro_valid) return false;
    float stored_scale[3]{};
    if (param_get(param_handle(dima::params::CAL_ACC0_XSCALE), &stored_scale[0]) != 0 ||
        param_get(param_handle(dima::params::CAL_ACC0_YSCALE), &stored_scale[1]) != 0 ||
        param_get(param_handle(dima::params::CAL_ACC0_ZSCALE), &stored_scale[2]) != 0) return false;
    // 本事务始终同时处理加速度计和陀螺仪；scale沿用已应用值，不保留单项分支。
    for (unsigned i = 0U; i < 3U; ++i)
        if (stored_scale[i] != active_accel[i + 3U]) return false;
    for (unsigned i = 0U; i < 3U; ++i) {
        accel[i] = active_accel[i]; imu_bias_scale_[i] = active_accel[i + 3U];
        if (!std::isfinite(imu_bias_scale_[i]) || imu_bias_scale_[i] < 0.5F || imu_bias_scale_[i] > 1.5F) return false;
        // 使用已有 EKF stable/variance 判据，不另造估计器。机体系残余偏置按
        // R^T 回到传感器系：accel offset += R^T*bias/scale，gyro 没有 scale。
        float da = 0.0F, dg = 0.0F;
        for (unsigned j = 0U; j < 3U; ++j) {
            da += matrix[j * 3U + i] * bias.accel_bias[j];
            dg += matrix[j * 3U + i] * bias.gyro_bias[j];
        }
        accel[i] += da / imu_bias_scale_[i]; gyro[i] += dg;
        if (!std::isfinite(accel[i]) || !std::isfinite(gyro[i])) return false;
    }
    if (!transaction_.prepare()) return false;
    const bool added =
        // 自动流程只提交 offset；设备 ID、scale 和 rotation 继续由已有硬件
        // 身份合同管理，不能因 EKF bias 收敛被校准流程接管。
        transaction_.add_float(dima::params::CAL_ACC0_XOFF, accel[0]) &&
        transaction_.add_float(dima::params::CAL_ACC0_YOFF, accel[1]) &&
        transaction_.add_float(dima::params::CAL_ACC0_ZOFF, accel[2]) &&
        transaction_.add_float(dima::params::CAL_GYRO0_XOFF, gyro[0]) && transaction_.add_float(dima::params::CAL_GYRO0_YOFF, gyro[1]) &&
        transaction_.add_float(dima::params::CAL_GYRO0_ZOFF, gyro[2]);
    // provisional 应用：残差重锁窗确认后才允许进入统一保存。
    if (!added || !apply_transaction(TransactionKind::Imu, now, true)) return false;
    imu_bias_finalizing_ = false;
    // 首次入口和失败后独立组调度共用事务交接，只有应用成功才推进阶段。
    if (status_.state != Status::STATE_PROFILE) enter_phase(Status::STATE_PROFILE, now, PhaseSubstate::Evaluate);
    else enter_substate(PhaseSubstate::Evaluate, now);
    return true;
}

bool AutoCalibrationMode::imu_bias_confirmed(std::uint64_t now) const noexcept
{
    // 前端确认：IMU 消费者已按本事务代次应用候选 offset。
    if (!transaction_.generation_valid() || !fresh(imu_sub_.get().timestamp_sample, now, 100000ULL) ||
        imu_sub_.get().timestamp_sample <= transaction_.applied_at()) return false;
    const float accel[6]{transaction_.expected_float(dima::params::CAL_ACC0_XOFF),
        transaction_.expected_float(dima::params::CAL_ACC0_YOFF), transaction_.expected_float(dima::params::CAL_ACC0_ZOFF),
        imu_bias_scale_[0], imu_bias_scale_[1], imu_bias_scale_[2]};
    const float gyro[3]{transaction_.expected_float(dima::params::CAL_GYRO0_XOFF),
        transaction_.expected_float(dima::params::CAL_GYRO0_YOFF), transaction_.expected_float(dima::params::CAL_GYRO0_ZOFF)};
    return imu_frontend_.accel_calibration_matches(transaction_.generation(),
            static_cast<std::int32_t>(imu_sub_.get().accel_device_id), accel) &&
        imu_frontend_.gyro_calibration_matches(transaction_.generation(),
            static_cast<std::int32_t>(imu_sub_.get().gyro_device_id), gyro);
}

bool AutoCalibrationMode::imu_bias_residual_valid(std::uint64_t now) const noexcept
{
    // 前端应用确认后使用EKF新样本的有效/稳定结果，不另造模式层改善比例。
    const auto &bias = bias_sub_.get();
    if (!imu_quality(now) || !rtk_quality(now) || !stopped() ||
        !fresh(bias.timestamp, now, 1000000ULL) || !fresh(bias.timestamp_sample, now, 1500000ULL) ||
        bias.timestamp_sample <= transaction_.applied_at()) return false;
    // 重融合后的验证也要求 EKF 自身的 stable 标志和新鲜采样，不能靠重复发布
    // 同一份旧 bias 将等待时长累计成三秒的有效残差证据。
    return bias.accel_bias_valid && bias.accel_bias_stable && bias.accel_device_id == imu_sub_.get().accel_device_id &&
        finite_bias(bias.accel_bias, bias.accel_bias_variance) &&
        bias.gyro_bias_valid && bias.gyro_bias_stable && bias.gyro_device_id == imu_sub_.get().gyro_device_id &&
        finite_bias(bias.gyro_bias, bias.gyro_bias_variance);
}

} // namespace dima::rover::modes
