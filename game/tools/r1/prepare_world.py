"""Generate the world entry scene and its offline Natural Earth basemap."""
import json
import urllib.request
from datetime import datetime, timezone
from pathlib import Path

from . import external_assets, skies, solar

GAME = Path(__file__).resolve().parents[2]
URL = "https://raw.githubusercontent.com/nvkelso/natural-earth-vector/master/geojson/ne_110m_land.geojson"

# The world opens on the map, so nothing is lit until the player picks a place.
# These values are only a placeholder before the runtime script reads the
# computer's UTC clock. The scene is kept deterministic during generation.
WORLD_EPOCH = datetime(2026, 6, 21, 14, 30, tzinfo=timezone.utc)
WORLD_TURBIDITY = 2.4
# The map's own default destination, so the opening light is not a place the
# player is nowhere near.
DEFAULT_LON, DEFAULT_LAT = 2.3522, 48.8566

# Haze as an extinction per metre where no visibility is measured, and how far
# the camera draws. A clear day's 150 km (Koschmieder: 3.912 / V); `sun_cycle.js`
# carries the same density every frame, and the measured visibility replaces
# it. See `landmarks.FAR_RANGE`.
FOG_DENSITY = 0.0000261
FAR_PLANE = 5000


def basemap():
    # Natural Earth land polygons: the harbour finder's coastline
    # (gen/harbours.cpp). The menu once drew a flat map from them too; the
    # globe (native/menu_globe.cpp) replaced it.
    cache = GAME / "assets" / "world" / "land.geojson"
    cache.parent.mkdir(parents=True, exist_ok=True)
    if not cache.exists():
        with urllib.request.urlopen(URL, timeout=40) as response:
            cache.write_bytes(response.read())
    (cache.parent/"SOURCES.json").write_text(json.dumps({"map":{"source":URL,"license":"Natural Earth public domain","use":"Coastlines for harbours only; not terrain or elevation"}},indent=2),encoding="utf-8")


def main():
    (GAME/"ui").mkdir(exist_ok=True)
    basemap()
    node = lambda t,n,**kw: dict({"type":t,"name":n,"enabled":True,"children":[],"behaviours":[]},**kw)
    scene = node("Scene","R1WorldEarth")
    # No lighting value below is chosen. Direction, colour, intensity, ambient,
    # horizon and the two exposures all come out of one instant and one air
    # column (plan §2.2) -- and `sun_cycle.js` then
    # carries the same model at runtime, moving the observer with the player so
    # the sun over Sydney is Sydney's rather than Paris's.
    sun = solar.sun_state(DEFAULT_LON, DEFAULT_LAT, WORLD_EPOCH, 0.0,
                          WORLD_TURBIDITY, solar.PEAK_INTENSITY)
    sky = skies.sky_state(sun)
    scene["children"] = [
        # The Sun as a lens sees it: ghosts and a diffraction star wherever it
        # is on screen, both measured against what hides it (SPEC §4.1).
        node("LightNode","Sun",groups=["sun"],lightType=0,castShadows=True,
             lensFlare=True,sunStar=True,
             color=[round(c,4) for c in sun.color],
             intensity=round(sun.intensity,4),
             direction=[round(c,6) for c in sun.direction],
             behaviours=[{"type":"ScriptBehaviour","enabled":True,
                          "script":"scripts/sun_cycle.js","hotReload":False,
                          "properties":{"anchorLon":DEFAULT_LON,"anchorLat":DEFAULT_LAT,
                                        "epochUnix":WORLD_EPOCH.timestamp(),
                                        "useSystemClock":True,
                                        "secondsPerSecond":1.0,
                                        "turbidity":WORLD_TURBIDITY,"altitude":0.0,
                                        "peakIntensity":solar.PEAK_INTENSITY}}]),
        node("Node","Player",groups=["player"],children=[
            node("Node","Body",importedFrom="assets/models/humans/player.glb",
                 transform={"position":[0,0,0],"rotation":[0,1,0,0],"scale":[1,1,1]})]),
        # The player's car, and a member of the entry scene rather than of a
        # tile: it is his, so it survives the neighbourhood he teleports out of
        # exactly as he does. Disabled until a spawn finds it a place to park
        # -- see `native/world.cpp` -- because a car standing at the scene
        # origin is a car standing in the middle of the Atlantic.
        #
        # The body carries the same 180° yaw as the player's: both kits model a
        # front facing +Z, and this world's forward is −Z.
        node("Node","Car",groups=["vehicle"],enabled=False,children=[
            node("Node","Near",importedFrom=external_assets.vehicle_model("city"),
                 transform={"position":[0,0,0],"rotation":[0,1,0,0],"scale":[1,1,1]}),
            node("Node","Far",importedFrom=external_assets.vehicle_model("city_far"),
                 transform={"position":[0,0,0],"rotation":[0,1,0,0],"scale":[1,1,1]})]),
        node("Camera","ExplorerCamera",groups=["camera"],fovDegrees=62,nearZ=.1,farZ=FAR_PLANE,priority=10,active=True),
        node("WebCanvasNode","WorldMap",groups=["world-ui"],width=1440,height=900,referenceWidth=1440,referenceHeight=900,
             scaleMode=1,mode=0,url="ui/world.html",hotReload=False,interactive=True,renderOrder=1000),
    ]
    scene["settings"] = dict(ambient=[round(c,4) for c in sun.ambient],
                 clearColor=[round(c,4) for c in sun.horizon_color],
                 giEnabled=True,giVolume=1,giQuality=0,
                 fogEnabled=True,fogColor=[round(c,4) for c in sun.horizon_color],
                 fogStart=160,fogDensity=FOG_DENSITY,
                 # Initial lighting; sun_cycle.js updates diffuse IBL and ambient.
                 iblEnabled=True,iblDiffuseIntensity=0.,iblSpecularIntensity=1.,
                 aoEnabled=True,bloomEnabled=True,bloomIntensity=.15,postProcessing=True,
                 autoExposure=True,exposureKey=.18,autoExposureMin=-1.,
                 autoExposureMax=1.5,autoExposureSpeed=1.5,
                 changeRenderingAtLoad=True,
                 # The first frame's sky, from the same series and the same
                 # rules `sun_cycle.js` then applies every frame (`r1/skies.py`).
                 skyboxTexture=sky.texture,skyboxBlendTexture=sky.blend_texture,
                 skyboxBlend=round(sky.blend,4),skyboxExposure=round(sky.exposure,4),
                 skyboxRotation=round(sky.rotation,6),skyboxBlendRotation=round(sky.blend_rotation,6),
                 skySunDirection=[round(c,6) for c in sky.sun_direction],
                 skySunColor=[round(c,4) for c in sky.sun_color],
                 skySunSize=skies.SUN_ANGULAR_RADIUS)
    (GAME/"scenes"/"earth.scene").write_text(json.dumps({"schema":2,"version":2,"scene":scene},indent=2),encoding="utf-8")


if __name__ == "__main__":
    main()
