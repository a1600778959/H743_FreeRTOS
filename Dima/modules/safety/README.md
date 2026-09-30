# Commander 安全与校准仲裁

Commander 只维护安全状态和 uORB 投影，不直接拥有传感器、串口或 PWM HAL。

## 校准仲裁

- Commander 是 `vehicle_command` 的唯一订阅者和 ACK 所有者。它按 PX4 的
  `MAV_CMD_PREFLIGHT_CALIBRATION` 规则分类请求，再通过由 schema 生成的
  `sensor_calibration_request` topic 分发到低优先级校准 worker。
- RC 校准仍由 Commander 直接处理：Disarmed 下 `param4=1` 进入，全零退出；
  gyro/mag/accel worker 只发布 `[cal]` 进度/终态，不再解析命令或发送第二个 ACK。
- `sensor_calibration_status.active` 投影为 `vehicle_status.calibration_enabled`。非手动模式在 RC 或外部 QGC 传感器校准期间仍拒绝 Arm；AUTO-owned Level 仅在独立停波锁确认后允许人工 Arm。Manual 的预检规则见下文，Disarm、Kill、Termination 始终保留。
- 传感器校准还持有 `ArmedFlashCoordinator` maintenance interlock，防止 Commander 在状态消息传播窗口中抢先解锁。
- Auto Calibration 在 Disarmed 显式进入后可以先运行静态 Level，不提前依赖 RC/PWM/双天线；进入后即可正常人工 Arm，实际 Armed 由 Commander 唯一维护，协调器只镜像状态。Level、RTK 基线采集、停车、参数应用及保存全程保持 Armed，运动条件只约束模式输出。任意真实 Disarm 都取消会话，不再产生阶段 Disarm/Arm 请求，也不保留独立的旧授权缓存。Arm 不是增益确认。
- 外部 Disarm 即使发生在尚未 Arm 的静态阶段，也会切回 Manual 取消会话；Kill、RC loss、模式切出、超时、安全/调度故障及重启保持原处理。阶段续行要求真实 Armed、匹配的会话与参数代次、消费者确认和运动就绪，不从失败或回滚窗口自动 Arm。
- 开/闭环校准使用共享 `RoverModeContract` 的精确安全投影，闭环仅开启 Velocity/Rates。参数 provisional 期间暂缓持久化，回滚无法确认会锁存 FAILED 与禁 Arm，不能重新进入运动。
- worker 反馈归属在接受请求时锁定；外部 QGC 保持标准 `[cal]` 协议，AUTO-owned Level/Cancel 不发送 `[cal]` start/progress/terminal，避免错误推进另一个 Sensors 界面事务。

## 既有安全合同

回调部分注册失败、正常 stop 和运行期 Error 共用逆注册顺序的注销与调度排空；未注册回调不会移除其他订阅者。公共清理函数不修改 Arm、会话授权或公开状态，调用者仍负责原有 Disarm/撤销时序、Error/Stopped 区分和逐项启动失败原因。

手动模式的基础预检仅检查当前参数配置中左右电机各至少一路。Commander 在同一参数事务内遍历正式生成的 PWM 通道合同读取 FUNC，不再把 MotorOutput 状态新鲜度、待应用标志、当前 PWM 状态或校准标志加入 Manual 预检，也不要求 RC 新鲜、摇杆居中或 Commander 参数有效。RC Arm/Toggle-Arm 不再在校准动作过滤处提前拒绝 Manual，但仍拒绝失鲜的正向动作请求。运动校准保留原来的参数、RC、居中和 Neutral 预检；`COM_ARM_STICK_DZ` 仅约束运动校准解锁。

该修改只收敛 Manual 入场条件，不清除 Kill/Termination 或故障锁存；MotorOutput 的硬停波、有效配置/命令检查继续执行。维护/Flash 仍经过最终原子门，解锁后的 RC loss、Commander 参数故障和执行器故障继续触发 Disarm，因此预检通过不代表能在无 RC、无效 PWM 配置或硬件故障下保持输出。GCS loss 不触发导航动作。传感器是否检测到目前是可观测健康信息，不会静默改变手动驾驶或 BootHealth 的既有策略。

对外 `pre_flight_checks_pass` 与 `ready_to_arm` 在 Disarmed 时还包含维护/Flash 忙状态，避免后台写入期间显示可解锁。实际 Arm 仍重新检查基础预检、读取同一维护诊断快照并经过 `try_arm()` 原子门。维护拒绝提示包含 owner、阶段、毫秒年龄和首个撤销原因；启动或改参后的短暂拒绝是正常保护，不缓存 Manual Arm。自动校准阶段间仅切换停波锁，不再次请求 Arm。

`NAV_RCL_ACT` 与 `NAV_DLL_ACT` 继续校验项目固定策略，非法读回仍令参数无效并触发原有保护；不保存常量策略的重复运行缓存。RC loss 直接执行既有 Disarm 行为。

源码/构建验证不等于车辆安全验证；校准中负向动作、参数应用竞争、看门狗、PWM safe-off 和真实解锁边沿均保持 `BOARD PENDING`。

## 头文件实现边界

Commander 的普通状态访问器定义在 Commander.cpp；仅改变定义位置，不改变 Armed 语义、安全互锁或输出锁存。 统一审查与验收见 docs/HEADER_IMPLEMENTATION_SPLIT_ZH.md。

## Armed 自动校准与物理停波

`arming_allowed` 表示允许人工授予会话，`awaiting_arm` 保留为阶段运动就绪诊断，`motion_allowed` 才允许运动。静态与提交时 `motion_inhibited=true`；Commander 只接纳新鲜同模式校准状态、独立停波锁和后端六路全零的 CONTROL_INHIBITED 证据。输出失鲜、映射破损、Retry/Fault 仍失败。停波/恢复有固定 250 ms 输出交接窗口；维护 lease 持有期间，协调器状态可覆盖最长 20 s 保存操作，RC 和 PWM 活性监控继续独立运行。

ArmedFlashCoordinator 的校准例外须经过“锁存停波 → PWM owner 确认 → 校准维护 lease”，普通 Armed 仍禁止维护/Flash。消费者用 ConfigurationUpdateLease 覆盖检查到应用之间的抢占窗口；任何维护、Flash 或消费者租约未释放都不能解开停波锁。外部 QGC 校准、普通维护和固件确认不取得此例外。
