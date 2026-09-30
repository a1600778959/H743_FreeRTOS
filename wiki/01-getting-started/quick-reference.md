---
title: 快速参考
description: 日常高频使用的命令、路径与文档入口
---

# 快速参考

## 命令速查

| 场景 | 命令 |
|------|------|
| 改代码后验收 | `make verify` |
| 只编译 | `make firmware` |
| 改了模块边界/构建规则 | `make check-architecture` |
| 静默直通构建 | `DIMA_PROGRESS=off make firmware` |
| 关闭缓存调试 | `DIMA_CCACHE=off make firmware` |
| 生成 IDE 数据库 | `make intellisense` |
| OTA 上传 | `make upload` |

## 目录速查

| 路径 | 内容 |
|------|------|
| `Dima/rover/` | 控制域（control/ + modes/） |
| `Dima/modules/` | motor、safety、mavlink、logging、sensors、parameters、boot_health、ekf2、rc |
| `Dima/middleware/` | parameters、logging、rover 契约 |
| `Dima/lib/rover/` | 校准/控制纯算法 |
| `Dima/platform/api/` | capability 契约（唯一允许上层依赖的硬件接口层） |
| `Boards/H743/` | 板级 + 组合根 |
| `docs/` | 权威文档（架构、校准方案、资源基线、ADR） |
| `tools/` | Python 工具链（架构检查、ELF 验证、参数生成） |

## 文档路由（权威入口）

| 要找什么 | 去哪 |
|----------|------|
| 依赖方向/禁止事项 | `docs/ARCHITECTURE_ZH.md` |
| 模块职责与调度约束 | 各模块 `README.md` |
| 自动校准方案与状态机 | `docs/AUTO_CALIBRATION_PLAN_ZH.md` / `AUTO_CALIBRATION_STATE_MACHINE_ZH.md` |
| 上车操作流程 | `docs/H743_VEHICLE_COMMISSIONING_ZH.md` |
| 来源与上游 commit | `docs/DIMA_SOURCE_MANIFEST.md` |
| 架构决策记录 | `docs/adr/`（0001-0006） |

## 关键 uORB Topic

| Topic | 生产者 | 消费者 |
|-------|--------|--------|
| `actuator_motors` | RoverDifferential | MotorOutput |
| `vehicle_status` / `armed` | Commander | 全仓库安全判定 |
| `auto_calibration_status` / `_request` | 校准模式 / Commander | 校准模式、BootHealth、MotorOutput |
| `sensor_gps` / `rtk_heading_status` | Um982Gps | EKF2、校准、MAVLink |
| `actuator_output_status` | MotorOutput | Commander、BootHealth、校准 |

## 排障速查

| 症状 | 第一检查点 |
|------|-----------|
| 构建失败于 ARCH 检查 | 最近是否 include 了禁止的头（HAL/CMSIS 进了上层） |
| USB 不枚举 | MotorOutput 租约失败是否 pending（已有 defer 250ms 保护） |
| 日志下载卡 0 字节 | 空闲回收是否清了列表状态（已有 Data 请求独立化修复） |
| 校准满区失败 | 参数分区是否还有遗留 token（DNA 已迁 SD，见 ADR 0006） |

## Related Pages

| Page | Relationship |
|------|-------------|
| [构建与验证](./build-and-verify.md) | make 体系详解 |
| [模块全景](../05-modules/modules.md) | Topic 生产/消费关系全集 |
