"""Reproducible, small RGB565 cloud sphere. No runtime shader or alpha layer."""
from pathlib import Path
import math
import numpy as np
from PIL import Image
import generate as assets

ROOT = Path(__file__).resolve().parents[2]
DEST = ROOT / 'components/XiaozhiApp/assets'


def sphere(phase):
    size, scale = 176, 3
    y, x = np.mgrid[:size*scale, :size*scale].astype(float)
    x = (x + .5 - size*scale/2) / (size*scale/2 - 3)
    y = (y + .5 - size*scale/2) / (size*scale/2 - 3)
    radius = np.sqrt(x*x + y*y)
    z = np.sqrt(np.maximum(0, 1-radius*radius))
    # Smooth, periodic cloud advection on a sphere. All frames wrap seamlessly.
    u = x + .10*np.sin(phase) + .13*np.sin(4*y + .7*np.cos(phase))
    v = y + .08*np.cos(phase) + .10*np.cos(5*x + .6*np.sin(phase))
    cloud = .40*np.sin(4*u-3*v+1.6*z) + .22*np.cos(7*v+2*u) + \
            .12*np.sin(11*u+5*v + .4*np.sin(phase))
    blue = np.clip(.35 + .40*u - .47*v + cloud*.47 +
                   .30*np.exp(-((u-.40)**2+(v+.55)**2)/.35), 0, 1)
    blue = np.clip((blue-.13)/.85, 0, 1)**1.5
    white = np.array([239., 245., 255.])
    cobalt = np.array([41., 100., 240.])
    color = white[None,None,:]*(1-blue[:,:,None]) + cobalt[None,None,:]*blue[:,:,None]
    # Pale billowing folds and restrained shaded rim keep the volume soft.
    fold = 1 - .055*(.5+.5*np.sin(9*v+4*u+2*cloud))*(1-blue)
    shade = (.88+.12*z) * fold
    color *= shade[:,:,None]
    highlight = np.exp(-((x+.34)**2+(y+.36)**2)/.28)*.08
    color += highlight[:,:,None]*255
    mask = np.clip((1-radius)*size*scale/2, 0, 1)
    color = np.clip(color,0,255)*mask[:,:,None]
    image = Image.fromarray(color.astype('uint8')).resize((size,size),Image.Resampling.LANCZOS)
    # Stable spatial dithering reduces contour bands on the RGB565 display.
    pixels = np.asarray(image).astype(float)
    jitter = np.random.default_rng(42).uniform(-.5,.5,pixels.shape)*[8,4,8]
    inside = np.max(pixels,axis=2)>4
    pixels += jitter*inside[:,:,None]
    return Image.fromarray(np.clip(pixels,0,255).astype('uint8'))


def generate_fonts():
    assets.ASSETS = DEST
    statuses = '需要网络连接正在准备小嘉等待设备激活连接待命聆听思考回答服务异常'
    hints = '轻点小球，或说「你好小智小嘉」唤醒词我在听请吧可打断检查网络稍后重试'
    assets.font_c(assets.HERE/'fonts/MiSans-Medium.ttf', 'xiaozhi_status_font',24,statuses)
    assets.font_c(assets.HERE/'fonts/MiSans-Medium.ttf', 'xiaozhi_hint_font',18,hints)
    # Chat subtitles use a separate 30px font. The common Puhui subset lacks
    # 珈; include both spellings for the built-in-font fallback path as well.
    assets.font_c(assets.HERE/'fonts/MiSans-Medium.ttf', 'xiaozhi_conversation_extra_font',30,'嘉珈')


def main():
    assets.ASSETS = DEST
    assets.OUT.mkdir(parents=True, exist_ok=True)
    frames = []
    for index in range(12):
        frame = sphere(index*2*math.pi/12)
        frames.append(frame)
        assets.image_c(frame, f'xiaozhi_orb_{index:02d}')
    frames[0].save(assets.OUT/'xiaozhi-orb.gif', save_all=True,
                   append_images=frames[1:], duration=160, loop=0)
    names = [f'xiaozhi_orb_{i:02d}' for i in range(12)]
    (DEST/'xiaozhi_orb_frames.c').write_text('#include "lvgl.h"\n'+
        '\n'.join(f'LV_IMAGE_DECLARE({n});' for n in names)+
        '\nconst lv_image_dsc_t *const xiaozhi_orb_frames[12] = {\n'+
        ',\n'.join(f'    &{n}' for n in names)+'\n};\n', encoding='utf-8')
    generate_fonts()


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument('--fonts-only', action='store_true')
    args = parser.parse_args()
    generate_fonts() if args.fonts_only else main()
