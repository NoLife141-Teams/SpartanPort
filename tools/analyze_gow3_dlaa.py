"""Inspect the bounded, completed GPU input/output pair of a real NGX DLAA call.

Finite, different output proves GPU output content, not universal image quality.
Raw data is never modified; PNGs are sRGB previews of linear RGB, alpha omitted.
"""
import argparse
import hashlib
import json
from pathlib import Path
import numpy as np
from PIL import Image


def analyze(directory):
    capture=json.loads((directory/'capture.json').read_text())
    if not capture['complete'] or not capture['gpu_complete'] or not capture['tick']:
        raise ValueError('GPU capture did not complete')
    if (capture['width'],capture['height'],capture['bytes_per_image'])!=(1920,1080,16588800):
        raise ValueError('Unexpected native DLAA dimensions or stride')
    arrays=[]
    files=[]
    for name in ['input','output']:
        path=directory/(name+'.rgba16f')
        raw=path.read_bytes()
        if len(raw)!=capture['bytes_per_image']:
            raise ValueError('Truncated GPU capture')
        pixels=np.frombuffer(raw,dtype='<f2').reshape(1080,1920,4)[...,:3].astype(np.float32)
        if not np.isfinite(pixels).all():
            raise ValueError('Nonfinite RGB in '+name)
        files.append(dict(name=name,sha256=hashlib.sha256(raw).hexdigest(),bytes=len(raw),
                          rgb_min=float(pixels.min()),rgb_max=float(pixels.max())))
        arrays.append(pixels)
        linear=np.clip(pixels,0,1)
        srgb=np.where(linear<=.0031308,12.92*linear,1.055*linear**(1/2.4)-.055)
        preview=Image.fromarray(np.rint(srgb*255).astype('uint8'))
        with (directory/(name+'.png')).open('xb') as stream:
            preview.save(stream,format='PNG')
    difference=np.abs(arrays[1]-arrays[0])
    report=dict(capture=capture,files=files,all_rgb_finite=True,
                changed_rgb_components=int(np.count_nonzero(difference)),
                rgb_mean_abs_difference=float(difference.mean()),
                rgb_p99_abs_difference=float(np.quantile(difference,.99)),
                identical=bool(not np.any(difference)),
                quality_validated=False,
                limitations='One actual completed NGX input/output pair; output alpha is unspecified and omitted. Difference is not a quality metric. Inspect gameplay and HUD separately.')
    with (directory/'analysis.json').open('x',encoding='utf-8') as stream:
        json.dump(report,stream,indent=2,allow_nan=False)
    if report['identical']:
        raise ValueError('NGX output is identical; investigate whether temporal filtering executed')
    return report


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory',type=Path)
    print(json.dumps(analyze(parser.parse_args().directory),indent=2))
