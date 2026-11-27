"""测试用构造器：合成 schema=2 帧与合成时间轴。"""
from __future__ import annotations

import json
import math

from tiltplay.bridge import BridgeConfig
from tiltplay.protocol import parse_raw
from tiltplay.state import DeviceConfig, DeviceState

DEVICE = "nano-a1b2c3d4e5f6"
BOOT = "000000000000001a"


def raw_payload(**overrides) -> dict:
    payload = {
        "schema": 2,
        "device_id": DEVICE,
        "boot_id": BOOT,
        "link_epoch": 1,
        "seq": 1,
        "sample_seq": 1,
        "t_us": 0,
        "acc_mps2": [0.0, 0.0, 9.80665],
        "gyro_rps": [0.0, 0.0, 0.0],
        "button": {"jump": False, "press_count": 0, "press_t_us": None},
        "sample_hz": 104,
    }
    payload.update(overrides)
    return payload


def frame(**overrides):
    return parse_raw(raw_payload(**overrides), DEVICE,
                     len(json.dumps(raw_payload(**overrides))))


def tilt_payload(f_deg: float, r_deg: float, **overrides) -> dict:
    f, r = math.radians(f_deg), math.radians(r_deg)
    g = 9.80665
    payload = raw_payload()
    payload["acc_mps2"] = [-g * math.sin(f) * math.cos(r),
                           -g * math.sin(r),
                           g * math.cos(f) * math.cos(r)]
    payload.update(overrides)
    return payload


def make_state(**cfg_overrides) -> DeviceState:
    cfg_overrides.setdefault("require_recenter", False)
    cfg = BridgeConfig(**cfg_overrides)
    state = DeviceState(DeviceConfig(device_id=DEVICE), cfg)
    return state


def attach_probe(state: DeviceState, now: float = 1000.0, device_t_us: int = 0) -> None:
    """让 offset_min = now - device_t_us，从而合成时间轴从 0 开始。"""
    state.on_status({"schema": 2, "device_id": DEVICE, "boot_id": BOOT,
                     "link_epoch": 1, "online": True}, now)
    state.on_probe_sent("probe-test", now)
    state.on_ack({"schema": 2, "cmd_id": "probe-test", "ok": True,
                  "t_us": device_t_us}, now)
