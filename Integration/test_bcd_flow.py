"""Run synthetic IMU -> B Python test gateway -> C receiver -> D motion in UE."""
from __future__ import annotations

import argparse
import configparser
from datetime import datetime
import json
import math
import os
from pathlib import Path
import select
import socket
import subprocess
import sys
import time

LUNA = Path(__file__).resolve().parents[1]
TEST = "LunaBCD.Schema2BridgeFlow"
RAW_ENDPOINT = ("127.0.0.1", 7004)
PHASE_ENDPOINT = ("127.0.0.1", 7003)
GRAVITY = 9.80665
PERIOD = 1.0 / 104.0
TILTS = {
    "pitch_8": (8.0, 0.0),
    "pitch_18": (18.0, 0.0),
    "pitch_-18": (-18.0, 0.0),
    "roll_-8": (0.0, -8.0),
    "roll_-18": (0.0, -18.0),
    "roll_18": (0.0, 18.0),
}
PULSES = {"below": 3.8, "up": 5.0, "down": -5.0}


def stop(process: subprocess.Popen) -> None:
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


def stream_imu(gateway: subprocess.Popen, unreal: subprocess.Popen, trace, timeout: float) -> None:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as phases:
        phases.bind(PHASE_ENDPOINT)
        phases.setblocking(False)
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as raw:
            phase = "neutral"
            phase_start = last_sample_at = next_sample = time.monotonic()
            deadline = next_sample + timeout
            sample_seq = 0
            angles = [0.0, 0.0]
            packet = b""
            while unreal.poll() is None:
                now = time.monotonic()
                if gateway.poll() is not None:
                    raise RuntimeError(f"B test gateway stopped with exit code {gateway.returncode}")
                if now > deadline:
                    raise TimeoutError(f"Unreal did not finish within {timeout:g} s")
                if select.select([phases], [], [], max(0.0, next_sample - now))[0]:
                    phase = phases.recv(128).decode("ascii")
                    phase_start = time.monotonic()
                    print(phase, file=trace, flush=True)
                now = time.monotonic()
                if now < next_sample:
                    continue
                if phase != "repeat_source" or not packet:
                    dt = max(PERIOD, now - last_sample_at)
                    target = TILTS.get(phase, (0.0, 0.0))
                    rates = []
                    for axis, degrees in enumerate(target):
                        limit = 70.0 * dt
                        delta = max(-limit, min(limit, degrees - angles[axis]))
                        angles[axis] += delta
                        rates.append(math.radians(delta) / dt)
                    pitch, roll = map(math.radians, angles)
                    pulse = PULSES.get(phase, 0.0) if now - phase_start < 0.16 else 0.0
                    sample_seq += 1
                    packet = json.dumps({
                        "schema": 2,
                        "device_id": "nano-a1b2c3d4e5f6",
                        "boot_id": "000000000000001a",
                        "link_epoch": 1,
                        "seq": sample_seq,
                        "sample_seq": sample_seq,
                        "sample_hz": 104,
                        "t_us": int(now * 1_000_000),
                        "acc_mps2": [
                            -GRAVITY * math.sin(pitch),
                            -GRAVITY * math.sin(roll),
                            GRAVITY * math.cos(pitch) * math.cos(roll) + pulse,
                        ],
                        "gyro_rps": [rates[1], -rates[0], 0.0],
                    }, separators=(",", ":")).encode()
                    last_sample_at = now
                raw.sendto(packet, RAW_ENDPOINT)
                next_sample = max(next_sample + PERIOD, now + 0.001)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--editor", type=Path, required=True, help="path to UnrealEditor-Cmd.exe")
    parser.add_argument("--gateway", type=Path, required=True, help="path to the legacy B fixture gateway")
    parser.add_argument("--timeout", type=float, default=180.0)
    args = parser.parse_args()
    editor, gateway_script = args.editor.resolve(), args.gateway.resolve()
    if not editor.is_file() or not gateway_script.is_file():
        parser.error("the UE editor and the legacy B fixture gateway must be present")

    ini = configparser.ConfigParser(strict=False, interpolation=None)
    ini.read(LUNA / "Config" / "DefaultGame.ini", encoding="utf-8")
    settings = ini["/Script/LunaMVP.LunaSettings"]
    run = LUNA / "Saved" / "Verification" / datetime.now().strftime("BCDFlow-%Y%m%d-%H%M%S-%f")
    run.mkdir(parents=True)
    print(f"Evidence: {run}", flush=True)
    cache = LUNA / "Saved" / "Cache"
    ue_log = run / "Unreal.log"
    env = {**os.environ, "PYTHONDONTWRITEBYTECODE": "1"}
    gateway_args = [
        sys.executable, "-B", str(gateway_script), "--fixture-port", str(RAW_ENDPOINT[1]),
        "--udp-port", settings.get("InputPort", "7001"),
        "--control-port", settings.get("ControlPort", "7002"),
    ]
    unreal_args = [
        str(editor), str(LUNA / "Luna_v1.uproject"), f"-ExecCmds=Automation RunTests {TEST}",
        "-TestExit=Automation Test Queue Empty", f"-ReportExportPath={run / 'Automation'}",
        "-unattended", "-NullRHI", "-nosound", "-NoSplash", "-ddc=InstalledNoZenLocalFallback",
        "-DisablePlugins=ModelContextProtocol,EditorToolset,ToolsetRegistry,PythonScriptPlugin,Terminal",
        f"-LocalDataCachePath={cache}", f"-abslog={ue_log}", "-stdout", "-FullStdOutLogOutput",
    ]
    with (run / "Gateway.log").open("w", encoding="utf-8") as gateway_log, \
            (run / "Unreal-console.log").open("w", encoding="utf-8") as console_log, \
            (run / "Fixture-phases.txt").open("w", encoding="utf-8") as trace:
        gateway = subprocess.Popen(gateway_args, cwd=gateway_script.parent, env=env,
                                   stdout=gateway_log, stderr=subprocess.STDOUT)
        try:
            unreal = subprocess.Popen(unreal_args, cwd=LUNA, stdout=console_log, stderr=subprocess.STDOUT)
            try:
                stream_imu(gateway, unreal, trace, args.timeout)
            finally:
                stop(unreal)
        finally:
            stop(gateway)

    log = ue_log.read_text(encoding="utf-8", errors="replace") if ue_log.is_file() else ""
    passed = any("Result={Success}" in line and TEST in line for line in log.splitlines())
    if not passed or unreal.returncode != 0:
        print(f"FAIL: {TEST}, UE exit code {unreal.returncode}", file=sys.stderr)
        return 1
    print(f"PASS: {TEST}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
