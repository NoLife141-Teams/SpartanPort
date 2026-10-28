"""Offline reference model only; no runtime import, I/O, input or process control.

`verified_source` is a contract for a FUTURE audited adapter, not an implemented
GoW3 state detector. Synthetic tests supply it explicitly. The live game must
remain unarmed until that adapter and the neutral-input integration are reviewed.
All times are supplied by the consumer's own monotonic clock, in milliseconds.
"""
from dataclasses import dataclass
from enum import Enum, auto


class State(Enum):
    UNKNOWN = auto()
    TITLE = auto()
    LOAD_MENU = auto()
    LOADING = auto()
    PLAYING = auto()
    PAUSED = auto()
    DEAD = auto()


class Action(Enum):
    SELECT_LOAD = auto()
    OPEN_LOAD = auto()
    CONFIRM_LOAD = auto()
    MOVE = auto()
    PAUSE = auto()
    RESUME = auto()


@dataclass(frozen=True)
class Identity:
    session: str
    pid: int
    process_birth: int
    window: int


@dataclass(frozen=True)
class Observation:
    identity: Identity
    sequence: int
    frame: int
    at: int
    state: State
    verified_source: bool = False
    focused: bool = False
    selected_load: bool = False
    selected_new_game: bool = False
    selected_save_verified: bool = False
    control_available: bool = False
    alive: bool = False
    lost: int = 0


@dataclass(frozen=True)
class Grant:
    identity: Identity
    number: int
    epoch: int
    observation: int
    expires: int
    action: Action


@dataclass(frozen=True)
class Command:
    identity: Identity
    grant: int
    sequence: int
    action: Action
    duration: int
    lx: int = 128
    ly: int = 128


@dataclass(frozen=True)
class Output:
    mode: str  # bypass (disabled), neutral, or action; adapter not implemented
    action: Action | None = None
    lx: int = 128
    ly: int = 128


@dataclass(frozen=True)
class MotionEvidence:
    identity: Identity
    command_sequence: int
    first_frame: int
    last_frame: int
    complete_intervals: int
    losses: int
    verified_source: bool = False
    observed_displacement: bool = False
    camera_changed: bool = False
    cut_or_external_motion: bool = True
    first_sample_at: int = 0
    last_sample_at: int = 0


class Policy:
    MAX_LEASE = 250
    OBSERVATION_TTL = 100
    MAX_RUN = 120_000

    def __init__(self, identity: Identity, *, enabled=False, now=0):
        self.identity = identity
        self.enabled = enabled
        self.started = self.last_time = now
        self.observation = None
        self.state = State.UNKNOWN
        self.epoch = 0
        self.last_grant_observation = 0
        self.grant_number = 0
        self.grant = None
        self.active = None
        self.deadline = 0
        self.last_command = 0
        self.abort_reason = None
        self.accepted_action = None
        self.transition_deadline = None
        self.motion_capture_pending = False
        self.motion_start_frame = None
        self.motion_accepted_at = self.motion_deadline = None

    def abort(self, reason):
        self.abort_reason = self.abort_reason or reason
        self.active = self.grant = None

    def clock(self, now):
        if type(now) is not int or now < self.last_time:
            self.abort('clock-regression')
            return False
        self.last_time = now
        if now - self.started >= self.MAX_RUN:
            self.abort('run-deadline')
        return not self.abort_reason

    def hint(self, _name):
        """Camera candidates, save mounts and guessed menu flags are not evidence."""

    def observe(self, obs: Observation, now):
        if not self.enabled or not self.clock(now):
            return False
        if obs.identity != self.identity:
            self.abort('identity-mismatch')
        elif not obs.verified_source:
            self.abort('unverified-observer')
        elif not obs.focused:
            self.abort('focus-lost')
        elif obs.lost:
            self.abort('observer-loss')
        elif type(obs.at) is not int or obs.at > now or now - obs.at >= self.OBSERVATION_TTL:
            self.abort('stale-observation')
        elif (type(obs.sequence) is not int or type(obs.frame) is not int or
              not 0 < obs.sequence < 1 << 64 or not 0 < obs.frame < 1 << 64):
            self.abort('invalid-observation')
        elif self.observation and (obs.sequence != self.observation.sequence + 1 or
                                  obs.frame <= self.observation.frame or
                                  obs.at < self.observation.at):
            self.abort('observation-order')
        elif not self.observation and obs.sequence != 1:
            self.abort('missing-baseline')
        elif obs.state in (State.UNKNOWN, State.DEAD):
            self.abort('dead' if obs.state is State.DEAD else 'unknown-state')
        elif obs.state is State.PLAYING and not (obs.alive and obs.control_available):
            self.abort('not-controllable-alive')
        if self.abort_reason:
            return False
        if self.transition_deadline is not None and now >= self.transition_deadline:
            self.abort('transition-timeout')
            return False

        if self.observation and obs.state is not self.state:
            allowed = {
                (State.TITLE, State.LOAD_MENU): Action.OPEN_LOAD,
                (State.LOAD_MENU, State.LOADING): Action.CONFIRM_LOAD,
                (State.LOADING, State.PLAYING): Action.CONFIRM_LOAD,
                (State.PLAYING, State.PAUSED): Action.PAUSE,
                (State.PAUSED, State.PLAYING): Action.RESUME,
            }
            expected = allowed.get((self.state, obs.state))
            if expected is None or self.accepted_action is not expected:
                self.abort('unexpected-state-transition')
                return False
            self.epoch += 1
            self.active = self.grant = None  # never carry a button into a new menu
            if obs.state is not State.LOADING:
                self.accepted_action = None
                self.transition_deadline = None
            else:
                self.transition_deadline = now + 30_000
        elif not self.observation:
            if obs.state is not State.TITLE:
                self.abort('unverified-load-route')
                return False
            self.epoch += 1
        self.observation, self.state = obs, obs.state
        if self.accepted_action is Action.SELECT_LOAD and obs.selected_load:
            self.active = self.grant = None
            self.accepted_action = self.transition_deadline = None
        return True

    def safe(self, now):
        if not self.clock(now) or not self.observation:
            return False
        if now - self.observation.at >= self.OBSERVATION_TTL:
            self.abort('observer-timeout')
        if self.transition_deadline is not None and now >= self.transition_deadline:
            self.abort('transition-timeout')
        return not self.abort_reason

    def offer(self, action, now):
        if not self.enabled or not self.safe(now):
            return None
        obs = self.observation
        if self.motion_capture_pending:
            self.abort('motion-proof-required')
            return None
        eligible = {
            Action.SELECT_LOAD: self.state is State.TITLE and obs.selected_new_game and not obs.selected_load,
            Action.OPEN_LOAD: self.state is State.TITLE and obs.selected_load,
            Action.CONFIRM_LOAD: self.state is State.LOAD_MENU and obs.selected_save_verified,
            Action.MOVE: self.state is State.PLAYING and obs.alive and obs.control_available,
            Action.PAUSE: self.state is State.PLAYING,
            Action.RESUME: self.state is State.PAUSED,
        }
        if not eligible.get(action, False):
            self.abort('action-state-mismatch')
            return None
        if obs.sequence <= self.last_grant_observation or self.active or self.grant:
            self.abort('grant-reuse-or-overlap')
            return None
        self.last_grant_observation = obs.sequence
        self.grant_number += 1
        self.grant = Grant(self.identity, self.grant_number, self.epoch,
                           obs.sequence, now + self.MAX_LEASE, action)
        return self.grant

    def submit(self, command: Command, now):
        if not self.enabled or not self.safe(now):
            return False
        grant = self.grant
        self.grant = None  # every attempt consumes the one-shot grant
        if (not grant or command.identity != self.identity or command.grant != grant.number or
                grant.epoch != self.epoch or command.action is not grant.action or now >= grant.expires):
            self.abort('invalid-or-expired-grant')
        elif (type(command.sequence) is not int or command.sequence <= self.last_command or
              command.sequence > (1 << 64) - 1):
            self.abort('command-replay')
        elif (type(command.duration) is not int or not 0 < command.duration <= self.MAX_LEASE or
              type(command.lx) is not int or type(command.ly) is not int or
              not 0 <= command.lx <= 255 or not 0 <= command.ly <= 255):
            self.abort('invalid-command')
        elif command.action is not Action.MOVE and (command.lx, command.ly) != (128, 128):
            self.abort('axes-outside-movement')
        elif command.action is Action.MOVE and (command.lx, command.ly) == (128, 128):
            self.abort('neutral-movement-request')
        if self.abort_reason:
            return False
        self.last_command = command.sequence
        self.active = command
        self.accepted_action = command.action
        if command.action is not Action.MOVE:
            self.transition_deadline = now + 2_000
        self.deadline = min(grant.expires, now + command.duration)
        self.motion_capture_pending = command.action is Action.MOVE
        if self.motion_capture_pending:
            self.motion_start_frame = self.observation.frame
            self.motion_accepted_at, self.motion_deadline = now, self.deadline
        return True

    def sample(self, now, *, exact_focus, reader_ok=True):
        if not self.enabled:
            return Output('bypass')  # no host input change on the disabled path
        if not exact_focus:
            self.abort('focus-lost')
        if not reader_ok:
            self.abort('channel-unavailable')
        if not self.safe(now):
            return Output('neutral')
        if self.active and now >= self.deadline:
            self.active = None  # later observations cannot resurrect this command
        if not self.active:
            return Output('neutral')
        return Output('action', self.active.action, self.active.lx, self.active.ly)

    def confirm_motion(self, proof: MotionEvidence, now):
        """Requires future correlated visual evidence, not matrix differences alone."""
        if not self.safe(now) or not self.motion_capture_pending or self.active:
            self.abort('motion-proof-order')
            return False
        if (proof.identity != self.identity or proof.command_sequence != self.last_command or
                not proof.verified_source or proof.first_frame <= self.motion_start_frame or
                proof.last_frame != proof.first_frame + 1 or proof.last_frame > self.observation.frame or
                not self.motion_accepted_at <= proof.first_sample_at < proof.last_sample_at < self.motion_deadline or
                proof.complete_intervals != 2 or proof.losses or not proof.observed_displacement or
                not proof.camera_changed or proof.cut_or_external_motion):
            self.abort('motion-not-proven')
            return False
        self.motion_capture_pending = False
        return True
