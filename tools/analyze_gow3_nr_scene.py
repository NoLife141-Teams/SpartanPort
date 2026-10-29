"""Validate completed native NR scene captures and independently match real pixels.

Correspondences use input color only, never the predicted vectors. Object/camera
cohorts are assigned afterwards. One or two groups of four consecutive GPU captures are required.
"""
import argparse
import hashlib
import json
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw
from validate_gow3_pixel_reprojection import match, HALF, SIDE, RADIUS


def statistics(values):
    values = np.asarray(values)
    return dict(points=int(values.size), median_px=float(np.median(values)) if values.size else None,
                p90_px=float(np.quantile(values, .9)) if values.size else None)


def load(directory, index):
    meta = json.loads((directory/f'{index}.json').read_text())
    if not meta['complete'] or not meta['gpu_complete'] or meta['hud_included'] or not meta['motion_depth']:
        raise ValueError('Incomplete capture or wrong NR stage')
    if (meta['width'], meta['height']) != (1920, 1080) or meta['jitter'] != [0, 0]:
        raise ValueError('Expected native unjittered 1080p')
    arrays, hashes = {}, {}
    for name, dtype, channels in [('input.rgba32f', '<f4', 4), ('output.rgba32f', '<f4', 4),
                                   ('depth.r32f', '<f4', 1), ('motion.rg16f', '<f2', 2),
                                   ('guides.rgba32f', '<f4', 4)]:
        path = directory/f'{index}.{name}'
        expected = 1920*1080*channels*np.dtype(dtype).itemsize
        if path.stat().st_size != expected:
            raise ValueError('Unexpected capture size: '+str(path))
        array = np.memmap(path, mode='r', dtype=dtype, shape=(1080, 1920, channels))
        if not np.isfinite(array).all():
            raise ValueError('Nonfinite GPU readback: '+str(path))
        arrays[name.split('.')[0]] = array
        hashes[name] = hashlib.sha256(path.read_bytes()).hexdigest()
    depth, flags = arrays['depth'][:, :, 0], arrays['guides'][:, :, 0]
    if not ((depth >= 0).all() and (depth <= 1).all() and np.isin(flags, [0, 1, 2]).all()):
        raise ValueError('Invalid hardware depth or object validity encoding')
    valid = flags >= 1
    if valid.any() and np.max(np.abs(arrays['guides'][:, :, 1][valid]-depth[valid])) > 2e-7:
        raise ValueError('Object vectors do not belong to the sampled depth surface')
    return meta, arrays, hashes


def analyze(directory):
    count=8 if (directory/'4.json').exists() else 4
    frames = [load(directory, index) for index in range(count)]
    report = dict(schema='gow3-nr-real-guides-v1', selection_uses_predicted_flow=False, frames=[], pairs=[])
    report['limitations'] = ('Geometry-history validity includes Gaia/world and characters; it is not an object-ID mask. '
        'Affine association is conservative pose matching for repeated meshes, not an engine object ID. '
        'Camera fallback cannot explain untracked deformation, foliage or transparency. Short sequences do not certify all scenes.')
    for index, (meta, data, hashes) in enumerate(frames):
        z = data['depth'][:, :, 0]
        valid = data['guides'][:, :, 0] >= 1
        flow = data['motion'].astype(np.float32)
        difference = np.abs(data['output'][:, :, :3]-data['input'][:, :, :3])
        # CPU matrix calculation, independent of the shader's float32 multiply.
        yy, xx = np.mgrid[32:1048:32, 32:1888:32]
        depth = z[yy, xx]
        current = np.asarray(meta['camera_vp']).reshape(4, 4, order='F')
        previous = np.asarray(meta['previous_vp']).reshape(4, 4, order='F')
        clip = np.stack((2*(xx+.5)/1920-1, 1-2*(yy+.5)/1080, 2*depth-1, np.ones_like(depth)), -1)
        projected = clip @ (previous @ np.linalg.inv(current)).T
        mask = (depth < 1) & (projected[:, :, 3] > 1e-5)
        ndc = projected[:, :, :2]/projected[:, :, 3:4]
        cpu = np.stack(((ndc[:, :, 0]+1)*960-xx-.5, (1-ndc[:, :, 1])*540-yy-.5), -1)
        diagnostic = data['guides'][yy, xx, 2:4]
        camera_error = np.linalg.norm(cpu-diagnostic, axis=-1)[mask]
        # RG16F stores may round towards zero rather than NumPy's nearest-even.
        # Accept either adjacent representable result, never an arbitrary epsilon.
        fallback = ~valid
        raw = data['guides'][:, :, 2:4]
        nearest = raw.astype(np.float16)
        towards_zero = np.where(np.abs(nearest.astype(np.float32)) > np.abs(raw),
            np.nextafter(nearest, np.float16(0)), nearest)
        wrong = (data['motion'] != nearest) & (data['motion'] != towards_zero)
        fallback_mismatches = int(np.any(wrong[fallback], axis=-1).sum())
        if fallback_mismatches:
            raise ValueError('Camera fallback and submitted NR motion differ')
        report['frames'].append(dict(index=index, scene=meta['scene'], tick=meta['tick'], reset=meta['reset'],
            scene_samples=meta.get('scene_samples',1),
            sha256=hashes, object_pixels=int(valid.sum()), surface_pixels=int((z < 1).sum()),
            affine_instance_pixels=int((data['guides'][:, :, 0]==2).sum()),
            depth_quantiles=np.quantile(z, [0, .01, .5, .99, 1]).tolist(),
            motion_max_abs=float(np.abs(flow).max()), camera_cpu_gpu=statistics(camera_error),
            camera_cpu_gpu_max_px=float(camera_error.max()) if camera_error.size else None,
            fallback_mismatches=fallback_mismatches,
            fallback_nearest_differences=int(np.any(data['motion'][fallback] != nearest[fallback], axis=-1).sum()),
            mean_abs_rgb_change=float(difference.mean())))
        if index%4 == 0:
            continue
        prev_meta, prev_data, _ = frames[index-1]
        if meta['scene'] != prev_meta['scene']+1 or meta['depth_uid'] != prev_meta['depth_uid'] or meta['previous_vp'] != prev_meta['camera_vp']:
            raise ValueError('Guides/camera are not consecutive scenes on the same depth resource')
        if meta['reset']:
            raise ValueError('Temporal history reset inside the captured sequence')
        pg = prev_data['input'][:, :, :3].mean(-1)
        cg = data['input'][:, :, :3].mean(-1)
        pw, cw = [np.lib.stride_tricks.sliding_window_view(gray, (SIDE, SIDE)) for gray in (pg, cg)]
        matches = []
        margin = HALF+RADIUS+4
        for y in range(48, 1080-48, 40):
            for x in range(48, 1920-48, 48):
                patch = cg[y-HALF:y+HALF+1, x-HALF:x+HALF+1]
                if patch.std() < .012 or not 0 < z[y, x] < .99999:
                    continue
                found = match(pw, patch, x, y)
                if found is None:
                    continue
                measured, score = found
                px, py = round(x+measured[0]), round(y+measured[1])
                if not (margin < px < 1920-margin and margin < py < 1080-margin):
                    continue
                back = match(cw, pg[py-HALF:py+HALF+1, px-HALF:px+HALF+1], px, py)
                if back is None or np.hypot(px+back[0][0]-x, py+back[0][1]-y) > 1:
                    continue
                predicted, camera = flow[y, x], data['guides'][y, x, 2:4]
                matches.append(dict(x=x, y=y, object=bool(valid[y, x]),
                    affine_instance=bool(data['guides'][y,x,0]==2),depth=float(z[y, x]),
                    measured_flow=list(measured), predicted_flow=predicted.tolist(),
                    error_px=float(np.linalg.norm(predicted-measured)),
                    reversed_error_px=float(np.linalg.norm(-predicted-measured)),
                    zero_motion_error_px=float(np.linalg.norm(measured)),
                    camera_only_error_px=float(np.linalg.norm(camera-measured)), match_error=score))
        if not matches:
            raise ValueError('No independent bidirectional pixel matches')
        cohorts = {}
        for name, selected in [('all', matches), ('geometry_history', [m for m in matches if m['object']]),
                               ('affine_instances', [m for m in matches if m['affine_instance']]),
                               ('camera_fallback', [m for m in matches if not m['object']]),
                               ('near_depth_below_0.99', [m for m in matches if m['depth'] < .99])]:
            cohorts[name] = dict(submitted=statistics([m['error_px'] for m in selected]),
                                 camera_only=statistics([m['camera_only_error_px'] for m in selected]),
                                 reversed=statistics([m['reversed_error_px'] for m in selected]),
                                 zero_motion=statistics([m['zero_motion_error_px'] for m in selected]))
        report['pairs'].append(dict(previous_scene=prev_meta['scene'], current_scene=meta['scene'],
                                    cohorts=cohorts, matches=matches))
    first = frames[0][1]
    for name in ('input', 'output'):
        Image.fromarray(np.rint(np.clip(first[name][:, :, :3], 0, 1)*255).astype('uint8')).save(directory/f'{name}.png')
    z = first['depth'][:, :, 0]
    lo, hi = np.quantile(z[z < 1], [.01, .99])
    depth = Image.fromarray(np.rint(np.clip((z-lo)/max(hi-lo, 1e-10), 0, 1)*255).astype('uint8')).convert('RGB')
    flow = first['motion'].astype(np.float32)
    hsv = np.stack(((np.arctan2(flow[:, :, 1], flow[:, :, 0])/(2*np.pi)+1)%1,
                    np.ones_like(z), np.minimum(np.linalg.norm(flow, axis=-1)/2, 1)), -1)
    motion = Image.fromarray(np.rint(hsv*255).astype('uint8'), mode='HSV').convert('RGB')
    panels = [(Image.open(directory/'input.png'), 'OFF: actual scene, before HUD'),
              (Image.open(directory/'output.png'), 'DLSS 5: same scene, depth + motion, history on'),
              (depth, 'Actual hardware depth (percentile visualization)'),
              (motion, 'Actual NR motion: previous - current; brightness saturates at 2 px')]
    canvas = Image.new('RGB', (1600, 960), (20, 20, 20))
    draw = ImageDraw.Draw(canvas)
    for index, (picture, label) in enumerate(panels):
        x, y = index%2*800, index//2*480
        canvas.paste(picture.resize((800, 450)), (x, y+30))
        draw.text((x+10, y+8), label, (255, 255, 255))
    canvas.save(directory/'diagnostic.png')
    with (directory/'analysis.json').open('w') as stream:
        json.dump(report, stream, indent=2, allow_nan=False)
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    result = analyze(parser.parse_args().directory)
    print(json.dumps({'frames': result['frames'], 'cohorts': [p['cohorts'] for p in result['pairs']]}, indent=2))
