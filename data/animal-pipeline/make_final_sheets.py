import json
from pathlib import Path
from PIL import Image,ImageDraw,ImageFont
ROOT=Path(__file__).resolve().parents[2]
QA=ROOT/'data/animal-pipeline/review'
FINAL=QA/'final-renders'
names=json.loads((FINAL/'rendered.json').read_text(encoding='utf-8'))
catalog=json.loads((ROOT/'game/assets/animals/manifest.json').read_text(encoding='utf-8'))
labels={a['id']:a['label'] for a in catalog['assets']}
try:
    font=ImageFont.truetype('C:/Windows/Fonts/arial.ttf',16)
except OSError:
    font=ImageFont.load_default()
for start in range(0,len(names),16):
    subset=names[start:start+16]
    im=Image.new('RGB',(1600,182*((len(subset)+3)//4)),'#25282b')
    d=ImageDraw.Draw(im)
    for i,name in enumerate(subset):
        x,y=(i%4)*400,(i//4)*182
        for j,suffix in enumerate(['','-lod1']):
            tile=Image.open(FINAL/(name+suffix+'.png')).convert('RGB').resize((200,160),Image.Resampling.LANCZOS)
            im.paste(tile,(x+j*200,y))
        d.text((x+4,y+161),name+' | LOD0 / LOD1',font=font,fill='white')
    im.save(FINAL/f'final-review-{start//16+1:02}.jpg',quality=93)
im=Image.new('RGB',(1440,212*((len(names)+5)//6)+46),'#25282b')
d=ImageDraw.Draw(im)
d.text((14,12),'R1World — animaux riggés : sélection de GLB finaux',font=font,fill='white')
for i,name in enumerate(names):
    x,y=(i%6)*240,46+(i//6)*212
    tile=Image.open(FINAL/(name+'.png')).convert('RGB').resize((240,192),Image.Resampling.LANCZOS)
    im.paste(tile,(x,y))
    d.text((x+5,y+193),labels[name],font=font,fill='white')
im.save(ROOT/'game/assets/animals/preview.jpg',quality=92)
print('Final pairs reviewed:',len(names))
