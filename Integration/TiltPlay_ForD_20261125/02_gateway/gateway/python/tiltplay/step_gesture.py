"""Experimental world-vertical impulse recognizer; no position integration."""
import math

G = 9.80665


class StepGesture:
    def __init__(self):
        self.reset()

    def reset(self):
        self.gravity = None
        self.last_us = None
        self.quiet_since = None
        self.candidate = None
        self.locked = True
        self.cool_until = 0

    def update(self, acc, gyro, t_us, enabled):
        if not enabled:
            self.reset()
            return None
        norm = math.sqrt(sum(v*v for v in acc))
        if self.last_us is None or not 0 < t_us - self.last_us <= 50000:
            self.reset()
            self.last_us = t_us
            if abs(norm-G) < 1.0:
                self.gravity = [v/norm for v in acc]
            return None
        dt = (t_us-self.last_us)/1e6
        self.last_us = t_us
        if self.gravity is None:
            if abs(norm-G) < 1.0:
                self.gravity = [v/norm for v in acc]
            return None
        # Gravity expressed in the rotating sensor frame: dg/dt = -omega x g.
        x, y, z = self.gravity
        wx, wy, wz = gyro
        g = [x-dt*(wy*z-wz*y), y-dt*(wz*x-wx*z), z-dt*(wx*y-wy*x)]
        length = math.sqrt(sum(v*v for v in g))
        g = [v/length for v in g]
        vertical = sum(a*b for a,b in zip(acc,g))-G
        angular = math.sqrt(sum(v*v for v in gyro))
        quiet = abs(vertical) < .8 and abs(norm-G) < 1.0 and angular < .3
        if quiet:
            # Correct drift only during quiet observations, not during translation.
            g = [.98*v+.02*a/norm for v,a in zip(g,acc)]
            length = math.sqrt(sum(v*v for v in g))
            g = [v/length for v in g]
            if self.quiet_since is None:
                self.quiet_since = t_us
        else:
            self.quiet_since = None
        self.gravity = g
        if norm > 25 or angular > 2:
            self.locked = True
            self.candidate = None
            self.cool_until = t_us+500000
            return None
        if self.locked:
            if (self.quiet_since is not None and t_us-self.quiet_since >= 250000
                    and t_us >= self.cool_until):
                self.locked = False
            return None
        direction = 1 if vertical >= 3.5 else -1 if vertical <= -3.5 else 0
        if not direction:
            self.candidate = None
            return None
        if self.candidate is None or self.candidate[0] != direction:
            self.candidate = (direction,t_us)
        if t_us-self.candidate[1] >= 40000:
            onset = self.candidate[1]
            self.locked = True
            self.candidate = None
            self.cool_until = t_us+500000
            return direction,onset
        return None
