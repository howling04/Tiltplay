"""Replay the recorded TiltPlay input through Luna's recenter gate."""

import json
from pathlib import Path
import select
import socket
import time


def main():
    handoff = Path(__file__).with_name("TiltPlay_ForD_20261125")
    fixture = next(handoff.glob("*/sample_input_40s.jsonl"))
    with fixture.open(encoding="utf-8") as stream:
        rows = (json.loads(line) for line in stream if line.strip())
        frames = [row.get("message", row) for row in rows]

    neutral = dict(frames[0], valid=True, move=[0.0, 0.0], angles_deg=[0.0, 0.0, 0.0])
    session = {key: neutral[key] for key in ("schema", "device_id", "bridge_id", "boot_id", "link_epoch")}
    first_source = frames[0]["source_seq"]
    target = ("127.0.0.1", 7001)
    index = 0
    packet_seq = 0
    replay_base = None
    next_send = time.monotonic()
    deadline = next_send + 300

    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as link:
        link.bind(("127.0.0.1", 7002))
        print("Waiting for Luna's C key. This session uses recorded frames.")
        while index < len(frames) and time.monotonic() < deadline:
            now = time.monotonic()
            if select.select([link], [], [], max(0.0, next_send - now))[0]:
                data, peer = link.recvfrom(4096)
                try:
                    request = json.loads(data)
                except (UnicodeDecodeError, json.JSONDecodeError):
                    continue
                if (isinstance(request, dict) and request.get("type") == "control"
                        and request.get("op") == "recenter" and isinstance(request.get("request_id"), str)
                        and all(request.get(k) == v for k, v in session.items())):
                    ready = replay_base is None
                    ack = dict(session, type="control_ack", op="recenter", request_id=request["request_id"],
                               ok=ready, code="CENTERED" if ready else "NOT_STILL")
                    link.sendto(json.dumps(ack).encode("utf-8"), peer)
                    if ready:
                        replay_base = packet_seq
                        deadline = time.monotonic() + len(frames) / 60.0 + 15
                        print("Luna recentered. Playing the captured input for about 40 seconds.")

            now = time.monotonic()
            if now >= next_send:
                if replay_base is None:
                    frame = dict(neutral, seq=packet_seq, source_seq=packet_seq)
                else:
                    sample = frames[index]
                    source_delta = (sample["source_seq"] - first_source) % (1 << 32)
                    frame = dict(sample, seq=packet_seq, source_seq=replay_base + source_delta)
                    index += 1
                link.sendto(json.dumps(frame, separators=(",", ":")).encode("utf-8"), target)
                packet_seq += 1
                next_send = max(next_send + 1.0 / 60.0, now)

    print(f"Replay sent {index}/{len(frames)} captured frames.")
    return 0 if index == len(frames) else 1


if __name__ == "__main__":
    raise SystemExit(main())
