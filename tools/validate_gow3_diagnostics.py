"""Validate the experimental GoW3 diagnostics without starting the game.

Use a configured CMake/Ninja GPU build, then run this with --build-dir.
--gpu adds two standalone offscreen Vulkan fixtures. No probe is executed.
Results and synthetic fixtures remain under a new out/gow3-validation directory.
"""
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
CPU_TARGETS = [
    'gow3-camera-inputs-test', 'gow3-pair-capture-test', 'gow3-group-capture-test',
    'gow3-capture-bytes-test', 'gow3-reprojection-contract-test',
    'gow3-depth-samples-test', 'gow3-clip-pipeline-test', 'gow3-vertex-contract-test',
    'gow3-readback-primitives-test', 'gow3-resource-provenance-test',
    'gow3-provenance-budget-test', 'constant-ring-stamps-test',
    'gow3-frame-trace-test', 'gow3-camera-frames-test', 'gow3-snapshot-reasons-test',
    'gow3-trace-trigger-test', 'health-passive-bridge-test', 'health-inline-test',
    'health-lifecycle-test', 'health-collection-test', 'keyboard-trace-test',
    'keyboard-trace-bridge-test', 'motion-history-test', 'ui-composition-test',
    'upscaler-support-test', 'actor-hook-test',
]
DIRECTORY_ARGUMENT = {
    'gow3-pair-capture-test', 'gow3-group-capture-test', 'gow3-capture-bytes-test',
    'gow3-resource-provenance-test', 'gow3-provenance-budget-test',
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--toolchain', type=Path,
                        default=ROOT / 'out/toolchain/msys64/clang64')
    parser.add_argument('--gpu', action='store_true')
    args = parser.parse_args()
    if os.name != 'nt':
        parser.error('This validation runner uses the Windows/MSYS2 test fixtures.')
    build = args.build_dir.resolve()
    toolchain = args.toolchain.resolve()
    stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    output = ROOT / 'out/gow3-validation' / stamp
    output.mkdir(parents=True, exist_ok=False)
    # Each characterization starts independently of the user's runtime settings.
    env = {k: v for k, v in os.environ.items() if not k.upper().startswith('SPARTAN_')}
    env['PATH'] = str(toolchain / 'bin') + os.pathsep + env.get('PATH', '')
    env['PYTHONDONTWRITEBYTECODE'] = '1'
    results = []

    def run(name, command, cwd=ROOT, extra_env=None):
        log = output / (name + '.log')
        try:
            with log.open('w', encoding='utf-8') as stream:
                completed = subprocess.run([str(x) for x in command], cwd=cwd,
                                           env=env | (extra_env or {}),
                                           stdout=stream, stderr=subprocess.STDOUT,
                                           timeout=60, check=False)
            code = completed.returncode
        except (OSError, subprocess.TimeoutExpired) as error:
            with log.open('a', encoding='utf-8') as stream:
                stream.write(str(error) + '\n')
            code = -1
        status = 'passed' if code == 0 else 'unsupported' if code == 77 else 'failed'
        results.append(dict(name=name, exit_code=code, status=status, log=str(log),
                            command=[str(x) for x in command]))
        print(f'{name}: {status}', flush=True)
        return code == 0

    def fixture(name):
        directory = output / name
        directory.mkdir()
        return directory

    run('python-experiments', [sys.executable, '-B', '-m', 'unittest', 'discover',
                              '-s', 'experiments/gow3_automation', '-p', 'test_*.py', '-v'])
    run('python-menu', [sys.executable, '-B', '-m', 'unittest', 'discover',
                       '-s', 'tests', '-p', 'test_native_menu_script.py', '-v'])
    specialized = {'health-passive-bridge-test', 'health-inline-test',
                   'health-collection-test', 'keyboard-trace-bridge-test'}
    for target in CPU_TARGETS:
        if target in specialized:
            continue
        cwd = fixture(target)
        command = [build / (target + '.exe')]
        if target in DIRECTORY_ARGUMENT:
            command.append(cwd / 'data')  # The executable must create a new directory.
        elif target == 'keyboard-trace-test':
            command.append(cwd)  # This fixture instead requires an existing directory.
        run(target, command, cwd)
    run('health-inline-test', [build / 'health-inline-test.exe', fixture('health-inline')])
    for mode in ('off', 'zero', 'truthy', 'relative', 'unc', 'existing', 'on',
                 'wrong-owner', 'unavailable', 'small-buffer'):
        name = 'health-bridge-' + mode
        run(name, [build / 'health-passive-bridge-test.exe', mode, fixture(name)])
    for mode in ('disabled', 'identity-failure', 'normal'):
        name = 'health-collection-' + mode
        run(name, [build / 'health-collection-test.exe', mode, fixture(name)])
    for mode in ('off', 'truthy', 'existing', 'on', 'expiry'):
        name = 'keyboard-bridge-' + mode
        run(name, [build / 'keyboard-trace-bridge-test.exe', mode, fixture(name)])

    # These retained characterization files have no CMake targets in Dot's work.
    for stem in ('gow3_offline_sequence', 'passive_events',
                 'keyboard_sampling_contract', 'native_menu_replay', 'pad_automation_contract'):
        c = stem in ('keyboard_sampling_contract', 'native_menu_replay', 'pad_automation_contract')
        source = ROOT / ('experiments/gow3_automation/test_passive_events.cpp'
                         if stem == 'passive_events' else f'tests/test_{stem}.{"c" if c else "cpp"}')
        exe = output / (stem + '.exe')
        command = [toolchain / ('bin/clang.exe' if c else 'bin/clang++.exe'),
                   '-std=c11' if c else '-std=c++23', '-O1', '-g', '-UNDEBUG',
                   '-Wall', '-Wextra', '-Werror', '-ffp-contract=off', '-pthread',
                   '-I' + str(ROOT), '-I' + str(ROOT / 'src'), source]
        if c:
            command += [ROOT / 'src/compat_win.c', '-lSDL3', '-lbcrypt']
        command += ['-o', exe]
        if run(stem + '-compile', command):
            directory = fixture(stem)
            run(stem, [exe, directory] if stem.endswith(('replay', 'contract'))
                and stem != 'keyboard_sampling_contract' else [exe], directory)
            if stem == 'keyboard_sampling_contract':
                # Compile the same sampler with its real trace bridge enabled.
                bridge_exe = output / 'keyboard-sampling-bridge.exe'
                obj = output / 'keyboard-sampling.obj'
                compile_c = command[:command.index('-o')] + ['-DSPARTAN_KEY_TRACE_RUNTIME', '-c']
                compile_c.remove(ROOT / 'src/compat_win.c')
                compile_c.remove('-lSDL3')
                compile_c.remove('-lbcrypt')
                if run('keyboard-sampling-bridge-compile', compile_c + ['-o', obj]):
                    compat = output / 'compat-win.obj'
                    compat_command = [toolchain / 'bin/clang.exe', '-std=c11', '-O1',
                                      '-c', ROOT / 'src/compat_win.c', '-o', compat]
                    link = [toolchain / 'bin/clang++.exe', '-std=c++23', '-O1', '-pthread',
                            obj, ROOT / 'gpu/shim/spartanport_keyboard_trace.cpp',
                            compat, '-lSDL3', '-lbcrypt', '-o', bridge_exe]
                    if run('keyboard-sampling-compat-compile', compat_command) and run('keyboard-sampling-bridge-link', link):
                        run('keyboard-sampling-bridge-off', [bridge_exe], fixture('sampler-off'))
                        active = fixture('sampler-on')
                        trigger = active / 'start.trigger'
                        trigger.write_text('start', encoding='utf-8')
                        run('keyboard-sampling-bridge-on', [bridge_exe], active, {
                            'SPARTAN_KEY_TRACE': '1',
                            'SPARTAN_KEY_TRACE_FILE': str(active / 'keyboard.jsonl'),
                            'SPARTAN_KEY_TRACE_TRIGGER': str(trigger),
                        })
    if args.gpu:
        for target, shaders in (('gow3-vulkan-depth-test', 'gow3-fixture-shaders'),
                                ('gow3-vulkan-readback-test', 'gow3-readback-shaders')):
            run(target, [build / (target + '.exe'), build / shaders], fixture(target))
    summary = dict(completed_utc=datetime.now(timezone.utc).isoformat(),
                   game_launched=False, gpu_requested=args.gpu,
                   all_passed=all(r['status'] == 'passed' for r in results), results=results)
    (output / 'result.json').write_text(json.dumps(summary, indent=2), encoding='utf-8')
    print('Results: ' + str(output), flush=True)
    return 0 if summary['all_passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
