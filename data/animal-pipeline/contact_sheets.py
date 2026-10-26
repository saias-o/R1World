import json
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont
ROOT=Path(__file__).resolve().parents[2]
QA=ROOT/'data/animal-pipeline/review'
records=[]
for p in sorted(QA.glob('*.json')):
    if p.name.startswith(('results','oga_','gltf','smoke')):
        continue
    r=json.loads(p.read_text())
    if isinstance(r,dict) and r.get('status')=='prepared':
        records.append(r)
for start in range(0,len(records),24):
    subset=records[start:start+24]
    sheet=Image.new('RGB',(1600,178*((len(subset)+3)//4)), '#25282b')
    draw=ImageDraw.Draw(sheet)
    for i,r in enumerate(subset):
        x,y=(i%4)*400,(i//4)*178
        for j,suffix in enumerate(['','-lod1']):
            im=Image.open(QA/(r['id']+suffix+'.png')).convert('RGB').resize((200,160),Image.Resampling.LANCZOS)
            sheet.paste(im,(x+j*200,y))
        draw.text((x+4,y+161),r['id']+'  |  LOD0 / LOD1',fill='white')
    sheet.save(QA/f'contact-{start//24+1:02}.jpg',quality=90)
print('Contact sheets:',(len(records)+23)//24,'animals:',len(records))
