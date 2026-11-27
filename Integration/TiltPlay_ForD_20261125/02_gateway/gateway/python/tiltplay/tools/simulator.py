"""模拟控制器（无硬件时的 A 部分替身）。

发布 schema=2 的 status/telemetry/raw，响应 cmd/ping，按脚本产生倾斜与按键。
默认行为已与 `tiltplay.bridge` 的校验和时序对齐，可直接用于 B/C/D 并行开发。
"""
from __future__ import annotations

import argparse
import json
import math
import sys
import threading
import time
import uuid

G = 9.80665


class MotionProfile:
    """脚本化运动：前后 ±20°、右倾 +15°，带斜坡以保证 dt 与角速度自洽。"""

    def __init__(self, period_s: float = 12.0):
        self.period_s = period_s

    def angles(self, t: float) -> tuple[float, float]:
        x = t % self.period_s
        if x < 1.0:
            return 0.0, 0.0
        if x < 2.0:
            return 20.0 * (x - 1.0), 0.0
        if x < 4.0:
            return 20.0, 0.0
        if x < 5.0:
            return 20.0 - 40.0 * (x - 4.0), 0.0
        if x < 7.0:
            return -20.0, 0.0
        if x < 8.0:
            return -20.0 + 20.0 * (x - 7.0), 0.0
        if x < 9.0:
            return 0.0, 15.0 * (x - 8.0)
        if x < 11.0:
            return 0.0, 15.0
        return 0.0, max(0.0, 15.0 - 15.0 * (x - 11.0))


class DeviceSimulator:
    def __init__(self, *, device_id: str = "nano-a1b2c3d4e5f6",
                 boot_id: str | None = None, link_epoch: int = 1,
                 host: str = "127.0.0.1", port: int = 1883,
                 username: str | None = None, password: str | None = None,
                 hz: float = 104.0, profile: MotionProfile | None = None,
                 start_angle: tuple[float, float] = (0.0, 0.0),
                 noise_deg: float = 0.05):
        self.device_id = device_id
        self.boot_id = boot_id or uuid.uuid4().hex[:16]
        self.link_epoch = link_epoch
        self.host = host
        self.port = port
        self.hz = hz
        self.profile = profile or MotionProfile()
        self.start_angle = start_angle
        self.noise_deg = noise_deg
        self.seq = 0
        self.sample_seq = 0
        self.press_count = 0
        self.press_t_us: int | None = None
        self.online = True
        self._t0 = time.perf_counter()
        self._last_angles = (0.0, 0.0)
        self._last_t = 0.0
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self.client = None
        self.published = 0
        self._username = username or self.device_id
        self._password = password

    # -- 时间 ---------------------------------------------------------------
    def device_time_us(self) -> int:
        return int((time.perf_counter() - self._t0) * 1e6)

    def elapsed(self) -> float:
        return time.perf_counter() - self._t0

    # -- MQTT ---------------------------------------------------------------
    def connect(self):
        from tiltplay import mqtt_io
        self.client = mqtt_io.make_client(
            client_id=f"dev-{self.device_id}",
            username=self._username, password=self._password,
            on_connect=self._on_connect, on_message=self._on_message,
        )
        self.client.connect(self.host, self.port, keepalive=15)
        self.client.loop_start()
        return self

    def _on_connect(self, client, userdata, flags, reason_code, properties=None):
        if reason_code == 0:
            client.subscribe(f"tiltplay/v2/{self.device_id}/cmd", qos=1)

    def _on_message(self, client, userdata, msg):
        try:
            cmd = json.loads(msg.payload.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            return
        try:
            if cmd.get("op") == "ping":
                self.publish_ack(cmd_id=cmd.get("cmd_id"), ok=True, code="PONG",
                                 boot_id=cmd.get("boot_id"),
                                 link_epoch=cmd.get("link_epoch"))
        except Exception as exc:  # 模拟器不因单条命令崩溃
            print(f"[simulator] cmd handler error: {exc}", file=sys.stderr)

    def _publish(self, suffix: str, payload: dict, qos: int, retain: bool = False) -> None:
        if self.client is None:
            return
        self.client.publish(f"tiltplay/v2/{self.device_id}/{suffix}",
                            json.dumps(payload, separators=(",", ":")),
                            qos=qos, retain=retain)

    def publish_status(self, online: bool | None = None, retain: bool = True) -> None:
        if online is not None:
            self.online = online
        self._publish("status", {
            "schema": 2, "device_id": self.device_id, "boot_id": self.boot_id,
            "link_epoch": self.link_epoch, "online": self.online,
            "firmware": "0.1.0-sim", "capabilities": ["raw6", "button"],
        }, qos=1, retain=retain)

    def publish_ack(self, cmd_id: str, ok: bool, code: str,
                    boot_id: str | None = None, link_epoch: int | None = None) -> None:
        self._publish("ack", {
            "schema": 2, "device_id": self.device_id,
            "boot_id": boot_id or self.boot_id,
            "link_epoch": link_epoch if link_epoch is not None else self.link_epoch,
            "cmd_id": cmd_id, "ok": ok, "code": code, "t_us": self.device_time_us(),
        }, qos=1)

    # -- 遥测 ---------------------------------------------------------------
    def _accel(self, f_deg: float, r_deg: float) -> tuple[float, float, float]:
        f, r = math.radians(f_deg), math.radians(r_deg)
        ax = -G * math.sin(f) * math.cos(r)
        ay = -G * math.sin(r)
        az = G * math.cos(f) * math.cos(r)
        return ax, ay, az

    def sample(self, t: float) -> tuple[tuple[float, float, float], tuple[float, float, float]]:
        f_deg, r_deg = self.profile.angles(t)
        f_deg += self.start_angle[0]
        r_deg += self.start_angle[1]
        if self.noise_deg:
            f_deg += self.noise_deg * math.sin(t * 37.0)
            r_deg += self.noise_deg * math.cos(t * 29.0)
        dt = max(t - self._last_t, 1e-3)
        df = (f_deg - self._last_angles[0]) / dt
        dr = (r_deg - self._last_angles[1]) / dt
        self._last_angles = (f_deg, r_deg)
        self._last_t = t
        acc = self._accel(f_deg, r_deg)
        gyro = (math.radians(dr), math.radians(-df), 0.0)
        return acc, gyro

    def publish_raw(self, acc=None, gyro=None, *, t: float | None = None,
                    press_count: int | None = None) -> int:
        if t is None:
            t = time.perf_counter() - self._t0
        if acc is None or gyro is None:
            acc, gyro = self.sample(t)
        self.seq = (self.seq + 1) & 0xFFFFFFFF
        self.sample_seq = (self.sample_seq + 1) & 0xFFFFFFFF
        payload = {
            "schema": 2, "device_id": self.device_id, "boot_id": self.boot_id,
            "link_epoch": self.link_epoch, "seq": self.seq,
            "sample_seq": self.sample_seq, "t_us": self.device_time_us(),
            "acc_mps2": [round(v, 5) for v in acc],
            "gyro_rps": [round(v, 6) for v in gyro],
            "button": {
                "jump": False,
                "press_count": self.press_count if press_count is None else press_count,
                "press_t_us": self.press_t_us,
            },
            "sample_hz": self.hz,
        }
        self._publish("telemetry/raw", payload, qos=0)
        self.published += 1
        return self.seq

    def press(self, count: int | None = None) -> int:
        self.press_count = (self.press_count + 1) & 0xFFFFFFFF if count is None else count
        self.press_t_us = self.device_time_us()
        self._publish("event", {
            "schema": 2, "device_id": self.device_id, "boot_id": self.boot_id,
            "link_epoch": self.link_epoch, "seq": (self.seq + 1) & 0xFFFFFFFF,
            "t_us": self.press_t_us, "event_id": f"press-{self.press_count}",
            "type": "jump_pressed", "press_count": self.press_count,
            "press_t_us": self.press_t_us,
        }, qos=1)
        return self.press_count

    # -- 运行 ---------------------------------------------------------------
    def run_for(self, duration_s: float) -> None:
        period = 1.0 / self.hz
        next_tick = time.perf_counter()
        end = next_tick + duration_s
        while time.perf_counter() < end and not self._stop.is_set():
            self.publish_raw()
            next_tick += period
            delay = next_tick - time.perf_counter()
            if delay > 0:
                time.sleep(delay)
            else:
                next_tick = time.perf_counter()

    def start_background(self, duration_s: float | None = None) -> None:
        def run():
            if duration_s is None:
                while not self._stop.is_set():
                    self.run_for(0.5)
            else:
                self.run_for(duration_s)

        self._thread = threading.Thread(target=run, daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=2.0)
        if self.client is not None:
            try:
                self.publish_status(online=False, retain=True)
                self.client.loop_stop()
                self.client.disconnect()
            except Exception:
                pass


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="TiltPlay 模拟控制器")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=1883)
    parser.add_argument("--device", default="nano-a1b2c3d4e5f6")
    parser.add_argument("--hz", type=float, default=104.0)
    parser.add_argument("--seconds", type=float, default=30.0)
    parser.add_argument("--press-at", type=float, nargs="*", default=[3.0])
    args = parser.parse_args(argv)

    sim = DeviceSimulator(device_id=args.device, host=args.host, port=args.port, hz=args.hz)
    sim.connect()
    sim.publish_status(True)
    start = time.perf_counter()
    pressed = set()
    while time.perf_counter() - start < args.seconds:
        sim.publish_raw()
        elapsed = time.perf_counter() - start
        for idx, at in enumerate(args.press_at):
            if idx not in pressed and elapsed >= at:
                pressed.add(idx)
                sim.press()
        time.sleep(max(0.0, 1.0 / args.hz - 0.001))
    sim.stop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

