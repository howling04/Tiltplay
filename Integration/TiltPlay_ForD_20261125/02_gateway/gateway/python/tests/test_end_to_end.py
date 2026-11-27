"""端到端回环测试：broker + 模拟控制器 + 网关 + UDP 采集。"""
from __future__ import annotations

import unittest

from tiltplay.tools.demo import run_demo

REPORT = None


def setUpModule():
    global REPORT
    REPORT = run_demo(12.0, quiet=True)


class EndToEndTests(unittest.TestCase):
    def test_frames_flow_at_60hz(self):
        self.assertGreater(REPORT["udp_frames"], 400)
        self.assertAlmostEqual(REPORT["udp_rate_hz"], 60.0, delta=3.0)

    def test_sequence_continuous(self):
        self.assertEqual(REPORT["seq_gaps"], 0)

    def test_recenter_accepted(self):
        self.assertTrue(REPORT["recenter"]["ok"], REPORT["recenter"])
        self.assertEqual(REPORT["recenter"]["code"], "CENTERED")

    def test_forward_range_and_single_axis(self):
        self.assertGreater(REPORT["move_forward_max"], 0.75)
        self.assertLess(REPORT["move_forward_min"], -0.7)
        self.assertEqual(REPORT["move_right_abs_max"], 0.0)

    def test_valid_ratio(self):
        self.assertGreater(REPORT["valid_ratio"], 0.4)

    def test_jump_token_consumed_once(self):
        self.assertEqual(REPORT["jump_token"], 1)
        self.assertTrue(REPORT["jump_consumed"])

    def test_stop_control_within_budget(self):
        self.assertIsNotNone(REPORT["stop_latency_ms"])
        self.assertLessEqual(REPORT["stop_latency_ms"], 300.0)

    def test_datagram_size_limit(self):
        self.assertLessEqual(REPORT["max_datagram_bytes"], 1200)


if __name__ == "__main__":
    unittest.main()
