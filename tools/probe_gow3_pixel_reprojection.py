"""Compare camera reprojection hypotheses on captured pixels; exploratory, not motion truth.

Uses approximate pixel centers (sample positions were not measured), nearest
neighbor matching, and a fixed smooth-depth mask. Moving objects remain present.
"""
import argparse
import json
from pathlib import Path
import numpy as np


def probe(session):
    report = json.loads((session / 'pixel-summary.json').read_text())
    if not report['valid']:
        raise ValueError('Both captures must have valid pixel/camera links')
    a, b = [f['runtime'] for f in report['frames']]
    if b['interval'] != a['interval'] + 1 or (a['width'], a['height']) != (b['width'], b['height']):
        raise ValueError('Expected consecutive equally sized intervals')
    w, h = a['width'], a['height']
    arrays = [np.memmap(session / 'pixels' / r['file'], dtype='<f4', mode='r', shape=(h, w, 2, 5)) for r in (a, b)]
    previous, current = arrays
    # Same deterministic grid and mask for every hypothesis. Avoid sky, sample
    # edges and large depth discontinuities; this cannot exclude moving objects.
    y, x = np.mgrid[2:h-2:4, 2:w-2:4]
    depth = current[y, x, 0, 4].astype('float64')
    mask = (depth > 0) & (depth < .9999) & (np.abs(current[y, x, 1, 4] - depth) < 1e-5)
    for dy, dx in ((0, 1), (0, -1), (1, 0), (-1, 0)):
        mask &= np.abs(current[y+dy, x+dx, 0, 4] - depth) < 1e-4
    y, x, depth = y[mask], x[mask], depth[mask]
    current_color = current[y, x, 0, :3]
    matrices = [np.asarray(r['camera_vp']).reshape((4, 4), order='F') for r in (a, b)]
    cases = []
    for name, minus_w, y_sign, transpose, identity in (
            ('MinusW_negative_viewport', True, -1, False, False),
            ('ZeroToW_negative_viewport', False, -1, False, False),
            ('MinusW_wrong_y_control', True, 1, False, False),
            ('MinusW_transposed_control', True, -1, True, False),
            ('identity_no_camera_control', True, -1, False, True)):
        pm, cm = matrices
        if transpose:
            pm, cm = pm.T, cm.T
        transform = np.eye(4) if identity else pm @ np.linalg.inv(cm)
        ndc = np.stack((2*(x+.5)/w-1, y_sign*(2*(y+.5)/h-1), depth*2-1 if minus_w else depth, np.ones_like(depth)))
        q = transform @ ndc
        front = q[3] > 1e-8
        q = q[:3] / np.where(np.abs(q[3]) > 1e-12, q[3], np.nan)
        px = (q[0]+1)*w/2-.5
        py = (y_sign*q[1]+1)*h/2-.5
        pz = (q[2]+1)/2 if minus_w else q[2]
        valid = front & np.isfinite(q).all(axis=0) & (px >= 0) & (px < w-1) & (py >= 0) & (py < h-1) & (pz >= 0) & (pz <= 1)
        ix, iy = np.rint(px[valid]).astype(int), np.rint(py[valid]).astype(int)
        previous_depth = previous[iy, ix, :, 4]
        depth_error = np.min(np.abs(previous_depth - pz[valid, None]), axis=1)
        color_error = np.mean(np.abs(previous[iy, ix, 0, :3] - current_color[valid]), axis=1)
        motion = np.hypot(px[valid]-x[valid], py[valid]-y[valid])
        quantiles = lambda values: np.quantile(values, [.5, .9, .99]).tolist() if len(values) else [None]*3
        cases.append(dict(hypothesis=name, input_points=len(x), valid_points=int(valid.sum()),
                          depth_error_quantiles=quantiles(depth_error),
                          linear_rgb_error_quantiles=quantiles(color_error),
                          motion_pixels_quantiles=quantiles(motion)))
    result = dict(schema='gow3-pixel-reprojection-probe-v1', intervals=[a['interval'], b['interval']],
                  cases=cases, projection_validated=False, motion_ground_truth=False,
                  limitations='Approximate pixel centers, no measured MSAA positions; nearest neighbor; smooth-depth subset; moving objects, uncovered producers and non-exhaustive provenance remain.')
    with (session / 'reprojection-probe.json').open('x') as stream:
        json.dump(result, stream, indent=2, allow_nan=False)
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('session', type=Path)
    result = probe(parser.parse_args().session)
    for case in result['cases']:
        print(case['hypothesis'], 'points', case['valid_points'], 'median_depth_error', case['depth_error_quantiles'][0],
              'median_rgb_error', case['linear_rgb_error_quantiles'][0], 'median_motion_px', case['motion_pixels_quantiles'][0])
