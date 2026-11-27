"""协议校验单测（附录 D 向量中的字段/类型/身份部分）。"""
from __future__ import annotations

import json
import math
import unittest

from tiltplay import protocol
from tiltplay.protocol import ProtocolError, newer, parse_topic

from helpers import DEVICE, raw_payload


class ParseTopicTests(unittest.TestCase):
    def test_raw_topic(self):
        self.assertEqual(parse_topic(f"tiltplay/v2/{DEVICE}/telemetry/raw"), (DEVICE, "raw"))

    def test_simple_topics(self):
        for suffix in ("event", "status", "cmd", "ack", "event_ack"):
            self.assertEqual(parse_topic(f"tiltplay/v2/{DEVICE}/{suffix}"), (DEVICE, suffix))

    def test_rejects_other_prefix(self):
        with self.assertRaises(ProtocolError) as ctx:
            parse_topic(f"tiltplay/v1/{DEVICE}/status")
        self.assertEqual(ctx.exception.code, "unknown_topic")


class RawValidationTests(unittest.TestCase):
    def test_accepts_valid_frame(self):
        frame = protocol.parse_raw(raw_payload(), DEVICE, 100)
        self.assertEqual(frame.device_id, DEVICE)
        self.assertEqual(frame.acc_mps2[2], 9.80665)

    def test_d01_schema_one(self):
        with self.assertRaises(ProtocolError) as ctx:
            protocol.parse_raw(raw_payload(schema=1), DEVICE)
        self.assertEqual(ctx.exception.code, "unsupported_schema")

    def test_d01_short_array(self):
        with self.assertRaises(ProtocolError) as ctx:
            protocol.parse_raw(raw_payload(acc_mps2=[0, 0]), DEVICE)
        self.assertEqual(ctx.exception.code, "invalid_array")

    def test_d10_identity_mismatch(self):
        with self.assertRaises(ProtocolError) as ctx:
            protocol.parse_raw(raw_payload(), "nano-ffffffffffff")
        self.assertEqual(ctx.exception.code, "id_mismatch")

    def test_missing_field(self):
        payload = raw_payload()
        del payload["sample_hz"]
        with self.assertRaises(ProtocolError) as ctx:
            protocol.parse_raw(payload, DEVICE)
        self.assertEqual(ctx.exception.code, "missing_field")

    def test_bool_is_not_int(self):
        with self.assertRaises(ProtocolError) as ctx:
            protocol.parse_raw(raw_payload(seq=True), DEVICE)
        self.assertEqual(ctx.exception.code, "invalid_type")

    def test_nan_rejected(self):
        with self.assertRaises(ProtocolError) as ctx:
            protocol.parse_raw(raw_payload(acc_mps2=[0.0, math.nan, 9.8]), DEVICE)
        self.assertEqual(ctx.exception.code, "invalid_array")

    def test_too_long(self):
        with self.assertRaises(ProtocolError) as ctx:
            protocol.parse_raw(raw_payload(), DEVICE, 1025)
        self.assertEqual(ctx.exception.code, "too_long")

    def test_bad_boot_id(self):
        with self.assertRaises(ProtocolError) as ctx:
            protocol.parse_raw(raw_payload(boot_id="XYZ"), DEVICE)
        self.assertEqual(ctx.exception.code, "bad_boot_id")

    def test_press_t_us_nullable(self):
        frame = protocol.parse_raw(raw_payload(), DEVICE)
        self.assertIsNone(frame.button.press_t_us)
        frame = protocol.parse_raw(raw_payload(button={
            "jump": False, "press_count": 1, "press_t_us": 0}), DEVICE)
        self.assertEqual(frame.button.press_t_us, 0)


class SequenceMathTests(unittest.TestCase):
    def test_increasing(self):
        self.assertTrue(newer(11, 10))
        self.assertFalse(newer(10, 10))
        self.assertFalse(newer(9, 10))

    def test_wraparound(self):
        self.assertTrue(newer(0, 0xFFFFFFFF))
        self.assertTrue(newer(1, 0))
        self.assertFalse(newer(0xFFFFFFFF, 0))


class JsonHelpersTests(unittest.TestCase):
    def test_json_bool_confusion_is_rejected(self):
        payload = json.loads(json.dumps(raw_payload()))
        payload["link_epoch"] = True
        with self.assertRaises(ProtocolError):
            protocol.parse_raw(payload, DEVICE)


if __name__ == "__main__":
    unittest.main()
