import unittest
from tiltplay.step_gesture import StepGesture, G
from tiltplay.bridge import Bridge, BridgeConfig
from helpers import make_state, attach_probe, frame, DEVICE, BOOT


class GestureTests(unittest.TestCase):
    def gesture(self, direction, axis=2):
        detector = StepGesture()
        events = []
        for i in range(150):
            a = [0.,0.,0.]
            a[axis] = G + (direction*4 if 40 <= i < 47 else
                           -direction*4 if 47 <= i < 54 else 0)
            result = detector.update(a, [0,0,0], i*10000, True)
            if result:
                events.append(result)
        return events

    def test_up_only_one_event_despite_braking(self):
        self.assertEqual(self.gesture(1), [(1,400000)])

    def test_down_only_one_event_despite_braking(self):
        self.assertEqual(self.gesture(-1), [(-1,400000)])

    def test_tilted_board_uses_gravity_reference(self):
        self.assertEqual(self.gesture(1, axis=0), [(1,400000)])

    def test_spike_and_disabled_do_not_trigger(self):
        d = StepGesture()
        for i in range(100):
            self.assertIsNone(d.update([0,0,G+(4 if i==40 else 0)],
                                       [0,0,0], i*10000, True))
        self.assertIsNone(d.update([0,0,G+4],[0,0,0],1000000,False))

    def test_impact_does_not_trigger(self):
        d = StepGesture()
        for i in range(100):
            self.assertIsNone(d.update([0,0,30 if 40<=i<47 else G],
                                       [0,0,0],i*10000,True))

    def test_state_event_age_and_session_reset(self):
        s = make_state()
        s.cfg.vertical_step_enabled = True
        attach_probe(s)
        s.centered = True
        for i in range(55):
            s.on_raw(frame(seq=i, sample_seq=i, t_us=i*10000,
                           acc_mps2=[0,0,G+(4 if 40<=i<47 else 0)]),1000+i*.01)
        event = s.step_snapshot(1000.54)
        self.assertEqual(event['direction'],1)
        self.assertGreaterEqual(event['age_bound_ms'],140)
        self.assertIsNone(s.step_snapshot(1000.61))
        s.pending_step = (3,1,540000)
        s.on_raw(frame(link_epoch=2),1000.62)
        self.assertIsNone(s.pending_step)

    def test_default_wire_shape_and_ack_scope(self):
        bridge = Bridge(BridgeConfig(),console=False)
        try:
            state = bridge._state_for(DEVICE)
            attach_probe(state)
            state.on_raw(frame(),1000)
            self.assertNotIn('vertical_step',bridge._udp_frame(state,1000))
            state.cfg.vertical_step_enabled = True
            state.centered = True
            state.pending_step = (0,-1,0)
            self.assertEqual(bridge._udp_frame(state,1000)['vertical_step']['token'],0)
            ack = dict(schema=2,device_id=DEVICE,bridge_id=bridge.bridge_id,
                       boot_id=BOOT,link_epoch=1,token=0)
            bridge._handle_step_ack(dict(ack,boot_id='0000000000000000'))
            self.assertIsNotNone(state.pending_step)
            bridge._handle_step_ack(ack)
            self.assertIsNone(state.pending_step)
        finally:
            bridge.udp_sock.close()
            bridge.log.close()
