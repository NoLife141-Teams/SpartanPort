"""Compare actual GPU RCAS levels on the same DLAA frame and bounded sequence.

Reports signal changes, not a universal image-quality or flicker score.
Requires NumPy and Pillow. All artifacts are created exclusively.
"""
import argparse
import hashlib
import json
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw, ImageFont


def write_image(path, image, **options):
    with path.open('xb') as stream:
        image.save(stream,**options)


def preview(rgb):
    x=np.clip(rgb,0,1)
    x=np.where(x<=.0031308,12.92*x,1.055*x**(1/2.4)-.055)
    return Image.fromarray(np.rint(x*255).astype('uint8'))


def gradient(x):
    return float((np.abs(np.diff(x,axis=0)).mean()+np.abs(np.diff(x,axis=1)).mean())*.5)


def analyze(directory):
    capture=json.loads((directory/'capture.json').read_text())
    w,h=capture['width'],capture['height']
    if not (capture['complete'] and capture['gpu_complete'] and capture['tick'] and
            (w,h) in [(1920,1080),(2560,1440)] and capture['bytes_per_image']==w*h*8):
        raise ValueError('Incomplete or unsupported GPU capture')
    images=[]
    files=[]
    for name in ['output','low','medium']:
        data=(directory/(name+'.rgba16f')).read_bytes()
        if len(data)!=w*h*8: raise ValueError('Truncated '+name)
        x=np.frombuffer(data,dtype='<f2').reshape(h,w,4)[...,:3].astype('float32')
        if not np.isfinite(x).all(): raise ValueError('Nonfinite '+name)
        images.append(x)
        files.append(dict(name=name,sha256=hashlib.sha256(data).hexdigest(),bytes=len(data)))
        if name!='output': write_image(directory/(name+'.png'),preview(x),format='PNG')
    base=images[0]
    padded=np.pad(base,((1,1),(1,1),(0,0)),mode='edge')
    local=[padded[y:y+h,x:x+w] for y in range(3) for x in range(3)]
    low=np.minimum.reduce(local);high=np.maximum.reduce(local)
    measures=[]
    for level,x in zip([0,20,40],images):
        excess=np.maximum(np.maximum(x-high,low-x),0)
        measures.append(dict(level=level,gradient=gradient(x),
                             mean_abs_change=float(np.abs(x-base).mean()),
                             mean_signed_change=float((x-base).mean()),
                             local_range_excess_max=float(excess.max()),
                             local_range_excess_fraction_over_0001=float((excess>.001).mean())))
    try: font=ImageFont.truetype('C:/Windows/Fonts/segoeui.ttf',22)
    except OSError: font=ImageFont.load_default()
    # Identical native-resolution areas, magnified without interpolation.
    boxes=[(230,220,550,450),(720,430,1040,660)] if w==1920 else [(300,400,620,630),(1090,690,1410,920)]
    canvas=Image.new('RGB',(1920,1010),'#111820');draw=ImageDraw.Draw(canvas)
    for col,(level,x) in enumerate(zip([0,20,40],images)):
        image=preview(x)
        draw.text((col*640+18,8),f'DLAA - nettete {level} %',font=font,fill='white')
        for row,box in enumerate(boxes):
            canvas.paste(image.crop(box).resize((640,460),Image.Resampling.NEAREST),(col*640,50+row*480))
    write_image(directory/'sharpness-comparison.png',canvas,format='PNG')
    seq=directory/'sequence';metadata=[];frames=[[],[],[]]
    for n in range(16):
        m=json.loads((seq/(str(n)+'.json')).read_text())
        if not(m['complete'] and m['gpu_complete'] and m['width']==w//2 and m['height']==h//2 and m['sharpness']==[0,20,40]):
            raise ValueError('Incomplete sequence frame')
        if metadata and (m['scene']!=metadata[-1]['scene']+8 or m['tick']<=metadata[-1]['tick']):
            raise ValueError('Unordered sequence')
        metadata.append(m)
        for level in range(3):
            data=(seq/f'{n}_{level}.rgba8').read_bytes()
            if len(data)!=(w//2)*(h//2)*4: raise ValueError('Truncated sequence')
            x=np.frombuffer(data,dtype='uint8').reshape(h//2,w//2,4)[...,:3]
            frames[level].append(x)
    temporal=[]
    for level in range(3):
        differences=[np.abs(frames[level][n].astype(float)-frames[level][n-1]).mean()/255 for n in range(1,16)]
        temporal.append(float(np.mean(differences)))
    animations=[]
    for n in range(16):
        panel=Image.new('RGB',(1440,320),'#111820');d=ImageDraw.Draw(panel)
        for col,level in enumerate([0,20,40]):
            d.text((col*480+14,5),f'Nettete {level} %',font=font,fill='white')
            panel.paste(Image.fromarray(frames[col][n]).resize((480,270),Image.Resampling.LANCZOS),(col*480,42))
        animations.append(panel)
    # 8 scene intervals at nominal 60 Hz; presentation timing was not captured.
    write_image(directory/'sharpness-motion.webp',animations[0],format='WEBP',save_all=True,
                append_images=animations[1:],duration=133,loop=0,lossless=True)
    strip=Image.new('RGB',(1440,1280),'#111820')
    for row,index in enumerate([0,5,10,15]): strip.paste(animations[index],(0,row*320))
    write_image(directory/'motion-contact.png',strip,format='PNG')
    report=dict(capture=capture,files=files,spatial=measures,sequence=metadata,
                unregistered_temporal_mae=temporal,
                temporal_mae_ratio_to_neutral=[v/temporal[0] for v in temporal],
                limitations='Three GPU levels on identical source frames. Sequence previews are half resolution, scene-spaced, not a measured-rate video. Temporal MAE includes real animation and camera motion; it is not a flicker or ghosting score. Sharpening affects presentation only.')
    with (directory/'sharpness-analysis.json').open('x',encoding='utf-8') as stream:
        json.dump(report,stream,indent=2,allow_nan=False)
    return report


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory',type=Path)
    report=analyze(parser.parse_args().directory)
    print(json.dumps({k:v for k,v in report.items() if k!='sequence'},indent=2))
