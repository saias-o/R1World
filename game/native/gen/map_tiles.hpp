// The menu map's Web Mercator viewport. Render each source tile at its native
// 256 px resolution so zoom never magnifies a downloaded raster.
#pragma once

#include <vector>

namespace r1 {

struct MapPoint { double lon=0, lat=0; };
struct MapPixel { double x=0, y=0; };
struct MapTile { int z=0,x=0,y=0; double left=0,top=0; };

class MapView {
public:
    static constexpr int width=1344,height=560,tilePixels=256;
    static constexpr double mercatorLimit=85.0511287798066;
    MapView(double centerLon,double centerLat,int level);
    MapPixel screen(double lon,double lat) const;
    MapPoint pointAt(double x,double y) const;
    std::vector<MapTile> visible() const;
    int level() const { return level_; }
private:
    static MapPixel project(double lon,double lat,int level);
    int level_;
    MapPixel center_;
};

}  // namespace r1
