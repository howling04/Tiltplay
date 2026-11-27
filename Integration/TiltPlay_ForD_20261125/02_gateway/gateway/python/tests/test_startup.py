import unittest
from tiltplay.tools.startup import StartupRecenter
from helpers import make_state, attach_probe, frame


class StartupTests(unittest.TestCase):
    def test_waits_for_readiness_then_stops_after_success(self):
        policy = StartupRecenter(100)
        calls = []
        def send(n):
            calls.append(n)
            return dict(ok=True, code='CENTERED')
        policy.tick(101, lambda: False, send)
        self.assertEqual(calls, [])
        policy.tick(101.1, lambda: True, send)
        policy.tick(102, lambda: True, send)
        self.assertEqual(calls, [1])
        self.assertTrue(policy.done)

    def test_transient_failure_retry_is_bounded_and_distinct(self):
        policy = StartupRecenter(100)
        calls = []
        def send(n):
            calls.append(n)
            return dict(ok=False, code='STALE')
        for now in (101, 101.1, 101.3, 103, 104):
            policy.tick(now, lambda: True, send)
        self.assertEqual(calls, [1, 2])
        self.assertEqual(policy.result['code'], 'STARTUP_TIMEOUT')

    def test_never_ready_times_out_without_sending(self):
        policy = StartupRecenter(100)
        policy.tick(103, lambda: False, lambda n: self.fail('must not send'))
        self.assertTrue(policy.done)
        self.assertFalse(policy.result['ok'])

    def test_stale_diagnostics_preserve_response_and_center(self):
        state = make_state()
        events = []
        state.log_fn = lambda event, **fields: events.append((event, fields))
        self.assertEqual(state.recenter(1000), 'STALE')
        self.assertFalse(state.centered)
        self.assertEqual(events[-1][1]['reason'], 'offline')
        attach_probe(state)
        state.on_raw(frame(), 1000)
        self.assertEqual(state.recenter(1000.3), 'STALE')
        self.assertEqual(events[-1][1]['reason'], 'sample_stale')
