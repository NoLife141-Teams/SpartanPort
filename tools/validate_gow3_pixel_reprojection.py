"""Independent image matches and explicit camera contract; no DLAA dispatch.

Matching never sees predicted flow. Depth cohorts separate candidate distant
background from the moving foreground. Neither cohort is a producer-ID mask.
"""
import argparse
from dataclasses import replace
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import numpy as np
try:
    from .gow3_reprojection import Viewport, reproject
except ImportError:
    from gow3_reprojection import Viewport, reproject

ROOT = Path(__file__).resolve().parents[1]
HALF, RADIUS, SIDE = 7, 20, 15


def match(windows, patch, x, y):
    search = windows[y-HALF-RADIUS:y-HALF+RADIUS+1, x-HALF-RADIUS:x-HALF+RADIUS+1]
    centered = search-search.mean(axis=(-1, -2), keepdims=True)
    target = patch-patch.mean()
    den = np.sqrt(np.sum(centered**2, axis=(-1, -2))*np.sum(target**2))
    errors = 1-np.sum(centered*target, axis=(-1, -2))/np.maximum(den, 1e-12)
    iy, ix = np.unravel_index(errors.argmin(), errors.shape)
    score = float(errors[iy, ix])
    other = errors.copy()
    other[max(0, iy-2):iy+3, max(0, ix-2):ix+3] = np.inf
    if ix in (0, 2*RADIUS) or iy in (0, 2*RADIUS) or score > .15 or score > float(other.min())*.8:
        return None
    def sub(a, b, c):
        return float(np.clip(.5*(a-c)/max(a-2*b+c, 1e-10), -.75, .75))
    return ((ix-RADIUS+sub(errors[iy, ix-1], errors[iy, ix], errors[iy, ix+1]),
             iy-RADIUS+sub(errors[iy-1, ix], errors[iy, ix], errors[iy+1, ix])), score)


def camera_view(rows, interval):
    cameras = [r for r in rows if r['kind'] == 'draw' and r['interval'] == interval and r['camera_observed']]
    states = set()
    for r in cameras:
        s = r['shader_evidence']
        if r['clip_disabled'] or len(r['viewports']) != 1 or not s['dynamic_state_recorded'] or s['bias_enabled'] or s['bounds_enabled']:
            raise ValueError('Unsupported camera viewport/depth state')
        states.add((r['clip_space'], *r['viewports'][0]['float_bits']))
    if len(states) != 1:
        raise ValueError('Missing/conflicting camera viewport')
    clip, *words = next(iter(states))
    if clip not in (0, 1):
        raise ValueError('Unknown clip-space enum')
    values = [struct.unpack('<f', int(word, 16).to_bytes(4, 'little'))[0] for word in words]
    return Viewport(*values, minus_w=clip == 0)


def file_hash(path):
    sha = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024*1024), b''):
            sha.update(block)
    return sha.hexdigest()


def probe(session, cpp_helper):
    report = json.loads((session/'pixel-summary.json').read_text())
    if not report['valid']:
        raise ValueError('Both captures must have valid pixel/camera links')
    a, b = [f['runtime'] for f in report['frames']]
    if b['interval'] != a['interval']+1 or (a['width'], a['height']) != (b['width'], b['height']):
        raise ValueError('Expected consecutive equally sized intervals')
    w, h = a['width'], a['height']
    arrays = []
    for frame in report['frames']:
        r = frame['runtime']
        path = session/'pixels'/r['file']
        if path.stat().st_size != w*h*40 or file_hash(path) != frame['sha256']:
            raise ValueError('Raw capture size/hash changed')
        arrays.append(np.memmap(path, dtype='<f4', mode='r', shape=(h, w, 2, 5)))
    previous, current = arrays
    rows = [json.loads(line) for path in sorted((session/'logs').glob('group.*.jsonl')) for line in path.read_text().splitlines()]
    pv, cv = [camera_view(rows, r['interval']) for r in (a, b)]
    pm, cm = [np.asarray(r['camera_vp']).reshape((4, 4), order='F') for r in (a, b)]
    pg, cg = [np.mean(image[:, :, 0, :3], axis=-1) for image in arrays]
    pw, cw = [np.lib.stride_tricks.sliding_window_view(gray, (SIDE, SIDE)) for gray in (pg, cg)]
    matches = []
    for y in range(48, h-48, 48):
        for x in range(48, w-48, 64):
            patch = cg[y-HALF:y+HALF+1, x-HALF:x+HALF+1]
            z = float(current[y, x, 0, 4])
            if patch.std() < .003 or not 0 < z < .9999:
                continue
            if max(abs(float(current[y+dy, x+dx, 0, 4])-z) for dy, dx in ((0, 1), (0, -1), (1, 0), (-1, 0))) > 1e-4:
                continue
            found = match(pw, patch, x, y)
            if found is None:
                continue
            (dx, dy), score = found
            px, py = round(x+dx), round(y+dy)
            back = match(cw, pg[py-HALF:py+HALF+1, px-HALF:px+HALF+1], px, py)
            if back is None or np.hypot(px+back[0][0]-x, py+back[0][1]-y) > 1:
                continue
            matches.append(dict(x=x, y=y, depth=z, measured_flow=[dx, dy], match_error=score))
    if not matches:
        raise ValueError('No independent bidirectional image correspondences')
    x, y, z = [np.array([m[k] for m in matches]) for k in ('x', 'y', 'depth')]
    measured = np.array([m['measured_flow'] for m in matches])
    cases = {}
    for name in ('captured_contract', 'wrong_ZeroToW', 'wrong_positive_height', 'reversed_time', 'no_camera'):
        v0, v1 = pv, cv
        if name == 'wrong_ZeroToW':
            v0, v1 = replace(pv, minus_w=False), replace(cv, minus_w=False)
        if name == 'wrong_positive_height':
            v0, v1 = replace(pv, y=0, height=abs(pv.height)), replace(cv, y=0, height=abs(cv.height))
        m0, m1 = (cm, pm) if name == 'reversed_time' else (pm, cm)
        if name == 'no_camera':
            m0 = m1 = cm
        px, py, pz, valid = reproject(m1, m0, v1, v0, x+.5, y+.5, z)
        flow = np.column_stack((px-x-.5, py-y-.5))
        if name == 'captured_contract':
            cpp_reference = (px, py, pz, valid)
        cases[name] = dict(flow=flow, errors=np.linalg.norm(flow-measured, axis=1), valid=valid)
    numeric = [' '.join(map(str, v.values()+[int(v.minus_w)])) for v in (pv, cv)]
    numeric += [' '.join(format(v, '.17g') for v in m.ravel(order='F')) for m in (pm, cm)]
    numeric += [f'{xx+.5} {yy+.5} {zz:.17g}' for xx, yy, zz in zip(x, y, z)]
    env = dict(os.environ)
    env['PATH'] = str(ROOT/'out/toolchain/msys64/clang64/bin')+os.pathsep+env.get('PATH', '')
    cpp = subprocess.run([str(cpp_helper.resolve())], input='\n'.join(numeric)+'\n', text=True, capture_output=True, check=True, env=env, timeout=30)
    points = np.array([[float(v) for v in line.split()] for line in cpp.stdout.splitlines()])
    px, py, pz, valid = cpp_reference
    if points.shape != (len(matches), 4) or not np.array_equal(points[:, 0].astype(bool), valid):
        raise ValueError('C++/Python validity mismatch')
    delta = np.abs(points[valid, 1:]-np.column_stack((px, py, pz))[valid])
    max_delta = delta.max(axis=0).tolist() if len(delta) else None
    if not max_delta or max(max_delta) > 1e-8:
        raise ValueError('C++/Python reprojection mismatch')
    cohorts = []
    for label, mask in (('all', np.ones(len(z), dtype=bool)), ('near_below_0.99', z < .99), ('far_0.995_to_0.9999', (z >= .995) & (z < .9999))):
        statistics = {}
        for name, case in cases.items():
            values = case['errors'][mask & case['valid']]
            statistics[name] = dict(points=len(values), median_px=float(np.median(values)) if len(values) else None,
                                    p90_px=float(np.quantile(values, .9)) if len(values) else None)
        cohorts.append(dict(cohort=label, candidates=int(mask.sum()), hypotheses=statistics))
    for i, item in enumerate(matches):
        item['predictions'] = {name: dict(flow=c['flow'][i].tolist(), error_px=float(c['errors'][i]), valid=bool(c['valid'][i])) for name, c in cases.items()}
    result = dict(schema='gow3-pixel-reprojection-correspondences-v2', intervals=[a['interval'], b['interval']],
                  raw_hashes=[f['sha256'] for f in report['frames']], matches=matches, cohorts=cohorts,
                  cpp_python_max_delta=max_delta, viewport_current=cv.values(), raw_clip='MinusWToW' if cv.minus_w else 'ZeroToW',
                  selection_uses_predicted_flow=False, runtime_dlaa_enabled=False, full_scene_motion_validated=False,
                  limitations='Far depth is a candidate static-world subset, not a producer/object-ID mask. Approximate centers, unknown MSAA positions, moving objects and incomplete provenance. Thresholds investigated on the first pair; independent capture required.')
    with (session/'reprojection-correspondences.json').open('x', encoding='utf-8') as stream:
        json.dump(result, stream, indent=2, allow_nan=False)
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('session', type=Path)
    parser.add_argument('--cpp-helper', type=Path, default=ROOT/'out/verification/dot-build/gow3-reproject-points.exe')
    result = probe(**vars(parser.parse_args()))
    for cohort in result['cohorts']:
        print(cohort['cohort'], json.dumps(cohort['hypotheses']))
    print('C++/Python maximum delta', result['cpp_python_max_delta'])
