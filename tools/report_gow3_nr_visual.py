"""Make measured visual comparisons from completed NR GPU readbacks, no image synthesis.

Default crops describe Gaia scene 693 in the 20261028T190232846974Z session.
Use --crop-spec for another observed frame, with [{"label":...,"xyxy":[...]}].
Temporal residuals use the independent input-color correspondences in analysis.json.
They are descriptive diagnostics, not a perceptual quality or flicker certification.
"""
import argparse
import json
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw
from analyze_gow3_nr_scene import load


def bilinear(data, x, y):
    ix, iy = np.floor(x).astype(int), np.floor(y).astype(int)
    fx, fy = (x-ix)[..., None], (y-iy)[..., None]
    return ((1-fy)*((1-fx)*data[iy, ix, :3]+fx*data[iy, ix+1, :3])+
            fy*((1-fx)*data[iy+1, ix, :3]+fx*data[iy+1, ix+1, :3]))


def report(directory,crop_spec=None):
    analysis = json.loads((directory/'analysis.json').read_text())
    count = len(analysis['frames'])
    frames = [load(directory, n) for n in range(count)]
    result = dict(schema='gow3-nr-visual-v1', same_frame_before_after=True,
        stage='LDR scene before guest final composition/HUD', frames=[], temporal=[])
    for meta, data, _ in frames:
        a, b = data['input'][:, :, :3], data['output'][:, :, :3]
        delta = b-a
        luma = np.array([.2126, .7152, .0722])
        la, lb = a@luma, b@luma
        def gradient_rms(gray):
            return float(np.sqrt((np.square(np.diff(gray, axis=0)).mean()+
                                  np.square(np.diff(gray, axis=1)).mean())/2))
        result['frames'].append(dict(scene=meta['scene'],mean_abs_rgb=float(abs(delta).mean()),
            rgb_change_p95=float(np.quantile(abs(delta),.95)),
            mean_luma_change=float((lb-la).mean()),
            gradient_rms_off=gradient_rms(la),gradient_rms_nr=gradient_rms(lb)))
    for pair in analysis['pairs']:
        pi = next(n for n,f in enumerate(frames) if f[0]['scene']==pair['previous_scene'])
        previous, current = frames[pi][1], frames[pi+1][1]
        values = []
        for match in pair['matches']:
            x,y=match['x'],match['y'];dx,dy=match['measured_flow']
            yy,xx=np.mgrid[y-7:y+8,x-7:x+8]
            errors={name:float(abs(current[name][y-7:y+8,x-7:x+8,:3]-
                bilinear(previous[name],xx+dx,yy+dy)).mean()) for name in ('input','output')}
            values.append(dict(affine_instance=match.get('affine_instance',False),**errors))
        cohorts={}
        for label,chosen in [('all',values),('affine_instances',[v for v in values if v['affine_instance']])]:
            # statistics calls its units px; these values are RGB residuals, not pixels.
            cohorts[label]={}
            for name in ('input','output'):
                a=np.asarray([v[name] for v in chosen])
                cohorts[label][name]=dict(points=int(a.size),median_rgb=float(np.median(a)) if a.size else None,
                    p90_rgb=float(np.quantile(a,.9)) if a.size else None)
        result['temporal'].append(dict(scene=pair['current_scene'],cohorts=cohorts))
    crops=[('Roche : microtexture',(480,330,880,510)),
           ('Vegetation : branches et feuilles',(1000,120,1400,300)),
           ('Personnage : armure et contours',(760,650,1160,830))]
    if crop_spec:
        crops=[(r['label'],r['xyxy']) for r in json.loads(crop_spec.read_text())]
    for _,(x0,y0,x1,y1) in crops:
        if not (0<=x0<x1<=1920 and 0<=y0<y1<=1080 and x1-x0==400 and y1-y0==180):
            raise ValueError('Expected 400x180 crops inside the 1080p source')
    result['crops']=[dict(label=label,xyxy=box,scale=2,filter='nearest') for label,box in crops]
    result['limitations']='Eight frames, two short sequences, no ground-truth photorealistic reference. Temporal residuals include lighting changes, occlusion and resampling error. Lower gradient energy does not distinguish denoising from lost detail.'
    canvas=Image.new('RGB',(1600,len(crops)*390),(18,18,18));draw=ImageDraw.Draw(canvas)
    for row,(label,box) in enumerate(crops):
        for col,name in enumerate(('input','output')):
            arr=frames[0][1][name][:,:,:3]
            image=Image.fromarray(np.rint(np.clip(arr,0,1)*255).astype('uint8'))
            canvas.paste(image.crop(box).resize((800,360),Image.Resampling.NEAREST),(col*800,row*390+30))
            msaa=frames[0][0].get('scene_samples',1)==2
            state=('MSAA x2 seul' if col==0 else 'MSAA x2 + DLSS 5 NR') if msaa else ('OFF' if col==0 else 'DLSS 5 NR')
            draw.text((col*800+10,row*390+8),f'{state} | {label} | meme frame, zoom 2x',(255,255,255))
    canvas.save(directory/'visual-crops.png')
    (directory/'visual-report.json').write_text(json.dumps(result,indent=2,allow_nan=False)+'\n')
    return result


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory',type=Path)
    parser.add_argument('--crop-spec',type=Path)
    args=parser.parse_args()
    print(json.dumps(report(args.directory,args.crop_spec),indent=2))
