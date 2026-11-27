r"""USB 板子 -> 网关校验 -> UDP 7001（给 UE 用）/ 同时落盘

这是 C 侧在 A 的 record_live.py 基础上改的"演示版"录制器，改动点（与原版的差别）：
1. 去掉硬编码的 40 秒五段复测脚本（原版会打印"前端下压/右倾"等复测提示并蜂鸣）
2. --seconds 0（默认）表示一直录，Ctrl+C 结束；给 D 演示用就是这个模式
3. 回中失败不再抛异常栈，改成给人看的中文提示后正常退出
4. 默认双轴（前后 + 左右都输出）；要单轴加 --single-axis
5. 保留：串口打不开时列出可用串口、退出时向 7001 发 valid=false 停控帧、RECENTER BLOCKED 提示

用法：
  python record_live.py --port COM7 --ue
  python record_live.py --port COM7 --ue --seconds 30 --out .\recordings\demo
"""
from pathlib import Path
import sys, json, time, threading, socket, argparse, csv
from types import SimpleNamespace
from dataclasses import replace

ROOT = Path(__file__).resolve().parent
sys.path[:0] = [str(ROOT / "gateway/python"), str(ROOT / "gateway/.vendor")]

import serial
from serial.tools import list_ports
from tiltplay.bridge import Bridge, BridgeConfig
from tiltplay.tools.recenter import request_recenter

CONFIG = ROOT / "gateway/config/bridge.json"


def open_serial(port):
    try:
        return serial.Serial(port, 921600, timeout=.1, write_timeout=.5)
    except (serial.SerialException, OSError) as exc:
        print(f"无法打开串口 {port}：{exc}", file=sys.stderr)
        print("请关闭 Arduino 串口监视器和其他占用串口的程序，确认 USB 数据线。", file=sys.stderr)
        ports = list(list_ports.comports())
        print("当前可用串口：", file=sys.stderr)
        for item in ports:
            print(f"  {item.device}  {item.description}", file=sys.stderr)
        if not ports:
            print("  未发现串口，请检查板子连接。", file=sys.stderr)
        return None


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--port", default="COM16", help="板子串口，例如 COM7")
    p.add_argument("--control-port", type=int, default=7002)
    p.add_argument("--device-id", default="nano-a1b2c3d4e5f6")
    p.add_argument("--seconds", type=float, default=0,
                   help="录制时长，0 = 一直录直到 Ctrl+C")
    p.add_argument("--single-axis", action="store_true",
                   help="只输出前后轴（左右恒 0）；默认双轴")
    p.add_argument("--prepare-timeout", type=float, default=20)
    p.add_argument("--out", default="")
    p.add_argument("--ue", action="store_true", help="把校验后的帧转发到 UE 127.0.0.1:7001")
    a = p.parse_args()

    link = open_serial(a.port)
    if link is None:
        return 2

    out = Path(a.out) if a.out else ROOT / "recordings" / time.strftime("%Y%m%d_%H%M%S")
    out.mkdir(parents=True, exist_ok=True)

    cfg = BridgeConfig.load(CONFIG)
    dev = next(iter(cfg.devices.values()))
    cfg.devices = {a.device_id: replace(dev, device_id=a.device_id)}
    cfg.force_single_axis = a.single_axis
    cfg.control_port = a.control_port
    cfg.log_dir = str(out / "gateway_log")
    cfg.console_enabled = False

    receiver = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    receiver.bind(("127.0.0.1", 0))
    receiver.settimeout(.1)
    cfg.udp_port = receiver.getsockname()[1]
    forward = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    raw = (out / "raw.jsonl").open("w", encoding="utf-8")
    control = (out / "status_ping_ack.jsonl").open("w", encoding="utf-8")
    udp = (out / "input_udp.jsonl").open("w", encoding="utf-8")
    recording = threading.Event()
    transport_stop = threading.Event()
    serial_errors = []
    counts = {"raw": 0, "input": 0, "valid_input": 0, "malformed_serial": 0}
    bridge = Bridge(cfg, console=False)
    write_lock = threading.Lock()

    class USBClient:
        """把网关原本要发到 MQTT 的命令，直接写进串口。"""
        def publish(self, topic, payload, qos=1):
            if not topic.endswith("/cmd"):
                return
            control.write(json.dumps({"host_perf_s": time.perf_counter(),
                                      "direction": "host_to_board", "topic": topic,
                                      "message": json.loads(payload)}) + "\n")
            control.flush()
            with write_lock:
                link.write(payload.encode("utf-8") + b"\n")

        def loop_stop(self):
            transport_stop.set()
            if worker.is_alive():
                worker.join(timeout=2)

        def disconnect(self):
            link.close()

    adapter = USBClient()
    bridge.client = adapter

    def read_serial():
        while not transport_stop.is_set():
            try:
                line = link.readline()
                if not line:
                    continue
                packet = json.loads(line)
                kind = packet.get("channel")
                if kind not in ("raw", "status", "ack"):
                    continue
                msg = packet["message"]
                topic = "tiltplay/v2/" + a.device_id + "/" + ("telemetry/raw" if kind == "raw" else kind)
                stamp = time.perf_counter()
                if kind == "raw" and recording.is_set():
                    raw.write(json.dumps({"host_perf_s": stamp, "transport": "usb_cdc",
                                          "message": msg}) + "\n")
                    counts["raw"] += 1
                elif kind != "raw":
                    control.write(json.dumps({"host_perf_s": stamp, "direction": "board_to_host",
                                              "transport": "usb_cdc", "topic": topic,
                                              "message": msg}) + "\n")
                    control.flush()
                bridge._on_message(None, None, SimpleNamespace(topic=topic, payload=json.dumps(msg).encode()))
            except (ValueError, KeyError, UnicodeDecodeError):
                counts["malformed_serial"] += 1
            except Exception as exc:
                if not transport_stop.is_set():
                    serial_errors.append(str(exc))
                transport_stop.set()

    worker = threading.Thread(target=read_serial, daemon=True)
    started = None
    valid_first = None
    center = None
    completed = False
    last = None
    shutdown_stop = None

    try:
        bridge.start(with_mqtt=False)
        worker.start()
        print("准备中：请把板子平放静止，等待探测与自动回中……", flush=True)
        deadline = time.perf_counter() + a.prepare_timeout
        next_center = 0
        next_report = 0
        while time.perf_counter() < deadline and not transport_stop.is_set():
            state = bridge.states.get(a.device_id)
            if state and time.perf_counter() >= next_report:
                blocked = (cfg.require_level_recenter and
                           (abs(state.tilt.theta_f) > cfg.recenter_level_max_deg or
                            abs(state.tilt.theta_r) > cfg.recenter_level_max_deg))
                if blocked:
                    print(f"板子还没放平：前后={state.tilt.theta_f:.1f}°，左右={state.tilt.theta_r:.1f}°，"
                          f"限 ±{cfg.recenter_level_max_deg:g}°。请平放后自动重试。", flush=True)
                else:
                    print(f"平放检查通过：前后={state.tilt.theta_f:.1f}°，左右={state.tilt.theta_r:.1f}°，回中中……", flush=True)
                next_report = time.perf_counter() + 2
            if (state and state.valid(ignore_center=True) and state.still_ok() and
                    (not cfg.require_level_recenter or
                     (abs(state.tilt.theta_f) <= cfg.recenter_level_max_deg and
                      abs(state.tilt.theta_r) <= cfg.recenter_level_max_deg)) and
                    time.perf_counter() >= next_center):
                center = request_recenter(a.device_id, port=bridge.control_sock.getsockname()[1])
                next_center = time.perf_counter() + .5
            try:
                data, _ = receiver.recvfrom(4096)
                frame = json.loads(data)
                if frame.get("device_id") == a.device_id and frame.get("valid"):
                    valid_first = frame
                    break
            except TimeoutError:
                pass

        if valid_first is None:
            state = bridge.states.get(a.device_id)
            diag = state.diagnostics() if state else None
            print("回中失败/板子没就绪，没有开始录制。", file=sys.stderr)
            print("常见原因：板子倾斜超过 ±10°、串口被占用、板子没烧 USB_Live 固件。", file=sys.stderr)
            print("诊断信息：" + json.dumps(diag, ensure_ascii=False), file=sys.stderr)
            return 3

        started = time.perf_counter()
        recording.set()
        print(f"回中成功，开始输出到 127.0.0.1:7001（{'一直录，Ctrl+C 结束' if a.seconds <= 0 else str(a.seconds) + ' 秒'}）。", flush=True)
        print(f"录制文件：{out}", flush=True)

        next_status = 0
        while not transport_stop.is_set():
            if a.seconds > 0 and time.perf_counter() - started >= a.seconds:
                completed = True
                break
            try:
                data, _ = receiver.recvfrom(4096)
                frame = json.loads(data)
                udp.write(json.dumps({"host_perf_s": time.perf_counter(), "message": frame}) + "\n")
                counts["input"] += 1
                counts["valid_input"] += bool(frame.get("valid"))
                if a.ue:
                    forward.sendto(data, ("127.0.0.1", 7001))
            except TimeoutError:
                pass
            if time.perf_counter() >= next_status:
                raw.flush(); udp.flush()
                state = bridge.states.get(a.device_id)
                last = state.diagnostics() if state else None
                move = locals().get('frame', {}).get("move")
                print(f"{time.perf_counter() - started:.0f}s  raw={counts['raw']}  "
                      f"valid={counts['valid_input']}/{counts['input']}  move={move}", flush=True)
                next_status = time.perf_counter() + 5
    except KeyboardInterrupt:
        print("收到 Ctrl+C，停止录制。", flush=True)
    finally:
        ended = time.perf_counter()
        recording.clear()
        state = bridge.states.get(a.device_id)
        last = state.diagnostics() if state else last
        bridge.stop()
        if link.is_open:
            link.close()
        # 退出前发一帧停控，防止 UE 端留着最后一帧继续走
        if state is not None:
            shutdown_stop = bridge._udp_frame(state, time.monotonic())
            if shutdown_stop is not None:
                shutdown_stop.update(valid=False, move=[0.0, 0.0], angles_deg=[0.0, 0.0, 0.0],
                                     jump_token=None, jump_age_bound_ms=None)
                forward.sendto(json.dumps(shutdown_stop).encode("utf-8"), ("127.0.0.1", 7001))
                udp.write(json.dumps({"host_perf_s": time.perf_counter(),
                                      "event": "shutdown_stop", "message": shutdown_stop}) + "\n")
        receiver.close(); forward.close()
        raw.close(); udp.close(); control.close()
        summary = {"completed": completed, "device_id": a.device_id,
                   "requested_seconds": a.seconds,
                   "recorded_seconds": ended - started if started else 0,
                   "transport": "usb_cdc", "recenter": center,
                   "first_valid_input": valid_first, "diagnostics": last,
                   "counts": counts, "serial_errors": serial_errors,
                   "force_single_axis": cfg.force_single_axis,
                   "control_port": cfg.control_port,
                   "require_level_recenter": cfg.require_level_recenter,
                   "level_limit_deg": cfg.recenter_level_max_deg,
                   "forward_output_sign": cfg.forward_output_sign,
                   "shutdown_stop_frame": shutdown_stop}
        (out / "summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
        with (out / "six_axis.csv").open("w", newline="", encoding="utf-8-sig") as fh:
            w = csv.writer(fh)
            w.writerow(["host_perf_s", "t_us", "seq", "sample_seq",
                        "ax_mps2", "ay_mps2", "az_mps2", "gx_rps", "gy_rps", "gz_rps"])
            for line in (out / "raw.jsonl").open(encoding="utf-8"):
                row = json.loads(line); m = row["message"]
                w.writerow([row["host_perf_s"], m["t_us"], m["seq"], m["sample_seq"],
                            *m["acc_mps2"], *m["gyro_rps"]])
        print("录制结束，summary.json 已写入：" + str(out / "summary.json"), flush=True)

    if not completed and a.seconds > 0:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
