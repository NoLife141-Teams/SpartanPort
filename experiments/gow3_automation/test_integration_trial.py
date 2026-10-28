"""Synthetic in-memory tests only: no child processes, files, saves or UI."""
from dataclasses import replace
import threading
import unittest
from integration_trial import (Action, AttestationUnavailable, Binding, Command,
                               LaunchIntent, Observation, SessionTrial, Stage,
                               WindowsUnavailable)


class SyntheticAdapter:
    """Test fixture, never a supported Windows adapter or live authentication."""
    def __init__(self):
        self.binding = None
        self.valid = True
        self.attest_calls = self.check_calls = 0

    def attest_created_child(self, intent):
        self.attest_calls += 1
        self.binding = Binding(intent,123,456,object())
        return self.binding

    def is_owned(self, binding):
        self.check_calls += 1
        return self.valid and binding is self.binding


class Tests(unittest.TestCase):
    def make(self, **kw):
        a=SyntheticAdapter()
        p=SessionTrial(LaunchIntent.new('a'*64,'synthetic-isolated-root'),a,enabled=True,**kw)
        return p,a

    def observe(self,p,at,stage,**kw):
        sequence=p.observation.sequence+1 if p.observation else 1
        self.assertTrue(p.observe(p.binding,Observation(sequence,at,stage,**kw),at),p.blocked)

    def send(self,p,at,action,**kw):
        c=Command(p.intent.session,p.last_command+1,action,**kw)
        self.assertTrue(p.submit(c,at),p.blocked)
        effect=p.sample(at)
        self.assertEqual(effect.action,action,p.blocked)
        return c,effect

    def ready(self,**kw):
        p,a=self.make(**kw)
        self.observe(p,0,Stage.TITLE,selected='new-game')
        self.send(p,0,Action.NAVIGATE_LOAD)
        self.observe(p,10,Stage.TITLE,selected='load')
        self.send(p,10,Action.OPEN_LOAD)
        self.observe(p,20,Stage.LOAD_DIALOG,dialog_kind='load',dialog_generation=7,
                     selected='existing-A',existing_slot='existing-A')
        c,_=self.send(p,20,Action.LOAD_EXISTING,slot='existing-A',dialog_generation=7)
        self.observe(p,30,Stage.LOADING,caused_by=c.sequence)
        self.observe(p,40,Stage.PLAYING,caused_by=c.sequence)
        p.take_acks()
        return p,a

    def test_windows_adapter_never_arms_or_retries(self):
        class Counting(WindowsUnavailable):
            calls=0
            def attest_created_child(self,intent):
                self.calls+=1
                return super().attest_created_child(intent)
        a=Counting()
        p=SessionTrial(LaunchIntent.new('a'*64,'synthetic'),a,enabled=True)
        for now in range(5):
            self.assertFalse(p.submit(Command(p.intent.session,1,Action.OPEN_LOAD),now))
            self.assertEqual(p.sample(now).mode,'neutral')
        self.assertEqual(a.calls,1)
        self.assertEqual(p.blocked,'windows-child-attestation-unavailable')
        self.assertIsNone(p.binding)
        self.assertEqual(p.take_acks(),())

    def test_disabled_does_not_call_adapter_or_override_input(self):
        a=SyntheticAdapter()
        p=SessionTrial(LaunchIntent.new('a'*64,'synthetic'),a)
        self.assertEqual(p.sample(-1).mode,'bypass')
        self.assertFalse(p.submit(Command(p.intent.session,1,Action.OPEN_LOAD),0))
        self.assertEqual((a.attest_calls,a.check_calls),(0,0))

    def test_session_is_created_before_attestation_and_unique(self):
        p,a=self.make();q,b=self.make()
        self.assertIs(p.intent,a.binding.intent)
        self.assertNotEqual(p.intent.session,q.intent.session)
        self.assertEqual((a.binding.pid,b.binding.pid),(123,123))
        self.assertIsNot(a.binding,b.binding)

    def test_wrong_launch_binding_fails(self):
        class Wrong(SyntheticAdapter):
            def attest_created_child(self,intent):
                return super().attest_created_child(replace(intent,session='other'))
        p=SessionTrial(LaunchIntent.new('a'*64,'synthetic'),Wrong(),enabled=True)
        self.assertEqual(p.blocked,'launch-binding-mismatch')

    def test_adapter_exception_is_fail_closed(self):
        class Broken(SyntheticAdapter):
            def attest_created_child(self,_intent):raise RuntimeError('unavailable')
        p=SessionTrial(LaunchIntent.new('a'*64,'synthetic'),Broken(),enabled=True)
        self.assertEqual(p.blocked,'attestation-adapter-error')

    def test_full_typed_route_and_close_requires_exit_ack(self):
        p,_=self.ready()
        # No alive/health flag is required or present in this model.
        c,_=self.send(p,40,Action.MOVE,duration_ms=80,ly=90)
        self.assertEqual(p.sample(119).ly,90)
        self.assertEqual(p.sample(120).mode,'neutral')
        acks=p.take_acks()
        self.assertEqual([(a.command,a.event,a.at) for a in acks],
                         [(c.sequence,'consumed',40),(c.sequence,'neutral-at-consumer',120)])
        c,e=self.send(p,120,Action.CLOSE_OWNED)
        self.assertIs(e.owned_binding,p.binding)
        self.assertEqual(p.stage,Stage.CLOSING)
        self.assertNotEqual(p.stage,Stage.CLOSED)
        self.assertTrue(p.exited(p.binding,c.sequence,121))
        self.assertEqual(p.stage,Stage.CLOSED)

    def test_load_never_accepts_save_delete_new_or_wrong_slot(self):
        for changes in ({'dialog_kind':'save'},{'dialog_kind':'delete'},
                        {'selected':'new-save'},{'existing_slot':''},
                        {'existing_slot':'other'}):
            p,_=self.make()
            self.observe(p,0,Stage.TITLE,selected='load')
            self.send(p,0,Action.OPEN_LOAD)
            values=dict(dialog_kind='load',dialog_generation=7,selected='existing-A',existing_slot='existing-A')
            values.update(changes)
            self.observe(p,1,Stage.LOAD_DIALOG,**values)
            self.assertFalse(p.submit(Command(p.intent.session,2,Action.LOAD_EXISTING,
                slot='existing-A',dialog_generation=7),1))

    def test_new_game_cannot_be_confirmed_as_load(self):
        p,_=self.make()
        self.observe(p,0,Stage.TITLE,selected='new-game')
        self.assertFalse(p.submit(Command(p.intent.session,1,Action.OPEN_LOAD),0))

    def test_wrong_dialog_generation_is_rejected(self):
        p,_=self.make()
        self.observe(p,0,Stage.TITLE,selected='load');self.send(p,0,Action.OPEN_LOAD)
        self.observe(p,1,Stage.LOAD_DIALOG,dialog_kind='load',dialog_generation=8,
                     selected='existing-A',existing_slot='existing-A')
        self.assertFalse(p.submit(Command(p.intent.session,2,Action.LOAD_EXISTING,
            slot='existing-A',dialog_generation=7),1))

    def test_dialog_changes_between_submit_and_consume(self):
        p,_=self.make()
        self.observe(p,0,Stage.TITLE,selected='load');self.send(p,0,Action.OPEN_LOAD)
        self.observe(p,1,Stage.LOAD_DIALOG,dialog_kind='load',dialog_generation=7,
                     selected='existing-A',existing_slot='existing-A')
        self.assertTrue(p.submit(Command(p.intent.session,2,Action.LOAD_EXISTING,
            slot='existing-A',dialog_generation=7),1))
        self.observe(p,2,Stage.LOAD_DIALOG,dialog_kind='delete',dialog_generation=8,
                     selected='existing-A',existing_slot='existing-A')
        self.assertEqual(p.sample(2).mode,'neutral')
        self.assertEqual(p.blocked,'state-changed-before-consumption')

    def test_playing_requires_the_specific_load_completion(self):
        p,_=self.make()
        self.assertFalse(p.observe(p.binding,Observation(1,0,Stage.PLAYING),0))
        p,_=self.make()
        self.observe(p,0,Stage.TITLE,selected='load');self.send(p,0,Action.OPEN_LOAD)
        self.observe(p,1,Stage.LOAD_DIALOG,dialog_kind='load',dialog_generation=7,
                     selected='existing-A',existing_slot='existing-A')
        self.send(p,1,Action.LOAD_EXISTING,slot='existing-A',dialog_generation=7)
        self.assertFalse(p.observe(p.binding,Observation(3,2,Stage.LOADING,caused_by=999),2))

    def test_duplicate_and_foreign_commands_stop(self):
        for changes in ({'session':'other'},{'sequence':2},{'sequence':5}):
            p,_=self.ready()
            c=replace(Command(p.intent.session,4,Action.MOVE,10,ly=90),**changes)
            self.assertFalse(p.submit(c,40))
            self.assertEqual(p.sample(40).mode,'neutral')

    def test_invalid_duration_axes_and_untyped_command(self):
        for changes in ({'duration_ms':0},{'duration_ms':251},{'duration_ms':True},
                        {'ly':-1},{'ly':256},{'lx':True},{'ly':128},{'action':'MOVE'}):
            p,_=self.ready()
            c=replace(Command(p.intent.session,4,Action.MOVE,10,ly=90),**changes)
            self.assertFalse(p.submit(c,40))

    def test_pending_expiration_never_sends_late_input(self):
        p,_=self.ready()
        self.assertTrue(p.submit(Command(p.intent.session,4,Action.MOVE,10,ly=90),40))
        self.assertEqual(p.take_acks(),())
        self.assertEqual(p.sample(290).mode,'neutral')
        self.assertEqual(p.blocked,'command-expired-before-consumption')

    def test_all_250_durations_neutralize_at_consumer_deadline(self):
        for duration in range(1,251):
            p,_=self.ready()
            self.send(p,40,Action.MOVE,duration_ms=duration,ly=90)
            self.assertEqual(p.sample(40+duration-1).ly,90)
            self.assertEqual(p.sample(40+duration).mode,'neutral')
            self.assertEqual(p.sample(41+duration).mode,'neutral')
            self.assertEqual([a.event for a in p.take_acks()],['consumed','neutral-at-consumer'])

    def test_delayed_consumer_never_resurrects_expired_motion(self):
        p,_=self.ready();self.send(p,40,Action.MOVE,duration_ms=100,ly=90)
        self.assertEqual(p.sample(400).mode,'neutral')
        self.assertEqual(p.sample(401).mode,'neutral')

    def test_focus_loss_or_observer_loss_neutralizes(self):
        for changes in ({'focused':False},{'losses':1},{'stage':Stage.UNKNOWN}):
            p,_=self.ready();self.send(p,40,Action.MOVE,duration_ms=100,ly=90)
            self.assertFalse(p.observe(p.binding,replace(Observation(6,50,Stage.PLAYING),**changes),50))
            self.assertEqual(p.sample(50).mode,'neutral')

    def test_observation_order_and_freshness(self):
        for changes,now in (({'sequence':7},50),({'sequence':5},50),({'at':51},50),({'at':39},50),({},540)):
            p,_=self.ready()
            self.assertFalse(p.observe(p.binding,replace(Observation(6,40,Stage.PLAYING),**changes),now))
        p,_=self.ready();self.assertEqual(p.sample(540).mode,'neutral')
        self.assertEqual(p.blocked,'observation-expired')

    def test_clock_regression_invalid_clock_and_run_limit(self):
        for now in (39,40.5,True,30_000):
            p,_=self.ready();self.assertEqual(p.sample(now).mode,'neutral')
            self.assertEqual(p.blocked,'invalid-clock-or-run-deadline')

    def test_reused_pid_or_copied_metadata_is_not_owned_capability(self):
        p,_=self.ready()
        forged=replace(p.binding)  # same values, different capability object
        self.assertEqual(forged,p.binding)
        self.assertFalse(p.observe(forged,Observation(6,50,Stage.PLAYING),50))
        self.assertEqual(p.sample(50).mode,'neutral')

    def test_revoked_owner_and_adapter_failure_stop(self):
        for broken in (False,True):
            p,a=self.ready();self.send(p,40,Action.MOVE,duration_ms=100,ly=90)
            if broken:
                def fail(_binding):raise RuntimeError('adapter failed')
                a.is_owned=fail
            else:a.valid=False
            self.assertEqual(p.sample(50).mode,'neutral')
            self.assertEqual(p.blocked,'owned-binding-lost')

    def test_ack_buffer_is_bounded_and_loss_stops_output(self):
        p,_=self.ready()
        p.ack_capacity=1
        self.send(p,40,Action.MOVE,duration_ms=10,ly=90)
        self.assertEqual(p.sample(50).mode,'neutral')
        self.assertEqual((len(p.acks),p.ack_losses),(1,1))
        self.assertEqual(p.blocked,'ack-overflow')
        p.take_acks()
        self.assertEqual(p.sample(51).mode,'neutral')
        self.assertEqual(p.ack_losses,1)

    def test_no_effect_if_consumption_ack_cannot_be_retained(self):
        p,_=self.ready();p.ack_capacity=1
        self.send(p,40,Action.MOVE,duration_ms=10,ly=90)
        p.take_acks();p.sample(50)  # one neutral ack now fills capacity
        self.assertTrue(p.submit(Command(p.intent.session,5,Action.MOVE,10,ly=80),50))
        self.assertEqual(p.sample(50).mode,'neutral')
        self.assertEqual(p.blocked,'ack-overflow')

    def test_close_never_addresses_another_binding(self):
        p,_=self.ready();q,_=self.ready()
        c,e=self.send(p,40,Action.CLOSE_OWNED)
        self.assertIsNot(e.owned_binding,q.binding)
        self.assertFalse(p.exited(q.binding,c.sequence,50))
        self.assertNotEqual(p.stage,Stage.CLOSED)

    def test_close_timeout_does_not_claim_or_force_exit(self):
        p,_=self.ready();self.send(p,40,Action.CLOSE_OWNED)
        self.assertEqual(p.sample(1040).mode,'neutral')
        self.assertEqual(p.blocked,'close-unconfirmed')
        self.assertFalse(any(a.event=='exit-confirmed' for a in p.take_acks()))

    def test_concurrent_consumers_consume_one_command_once(self):
        p,_=self.ready()
        self.assertTrue(p.submit(Command(p.intent.session,4,Action.MOVE,10,ly=90),40))
        barrier=threading.Barrier(9);effects=[]
        def consume():
            barrier.wait();effects.append(p.sample(40))
        workers=[threading.Thread(target=consume) for _ in range(8)]
        for t in workers:t.start()
        barrier.wait()
        for t in workers:t.join(timeout=2);self.assertFalse(t.is_alive())
        self.assertEqual(len(effects),8)
        self.assertTrue(all(e.ly==90 for e in effects))
        self.assertEqual([a.event for a in p.take_acks()],['consumed'])


if __name__=='__main__':unittest.main(verbosity=2)
