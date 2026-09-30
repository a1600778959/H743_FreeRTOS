# SPDX-License-Identifier: BSD-3-Clause
# 组合校准协调器发往 Commander 的内部请求；不扩展锁定 PX4 ActionRequest。
uint8 ORB_QUEUE_LENGTH = 4
# 阶段保持 Armed；旧阶段 Disarm/Arm 请求已退役，现存请求编号保持不变。
uint8 REQUEST_LEVEL = 1
uint8 REQUEST_CANCEL_LEVEL = 2
uint8 REQUEST_EXIT = 3

uint64 timestamp
uint32 session_id
uint32 sequence
uint32 parameter_set_count
uint8 request
