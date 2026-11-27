"""把已录好的网关输出（input_udp.jsonl）按 60 Hz 回放到 UE 7001。
用途：不接板子也能验证 UE 侧是否收帧/有效/方向正确（交付前冒烟用）。
"""
import json, socket, sys, time
from pathlib import Path

src = Path(sys.argv[1])
target = ("127.0.0.1", int(sys.argv[2]) if len(sys.argv) > 2 else 7001)
hz = float(sys.argv[3]) if len(sys.argv) > 3 else 60.0

frames = []
with src.open(encoding="utf-8") as f:
    for line in f:
        line = line.strip()
        if not line:
            continue
        rec = json.loads(line)
        frames.append(rec.get("message", rec))

tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
period = 1.0 / hz
t0 = time.perf_counter()
sent = 0
for fr in frames:
    tx.sendto(json.dumps(fr, separators=(",", ":")).encode("utf-8"), target)
    sent += 1
    nxt = t0 + sent * period
    d = nxt - time.perf_counter()
    if d > 0:
        time.sleep(d)
print("replayed", sent, "frames to", target, "in %.1fs" % (time.perf_counter() - t0))
