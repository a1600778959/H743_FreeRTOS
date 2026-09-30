# Rover 控制与执行器组合

制动响应适配：首轮u=ramp*clamp(v/v_entry,0,1)，ramp沿用0.15/s，速度下降时同步减弱未知响应激励。首轮由实际后端J/Q和前向速度变化生成braking_input_gain，经已有会话/观测代次/停波握手应用；第二轮及普通停车冻结g，执行u=g*v。开环限速也按g*v施力，在limit-max(RO_SPEED_TH,0.08)下边界释放（不低于既有0.08停速分辨率，也不高于limit），避免对限速线渐近；恢复正向请求沿用0.15/s斜坡。接管与恢复均标记braking_speed_limited，不进入稳定辨识。无新冷却时长、历元计数门或最小反向幅值。g=0且代次非零表示实测滑停。

2026-09-28 制动过约束剔除：删除固定100ms施力/420ms等待、最大下降率预测及相关状态。制动连续使用实测有符号速度，真实近零/反向或失鲜后锁存撤力；Brake轮端线性映射、公共输出保护和停车确认保留。相邻GPS历元仍允许配对；鲜度只向过去计入速度落后量，速度领先时使用较早航向到达时间保守计龄，不向未来外推。未增加参数或替代控制器。

MODE_HEADING_TARGET为校准提供目标航向执行：使用与请求同参考系的RTK航向累计转角；请求标识/会话不变时不重启机动，变更目标须换标识。该通路不启用Speed/YawRate PI，也没有指定转速或固定完成用时；按用户指定0.4～0.8范围固定采用0.6归一化转向幅度，删除起转探测、私有爬升及输入缓存；运动角速度不再决定输出幅度。先停车再起转，到位或越界撤力；3度角度容差、实际停稳和撤力请求之后的后端零输出共同决定完成。输入仍只进入现有DifferentialDrive与MotorOutput；反向响应、传感器/命令失鲜及独立围栏/权限保护保留。

自动校准的请求—执行双验有意保留：模式负责实验轮次/候选，控制器独立复核请求新鲜度、会话冻结包络、两轮制动序列及模型代次。两侧的fresh/session_limits/轮次校验不能合成由模式单方担保；后端确认是模型交接依据。

校准参数确认由对应值接口同时核对应用有效性、参数代次与实际参数值；删除无人独立使用的仅代次确认接口。增益、减速度和制动模型的各自确认职责保留。

2026-09-24 限制器观测：RoverControlStatus.input_limited由本周期Speed/YawRate控制器实际采用的设定与原请求比较产生，用于区分规划参数被内环限斜率遮蔽；不改变控制器输出、PWM、REV或运动许可。

第二轮减法修正：模式不再产生负E坐标制动请求，只发布有效停车意图；RoverDifferential的唯一制动实现由正式测量、普通停车和超速接管共用，删除旧普通分支的0.15/s缓升、2.4 m/s²判据及相关缓存。保留原正式观测的有界比例策略，不新增制动控制器。明确停车优先于巡航减速，接管交接保留同一次制动的已确认方向；近零锁存撤力、MOT_SLEW_RATE/REV_DELAY及末端E包络继续生效。主动制动使用公共Brake用途：轮端q=E*u，跳过驱动MIN/EXPO/ASYM；Drive用途保持原整形。限速转停车不解除释放锁存；测速失效也锁存撤力，仅新的行驶动作或正式观测复位。校准闭环复用PI的负反馈制动输出，不再二次截成零。参数/请求来源/TTL/会话及全局安全互锁保持。


2026-09-23 公式修正：校准与导航的Speed PI输出统一在混控前[0,1]坐标按1-|steering|限幅；MOT_THR_MAX只由公共DifferentialDrive施加一次，校准开环原E坐标入口仍除E换基。这样E=0.5时开/闭环最大轮端输出均为0.5，不再出现闭环0.25。Commander、请求TTL/来源/参数代次、MotorOutput停波/REV等保护不变。


- 本轮内环已验证且参数可用时，校准纯旋转沿用 MODE_SPEED_YAW_RATE，由现有 yaw_rate_controller_ 执行；初始辨识不依赖旧 PID，使用既有 MODE_NORMALIZED_AXES 开环激励。控制层不为开环校准要求 PID 参数，不注入固定增益。两种纯旋转请求均固定零纵向并排除纵向制动；原零/零停车路径保留。物理目标、公共混控和 PWM/REV 的职责不变。

当前自动校准巡航控制：直行模式按用户巡航速度调节实验请求；执行层超过该速度时复用已有主动制动，回到巡航范围后继续实验。巡航超速不触发模式终止或闭环无效帧，不增加比例速度上限。正式制动观测与停波阶段优先；围栏及传感器故障仍由原安全链处理；闭环目标遵守既有 RO_YAW_RATE_LIM，不另加固定 0.6 rad/s 请求上限或实测转速中断。以下历史初探 run=0 说明已退役。

制动实测模型交接保留会话、轮次/代次、SETTLE及真实停波检查。候选正值范围与现有RO_DECEL_LIM一致（不超过100 m/s²），不再依赖旧测量公式的1.5人为上限；测量端对越界数据明确拒绝，不能发布执行层永不接收的候选。

正式制动标定前的braking_full_output窗口以确认MOT_THR_MAX满输出持续大于3秒为启动条件，初速取实测值，不要求加速度稳态；只有当前获准、开环、直线的平台请求才跳过巡航超速接管。请求有效性、冻结电机包络、TTL和安全投影仍由原链核验；正式制动及退出平台后恢复原有策略。该标志来自正式消息生成链，不添加私有轮端通路。

2026-09-18 初探 run=0 超速调节：控制器以 100 Hz 独立识别超过初探目标，锁住正向输出并复用反向制动，允许超过原 0.45 m/s／入场速度时继续减速。接管通过 `rover_control_status.braking_speed_limited` 反馈，模式进入生成的 `BRAKING_SPEED_LIMIT` 并确认后端零输出后交还；跨队列不同步不能令旧正请求重新生效。调速制动以采样区间中点为恢复目标，只在新速度历元上重新允许反向力；完整停车仍采用原有零速撤销锁存。`BRAKING_ACTIVE/SETTLE` 完整停车优先，不被调速恢复覆盖。单帧失效先撤输出并清反向请求，保留当前会话已证实的方向；会话结束/重建及运行复位清除接管状态。不改变其他模式的输出许可。下文旧的速度上限和负纵向限制在此专用路径按本条执行。

2026-09-20 第三版责任收敛：`Commander` 负责 RC/Kill/Failsafe、参数和全局 Armed，`RoverDifferential` 负责 request source、校准会话/包络、TTL 与输出合同，`MotorOutput` 负责 PWM 后端 Retry/Fault/Hard Safe Off，`AutoCalibrationMode` 只负责实验阶段、RTK/采样质量和空间边界。校准初探正向加速直接复用 Manual 等价 `DifferentialDrive` 语义，模式侧是唯一正向 ramp 所有者；不加入 Manual/Calibration parity 检查，也不加入 1°/3°/5° 起步纠偏。无效帧仍由 `publish_invalid()` 清零并交给全局安全层收尾。

当前修复补充：硬边界（围栏、控制链/传感器失鲜、180 s 运动截止）立即终止；RTK 精度、基线和加速度质量需连续 300 ms 才终止；正常开环请求不再绑定 IMU sample。主动制动只在 BRAKING_ACTIVE、BRAKING_SPEED_LIMIT 和明确停车/返程状态接管，普通零请求不自动反向。校准回滚期间 Hard Safe Off 由 BootHealth 作为有界安全收尾态喂狗，不恢复 PWM。

- `RoverDifferential` 在 `wq:rate_ctrl` 以 100 Hz 校验请求采样时间、Commander 三 Topic 一致性和参数快照；Manual、Navigation 与 Calibration 的逐字段模式校验同执行器层共用 `RoverModeContract`，全局安全处置由 Commander/控制器/PWM 后端各自负责；Manual 直接消费归一化双轴，Navigation 消费物理速度/yaw-rate 并执行 Speed PI 与 YawRate PI，再发布 `actuator_motors`。Motor1 为右侧、Motor2 为左侧，其余十路保持 NaN。
- 纯算法 `Dima/lib/rover/DifferentialDrive.*` 使用固定存储，组合 PX4 v1.17.0 的两轴控制边界与 ArduPilot Rover 的饱和优先级、油门 slew、静摩擦补偿、反向不对称和独立换向延时行为；本目录只保留消息、参数和安全状态的运行适配。ArduPilot GPL 源码只作行为参考，不复制。`MOT_THR_MIN` 静摩擦补偿于 2026-09-18 落地：Manual 正向杆量（越过死区，经 |T|+|S| 缩放后）在 DifferentialDrive 内不低于该下限，直接跳过堵转死区；校准开环跳过 Manual 入口的额外地板，但仍经过一次共有 MIN/EXPO/ASYM 整形；精确零、倒退、闭环调速与非校准导航不夹持，避免在执行器层重复抬高 ramp 或卡住减速调节。
- 参数更新在 ARMED 期间只标记 pending；完整、同时间戳的 DISARMED 安全快照到达后才整体应用，禁止半更新。控制参数无效只抑制 Navigation 输出，不参与 Commander 的 Mission 模式切换。
- `RoverControlValidation.hpp` 集中校验两层共有的 Speed/YawRate 内环参数；控制层仍额外检查速度死区小于满油门速度，AutoMode 保留巡航、停车/转向滞回和 Heading 输出边界。各层在自己的参数快照上独立调用，不增加模式切换门槛。
- Rover Manual 与 AUTO 分别位于 `rover/modes/ManualMode.*`、`AutoMode.*`；两者都只能发布 one-of `rover_motion_request`，不得直接进入本目录内部对象。控制层先计算 steering，再把 Navigation longitudinal 限制到 `1-|steering|`，且自动控制固定 `manual_source=false`。
- 安全 PWM 输出位于 `modules/motor/`，本目录不得直接访问 `ActuatorPwm` 或板级 TIM/GPIO。
- `SOURCE_CALIBRATION` 保持独立来源：开环使用 normalized axes，增益验证使用已有 speed/yaw-rate 模式及本层真实 PI。Commander、控制器、PWM 和 watchdog 共用精确开/闭环安全投影，闭环只打开 Velocity/Rates，不伪装成 Mission。
- `RoverDifferentialCalibration` 在完整 Disarmed 快照中独立锁存入场点、圆形半径、RO_CAL_DIST、RO_DECEL_LIM 初值、正 RO_SPEED_LIM 与 MOT_THR_MAX 包络 E，以新鲜同设备 GNSS 分别复核直线距离和圆形余量；低速/正式制动测量仅在专用状态开放，RAM 模型逐代停波确认后只授权返程，正式保存后才授权普通运动；模型采用实测值与后续较小运行减速度计算 v²/(2a)；会话快照改变后永久失效。所有校准阶段纵向请求/闭环输出 [0,E]、转向 [-1,1]、最终每轮 [-E,E]；超速接管保留 0.15/s 反向缓升，正式 ACTIVE 使用共有 DifferentialDrive 斜率/换向等待，TTL、速度/加速度门禁不变。
- `rover_control_status` 是本层产生的内部 uORB 反馈，包含请求/session/参数代次、真实 PI 设定/反馈/积分及最终执行器均值/差分。消费者应用确认与“闭环配置可运行”分开，允许旧零增益回滚到导航未就绪状态。
- 校准纵向请求/速度一律非负，FWD→CW→CCW；原地转向仍允许左右轮反向且保留换向等待。反馈中的未零区化速度和混控/整形/slew/Arm ramp/安全限制原因继续服务辨识；后端是否真正应用及应用时间由 actuator_output_status 单独提供。


## 头文件实现边界

RoverControlValidation.cpp 持有参数有效性判断，头文件只声明共享接口；控制层和执行器层各自的安全锁存继续独立。 统一审查与验收见 docs/HEADER_IMPLEMENTATION_SPLIT_ZH.md。

校准开环的纵向请求使用 [0,E] 包络坐标，差速器入口除以 E 转成原有 [0,1] 整形输入，避免 E<1 时重复缩放为 E²；速度 FF、响应剖面及辨识的整形前反馈仍使用差速器标准坐标。开环校准正常前进和 Manual 共用同一 `DifferentialDrive` 目标与最终轮端应用路径；闭环 PI、主动制动和超速接管仍保留各自必要的控制/安全处理。`BRAKING_PROBE` 的正向 ramp 只由模式侧按实际 dt 负责，控制层不再叠加正常校准末端 ramp；无效周期仍由全局硬停波清零。

Manual 的转向正值始终表示车头顺时针，前进与倒车不再反转转向符号。其余电机行为按 APM 参考基线核对：先对操作者两轴同比缩放，后续在 [-1/ASYM,1] 中限制转向可行范围，仍允许急转内轮反转。APM 的“MIN 后 EXPO”顺序及左右独立换向等待保持一致；本地没有同向弧线限制、前后切换强制清零或双轮共同换向等待。导航零纵向停车、超速接管的 0.15/s 反向缓升、Arm ramp 和冻结包络继续有效；正式 ACTIVE 制动不再叠加第二套私有轮端 slew。细节及 QGC 诊断见 modules/motor/README.md。

自动校准可在静态阶段提前 Arm。独立校准停波锁生效时，控制器持续发布全 NaN 失效帧，PWM 后端保持物理停波。完整新鲜安全快照加上校准停波确认允许 Armed 参数应用；ConfigurationUpdateLease 保证整份配置应用完成前不能恢复运动。普通 Manual/AUTO 的 Armed 参数冻结不变。

主动制动由 `RoverDifferentialBraking` 统一用于制动测量与自动校准的正常直线停车。专用测量阶段允许负纵向请求，普通校准只在明确零速度/零转向停车请求下内部建立反向力；不改变 Manual 或 Mission 的输出合同。正式 ACTIVE 负请求与前进共用 DifferentialDrive 的 MIN/EXPO/ASYM、配置电机 slew、`MOT_REV_DELAY` 和逐轮换向等待，不再经过第二套 0.15/s 轮端历史；超速接管仍保留独立反向缓升和有限 handover。接近零速时按固定前进方向、速度误差与延迟预测锁定撤销反向输出，末端历史不得继续倒车。全输出测量阶段可超过 RO_SPEED_LIM，其他阶段恢复冻结上限。

2026-09-24 校准输出职责：未标定开环保留既有巡航限速制动；闭环运动由共用Speed PI处理速度误差，calibration_braking_command不再在PI计算后以限速理由覆盖纵向/转向。正式制动观测和明确零目标停车继续使用公共制动，输出权限、围栏与后端保护仍独立生效。
