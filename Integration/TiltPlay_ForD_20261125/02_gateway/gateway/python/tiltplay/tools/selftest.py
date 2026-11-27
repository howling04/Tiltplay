"""一键自测：单元测试 + 端到端回环，退出码用于 CI/验收记录。"""
from __future__ import annotations

import argparse
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
TESTS = ROOT / "python" / "tests"


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="TiltPlay B 部分自测")
    parser.add_argument("-v", "--verbose", action="store_true")
    parser.add_argument("--pattern", default="test_*.py")
    args = parser.parse_args(argv)

    sys.path.insert(0, str(ROOT / "python"))
    sys.path.insert(0, str(TESTS))
    sys.path.insert(0, str(ROOT / ".vendor"))

    loader = unittest.TestLoader()
    suite = loader.discover(str(TESTS), pattern=args.pattern)
    runner = unittest.TextTestRunner(verbosity=2 if args.verbose else 1)
    result = runner.run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
