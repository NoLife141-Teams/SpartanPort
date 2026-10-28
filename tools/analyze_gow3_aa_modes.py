"""Validate and display three real, completed 1080p AA captures.

Each mode comes from an isolated process. Equal scene indexes do not imply
identical animation; camera matrices and that limitation remain in the report.
Raw captures are read only; all generated artifacts use exclusive creation.
"""
import argparse
import hashlib
import json
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont


def preview(rgb):
    linear = np.clip(rgb, 0, 1)
    srgb = np.where(linear <= .0031308, linear * 12.92, 1.055 * linear ** (1 / 2.4) - .055)
    return Image.fromarray(np.rint(srgb * 255).astype('uint8'))


def analyze(sessions, output):
    output.mkdir(parents=True, exist_ok=False)
    reports, pictures, dlaa_pair = [], [], []
    labels = ['Sans AA (MSAA et DLAA off)', 'MSAA x2 seul', 'DLAA seul (MSAA off)']
    for mode, session in zip(['none', 'msaa2', 'dlaa'], sessions, strict=True):
        manifest = json.loads((session / 'session.json').read_text(encoding='utf-8'))
        directory = session / 'dlss-capture'
        capture = json.loads((directory / 'compare.json').read_text())
        if manifest['aa_mode'] != mode or not capture['complete'] or not capture['gpu_complete']:
            raise ValueError('Wrong mode or incomplete capture: ' + mode)
        if (capture['width'], capture['height'], capture['sharpness']) != (1920, 1080, 0):
            raise ValueError('Comparison must be native 1080p with sharpening off')
        if capture['samples'] != (2 if mode == 'msaa2' else 1) or capture['dlaa'] != (mode == 'dlaa'):
            raise ValueError('GPU source sample count or DLAA state mismatch')
        if (mode == 'dlaa' and capture['evaluated'] < 32) or (mode != 'dlaa' and capture['evaluated']):
            raise ValueError('NGX evaluation count does not match the mode')
        arrays, files = [], []
        for name in ['input', 'output']:
            raw = (directory / f'compare-{name}.rgba16f').read_bytes()
            if len(raw) != 1920 * 1080 * 8:
                raise ValueError('Truncated readback')
            rgb = np.frombuffer(raw, dtype='<f2').reshape(1080, 1920, 4)[..., :3].astype(np.float32)
            if not np.isfinite(rgb).all():
                raise ValueError('Nonfinite GPU RGB')
            arrays.append(rgb)
            files.append(dict(name=name, bytes=len(raw), sha256=hashlib.sha256(raw).hexdigest()))
            with (output / f'{mode}-{name}.png').open('xb') as stream:
                preview(rgb).save(stream, format='PNG')
        difference = np.abs(arrays[1] - arrays[0])
        identical = not bool(np.any(difference))
        if identical == (mode == 'dlaa'):
            raise ValueError('Expected passthrough baselines and a changed NGX output')
        reports.append(dict(mode=mode, session=str(session), binary_sha256=manifest['probe_sha256'],
                            capture=capture, files=files, identical_input_output=identical,
                            rgb_mean_abs_difference=float(difference.mean())))
        pictures.append(preview(arrays[1]))
        if mode == 'dlaa':
            dlaa_pair = [preview(array) for array in arrays]
    if len({r['binary_sha256'] for r in reports}) != 1:
        raise ValueError('All modes must use the same probe binary')
    camera0 = np.array(reports[0]['capture']['camera_vp'])
    for report in reports:
        report['max_camera_matrix_delta_vs_none'] = float(np.max(np.abs(np.array(report['capture']['camera_vp']) - camera0)))
    font_path = Path('C:/Windows/Fonts/arial.ttf')
    font = ImageFont.truetype(str(font_path), 22) if font_path.is_file() else ImageFont.load_default()
    # Three identical pixel rectangles, displayed at integer 2x without smoothing.
    crops = [(850, 620, 1170, 860), (300, 300, 620, 540), (1420, 420, 1740, 660)]
    canvas = Image.new('RGB', (1920, 36 + 360 + 3 * 510), '#16191d')
    draw = ImageDraw.Draw(canvas)
    for i, (label, picture) in enumerate(zip(labels, pictures, strict=True)):
        x = i * 640
        draw.text((x + 12, 6), label, font=font, fill='white')
        canvas.paste(picture.resize((640, 360), Image.Resampling.LANCZOS), (x, 36))
        for n, crop in enumerate(crops):
            top = 396 + n * 510
            draw.text((x + 12, top + 3), f'Zone {n+1} - pixels x2', font=font, fill='#bbbbbb')
            canvas.paste(picture.crop(crop).resize((640, 480), Image.Resampling.NEAREST), (x, top + 30))
    with (output / 'comparison.png').open('xb') as stream:
        canvas.save(stream, format='PNG')
    paired = Image.new('RGB', (1280, 36 + 360 + 3 * 510), '#16191d')
    draw = ImageDraw.Draw(paired)
    for i, (label, picture) in enumerate(zip(['Entree simple avec jitter', 'Sortie DLAA - meme frame GPU'], dlaa_pair, strict=True)):
        x = i * 640
        draw.text((x + 12, 6), label, font=font, fill='white')
        paired.paste(picture.resize((640, 360), Image.Resampling.LANCZOS), (x, 36))
        for n, crop in enumerate(crops):
            top = 396 + n * 510
            draw.text((x + 12, top + 3), f'Zone {n+1} - pixels x2', font=font, fill='#bbbbbb')
            paired.paste(picture.crop(crop).resize((640, 480), Image.Resampling.NEAREST), (x, top + 30))
    with (output / 'dlaa-pair.png').open('xb') as stream:
        paired.save(stream, format='PNG')
    result = dict(modes=reports, crops=crops, quality_score=False,
                  limitations='Three isolated sessions; animations/effects may differ even at equal composition index. Camera deltas reported. These are linear scene colors encoded as sRGB previews, before final game composition/HUD. Baseline disables known MSAA and all port AA; absence of every possible guest post-AA pass has not been established. Differences are not quality scores.')
    with (output / 'analysis.json').open('x', encoding='utf-8') as stream:
        json.dump(result, stream, indent=2, allow_nan=False)
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--none', type=Path, required=True)
    parser.add_argument('--msaa2', type=Path, required=True)
    parser.add_argument('--dlaa', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(analyze([args.none, args.msaa2, args.dlaa], args.output), indent=2))
