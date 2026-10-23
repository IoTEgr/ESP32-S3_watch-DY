"""Export the actual headless LVGL transition frames as GIF and H.264."""
from pathlib import Path
import subprocess
from PIL import Image
ROOT=Path(__file__).resolve().parents[2]
OUT=ROOT/'output/watchface'

def phase(kind,target):
    result=[]
    target_bytes=Image.open(OUT/target).convert('RGB').tobytes()
    for path in sorted(OUT.glob('control-'+kind+'-*.ppm')):
        frame=Image.open(path).convert('RGB')
        result.append((path,frame))
        if len(result)>1 and frame.tobytes()==target_bytes:break
    return result

if __name__=='__main__':
    opening=phase('open','control-main.ppm')
    closing=phase('close','control-underlay.ppm')
    assert opening and closing
    sequence=[(opening[0][0],opening[0][1],600)]
    sequence += [(p,im,30) for p,im in opening]
    sequence += [(opening[-1][0],opening[-1][1],700)]
    sequence += [(p,im,30) for p,im in closing]
    sequence += [(closing[-1][0],closing[-1][1],600)]
    # One shared palette avoids color flicker between adjacent animation frames.
    montage=Image.new('RGB',(164*len(sequence),201))
    for n,(_,im,_) in enumerate(sequence):montage.paste(im.resize((164,201)),(n*164,0))
    palette=montage.quantize(colors=256,method=Image.Quantize.MEDIANCUT)
    frames=[im.quantize(palette=palette,dither=Image.Dither.NONE) for _,im,_ in sequence]
    frames[0].save(OUT/'control-glass-animation.gif',save_all=True,append_images=frames[1:],
                   duration=[d for _,_,d in sequence],loop=0,disposal=2,optimize=False)
    listing=[]
    for path,_,duration in sequence:
        listing += ["file '"+path.as_posix()+"'",f'duration {duration/1000:.6f}']
    listing += ["file '"+sequence[-1][0].as_posix()+"'"]
    concat=OUT/'control-motion-concat.txt';concat.write_text('\n'.join(listing)+'\n',encoding='utf-8')
    ffmpeg=Path(r'D:/ffmpeg/bin/ffmpeg.exe')
    subprocess.run([str(ffmpeg),'-hide_banner','-loglevel','error','-y','-f','concat','-safe','0',
                    '-i',str(concat),'-vf','fps=30','-c:v','libx264','-crf','18','-pix_fmt','yuv420p',
                    '-movflags','+faststart',str(OUT/'control-glass-animation.mp4')],check=True)
    Image.open(OUT/'control-main.ppm').save(OUT/'control-glass-main.png')
    Image.open(OUT/'control-bottom.ppm').save(OUT/'control-glass-bottom.png')
    print(f'Exported actual LVGL motion: open={len(opening)} frames, close={len(closing)} frames.')
