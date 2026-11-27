"""Capture a short sample of what the attached board is sending over USB CDC."""

import json
from pathlib import Path
import sys
import time

handoff = Path(__file__).with_name("TiltPlay_ForD_20261125")
sys.path.insert(0, str(handoff / "02_gateway" / "gateway" / ".vendor"))
import serial


def main(port: str, report: Path) -> int:
    counts = {"raw": 0, "status": 0, "ack": 0, "firmware_error": 0, "other_json": 0, "text": 0}
    examples = {}
    received_bytes = 0
    error = None
    print(f"Reading {port} at 921600 baud for 8 seconds...", flush=True)
    try:
        with serial.Serial(port, 921600, timeout=0.2) as link:
            until = time.monotonic() + 8
            while time.monotonic() < until:
                data = link.readline(4096)
                if not data:
                    continue
                received_bytes += len(data)
                line = data.decode("utf-8", errors="replace").strip()
                sample = line
                try:
                    packet = json.loads(line)
                except ValueError:
                    channel = "text"
                else:
                    channel = "other_json"
                    if (isinstance(packet, dict) and packet.get("channel") in ("raw", "status", "ack")
                            and isinstance(packet.get("message"), dict) and packet["message"].get("schema") == 2):
                        channel = packet["channel"]
                    elif isinstance(packet, dict) and isinstance(packet.get("error"), str):
                        channel = "firmware_error"
                        sample = packet["error"]
                counts[channel] += 1
                samples = examples.setdefault(channel, [])
                if len(samples) < 3:
                    samples.append(sample[:1200])
    except (OSError, serial.SerialException) as exc:
        error = str(exc)

    report.write_text(json.dumps({"port": port, "baud": 921600, "received_bytes": received_bytes,
                                  "counts": counts, "error": error, "examples": examples},
                                 ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"Serial probe saved to {report}.")
    print(f"Received {received_bytes} bytes, including {counts['raw']} schema 2 raw frames and {counts['status']} status frames.")
    if error:
        print(error)
    if counts["firmware_error"]:
        print("Firmware reported:", ", ".join(examples["firmware_error"]))
    return 0 if counts["raw"] else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1], Path(sys.argv[2])))
