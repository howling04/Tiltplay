import unittest
from helpers import make_state, frame


class StillWindowTests(unittest.TestCase):
    def test_batched_receive_timestamps_allow_continuous_stillness(self):
        state = make_state()
        # Distinct samples arrive in pairs with identical receive timestamps.
        for i in range(61):
            state.on_raw(frame(seq=i, sample_seq=i, t_us=i*10000),
                         1000 + (i//2)*.02)
        self.assertTrue(state.still_ok())

    def test_instant_backlog_does_not_supply_receive_window(self):
        state = make_state()
        for i in range(61):
            state.on_raw(frame(seq=i, sample_seq=i, t_us=i*10000),1000)
        self.assertFalse(state.still_ok())

    def test_equal_device_time_still_restarts_window(self):
        state = make_state()
        self.feed(state,1000,51)
        self.feed(state,1000.51,1,device_start=500000)
        self.assertFalse(state.still_ok())

    def test_backward_receive_time_restarts_window(self):
        state = make_state()
        self.feed(state,1000,51)
        self.feed(state,1000.49,1,device_start=510000)
        self.assertFalse(state.still_ok())

    def feed(self, state, start, count, device_start=None, gyro=None):
        if device_start is None:
            device_start = round((start - 1000) * 1e6)
        for i in range(count):
            seq = state.last_seq + 1 if state.have_seq else 1
            state.on_raw(frame(seq=seq, sample_seq=seq,
                               t_us=device_start + i * 10000,
                               gyro_rps=gyro or [0, 0, 0]), start + i * .01)

    def test_five_samples_not_enough(self):
        state = make_state()
        self.feed(state, 1000, 5)
        self.assertFalse(state.still_ok())

    def test_complete_window_passes(self):
        state = make_state()
        self.feed(state, 1000, 50)
        self.assertFalse(state.still_ok())
        self.feed(state, 1000.5, 1)
        self.assertTrue(state.still_ok())

    def test_motion_restarts_window(self):
        state = make_state()
        self.feed(state, 1000, 51)
        self.feed(state, 1000.51, 1, gyro=[.5, 0, 0])
        self.feed(state, 1000.52, 50)
        self.assertFalse(state.still_ok())
        self.feed(state, 1001.02, 1)
        self.assertTrue(state.still_ok())

    def test_receive_gap_restarts_window(self):
        state = make_state()
        self.feed(state, 1000, 51)
        self.feed(state, 1000.7, 1)
        self.assertFalse(state.still_ok())

    def test_device_gap_restarts_window(self):
        state = make_state()
        self.feed(state, 1000, 51)
        self.feed(state, 1000.51, 1, device_start=700000)
        self.assertFalse(state.still_ok())

    def test_new_session_clears_window(self):
        state = make_state()
        self.feed(state, 1000, 51)
        state.on_raw(frame(link_epoch=2), 1000.51)
        self.assertFalse(state.still_ok())
