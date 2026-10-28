import unittest
from dataclasses import replace
from policy import Action, Command, Identity, MotionEvidence, Observation, Policy, State


IDENTITY = Identity('synthetic-session', 123, 456, 789)


def proof(**changes):
    return replace(MotionEvidence(IDENTITY, 3, 5, 6, 2, 0, True, True, True, False, 90, 100), **changes)


def obs(sequence, at, state=State.TITLE, **kw):
    return Observation(IDENTITY, sequence, sequence, at, state, verified_source=True,
                       focused=True, **kw)


class Tests(unittest.TestCase):
    def ready(self):
        p = Policy(IDENTITY, enabled=True)
        self.assertTrue(p.observe(obs(1, 0, selected_load=True), 0))
        self.send(p, Action.OPEN_LOAD, 0)
        self.assertTrue(p.observe(obs(2, 10, State.LOAD_MENU, selected_save_verified=True), 10))
        self.send(p, Action.CONFIRM_LOAD, 10)
        self.assertTrue(p.observe(obs(3, 20, State.LOADING), 20))
        self.assertTrue(p.observe(obs(4, 30, State.PLAYING, alive=True, control_available=True), 30))
        return p

    def send(self, p, action, now, **kw):
        g = p.offer(action, now)
        self.assertIsNotNone(g)
        c = Command(IDENTITY, g.number, p.last_command + 1, action, 100, **kw)
        self.assertTrue(p.submit(c, now))
        return c

    def test_disabled_bypasses_without_tracking_or_offers(self):
        p = Policy(IDENTITY)
        self.assertFalse(p.observe(obs(1, 0), 0))
        self.assertIsNone(p.offer(Action.MOVE, 0))
        self.assertEqual(p.sample(999999, exact_focus=False, reader_ok=False).mode, 'bypass')
        self.assertIsNone(p.observation)
        self.assertEqual(p.grant_number, 0)

    def test_hints_cannot_classify_or_load(self):
        p = Policy(IDENTITY, enabled=True)
        for hint in ('camera-candidate', 'save-mounted', 'note_game_menu=false', 'frames-presented'):
            p.hint(hint)
        self.assertIs(p.state, State.UNKNOWN)
        self.assertIsNone(p.offer(Action.MOVE, 0))

    def test_unverified_unknown_dead_or_focus_lost_fail_closed(self):
        for change in ({'verified_source': False}, {'state': State.UNKNOWN},
                       {'state': State.DEAD}, {'focused': False}, {'lost': 1}):
            with self.subTest(change=change):
                p = Policy(IDENTITY, enabled=True)
                self.assertFalse(p.observe(replace(obs(1, 0), **change), 0))
                self.assertEqual(p.sample(0, exact_focus=True).mode, 'neutral')
                self.assertIsNotNone(p.abort_reason)

    def test_new_game_selection_cannot_be_confirmed(self):
        p = Policy(IDENTITY, enabled=True)
        p.observe(obs(1, 0, selected_load=False), 0)
        self.assertIsNone(p.offer(Action.OPEN_LOAD, 0))

    def test_navigation_requires_observed_new_game_and_verifies_load_before_confirm(self):
        p = Policy(IDENTITY, enabled=True)
        p.observe(obs(1, 0, selected_new_game=True), 0)
        self.send(p, Action.SELECT_LOAD, 0)
        p.observe(obs(2, 10, selected_load=True), 10)
        self.assertEqual(p.sample(10, exact_focus=True).mode, 'neutral')
        self.assertIsNotNone(p.offer(Action.OPEN_LOAD, 10))

    def test_loaded_route_requires_loading_and_alive_control(self):
        p = Policy(IDENTITY, enabled=True)
        self.assertFalse(p.observe(obs(1, 0, State.PLAYING, alive=True, control_available=True), 0))
        p = self.ready()
        self.assertIs(p.state, State.PLAYING)
        self.assertFalse(p.observe(obs(5, 40, State.PLAYING, alive=False, control_available=True), 40))

    def test_menu_transition_cancels_input_before_next_sample(self):
        p = self.ready()
        self.send(p, Action.PAUSE, 30)
        p.observe(obs(5, 40, State.PAUSED), 40)
        self.assertEqual(p.sample(40, exact_focus=True).mode, 'neutral')
        self.send(p, Action.RESUME, 40)
        p.observe(obs(6, 50, State.PLAYING, alive=True, control_available=True), 50)
        self.assertEqual(p.sample(50, exact_focus=True).mode, 'neutral')

    def test_unexpected_menu_or_death_latches_stop(self):
        for state in (State.TITLE, State.LOAD_MENU, State.PAUSED, State.DEAD):
            p = self.ready()
            self.send(p, Action.MOVE, 30, ly=80)
            self.assertFalse(p.observe(obs(5, 40, state), 40))
            self.assertEqual(p.sample(40, exact_focus=True).mode, 'neutral')
            self.assertFalse(p.observe(obs(6, 50, State.PLAYING, alive=True, control_available=True), 50))

    def test_pid_reuse_window_session_and_order_rejected(self):
        for change in ({'pid': 124}, {'process_birth': 457}, {'session': 'other'}, {'window': 790}):
            p = self.ready()
            bad = replace(obs(5, 40, State.PLAYING, alive=True, control_available=True),
                          identity=replace(IDENTITY, **change))
            self.assertFalse(p.observe(bad, 40))
        for sequence, frame in ((6, 5), (4, 5), (5, 4)):
            p = self.ready()
            self.assertFalse(p.observe(replace(obs(sequence, 40, State.PLAYING,
                                                   alive=True, control_available=True), frame=frame), 40))

    def test_stale_future_clock_regression_and_run_deadline(self):
        for stamp, now in ((0, 100), (101, 100)):
            p = Policy(IDENTITY, enabled=True)
            self.assertFalse(p.observe(obs(1, stamp), now))
        p = self.ready()
        self.assertEqual(p.sample(29, exact_focus=True).mode, 'neutral')
        p = self.ready()
        self.assertEqual(p.sample(120000, exact_focus=True).mode, 'neutral')
        self.assertEqual(p.abort_reason, 'run-deadline')

    def test_expiry_is_checked_at_sample_and_never_resurrects(self):
        p = self.ready()
        self.send(p, Action.MOVE, 30, ly=80)
        self.assertEqual(p.sample(31, exact_focus=True).ly, 80)
        p.observe(obs(5, 100, State.PLAYING, alive=True, control_available=True), 100)
        self.assertEqual(p.sample(129, exact_focus=True).mode, 'action')
        self.assertEqual(p.sample(130, exact_focus=True).mode, 'neutral')
        p.observe(obs(6, 140, State.PLAYING, alive=True, control_available=True), 140)
        self.assertEqual(p.sample(140, exact_focus=True).mode, 'neutral')

    def test_observer_and_channel_failure_stop_before_expiry(self):
        for focus, reader in ((False, True), (True, False)):
            p = self.ready()
            self.send(p, Action.MOVE, 30, ly=80)
            self.assertEqual(p.sample(31, exact_focus=focus, reader_ok=reader).mode, 'neutral')
        p = self.ready()
        g = p.offer(Action.MOVE, 30)
        p.submit(Command(IDENTITY, g.number, 3, Action.MOVE, 250, ly=80), 30)
        self.assertEqual(p.sample(130, exact_focus=True).mode, 'neutral')
        self.assertEqual(p.abort_reason, 'observer-timeout')

    def test_expired_grant_repeated_command_and_no_renewal(self):
        p = self.ready()
        c = self.send(p, Action.MOVE, 30, ly=80)
        self.assertFalse(p.submit(c, 31))
        p = self.ready()
        g = p.offer(Action.MOVE, 30)
        for seq, at in ((5, 100), (6, 180), (7, 260)):
            p.observe(obs(seq, at, State.PLAYING, alive=True, control_available=True), at)
        self.assertFalse(p.submit(Command(IDENTITY, g.number, 3, Action.MOVE, 250, ly=80), 280))

    def test_invalid_command_lengths_values_and_sequence(self):
        for change in ({'duration': 0}, {'duration': 251}, {'duration': True}, {'lx': -1},
                       {'ly': 256}, {'sequence': 2}, {'sequence': 1 << 64}, {'identity': replace(IDENTITY, pid=1)}):
            p = self.ready()
            g = p.offer(Action.MOVE, 30)
            c = Command(IDENTITY, g.number, 3, Action.MOVE, 100, ly=80)
            self.assertFalse(p.submit(replace(c, **change), 30))
            self.assertEqual(p.sample(30, exact_focus=True).mode, 'neutral')

    def test_matrix_change_alone_cannot_prove_motion(self):
        for full, loss, displaced, changed in ((1, 0, True, True), (2, 1, True, True),
                                               (2, 0, False, True), (2, 0, True, False)):
            p = self.ready()
            self.send(p, Action.MOVE, 30, ly=80)
            p.observe(obs(5, 90, State.PLAYING, alive=True, control_available=True), 90)
            p.observe(obs(6, 100, State.PLAYING, alive=True, control_available=True), 100)
            p.sample(130, exact_focus=True)
            self.assertFalse(p.confirm_motion(proof(complete_intervals=full, losses=loss,
                                                    observed_displacement=displaced, camera_changed=changed), 130))

    def test_motion_proof_required_before_next_phase(self):
        p = self.ready()
        self.send(p, Action.MOVE, 30, ly=80)
        p.observe(obs(5, 100, State.PLAYING, alive=True, control_available=True), 100)
        p.sample(130, exact_focus=True)
        self.assertIsNone(p.offer(Action.PAUSE, 130))
        p = self.ready()
        self.send(p, Action.MOVE, 30, ly=80)
        p.observe(obs(5, 90, State.PLAYING, alive=True, control_available=True), 90)
        p.observe(obs(6, 100, State.PLAYING, alive=True, control_available=True), 100)
        p.sample(130, exact_focus=True)
        self.assertTrue(p.confirm_motion(proof(), 130))
        self.assertIsNotNone(p.offer(Action.PAUSE, 130))

    def test_unrelated_or_enemy_motion_proof_rejected(self):
        for change in ({'command_sequence': 2}, {'first_frame': 4}, {'last_frame': 7},
                       {'cut_or_external_motion': True}, {'verified_source': False},
                       {'first_sample_at': 29}, {'last_sample_at': 130},
                       {'identity': replace(IDENTITY, process_birth=457)}):
            p = self.ready()
            self.send(p, Action.MOVE, 30, ly=80)
            p.observe(obs(5, 90, State.PLAYING, alive=True, control_available=True), 90)
            p.observe(obs(6, 100, State.PLAYING, alive=True, control_available=True), 100)
            p.sample(130, exact_focus=True)
            self.assertFalse(p.confirm_motion(proof(**change), 130))

    def test_delayed_motion_report_cannot_override_observer_timeout(self):
        p = self.ready()
        self.send(p, Action.MOVE, 30, ly=80)
        p.observe(obs(5, 90, State.PLAYING, alive=True, control_available=True), 90)
        p.observe(obs(6, 100, State.PLAYING, alive=True, control_available=True), 100)
        p.sample(130, exact_focus=True)
        self.assertFalse(p.confirm_motion(proof(), 200))
        self.assertEqual(p.abort_reason, 'observer-timeout')

    def test_late_menu_response_is_not_associated_with_an_old_command(self):
        p = self.ready()
        self.send(p, Action.PAUSE, 30)
        seq = 5
        for at in range(80, 2030, 50):
            self.assertTrue(p.observe(obs(seq, at, State.PLAYING, alive=True, control_available=True), at))
            seq += 1
        self.assertFalse(p.observe(obs(seq, 2030, State.PAUSED), 2030))
        self.assertEqual(p.abort_reason, 'transition-timeout')

    def test_many_lease_lengths_never_emit_after_fixed_deadline(self):
        for duration in range(1, 251):
            p = self.ready()
            g = p.offer(Action.MOVE, 30)
            self.assertTrue(p.submit(Command(IDENTITY, g.number, 3, Action.MOVE, duration, ly=80), 30))
            seq = 5
            for at in range(30, 30 + duration + 2):
                if at > 30 and (at - 30) % 50 == 0:
                    p.observe(obs(seq, at, State.PLAYING, alive=True, control_available=True), at)
                    seq += 1
                out = p.sample(at, exact_focus=True)
                self.assertEqual(out.mode, 'action' if at < 30 + duration else 'neutral')


if __name__ == '__main__':
    unittest.main(verbosity=2)
