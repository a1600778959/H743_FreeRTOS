#define MODULE_NAME "auto_cal"
#include "AutoCalibrationMode.hpp"

#include "logging/logging.hpp"

namespace dima::rover::modes {

// 组依赖用于失败会计；成功链由阶段调度器推进，回滚后只尝试独立静态组。

std::uint32_t AutoCalibrationMode::group_dependents(std::uint8_t bit) noexcept
{
    // 表为拓扑序：向后单遍扫描，直接或经链式依赖引用失败组的后继全部
    // 计入；返回值包含失败组自身位。
    std::uint32_t dependents = group_stage_mask(bit);
    for (const auto &step : kGroupSteps) {
        const std::uint32_t mask = group_stage_mask(step.bit);
        if ((mask & dependents) != 0U) continue;
        if ((step.depends & dependents) != 0U) dependents |= mask;
    }
    return dependents;
}

bool AutoCalibrationMode::group_entry_available(unsigned row) const noexcept
{
    // 入场门禁：依赖组必须未置 unavailable 且已完成。命中 unavailable 是
    // 依赖失败；从未完成是依赖未闭合，两者都不允许本组假装可独立完成。
    const std::uint32_t depends = kGroupSteps[row].depends;
    return (depends & status_.unavailable_stages) == 0U &&
        (depends & ~status_.provisional_validated_stages) == 0U;
}

void AutoCalibrationMode::skip_group(unsigned row, const char *cause) noexcept
{
    const auto &step = kGroupSteps[row];
    const std::uint32_t mask = group_stage_mask(step.bit);
    // skipped 与 unavailable 分开记录：本组从未尝试，不冒充失败组；结果
    // 会计（finish 拆分）据此把有因跳过移出失败分母。
    status_.skipped_stages |= mask & ~(status_.provisional_validated_stages | status_.unavailable_stages);
    PX4_WARN("[autocal] group bit=%u skipped: %s (deps=0x%lx validated_ram=0x%lx unavailable=0x%lx)",
        static_cast<unsigned>(step.bit), cause, static_cast<unsigned long>(step.depends),
        static_cast<unsigned long>(status_.provisional_validated_stages),
        static_cast<unsigned long>(status_.unavailable_stages));
}

void AutoCalibrationMode::skip_tuning_chain() noexcept
{
    // 整定链入场依赖未闭合：链上组（INNER..NAV，含 RUNTIME）全部按“有因
    // 跳过”记录并收尾。SPEED/YAW 等依赖组自身的记账仍在既有前置流程，
    // MAG/IMU_BIAS 链外组不在此处理。
    for (unsigned row = kGroupRowInner; row <= kGroupRowNav; ++row) {
        const std::uint32_t mask = group_stage_mask(kGroupSteps[row].bit);
        if ((status_.provisional_validated_stages & mask) == mask) continue;
        skip_group(row, group_entry_available(row) ? "chain predecessor skipped"
            : "dependency unavailable");
    }
}

bool AutoCalibrationMode::advance_group_scheduler(unsigned row, std::uint64_t now) noexcept
{
    // 组失败回滚后的步进器：从失败行的下一行继续，按表序找下一个可独立
    // 入场的组；命中依赖的组按“有因跳过”记录后继续向后看。
    for (++row; row < kGroupStepCount; ++row) {
        const auto &step = kGroupSteps[row];
        const std::uint32_t mask = group_stage_mask(step.bit);
        if ((status_.provisional_validated_stages & mask) == mask) continue;
        if (!group_entry_available(row)) {
            // 只对尚未被失败级联标记的组记 skipped，避免同一组同时落入
            // unavailable 与 skipped 两个通道。
            if ((mask & (status_.provisional_validated_stages | status_.unavailable_stages)) == 0U)
                skip_group(row, "dependency unavailable");
            continue;
        }
        // 唯一调用方已确认回滚结束，事务不可能仍为 Provisional；
        // 不能重入同一增益候选。仅保留可独立尝试的 IMU 静态组。
        if (step.bit == kGroupBitImuBias && begin_imu_bias(now)) return true;
    }
    return false;
}

void AutoCalibrationMode::fail_tuning(std::uint8_t reason, std::uint64_t now) noexcept
{
    // 失败组与依赖级联由调度器表统一推导，gain_group 只负责映射到表位。
    // 已完成的独立组（Level、RTK、制动、磁等）保留，终态据此报告 PARTIAL。
    std::uint8_t bit = kGroupBitInner;
    std::uint32_t dependents = 0U;
    switch (status_.gain_group) {
    case Status::GAIN_INNER:
        bit = kGroupBitInner;
        dependents = group_dependents(kGroupBitInner);
        break;
    case Status::GAIN_HEADING:
        bit = kGroupBitHeading;
        dependents = group_dependents(kGroupBitHeading);
        break;
    case Status::GAIN_PATH:
        bit = kGroupBitPath;
        dependents = group_dependents(kGroupBitPath);
        break;
    case Status::GAIN_NAVIGATION:
        bit = kGroupBitNav;
        // 转驱（NAV_STRATEGY）与 PATH 同属 Navigation cohort：转驱验证失败后
        // 路径试验不会再运行。组表里 PATH 行位于 NAV 行之前，
        // group_dependents 的表序单向后向扫描推不出这层依赖，须对照
        // GAIN_NONE 分支并入 RUNTIME 的写法显式级联 STAGE_PATH_GAIN。
        dependents = group_dependents(kGroupBitNav) | Status::STAGE_PATH_GAIN;
        break;
    default:
        // GAIN_NONE：整定链入场失败，链上全部未完成组（含 RUNTIME）一并
        // 不可用。
        dependents = group_dependents(bit) | Status::STAGE_RUNTIME;
        break;
    }
    // 组失败的统一入口：先按依赖级联标记不可用（已完成组保留），实验类
    // 失败（验证不过/样本不足/目标不可观测/残差超标等）进入共享停车回滚，
    // 回滚确认后由步进器从失败行继续向后找独立组；公共安全失败（围栏/
    // 失鲜/后端 FAULT/事务/存储/参数代次）仍直接走会话级 terminate。
    // 依赖失败只登记一次；停车回滚期间不产生新的验证位，cohort失效由回滚出口处理。
    status_.unavailable_stages |= dependents & ~status_.provisional_validated_stages;
    if (status_.failure_reason == Status::FAILURE_NONE) status_.failure_reason = reason;
    const bool maneuver_failure = reason == Status::FAILURE_GAIN_VALIDATION ||
        reason == Status::FAILURE_PROFILE_UNOBSERVABLE ||
        reason == Status::FAILURE_IDENTIFICATION ||
        reason == Status::FAILURE_PATH_UNOBSERVABLE ||
        reason == Status::FAILURE_DYNAMICS_UNOBSERVABLE ||
        reason == Status::FAILURE_MOTION_UNAVAILABLE;
    if (maneuver_failure && session_.abort_reason == Status::FAILURE_NONE) {
        // 组级失败不直接伪装成传感器/后端会话故障：先经停车子状态等待
        // 真实停波，再回滚当前 provisional 组；此前已确认的独立组仍可
        // 报告 PARTIAL。
        // 非NONE原因同时表示待停车/回滚，不再维护与原因重复的pending标志。
        session_.abort_reason = reason;
        session_.abort_group = bit;
        validation_passed_ = false;
        exercise_running_ = false;
        physical_speed_ = physical_rate_ = longitudinal_ = steering_ = 0.0F;
        enter_substate(PhaseSubstate::WaitStop, now);   // 当前阶段内停车→回滚→组调度器继续
        PX4_WARN("[autocal] maneuver abort group bit=%u reason=%u; stopping before rollback",
            static_cast<unsigned>(bit), static_cast<unsigned>(reason));
        return;
    }
    // 公共安全失败由terminate统一撤销许可并请求退出，不在组调度器再维护一次。
    terminate(reason, false, now);
}

void AutoCalibrationMode::after_gain_validated(std::uint64_t now) noexcept
{
    // 关联验证完成只登记 RAM 证据；completed 位留到最终保存成功后设置。
    if (!read_tuning_config()) { fail_tuning(Status::FAILURE_PARAMETER, now); return; }
    // 导航组可能仅通过转驱而缺少 jerk/减速证据；验证后仍应报告该组部分
    // 未完成，不能同时把同一组置入已验证与 unavailable。INNER 的完整
    // 证据在两轴验证结束时登记，此处只消费结果，不从镜像重新恢复已失效位。
    status_.provisional_validated_stages &= ~status_.unavailable_stages;
    if ((status_.provisional_validated_stages & Status::STAGE_INNER_GAINS) == 0U)
        status_.unavailable_stages |= Status::STAGE_INNER_GAINS;
    if (runtime_fully_observed_) status_.provisional_validated_stages |= Status::STAGE_RUNTIME;
    else status_.unavailable_stages |= Status::STAGE_RUNTIME;
    // 能力范围外的电机整形/SLEW 不进入本次事务，也不写入 skipped_stages；
    // 它是退役组，不属于结果会计。
    if (status_.failure_reason == Status::FAILURE_DYNAMICS_UNOBSERVABLE ||
        status_.failure_reason == Status::FAILURE_MECHANICAL_ASYMMETRY) status_.failure_reason = Status::FAILURE_NONE;
    runtime_cohort_ = false;
    enter_finalize(now, false);
}

} // namespace dima::rover::modes
