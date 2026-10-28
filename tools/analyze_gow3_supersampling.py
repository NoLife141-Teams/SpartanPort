"""Compare actual 1080p swapchain pixels for native, 4K OFF and 4K DLAA.

Readbacks are immutable. Output uses a new directory. Different sessions are
not synchronized gameplay; animation is an inspection aid, not a quality score.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re

import numpy as np
from PIL import Image, ImageDraw, ImageFont


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def load_display(directory, index, expected_dlaa):
    meta = json.loads((directory / f'{index}.json').read_text())
    if not meta['complete'] or not meta['gpu_complete'] or meta['index'] != index:
        raise ValueError('Incomplete display capture')
    if (meta['width'], meta['height'], meta['presentation_ordinal']) != (1920, 1080, index * 4 + 1):
        raise ValueError('Unexpected display size or unique frame cadence')
    if meta['dlaa_state_at_record'] != expected_dlaa:
        raise ValueError('DLAA state did not match the selected session')
    formats = {'B8G8R8A8Unorm': 'BGRA', 'B8G8R8A8Srgb': 'BGRA',
               'R8G8B8A8Unorm': 'RGBA', 'R8G8B8A8Srgb': 'RGBA'}
    if meta['format'] not in formats:
        raise ValueError('Unsupported SDR display format')
    path = directory / f'{index}.raw'
    raw = path.read_bytes()
    if len(raw) != 1920 * 1080 * 4:
        raise ValueError('Truncated display readback')
    # These are actual SDR swapchain bytes, already encoded for display; no
    # extra gamma transform, tone map, sharpening or offline downsampling.
    image = Image.frombytes('RGBA', (1920, 1080), raw, 'raw', formats[meta['format']]).convert('RGB')
    meta['sha256'] = hashlib.sha256(raw).hexdigest()
    return meta, image


def scene_proof(session, size, dlaa):
    directory = session / 'dlss-capture'
    proof = json.loads((directory / 'compare.json').read_text())
    if not proof['complete'] or not proof['gpu_complete'] or proof['samples'] != 1 or proof['dlaa'] != dlaa:
        raise ValueError('Scene source contract mismatch')
    if (proof['width'], proof['height']) != size or proof['sharpness'] != 0:
        raise ValueError('Unexpected scene resolution or sharpness')
    if (dlaa and proof['evaluated'] < 32) or (not dlaa and proof['evaluated']):
        raise ValueError('Unexpected NGX evaluation count')
    arrays, files = [], []
    for name in ['input', 'output']:
        path = directory / f'compare-{name}.rgba16f'
        if path.stat().st_size != size[0] * size[1] * 8:
            raise ValueError('Truncated scene readback')
        array = np.memmap(path, dtype='<f2', mode='r', shape=(size[1], size[0], 4))
        for row in range(0, size[1], 128):
            if not np.isfinite(array[row:row + 128, :, :3]).all():
                raise ValueError('Nonfinite scene RGB')
        arrays.append(array)
        files.append(dict(name=name, bytes=path.stat().st_size, sha256=digest(path)))
    total, changed = 0., 0
    for row in range(0, size[1], 128):
        diff = np.abs(arrays[1][row:row+128, :, :3].astype('float32') - arrays[0][row:row+128, :, :3].astype('float32'))
        total += float(diff.sum(dtype=np.float64))
        changed += int(np.count_nonzero(diff))
    if bool(changed) != dlaa:
        raise ValueError('Expected passthrough OFF and nonidentical NGX output ON')
    return dict(metadata=proof, files=files, changed_rgb_components=changed,
                mean_abs_rgb_difference=total / (size[0] * size[1] * 3))


def analyze(sessions, output):
    output.mkdir(parents=True, exist_ok=False)
    labels = ['1080p - DLAA OFF', '4K vers 1080p - DLAA OFF', '4K vers 1080p - DLAA ON']
    keys = ['native-off', '4k-off', '4k-dlaa']
    reports, sequences = [], []
    for i, session in enumerate(sessions):
        session = session.resolve()
        manifest = json.loads((session / 'session.json').read_text(encoding='utf-8'))
        dlaa = i == 2
        size = (1920, 1080) if i == 0 else (3840, 2160)
        if manifest['aa_mode'] != ('dlaa' if dlaa else 'none') or manifest['environment']['SPARTAN_NO_MSAA'] != '1':
            raise ValueError('MSAA or session mode mismatch')
        proof = scene_proof(session, size, dlaa)
        metadata, pictures = [], []
        for index in range(16):
            meta, picture = load_display(session / 'display-capture', index, dlaa)
            metadata.append(meta)
            pictures.append(picture)
            with (output / f'{keys[i]}-{index:02}.png').open('xb') as stream:
                picture.save(stream, format='PNG')
        times = np.array([m['recorded_ms'] for m in metadata])
        intervals = np.diff(times)
        if np.any(intervals <= 0):
            raise ValueError('Nonmonotonic presentation recording times')
        log = (session / 'logs/stdout.log').read_text(encoding='utf-8', errors='replace')
        after_capture = log.split('GoW3 display: index=15', 1)[1]
        profiles = [dict(ms_per_profile_frame=float(ms), frames=int(frames)) for ms, frames in
                    re.findall(r'GPU profile: ([\d.]+) ms/frame over (\d+) frames', after_capture)]
        sequences.append(pictures)
        reports.append(dict(label=labels[i], session=str(session), probe_sha256=manifest['probe_sha256'],
                            scene_proof=proof, display_frames=metadata,
                            recording_interval_median_ms=float(np.median(intervals)),
                            profile_windows_after_capture=profiles,
                            profile_limit='Timestamp spans include idle/submission gaps; not pure GPU busy time or an uncapped FPS benchmark.'))
    if len({r['probe_sha256'] for r in reports}) != 1:
        raise ValueError('Three modes must use the same probe binary')
    font = ImageFont.truetype('C:/Windows/Fonts/arial.ttf', 22)
    crops = [(850, 600, 1170, 840), (760, 180, 1080, 420), (1420, 420, 1740, 660)]
    def panel(index, zooms=True):
        canvas = Image.new('RGB', (1920, 396 + (3 * 510 if zooms else 0)), '#16191d')
        draw = ImageDraw.Draw(canvas)
        for column, pictures in enumerate(sequences):
            x = column * 640
            draw.text((x + 10, 7), labels[column], font=font, fill='white')
            canvas.paste(pictures[index].resize((640, 360), Image.Resampling.LANCZOS), (x, 36))
            if zooms:
                for n, crop in enumerate(crops):
                    top = 396 + n * 510
                    draw.text((x + 10, top + 3), f'Zone {n+1} - pixels affiches x2', font=font, fill='#bbbbbb')
                    canvas.paste(pictures[index].crop(crop).resize((640, 480), Image.Resampling.NEAREST), (x, top + 30))
        return canvas
    with (output / 'comparison.png').open('xb') as stream:
        panel(0).save(stream, format='PNG')
    with (output / 'overview.png').open('xb') as stream:
        panel(0, False).save(stream, format='PNG')
    contact = Image.new('RGB', (1920, 396 * 4))
    for row, index in enumerate([0, 5, 10, 15]):
        contact.paste(panel(index, False), (0, row * 396))
    with (output / 'motion-contact.png').open('xb') as stream:
        contact.save(stream, format='PNG')
    # The same index within each independent recording; these are not synchronized
    # actors/cameras. Fixed playback interval is disclosed in the JSON report.
    duration = int(round(float(np.median([r['recording_interval_median_ms'] for r in reports]))))
    with (output / 'motion.webp').open('xb') as stream:
        frames = [panel(index, False) for index in range(16)]
        frames[0].save(stream, format='WEBP', save_all=True, append_images=frames[1:],
                       duration=duration, loop=0, quality=90, method=4)
    result = dict(modes=reports, crops=crops, animation_interval_ms=duration,
                  limitations='Independent gameplay sessions, slightly different cameras/poses and unsynchronized motion. Actual 1080p swapchain bytes after game composition and normal presentation blit, before port overlay. No extra sharpening or offline downsampling. Equal recording ordinal is not proof of equal composition scene. Still images and one-second sequences do not establish universal ghosting/flicker quality.')
    with (output / 'analysis.json').open('x', encoding='utf-8') as stream:
        json.dump(result, stream, indent=2, allow_nan=False)
    return dict(output=str(output.resolve()), modes=[dict(label=r['label'], scene=r['scene_proof']['metadata'],
                recording_interval_median_ms=r['recording_interval_median_ms']) for r in reports])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native', type=Path, required=True)
    parser.add_argument('--off4k', type=Path, required=True)
    parser.add_argument('--dlaa4k', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(analyze([args.native, args.off4k, args.dlaa4k], args.output), indent=2))
