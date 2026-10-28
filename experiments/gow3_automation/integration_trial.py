"""In-memory integration contract, NOT a live control adapter.

No filesystem, process, window, socket, pipe, save or runtime API is used here.
WindowsUnavailable keeps the missing attestation terminal. Synthetic adapters
live only in the tests. Opaque Python object identity tests capability routing;
it does not establish Windows authentication or make log claims trustworthy.
"""
from collections import deque
from dataclasses import dataclass
from enum import Enum, auto
import threading
import uuid


@dataclass(frozen=True)
class LaunchIntent:
    session: str
    image_sha256: str
    isolated_root: str

    @classmethod
    def new(cls, image_sha256, isolated_root):
        return cls(uuid.uuid4().hex, image_sha256, isolated_root)


@dataclass(frozen=True)
class Binding:
    intent: LaunchIntent
    pid: int
    birth: int
    owner_cookie: object


class AttestationUnavailable(RuntimeError):
    pass


class WindowsUnavailable:
    """No calls or fallback: the framework has no approved attestation adapter."""
    def attest_created_child(self, _intent):
        raise AttestationUnavailable('windows-child-attestation-unavailable')

    def is_owned(self, _binding):
        return False


class Stage(Enum):
    UNKNOWN = auto()
    TITLE = auto()
    LOAD_DIALOG = auto()
    LOADING = auto()
    PLAYING = auto()
    CLOSING = auto()
    CLOSED = auto()
    STOPPED = auto()


class Action(Enum):
    NAVIGATE_LOAD = auto()
    OPEN_LOAD = auto()
    LOAD_EXISTING = auto()
    MOVE = auto()
    CLOSE_OWNED = auto()


@dataclass(frozen=True)
class Observation:
    sequence: int
    at: int
    stage: Stage
    focused: bool = True
    selected: str = ''
    dialog_kind: str = ''
    dialog_generation: int = 0
    existing_slot: str = ''
    caused_by: int = 0
    losses: int = 0


@dataclass(frozen=True)
class Command:
    session: str
    sequence: int
    action: Action
    duration_ms: int = 0
    lx: int = 128
    ly: int = 128
    slot: str = ''
    dialog_generation: int = 0


@dataclass(frozen=True)
class Ack:
    session: str
    command: int
    event: str
    at: int


@dataclass(frozen=True)
class Effect:
    mode: str = 'neutral'
    action: Action | None = None
    lx: int = 128
    ly: int = 128
    owned_binding: Binding | None = None


class SessionTrial:
    MAX_DURATION = 250
    OBSERVATION_TTL = 500
    MAX_RUN = 30_000
    CLOSE_TIMEOUT = 1_000

    def __init__(self, intent, adapter, *, enabled=False, now=0, ack_capacity=16):
        if type(ack_capacity) is not int or not 1 <= ack_capacity <= 64:
            raise ValueError('ack capacity must be 1..64')
        if type(now) is not int or now < 0:
            raise ValueError('initial clock must be a nonnegative integer')
        self.intent, self.adapter, self.enabled = intent, adapter, enabled
        self.binding = None
        self.stage, self.blocked = Stage.UNKNOWN, None
        self.started = self.now = now
        self.observation = self.pending = self.active = self.expected = None
        self.last_command = self.deadline = self.close_deadline = self.pending_expires = 0
        self.acks, self.ack_capacity, self.ack_losses = deque(), ack_capacity, 0
        self.lock = threading.Lock()
        if enabled:
            try:
                binding = adapter.attest_created_child(intent)
                if binding.intent != intent or not adapter.is_owned(binding):
                    raise AttestationUnavailable('launch-binding-mismatch')
                self.binding = binding
            except AttestationUnavailable as error:
                self._stop(str(error))
            except Exception:
                self._stop('attestation-adapter-error')

    def _stop(self, reason):
        self.blocked = self.blocked or reason
        self.pending = self.active = None
        self.stage = Stage.STOPPED

    def _safe(self, now):
        if not self.enabled or self.blocked or self.stage is Stage.CLOSED:
            return False
        if type(now) is not int or now < self.now or now-self.started >= self.MAX_RUN:
            self._stop('invalid-clock-or-run-deadline')
        else:
            self.now = now
            try:
                owned = self.adapter.is_owned(self.binding)
            except Exception:
                owned = False
            if not owned:
                self._stop('owned-binding-lost')
        return not self.blocked

    def _fresh(self):
        return self.observation is not None and 0 <= self.now-self.observation.at < self.OBSERVATION_TTL

    def _record(self, command, event):
        if len(self.acks) == self.ack_capacity:
            self.ack_losses += 1
            self._stop('ack-overflow')
            return False
        self.acks.append(Ack(self.intent.session, command, event, self.now))
        return True

    def take_acks(self):
        with self.lock:
            result = tuple(self.acks)
            self.acks.clear()
            return result

    def observe(self, binding, observation, now):
        with self.lock:
            if not self._safe(now):
                return False
            previous = self.observation
            if (binding is not self.binding or observation.losses or
                type(observation.sequence) is not int or observation.sequence != (previous.sequence+1 if previous else 1) or
                type(observation.at) is not int or not 0 <= now-observation.at < self.OBSERVATION_TTL or
                (previous and observation.at < previous.at) or not observation.focused):
                self._stop('observation-identity-order-focus-or-freshness')
                return False
            old, new = self.stage, observation.stage
            expected_action = self.expected.action if self.expected else None
            transitions = {(Stage.UNKNOWN, Stage.TITLE): None,
                           (Stage.TITLE, Stage.LOAD_DIALOG): Action.OPEN_LOAD,
                           (Stage.LOAD_DIALOG, Stage.LOADING): Action.LOAD_EXISTING,
                           (Stage.LOADING, Stage.PLAYING): Action.LOAD_EXISTING}
            if new != old and ((old,new) not in transitions or transitions[(old,new)] != expected_action):
                self._stop('unverified-state-transition')
                return False
            if new in (Stage.LOADING, Stage.PLAYING) and new != old and observation.caused_by != self.expected.sequence:
                self._stop('wrong-load-command')
                return False
            if new not in (Stage.TITLE, Stage.LOAD_DIALOG, Stage.LOADING, Stage.PLAYING):
                self._stop('unknown-state')
                return False
            self.observation, self.stage = observation, new
            if (new is Stage.PLAYING or new is Stage.LOAD_DIALOG or
                (new is Stage.TITLE and observation.selected == 'load')):
                self.expected = None
            return True

    def _eligible(self, c):
        o = self.observation
        if not isinstance(c.action, Action) or not o:
            return False
        if c.action is not Action.MOVE and (c.duration_ms or c.lx != 128 or c.ly != 128):
            return False
        if c.action is not Action.LOAD_EXISTING and (c.slot or c.dialog_generation):
            return False
        if c.action is Action.NAVIGATE_LOAD:
            return self.stage is Stage.TITLE and o.selected == 'new-game' and not self.expected
        if c.action is Action.OPEN_LOAD:
            return self.stage is Stage.TITLE and o.selected == 'load' and not self.expected
        if c.action is Action.LOAD_EXISTING:
            return (self.stage is Stage.LOAD_DIALOG and o.dialog_kind == 'load' and
                    c.slot != '' and c.slot == o.existing_slot and o.selected == c.slot and
                    type(c.dialog_generation) is int and c.dialog_generation > 0 and
                    c.dialog_generation == o.dialog_generation and not self.expected)
        if c.action is Action.MOVE:
            return (self.stage is Stage.PLAYING and type(c.duration_ms) is int and
                    1 <= c.duration_ms <= self.MAX_DURATION and type(c.lx) is int and
                    type(c.ly) is int and 0 <= c.lx <= 255 and 0 <= c.ly <= 255 and
                    (c.lx,c.ly) != (128,128))
        return c.action is Action.CLOSE_OWNED and self.stage not in (Stage.UNKNOWN,Stage.CLOSING)

    def submit(self, command, now):
        with self.lock:
            if not self._safe(now):
                return False
            if (not self._fresh() or command.session != self.intent.session or
                type(command.sequence) is not int or command.sequence != self.last_command+1 or
                self.pending or self.active or not self._eligible(command)):
                self._stop('command-session-sequence-state-or-overlap')
                return False
            self.pending = command  # no acceptance/consumption ack yet
            self.pending_expires = now+self.MAX_DURATION
            return True

    def sample(self, now):
        with self.lock:
            if not self.enabled:
                return Effect('bypass')
            if not self._safe(now):
                return Effect()
            if self.stage is Stage.CLOSING:
                if now >= self.close_deadline:
                    self._stop('close-unconfirmed')
                return Effect()
            if not self._fresh():
                self._stop('observation-expired')
                return Effect()
            if self.active and now >= self.deadline:
                completed, self.active = self.active, None
                self._record(completed.sequence,'neutral-at-consumer')
            if self.pending:
                c, self.pending = self.pending, None
                if now >= self.pending_expires:
                    self._stop('command-expired-before-consumption')
                    return Effect()
                if not self._eligible(c):
                    self._stop('state-changed-before-consumption')
                    return Effect()
                if not self._record(c.sequence,'consumed'):
                    return Effect()
                self.last_command = c.sequence
                if c.action is Action.MOVE:
                    self.active, self.deadline = c, now+c.duration_ms
                elif c.action is Action.CLOSE_OWNED:
                    self.stage, self.close_deadline = Stage.CLOSING, now+self.CLOSE_TIMEOUT
                    return Effect('close-request',c.action,owned_binding=self.binding)
                else:
                    self.expected = c
                    return Effect('action',c.action)
            if self.active:
                return Effect('action',Action.MOVE,self.active.lx,self.active.ly)
            return Effect()

    def exited(self, binding, command_sequence, now):
        with self.lock:
            if not self._safe(now):
                return False
            if (binding is not self.binding or self.stage is not Stage.CLOSING or
                command_sequence != self.last_command or now >= self.close_deadline):
                self._stop('exit-not-associated')
                return False
            if not self._record(command_sequence,'exit-confirmed'):
                return False
            self.stage = Stage.CLOSED
            return True
