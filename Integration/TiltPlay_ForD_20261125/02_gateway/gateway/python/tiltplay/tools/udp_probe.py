"""UDP 探针：占用 7001，打印/记录网关输出帧，核对序号与字段范围。

与 UE5.8 二选一占用端口；探针必须先退出再由 UE 绑定。
"""
from __future__ import annotations

import argparse
import json
import socket
import sys
import time


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="TiltPlay UDP 输入探针")
    parser.add_argument("--port", type=int, default=7001)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--seconds", type=float, default=0.0, help="0 表示一直运行")
    parser.add_argument("--jsonl", default="")
    parser.add_argument("--every", type=float, default=0.5, help="控制台打印间隔(秒)")
    args = parser.parse_args(argv)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((args.host, args.port))
    sock.settimeout(0.2)
    print(f"listening {args.host}:{args.port} ... (Ctrl+C to stop)")

    out = open(args.jsonl, "w", encoding="utf-8") if args.jsonl else None
    start = time.monotonic()
    count = 0
    valid = 0
    gaps = 0
    last_seq = None
    last_print = 0.0
    try:
        while True:
            if args.seconds and time.monotonic() - start > args.seconds:
                break
            try:
                data, _ = sock.recvfrom(2048)
            except socket.timeout:
                continue
            count += 1
            frame = json.loads(data.decode("utf-8"))
            if out:
                out.write(json.dumps(frame, ensure_ascii=False) + "\n")
            if last_seq is not None and frame["seq"] != (last_seq + 1) & 0xFFFFFFFF:
                gaps += 1
            last_seq = frame["seq"]
            valid += 1 if frame.get("valid") else 0
            now = time.monotonic()
            if now - last_print >= args.every:
                last_print = now
                print(f"[{count:5d}] seq={frame['seq']:6d} valid={frame['valid']} "
                      f"move=[{frame['move'][0]:+.2f},{frame['move'][1]:+.2f}] "
                      f"age={frame['source_age_ms']}ms bound={frame['source_bound_ms']}ms "
                      f"jump={frame['jump_token']}")
    except KeyboardInterrupt:
        pass
    finally:
        sock.close()
        if out:
            out.close()
    elapsed = max(time.monotonic() - start, 1e-6)
    print(f"\nframes={count} valid={valid} ({valid / max(count,1) * 100:.1f}%) "
          f"rate={count / elapsed:.1f} Hz seq_gaps={gaps}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
