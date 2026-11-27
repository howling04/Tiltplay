"""每设备输入状态机 (TP-DEV-001 §1.6.1, §3.3.2–3.3.7, §3.5)。

一个 DeviceState 只服务一个 device_id，跨设备状态完全隔离。
它负责: 启动会话/连接轮次的门控、序号去重、设备时间探测与年龄上界、
按键计数联合去重、静置回中、以及生成 UDP 输入快照。
"""
from __future__ import annotations

import math
import time
from dataclasses import dataclass, field

from .fusion import ComplementaryTilt, TiltConfig, clamp_vector, map_axis
from .protocol import RawFrame, newer
from .step_gesture import StepGesture


def mono() -> float:
    return time.monotonic()


@dataclass
class Probe:
    cmd_id: str
    sent_mono: float
    boot_id: str
    link_epoch: int
    ack_mono: float | None = None
    device_t_us: int | None = None
    ok: bool = False
    rtt_ms: float | None = None


@dataclass
class Counters:
    raw_total: int = 0
    raw_bad: int = 0
    raw_dup: int = 0
    raw_ooo: int = 0
    seq_gap: int = 0
    queue_drop: int = 0
    event_total: int = 0
    event_bad: int = 0
    event_expired: int = 0
    event_dup: int = 0
    jump_issued: int = 0
    status_total: int = 0
    acks: int = 0
    probes: int = 0
    probe_fail: int = 0
    resets: int = 0

    def as_dict(self) -> dict:
        return dict(self.__dict__)


@dataclass
class DeviceConfig:
    device_id: str
    player_slot: int = 0
    mode: str = "tilt2"
    vertical_step_enabled: bool = False
    axis_matrix: tuple = ((0, 1), (1, 1), (2, 1))


class DeviceState:
    def __init__(self, cfg: DeviceConfig, bridge_cfg, *, clock=mono):
        self.cfg = cfg
        self.bcfg = bridge_cfg
        self.clock = clock
        self.tilt = ComplementaryTilt(TiltConfig(
            deadzone_deg=bridge_cfg.deadzone_deg,
            saturation_deg=bridge_cfg.saturation_deg,
            tau_s=bridge_cfg.fusion_tau_s,
            gyro_pitch_sign=bridge_cfg.gyro_pitch_sign,
            gyro_roll_sign=bridge_cfg.gyro_roll_sign,
            axis_matrix=cfg.axis_matrix,
        ))
        self.counters = Counters()
        self.retired_boots: set[str] = set()
        self.event_dedupe: dict[str, float] = {}
        self.boot_id: str | None = None
        self.link_epoch: int | None = None
        self.log_fn = None
        self._reset_session(None, None)

    # ---------- 会话 ----------
    def _reset_session(self, boot_id: str | None, link_epoch: int | None) -> None:
        if self.boot_id is not None and self.boot_id != boot_id:
            self.retired_boots.add(self.boot_id)
        self.boot_id = boot_id
        self.link_epoch = link_epoch
        self.online = False
        self.confirmed = False
        self.have_seq = False
        self.last_seq = -1
        self.last_sample_seq = None
        self.last_sample_t_us = None
        self.last_raw_mono: float | None = None
        self.last_frame_mono: float | None = None
        self.samples_confirmed = 0
        self.probe: Probe | None = None
        self.offset_min_s: float | None = None
        self.probe_ok_mono: float | None = None
        self.centered = False
        self.neutral_f = 0.0
        self.neutral_r = 0.0
        self.press_baseline: int | None = None
        self.pending_token: int | None = None
        self.pending_press_t_us: int | None = None
        self.still_buf: list[tuple[float, float, float]] = []
        self.still_since: float | None = None
        self.still_last: float | None = None
        self.still_device_since: int | None = None
        self.still_device_last: int | None = None
        self.step_gesture = StepGesture()
        self.step_counter = 0
        self.pending_step = None
        self.tilt.reset()
        self.counters.resets += 1

    def _adopt_session(self, boot_id: str, link_epoch: int, now: float) -> None:
        if boot_id in self.retired_boots:
            return
        if self.boot_id == boot_id and self.link_epoch == link_epoch:
            return
        # Epochs are monotonic within one boot (guide 2.6/D11).
        # Do not infer seq-style wraparound for connection epochs.
        if self.boot_id == boot_id and self.link_epoch is not None and link_epoch < self.link_epoch:
            return
        self._reset_session(boot_id, link_epoch)
        self._log("session_adopted", boot_id=boot_id, link_epoch=link_epoch)

    def on_status(self, msg: dict, now: float | None = None) -> None:
        now = now or self.clock()
        self.counters.status_total += 1
        boot_id = msg["boot_id"]
        epoch = msg["link_epoch"]
        if not msg["online"]:
            if boot_id == self.boot_id and epoch == self.link_epoch:
                self.online = False
                self.pending_token = None
            return
        self._adopt_session(boot_id, epoch, now)
        if boot_id == self.boot_id and epoch == self.link_epoch:
            self.online = True

    # ---------- 时间探测 ----------
    def needs_probe(self, now: float | None = None) -> bool:
        now = now or self.clock()
        if not self.online or self.boot_id is None:
            return False
        if self.probe is None:
            return True
        if self.probe.ok:
            return (now - self.probe_ok_mono) * 1000.0 >= self.bcfg.probe_interval_ms
        return (now - self.probe.sent_mono) * 1000.0 >= self.bcfg.probe_rtt_max_ms * 4

    def on_probe_sent(self, cmd_id: str, now: float | None = None) -> None:
        now = now or self.clock()
        self.probe = Probe(cmd_id, now, self.boot_id or "", self.link_epoch or 0)
        self.counters.probes += 1

    def on_ack(self, msg: dict, now: float | None = None) -> bool:
        now = now or self.clock()
        self.counters.acks += 1
        probe = self.probe
        if probe is None or msg.get("cmd_id") != probe.cmd_id:
            return False
        if not msg.get("ok"):
            self.counters.probe_fail += 1
            probe.ok = False
            return False
        device_t_us = msg.get("t_us")
        if not isinstance(device_t_us, int):
            self.counters.probe_fail += 1
            return False
        probe.ack_mono = now
        probe.device_t_us = device_t_us
        probe.rtt_ms = (now - probe.sent_mono) * 1000.0
        # 时钟偏移的保守下界: A - D，年龄用下界估计 => 得到年龄上界
        self.offset_min_s = probe.sent_mono - device_t_us / 1e6
        if probe.rtt_ms <= self.bcfg.probe_rtt_max_ms:
            probe.ok = True
            self.probe_ok_mono = now
            self._log("probe_ok", rtt_ms=round(probe.rtt_ms, 2),
                      device_t_us=device_t_us)
        else:
            self.counters.probe_fail += 1
            probe.ok = False
            self._log("probe_rtt_exceeded", rtt_ms=round(probe.rtt_ms, 2))
        return probe.ok

    def sample_age_bound_ms(self, now: float | None = None) -> float | None:
        now = now or self.clock()
        if self.offset_min_s is None or self.last_sample_t_us is None:
            return None
        sample_host = self.last_sample_t_us / 1e6 + self.offset_min_s
        return (now - sample_host) * 1000.0 + self.bcfg.clock_margin_ms

    # ---------- 遥测 ----------
    def on_raw(self, frame: RawFrame, now: float | None = None) -> str:
        now = now or self.clock()
        self.counters.raw_total += 1

        if frame.boot_id in self.retired_boots:
            self.counters.raw_ooo += 1
            return "retired_boot"
        if self.boot_id is None or frame.boot_id != self.boot_id:
            # 新启动会话接管；旧 boot 进入退役集，迟到的旧包不会重新激活
            self._adopt_session(frame.boot_id, frame.link_epoch, now)
            if frame.boot_id != self.boot_id:
                self.counters.raw_ooo += 1
                return "wrong_session"
        elif frame.link_epoch != self.link_epoch:
            # 同一启动会话内的新网络连接轮次：重建会话基线
            self._adopt_session(frame.boot_id, frame.link_epoch, now)
            if frame.link_epoch != self.link_epoch:
                self.counters.raw_ooo += 1
                return "old_epoch"

        self.last_frame_mono = now

        if self.have_seq:
            if frame.seq == self.last_seq:
                self.counters.raw_dup += 1
                return "duplicate"
            if not newer(frame.seq, self.last_seq):
                self.counters.raw_ooo += 1
                return "out_of_order"
            expected = (self.last_seq + 1) & 0xFFFFFFFF
            if frame.seq != expected:
                self.counters.seq_gap += 1
        self.last_seq = frame.seq
        self.have_seq = True

        if self.last_sample_seq is not None and frame.sample_seq == self.last_sample_seq:
            self.counters.raw_dup += 1
            return "duplicate_sample"

        dt_s = None
        if self.last_sample_t_us is not None and frame.t_us > self.last_sample_t_us:
            dt_s = (frame.t_us - self.last_sample_t_us) / 1e6

        self.last_sample_seq = frame.sample_seq
        self.last_sample_t_us = frame.t_us
        # 仅"新的完整样本"刷新新鲜度
        self.last_raw_mono = now
        self.samples_confirmed += 1
        if not self.confirmed and self.samples_confirmed >= 3 and self.probe_ok_mono is not None:
            self.confirmed = True

        self.tilt.update(frame.acc_mps2, frame.gyro_rps, dt_s)
        self._track_still(now, frame)
        self._handle_press(frame.button.press_count, frame.button.press_t_us, now)
        if self.cfg.vertical_step_enabled:
            eligible = self.valid(now) and self.centered
            event = self.step_gesture.update(frame.acc_mps2, frame.gyro_rps,
                                             frame.t_us, eligible)
            if not eligible:
                self.pending_step = None
            elif event is not None:
                direction, onset = event
                self.step_counter = (self.step_counter + 1) & 0xffffffff
                self.pending_step = (self.step_counter, direction, onset)
                self._log('vertical_step', token=self.step_counter, direction=direction)
        return "ok"

    def step_snapshot(self, now):
        if not self.cfg.vertical_step_enabled or not self.valid(now) or not self.centered:
            self.pending_step = None
            self.step_gesture.reset()
            return None
        if self.pending_step is None:
            return None
        token, direction, onset = self.pending_step
        age = self._press_age_ms(onset, now)
        if age is None or age < 0 or age > self.bcfg.event_ttl_ms:
            self.pending_step = None
            return None
        return dict(token=token, direction=direction, age_bound_ms=round(age, 1))

    def _track_still(self, now: float, frame: RawFrame) -> None:
        gyro = math.sqrt(sum(v * v for v in frame.gyro_rps))
        acc_norm = math.sqrt(sum(v * v for v in frame.acc_mps2))
        # Require continuous observations in both receive and device time.
        # 50 ms is a local observation-gap guard, not a change to stale_ms.
        continuous = (self.still_last is not None and
                      0 <= now - self.still_last <= 0.05 and
                      self.still_device_last is not None and
                      0 < frame.t_us - self.still_device_last <= 50000)
        stationary = (gyro < self.bcfg.still_gyro_rps and
                      abs(acc_norm - 9.80665) < self.bcfg.still_acc_tol * 9.80665)
        if not stationary:
            self.still_since = None
            self.still_device_since = None
        elif not continuous or self.still_since is None:
            self.still_since = now
            self.still_device_since = frame.t_us
        self.still_last = now
        self.still_device_last = frame.t_us
        self.still_buf.append((now, gyro, acc_norm))
        horizon = now - self.bcfg.still_window_ms / 1000.0
        while self.still_buf and self.still_buf[0][0] < horizon:
            self.still_buf.pop(0)

    def _press_age_ms(self, press_t_us: int | None, now: float) -> float | None:
        if press_t_us is None or self.offset_min_s is None:
            return None
        return (now - (press_t_us / 1e6 + self.offset_min_s)) * 1000.0 + self.bcfg.clock_margin_ms

    def _handle_press(self, press_count: int, press_t_us: int | None, now: float) -> None:
        if self.press_baseline is None:
            self.press_baseline = press_count
            return
        if press_count == self.press_baseline:
            return
        advanced = (press_count - self.press_baseline) & 0xFFFFFFFF
        if advanced >= 0x80000000:  # 旧计数
            return
        self.press_baseline = press_count
        if press_t_us is None:
            self._log("press_without_time", press_count=press_count)
            return
        age = self._press_age_ms(press_t_us, now)
        if age is None or age > self.bcfg.event_ttl_ms:
            self.counters.event_expired += 1
            return
        self.pending_token = press_count
        self.pending_press_t_us = press_t_us
        self.counters.jump_issued += 1

    def on_event(self, msg: dict, now: float | None = None) -> str:
        now = now or self.clock()
        self.counters.event_total += 1
        if msg["boot_id"] in self.retired_boots:
            self.counters.event_expired += 1
            return "retired_boot"
        if msg["boot_id"] != self.boot_id or msg["link_epoch"] != self.link_epoch:
            self.counters.event_expired += 1
            return "wrong_session"

        key = msg["event_id"]
        self._purge_dedupe(now)
        if key in self.event_dedupe:
            self.counters.event_dup += 1
            return "duplicate"
        self.event_dedupe[key] = now

        press_count = msg["press_count"]
        press_t_us = msg.get("press_t_us")
        if self.press_baseline is None:
            self.press_baseline = press_count
            return "baseline"
        if press_count == self.press_baseline:
            self.counters.event_dup += 1
            return "duplicate_count"
        self.press_baseline = press_count
        age = self._press_age_ms(press_t_us, now)
        if age is None or age > self.bcfg.event_ttl_ms:
            self.counters.event_expired += 1
            return "expired"
        self.pending_token = press_count
        self.pending_press_t_us = press_t_us
        self.counters.jump_issued += 1
        return "accepted"

    def _purge_dedupe(self, now: float) -> None:
        for key in [k for k, t in self.event_dedupe.items() if now - t > 60.0]:
            del self.event_dedupe[key]

    # ---------- 回中 ----------
    def still_ok(self) -> bool:
        if (self.still_since is None or self.still_last is None or
                self.still_device_since is None or self.still_device_last is None):
            return False
        window = self.bcfg.still_window_ms / 1000.0
        if (self.still_last - self.still_since < window - 1e-9 or
                self.still_device_last - self.still_device_since <
                self.bcfg.still_window_ms * 1000):
            return False
        if not self.still_buf:
            return False
        if len(self.still_buf) < 5:
            return False
        for _, gyro, acc_norm in self.still_buf:
            if gyro > self.bcfg.still_gyro_rps:
                return False
            if abs(acc_norm - 9.80665) > self.bcfg.still_acc_tol * 9.80665:
                return False
        return True

    def recenter(self, now: float | None = None) -> str:
        now = now or self.clock()
        if not self.valid(now, ignore_center=True):
            reason = ('offline' if not self.online else
                      'probe_not_ready' if self.probe is None or not self.probe.ok else
                      'probe_expired' if self.probe_ok_mono is None or
                      (now - self.probe_ok_mono) * 1000 > self.bcfg.probe_ttl_ms else
                      'sample_missing' if self.source_age_ms(now) is None or
                      self.sample_age_bound_ms(now) is None else 'sample_stale')
            self._log('recenter_not_ready', reason=reason,
                      source_age_ms=self.source_age_ms(now),
                      source_bound_ms=self.sample_age_bound_ms(now))
            return "STALE"
        if self.bcfg.require_recenter and not self.still_ok():
            return "NOT_STILL"
        if getattr(self.bcfg, "require_level_recenter", False):
            limit = self.bcfg.recenter_level_max_deg
            if abs(self.tilt.theta_f) > limit or abs(self.tilt.theta_r) > limit:
                self._log("recenter_not_level", theta_f=self.tilt.theta_f,
                          theta_r=self.tilt.theta_r, limit_deg=limit)
                return "NOT_STILL"
        self.neutral_f = self.tilt.theta_f
        self.neutral_r = self.tilt.theta_r
        self.centered = True
        self.pending_token = None
        self.pending_step = None
        self.step_gesture.reset()
        self._log("recentered", neutral_f=round(self.neutral_f, 2),
                  neutral_r=round(self.neutral_r, 2))
        return "CENTERED"

    # ---------- 输出 ----------
    def source_age_ms(self, now: float | None = None) -> float | None:
        now = now or self.clock()
        if self.last_raw_mono is None:
            return None
        return (now - self.last_raw_mono) * 1000.0

    def valid(self, now: float | None = None, *, ignore_center: bool = False) -> bool:
        now = now or self.clock()
        if not self.online or self.boot_id is None:
            return False
        if self.probe_ok_mono is None or (now - self.probe_ok_mono) * 1000.0 > self.bcfg.probe_ttl_ms:
            return False
        age = self.source_age_ms(now)
        bound = self.sample_age_bound_ms(now)
        if age is None or bound is None:
            return False
        if age > self.bcfg.stale_ms or bound > self.bcfg.stale_ms:
            return False
        if self.bcfg.require_recenter and not self.centered and not ignore_center:
            return False
        return True

    def move(self, now: float | None = None) -> tuple[float, float]:
        now = now or self.clock()
        if not self.valid(now):
            return 0.0, 0.0
        uf = map_axis(self.tilt.theta_f - self.neutral_f,
                      self.bcfg.deadzone_deg, self.bcfg.saturation_deg)
        uf *= getattr(self.bcfg, "forward_output_sign", 1.0)
        ur = map_axis(self.tilt.theta_r - self.neutral_r,
                      self.bcfg.deadzone_deg, self.bcfg.saturation_deg)
        if self.cfg.mode == "tilt1" or getattr(self.bcfg, "force_single_axis", False):
            ur = 0.0
        return clamp_vector(uf, ur)

    def pending_jump(self, now: float | None = None) -> tuple[int | None, float | None]:
        now = now or self.clock()
        if self.pending_token is None:
            return None, None
        age = self._press_age_ms(self.pending_press_t_us, now)
        if age is None or age > self.bcfg.event_ttl_ms:
            self.counters.event_expired += 1
            self.pending_token = None
            self.pending_press_t_us = None
            return None, None
        return self.pending_token, age

    def consume_jump(self, token: int) -> bool:
        if self.pending_token is not None and self.pending_token == token:
            self.pending_token = None
            self.pending_press_t_us = None
            return True
        return False

    def snapshot(self, now: float | None = None) -> dict:
        now = now or self.clock()
        if getattr(self.bcfg, "button_jump_enabled", False):
            token, jump_age = self.pending_jump(now)
        else:
            token, jump_age = None, None
        valid = self.valid(now)
        move = self.move(now)
        angles = [0.0, 0.0, 0.0]
        if self.cfg.mode == "head3":
            angles = [round(self.tilt.theta_f - self.neutral_f, 2), 0.0,
                      round(self.tilt.theta_r - self.neutral_r, 2)]
        return {
            "valid": valid,
            "move": [round(move[0], 4), round(move[1], 4)],
            "angles_deg": angles,
            "jump_token": token,
            "jump_age_bound_ms": None if jump_age is None else round(jump_age, 1),
            "source_age_ms": None if self.source_age_ms(now) is None else round(self.source_age_ms(now), 1),
            "source_bound_ms": None if self.sample_age_bound_ms(now) is None else round(self.sample_age_bound_ms(now), 1),
        }

    def diagnostics(self, now: float | None = None) -> dict:
        now = now or self.clock()
        return {
            "device_id": self.cfg.device_id,
            "boot_id": self.boot_id,
            "link_epoch": self.link_epoch,
            "online": self.online,
            "confirmed": self.confirmed,
            "centered": self.centered,
            "probe_ok": bool(self.probe and self.probe.ok),
            "probe_rtt_ms": None if not self.probe else round(self.probe.rtt_ms or 0, 2),
            "source_age_ms": self.source_age_ms(now),
            "source_bound_ms": self.sample_age_bound_ms(now),
            "valid": self.valid(now),
            "theta_f": round(self.tilt.theta_f, 2),
            "theta_r": round(self.tilt.theta_r, 2),
            "counters": self.counters.as_dict(),
        }

    def _log(self, event: str, **fields) -> None:
        sink = getattr(self, "log_fn", None)
        if sink:
            sink(event, device_id=self.cfg.device_id, **fields)
