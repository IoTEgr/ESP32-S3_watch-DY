"""Cache official reference assets, keeping their URLs and original files."""
from concurrent.futures import ThreadPoolExecutor
from html.parser import HTMLParser
from pathlib import Path
import io
import json
import urllib.request
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[2]
DEST = ROOT/'tools/watchface/icon_sources'
APP_PAGE = 'https://support.apple.com/en-ie/guide/watch/apda1bf1a95b/27/watchos/27'
CONTROL_PAGE = 'https://support.apple.com/en-ie/guide/watch/apd06bc15da1/27/watchos/27'
APP_NAMES = ['Music', 'Settings', 'Photos', 'Calculator', 'Voice Memos',
             'World Clock', 'Mindfulness', 'Now Playing', 'Remote', 'Notes']


def download(url):
    request = urllib.request.Request(url, headers={'User-Agent':'Mozilla/5.0'})
    with urllib.request.urlopen(request,timeout=30) as response:
        return response.read()


class Images(HTMLParser):
    def __init__(self):
        super().__init__()
        self.images = {}

    def handle_starttag(self, tag, attributes):
        attributes = dict(attributes)
        if tag == 'img' and attributes.get('src','').startswith('https://ipcdn-web.apple.com/'):
            self.images[attributes.get('alt','')] = attributes['src']


def main():
    DEST.mkdir(parents=True,exist_ok=True)
    records = []
    for category,page in [('app',APP_PAGE),('control',CONTROL_PAGE)]:
        parser = Images()
        parser.feed(download(page).decode('utf-8'))
        for alt,url in parser.images.items():
            if category=='app' and alt.removesuffix(' icon') not in APP_NAMES:
                continue
            if category=='control' and 'button' not in alt:
                continue
            name=alt.lower().replace(' icon','').replace(' switch button','').replace(' button','').replace(' ','_').replace('-','')
            records.append({'name':category+'_'+name,'alt':alt,'url':url,'page':page})
    # Apple-hosted App Store metadata identifies OpenAI's real ChatGPT app icon.
    store_url='https://itunes.apple.com/lookup?id=6448311069&country=us'
    app=json.loads(download(store_url))['results'][0]
    if app['bundleId']!='com.openai.chat':
        raise RuntimeError('Unexpected app identity')
    records.append({'name':'app_chatgpt','alt':'ChatGPT (OpenAI)',
                    'url':app['artworkUrl512'],'page':store_url})

    def fetch(record):
        path=DEST/(record['name']+'.png')
        if not path.exists():
            image=Image.open(io.BytesIO(download(record['url'])))
            image.save(path)
        with Image.open(path) as image:
            record['size']=list(image.size)
        return record

    with ThreadPoolExecutor(max_workers=8) as pool:
        records=list(pool.map(fetch,records))
    (DEST/'sources.json').write_text(json.dumps(records,ensure_ascii=False,indent=2),encoding='utf-8')
    sheet=Image.new('RGB',(600,((len(records)+4)//5)*140),'#202020')
    draw=ImageDraw.Draw(sheet)
    for i,record in enumerate(records):
        image=Image.open(DEST/(record['name']+'.png')).convert('RGBA')
        image.thumbnail((96,96),Image.Resampling.LANCZOS)
        x,y=(i%5)*120,(i//5)*140
        sheet.paste(image,(x+(120-image.width)//2,y),image)
        draw.text((x+3,y+100),record['name'].replace('control_','cc_'),fill='white')
    sheet.save(ROOT/'output/watchface/icon-sources.png')
    print('\n'.join(f"{r['name']} {r['size']}" for r in records))


if __name__=='__main__': main()
