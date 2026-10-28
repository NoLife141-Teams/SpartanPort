"""Prepare and optionally launch an isolated, silent GoW3 diagnostics session.

The UI remains controlled through Computer Use after observing each screen.
No timed input sequence, automatic save selection or game-file modification.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def hashes(directory):
    result = {}
    for path in sorted(directory.rglob('*')):
        if path.is_symlink() or getattr(path.lstat(), 'st_file_attributes', 0) & 0x400:
            raise ValueError('Reparse point in source: ' + str(path))
        if path.is_file():
            result[path.relative_to(directory).as_posix()] = hashlib.sha256(path.read_bytes()).hexdigest()
    return result


def prepare(seed, game, probe, pixels=False):
    seed, game, probe = seed.resolve(), game.resolve(), probe.resolve()
    if not (game / 'eboot.bin').is_file() or not probe.is_file():
        raise ValueError('Game eboot.bin or validated probe is missing')
    savedata = seed / 'user/savedata'
    before = hashes(savedata)
    if not before:
        raise ValueError('No test save found')
    stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    session = ROOT / 'out/gow3-sessions' / stamp
    session.mkdir(parents=True, exist_ok=False)
    for name in ('runtime/user', 'backup', 'logs', 'captures', 'triggers', 'tmp', 'bin'):
        (session / name).mkdir(parents=True)
    source_probe = probe
    probe_hash = hashlib.sha256(source_probe.read_bytes()).hexdigest()
    probe = session / 'bin/spartanport-probe.exe'
    shutil.copy2(source_probe, probe)
    if hashlib.sha256(probe.read_bytes()).hexdigest() != probe_hash:
        raise ValueError('Probe copy verification failed')
    for destination in (session / 'backup/savedata', session / 'runtime/user/savedata'):
        shutil.copytree(savedata, destination)
        if hashes(destination) != before:
            raise ValueError('Test save copy verification failed')
    if hashes(savedata) != before:
        raise ValueError('Source save changed while copying')
    cache = seed / 'user/cache'
    if cache.is_dir():
        hashes(cache)  # Reject reparse points before copying the existing shader cache.
        shutil.copytree(cache, session / 'runtime/user/cache')
    settings = {}
    for line in (seed / 'spartanport.ini').read_text(encoding='utf-8-sig').splitlines():
        if '=' in line and not line.startswith(('#', ';')):
            key, value = line.split('=', 1)
            settings[key] = value
    settings.update(cheat_health='1', display_mode='windowed', mouse_lock='0',
                    upscaler='none', jitter='0', object_motion='0', reactive='0',
                    fsr1='0', rcas='0', restart_unconfirmed='0')
    config = session / 'runtime/spartanport.ini'
    config.write_text(''.join(f'{k}={v}\n' for k, v in settings.items()), encoding='utf-8')
    user = session / 'runtime/user'
    variables = {
        'SPARTAN_GAME_DIR': str(game), 'SPARTAN_DATA_DIR': str(session / 'runtime'),
        'SPARTAN_USER_DIR': str(user), 'SPARTAN_GPU_USER_DIR': str(user),
        'SPARTAN_CONFIG': str(config), 'SPARTAN_PROBE': str(probe),
        'MSYS2_ROOT': str(ROOT / 'out/toolchain/msys64'),
        'SPARTAN_AUDIO': 'none', 'SPARTAN_PATCHES': 'Skip Intro',
        'SPARTAN_MODS_ENABLED': '0', 'SPARTAN_DISCORD': '0',
        'SPARTAN_UPSCALER': 'none', 'SPARTAN_EXPERIMENTAL_UPSCALER': '0',
        'SPARTAN_DLSS': '0', 'SPARTAN_JITTER': '0', 'SPARTAN_OBJECT_MOTION': '0',
        'SPARTAN_DEBUG_MOTION': '0', 'SPARTAN_FSR1': '0', 'SPARTAN_RCAS': '0',
        'SPARTAN_DRAW_PIPE': '1', 'SPARTAN_MOUSE_LOCK': '0', 'SPARTAN_HDR': '0',
        'SPARTAN_GOW3_INPUT_DIAGNOSTICS': '1', 'SPARTAN_GOW3_PAIR_METADATA': '1',
        'SPARTAN_GOW3_SHADER_METADATA': '1', 'SPARTAN_GOW3_GROUP_METADATA': '1',
        'SPARTAN_GOW3_PROVENANCE': '1', 'SPARTAN_GOW3_FRAME_TRACE': '0',
        'SPARTAN_GOW3_PAIR_FILE': str(session / 'logs/group'),
        'SPARTAN_GOW3_PAIR_TRIGGER': str(session / 'capture'),
        'SPARTAN_CAPTURE_DIR': str(session / 'captures'),
        'SPARTAN_CAPTURE_TRIGGER': str(session / 'triggers/pass.trigger'),
        'SPARTAN_FRAME_CAPTURE_KEY': '0', 'SPARTAN_DIAG_IMAGES': '0',
        'SPARTAN_DUMP_SHADERS': '0', 'SPARTAN_HEALTH_PASSIVE': '0',
        'SPARTAN_KEY_TRACE': '0', 'TEMP': str(session / 'tmp'), 'TMP': str(session / 'tmp'),
        'SPARTAN_UI_KEY_BUFFER': '1',
        'PYTHONDONTWRITEBYTECODE': '1', 'PYTHONUNBUFFERED': '1', 'PYTHONIOENCODING': 'utf-8',
    }
    if pixels:
        (session / 'pixels').mkdir()
        variables.update(SPARTAN_GOW3_PIXELS='1', SPARTAN_GOW3_PIXEL_DIR=str(session / 'pixels'))
    manifest = dict(schema='gow3-supervised-session-v1', session=str(session),
                    prepared_utc=datetime.now(timezone.utc).isoformat(),
                    source_seed=str(seed), save_hashes=before, backup_verified=True,
                    source_probe=str(source_probe), probe=str(probe), probe_sha256=probe_hash,
                    environment=variables, cheats=dict(health=True),
                    skip_intro_requested=True, save_load_confirmed=False)
    (session / 'session.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    return session, manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--seed-data', type=Path, required=True)
    parser.add_argument('--game-dir', type=Path, required=True)
    parser.add_argument('--probe', type=Path, default=ROOT / 'out/verification/dot-build/spartanport-probe.exe')
    parser.add_argument('--launch', action='store_true')
    parser.add_argument('--pixels', action='store_true', help='Extract two real MSAA color/depth intervals with the first metadata trigger')
    args = parser.parse_args()
    session, manifest = prepare(args.seed_data, args.game_dir, args.probe, args.pixels)
    if args.launch:
        env = {k: v for k, v in os.environ.items() if not k.upper().startswith(('SPARTAN_', 'GOW3_'))}
        env.update(manifest['environment'])
        env['PATH'] = str(ROOT / 'out/toolchain/msys64/clang64/bin') + os.pathsep + env.get('PATH', '')
        with (session / 'logs/stdout.log').open('w', encoding='utf-8') as stdout, \
             (session / 'logs/stderr.log').open('w', encoding='utf-8') as stderr:
            child = subprocess.Popen([sys.executable, '-B', '-u', str(ROOT / 'run.py')],
                                     cwd=ROOT, env=env, stdin=subprocess.DEVNULL,
                                     stdout=stdout, stderr=stderr,
                                     creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
        manifest['runner_pid'] = child.pid
        manifest['launch_utc'] = datetime.now(timezone.utc).isoformat()
        (session / 'session.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    print(json.dumps(dict(session=str(session), runner_pid=manifest.get('runner_pid'),
                         backup_files=len(manifest['save_hashes']), backup_verified=True)))


if __name__ == '__main__':
    main()
