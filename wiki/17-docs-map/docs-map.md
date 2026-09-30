---
title: 权威文档导读
description: docs/ 下 27 篇文档的分类地图与阅读时机
---

# 权威文档导读

`docs/` 是方案与审计的沉淀地。**冲突规则**：当旧 README、方案文档与现实有差异时，先按目标固件版本核对权威输入与当前消费者，不把旧方案当作已实现行为（[部署指南 §15](https://github.com/a1600778959/H743_FreeRTOS/blob/main/docs/H743_VEHICLE_COMMISSIONING_ZH.md#L402)）。

## 方案与规格（设计时读）

| 文档 | 内容 | 何时读 |
|------|------|--------|
| [AUTO_CALIBRATION_PLAN_ZH](https://github.com/a1600778959/H743_FreeRTOS/blob/main/docs/AUTO_CALIBRATION_PLAN_ZH.md) | 自动校准权威方案 | 改校准前 |
| [AUTO_CALIBRATION_STATE_MACHINE_ZH](https://github.com/a1600778959/H743_FreeRTOS/blob/main/docs/AUTO_CALIBRATION_STATE_MACHINE_ZH.md) | 状态机细节（11 状态↔旧能力映射） | 改校准状态机前 |
| [AUTO_CALIBRATION_OFFICIAL_TUNING_PLAN_ZH](https://github.com/a1600778959/H743_FreeRTOS/blob/main/docs/AUTO_CALIBRATION_OFFICIAL_TUNING_PLAN_ZH.md) | PX4 官方三项导航参数标定法（PP/jerk/RED） | 改导航实验前 |
| [AUTO_CALIBRATION_FORMULA_REMOVAL_ZH](https://github.com/a1600778959/H743_FreeRTOS/blob/main/docs/AUTO_CALIBRATION_FORMULA_REMOVAL_ZH.md) | 问题代码删减清单 + Skogestad PI 公式来源 | 动校准数学前（防回潮） |
| [DIMA_ROVER_PORTING_PLAN_ZH](https://github.com/a1600778959/H743_FreeRTOS/blob/main/docs/DIMA_ROVER_PORTING_PLAN_ZH.md) | PX4→Dima 移植计划 | 追溯移植决策 |
| [PX4_GENERATION_PIPELINE_REFACTOR_SPEC_ZH](https://github.com/a1600778959/H743_FreeRTOS/blob/main/docs/PX4_GENERATION_PIPELINE_REFACTOR_SPEC_ZH.md) | 生成管线重构规格 | 动生成链前 |
| [PX4_GPS_IMU_DIAGNOSTICS_SPEC_ZH](https://github.com/a1600778959/H743_FreeRTOS/blob/main/docs/PX4_GPS_IMU_DIAGNOSTICS_SPEC_ZH.md) | GPS/IMU 诊断规格 | 动传感器诊断前 |

## 审计与教训（防回潮读）

| 文档 | 内容 |
|------|------|
| [MANUAL_DRIVE_DIRECTION_AUDIT_ZH](https://github.com/a1600778959/H743_FreeRTOS/blob/main/docs/MANUAL_DRIVE_DIRECTION_AUDIT_ZH.md) | 手动方向语义审计（侧别/REV/四处分叉合并） |
| [IMU_TELEMETRY_STALL_ZH](https://github.com/a1600778959/H743_FreeRTOS/blob/main/docs/IMU_TELEMETRY_STALL_ZH.md) | IMU 遥测停摆事件复盘 |
| [SD_LOG_SPACE_DIAGNOSIS_20260910_ZH](https://github.com/a1600778959/H743_FreeRTOS/blob/main/docs/SD_LOG_SPACE_DIAGNOSIS_20260910_ZH.md) | SD 日志空间策略诊断 |
| [SDMMC_IDMA_CPU_ACCEPTANCE_ZH](https://github.com/a1600778959/H743_FreeRTOS/blob/main/docs/SDMMC_IDMA_CPU_ACCEPTANCE_ZH.md) | SDMMC IDMA 的 CPU 占用验收 |
| [PARAMETER_SIMPLIFICATION_ZH](https://github.com/a1600778959/H743_FreeRTOS/blob/main/docs/PARAMETER_SIMPLIFICATION_ZH.md) | 参数精简记录 |
| [HEADER_IMPLEMENTATION_SPLIT_ZH](https://github.com/a1600778959/H743_FreeRTOS/blob/main/docs/HEADER_IMPLEMENTATION_SPLIT_ZH.md) | 头/实现分离约定 |

## 资源与基线（预算时读）

| 文档 | 内容 |
|------|------|
| [DIMA_RESOURCE_BASELINE_ZH](https://github.com/a1600778959/H743_FreeRTOS/blob/main/docs/DIMA_RESOURCE_BASELINE_ZH.md) | 构建产物/镜像版本总览 |
| [DIMA_PHASE3/4/5_RESOURCE_BASELINE_ZH](https://github.com/a1600778959/H743_FreeRTOS/blob/main/docs/DIMA_PHASE3_RESOURCE_BASELINE_ZH.md) | 各阶段 Flash/RAM 增量验收 |
| [DTCM_MIGRATION_ZH](https://github.com/a1600778959/H743_FreeRTOS/blob/main/docs/DTCM_MIGRATION_ZH.md) | DTCM 迁移记录 |
| [CODE_SIZE_REDUCTION / _FOUR_OPTIMIZATIONS / _NEXT_OPTIMIZATIONS](https://github.com/a1600778959/H743_FreeRTOS/blob/main/docs/CODE_SIZE_REDUCTION_ZH.md) | Flash 瘦身三战役 |
| [BUILD_SPEED_OPTIMIZATION_ZH](https://github.com/a1600778959/H743_FreeRTOS/blob/main/docs/BUILD_SPEED_OPTIMIZATION_ZH.md) | 构建提速（进度计划化/ccache） |

## 操作与恢复（现场读）

| 文档 | 内容 |
|------|------|
| [H743_VEHICLE_COMMISSIONING_ZH](https://github.com/a1600778959/H743_FreeRTOS/blob/main/docs/H743_VEHICLE_COMMISSIONING_ZH.md) | 上车部署与操作指南（wiki 11 分区的权威源） |
| [MCUBOOT_USB_RECOVERY_ZH](https://github.com/a1600778959/H743_FreeRTOS/blob/main/docs/MCUBOOT_USB_RECOVERY_ZH.md) | USB DFU 恢复手册 |
| [MAVLINK_UART_ZH](https://github.com/a1600778959/H743_FreeRTOS/blob/main/docs/MAVLINK_UART_ZH.md) | MAVLink 双链路说明 |
| [DIMA_SOURCE_MANIFEST](https://github.com/a1600778959/H743_FreeRTOS/blob/main/docs/DIMA_SOURCE_MANIFEST.md) | 上游来源/commit/许可证 |
| [ARCHITECTURE_ZH](https://github.com/a1600778959/H743_FreeRTOS/blob/main/docs/ARCHITECTURE_ZH.md) | 架构与依赖规则（AGENTS.md 的展开） |
| [adr/](./adr-index.md) | 6 篇架构决策记录（另有索引页） |

## Related Pages

| Page | Relationship |
|------|-------------|
| [快速参考](../01-getting-started/quick-reference.md) | 文档路由速查 |
| [ADR 索引](./adr-index.md) | 决策记录 |
| [首次运行总流程](../11-first-run/first-run-flow.md) | 操作类文档的使用场景 |
