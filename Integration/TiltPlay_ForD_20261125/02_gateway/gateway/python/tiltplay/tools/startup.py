"""Bounded startup recenter policy; tick accepts injectable time and callbacks."""
class StartupRecenter:
    def __init__(self, start, timeout=2.8, interval=0.25):
        self.deadline = start + timeout
        self.next_attempt = start + 0.6
        self.interval = interval
        self.result = None
        self.done = False
        self.attempts = 0

    def tick(self, now, ready, send):
        if self.done:
            return
        if now >= self.deadline:
            self.result = dict(ok=False, code="STARTUP_TIMEOUT", last_result=self.result)
            self.done = True
            return
        if now < self.next_attempt or not ready():
            return
        self.attempts += 1
        self.next_attempt = now + self.interval
        self.result = send(self.attempts)
        if self.result.get("ok") or self.result.get("code") not in (
                "STALE", "NOT_STILL", "TIMEOUT"):
            self.done = True
