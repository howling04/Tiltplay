"""向 7002 发送软件回中请求并打印确认（模拟 UE 端按 C）。"""
from __future__ import annotations

import argparse
import json
import socket
import sys
import time
import uuid


def request_recenter(device_id: str, *, port: int = 7002, bridge_id: str | None = None,
                     boot_id: str | None = None, link_epoch: int | None = None,
                     retries: int = 3, timeout: float = 0.3) -> dict:
    payload = {
        "schema": 2, "type": "control", "op": "recenter",
        "request_id": f"ui-{uuid.uuid4().hex[:6]}",
        "device_id": device_id,
    }
    if bridge_id:
        payload["bridge_id"] = bridge_id
    if boot_id:
        payload["boot_id"] = boot_id
    if link_epoch is not None:
        payload["link_epoch"] = link_epoch

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(timeout)
    try:
        for attempt in range(retries):
            sock.sendto(json.dumps(payload).encode(), ("127.0.0.1", port))
            try:
                data, _ = sock.recvfrom(2048)
            except socket.timeout:
                continue
            ack = json.loads(data.decode())
            if ack.get("request_id") == payload["request_id"]:
                ack["attempts"] = attempt + 1
                return ack
    finally:
        sock.close()
    return {"ok": False, "code": "TIMEOUT", "attempts": retries}


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="TiltPlay 软件回中请求")
    parser.add_argument("--device", default="nano-a1b2c3d4e5f6")
    parser.add_argument("--port", type=int, default=7002)
    args = parser.parse_args(argv)
    ack = request_recenter(args.device, port=args.port)
    print(json.dumps(ack, ensure_ascii=False, indent=2))
    return 0 if ack.get("ok") else 1


if __name__ == "__main__":
    raise SystemExit(main())
