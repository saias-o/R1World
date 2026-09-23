"""Survey-first vegetation, bounded by OSM areas and tile ownership.
Point positions/heights are observations; rows and area distributions are inferred.
No automatic trees on lawns, cropland, playing fields, water or bare desert.
"""
import math
import json
from functools import lru_cache
from collections import Counter
from .atlas import seeded
from .streets import width, length_tag
from .street_surfaces import Polygon, LineString, unary_union
from shapely.geometry import Point, box
from .world_tiles import tile_at

REVISION=2
TILE_NATURE_BUDGET=320
MODEL_DIR='assets/models/external/nature_cards'


def species(tags,lon,lat,identifier,forest=False):
    """Morphology from botanical tags first, then a conservative regional fallback."""
    genus=(tags.get('genus','')+' '+tags.get('species','')).lower()
    if 'pinus' in genus:return 'pine_sapling','botanical-tag'
    if any(s in genus for s in ('abies','picea','cedrus')):return 'fir_sapling','botanical-tag'
    if 'aloidendron' in genus or 'aloe dichotoma' in genus:return 'quiver_tree','botanical-tag'
    if tags.get('leaf_type')=='needleleaved':return 'pine_sapling','leaf-type'
    if tags.get('leaf_type')=='broadleaved' or genus.strip():return 'broadleaf','generic-morphology-for-tag'
    if forest and (abs(lat)>57 or (5<lon<16 and 44<lat<48)):
        return ('fir_sapling','pine_sapling')[seeded(identifier,17).randrange(2)],'regional-inference'
    # Quiver trees are southern African dryland plants, not a synonym for heat.
    if 13<lon<23 and -32<lat<-20:return 'quiver_tree','regional-inference'
    return 'broadleaf','regional-inference'


@lru_cache(maxsize=16)
def source_height(root,model):
    path=root/(MODEL_DIR+'/'+model+'.source.json')
    return json.loads(path.read_text(encoding='utf-8'))['height']


def plan_nature(osm,tile,anchor,ground,game_root,budget=TILE_NATURE_BUDGET):
    bounds=tile.bounds
    def xy(lo,la):
        p=anchor.geodetic_to_engine(lo,la,0);return p[0],p[2]
    sw=xy(bounds.west,bounds.south);ne=xy(bounds.east,bounds.north)
    region=box(min(sw[0],ne[0]),min(sw[1],ne[1]),max(sw[0],ne[0]),max(sw[1],ne[1]))
    blocks=[]
    for way in osm.buildings:
        if len(way.points)>=4:blocks.append(Polygon([xy(*p) for p in way.points]).buffer(.6))
    for way in osm.roads:
        if way.tags.get('bridge') not in (None,'no') or way.tags.get('tunnel') not in (None,'no'):continue
        blocks.append(LineString([xy(*p) for p in way.points]).buffer(width(way.tags)/2+.7))
    for way in osm.landcover:
        if way.tags.get('natural')=='water' or 'water' in way.tags or way.tags.get('leisure')=='pitch':
            blocks.append(Polygon([xy(*p) for p in way.points]).buffer(.5))
    blocked=unary_union(blocks)
    candidates=[]
    for feature in osm.features:
        if feature.tags.get('natural')=='tree' and tile_at(feature.lon,feature.lat)==tile:
            candidates.append((0,feature.osm_id,feature.lon,feature.lat,feature.tags,'osm-point',False))
    # Rows are mapped lines; spacing is inferred unless tree count is tagged.
    for way in getattr(osm,'tree_rows',()):
        line=LineString([xy(*p) for p in way.points])
        if line.length<1:continue
        count=max(1,min(2000,int(length_tag(way.tags.get('tree_count'),line.length/8))))
        for i in range(count):
            p=line.interpolate((i+.5)*line.length/count)
            lo,la,_=anchor.engine_to_geodetic(p.x,0,p.y)
            if tile_at(lo,la)==tile:candidates.append((1,-(abs(way.osm_id)*4096+i),lo,la,way.tags,'osm-row-inferred-spacing',False))
    for way in sorted(osm.vegetation_areas,key=lambda w:w.osm_id):
        tags=way.tags
        forest=tags.get('natural')=='wood' or tags.get('landuse')=='forest'
        scrub=tags.get('natural') in ('scrub','shrubbery')
        park=tags.get('leisure') in ('park','garden')
        orchard=tags.get('landuse')=='orchard'
        herb=tags.get('landuse') in ('grass','meadow') or tags.get('natural') in ('grassland','wetland')
        if not(forest or scrub or park or orchard or herb):continue
        polygon=Polygon([xy(*p) for p in way.points]).buffer(0).intersection(region).difference(blocked)
        if polygon.is_empty:continue
        spacing=7 if herb else 12 if forest else 8 if scrub else 24 if park else 9
        x0,z0,x1,z1=polygon.bounds
        # A deterministic stratified distribution, not a uniform planted grid.
        for ix in range(math.floor(x0/spacing),math.ceil(x1/spacing)):
            for iz in range(math.floor(z0/spacing),math.ceil(z1/spacing)):
                ident=-(abs(way.osm_id)*1000003+ix*73856093+iz*19349663)
                rng=seeded(ident,0x4E4154)
                x=(ix+.2+rng.random()*.6)*spacing;z=(iz+.2+rng.random()*.6)*spacing
                if not polygon.contains(Point(x,z)):continue
                lo,la,_=anchor.engine_to_geodetic(x,0,z)
                if tile_at(lo,la)!=tile:continue
                inferred=dict(tags)
                if scrub:inferred['r1:shrub']='yes'
                if herb:inferred['r1:grass']='yes'
                candidates.append((3 if herb else 2,ident,lo,la,inferred,'osm-area-inferred-density',forest))
    # Surveyed trees always win over inferred fill; hash distributes capped
    # populations throughout the tile instead of keeping one corner of a forest.
    candidates.sort(key=lambda c:(c[0],seeded(c[1],99).random(),c[1]))
    nodes=[];occupied={};sources=Counter();models=Counter();rejected=0;budget_dropped=0;trees=0;grasses=0;heights=0;reasons=Counter()
    for rank,ident,lo,la,tags,source,forest in candidates:
        x,z=xy(lo,la);key=(math.floor(x/3),math.floor(z/3))
        near=[p for dx in (-1,0,1) for dz in (-1,0,1) for p in occupied.get((key[0]+dx,key[1]+dz),())]
        if any(math.hypot(x-a,z-b)<3 for a,b in near):rejected+=1;continue
        # Roads exclude inferred planting; surveyed trees may be mapped on a
        # median, so retain that observation unless inside a mapped building.
        if rank and blocked.contains(Point(x,z)):rejected+=1;continue
        grass=tags.get('r1:grass')=='yes'
        if (grasses>=160 if grass else trees>=budget):budget_dropped+=1;continue
        occupied.setdefault(key,[]).append((x,z))
        model,reason=species(tags,lo,la,ident,forest)
        if model=='broadleaf' and abs(la)>=30:model='urban_tree'
        if grass:
            arid=(-18<lo<60 and 18<la<32) or (13<lo<23 and -32<la<-20) or (115<lo<140 and -30<la<-20)
            model='grass_tall' if tags.get('natural')=='wetland' else 'grass_dry' if arid else 'grass_fresh'
        rng=seeded(ident,81)
        nominal=(1.1 if model=='grass_tall' else .45) if grass else 2 if tags.get('r1:shrub') else 13 if forest else 10 if abs(la)<30 else 9
        observed=length_tag(tags.get('height'),0)
        height=observed if observed>0 else nominal*(.8+rng.random()*.4)
        scale=height
        heights+=int(observed>0);reasons[reason]+=1
        if grass:grasses+=1
        else:trees+=1
        if grass:
            children=[{'type':'Node','name':'Near','importedFrom':f'assets/models/external/nature_selected/{model}.glb'}]
        else:
            children=[{'type':'Node','name':'Far','importedFrom':f'{MODEL_DIR}/{model}.glb',
                       'transform':{'scale':[1/source_height(game_root,model)]*3}}]
            if model=='urban_tree':children.append({'type':'Node','name':'Near','importedFrom':'assets/models/external/nature_selected/urban_tree.glb'})
        yaw=seeded(ident,71).random()*math.tau
        nodes.append({'type':'Node','name':f'Nature {source} {ident}','enabled':True,'groups':['vegetation','grass' if grass else 'tree'],'children':children,
                      'transform':{'position':list(ground(lo,la)),'rotation':[0,math.sin(yaw/2),0,math.cos(yaw/2)],'scale':[scale]*3}})
        sources[source]+=1;models[model]+=1
    return nodes,{'revision':REVISION,'placed':len(nodes),'trees':trees,'grassTufts':grasses,'heightsMeasured':heights,'modelSelection':dict(reasons),'bySource':dict(sources),'byModel':dict(models),
                  'rejectedOverlap':rejected,'droppedForBudget':budget_dropped,'budget':budget,
                  'representation':'layered cards baked from original CC0 scans',
                  'speciesPolicy':'botanical/leaf tags then approximate regional morphology'}
