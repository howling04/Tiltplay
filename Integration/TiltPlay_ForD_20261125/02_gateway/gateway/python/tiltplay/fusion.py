"""双轴倾斜估计 (TP-DEV-001 §1.7.1, §3.4.1)。

V3/V4 只需要受限角度内的两个控制量: 前后 θf、左右 θr。
- 加速度给出低频参考: θf=atan2(-ax,az), θr=atan2(-ay,az)（C 坐标: 前/右/上, 静置 az≈+g）
- 陀螺仪给出高频增量, 用一阶互补滤波融合
- 死区 3°、饱和 25°, 输出 u∈[-1,1]
"""
from __future__ import annotations

import math
from dataclasses import dataclass

G = 9.80665


@dataclass
class TiltConfig:
    deadzone_deg: float = 3.0
    saturation_deg: float = 25.0
    tau_s: float = 0.5
    gyro_pitch_sign: int = -1
    gyro_roll_sign: int = 1
    axis_matrix: tuple[tuple[int, int], tuple[int, int], tuple[int, int]] = ((0, 1), (1, 1), (2, 1))


def apply_axis_matrix(vec, matrix) -> tuple[float, float, float]:
    """库坐标 -> 控制器 C 坐标(前/右/上)。matrix[i] = (源轴索引, 符号)。"""
    return tuple(sign * float(vec[idx]) for idx, sign in matrix)  # type: ignore[return-value]


def accel_angles(acc_c) -> tuple[float, float]:
    """返回 (θf, θr)，单位度。仅在受限角度内使用。"""
    ax, ay, az = acc_c
    if abs(az) < 1e-6:
        az = 1e-6 if az >= 0 else -1e-6
    theta_f = math.degrees(math.atan2(-ax, az))
    theta_r = math.degrees(math.atan2(-ay, az))
    return theta_f, theta_r


def map_axis(theta_deg: float, deadzone: float, saturation: float) -> float:
    magnitude = abs(theta_deg)
    if magnitude <= deadzone:
        return 0.0
    scale = min((magnitude - deadzone) / (saturation - deadzone), 1.0)
    return math.copysign(scale, theta_deg)


def clamp_vector(vf: float, vr: float) -> tuple[float, float]:
    length = math.hypot(vf, vr)
    if length > 1.0:
        return vf / length, vr / length
    return vf, vr


class ComplementaryTilt:
    """单设备、两轴的互补滤波状态。"""

    def __init__(self, cfg: TiltConfig):
        self.cfg = cfg
        self.theta_f = 0.0
        self.theta_r = 0.0
        self.initialised = False
        self.last_dt_s = 0.0
        self.resets = 0

    def reset(self) -> None:
        self.theta_f = 0.0
        self.theta_r = 0.0
        self.initialised = False
        self.resets += 1

    def update(self, acc_mps2, gyro_rps, dt_s: float | None) -> tuple[float, float]:
        acc_c = apply_axis_matrix(acc_mps2, self.cfg.axis_matrix)
        gyro_c = apply_axis_matrix(gyro_rps, self.cfg.axis_matrix)
        acc_f, acc_r = accel_angles(acc_c)

        if not self.initialised or dt_s is None or dt_s <= 0 or dt_s > 0.25:
            # 首帧、异常 dt 或长间断: 用加速度直接重置, 不把一帧角速度当整段运动
            self.theta_f, self.theta_r = acc_f, acc_r
            self.initialised = True
            self.last_dt_s = 0.0 if dt_s is None else max(dt_s, 0.0)
            return self.theta_f, self.theta_r

        alpha = self.cfg.tau_s / (self.cfg.tau_s + dt_s)
        rate_f = self.cfg.gyro_pitch_sign * gyro_c[1]  # 绕 +右轴
        rate_r = self.cfg.gyro_roll_sign * gyro_c[0]   # 绕 +前轴
        self.theta_f = alpha * (self.theta_f + math.degrees(rate_f) * dt_s) + (1 - alpha) * acc_f
        self.theta_r = alpha * (self.theta_r + math.degrees(rate_r) * dt_s) + (1 - alpha) * acc_r
        self.last_dt_s = dt_s
        return self.theta_f, self.theta_r

    def move(self) -> tuple[float, float]:
        uf = map_axis(self.theta_f, self.cfg.deadzone_deg, self.cfg.saturation_deg)
        ur = map_axis(self.theta_r, self.cfg.deadzone_deg, self.cfg.saturation_deg)
        return clamp_vector(uf, ur)
