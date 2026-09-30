---
title: 架构门禁与验证工具
description: tools/ 下的 Python 工具链：架构审计、ELF 验证、参数与日志生成
---

# 架构门禁与验证工具

## 工具清单

| 工具 | 职责 | Source |
|------|------|--------|
| `tools/check_architecture.py` | 架构门禁入口，`--profile fast/full` 两档 | [check_architecture.py:4](https://github.com/a1600778959/H743_FreeRTOS/blob/main/tools/check_architecture.py#L4) |
| `tools/architecture/dependency.py` | 依赖方向图检查 | [dependency.py](https://github.com/a1600778959/H743_FreeRTOS/blob/main/tools/architecture/dependency.py) |
| `tools/elf_support/layout.py` | ELF 布局/符号合同（DMA 区、向量、used 属性） | [layout.py](https://github.com/a1600778959/H743_FreeRTOS/blob/main/tools/elf_support/layout.py) |
| `tools/logging/generate_logger_contract.py` | logger_topics.yaml → 格式缓存容量推导 | [generate_logger_contract.py](https://github.com/a1600778959/H743_FreeRTOS/blob/main/tools/logging/generate_logger_contract.py) |
| `tools/calibration_suggest.py` | QGC 导出生成只读校准建议 | [calibration_suggest.py](https://github.com/a1600778959/H743_FreeRTOS/blob/main/tools/calibration_suggest.py) |
| `tools/upload/` | 上传工具解析与模型 | [upload/cli.py](https://github.com/a1600778959/H743_FreeRTOS/blob/main/tools/upload/cli.py) |
| `tools/build_support/` | 构建会话状态（jobs/ccache/会话哈希） | [build_support/session.py](https://github.com/a1600778959/H743_FreeRTOS/blob/main/tools/build_support/session.py) |

## 门禁两档

```mermaid
flowchart LR
  G{入口} --> V[verify / app-check]
  G --> CA[check-architecture]
  V -->|fast ~5s| F[关键 4 项]
  CA -->|full ~15s| A[全部扩展审计]
  F --> F1[依赖方向]
  F --> F2[include 所有权]
  F --> F3[硬件操作边界]
  A --> A1[构建隔离]
  A --> A2[目录/生成合同]
  A --> A3[命名空间]
  A --> A4[组合根唯一性]
```
<!-- Sources: tools/check_architecture.py:4, GNUmakefile:48 -->

fast 档内嵌在 `verify` 里每次都跑（~5 秒，504 个第一方源文件）；full 档只在显式请求时跑（~15 秒）。**修改模块边界或构建规则后必须显式跑 full**。

## 三层验证哲学

```mermaid
flowchart TB
  SRC[源码级<br>架构门禁] --> ELF[产物级<br>ELF 符号合同]
  ELF --> IMG[镜像级<br>签名/摘要/布局]
  SRC --> |依赖规则违规即失败| FAIL1[编译失败]
  ELF --> |lto_priv/used/向量| FAIL2[ELF 验证失败]
  IMG --> |摘要不匹配| FAIL3[拒绝启动]
```
<!-- Sources: tools/elf_support/layout.py, make/release.mk:57 -->

源码门禁防"写错"，ELF 验证防"链接错"（尤其 LTO 时代的符号内部化），镜像验证防"传输/烧写错"。三层各自独立，任何一层绿都不代表其他层可跳过。

## 参数与日志生成链

```mermaid
flowchart LR
  Y[definitions/*.yaml] --> GEN[参数生成器]
  GEN --> META[metadata 校验 stamp]
  LT[logger_topics.yaml] --> LC[logger contract 生成]
  LC --> CAPV[容量验证：49 Topics/301 参数/8 Profile]
```
<!-- Sources: tools/logging/generate_logger_contract.py, GNUmakefile:48 -->

参数定义是唯一事实源：改 YAML → 经正式 Make 入口生成 → metadata stamp 校验 → QGC Metadata 同版提供。**禁止手写参数/消息派生列表**。

## 已知边界

- Git Bash 直跑检查器会有 `make -pn` 伪违规 → 走 `make.cmd`。
- 架构白名单是显式登记制（如 `FatFsAtomicFileStore.cpp` 因 `get_fattime` 的全局 C ABI 例外）。
- 工具全部锁定在仓库内，随构建自动解析，无全局安装。

## Related Pages

| Page | Relationship |
|------|-------------|
| [构建与验证](../01-getting-started/build-and-verify.md) | 门禁在构建中的位置 |
| [构建、签名与 OTA](../09-build-release/build-release.md) | 产物级验证的延续 |
| [分层架构与依赖规则](../02-architecture/layered-architecture.md) | 被检查的规则本体 |
