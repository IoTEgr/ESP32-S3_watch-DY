"""Reproducible UI glyphs and antialiased vector icons for the control center."""
from pathlib import Path
import math
from PIL import Image, ImageDraw
from generate import ROOT, HERE, font_c, image_c

N=4

def icon(name):
    im=Image.new('RGBA',(56*N,56*N)); d=ImageDraw.Draw(im)
    def line(points,w=3):
        p=[(round(x*N),round(y*N)) for x,y in points]
        d.line(p,fill='white',width=round(w*N),joint='curve')
        r=w*N/2
        for x,y in (p[0],p[-1]):d.ellipse((x-r,y-r,x+r,y+r),fill='white')
    def ellipse(box,w=3,fill=None):d.ellipse(tuple(int(x*N) for x in box),outline='white',width=w*N,fill=fill)
    def rect(box,r=4,w=3,fill=None):d.rounded_rectangle(tuple(int(x*N) for x in box),r*N,outline='white',width=w*N,fill=fill)
    def poly(p):d.polygon([(int(x*N),int(y*N)) for x,y in p],fill='white')
    def arc(cx,cy,r,a,b,w=3):
        line([(cx+math.cos(math.radians(t))*r,cy+math.sin(math.radians(t))*r) for t in range(a,b+1,2)],w)
    if name=='wifi':
        for r in [9,18,27]:arc(28,43,r,225,315,4)
        ellipse((25,40,31,46),fill='white')
    elif name=='battery':
        rect((7,17,46,40),5);rect((47,24,50,33),1,1,'white');rect((12,22,38,35),2,1,'white')
    elif name=='mute':
        line([(13,37),(16,32),(16,22),(19,16),(25,13),(31,13),(37,17),(40,24),(40,32),(43,37),(13,37)])
        arc(28,39,6,0,180);line([(9,8),(47,46)],4)
    elif name=='torch':
        poly([(16,10),(40,10),(38,19),(33,26),(33,46),(23,46),(23,26),(18,19)])
        d.rectangle((19*N,14*N,37*N,17*N),fill=(0,0,0,0))
        ellipse((26,29,30,33),1,fill=(0,0,0,0))
    elif name=='brightness':
        ellipse((18,18,38,38),3)
        for t in range(0,360,45):
            a=math.radians(t);line([(28+math.cos(a)*17,28+math.sin(a)*17),(28+math.cos(a)*23,28+math.sin(a)*23)],3)
    elif name=='volume':
        poly([(8,22),(17,22),(28,13),(28,43),(17,34),(8,34)])
        arc(27,28,12,-55,55);arc(27,28,21,-55,55)
    elif name=='airplane':
        poly([(25,8),(30,8),(32,23),(49,33),(49,38),(32,33),(31,44),(37,48),(37,51),(28,48),(19,51),(19,48),(25,44),(24,33),(7,38),(7,33),(24,23)])
    elif name=='wrist':
        rect((17,13,39,43),7);line([(23,4),(23,12)]);line([(33,4),(33,12)])
        line([(23,44),(23,52)]);line([(33,44),(33,52)])
        line([(22,29),(27,24),(32,29)]);line([(27,24),(27,35)])
        arc(30,28,20,-40,40,2)
    elif name=='haptic':
        rect((20,13,36,44),4);line([(12,15),(8,21),(12,28),(8,35),(12,42)],2)
        line([(44,15),(48,21),(44,28),(48,35),(44,42)],2)
    elif name=='awake':
        rect((12,12,44,44),9);line([(28,20),(28,29),(35,29)])
        line([(6,24),(3,28),(6,32)],2);line([(50,24),(53,28),(50,32)],2)
    elif name=='lock':
        rect((14,25,42,47),6);arc(28,24,10,180,360);line([(18,24),(18,29)]);line([(38,24),(38,29)])
        ellipse((26,33,30,37),1,'white');line([(28,36),(28,40)],2)
    elif name=='timer':
        ellipse((12,15,44,47));line([(23,8),(33,8)],4);line([(28,9),(28,15)])
        line([(28,22),(28,31),(35,35)]);line([(41,14),(45,18)],3)
    elif name=='power':arc(28,29,18,-60,240,4);line([(28,7),(28,28)],4)
    elif name=='settings':
        ellipse((12,12,44,44),6);ellipse((22,22,34,34),3)
        for t in range(0,360,45):
            a=math.radians(t);line([(28+math.cos(a)*18,28+math.sin(a)*18),(28+math.cos(a)*23,28+math.sin(a)*23)],5)
    image_c(im.resize((56,56),Image.Resampling.LANCZOS),'cc_'+name,True)
    if name=='wifi':
        image_c(im.resize((21,21),Image.Resampling.LANCZOS),'cc_wifi_small',True)

if __name__=='__main__':
    sources=[ROOT/'components/watch_shell/watch_control_view.c',ROOT/'components/watch_shell/watch_control.cpp']
    text=''.join(p.read_text(encoding='utf-8') for p in sources)
    chars=''.join(chr(i) for i in range(32,127))+''.join(c for c in text if '\u3000'<=c<='\u9fff')+'×‹'
    font_c(HERE/'fonts/MiSans-Medium.ttf','font_control',22,chars)
    font_c(HERE/'fonts/MiSans-Medium.ttf','font_control_small',18,chars)
    font_c(HERE/'fonts/MiSans-Demibold.ttf','font_control_large',46,'0123456789:%-‹时间到选择时长')
    for name in ['wifi','battery','mute','torch','brightness','volume','airplane','wrist','haptic','awake','lock','timer','power','settings']:icon(name)
    from generate_watch_icons import control_icons
    control_icons()
    print('Generated 3 subset fonts and 14 transparent vector icons.')
