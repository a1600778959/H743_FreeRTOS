# SPDX-License-Identifier: BSD-3-Clause
# Rover 自动校准状态机的唯一状态/结果/故障枚举权威源。
# ENUM_LABELS STATE RESULT FAILURE FENCE GAIN EXCITATION BRAKING
#
# 消息版本：2（制动响应候选，由同一权威生成链更新所有消费者）。
# 本版为不兼容重编：状态区压平为 11 个顶层阶段，删除全部旧细粒度状态常量
# （WAIT_ARM_*/STOP_*/COMMIT_*/APPLY_*/RESTORE_*/PROFILE_REVERSE/PROFILE_FULL
# 及独立 Return/Braking/Deceleration 状态），不保留旧数值兼容、别名或映射层。
# 阶段内部的 Arm 等待、掉头、返程、制动观测、停车确认与事务推进属于内部
# 调度合同（SessionController 的 PhaseSubstate/TransactionKind/TransactionPhase），
# 不进入本消息；消费端只能按 11 个阶段解释 state 字段。
# 版本证据：终态正式生成后的 topic hash 与 logger contract（MESSAGE_VERSION
# 随之递增），不引入手写生成物。
uint8 ORB_QUEUE_LENGTH = 8

uint32 MESSAGE_VERSION = 2

# 顶层阶段（压平后唯一状态集；阶段内推进见内部调度合同）。
uint8 STATE_IDLE = 0            # 无活动会话。
uint8 STATE_PREFLIGHT = 1       # 入场检查与 Level 事务（请求/结果均在本阶段处理）。
uint8 STATE_BASELINE = 2        # RTK 基线静态采集，随后进入首个统一 Arm gate。
uint8 STATE_STRAIGHT = 3        # 直线往返动力学辨识；含内联制动观测、掉头与返程子状态。
uint8 STATE_TURN = 4            # 原地 CW/CCW 旋转：角速度辨识、磁覆盖采集与动力学提交。
uint8 STATE_MAGNETIC = 5        # 磁评估/提交：bootstrap、偏置事务、磁-油门补偿与恢复决策。
uint8 STATE_PROFILE = 6         # IMU 偏置收尾 + 稳态响应采集（前进/CW/CCW 平台）与运行候选。
uint8 STATE_IDENTIFICATION = 7  # 闭环辨识实验（速度/角速度激励）与内环增益候选计算。
uint8 STATE_VALIDATION = 8      # 内环/航向增益的闭环阶跃验证、修复与 cohort 推进。
uint8 STATE_NAVIGATION = 9      # 转驱与路径试验：导航候选比较、确认圈与最终选择。
uint8 STATE_FINALIZE = 10       # 统一收尾：Prepare→Save 或 Rollback→WaitFlash→Publish→Exit。

uint8 RESULT_NONE = 0
uint8 RESULT_RUNNING = 1
uint8 RESULT_SUCCESS = 2
uint8 RESULT_PARTIAL = 3
uint8 RESULT_FAILED = 4
uint8 RESULT_CANCELLED = 5

uint8 FAILURE_NONE = 0
uint8 FAILURE_PREFLIGHT = 3
uint8 FAILURE_SENSOR_STALE = 4
uint8 FAILURE_NOT_STATIONARY = 5
uint8 FAILURE_RTK_INCONSISTENT = 7
uint8 FAILURE_MOTION_ENVELOPE = 8
uint8 FAILURE_CONTROL_LOSS = 9
uint8 FAILURE_ESTIMATOR = 10
uint8 FAILURE_PARAMETER = 11
uint8 FAILURE_FRONTEND_CONFIRMATION = 12
uint8 FAILURE_MECHANICAL_ASYMMETRY = 13
uint8 FAILURE_MAG_DISTURBED = 14
uint8 FAILURE_TIMEOUT = 15
uint8 FAILURE_OPERATOR_CANCEL = 16
uint8 FAILURE_DYNAMICS_UNOBSERVABLE = 17
uint8 FAILURE_ENTRY_POSITION = 18
uint8 FAILURE_FENCE_SPACE = 19
uint8 FAILURE_FENCE_BOUNDARY = 20
uint8 FAILURE_DECELERATION = 21
uint8 FAILURE_IDENTIFICATION = 22
uint8 FAILURE_GAIN_VALIDATION = 23
uint8 FAILURE_MOTION_UNAVAILABLE = 24
uint8 FAILURE_PATH_UNOBSERVABLE = 25
uint8 FAILURE_PROFILE_UNOBSERVABLE = 26
uint8 FAILURE_DRIVE_ENVELOPE = 28
# Level 提交阶段持久化（Flash 分区满/擦除重建失败）持续不可用；参数值仍在
# RAM 生效，但掉电即丢，恢复存储后须重跑校准。
uint8 FAILURE_STORAGE = 29

uint8 FENCE_UNAVAILABLE = 0
uint8 FENCE_SAFE = 1
uint8 FENCE_STOP_MARGIN = 2
uint8 FENCE_OUTSIDE = 3
uint8 FENCE_POSITION_LOST = 4
uint8 FENCE_DECELERATION_INVALID = 5
uint8 FENCE_BRAKING_PROBE = 6 # 仅有界探测范围，不代表已有停车模型。

uint8 GAIN_NONE = 0
uint8 GAIN_INNER = 1
uint8 GAIN_HEADING = 2
uint8 GAIN_PATH = 3
uint8 GAIN_NAVIGATION = 4

# 响应采集子状态；换档由请求到位/连续稳态/有效样本驱动，不按固定时刻跳档。
uint8 EXCITATION_NONE = 0
uint8 EXCITATION_PROBE = 1
uint8 EXCITATION_RAMP = 2
uint8 EXCITATION_SETTLE = 3
uint8 EXCITATION_COLLECT = 4
uint8 EXCITATION_BRAKE = 5
uint8 BRAKING_ACCELERATE = 0
uint8 BRAKING_ACTIVE = 1 # 正向运动中请求反向驱动力，零速附近由控制器锁定撤销。
uint8 BRAKING_SETTLE = 2
uint8 BRAKING_SPEED_LIMIT = 3 # 初探超速调节；降速后继续采样，不计完整制动观测。

uint32 STAGE_LEVEL = 1
uint32 STAGE_RTK = 2
uint32 STAGE_SPEED = 4
uint32 STAGE_YAW = 8
uint32 STAGE_MAG = 16
uint32 STAGE_MAG_SKIPPED = 32
uint32 STAGE_INNER_GAINS = 64
uint32 STAGE_HEADING_GAIN = 128
uint32 STAGE_PATH_GAIN = 256
uint32 STAGE_IMU_BIAS = 512
uint32 STAGE_MOTOR_PROFILE = 1024
uint32 STAGE_RUNTIME = 2048
uint32 STAGE_NAV_STRATEGY = 4096
uint32 STAGE_MAG_MOT = 8192 # 磁偏置与电机干扰补偿分别报告，后者保存确认后才完成。
uint32 STAGE_DECELERATION = 16384 # 首轮响应辨识、第二轮制动验证、消费者应用及保存均完成。
uint32 NAV_FIELD_LOOKAHEAD = 1
uint32 NAV_FIELD_JERK = 2
uint32 NAV_FIELD_SPEED_REDUCTION = 4
uint32 NAV_FIELD_TRANSITIONS = 8

uint64 timestamp
uint64 timestamp_sample
uint32 session_id
uint32 sequence
uint32 completed_stages # 本次会话最终保存成功的成果；运行中为0，回滚不得置位。
uint32 unavailable_stages
uint32 skipped_stages # 能力范围外或无对应设备，未尝试且不声称已完成；与 unavailable 区分。
uint32 provisional_validated_stages # RAM中已确认的组证据，供阶段依赖使用；尚未最终保存。
uint8 excitation_phase
uint8 excitation_level # 0..5 为六档平台，6 为非零下降平台。
uint32 excitation_samples
float32 excitation_target # 请求绝对值：前进 [0,E]，转向 [0,1]。
float32 excitation_remaining_s # 本次运动硬截止剩余秒数；NONE 时为 0。
uint8 state
uint8 result
uint8 failure_reason
uint8 progress
bool active
bool motion_allowed
bool arming_allowed # 接受人工会话授权；与运动就绪分离，终止/回滚时必须撤销。
bool motion_inhibited # 模式主动锁住 PWM；Armed 静态/提交阶段须持续确认物理停波。
bool ready_for_entry
bool awaiting_arm
bool session_authorized # 协调器观察到首次 Arm 后的诊断镜像；实际授权只属于 Commander。
bool magnetometer_present
bool mag_bootstrap_active
bool closed_loop
bool gains_provisional
bool fence_center_valid
uint8 fence_state
uint8 gain_group
uint64 fence_center_timestamp
uint32 fence_device_id
float64 fence_latitude_deg
float64 fence_longitude_deg
float32 fence_origin_error_m
float32 fence_radius_m
float32 fence_stop_distance_m # v²/(2a)名义距离加配置/实测输出等待留距；无模型为NaN，与实测距离分开。
float32 straight_distance_m # 入场冻结 RO_CAL_DIST。
float32 entry_deceleration_m_s2 # 入场配置诊断快照，允许未配置的 -1/0。
float32 braking_deceleration_m_s2 # 本次前向停车的Δv/Δt；首轮临时模型，第二轮验证后保存，不混合不同制动策略。
float32 braking_input_gain # 首轮输入冲量加权幅值/(E*前向速度下降)，不把零输出等待或回摆计入分母；仅本会话使用，代次非零时0表示滑停无需反向力。
float32 braking_initial_speed_m_s # 最近一次停车的实际初速。
float32 braking_stop_distance_m # 最近一次首次前向停车前的速度积分/定位投影最大值；不累计回摆，安全余量另算。
float32 braking_stop_time_s # 从制动起始速度历元到首次过零插值点或首次进入停速带，不含随后回摆/停稳确认。
float32 braking_distance_envelope_m # 本轮已接受正式停车的最大实测前向距离，仅作观测诊断。
float32 braking_speed_envelope_m_s # 本轮正式制动期间观测到的最高前向速度，仅作诊断，不作为运行速度上限。
uint8 braking_observations # 已通过质量检查的完整停车次数。
uint8 braking_run # 1/2=去程/返程各一次满输出持续大于3秒后触发的正式观测。
uint8 braking_phase
uint8 braking_model_generation # 1/2=每轮观测各递增一次；完成位仍只代表已保存。
float32 braking_full_speed_m_s # 正式全输出平台实测车速，不是编码器 RPM。
bool braking_full_output # 正式制动前的满输出加速/持续计时窗口；不要求车速稳态，仅该窗口不受巡航限速提前截断。
bool straight_active # 直线运动及关联掉头/返程使用独立距离范围。
float32 fence_distance_m
float32 fence_margin_m
float32 fence_working_radius_m
float32 session_speed_limit_m_s
float32 session_motor_limit
float32 entry_cruise_speed_m_s
float32 observed_forward_speed_m_s
float32 observed_output_max
# 100*norm(K_body)*冻结包络/WMM 场强；-1=未知，可以超过 100%。
float32 mag_interference_pct
float32 level_roll_offset_deg
float32 level_pitch_offset_deg
float32 rtk_baseline_m
float32 rtk_yaw_offset_deg
float32 maximum_speed_m_s
float32 yaw_rate_correction
float32 speed_p
float32 speed_i
float32 yaw_rate_p
float32 yaw_rate_i
float32 heading_p
float32 lookahead_gain
float32 validation_error
uint32 navigation_observed_fields

# 三项导航整定只扩展观测，不定义第二套导航运动状态。
uint8 NAV_TUNE_NONE = 0
uint8 NAV_TUNE_LOOKAHEAD = 1
uint8 NAV_TUNE_JERK = 2
uint8 NAV_TUNE_SPEED_REDUCTION = 3
uint8 navigation_tuning_item
float32 navigation_candidate # 当前实验候选；RED独立测速时为m/s，系数确认时为无量纲。
bool navigation_speed_probe # true表示候选值为独立转弯测速目标m/s。
float32 navigation_xtrack_max_m
float32 navigation_speed_rms_m_s
float32 navigation_jerk_rms_m_s3
