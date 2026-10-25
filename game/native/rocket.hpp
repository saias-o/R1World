#pragma once
#include <algorithm>
#include <cmath>

namespace r1::rocket {
constexpr int kProjectiles=12, kExplosions=6;
constexpr float kSpeed=85, kLifetime=6, kBlastRadius=14, kDestroyRadius=6;
inline double damage(double distance) {
    if (!std::isfinite(distance) || distance >= kBlastRadius) return 0;
    return 180 * std::clamp(1.0-distance/kBlastRadius,0.0,1.0);
}
// Continuous observation before launch, then a real reload. Losing sight
// abandons the lock; entering a new vehicle never inherits a finished lock.
struct FireControl {
    double lock=0, cooldown=2;
    void update(double dt,bool eligible) {
        if (!(dt>0) || !std::isfinite(dt)) return;
        cooldown=std::max(0.,cooldown-dt);
        lock=eligible?std::min(1.4,lock+dt):0;
    }
    bool ready()const{return lock>=1.4&&cooldown<=0;}
    void fired(bool aerial){lock=0;cooldown=aerial?5.5:7.;}
};
inline bool authorized(double wanted,bool engaged,bool visible,double distance,bool aerial) {
    return wanted>=60&&engaged&&visible&&distance>24&&distance<(aerial?230:115);
}
// Interception time from dot products: |relativePosition + velocity*t| = speed*t.
// No valid intercept (target escaping faster) means no speculative lead.
inline double intercept(double distanceSquared,double positionDotVelocity,double velocitySquared) {
    const double a=velocitySquared-kSpeed*kSpeed,b=2*positionDotVelocity;
    if(std::abs(a)<1e-6)return b<0?std::clamp(-distanceSquared/b,0.,2.):0.;
    const double discriminant=b*b-4*a*distanceSquared;
    if(discriminant<0)return 0;
    const double root=std::sqrt(discriminant),t0=(-b-root)/(2*a),t1=(-b+root)/(2*a);
    const double t=t0>0&&(t1<=0||t0<t1)?t0:t1;
    return t>0?std::min(t,2.):0.;
}
}
