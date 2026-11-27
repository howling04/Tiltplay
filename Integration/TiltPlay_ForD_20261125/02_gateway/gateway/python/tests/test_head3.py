import unittest
from helpers import make_state, attach_probe, frame


class Head3Tests(unittest.TestCase):
    def test_angles_order_relative_center_and_single_axis(self):
        state = make_state()
        state.cfg.mode = 'head3'
        attach_probe(state)
        state.on_raw(frame(), 1000)
        state.tilt.theta_f = 20
        state.tilt.theta_r = -12
        state.neutral_f = 2
        state.neutral_r = -3
        snap = state.snapshot(1000)
        self.assertEqual(snap['angles_deg'], [18, 0, -9])
        self.assertGreater(snap['move'][0], 0)
        self.assertEqual(snap['move'][1], 0)
        self.assertEqual(state.recenter(1000), 'CENTERED')
        self.assertEqual(state.snapshot(1000)['angles_deg'], [0, 0, 0])

    def test_stale_head3_still_stops_move(self):
        state = make_state()
        state.cfg.mode = 'head3'
        attach_probe(state)
        state.on_raw(frame(), 1000)
        state.tilt.theta_f = 20
        snap = state.snapshot(1000.3)
        self.assertFalse(snap['valid'])
        self.assertEqual(snap['move'], [0, 0])
        self.assertIsNone(snap['jump_token'])
