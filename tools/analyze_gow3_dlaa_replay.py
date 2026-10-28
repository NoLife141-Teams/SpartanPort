"""Analyze an opt-in real NGX jitter replay; never treats sharpness as correctness.

Usage: python tools/analyze_gow3_dlaa_replay.py ANALYTIC_DIRECTORY REAL_DIRECTORY
The analytic run fits known X/Y waves, independently of the NGX motion/jitter.
The real run uses a frozen, bilinearly resampled crop, not fresh game frames.
"""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw, ImageFont


def load(directory, name):
    data=(directory/(name+'.rgba16f')).read_bytes()
    if len(data)!=768*512*8:
        raise ValueError('Truncated replay output: '+name)
    array=np.frombuffer(data,dtype='<f2').reshape(512,768,4)[...,:3].astype(np.float64)
    if not np.isfinite(array).all():
        raise ValueError('Nonfinite RGB: '+name)
    return array,hashlib.sha256(data).hexdigest()


def read_metrics(directory):
    with (directory/'results.csv').open() as f:
        return {r['name']:{k:float(v) for k,v in r.items() if k!='name'}
                for r in csv.DictReader(f)}


def analyze(analytic, real):
    fits={}
    for name in ['negative','positive','zero']:
        phases=[]
        for frame in range(40,48):
            a,_=load(analytic,f'{name}.{frame}')
            axes=[]
            for t,z in [(np.arange(64,752),a[200,64:752,0]),
                        (np.arange(16,496),a[16:496,300,1])]:
                theta=t*2*np.pi*.14
                basis=np.stack([np.sin(theta),np.cos(theta),np.ones_like(theta)],axis=1)
                coefficients=np.linalg.lstsq(basis,z,rcond=None)[0]
                axes.append(dict(amplitude_retained=float(np.hypot(*coefficients[:2])/.18),
                                 phase_error_pixels=float(np.arctan2(coefficients[1],coefficients[0])/(2*np.pi*.14))))
            phases.append(axes)
        fits[name]=dict(phases=phases,
                       mean_amplitude_retained=float(np.mean([[a['amplitude_retained'] for a in p] for p in phases])),
                       max_abs_phase_error_pixels=max(abs(a['phase_error_pixels']) for p in phases for a in p))
    m=read_metrics(real)
    reductions={key:1-m[new]['rmse']/m[old]['rmse'] for key,old,new in
                [('static','negative','positive'),('translated','moving','moving-positive')]}
    checks=dict(corrected_phase_below_002_pixels=fits['positive']['max_abs_phase_error_pixels']<.02,
                inverse_phase_above_004_pixels=fits['negative']['max_abs_phase_error_pixels']>.04,
                static_rmse_reduced=reductions['static']>.1,
                translated_rmse_reduced=reductions['translated']>.2)
    canvas=Image.new('RGB',(1536,1072),'#111820')
    draw=ImageDraw.Draw(canvas)
    try: font=ImageFont.truetype('C:/Windows/Fonts/segoeui.ttf',23)
    except OSError: font=ImageFont.load_default()
    hashes={}
    for col,name in enumerate(['negative','positive']):
        a,hashes[name]=load(real,name)
        a=np.clip(a,0,1)
        srgb=np.where(a<=.0031308,12.92*a,1.055*a**(1/2.4)-.055)
        preview=Image.fromarray(np.rint(srgb*255).astype('uint8'))
        label='Ancien jitter inverse' if col==0 else 'Jitter corrige - sans accentuation'
        draw.text((col*768+18,8),label,font=font,fill='white')
        canvas.paste(preview.crop((0,0,384,512)).resize((768,1024),Image.Resampling.NEAREST),(col*768,48))
    # Derived artifacts use exclusive creation; raw evidence is never modified.
    with (real/'jitter-comparison.png').open('xb') as f: canvas.save(f,format='PNG')
    report=dict(analytic_fits=fits,real_metrics=m,rmse_reduction=reductions,
                output_sha256=hashes,checks=checks,all_passed=all(checks.values()),
                limitations='Controlled frozen real crop and analytic patterns. Does not certify every game draw, transparency, disocclusion or temporal stability.')
    with (real/'jitter-analysis.json').open('x',encoding='utf-8') as f:
        json.dump(report,f,indent=2,allow_nan=False)
    if not report['all_passed']:
        raise ValueError('Jitter validation failed; inspect jitter-analysis.json')
    return report


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('analytic',type=Path)
    parser.add_argument('real',type=Path)
    args=parser.parse_args()
    report=analyze(args.analytic,args.real)
    print(json.dumps({k:v for k,v in report.items() if k!='analytic_fits'},indent=2))
