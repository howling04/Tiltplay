"""端到端离线演示：内置 broker + 模拟控制器 + 网关 + UDP 探针。

用途:
1. 无硬件时验证 B 部分全链路（协议、会话、探测、回中、60 Hz 输出、失联停控）。
2. 生成可复现的演示数据与 JSON 报告（tests/reports/），供进度汇报与验收记录引用。

运行: python -m tiltplay.tools.demo --seconds 12
"""
from __future__ import annotations

import argparse
import json
import socket
import statistics
import sys
import threading
import time
from dataclasses import replace
from pathlib import Path

import sys as _sys
_sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "python" / "tests"))

from tiltplay.bridge import Bridge, BridgeConfig, configure_console_logging  # noqa: E402
from tiltplay.tools.simulator import DeviceSimulator, MotionProfile  # noqa: E402

try:
    from mqtt_stub import MqttStubBroker
except ImportError:  # 作为包被引用时
    from .mqtt_stub import MqttStubBroker  # type: ignore


class OffsetProfile(MotionProfile):
    """前 delay 秒保持水平，给探测与回中留出静置窗口。"""

    def __init__(self, delay: float = 3.0, period_s: float = 8.0):
        super().__init__(period_s)
        self.delay = delay

    def angles(self, t: float):
        if t < self.delay:
            return 0.0, 0.0
        return super().angles(t - self.delay)


class UdpCollector:
    def __init__(self, port: int = 0):
        """port=0 时由系统分配空闲端口，避免与 UE 的 7001 或上一次演示冲突。"""
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", port))
        self.port = self.sock.getsockname()[1]
        self.sock.settimeout(0.2)
        self.frames: list[dict] = []
        self.times: list[float] = []
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)

    def start(self):
        self._thread.start()
        return self

    def _run(self):
        while not self._stop.is_set():
            try:
                data, _ = self.sock.recvfrom(2048)
            except socket.timeout:
                continue
            except OSError:
                return
            try:
                frame = json.loads(data.decode("utf-8"))
            except (UnicodeDecodeError, json.JSONDecodeError):
                continue
            self.frames.append(frame)
            self.times.append(time.monotonic())

    def stop(self):
        self._stop.set()
        self._thread.join(timeout=1.0)
        self.sock.close()


def send_recenter(port: int, device_id: str, request_id: str = "ui-demo") -> dict:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(0.3)
    try:
        sock.sendto(json.dumps({
            "schema": 2, "type": "control", "request_id": request_id,
            "op": "recenter", "device_id": device_id,
        }).encode(), ("127.0.0.1", port))
        data, _ = sock.recvfrom(2048)
        return json.loads(data.decode())
    finally:
        sock.close()


def send_input_ack(port: int, frame: dict) -> None:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.sendto(json.dumps({
            "schema": 2, "type": "input_ack", "bridge_id": frame["bridge_id"],
            "device_id": frame["device_id"], "boot_id": frame["boot_id"],
            "link_epoch": frame["link_epoch"], "jump_token": frame["jump_token"],
        }).encode(), ("127.0.0.1", port))
    finally:
        sock.close()


def run_demo(seconds: float = 12.0, *, quiet: bool = False,
             udp_port: int = 0, control_port: int = 0) -> dict:
    """端口默认为 0（自动分配），不会占用 UE 的 7001/7002，可与其他实例并存。"""
    if not quiet:
        configure_console_logging()
    broker = MqttStubBroker().start()
    collector = UdpCollector(udp_port)
    cfg = BridgeConfig.load(Path(__file__).resolve().parents[3] / "config" / "bridge.json")
    cfg = replace(cfg, mqtt_host="127.0.0.1", mqtt_port=broker.port,
                  udp_port=collector.port, control_port=control_port,
                  log_dir=str(Path(__file__).resolve().parents[3] / "tests" / "logs"),
                  button_jump_enabled=True)
    bridge = Bridge(cfg, console=not quiet)
    collector.start()
    bridge.start()
    control_port = bridge.control_sock.getsockname()[1]

    sim = DeviceSimulator(device_id="nano-a1b2c3d4e5f6", host="127.0.0.1",
                          port=broker.port, hz=104.0, profile=OffsetProfile())
    sim.connect()
    sim.publish_status(True)
    sim.start_background()

    recenter_result = None
    jump_token = None
    acked = False
    start = time.monotonic()
    from .startup import StartupRecenter
    startup = StartupRecenter(start)
    def startup_ready():
        state = bridge.states.get(sim.device_id)
        return state is not None and state.valid(ignore_center=True) and state.still_ok()
    def startup_send(attempt):
        try:
            return send_recenter(control_port, sim.device_id, f"startup-{attempt}")
        except socket.timeout:
            return {"ok": False, "code": "TIMEOUT"}
    stop_publishing_at = start + seconds - 1.5

    while time.monotonic() - start < seconds:
        now = time.monotonic()
        startup.tick(now, startup_ready, startup_send)
        recenter_result = startup.result
        if now >= start + 5.2 and jump_token is None:
            token = sim.press()
            jump_token = token
        if jump_token is not None and not acked:
            for frame, ts in zip(collector.frames, collector.times):
                if frame.get("jump_token") == jump_token:
                    send_input_ack(control_port, frame)
                    acked = True
                    break
        if now >= stop_publishing_at and sim._thread and sim._thread.is_alive():
            sim._stop.set()
        time.sleep(0.05)

    sim.stop()
    time.sleep(0.6)
    bridge.stop()
    collector.stop()
    broker.stop()

    frames = collector.frames
    report = summarise(frames, collector.times, cfg, recenter_result, jump_token, acked,
                       seconds, stop_publishing_at - start)
    report["bridge_frames_sent"] = bridge.sent_frames
    report["broker_published"] = broker.published
    report["sim_published"] = sim.published
    report["udp_port"] = collector.port
    report["control_port"] = control_port
    report["log"] = str(Path(cfg.log_dir) / "bridge.jsonl")
    if not quiet:
        print_report(report)
    out_dir = Path(__file__).resolve().parents[3] / "tests" / "reports"
    out_dir.mkdir(parents=True, exist_ok=True)
    out_path = out_dir / f"demo_{time.strftime('%Y%m%d_%H%M%S')}.json"
    out_path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    report["report_path"] = str(out_path)
    return report


def summarise(frames, times, cfg, recenter_result, jump_token, acked, seconds,
              stop_at: float | None = None) -> dict:
    if not frames:
        return {"ok": False, "reason": "no UDP frames", "frames": 0}
    duration = times[-1] - times[0] if len(times) > 1 else 0.0
    valid = [f for f in frames if f.get("valid")]
    moves = [f["move"][0] for f in frames]
    rights = [f["move"][1] for f in frames]
    seqs = [f["seq"] for f in frames]
    gaps = sum(1 for a, b in zip(seqs, seqs[1:]) if b != (a + 1) & 0xFFFFFFFF)
    udp_sizes = [len(json.dumps(f).encode()) for f in frames]

    base = times[0]
    window_start = base + (stop_at - 1.0 if stop_at is not None else 0.0)
    window = [(t, f["valid"]) for t, f in zip(times, frames) if t >= window_start]
    last_valid = max((t for t, v in window if v), default=None)
    first_invalid = next((t for t, v in window if last_valid and t > last_valid and not v), None)
    # 停控时延应从"最后一个新样本到达"算起，而不是从"最后一帧 valid"算起：
    # 后者只差一帧(约 16 ms)，会把 250 ms 失效窗口算漏。
    last_new_sample_t = None
    prev_source_seq = None
    for t, frame in zip(times, frames):
        if frame.get("source_seq") != prev_source_seq:
            last_new_sample_t = t
        prev_source_seq = frame.get("source_seq")
    stop_latency_ms = None
    if first_invalid is not None and last_new_sample_t is not None:
        stop_latency_ms = (first_invalid - last_new_sample_t) * 1000.0

    return {
        "ok": bool(valid) and recenter_result and recenter_result.get("ok"),
        "duration_s": round(seconds, 2),
        "udp_frames": len(frames),
        "udp_rate_hz": round(len(frames) / duration, 1) if duration else None,
        "valid_frames": len(valid),
        "valid_ratio": round(len(valid) / len(frames), 3),
        "move_forward_max": round(max(moves), 3),
        "move_forward_min": round(min(moves), 3),
        "move_right_abs_max": round(max(abs(v) for v in rights), 3),
        "seq_gaps": gaps,
        "max_datagram_bytes": max(udp_sizes),
        "recenter": recenter_result,
        "jump_token": jump_token,
        "jump_consumed": acked,
        "stop_latency_ms": None if stop_latency_ms is None else round(stop_latency_ms, 1),
        "sample_frame": frames[len(frames) // 2],
    }


def print_report(r: dict) -> None:
    print("=" * 68)
    print("TiltPlay B-part offline end-to-end demo")
    print("=" * 68)
    print(f"  ports ................... udp={r['udp_port']} control={r['control_port']}")
    print(f"  UDP frames .............. {r['udp_frames']}  ({r['udp_rate_hz']} Hz)")
    print(f"  valid frames ............ {r['valid_frames']}  ({r['valid_ratio'] * 100:.1f} %)")
    print(f"  forward move min/max .... {r['move_forward_min']} / {r['move_forward_max']}")
    print(f"  |right| max ............. {r['move_right_abs_max']}  (V3 forces right=0)")
    print(f"  seq gaps ................ {r['seq_gaps']}")
    print(f"  max datagram ............ {r['max_datagram_bytes']} B (limit 1200)")
    print(f"  recenter ................ {r['recenter']}")
    print(f"  jump token / consumed ... {r['jump_token']} / {r['jump_consumed']}")
    print(f"  stop-control latency .... {r['stop_latency_ms']} ms (budget 300)")
    print("-" * 68)
    print("  sample frame:")
    print("  " + json.dumps(r["sample_frame"], ensure_ascii=False))
    print("=" * 68)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="TiltPlay 端到端离线演示")
    parser.add_argument("--seconds", type=float, default=12.0)
    parser.add_argument("--udp-port", type=int, default=0,
                        help="演示用 UDP 端口，0 表示自动分配（默认，不占用 UE 的 7001）")
    parser.add_argument("--control-port", type=int, default=0,
                        help="控制口，0 表示自动分配")
    args = parser.parse_args(argv)
    report = run_demo(args.seconds, udp_port=args.udp_port,
                      control_port=args.control_port)
    return 0 if report.get("ok") else 1


if __name__ == "__main__":
    raise SystemExit(main())
