"""schema=2 协议校验与序号工具 (TP-DEV-001 v2.0 §1.6, §3.3)。

设计要点:
- 全部校验返回 (frame, None) 或 (None, error_code)，不抛异常到网络回调。
- 布尔值不得冒充整数；NaN/Infinity/超长数组/非法字符串一律拒绝。
- 校验器不修改输入，不做单位换算以外的事情。
"""
from __future__ import annotations

import math
import re
from dataclasses import dataclass, field
from typing import Any

SCHEMA = 2
MAX_MQTT_PAYLOAD = 1024
MAX_UDP_PAYLOAD = 1200

DEVICE_ID_RE = re.compile(r"^[a-z0-9-]{1,32}$")
BOOT_ID_RE = re.compile(r"^[0-9a-f]{16}$")
UINT32_MAX = 0xFFFFFFFF

# 量程裕量: 库默认 ±4 g / ±2000 dps, 留 2 倍余量做"显然非法"判定
ACC_LIMIT_MPS2 = 16 * 9.80665
GYRO_LIMIT_RPS = 2000 * math.pi / 180 * 2

ERRORS = {
    "bad_json": "无法解析为 JSON 对象",
    "too_long": "超过长度上限",
    "unsupported_schema": "schema 不受支持",
    "bad_device_id": "device_id 非法",
    "bad_boot_id": "boot_id 非法",
    "id_mismatch": "主题/账号与消息体设备标识不一致",
    "missing_field": "缺少必需字段",
    "invalid_type": "字段类型错误",
    "invalid_array": "数组长度或元素非法",
    "invalid_number": "数值为 NaN/Infinity 或超范围",
    "invalid_sequence": "序号非法",
    "unknown_topic": "主题后缀未知",
}


class ProtocolError(Exception):
    def __init__(self, code: str, detail: str = ""):
        self.code = code
        self.detail = detail or ERRORS.get(code, code)
        super().__init__(f"{code}: {self.detail}")


def is_int(value: Any) -> bool:
    """严格整数：排除 bool（Python 中 True 是 int 的子类）。"""
    return isinstance(value, int) and not isinstance(value, bool)


def is_number(value: Any) -> bool:
    return (isinstance(value, (int, float)) and not isinstance(value, bool)
            and math.isfinite(float(value)))


def _require(obj: dict, key: str, kind: str) -> Any:
    if key not in obj:
        raise ProtocolError("missing_field", f"缺少字段 {key}")
    value = obj[key]
    if kind == "int":
        if not is_int(value) or not 0 <= value <= UINT32_MAX:
            raise ProtocolError("invalid_type", f"{key} 必须是 uint32")
    elif kind == "u64":
        if not is_int(value) or not 0 <= value <= 0xFFFFFFFFFFFFFFFF:
            raise ProtocolError("invalid_type", f"{key} 必须是 uint64")
    elif kind == "str":
        if not isinstance(value, str):
            raise ProtocolError("invalid_type", f"{key} 必须是字符串")
    elif kind == "bool":
        if not isinstance(value, bool):
            raise ProtocolError("invalid_type", f"{key} 必须是布尔值")
    elif kind == "number":
        if not is_number(value):
            raise ProtocolError("invalid_number", f"{key} 必须是有限数值")
    elif kind == "vec3":
        if not isinstance(value, list) or len(value) != 3:
            raise ProtocolError("invalid_array", f"{key} 必须是长度 3 的数组")
        for item in value:
            if not is_number(item):
                raise ProtocolError("invalid_array", f"{key} 含非法元素")
    return value


@dataclass(frozen=True)
class Button:
    jump: bool
    press_count: int
    press_t_us: int | None


@dataclass(frozen=True)
class RawFrame:
    device_id: str
    boot_id: str
    link_epoch: int
    seq: int
    sample_seq: int
    t_us: int
    acc_mps2: tuple[float, float, float]
    gyro_rps: tuple[float, float, float]
    button: Button
    sample_hz: float
    received_mono: float = 0.0

    @property
    def session(self) -> tuple[str, int]:
        return (self.boot_id, self.link_epoch)


def _check_envelope(msg: dict, topic_device_id: str | None) -> None:
    if not is_int(msg.get("schema")):
        raise ProtocolError("invalid_type", "schema 必须是整数")
    if msg["schema"] != SCHEMA:
        raise ProtocolError("unsupported_schema", f"schema={msg['schema']}")
    device_id = msg.get("device_id")
    if not isinstance(device_id, str) or not DEVICE_ID_RE.match(device_id):
        raise ProtocolError("bad_device_id")
    if topic_device_id is not None and device_id != topic_device_id:
        raise ProtocolError("id_mismatch", f"topic={topic_device_id} body={device_id}")
    boot_id = msg.get("boot_id")
    if not isinstance(boot_id, str) or not BOOT_ID_RE.match(boot_id):
        raise ProtocolError("bad_boot_id")
    _require(msg, "link_epoch", "int")


def parse_raw(msg: Any, topic_device_id: str | None = None,
              payload_len: int = 0) -> RawFrame:
    if payload_len > MAX_MQTT_PAYLOAD:
        raise ProtocolError("too_long", f"{payload_len} > {MAX_MQTT_PAYLOAD}")
    if not isinstance(msg, dict):
        raise ProtocolError("invalid_type", "payload 必须是 JSON 对象")
    _check_envelope(msg, topic_device_id)

    seq = _require(msg, "seq", "int")
    sample_seq = _require(msg, "sample_seq", "int")
    t_us = _require(msg, "t_us", "u64")
    acc = _require(msg, "acc_mps2", "vec3")
    gyro = _require(msg, "gyro_rps", "vec3")
    if any(abs(v) > ACC_LIMIT_MPS2 for v in acc):
        raise ProtocolError("invalid_number", "acc_mps2 超量程")
    if any(abs(v) > GYRO_LIMIT_RPS for v in gyro):
        raise ProtocolError("invalid_number", "gyro_rps 超量程")

    button = msg.get("button")
    if not isinstance(button, dict):
        raise ProtocolError("invalid_type", "button 必须是对象")
    jump = _require(button, "jump", "bool")
    press_count = _require(button, "press_count", "int")
    if "press_t_us" not in button:
        raise ProtocolError("missing_field", "缺少字段 press_t_us")
    press_t_us = button["press_t_us"]
    if press_t_us is not None and (not is_int(press_t_us) or not 0 <= press_t_us <= 0xFFFFFFFFFFFFFFFF):
        raise ProtocolError("invalid_type", "press_t_us 必须是 uint64 或 null")

    sample_hz = _require(msg, "sample_hz", "number")
    if not 1 <= sample_hz <= 1000:
        raise ProtocolError("invalid_number", "sample_hz 越界")

    return RawFrame(
        device_id=msg["device_id"], boot_id=msg["boot_id"],
        link_epoch=msg["link_epoch"], seq=seq, sample_seq=sample_seq,
        t_us=t_us, acc_mps2=tuple(float(v) for v in acc),
        gyro_rps=tuple(float(v) for v in gyro),
        button=Button(bool(jump), press_count, press_t_us),
        sample_hz=float(sample_hz),
    )


def parse_status(msg: Any, topic_device_id: str | None = None) -> dict:
    if not isinstance(msg, dict):
        raise ProtocolError("invalid_type", "payload 必须是 JSON 对象")
    _check_envelope(msg, topic_device_id)
    if not isinstance(msg.get("online"), bool):
        raise ProtocolError("invalid_type", "online 必须是布尔值")
    return msg


def parse_event(msg: Any, topic_device_id: str | None = None) -> dict:
    if not isinstance(msg, dict):
        raise ProtocolError("invalid_type", "payload 必须是 JSON 对象")
    _check_envelope(msg, topic_device_id)
    for key in ("seq", "t_us", "press_count"):
        if not is_int(msg.get(key)):
            raise ProtocolError("invalid_type", f"{key} 必须是整数")
    if not isinstance(msg.get("event_id"), str) or not msg["event_id"]:
        raise ProtocolError("invalid_type", "event_id 非法")
    if msg.get("type") not in ("jump_pressed",):
        raise ProtocolError("invalid_type", "type 非法")
    press_t_us = msg.get("press_t_us")
    if press_t_us is not None and not is_int(press_t_us):
        raise ProtocolError("invalid_type", "press_t_us 非法")
    return msg


def parse_ack(msg: Any) -> dict:
    if not isinstance(msg, dict):
        raise ProtocolError("invalid_type", "payload 必须是 JSON 对象")
    if msg.get("schema") != SCHEMA:
        raise ProtocolError("unsupported_schema")
    if not isinstance(msg.get("cmd_id"), str):
        raise ProtocolError("invalid_type", "cmd_id 非法")
    if not isinstance(msg.get("ok"), bool):
        raise ProtocolError("invalid_type", "ok 非法")
    return msg


def newer(a: int, b: int) -> bool:
    """模 2^32 序号比较：a 是否严格新于 b。跨度须小于半个空间。"""
    delta = (a - b) & UINT32_MAX
    return 0 < delta < 0x80000000


def parse_topic(topic: str) -> tuple[str, str]:
    parts = topic.split("/")
    if len(parts) == 5 and parts[0] == "tiltplay" and parts[1] == "v2" and parts[3] == "telemetry" and parts[4] == "raw":
        return parts[2], "raw"
    if len(parts) == 4 and parts[0] == "tiltplay" and parts[1] == "v2":
        if parts[3] in ("event", "status", "cmd", "ack", "event_ack"):
            return parts[2], parts[3]
    raise ProtocolError("unknown_topic", topic)
