"""Local synthetic validation only; no live game or UI input."""
import importlib.util
import json
import hashlib
import os
from pathlib import Path
import tempfile
import unittest

spec=importlib.util.spec_from_file_location('step',Path(__file__).resolve().parents[1]/'scripts/gow3_native_menu_step.py')
step=importlib.util.module_from_spec(spec);spec.loader.exec_module(step)

class TestNativeMenuScript(unittest.TestCase):
    def setUp(self):
        base=Path(__file__).resolve().parents[1]/'out'/'native-menu-script'/'fixtures'
        base.mkdir(parents=True, exist_ok=True)
        self.root=Path(tempfile.mkdtemp(dir=base))  # Retained task-local fixture evidence.
        self.old=(step.DATA,step.PROBE,step.HASH)
        step.DATA=self.root/'data';step.PROBE=self.root/'probe.bin';step.PROBE.write_bytes(b'fixture')
        step.HASH=hashlib.sha256(b'fixture').hexdigest()
        self.s=step.DATA/'sessions'/'one';self.n=self.s/'native'
        self.n.mkdir(parents=True);(self.s/'logs').mkdir()
        self.write('launch.json',{'session':str(self.s),'probe':str(step.PROBE),'backupVerified':True,'probeHash':step.HASH})
        self.env={'SPARTAN_USER_DIR':str(step.DATA/'user'),'SPARTAN_GPU_USER_DIR':str(step.DATA/'user'),
                  'SPARTAN_UPSCALER':'none','SPARTAN_DLSS':'0','SPARTAN_JITTER':'0','SPARTAN_AUDIO':'none',
                  'SPARTAN_PAD_FILE':str(self.n/'menu-command.txt'),'SPARTAN_PAD_REPLAY':str(self.n/'menu-down.replay'),
                  'SPARTAN_KEY_TRACE_FILE':str(self.s/'logs'/'keyboard.jsonl')}
        self.write('launch-environment.json',self.env)
        self.header={'type':'header','schema':'keyboard-s-left-y-v1','pid':42,'birth':99,'start':100,'frequency':1000}
        self.row={'type':'read_range','end':150,'context0':17,'context1':17,'flags':25,'host_y':128,'final_y':128}
        self.trace();(self.n/'menu-down.replay').write_bytes(step.RECIPE);(self.n/'menu-command.txt').write_bytes(b'')
        self.ident={'birth':99,'image':str(step.PROBE)}
    def tearDown(self):
        step.DATA,step.PROBE,step.HASH=self.old
    def write(self,name,data):(self.s/name).write_text(json.dumps(data),encoding='utf-8')
    def trace(self,extra=None):
        (self.s/'logs'/'keyboard.jsonl').write_text('\n'.join(json.dumps(x) for x in [self.header,self.row]+(extra or []))+'\n',encoding='utf-8')
    def validate(self,now=200):return step.validate(self.s,42,99,self.ident,now)
    def test_valid_publish_once(self):
        self.assertEqual(self.validate(),self.n);step.publish(self.n,42,99)
        self.assertEqual((self.n/'menu-command.txt').read_bytes(),b'replay\n')
        with self.assertRaises(FileExistsError):step.publish(self.n,42,99)
    def test_uncertain_intent_never_retries(self):
        (self.n/'down-intent.json').write_text('{}')
        with self.assertRaises(FileExistsError):step.publish(self.n,42,99)
        self.assertEqual((self.n/'menu-command.txt').read_bytes(),b'')
    def test_wrong_process(self):
        for field,value in [('birth',98),('image',str(self.root/'other.exe'))]:
            old=self.ident[field];self.ident[field]=value
            with self.assertRaises(RuntimeError):self.validate()
            self.ident[field]=old
    def test_wrong_trace_pid_birth(self):
        for field,value in [('pid',43),('birth',98)]:
            old=self.header[field];self.header[field]=value;self.trace()
            with self.assertRaises(RuntimeError):self.validate()
            self.header[field]=old
    def test_stale_or_expired(self):
        for now in (99,650,10100):
            with self.assertRaises(RuntimeError):self.validate(now)
    def test_stopped(self):
        self.trace([{'type':'stop'}])
        with self.assertRaises(RuntimeError):self.validate()
    def test_focus_or_input_unknown(self):
        for field,value in [('context1',16),('flags',31),('host_y',255),('final_y',255)]:
            old=self.row[field];self.row[field]=value;self.trace()
            with self.assertRaises(RuntimeError):self.validate()
            self.row[field]=old
    def test_wrong_configuration(self):
        self.env['SPARTAN_PAD_FILE']=str(self.root/'other.txt');self.write('launch-environment.json',self.env)
        with self.assertRaises(RuntimeError):self.validate()
    def test_recipe_changed(self):
        (self.n/'menu-down.replay').write_bytes(b'cross')
        with self.assertRaises(RuntimeError):self.validate()
    def test_probe_changed(self):
        step.PROBE.write_bytes(b'other')
        with self.assertRaises(RuntimeError):self.validate()
    def test_already_triggered(self):
        (self.n/'menu-command.txt').write_bytes(b'replay\n')
        with self.assertRaises(RuntimeError):self.validate()
    def test_query_current_process_read_only(self):
        identity=step.process_identity(os.getpid())
        self.assertGreater(identity['birth'],0);self.assertTrue(Path(identity['image']).is_file())

if __name__=='__main__':unittest.main()
