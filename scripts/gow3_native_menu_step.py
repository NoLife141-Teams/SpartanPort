"""One supervised native Down replay, not a full autonomous load/movement driver.

No Windows input API is used. Computer Use must verify the exact window, focus,
and New Game selection immediately before invoking this once. A PID, birth time,
image hash and active trace must match our launch. Only the session's native
replay trigger is written. The existing replay reaches neutral after 200 ms on
the consumer's clock even if this script exits; host input then resumes.

No confirm command: the existing host save selector needs SDL events and is not
driven by pad replay. A native adapter for that dialog has not been added.
"""
import argparse
import ctypes
from ctypes import wintypes as W
import hashlib
import json
import os
from pathlib import Path
import time

REPO = Path(__file__).resolve().parents[1]
DATA = REPO / 'out' / 'gow3-input-collection-f586605'
PROBE = REPO / 'out' / 'dlaa-s-stick-trace' / 'bin' / 'spartanport-probe.exe'
HASH = 'e38f220dbe8b2cebb9f4bbea1ca79c47e72172c44573e61b1848154faad44800'
RECIPE = b'0 0 128 255 128 128 0 0\n200 0 128 128 128 128 0 0\n'

def no_reparse(path):
    for p in (path, *path.parents):
        if p.exists() and getattr(p.lstat(), 'st_file_attributes', 0) & 0x400:
            raise RuntimeError('Reparse point refused: '+str(p))

def read_json(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))

def process_identity(pid):
    k = ctypes.WinDLL('kernel32', use_last_error=True)
    k.OpenProcess.argtypes = [W.DWORD, W.BOOL, W.DWORD]; k.OpenProcess.restype = W.HANDLE
    k.CloseHandle.argtypes = [W.HANDLE]; k.CloseHandle.restype = W.BOOL
    k.GetProcessTimes.argtypes = [W.HANDLE,*([ctypes.POINTER(W.FILETIME)]*4)]; k.GetProcessTimes.restype=W.BOOL
    k.GetExitCodeProcess.argtypes=[W.HANDLE,ctypes.POINTER(W.DWORD)]; k.GetExitCodeProcess.restype=W.BOOL
    k.QueryFullProcessImageNameW.argtypes=[W.HANDLE,W.DWORD,W.LPWSTR,ctypes.POINTER(W.DWORD)]; k.QueryFullProcessImageNameW.restype=W.BOOL
    handle=k.OpenProcess(0x1000,False,pid)  # query-limited only; no process writes/termination
    if not handle: raise ctypes.WinError(ctypes.get_last_error())
    try:
        born,exit_,kernel,user=(W.FILETIME() for _ in range(4));code=W.DWORD();size=W.DWORD(32768);image=ctypes.create_unicode_buffer(size.value)
        if not k.GetProcessTimes(handle,ctypes.byref(born),ctypes.byref(exit_),ctypes.byref(kernel),ctypes.byref(user)): raise ctypes.WinError(ctypes.get_last_error())
        if not k.GetExitCodeProcess(handle,ctypes.byref(code)) or code.value!=259: raise RuntimeError('Target process is not active')
        if not k.QueryFullProcessImageNameW(handle,0,image,ctypes.byref(size)): raise ctypes.WinError(ctypes.get_last_error())
        return {'birth':(born.dwHighDateTime<<32)|born.dwLowDateTime,'image':image.value}
    finally:k.CloseHandle(handle)

def qpc():
    k=ctypes.WinDLL('kernel32',use_last_error=True);v=ctypes.c_longlong()
    k.QueryPerformanceCounter.argtypes=[ctypes.POINTER(ctypes.c_longlong)]; k.QueryPerformanceCounter.restype=W.BOOL
    if not k.QueryPerformanceCounter(ctypes.byref(v)):raise ctypes.WinError(ctypes.get_last_error())
    return v.value

def validate(session,pid,birth,identity,now):
    session=Path(os.path.abspath(session));no_reparse(session)
    if session.parent != DATA/'sessions': raise RuntimeError('Session outside isolated task')
    for p in (session/'launch.json',session/'launch-environment.json',PROBE):no_reparse(p)
    launch=read_json(session/'launch.json');env=read_json(session/'launch-environment.json')
    if Path(launch['session'])!=session or Path(launch['probe'])!=PROBE:raise RuntimeError('Launch identity mismatch')
    if not launch['backupVerified'] or launch['probeHash'].lower()!=HASH:raise RuntimeError('Launch verification missing')
    if hashlib.sha256(PROBE.read_bytes()).hexdigest()!=HASH:raise RuntimeError('Probe hash changed')
    if identity['birth']!=birth or Path(identity['image'])!=PROBE:raise RuntimeError('PID birth/image mismatch')
    expected={'SPARTAN_USER_DIR':str(DATA/'user'),'SPARTAN_GPU_USER_DIR':str(DATA/'user'),
              'SPARTAN_UPSCALER':'none','SPARTAN_DLSS':'0','SPARTAN_JITTER':'0','SPARTAN_AUDIO':'none',
              'SPARTAN_PAD_FILE':str(session/'native'/'menu-command.txt'),
              'SPARTAN_PAD_REPLAY':str(session/'native'/'menu-down.replay')}
    for key,value in expected.items():
        if env.get(key)!=value:raise RuntimeError('Unexpected launch setting '+key)
    if env.get('SPARTAN_PAD_RECORD'):raise RuntimeError('Unexpected recording')
    trace=Path(env['SPARTAN_KEY_TRACE_FILE'])
    if trace!=session/'logs'/'keyboard.jsonl':raise RuntimeError('Trace outside session')
    no_reparse(trace)
    lines=trace.read_text(encoding='utf-8').splitlines();h=json.loads(lines[0])
    if h['schema']!='keyboard-s-left-y-v1' or h['pid']!=pid or h['birth']!=birth:raise RuntimeError('Trace identity mismatch')
    if not h['start']<=now<h['start']+10*h['frequency']:raise RuntimeError('Trace is stale')
    rows=[json.loads(line) for line in lines[1:]]
    if any(row.get('type')=='stop' for row in rows):raise RuntimeError('Trace already stopped')
    reads=[row for row in rows if row.get('type')=='read_range']
    if not reads:raise RuntimeError('No sampled neutral/focus observation yet')
    last=reads[-1]
    if not 0 <= now-last['end'] < h['frequency']//2:raise RuntimeError('Sampled observation is stale')
    if last['context0']!=last['context1'] or last['context1']&7!=1 or last['flags']!=25 or last['host_y']!=128 or last['final_y']!=128:
        raise RuntimeError('Focus, capture, physical input or neutral state is not established')
    native=session/'native';no_reparse(native)
    no_reparse(native/'menu-down.replay');no_reparse(native/'menu-command.txt')
    if (native/'menu-down.replay').read_bytes()!=RECIPE:raise RuntimeError('Replay recipe changed')
    if (native/'menu-command.txt').read_bytes()!=b'':raise RuntimeError('Command already published')
    return native

def publish(native,pid,birth):
    # Exclusive intent marker rejects retries even after an uncertain outcome.
    with (native/'down-intent.json').open('x',encoding='utf-8') as f:
        json.dump({'pid':pid,'birth':birth,'utc':time.time(),'duration_ms':200,'action':'native left Y=255 then neutral'},f)
    staging=native/'down-command.staging'
    with staging.open('xb') as f:f.write(b'replay\n');f.flush();os.fsync(f.fileno())
    # Same directory publication. Initial command is empty; size changes 0 -> 7,
    # so Windows whole-second mtime caching cannot hide it.
    os.replace(staging,native/'menu-command.txt')
    return {'published':True,'action':'native Down replay','duration_ms':200,'neutral_in_recipe':True,
            'acknowledged_by_game':False,'warning':'Verify final_y and menu visually before any next action'}

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--session',required=True,type=Path)
    p.add_argument('--pid',required=True,type=int);p.add_argument('--birth',required=True,type=int)
    args=p.parse_args()
    identity=process_identity(args.pid)
    native=validate(args.session,args.pid,args.birth,identity,qpc())
    print(json.dumps(publish(native,args.pid,args.birth)))

if __name__=='__main__':main()
