"""TiltPlay 网关主进程 (B 部分)。

链路: MQTT(tiltplay/v2/+) -> 校验 -> 每设备有界队列 -> 融合/去重/回中
      -> 60 Hz UDP 127.0.0.1:7001 -> UE5.8
      <- 7002 回中请求 / input_ack
"""
from __future__ import annotations

import argparse
import collections
import json
import logging
import logging.handlers
import os
import signal
import socket
import sys
import threading
import time
import uuid
from dataclasses import dataclass, field
from pathlib import Path

from . import protocol
from .protocol import ProtocolError
from .state import DeviceConfig, DeviceState, mono

LOG = logging.getLogger("tiltplay.bridge")


# --------------------------------------------------------------------------- 配置
@dataclass
class BridgeConfig:
    mqtt_host: str = "127.0.0.1"
    mqtt_port: int = 1883
    mqtt_username: str = "bridge"
    mqtt_password: str | None = None
    mqtt_client_id: str = "bridge-host"
    udp_host: str = "127.0.0.1"
    udp_port: int = 7001
    control_port: int = 7002
    telemetry_hz: float = 104.0
    udp_hz: float = 60.0
    stale_ms: float = 250.0
    event_ttl_ms: float = 200.0
    probe_interval_ms: float = 2000.0
    probe_ttl_ms: float = 5000.0
    probe_rtt_max_ms: float = 50.0
    clock_margin_ms: float = 10.0
    deadzone_deg: float = 3.0
    saturation_deg: float = 25.0
    still_window_ms: float = 500.0
    still_gyro_rps: float = 0.12
    still_acc_tol: float = 0.15
    require_recenter: bool = True
    force_single_axis: bool = True
    require_level_recenter: bool = False
    recenter_level_max_deg: float = 10.0
    forward_output_sign: float = 1.0
    button_jump_enabled: bool = False
    fusion_tau_s: float = 0.5
    gyro_pitch_sign: int = -1
    gyro_roll_sign: int = 1
    log_dir: str = "tests/logs"
    console_enabled: bool = True
    console_status_s: float = 5.0
    devices: dict[str, DeviceConfig] = field(default_factory=dict)

    @classmethod
    def load(cls, path: str | Path) -> "BridgeConfig":
        raw = json.loads(Path(path).read_text(encoding="utf-8"))
        mqtt_cfg = raw.get("mqtt", {})
        udp_cfg = raw.get("udp", {})
        fusion = raw.get("fusion", {})
        log_cfg = raw.get("log", {})
        password = mqtt_cfg.get("password")
        env_name = mqtt_cfg.get("password_env")
        if not password and env_name:
            password = os.environ.get(env_name)
        devices = {}
        for dev_id, dev in raw.get("devices", {}).items():
            matrix = tuple(tuple(row) for row in dev.get("axis_matrix", ((0, 1), (1, 1), (2, 1))))
            devices[dev_id] = DeviceConfig(
                device_id=dev_id,
                player_slot=int(dev.get("player_slot", 0)),
                mode=dev.get("mode", "tilt2"),
                vertical_step_enabled=bool(dev.get("vertical_step_enabled", False)),
                axis_matrix=matrix,
            )
        return cls(
            mqtt_host=mqtt_cfg.get("host", "127.0.0.1"),
            mqtt_port=int(mqtt_cfg.get("port", 1883)),
            mqtt_username=mqtt_cfg.get("username", "bridge"),
            mqtt_password=password,
            mqtt_client_id=mqtt_cfg.get("client_id", "bridge-host"),
            udp_host=udp_cfg.get("host", "127.0.0.1"),
            udp_port=int(udp_cfg.get("port", 7001)),
            control_port=int(udp_cfg.get("control_port", 7002)),
            telemetry_hz=float(raw.get("telemetry_hz", 104)),
            udp_hz=float(raw.get("udp_hz", 60)),
            stale_ms=float(raw.get("stale_ms", 250)),
            event_ttl_ms=float(raw.get("event_ttl_ms", 200)),
            probe_interval_ms=float(raw.get("probe_interval_ms", 2000)),
            probe_ttl_ms=float(raw.get("probe_ttl_ms", 5000)),
            probe_rtt_max_ms=float(raw.get("probe_rtt_max_ms", 50)),
            clock_margin_ms=float(raw.get("clock_margin_ms", 10)),
            deadzone_deg=float(raw.get("deadzone_deg", 3)),
            saturation_deg=float(raw.get("saturation_deg", 25)),
            still_window_ms=float(raw.get("still_window_ms", 500)),
            still_gyro_rps=float(raw.get("still_gyro_rps", 0.12)),
            still_acc_tol=float(raw.get("still_acc_tol", 0.15)),
            require_recenter=bool(raw.get("require_recenter", True)),
            force_single_axis=bool(raw.get("force_single_axis", True)),
            forward_output_sign=float(raw.get("forward_output_sign", 1.0)),
            require_level_recenter=bool(raw.get("require_level_recenter", False)),
            recenter_level_max_deg=float(raw.get("recenter_level_max_deg", 10.0)),
            button_jump_enabled=bool(raw.get("button_jump_enabled", False)),
            fusion_tau_s=float(fusion.get("tau_s", 0.5)),
            gyro_pitch_sign=int(fusion.get("gyro_pitch_sign", -1)),
            gyro_roll_sign=int(fusion.get("gyro_roll_sign", 1)),
            log_dir=log_cfg.get("dir", "tests/logs"),
            console_enabled=bool(raw.get("console", {}).get("enabled", True)),
            console_status_s=float(raw.get("console", {}).get("status_s", 5)),
            devices=devices,
        )


# --------------------------------------------------------------------------- 日志
class JsonlLog:
    def __init__(self, path: Path, max_bytes: int = 20 * 1024 * 1024, backups: int = 5):
        self.path = Path(path)
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.max_bytes = max_bytes
        self.backups = backups
        self._lock = threading.Lock()
        self._fh = open(self.path, "a", encoding="utf-8")
        self._size = self.path.stat().st_size

    def write(self, event: str, **fields) -> None:
        record = {"ts": time.time(), "event": event, **fields}
        line = json.dumps(record, ensure_ascii=False, default=str) + "\n"
        with self._lock:
            self._fh.write(line)
            self._fh.flush()
            self._size += len(line.encode("utf-8"))
            if self._size >= self.max_bytes:
                self._rotate()

    def _rotate(self) -> None:
        self._fh.close()
        for i in range(self.backups - 1, 0, -1):
            src = self.path.with_suffix(self.path.suffix + f".{i}")
            dst = self.path.with_suffix(self.path.suffix + f".{i + 1}")
            if src.exists():
                os.replace(src, dst)
        os.replace(self.path, self.path.with_suffix(self.path.suffix + ".1"))
        self._fh = open(self.path, "a", encoding="utf-8")
        self._size = 0

    def close(self) -> None:
        with self._lock:
            self._fh.close()


# --------------------------------------------------------------------------- 桥接
class _TeeLog:
    """JSONL 落盘 + 关键事件同步打印到控制台（终端不再是"哑的"）。"""

    def __init__(self, inner, mirror):
        self.inner = inner
        self.mirror = mirror

    def write(self, event: str, **fields) -> None:
        self.inner.write(event, **fields)
        try:
            self.mirror(event, fields)
        except Exception:
            pass

    def close(self) -> None:
        self.inner.close()


class Bridge:
    def __init__(self, cfg: BridgeConfig, *, client_factory=None, log=None,
                 console: bool = True):
        self.cfg = cfg
        self.bridge_id = f"b-{uuid.uuid4().hex[:8]}"
        self._start_mono = mono()
        raw_log = log or JsonlLog(Path(cfg.log_dir) / "bridge.jsonl")
        self.console = console and cfg.console_enabled
        self.log = _TeeLog(raw_log, self._console_event) if self.console else raw_log
        self.states: dict[str, DeviceState] = {}
        self.raw_queues: dict[str, collections.deque] = {}
        self.event_queue: collections.deque = collections.deque(maxlen=256)
        self.stop_event = threading.Event()
        self._threads: list[threading.Thread] = []
        self.udp_seq: dict[str, int] = {}
        self.sent_frames = 0
        self.sent_errors = 0
        self._client_factory = client_factory
        self.client = None
        self.mqtt_connected = False
        self._connect_warned = False
        self._event_seen: dict[str, int] = {}
        self.udp_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.control_sock: socket.socket | None = None
        self.started_wall = time.time()

        for dev_cfg in cfg.devices.values():
            self._state_for(dev_cfg.device_id)

    # -- 控制台反馈 ---------------------------------------------------------
    _QUIET_EVENTS = {"probe_sent"}

    def _console_event(self, event: str, fields: dict) -> None:
        if event in self._QUIET_EVENTS:
            return
        seen = self._event_seen.get(event, 0) + 1
        self._event_seen[event] = seen
        if event in ("raw_rejected", "event_rejected", "status_rejected",
                     "ack_rejected", "bad_json") and seen > 5 and seen % 200:
            return
        if event in ("raw_dropped",) and seen > 3 and seen % 200 == 0:
            LOG.info("raw_dropped 累计 %d 次", seen)
            return
        if event in ("raw_dropped",):
            return

        dev = fields.get("device_id", "")
        if event == "bridge_start":
            LOG.info("网关启动 bridge_id=%s  UDP->%s  控制口=%s  需回中=%s",
                     self.bridge_id, fields.get("udp"), fields.get("control_port"),
                     fields.get("require_recenter"))
            LOG.info("等待 MQTT %s:%s ...", self.cfg.mqtt_host, self.cfg.mqtt_port)
        elif event == "mqtt_connected":
            LOG.info("MQTT 已连接 %s", fields.get("broker"))
        elif event == "mqtt_connect_failed":
            LOG.warning("MQTT 连接被拒: %s（检查账号/ACL）", fields.get("reason"))
        elif event == "mqtt_disconnected":
            if self.stop_event.is_set():
                LOG.info("MQTT 已断开（网关退出）")
            else:
                LOG.warning("MQTT 断开: %s（会自动重连）", fields.get("reason"))
        elif event == "session_adopted":
            LOG.info("设备 %s 会话确认 boot_id=%s epoch=%s", dev,
                     fields.get("boot_id"), fields.get("link_epoch"))
        elif event == "probe_ok":
            LOG.info("设备 %s 时间探测成功 rtt=%.1f ms", dev, fields.get("rtt_ms") or 0.0)
        elif event == "probe_rtt_exceeded":
            LOG.warning("设备 %s 探测 RTT 超限 %.1f ms（>%.0f）", dev,
                        fields.get("rtt_ms") or 0.0, self.cfg.probe_rtt_max_ms)
        elif event == "recenter_not_ready":
            LOG.warning("Recenter STALE device=%s reason=%s age=%s bound=%s", dev,
                        fields.get("reason"), fields.get("source_age_ms"),
                        fields.get("source_bound_ms"))
        elif event == "recenter_result":
            code = fields.get("code")
            if fields.get("ok"):
                LOG.info("设备 %s 回中成功 (%s)", dev, code)
            else:
                LOG.warning("设备 %s 回中被拒: %s", dev, code)
        elif event == "jump_consumed":
            LOG.info("设备 %s 跳跃令牌 %s 已确认", dev, fields.get("token"))
        elif event == "bridge_stop":
            LOG.info("网关退出：发送 %s 帧，错误 %s 次",
                     fields.get("udp_frames"), fields.get("udp_errors"))
        elif event.endswith("_rejected"):
            LOG.warning("%s %s 被拒: %s（第 %d 次）", event, dev,
                        fields.get("code"), seen)
        elif event == "bad_json":
            LOG.warning("非法 JSON 丢弃 device=%s（第 %d 次）", dev, seen)

    # -- 设备状态 -----------------------------------------------------------
    def _state_for(self, device_id: str) -> DeviceState:
        state = self.states.get(device_id)
        if state is not None:
            return state
        dev_cfg = self.cfg.devices.get(device_id) or DeviceConfig(device_id=device_id)
        state = DeviceState(dev_cfg, self.cfg)
        state.log_fn = self._state_log
        self.states[device_id] = state
        self.raw_queues[device_id] = collections.deque(maxlen=8)
        self.udp_seq[device_id] = 0
        return state

    def _state_log(self, event: str, **fields) -> None:
        self.log.write(event, **fields)

    # -- MQTT ---------------------------------------------------------------
    def _build_client(self):
        from . import mqtt_io
        return mqtt_io.make_client(
            client_id=f"{self.cfg.mqtt_client_id}-{uuid.uuid4().hex[:4]}",
            username=self.cfg.mqtt_username,
            password=self.cfg.mqtt_password,
            on_connect=self._on_connect,
            on_message=self._on_message,
            on_disconnect=self._on_disconnect,
        )

    def _on_connect(self, client, userdata, flags, reason_code, properties=None):
        if reason_code != 0:
            self.log.write("mqtt_connect_failed", reason=str(reason_code))
            return
        client.subscribe([
            ("tiltplay/v2/+/telemetry/raw", 0),
            ("tiltplay/v2/+/event", 1),
            ("tiltplay/v2/+/status", 1),
            ("tiltplay/v2/+/ack", 1),
        ])
        self.log.write("mqtt_connected", broker=f"{self.cfg.mqtt_host}:{self.cfg.mqtt_port}")
        self.mqtt_connected = True

    def _on_disconnect(self, client, userdata, flags, reason_code, properties=None):
        self.mqtt_connected = False
        self.log.write("mqtt_disconnected", reason=str(reason_code))

    def _on_message(self, client, userdata, msg):
        if len(msg.payload) > protocol.MAX_MQTT_PAYLOAD:
            self.log.write("payload_too_long", topic=msg.topic, size=len(msg.payload))
            return
        try:
            device_id, kind = protocol.parse_topic(msg.topic)
        except ProtocolError as exc:
            self.log.write("topic_rejected", topic=msg.topic, code=exc.code)
            return
        try:
            payload = json.loads(msg.payload.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            self.log.write("bad_json", topic=msg.topic, device_id=device_id)
            return

        now = mono()
        state = self._state_for(device_id)
        if kind == "raw":
            if len(self.raw_queues[device_id]) == self.raw_queues[device_id].maxlen:
                state.counters.queue_drop += 1
            try:
                frame = protocol.parse_raw(payload, device_id, len(msg.payload))
            except ProtocolError as exc:
                state.counters.raw_bad += 1
                self.log.write("raw_rejected", device_id=device_id, code=exc.code,
                               detail=exc.detail)
                return
            self.raw_queues[device_id].append((frame, now))
        elif kind == "status":
            try:
                parsed = protocol.parse_status(payload, device_id)
            except ProtocolError as exc:
                self.log.write("status_rejected", device_id=device_id, code=exc.code)
                return
            self.event_queue.append(("status", device_id, parsed, now))
        elif kind == "event":
            try:
                parsed = protocol.parse_event(payload, device_id)
            except ProtocolError as exc:
                state.counters.event_bad += 1
                self.log.write("event_rejected", device_id=device_id, code=exc.code)
                return
            self.event_queue.append(("event", device_id, parsed, now))
        elif kind == "ack":
            try:
                parsed = protocol.parse_ack(payload)
            except ProtocolError as exc:
                self.log.write("ack_rejected", device_id=device_id, code=exc.code)
                return
            self.event_queue.append(("ack", device_id, parsed, now))

    # -- 处理线程 -----------------------------------------------------------
    def _drain(self) -> None:
        # 控制/状态/探测响应优先，避免被 104 Hz 遥测淹没
        while self.event_queue:
            kind, device_id, payload, now = self.event_queue.popleft()
            state = self._state_for(device_id)
            if kind == "status":
                state.on_status(payload, now)
            elif kind == "event":
                result = state.on_event(payload, now)
                self.log.write("event_result", device_id=device_id, result=result,
                               press_count=payload.get("press_count"))
            elif kind == "ack":
                state.on_ack(payload, now)

        for device_id, queue in self.raw_queues.items():
            state = self.states[device_id]
            while queue:
                frame, now = queue.popleft()
                result = state.on_raw(frame, now)
                if result not in ("ok", "duplicate", "duplicate_sample"):
                    self.log.write("raw_dropped", device_id=device_id, result=result)

    def _drain_loop(self) -> None:
        while not self.stop_event.is_set():
            self._drain()
            self.stop_event.wait(0.002)

    def _probe_loop(self) -> None:
        while not self.stop_event.is_set():
            now = mono()
            for state in list(self.states.values()):
                try:
                    if state.needs_probe(now):
                        cmd_id = f"probe-{uuid.uuid4().hex[:8]}"
                        state.on_probe_sent(cmd_id, now)
                        self.log.write("probe_sent", device_id=state.cfg.device_id,
                                       cmd_id=cmd_id, boot_id=state.boot_id)
                        self._publish_cmd(state, {
                            "schema": 2,
                            "device_id": state.cfg.device_id,
                            "boot_id": state.boot_id,
                            "link_epoch": state.link_epoch,
                            "cmd_id": cmd_id,
                            "op": "ping",
                            "args": {},
                            "deadline_t_us": (state.last_sample_t_us or 0) + 500_000,
                        })
                except Exception:  # 单设备异常不影响其他设备
                    LOG.exception("probe loop error")
            self.stop_event.wait(0.1)

    def _publish_cmd(self, state: DeviceState, payload: dict) -> None:
        if self.client is None:
            return
        topic = f"tiltplay/v2/{state.cfg.device_id}/cmd"
        self.client.publish(topic, json.dumps(payload), qos=1)

    def _send_loop(self) -> None:
        period = 1.0 / max(self.cfg.udp_hz, 1.0)
        next_tick = mono()
        status_period = self.cfg.console_status_s
        next_status = mono() + status_period if self.console and status_period > 0 else None
        last_frames = 0
        last_status_t = mono()
        while not self.stop_event.is_set():
            now = mono()
            for state in list(self.states.values()):
                frame = self._udp_frame(state, now)
                if frame is None:
                    continue
                blob = json.dumps(frame, separators=(",", ":")).encode("utf-8")
                if len(blob) > protocol.MAX_UDP_PAYLOAD:
                    self.sent_errors += 1
                    continue
                try:
                    self.udp_sock.sendto(blob, (self.cfg.udp_host, self.cfg.udp_port))
                    self.sent_frames += 1
                except OSError:
                    self.sent_errors += 1
            if next_status is not None and now >= next_status:
                self._print_status(now, last_frames, last_status_t)
                last_frames = self.sent_frames
                last_status_t = now
                next_status = now + status_period
            next_tick += period
            delay = next_tick - mono()
            if delay < 0:
                next_tick = mono()
                delay = 0
            self.stop_event.wait(delay)

    def _print_status(self, now: float, last_frames: int, last_t: float) -> None:
        """周期性状态行：一眼看出到底是"没数据"还是"数据无效"。"""
        elapsed = max(now - last_t, 1e-6)
        rate = (self.sent_frames - last_frames) / elapsed
        parts = []
        for state in self.states.values():
            snap = state.snapshot(now)
            parts.append(
                f"{state.cfg.device_id}: valid={snap['valid']} "
                f"age={snap['source_age_ms']}ms move={snap['move']}"
            )
        LOG.info("[%.0fs] %.1f Hz  %s", now - self._start_mono, rate,
                 " | ".join(parts) if parts else "无设备")
        if not self.mqtt_connected and not self._connect_warned and \
                now - self._start_mono > 5.0:
            self._connect_warned = True
            LOG.warning("还没连上 broker %s:%s —— 检查 mqtt.host / 端口 / 账号 ACL；"
                        "进程会一直重试，不会退出", self.cfg.mqtt_host, self.cfg.mqtt_port)

    def _udp_frame(self, state: DeviceState, now: float) -> dict | None:
        if state.boot_id is None:
            return None
        snapshot = state.snapshot(now)
        seq = self.udp_seq[state.cfg.device_id]
        self.udp_seq[state.cfg.device_id] = (seq + 1) & 0xFFFFFFFF
        result = {
            "schema": 2,
            "type": "input",
            "bridge_id": self.bridge_id,
            "device_id": state.cfg.device_id,
            "boot_id": state.boot_id,
            "link_epoch": state.link_epoch,
            "seq": seq,
            "source_seq": state.last_sample_seq if state.last_sample_seq is not None else 0,
            "source_t_us": state.last_sample_t_us if state.last_sample_t_us is not None else 0,
            "bridge_t_us": int((now - self._start_mono) * 1e6),
            "source_age_ms": snapshot["source_age_ms"],
            "source_bound_ms": snapshot["source_bound_ms"],
            "valid": snapshot["valid"],
            "mode": state.cfg.mode,
            "move": snapshot["move"],
            "angles_deg": snapshot["angles_deg"],
            "jump_token": snapshot["jump_token"],
            "jump_age_bound_ms": snapshot["jump_age_bound_ms"],
        }
        if state.cfg.vertical_step_enabled:
            result["vertical_step"] = state.step_snapshot(now)
        return result

    # -- 控制口 7002 --------------------------------------------------------
    def _control_loop(self) -> None:
        assert self.control_sock is not None
        self.control_sock.settimeout(0.2)
        while not self.stop_event.is_set():
            try:
                data, addr = self.control_sock.recvfrom(2048)
            except socket.timeout:
                continue
            except OSError:
                break
            if len(data) > protocol.MAX_UDP_PAYLOAD:
                continue
            try:
                msg = json.loads(data.decode("utf-8"))
            except (UnicodeDecodeError, json.JSONDecodeError):
                self.log.write("control_bad_json", addr=str(addr))
                continue
            if not isinstance(msg, dict):
                continue
            kind = msg.get("type")
            if kind == "control":
                self._handle_control(msg, addr)
            elif kind == "input_ack":
                self._handle_input_ack(msg, addr)
            elif kind == "vertical_step_ack":
                self._handle_step_ack(msg)

    def _handle_step_ack(self, msg):
        state = self.states.get(msg.get('device_id'))
        if state is None or msg.get('schema') != 2:
            return
        if (msg.get('bridge_id') != self.bridge_id or msg.get('boot_id') != state.boot_id
                or type(msg.get('link_epoch')) is not int
                or msg['link_epoch'] != state.link_epoch):
            return
        token = msg.get('token')
        if (type(token) is int and state.pending_step is not None
                and token == state.pending_step[0]):
            state.pending_step = None

    def _handle_control(self, msg: dict, addr) -> None:
        request_id = msg.get("request_id")
        device_id = msg.get("device_id")
        reply = {
            "schema": 2, "type": "control_ack",
            "request_id": request_id, "bridge_id": self.bridge_id,
            "device_id": device_id, "ok": False, "code": "REJECTED",
        }
        if msg.get("bridge_id") not in (None, self.bridge_id):
            reply["code"] = "BRIDGE_MISMATCH"
        elif msg.get("op") != "recenter":
            reply["code"] = "UNSUPPORTED_OP"
        elif device_id not in self.states:
            reply["code"] = "UNKNOWN_DEVICE"
        else:
            state = self.states[device_id]
            reply["boot_id"] = state.boot_id
            reply["link_epoch"] = state.link_epoch
            if msg.get("boot_id") not in (None, state.boot_id):
                reply["code"] = "SESSION_MISMATCH"
            elif msg.get("link_epoch") not in (None, state.link_epoch):
                reply["code"] = "EPOCH_MISMATCH"
            else:
                code = state.recenter(mono())
                reply["ok"] = code == "CENTERED"
                reply["code"] = code
        self._send_control(reply, addr)
        self.log.write("recenter_result", request_id=request_id, device_id=device_id,
                       ok=reply["ok"], code=reply["code"])

    def _handle_input_ack(self, msg: dict, addr) -> None:
        device_id = msg.get("device_id")
        state = self.states.get(device_id)
        if state is None:
            return
        if msg.get("bridge_id") not in (None, self.bridge_id):
            return
        token = msg.get("jump_token")
        if isinstance(token, int) and state.consume_jump(token):
            self.log.write("jump_consumed", device_id=device_id, token=token)

    def _send_control(self, payload: dict, addr) -> None:
        try:
            blob = json.dumps(payload).encode("utf-8")
            assert self.control_sock is not None
            self.control_sock.sendto(blob, addr)
        except OSError:
            pass

    # -- 生命周期 -----------------------------------------------------------
    def start(self, *, with_mqtt: bool = True) -> None:
        self.control_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.control_sock.bind((self.cfg.udp_host, self.cfg.control_port))
        self.log.write("bridge_start", bridge_id=self.bridge_id,
                       udp=f"{self.cfg.udp_host}:{self.cfg.udp_port}",
                       control_port=self.control_sock.getsockname()[1],
                       require_recenter=self.cfg.require_recenter)
        if with_mqtt:
            self.client = (self._client_factory or self._build_client)()
            self.client.connect_async(self.cfg.mqtt_host, self.cfg.mqtt_port,
                                      keepalive=15)
            self.client.loop_start()
        for target in (self._drain_loop, self._probe_loop, self._send_loop, self._control_loop):
            thread = threading.Thread(target=target, daemon=True)
            thread.start()
            self._threads.append(thread)

    def stop(self) -> None:
        self.stop_event.set()
        for thread in self._threads:
            thread.join(timeout=2.0)
        if self.client is not None:
            try:
                self.client.loop_stop()
                self.client.disconnect()
            except Exception:
                pass
        for sock in (self.control_sock, self.udp_sock):
            try:
                if sock is not None:
                    sock.close()
            except OSError:
                pass
        self.log.write("bridge_stop", bridge_id=self.bridge_id,
                       udp_frames=self.sent_frames, udp_errors=self.sent_errors)
        self.log.close()

    def run_forever(self) -> None:
        self.start()
        try:
            while not self.stop_event.wait(0.5):
                pass
        except KeyboardInterrupt:
            pass
        finally:
            self.stop()


def configure_console_logging(verbose: bool = False) -> None:
    handler = logging.StreamHandler(sys.stdout)
    handler.setFormatter(logging.Formatter("%(asctime)s %(levelname)s %(name)s: %(message)s"))
    root = logging.getLogger()
    root.handlers[:] = [handler]
    root.setLevel(logging.DEBUG if verbose else logging.INFO)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="TiltPlay IoT 数据网关")
    parser.add_argument("--config", default="config/bridge.json")
    parser.add_argument("--no-mqtt", action="store_true", help="仅用于离线管线调试")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args(argv)
    configure_console_logging(args.verbose)
    cfg = BridgeConfig.load(args.config)
    bridge = Bridge(cfg)
    signal.signal(signal.SIGINT, lambda *_: bridge.stop_event.set())
    bridge.run_forever()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
