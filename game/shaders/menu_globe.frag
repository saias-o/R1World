#version 450
layout(location=0) in vec2 uv;
layout(location=0) out vec4 colour;
layout(set=0,binding=0) uniform sampler2D dayMap;
layout(set=0,binding=1) uniform sampler2D cloudMap;
layout(set=0,binding=2) uniform sampler2D nightMap;
layout(set=0,binding=3) uniform sampler2D galaxyMap;
layout(set=0,binding=4) uniform sampler2D tileMap;
layout(set=0,binding=5) uniform sampler2D tileWest;
layout(set=0,binding=6) uniform sampler2D tileEast;
layout(set=0,binding=7) uniform sampler2D tileNorth;
layout(set=0,binding=8) uniform sampler2D tileSouth;
layout(set=0,binding=9) uniform sampler2D tileNorthWest;
layout(set=0,binding=10) uniform sampler2D tileNorthEast;
layout(set=0,binding=11) uniform sampler2D tileSouthWest;
layout(set=0,binding=12) uniform sampler2D tileSouthEast;
// east.w is the globe's opacity, faded out by the arrival into the world it
// shows; north.w the strength of its vignette; outward.w the neighbour mask.
layout(push_constant) uniform Parameters {
    vec4 east,north,outward,viewport,lens,tile,sun;
} pc;
const float pi=3.14159265359;
const float tangent=.41421356237;
vec2 globeUv(vec3 p) {return vec2(atan(p.x,p.z)/(2.*pi)+.5,.5-asin(clamp(p.y,-1.,1.))/pi);}
// Web Mercator position of a globe point inside tile z, outside [0,1) when
// the point is not on it. pc.tile.yz is the view centre minus the tile corner,
// in tiles, from doubles on the CPU. Near the street a float on the unit sphere
// is only good to ~0.4 m, a pixel of a level-19 tile: the point is then taken
// relative to the view centre, where floats keep their precision.
vec2 tileCoord(vec2 coord,vec3 offset) {
    float n=exp2(pc.tile.x);
    if(pc.tile.x>12.5) {
        float cosLat=sqrt(max(1.-pc.outward.y*pc.outward.y,1e-6));
        vec2 d=vec2(dot(offset,pc.east.xyz)/cosLat,-dot(offset,pc.north.xyz)/cosLat);
        return pc.tile.yz+d/(2.*pi)*n;
    }
    float lat=(.5-coord.y)*pi;
    vec2 mercator=vec2(coord.x,(1.-asinh(tan(clamp(lat,-1.48442223,1.48442223)))/pi)*.5)*n;
    float centreLon=atan(pc.outward.x,pc.outward.z)/(2.*pi)+.5;
    float centreLat=asin(clamp(pc.outward.y,-1.,1.));
    vec2 view=vec2(centreLon,(1.-asinh(tan(clamp(centreLat,-1.48442223,1.48442223)))/pi)*.5)*n;
    vec2 d=mercator-view;d.x-=n*round(d.x/n);
    return pc.tile.yz+d;
}
bool offTile(vec2 t) {return any(lessThan(t,vec2(0)))||any(greaterThanEqual(t,vec2(1)));}
// A geographic boundary is halfway between the last texel of one image and
// the first of its neighbour. Clamping two separate images leaves a step at
// that boundary; fetch the same four texels from either side instead. Fetches
// decode sRGB before interpolation, just like the hardware linear sampler.
vec3 tileTexel(ivec2 p,ivec2 size) {
    int x=p.x<0?-1:p.x>=size.x?1:0,y=p.y<0?-1:p.y>=size.y?1:0;
    int neighbour=y==0?(x<0?0:1):x==0?(y<0?2:3):y<0?(x<0?4:5):(x<0?6:7);
    if((x==0&&y==0)||(int(pc.outward.w)&(1<<neighbour))==0)
        return texelFetch(tileMap,clamp(p,ivec2(0),size-1),0).rgb;
    p-=ivec2(x,y)*size;
    if(neighbour==0)return texelFetch(tileWest,p,0).rgb;
    if(neighbour==1)return texelFetch(tileEast,p,0).rgb;
    if(neighbour==2)return texelFetch(tileNorth,p,0).rgb;
    if(neighbour==3)return texelFetch(tileSouth,p,0).rgb;
    if(neighbour==4)return texelFetch(tileNorthWest,p,0).rgb;
    if(neighbour==5)return texelFetch(tileNorthEast,p,0).rgb;
    if(neighbour==6)return texelFetch(tileSouthWest,p,0).rgb;
    return texelFetch(tileSouthEast,p,0).rgb;
}
vec3 tileSample(vec2 t,vec2 dx,vec2 dy) {
    ivec2 size=textureSize(tileMap,0);
    // Preserve the sampler's mipmapping and anisotropy when minifying, and
    // its interior magnification. Neighbours are needed only along an edge.
    vec2 p=t*vec2(size)-.5;
    if(max(length(dx*vec2(size)),length(dy*vec2(size)))>=1.||
       (all(greaterThanEqual(p,vec2(0)))&&all(lessThan(p,vec2(size-1)))))
        return textureGrad(tileMap,t,dx,dy).rgb;
    ivec2 a=ivec2(floor(p));vec2 f=fract(p);
    return mix(mix(tileTexel(a,size),tileTexel(a+ivec2(1,0),size),f.x),
               mix(tileTexel(a+ivec2(0,1),size),tileTexel(a+ivec2(1,1),size),f.x),f.y);
}
vec2 sphere(vec3 eye,vec3 ray,float radius) {
    float b=dot(eye,ray),c=dot(eye,eye)-radius*radius,h=b*b-c;
    if(h<0.)return vec2(-1.);
    return vec2(c/(-b+sqrt(h)),-b+sqrt(h));
}
void main() {
    gl_FragDepth=1.;
    if(pc.tile.w<-.5) {colour=vec4(.0005,.001,.0025,pc.east.w);return;}
    vec2 local=(uv-pc.viewport.xy)/pc.viewport.zw;
    float aspect=pc.lens.x*pc.viewport.z/pc.viewport.w;
    vec3 cameraRay=normalize(vec3((local.x-.5)*2.*aspect*tangent,(.58-local.y)*2.*tangent,-1.));
    mat3 basis=mat3(pc.east.xyz,pc.north.xyz,pc.outward.xyz);
    vec3 ray=basis*cameraRay,eye=pc.outward.xyz*(1.+pc.lens.y);
    vec2 hit=sphere(eye,ray,1.);
    // The map is drawn as a map, unlit; imagery is surface and is lit below.
    bool imagery=pc.tile.w>1.5;
    vec2 tileUv=vec2(0);
    vec3 tileAlbedo=vec3(0);
    if(pc.tile.w>.5) {
        if(hit.x<=0.)discard;
        tileUv=tileCoord(globeUv(normalize(eye+ray*hit.x)),pc.outward.xyz*pc.lens.y+ray*hit.x);
        // Derivatives must be taken before the per-tile discard.
        vec2 dx=dFdx(tileUv),dy=dFdy(tileUv);
        if(offTile(tileUv))discard;
        tileAlbedo=tileSample(tileUv,dx,dy);
        if(!imagery){colour=vec4(tileAlbedo*.85,pc.lens.w*pc.sun.w*pc.east.w);return;}
    }
    // A real equirectangular Milky Way behind the globe. Its orientation moves
    // with the orbit camera, rather than being painted onto the screen.
    vec3 skyRay=normalize(vec3(ray.x*.82+ray.y*.57,-ray.x*.57+ray.y*.82,ray.z));
    vec3 sky=texture(galaxyMap,globeUv(skyRay)).rgb*.17+vec3(.0005,.001,.0025);
    // Earth-fixed, from the subsolar point at the world's hour: turning the
    // globe turns day and night with it.
    vec3 sun=pc.sun.xyz;
    vec3 result=sky;
    if(hit.x>0.) {
        vec3 point=eye+ray*hit.x,n=normalize(point);
        vec2 coord=globeUv(n);
        vec3 albedo=imagery?tileAlbedo:texture(dayMap,coord).rgb;
        float daylight=dot(n,sun);
        float light=smoothstep(-.18,.8,daylight);
        float night=1.-smoothstep(-.12,.15,daylight);
        vec3 city=texture(nightMap,coord).rgb;
        float cloud=texture(cloudMap,vec2(coord.x+pc.lens.z*.000009,coord.y)).r;
        float shadow=texture(cloudMap,vec2(coord.x+pc.lens.z*.000009+.0014,coord.y+.0008)).r;
        float cloudFade=1.-smoothstep(5.,35.,1./pc.lens.y);
        albedo*=1.-shadow*.20*cloudFade;
        result=albedo*(.018+light*.9);
        result+=city*vec3(1.8,1.35,.9)*night*2.2;
        float ocean=1.-smoothstep(.005,.055,albedo.r-albedo.b*.5);
        vec3 halfVector=normalize(sun-ray);
        result+=vec3(.65,.82,1.)*pow(max(dot(n,halfVector),0.),95.)*.5*ocean*light;
        result=mix(result,vec3(.012)+vec3(1.08,1.12,1.17)*max(daylight,0.),cloud*.84*cloudFade);
        float rim=pow(1.-max(dot(n,-ray),0.),3.6)*light;
        result+=vec3(.025,.19,.48)*rim;
        // A small, geographic selection marker, occluded by the far hemisphere,
        // is drawn by the RmlUi overlay using the same projection.
    }
    // Eight samples through an exponential density shell: a thin blue limb,
    // including the part beyond the solid sphere. Cloud motion is independent.
    vec2 air=sphere(eye,ray,1.026);
    if(air.y>0.) {
        float start=max(0.,air.x),end=hit.x>0.?hit.x:air.y;
        float stepSize=max(0.,end-start)/8.;
        vec3 scattering=vec3(0.);
        for(int i=0;i<8;++i) {
            vec3 p=eye+ray*(start+(float(i)+.5)*stepSize);
            float height=max(0.,length(p)-1.);
            float density=exp(-height/.0055);
            float illumination=smoothstep(-.2,.35,dot(normalize(p),sun));
            scattering+=vec3(.10,.35,.85)*density*illumination*stepSize*12.;
        }
        result+=scattering;
    }
    // Subtle edge falloff keeps the sparse controls legible without a panel.
    float vignette=1.-.28*pc.north.w*pow(length((uv-.5)*vec2(1.,.85)),1.4);
    // A tile fading in over its stand-in; the base globe is opaque.
    colour=vec4(result*vignette,(imagery?pc.sun.w:1.)*pc.east.w);
}
