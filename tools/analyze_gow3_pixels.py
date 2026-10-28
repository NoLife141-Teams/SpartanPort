"""Validate real two-sample GPU captures against their exact grouped draw/camera interval.

Requires NumPy and Pillow. Previews apply sRGB encoding to sample 0 only;
raw arrays remain unmodified. No claims about pre-HUD, motion or DLAA quality.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import numpy as np
from PIL import Image


def analyze(session):
    directory = session / 'pixels'
    journal = [json.loads(line) for line in (directory / 'pixels.jsonl').read_text().splitlines()]
    if not journal or journal[0].get('schema') != 'gow3-real-pixels-v1':
        raise ValueError('Missing real-pixel schema marker')
    results = [r for r in journal if r['kind'] == 'result']
    if len(results) != 2:
        raise ValueError('Expected exactly two closed pixel results')
    if results[1]['interval'] != results[0]['interval'] + 1 or sum(r['bytes'] for r in results) > 192*1024*1024:
        raise ValueError('Interval sequence or file budget invalid')
    rows = []
    for path in sorted((session / 'logs').glob('group.*.jsonl')):
        rows.extend(json.loads(line) for line in path.read_text().splitlines())
    frames = []
    for result in results:
        errors = []
        interval = result['interval']
        if result['file'] != f'interval-{interval}.rgba-depth.f32' or not result['tick']:
            raise ValueError('Pixel filename or submission tick invalid')
        draws = [r for r in rows if r['kind'] == 'draw' and r['interval'] == interval]
        selected = [r for r in draws if r['draw'] == result['draw']]
        closes = [r for r in rows if r['kind'] == 'interval' and r['interval'] == interval]
        if not result['complete'] or not result['submitted'] or not result['gpu_complete']:
            errors.append('runtime_capture_incomplete:' + result['error'])
        if len(selected) != 1 or len(closes) != 1:
            errors.append('draw_or_interval_link_missing')
        else:
            draw, close = selected[0], closes[0]
            if (draw['vs'], draw['ps'], draw['vs_permutation_key'], draw['ps_permutation_key']) != (
                    '475a41468b1a1fbc', '475a4146e3a74097', 'efd04d8023fec408', 'efd0530963dc1d55'):
                errors.append('composition_permutation_mismatch')
            if not draw['recorded'] or draw['indirect'] or draw['proxy_path']:
                errors.append('composition_not_native_direct_recorded')
            if int(close['closing_flip']['id'], 16) != result['closing_flip'] or close['camera_frame'] != result['camera_frame']:
                errors.append('closure_token_mismatch')
            for binding, label in ((1, 'depth'), (2, 'color')):
                ds = [d for d in draw['image_descriptors'] if d['binding'] == binding]
                saved = result[label]
                if len(ds) != 1 or not ds[0]['provenance_observed']:
                    errors.append(label + '_descriptor_missing')
                    continue
                observed = ds[0]['resource']
                keys = {'uid': 'uid', 'backing': 'backing_ordinal', 'view_ordinal': 'view_ordinal',
                        'image': 'image', 'view': 'view', 'format': 'format', 'samples': 'samples'}
                if observed['resolution'] != 2 or not observed['current_backing'] or any(saved[k] != observed[v] for k, v in keys.items()):
                    errors.append(label + '_source_identity_mismatch')
            cameras = [r for r in draws if r['camera_observed']]
            variants = {tuple(r['words_hex'][:16]) for r in cameras if r['snapshot_present']}
            if not cameras or len(variants) != 1 or any(r['snapshot_missing'] or r['snapshot_conflict'] for r in cameras):
                errors.append('camera_snapshots_incomplete')
            else:
                matrix = [struct.unpack('<f', int(v, 16).to_bytes(4, 'little'))[0] for v in next(iter(variants))]
                if matrix != result['camera_vp']:
                    errors.append('camera_matrix_mismatch')
            if any(int(r['camera_source_depth']['uid'], 16) != result['depth']['uid'] or
                   int(r['camera_source_depth']['guest_address_label'], 16) != result['depth']['address'] for r in cameras):
                errors.append('camera_depth_source_mismatch')
        frame = dict(runtime=result, errors=errors)
        path = directory / result['file']
        if result['complete'] and path.is_file():
            w, h = result['width'], result['height']
            expected = w * h * 2 * 5 * 4
            if not (0 < w <= 1920 and 0 < h <= 1080 and path.stat().st_size == expected == result['bytes']):
                raise ValueError('Pixel dimensions/size mismatch')
            pixels = np.memmap(path, dtype='<f4', mode='r', shape=(h, w, 2, 5))
            color, depth = pixels[..., :4], pixels[..., 4]
            finite = bool(np.isfinite(pixels).all())
            if not finite:
                raise ValueError('Nonfinite raw pixel values')
            if not ((depth >= 0).all() and (depth <= 1).all()):
                errors.append('depth_out_of_range')
            sha = hashlib.sha256()
            with path.open('rb') as stream:
                for block in iter(lambda: stream.read(1024 * 1024), b''):
                    sha.update(block)
            frame.update(sha256=sha.hexdigest(), bytes=expected, all_finite=finite,
                         depth_min=float(depth.min()), depth_max=float(depth.max()),
                         depth_quantiles=np.quantile(depth, [0, .01, .5, .99, 1]).tolist(),
                         depth_distinct_sample_pixels=int(np.count_nonzero(depth[:, :, 0] != depth[:, :, 1])),
                         color_distinct_sample_pixels=int(np.count_nonzero(np.any(color[:, :, 0] != color[:, :, 1], axis=-1))),
                         rgba_min=color.min(axis=(0, 1, 2)).tolist(), rgba_max=color.max(axis=(0, 1, 2)).tolist())
            rgb = np.clip(color[:, :, 0, :3], 0, 1)
            encoded = np.where(rgb <= .0031308, 12.92 * rgb, 1.055 * rgb ** (1 / 2.4) - .055)
            image = Image.fromarray(np.rint(encoded * 255).astype('uint8'))
            image.thumbnail((960, 540))
            with (directory / f'interval-{interval}-color-sample0.png').open('xb') as stream:
                image.save(stream, format='PNG')
            d = depth[:, :, 0]
            lo, hi = np.quantile(d, [.01, .99])
            visible = np.clip((d - lo) / max(float(hi - lo), 1e-12), 0, 1)
            image = Image.fromarray(np.rint(visible * 255).astype('uint8'))
            image.thumbnail((960, 540))
            with (directory / f'interval-{interval}-depth-sample0.png').open('xb') as stream:
                image.save(stream, format='PNG')
        else:
            errors.append('pixel_file_missing')
        frame['valid'] = not errors
        frames.append(frame)
    report = dict(schema='gow3-real-pixel-analysis-v1', frames=frames,
                  valid=all(f['valid'] for f in frames), pre_hud_proven=False,
                  depth_projection_validated=False, dlaa_validated=False,
                  previews='Sample 0, GPU image row order. Color sRGB-encoded for display; depth 1%-99% contrast stretch, not linear distance.')
    with (session / 'pixel-summary.json').open('x', encoding='utf-8') as stream:
        json.dump(report, stream, indent=2, allow_nan=False)
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('session', type=Path)
    report = analyze(parser.parse_args().session)
    for f in report['frames']:
        print('interval', f['runtime']['interval'], 'valid', f['valid'], 'errors', f['errors'])
    raise SystemExit(0 if report['valid'] else 1)
