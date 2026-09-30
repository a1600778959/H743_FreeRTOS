# ADR 0006：DNA 分配表迁出参数 FlashFS 分区，改存 SD 原子文件域

日期：2026-09-18
状态：源码实施；板端验收待执行

## 问题

参数 FlashFS 分区（0x081E0000，单个 128 KiB 扇区，追加写）同时承载两个 token：参数快照（'parm'）与 DroneCAN 动态节点分配表（'dna0'，每条约 2.2 KiB）。FlashFS 的整区擦除带独占校验：只要分区里存在其他 token 的有效记录即拒绝（-ENOTEMPTY）。两个 token 互相锁死，分区一旦写满便**永久无法回收**——2026-09-18 的 LOG100 中两次自动校准失败于 COMMIT_LEVEL 的持久化超时，根因即此（叠加 autosave 满区挂起与擦除链路静默失败）。

## 决策

1. `AtomicFileDomain` 新增第三域 DroneCan（`0:/dima/dnacan.{bin,bak,tmp}` 三文件轮换），分配表改存该域；DroneCanMag2 不再注入 FlashFS，参数分区恢复 'parm' 单 token 独占，满区时"SD 提交同代快照 → 整区擦除重建"重新可用。
2. 分配语义不变：已知 uid 直接复用原 node id 不写介质；新 uid 先保存成功再对外承诺绑定；加载短读/格式不符 fail-closed。
3. 无卡降级：load 将 -ENODEV 映射为合法空表（Mission 域先例）；保存遇 -ENODEV 且 `allow_volatile_fallback` 时先按重试节奏等待宽限窗（15 s，覆盖开机 SD 慢就绪的挂载竞态），仍无介质才按 RAM-only 完成提交并仍发有界 StorageFailure 事件——绑定不跨上电，由下次上电的确定性重分配恢复。仅 -ENODEV 降级，I/O 等真实故障保持重试。换卡/无卡启动后首次保存先重新发现 primary/backup/tmp（驱动自有缓冲承载读回）。
4. 一次性迁移：组合根在参数服务初始化后调用 `FlashFS::invalidate_records('dna0')`，把遗留记录的 commit 字编程为全零（只清位不擦除、不动追加高水位）。掉电中断后下次上电幂等续跑；全部失效后为空操作，仅在确有失效/失败时输出日志。
5. 独占擦除门槛本身不放宽：多 token 联合重建需要跨模块事务协调，收益不抵复杂度；存储布局单主人化是更直接的修复。

## 选择理由与边界

- SD 原子文件域已有成熟基建（三文件轮换、回读校验、介质会话失效处理），参数与 Mission 域为先例；迁移只改适配层接线。
- 代价：无 SD 的上电会话分配表为空、绑定易失。单设备总线上集中式分配是确定性的（preferred/max 向下选择），通常仍获得同一 node id；多 DNA 客户端并发且无卡的场景不保证跨上电稳定，本产品当前只有一个 DroneCAN 外设（RM3100 磁力计）。
- 遗留 'dna0' 记录占用空间直到下一次满区擦除才物理回收；软失效只解除独占校验阻塞。

## 验收

- 本机构建：`make dima_rover` / `check-architecture` / `verify` 全绿。
- 板端（BOARD PENDING）：含遗留 'dna0' 记录的分区开机出现 invalidation 日志；满区分区经自动校准 COMMIT_LEVEL 完成擦除重建（"rebuilding inside level commit" 后成功推进）；无卡开机 DNA 正常分配磁力计且出现易失降级事件；插卡后新绑定正常持久化。
