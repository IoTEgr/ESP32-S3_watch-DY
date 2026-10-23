"""Compile a headless, native LVGL preview with the installed Qt Clang/VC SDK."""
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor, as_completed
import hashlib
import os
import subprocess
import sys
from PIL import Image

ROOT=Path(__file__).resolve().parents[2]
LV=ROOT/'managed_components/lvgl__lvgl'
BUILD=ROOT/'build/watchface-host'
CLANG=Path(r'D:\Qt\Tools\QtCreator\bin\clang\bin\clang.exe')
LLD=Path(r'C:\Espressif\tools\esp-clang\esp-21.1.3_20260408\esp-clang\bin\lld.exe')
VC=Path(r'D:\VS2013\VC')
SDK=Path(r'C:\Program Files (x86)\Windows Kits\8.1')


def main():
    BUILD.mkdir(parents=True,exist_ok=True)
    expected = ROOT/'output/watchface/gif-expected.txt'
    inputs = [*list((ROOT/'spiffs/watchface').glob('*.gif')), Path(__file__).with_name('verify_gif_frames.py')]
    if not expected.exists() or any(p.stat().st_mtime > expected.stat().st_mtime for p in inputs):
        from verify_gif_frames import main as prepare_expected
        prepare_expected()
    conf='''#ifndef LV_CONF_H
#define LV_CONF_H
#define LV_COLOR_DEPTH 16
#define LV_DEF_REFR_PERIOD 15
#define LV_USE_OS 5
#define LV_USE_LOG 0
#define LV_USE_STDLIB_MALLOC 0
#define LV_USE_STDLIB_STRING 0
#define LV_USE_STDLIB_SPRINTF 0
#define LV_MEM_SIZE (32*1024*1024)
#define LV_USE_THEME_DEFAULT 0
#define LV_USE_THEME_SIMPLE 0
#define LV_USE_THEME_MONO 0
#define LV_DRAW_SW_DRAW_UNIT_CNT 2
#define LV_FONT_FMT_TXT_LARGE 1
#define LV_USE_GIF 1
#define LV_USE_SNAPSHOT 1
#define LV_USE_FS_STDIO 1
#define LV_FS_STDIO_LETTER 65
#define LV_FS_STDIO_PATH "PROJECT_ROOT"
#define LV_FS_STDIO_CACHE_SIZE 0
#define LV_USE_FS_POSIX 0
#endif
'''.replace('PROJECT_ROOT',ROOT.as_posix())
    (BUILD/'lv_conf.h').write_text(conf,encoding='utf-8')
    flags=['--target=x86_64-pc-windows-msvc','-std=c11','-O1','-fms-extensions',
           '-DWIN32','-D_WIN32','-D_CRT_SECURE_NO_WARNINGS','-DLV_CONF_INCLUDE_SIMPLE',
           '-DLV_KCONFIG_IGNORE','-DLV_LVGL_H_INCLUDE_SIMPLE','-Wno-everything']
    includes=[VC/'include',SDK/'Include/shared',SDK/'Include/um',BUILD,LV,
              ROOT/'components/watch_shell/include',ROOT/'components/XiaozhiApp',ROOT/'build/config']
    for p in includes: flags+=['-I',str(p)]
    sources=[p for p in (LV/'src').rglob('*.c') if p != LV/'src/widgets/gif/lv_gif.c']
    sources+=[ROOT/'patches/lvgl-gif/lv_gif.c']
    sources+=[ROOT/'components/watch_shell/watch_launcher_view.c',ROOT/'components/watch_shell/watch_face_view.c',ROOT/'components/watch_shell/watch_control_view.c',ROOT/'components/watch_shell/watch_control_effect.c',ROOT/'tools/watchface/preview_lvgl.c']
    sources+=list((ROOT/'components/watch_shell/assets').glob('*.c'))
    sources+=[ROOT/'components/XiaozhiApp/xiaozhi_voice_view.c',ROOT/'tools/watchface/preview_voice.c']
    sources+=[p for p in (ROOT/'components/XiaozhiApp/assets').glob('xiaozhi_*.c')]
    # The production cbin font contains arbitrary conversation glyphs. A small
    # host-only subset provides the same 30px layout for the captured examples.
    import generate as asset_generator
    asset_generator.ASSETS=BUILD
    asset_generator.font_c(ROOT/'managed_components/78__xiaozhi-fonts/ttf/puhui-common.ttf',
                           'voice_preview_font',30,
                           '0123456789小嘉今天宜昌天气晴，适合出去走走。请在小智控制面板添加设备并输入激活码')
    sources.append(BUILD/'voice_preview_font.c')
    for component,name in [('MusicPlayer','musicplayer'),('Settings','settings'),('Gallery','gallery'),
                           ('draw','drawpanel'),('XiaozhiApp','xiaozhi'),('Gravitysphere','qmi8658ball'),
                           ('brookesia_app_calculator','calculator'),('SpecAnalyzer','specanalyzer'),
                           ('Rtc','rtc'),('Recorder','recorder'),('VideoPlayer','vedioplayer')]:
        sources.append(ROOT/f'components/{component}/assets/img_app_{name}.c')
    # The view header and asset contents participate in the host object cache.
    stamp=conf+(ROOT/'components/watch_shell/include/watch_face_layout.h').read_text(encoding='utf-8')
    stamp+=(ROOT/'components/watch_shell/include/watch_face_view.h').read_text(encoding='utf-8')
    stamp+=(ROOT/'components/watch_shell/include/watch_control_view.h').read_text(encoding='utf-8')
    stamp+=(ROOT/'components/watch_shell/include/watch_face_animations.h').read_text(encoding='utf-8')
    stamp+=(ROOT/'components/watch_shell/include/watch_launcher_view.h').read_text(encoding='utf-8')
    stamp+=(ROOT/'components/XiaozhiApp/xiaozhi_voice_view.h').read_text(encoding='utf-8')
    stamp+=(ROOT/'components/XiaozhiApp/xiaozhi_wake_config.h').read_text(encoding='utf-8')
    stamp+=(ROOT/'build/config/sdkconfig.h').read_text(encoding='utf-8')
    key=hashlib.sha256(stamp.encode()).hexdigest()[:8]
    def compile_one(src):
        obj=BUILD/(hashlib.sha256(str(src).encode()).hexdigest()[:14]+'-'+key+'.obj')
        if obj.exists() and obj.stat().st_mtime>src.stat().st_mtime:return obj,None
        p=subprocess.run([str(CLANG),*flags,'-c',str(src),'-o',str(obj)],capture_output=True,text=True)
        return obj,None if p.returncode==0 else f'{src.relative_to(ROOT)}\n{p.stderr[:3500]}'
    objects=[];errors=[]
    with ThreadPoolExecutor(max_workers=8) as pool:
        jobs=[pool.submit(compile_one,s) for s in sources]
        for task in as_completed(jobs):
            obj,error=task.result();objects.append(obj)
            if error:errors.append(error)
    if errors:
        (BUILD/'compile-errors.txt').write_text('\n'.join(errors),encoding='utf-8')
        print('\n'.join(errors[:3]));print(f'{len(errors)} host compile failures');return 1
    args=[*map(str,objects),'/out:'+str(BUILD/'preview.exe'),'/subsystem:console',
          '/defaultlib:libcmt','/defaultlib:oldnames','/defaultlib:kernel32',
          '/libpath:'+str(VC/'lib/amd64'),'/libpath:'+str(SDK/'Lib/winv6.3/um/x64')]
    (BUILD/'link.rsp').write_text('\n'.join('"'+a+'"' for a in args),encoding='utf-8')
    p=subprocess.run([str(LLD),'-flavor','link','@'+str(BUILD/'link.rsp')],capture_output=True,text=True)
    if p.returncode:print(p.stderr[:6000]);return 2
    reports=[];full_frames={}
    for mode in ([],['--partial']):
        p=subprocess.run([str(BUILD/'preview.exe'),*mode],cwd=ROOT,capture_output=True,text=True)
        reports.append(p.stdout+p.stderr)
        print(p.stdout,p.stderr)
        if p.returncode:
            (ROOT/'output/watchface/ui-runtime-verification.txt').write_text('\n'.join(reports),encoding='utf-8')
            print('Renderer exit:',p.returncode);return 3
        frames={f.name:hashlib.sha256(f.read_bytes()).hexdigest()
                for f in (ROOT/'output/watchface').glob('*.ppm')}
        if not mode:
            full_frames=frames
            saved=ROOT/'output/watchface/full-render-reference';saved.mkdir(exist_ok=True)
            for name in frames:(saved/name).write_bytes((ROOT/'output/watchface'/name).read_bytes())
        elif frames!=full_frames:
            mismatch=[f for f in frames if frames[f]!=full_frames.get(f)]
            # LVGL's image resampler rounds independently at clipped stripe
            # boundaries. Permit one RGB565 level in the launcher captures;
            # all other UI captures must remain byte-identical.
            allowed={'launcher-favorites.ppm','launcher-all-apps.ppm',
                     'launcher-expanding.ppm','launcher-scrolled.ppm','launcher-native-slide.ppm'}
            for name in mismatch:
                if name not in allowed:break
                a=Image.open(ROOT/'output/watchface/full-render-reference'/name)
                b=Image.open(ROOT/'output/watchface'/name)
                differences=[tuple(abs(x-y) for x,y in zip(pa,pb))
                             for pa,pb in zip(a.get_flattened_data(),b.get_flattened_data()) if pa!=pb]
                if len(differences)>256 or any(r>9 or g>5 or blue>9 for r,g,blue in differences):break
            else:
                reports.append(f'SCALED_ICON_ROUNDING=PASS ({len(mismatch)} launcher images, <=256 pixels each, <=1 RGB565 level)\n')
                mismatch=[]
            if mismatch:
                reports.append('FULL/PARTIAL image mismatch: '+str(mismatch))
                (ROOT/'output/watchface/ui-runtime-verification.txt').write_text('\n'.join(reports),encoding='utf-8')
                print(reports[-1]);return 4
    reports.append(f'FULL_PARTIAL_PIXELS=PASS ({len(full_frames)} UI images checked; launcher resampling bounded to one RGB565 level)\n')
    print(reports[-1])
    (ROOT/'output/watchface/ui-runtime-verification.txt').write_text('\n'.join(reports),encoding='utf-8')
    for src in (ROOT/'output/watchface').glob('*.ppm'):
        Image.open(src).save(src.with_suffix('.png'))
    return 0


if __name__=='__main__':sys.exit(main())
