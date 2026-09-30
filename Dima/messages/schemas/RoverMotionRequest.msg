# SPDX-License-Identifier: BSD-3-Clause
# Dima Manual/Navigation rover motion boundary.
uint8 ORB_QUEUE_LENGTH = 8

uint32 MESSAGE_VERSION = 1
uint8 SOURCE_MANUAL = 0
uint8 SOURCE_NAVIGATION = 1
uint8 SOURCE_CALIBRATION = 2
uint8 MODE_NORMALIZED_AXES = 0
uint8 MODE_SPEED_YAW_RATE = 1
uint8 MODE_HEADING_TARGET = 2

uint64 timestamp
uint64 timestamp_sample
uint32 sequence
bool valid
uint8 source
uint8 mode
float32 normalized_longitudinal
float32 normalized_steering
float32 speed_m_s
float32 yaw_rate_rad_s

# 校准掉头目标：与rtk_heading_status.array_heading_rad同一参考系，不是角速度。
# 机动标识在本次目标执行期间保持不变；其余模式的三个字段保持零。
# 初始方向+1顺时针/-1逆时针；到位后的必要修正由控制层选择方向。
uint64 heading_request_timestamp
float32 heading_target_rad
int8 heading_direction
