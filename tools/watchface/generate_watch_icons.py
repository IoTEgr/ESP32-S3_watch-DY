"""Convert cached watchOS/ChatGPT icons into the firmware's LVGL assets.

Original assets and source URLs live in icon_sources/. Run fetch_watch_icons.py
only to refresh them; normal generation and firmware builds are offline.
"""
from pathlib import Path
import json
from PIL import Image, ImageDraw
import generate as assets

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
SOURCES = HERE/'icon_sources'
MAPPING = [
    ('Gravitysphere','img_app_qmi8658ball','mindfulness'),
    ('Rtc','img_app_rtc','world_clock'),
    ('brookesia_app_calculator','img_app_calculator','calculator'),
    ('draw','img_app_drawpanel','notes'),
    ('SpecAnalyzer','img_app_specanalyzer','now_playing'),
    ('MusicPlayer','img_app_musicplayer','music'),
    ('Gallery','img_app_gallery','photos'),
    ('VideoPlayer','img_app_vedioplayer','remote'),
    ('Recorder','img_app_recorder','voice_memos'),
    ('Settings','img_app_settings','settings'),
    ('XiaozhiApp','img_app_xiaozhi','chatgpt'),
]


def app_icon(source):
    image=Image.open(SOURCES/f'app_{source}.png').convert('RGBA')
    image=image.resize((112,112),Image.Resampling.LANCZOS)
    # Round all App Store artwork to the watch's circular container. Keep alpha
    # at the edge so the blurred favorites backdrop shows through cleanly.
    mask=Image.new('L',(448,448))
    ImageDraw.Draw(mask).ellipse((0,0,447,447),fill=255)
    mask=mask.resize((112,112),Image.Resampling.LANCZOS)
    from PIL import ImageChops
    image.putalpha(ImageChops.multiply(image.getchannel('A'),mask))
    return image


def control_icons():
    assets.ASSETS=ROOT/'components/watch_shell/assets'
    # Official monochrome Control Center glyphs; LVGL tints them white in the
    # actual view. The battery tile keeps its real, live percentage label.
    controls={'wifi':'wifi','mute':'silent_mode','torch':'flashlight',
              'volume':'volume','airplane':'airplane_mode'}
    for name,source in controls.items():
        image=Image.open(SOURCES/f'control_{source}.png').convert('RGBA')
        image.thumbnail((50,50),Image.Resampling.LANCZOS)
        glyph=Image.new('RGBA',(56,56))
        glyph.alpha_composite(image,((56-image.width)//2,(56-image.height)//2))
        assets.image_c(glyph,'cc_'+name,True)
        if name=='wifi':
            assets.image_c(glyph.resize((21,21),Image.Resampling.LANCZOS),'cc_wifi_small',True)


def overview_icon():
    """Original seven-color overview glyph, independent of installed apps."""
    overview=Image.new('RGBA',(72*4,72*4))
    draw=ImageDraw.Draw(overview)
    positions=[(28,12),(14,21),(42,21),(14,39),(42,39),(28,48),(28,30)]
    colors=['#EE456E','#4DA7FF','#61D579','#A567F0','#F6B84F','#41CAD6','#FA7954']
    for (x,y),color in zip(positions,colors):
        draw.ellipse((x*4,y*4,(x+15)*4-1,(y+15)*4-1),fill=color)
    assets.ASSETS=ROOT/'components/watch_shell/assets'
    assets.image_c(overview.resize((112,112),Image.Resampling.LANCZOS),'launcher_overview',True)


def main():
    manifest=json.loads((SOURCES/'sources.json').read_text(encoding='utf-8'))
    by_name={r['name']:r for r in manifest}
    icons={}
    for component,name,source in MAPPING:
        folder=ROOT/f'components/{component}/assets'
        folder.mkdir(parents=True,exist_ok=True)
        assets.ASSETS=folder
        image=app_icon(source)
        assets.image_c(image,name,True)
        image.save(folder/f'{name}.png')
        icons[source]=image

    overview_icon()
    control_icons()

    sheet=Image.new('RGB',(660,280),'#101014')
    draw=ImageDraw.Draw(sheet)
    for i,(component,name,source) in enumerate(MAPPING):
        x,y=i%6*110,i//6*140
        icon=icons[source].resize((90,90),Image.Resampling.LANCZOS)
        sheet.paste(icon,(x+10,y+8),icon)
        draw.text((x+5,y+108),component[:16],fill='white')
    sheet.save(ROOT/'output/watchface/watchos-app-icons.png')
    mapping=[{'component':c,'symbol':n,'reference':s,
              'source':by_name['app_'+s]['url']} for c,n,s in MAPPING]
    (SOURCES/'firmware-mapping.json').write_text(json.dumps(mapping,indent=2),encoding='utf-8')
    print('Generated 11 app icons, seven-color overview, 5 official control glyphs and Wi-Fi indicator.')


if __name__=='__main__': main()
