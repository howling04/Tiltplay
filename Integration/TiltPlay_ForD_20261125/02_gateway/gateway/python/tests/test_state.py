"""序号、启动会话、新鲜度、按键去重与回中的单测（附录 D 向量）。"""
from __future__ import annotations

import unittest

from tiltplay.state import DeviceState

from helpers import (BOOT, DEVICE, attach_probe, frame, make_state, tilt_payload)


def feed(state: DeviceState, now: float, **overrides):
    return state.on_raw(frame(**overrides), now)


class SequenceTests(unittest.TestCase):
    def setUp(self):
        self.state = make_state()
        attach_probe(self.state)

    def test_t02_gap_and_duplicate(self):
        results = []
        now = 1000.0
        for seq in (10, 12, 11, 12, 13):
            results.append(self.state.on_raw(frame(seq=seq, sample_seq=seq), now))
            now += 0.0096
        self.assertEqual(results, ["ok", "ok", "out_of_order", "duplicate", "ok"])
        self.assertEqual(self.state.counters.seq_gap, 1)
        self.assertEqual(self.state.counters.raw_dup, 1)
        self.assertEqual(self.state.counters.raw_ooo, 1)

    def test_t03_wraparound(self):
        now = 1000.0
        for seq in (0xFFFFFFFE, 0xFFFFFFFF, 0, 1):
            self.assertEqual(self.state.on_raw(frame(seq=seq, sample_seq=seq), now), "ok")
            now += 0.0096
        self.assertEqual(self.state.last_seq, 1)

    def test_duplicate_sample_does_not_refresh_age(self):
        now = 1000.0
        self.state.on_raw(frame(seq=1, sample_seq=1), now)
        first_age = self.state.source_age_ms(now)
        repeated = self.state.on_raw(frame(seq=2, sample_seq=1), now + 0.2)
        self.assertEqual(repeated, "duplicate_sample")
        self.assertAlmostEqual(self.state.source_age_ms(now + 0.2), first_age + 200,
                               delta=1.0)


class SessionTests(unittest.TestCase):
    def setUp(self):
        self.state = make_state()
        attach_probe(self.state)

    def test_d04_old_boot_not_reactivated(self):
        self.state.on_raw(frame(seq=1, sample_seq=1), 1000.0)
        new_boot = "00000000000000ff"
        self.state.on_raw(frame(seq=1, sample_seq=1, boot_id=new_boot), 1001.0)
        self.assertEqual(self.state.boot_id, new_boot)
        result = self.state.on_raw(frame(seq=2, sample_seq=2, boot_id=BOOT), 1002.0)
        self.assertEqual(result, "retired_boot")
        self.assertEqual(self.state.boot_id, new_boot)

    def test_link_epoch_change_resets(self):
        self.state.on_raw(frame(seq=5, sample_seq=5), 1000.0)
        self.state.on_status({"schema": 2, "device_id": DEVICE, "boot_id": BOOT,
                              "link_epoch": 2, "online": True}, 1001.0)
        self.assertEqual(self.state.link_epoch, 2)
        self.assertFalse(self.state.valid(1001.0))

    def test_old_epoch_raw_preserves_live_state_and_expires(self):
        state = self.state
        state.on_raw(frame(seq=5, sample_seq=5), 1000.0)
        state.centered = True
        probe = state.probe
        resets = state.counters.resets
        for now in (1000.1, 1000.2, 1000.3):
            self.assertEqual(state.on_raw(frame(link_epoch=0, seq=99, sample_seq=99), now),
                             "old_epoch")
        self.assertEqual(state.link_epoch, 1)
        self.assertTrue(state.centered)
        self.assertIs(state.probe, probe)
        self.assertEqual(state.counters.resets, resets)
        self.assertEqual(state.last_seq, 5)
        self.assertEqual(state.last_raw_mono, 1000.0)
        self.assertEqual(state.last_frame_mono, 1000.0)
        self.assertTrue(state.valid(1000.1))
        self.assertFalse(state.valid(1000.3))

    def test_old_epoch_status_cannot_reset_or_revive(self):
        state = self.state
        state.on_raw(frame(), 1000.0)
        state.centered = True
        resets = state.counters.resets
        msg = dict(boot_id=BOOT, link_epoch=0, online=True)
        state.on_status(msg, 1000.1)
        self.assertEqual(state.link_epoch, 1)
        self.assertTrue(state.centered)
        self.assertEqual(state.counters.resets, resets)
        state.online = False
        state.on_status(msg, 1000.2)
        self.assertFalse(state.online)

    def test_new_epoch_raw_resets_but_old_status_cannot_disconnect(self):
        state = self.state
        state.centered = True
        self.assertEqual(state.on_raw(frame(link_epoch=2), 1000.1), "ok")
        self.assertEqual(state.link_epoch, 2)
        self.assertFalse(state.centered)
        state.online = True
        state.on_status(dict(boot_id=BOOT, link_epoch=1, online=False), 1000.2)
        self.assertTrue(state.online)

    def test_epoch_max_does_not_wrap_within_same_boot(self):
        state = self.state
        state.on_raw(frame(link_epoch=0xffffffff), 1000.0)
        self.assertEqual(state.on_raw(frame(link_epoch=0), 1000.1), "old_epoch")
        self.assertEqual(state.link_epoch, 0xffffffff)
        self.assertEqual(state.on_raw(frame(boot_id="000000000000001b", link_epoch=1),
                                      1000.2), "ok")
        self.assertEqual(state.link_epoch, 1)


class PressTests(unittest.TestCase):
    def setUp(self):
        self.state = make_state(button_jump_enabled=True)
        attach_probe(self.state)

    def test_d05_baseline_repeat_then_one_jump(self):
        now = 1000.0
        for count, expected in ((7, 0.0), (7, None), (8, 8)):
            self.state.on_raw(frame(seq=int(now * 1000) & 0xFFFF, sample_seq=int(now * 1000) & 0xFFFF,
                                    button={"jump": False, "press_count": count,
                                            "press_t_us": int((now - 1000.0) * 1e6)}), now)
            token, _ = self.state.pending_jump(now)
            if expected is None:
                self.assertIsNone(token)
            elif expected == 0.0:
                self.assertIsNone(token)
            else:
                self.assertEqual(token, expected)
            now += 0.0096

    def test_d13_press_without_time(self):
        now = 1000.0
        self.state.on_raw(frame(seq=1, sample_seq=1,
                                button={"jump": True, "press_count": 1,
                                        "press_t_us": None}), now)
        token, _ = self.state.pending_jump(now)
        self.assertIsNone(token)

    def test_d14_token_zero_is_valid(self):
        now = 1000.0
        self.state.on_raw(frame(seq=1, sample_seq=1,
                                button={"jump": True, "press_count": 0,
                                        "press_t_us": 0}), now)
        self.state.pending_token = 0
        self.state.pending_press_t_us = 0
        token, _ = self.state.pending_jump(now)
        self.assertEqual(token, 0)
        self.assertTrue(self.state.consume_jump(0))

    def test_d06_event_and_raw_count_once(self):
        now = 1000.0
        self.state.on_raw(frame(seq=1, sample_seq=1,
                                button={"jump": False, "press_count": 2,
                                        "press_t_us": None}), now)
        now += 0.0096
        self.state.on_raw(frame(seq=2, sample_seq=2,
                                button={"jump": True, "press_count": 3,
                                        "press_t_us": 0}), now)
        self.assertEqual(self.state.counters.jump_issued, 1)
        event = {"schema": 2, "device_id": DEVICE, "boot_id": BOOT, "link_epoch": 1,
                 "seq": 9, "t_us": 9600, "event_id": "press-3", "type": "jump_pressed",
                 "press_count": 3, "press_t_us": 0}
        self.assertEqual(self.state.on_event(event, now + 0.05), "duplicate_count")
        self.assertEqual(self.state.counters.jump_issued, 1)

    def test_expired_press_not_issued(self):
        now = 1000.0
        self.state.on_raw(frame(seq=1, sample_seq=1), now)
        self.state.on_raw(frame(seq=2, sample_seq=2,
                                button={"jump": True, "press_count": 1,
                                        "press_t_us": 0}), now + 0.5)
        token, _ = self.state.pending_jump(now + 0.5)
        self.assertIsNone(token)
        self.assertEqual(self.state.counters.event_expired, 1)


class FreshnessTests(unittest.TestCase):
    def test_valid_then_stale(self):
        state = make_state()
        attach_probe(state)
        now = 1000.0
        for i in range(1, 6):
            state.on_raw(frame(seq=i, sample_seq=i, t_us=(i - 1) * 9615), now)
            now += 0.009615
        self.assertTrue(state.valid(now))
        self.assertFalse(state.valid(now + 0.26))
        self.assertEqual(state.move(now + 0.26), (0.0, 0.0))

    def test_probe_expiry_stops_control(self):
        state = make_state()
        attach_probe(state)
        now = 1000.0
        for i in range(1, 5):
            state.on_raw(frame(seq=i, sample_seq=i), now)
            now += 0.0096
        self.assertTrue(state.valid(now))
        self.assertFalse(state.valid(now + 6.0))

    def test_tilt_mapping(self):
        state = make_state()
        attach_probe(state)
        now = 1000.0
        for i in range(1, 6):
            payload = tilt_payload(20.0, 0.0, seq=i, sample_seq=i)
            state.on_raw(frame(**payload), now)
            now += 0.0096
        forward, right = state.move(now)
        self.assertAlmostEqual(forward, (20 - 3) / 22, delta=0.05)
        self.assertEqual(right, 0.0)

    def test_deadzone(self):
        state = make_state()
        attach_probe(state)
        now = 1000.0
        for i in range(1, 6):
            state.on_raw(frame(**tilt_payload(2.0, 0.0, seq=i, sample_seq=i)), now)
            now += 0.0096
        self.assertEqual(state.move(now), (0.0, 0.0))


class RecenterTests(unittest.TestCase):
    def test_not_still_is_rejected(self):
        state = make_state(require_recenter=True)
        attach_probe(state)
        now = 1000.0
        # 带角速度 => 不满足静置
        for i in range(1, 30):
            state.on_raw(frame(seq=i, sample_seq=i, t_us=int(i * 9615),
                               gyro_rps=[0.5, 0.5, 0.0]), now)
            now += 0.0096
        self.assertEqual(state.recenter(now), "NOT_STILL")

    def test_still_then_centered(self):
        state = make_state(require_recenter=True)
        attach_probe(state)
        now = 1000.0
        for i in range(1, 80):
            state.on_raw(frame(seq=i, sample_seq=i, t_us=int(i * 9615)), now)
            now += 0.0096
        self.assertFalse(state.valid(now))
        self.assertEqual(state.recenter(now), "CENTERED")
        self.assertTrue(state.valid(now))


if __name__ == "__main__":
    unittest.main()
