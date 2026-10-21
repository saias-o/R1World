// The arrival: from Go on the globe to the player's feet on the ground.
//
// Twenty seconds, as a skydive. For ten the menu globe flies to the place
// chosen; then its satellite picture fades into the world it was showing, the
// player falling through it; at twenty he opens his parachute fifty metres
// above the ground he chose, and flies it down himself. He steers from the
// moment the world shows, before the canopy too.
//
// This file is the rules of that flight, kept out of the frame so the tests
// can hold them: the globe's path, the clock that waits for the world, the
// fall and the canopy. The game draws what these decide (native/world.cpp);
// nothing here touches the engine.
//
// Headings are the game's: degrees clockwise from north. Velocities are
// metres per second east and north.
#pragma once

#include <algorithm>
#include <cmath>

namespace r1::arrival {

constexpr double kPi = 3.14159265358979323846;
constexpr double kRadians = kPi / 180.;

// ── the timeline, in seconds after Go ───────────────────────────────────────

constexpr double kZoomSeconds = 10.;    // the globe flies down to the drop zone
constexpr double kFadeSeconds = 12.5;   // ...and has faded into the world
constexpr double kSwoopSeconds = 15.;   // the camera has come down behind him
constexpr double kDeploySeconds = 20.;  // the parachute opens
constexpr double kDeployHeight = 50.;   // metres above the ground chosen

// ── the fall ────────────────────────────────────────────────────────────────
//
// Belly to earth a body falls at about 55 m/s, 200 km/h. Tracking -- arms
// back, flying forward -- trades some of it for distance; it is how he slows
// in the last seconds before the canopy opens.
constexpr double kTerminal = 56.;
constexpr double kTrackFrom = 17.;
constexpr double kOpeningSpeed = 32.;

inline double fallSpeed(double t) {
    if (t <= kTrackFrom) return kTerminal;
    const double u = std::clamp((t - kTrackFrom) / (kDeploySeconds - kTrackFrom), 0., 1.);
    return kTerminal + (kOpeningSpeed - kTerminal) * u;
}

// Metres above the ground chosen at second `t`: still before the world shows,
// kDeployHeight when the canopy opens. The fall is a function of time, never
// of the keys: steering moves him across, the canopy still opens at twenty.
inline double fallHeight(double t) {
    t = std::clamp(t, kZoomSeconds, kDeploySeconds);
    if (t >= kTrackFrom) return kDeployHeight + (fallSpeed(t) + kOpeningSpeed) * .5 * (kDeploySeconds - t);
    return kDeployHeight + (kTerminal + kOpeningSpeed) * .5 * (kDeploySeconds - kTrackFrom) + kTerminal * (kTrackFrom - t);
}

// How high above what is under him the canopy must open, falling at `speed`:
// kDeployHeight at the speed it is planned to open at, higher the faster he
// still falls -- over a hill or a tower the fall reaches it early -- so that
// the wing always has the height it needs to bite.
inline double openingHeight(double speed) { return kDeployHeight + std::max(0., speed - kOpeningSpeed) * 1.5; }

// While the globe fades, the world's camera is the globe's: straight down,
// this far above him, so the picture going is the picture coming.
constexpr double kCameraAbove = 22.;
// Where the fall begins: this far short of the opening point, so that he flies
// to it -- and the canopy then this far on to the ground chosen.
constexpr double kApproach = 200.;
constexpr double kGlideLead = 50.;

// ── the globe's flight ──────────────────────────────────────────────────────
//
// Van Wijk and Nuij's optimal path ("Smooth and efficient zooming and
// panning", 2003), the one map viewers fly: it climbs out when the place is
// far, so the ground never streams past faster than the eye follows, and
// descends onto it. Distances are on the unit sphere, altitudes in Earth
// radii, as the globe's own camera is (menu_globe.hpp, GlobeView).
struct GlobePose { double lon = 0, lat = 0, altitude = 1; };

class GlobeFlight {
public:
    static constexpr double kRho = 1.41421356237;
    // The globe's half field of view (GlobeView::tangent): a view's width on
    // the ground is its altitude times twice this.
    static constexpr double kTangent = .414213562373095;
    static constexpr double kHighest = 3.;

    GlobeFlight() = default;
    // `arrival`: how fast the camera comes down at the end, in m/s, so that
    // the globe hands over to a fall already under way; `radius` in metres.
    GlobeFlight(GlobePose from, GlobePose to, double arrival, double radius) : from_(from), to_(to) {
        a_ = unit(from.lon, from.lat);
        b_ = unit(to.lon, to.lat);
        d_ = std::atan2(norm(cross(a_, b_)), dot(a_, b_));
        w0_ = std::max(from.altitude, 1e-9) * 2 * kTangent;
        w1_ = std::max(to.altitude, 1e-9) * 2 * kTangent;
        double slope = 0;  // d ln(width) / d(path fraction), at the end
        if (d_ < kSamePlace) {
            S_ = std::log(w1_ / w0_) / kRho;
            slope = kRho * S_;
        } else {
            const double rho2 = kRho * kRho, rho4 = rho2 * rho2;
            const double b0 = (w1_ * w1_ - w0_ * w0_ + rho4 * d_ * d_) / (2 * w0_ * rho2 * d_);
            const double b1 = (w1_ * w1_ - w0_ * w0_ - rho4 * d_ * d_) / (2 * w1_ * rho2 * d_);
            // log(sqrt(b^2+1) - b), without the cancellation for a large b.
            r0_ = -std::asinh(b0);
            r1_ = -std::asinh(b1);
            S_ = (r1_ - r0_) / kRho;
            slope = -kRho * S_ * std::tanh(r1_);
        }
        // The ease's end slope, so the descent rate at the end is `arrival`.
        const double wanted = -arrival / (std::max(to.altitude, 1e-9) * radius) * kZoomSeconds;
        endSlope_ = slope < -1e-6 ? std::clamp(wanted / slope, .02, 2.5) : .25;
    }
    // The pose `seconds` after Go, eased in and arriving at the descent rate
    // asked for.
    GlobePose at(double seconds) const {
        const double x = std::clamp(seconds / kZoomSeconds, 0., 1.);
        const double p = x * x * (3 - 2 * x) + endSlope_ * (x * x * x - x * x);
        return along(p);
    }
    // The pose at fraction `p` of the path, from 0 to 1.
    GlobePose along(double p) const {
        if (p <= 0) return from_;
        if (p >= 1) return to_;
        double u = p, w = 0;
        if (d_ < kSamePlace) w = w0_ * std::exp(kRho * p * S_);
        else {
            const double s = p * S_;
            u = w0_ / (kRho * kRho * d_) * (std::cosh(r0_) * std::tanh(kRho * s + r0_) - std::sinh(r0_));
            w = w0_ * std::cosh(r0_) / std::cosh(kRho * s + r0_);
        }
        u = std::clamp(u, 0., 1.);
        const Vec q = slerp(u);
        GlobePose out;
        out.lon = std::atan2(q.x, q.z) / kRadians;
        out.lat = std::asin(std::clamp(q.y, -1., 1.)) / kRadians;
        out.altitude = std::min(kHighest, w / (2 * kTangent));
        return out;
    }
    double arc() const { return d_; }
    double endSlope() const { return endSlope_; }

private:
    struct Vec { double x = 0, y = 0, z = 0; };
    // The globe's axes (GlobeView::direction): y north, z towards lon 0.
    static Vec unit(double lon, double lat) {
        lon *= kRadians; lat *= kRadians;
        return {std::cos(lat) * std::sin(lon), std::sin(lat), std::cos(lat) * std::cos(lon)};
    }
    static double dot(Vec a, Vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    static Vec cross(Vec a, Vec b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
    static double norm(Vec a) { return std::sqrt(dot(a, a)); }
    // Under ten centimetres apart the flight is a pure zoom.
    static constexpr double kSamePlace = 1.6e-8;
    Vec slerp(double u) const {
        if (d_ < 1e-12) return a_;
        const double s = std::sin(d_), ka = std::sin((1 - u) * d_) / s, kb = std::sin(u * d_) / s;
        return {a_.x * ka + b_.x * kb, a_.y * ka + b_.y * kb, a_.z * ka + b_.z * kb};
    }
    GlobePose from_, to_;
    Vec a_, b_;
    double d_ = 0, w0_ = 1, w1_ = 1, r0_ = 0, r1_ = 0, S_ = 0, endSlope_ = .25;
};

// ── the clock that waits for the world ──────────────────────────────────────
//
// The fade must find the tiles cooked and mounted: a picture that fades into
// bare ground, then fills in, is the jump the whole arrival exists to avoid.
// So the zoom is the one part that may wait. While the world behind the globe
// is not ready, its last seconds slow smoothly to a stop at kZoomSeconds, the
// camera hanging over the drop zone; they resume, as smoothly, once it is.
// Past kZoomSeconds nothing waits again.
class Clock {
public:
    static constexpr double kHoldEase = 1.5;  // s before kZoomSeconds the slowing starts
    static constexpr double kRateEase = .3;   // s: how fast the pace changes

    double seconds = 0, rate = 1;

    // Advances by `dt` real seconds; returns the arrival's own seconds elapsed.
    double advance(double dt, bool worldReady) {
        dt = std::max(0., dt);
        const double before = seconds;
        const bool waits = !worldReady && seconds <= kZoomSeconds;
        // Slowing follows the distance left, so the stop is never abrupt;
        // resuming eases back to the real pace.
        const double target = waits ? std::clamp((kZoomSeconds - seconds) / kHoldEase, 0., 1.) : 1.;
        rate += (target - rate) * (1 - std::exp(-dt / kRateEase));
        if (waits) rate = std::min(rate, target);
        seconds += rate * dt;
        if (waits) seconds = std::min(seconds, kZoomSeconds);
        waiting_ = waits;
        return seconds - before;
    }
    // All but stopped at the end of the zoom, waiting for the world.
    bool holding() const { return waiting_ && rate < .05; }

private:
    bool waiting_ = false;
};

// ── steering ────────────────────────────────────────────────────────────────

struct Steer { double forward = 0, right = 0; };  // the keys, each -1..1
inline bool touched(const Steer& s) { return std::abs(s.forward) > 1e-3 || std::abs(s.right) > 1e-3; }
inline double bearing(double east, double north) { return std::atan2(east, north) / kRadians; }
inline double wrapDegrees(double a) { return a - 360. * std::floor((a + 180.) / 360.); }

// In free fall he flies across the ground as a tracking skydiver does: forward
// fast, backwards and sideways slowly, the air taking a second to turn him.
// Until the player touches the keys an autopilot flies him to the opening
// point, arriving as the canopy opens; the first key press hands him over for
// good. With the keys let go he falls straight, belly to earth.
class Freefall {
public:
    static constexpr double kForward = 24., kBack = 8., kSide = 12., kMost = 30.;
    static constexpr double kLag = 1.2;  // s, the air's inertia

    double east = 0, north = 0;  // horizontal velocity
    bool steered = false;

    // `goal`: metres to the opening point; `left`: seconds until it opens.
    void step(double dt, const Steer& in, double headingDeg, double goalEast, double goalNorth, double left) {
        if (dt <= 0) return;
        steered = steered || touched(in);
        double wantE = 0, wantN = 0;
        if (steered) {
            const double h = headingDeg * kRadians, f = in.forward > 0 ? kForward : kBack;
            wantE = std::sin(h) * f * in.forward + std::cos(h) * kSide * in.right;
            wantN = std::cos(h) * f * in.forward - std::sin(h) * kSide * in.right;
        } else {
            // Guidance: the steady velocity that, through the air's lag,
            // covers exactly the distance left by the time the canopy opens.
            // Held at least once the lag has the time to act.
            const double t = std::max(left, .5 * kLag), g = kLag * (1 - std::exp(-t / kLag));
            wantE = (goalEast - east * g) / (t - g);
            wantN = (goalNorth - north * g) / (t - g);
        }
        const double want = std::hypot(wantE, wantN);
        if (want > kMost) { wantE *= kMost / want; wantN *= kMost / want; }
        const double k = 1 - std::exp(-dt / kLag);
        east += (wantE - east) * k;
        north += (wantN - north) * k;
    }
    double speed() const { return std::hypot(east, north); }
    // Where his body faces: where he flies, or where the player looks.
    double facing(double headingDeg) const {
        return steered || speed() < 2. ? headingDeg : bearing(east, north);
    }
};

// ── the canopy ──────────────────────────────────────────────────────────────
//
// A docile ram-air canopy, the kind a first jump is made under: it flies
// forward at about 9 m/s and sinks at 4.5. Front risers (forward) dive it
// faster, brakes (back) slow it, toggles (left and right) turn it; it turns
// toward where the player looks too. Near the ground he flares by himself, so
// a landing is a step and never a fall.
class Canopy {
public:
    static constexpr double kInflate = 1.6;  // s from the pull to a full wing
    static constexpr double kForward = 9., kSink = 4.5;
    static constexpr double kRiserForward = 12.5, kRiserSink = 6.5;
    static constexpr double kBrakeForward = 4., kBrakeSink = 3.;
    static constexpr double kFlareHeight = 4., kFlareForward = 3., kFlareSink = 1.4;
    static constexpr double kToggleTurn = 60.;  // deg/s on the toggles
    // A sport canopy opens at three to five g; never harder than this.
    static constexpr double kMostDeceleration = 4.5 * 9.81;
    static constexpr double kMostTurn = 80.;    // deg/s toward where he looks

    double heading = 0, forward = 0, sink = 0, age = 0;
    bool steered = false;

    void open(double headingDeg, double horizontalSpeed, double fallSpeed) {
        heading = wrapDegrees(headingDeg);
        forward = horizontalSpeed;
        sink = fallSpeed;
        age = 0;
    }
    // `wanted`: the heading the player looks along (the game's yaw), turned
    // by the toggles here; `goal`: metres to the ground chosen; `height`:
    // metres above what he lands on. Returns the new `wanted`.
    double step(double dt, const Steer& in, double wanted, double goalEast, double goalNorth, double height) {
        if (dt <= 0) return wanted;
        age += dt;
        steered = steered || touched(in);
        double targetForward = kForward, targetSink = kSink;
        if (steered) {
            wanted = wrapDegrees(wanted + in.right * kToggleTurn * dt);
            if (in.forward > 0) {
                targetForward += (kRiserForward - kForward) * in.forward;
                targetSink += (kRiserSink - kSink) * in.forward;
            } else if (in.forward < 0) {
                targetForward += (kBrakeForward - kForward) * -in.forward;
                targetSink += (kBrakeSink - kSink) * -in.forward;
            }
        } else {
            // To the ground chosen, braking as it comes under him.
            const double away = std::hypot(goalEast, goalNorth);
            if (away > 3.) wanted = bearing(goalEast, goalNorth);
            const double reach = height / kSink * kForward;  // what is left to fly
            if (away < reach) targetForward = std::max(kBrakeForward, kForward * away / std::max(reach, 1.));
        }
        if (height < kFlareHeight) {
            const double flare = 1 - std::clamp(height / kFlareHeight, 0., 1.);
            targetForward += (kFlareForward - targetForward) * flare;
            targetSink += (kFlareSink - targetSink) * flare;
        }
        // The wing turns toward the heading asked for, at a wing's rate.
        const double turn = wrapDegrees(wanted - heading);
        const double most = kMostTurn * dt * inflation();
        heading = wrapDegrees(heading + std::clamp(turn, -most, most));
        // Opening, the lines stretch before the wing bites: the deceleration
        // builds over the first half second.
        const double bite = std::clamp(age / .5, 0., 1.);
        const double sinkLag = .25 + .3 * (1 - bite);
        const double change = (targetSink - sink) * (1 - std::exp(-dt * bite / sinkLag));
        sink += std::max(change, -kMostDeceleration * dt);
        forward += (targetForward - forward) * (1 - std::exp(-dt / .8));
        return wanted;
    }
    // How open the wing is, 0 packed to 1 flying.
    double inflation() const {
        const double x = std::clamp(age / kInflate, 0., 1.);
        return x * x * (3 - 2 * x);
    }
    double east() const { return std::sin(heading * kRadians) * forward; }
    double north() const { return std::cos(heading * kRadians) * forward; }
    // How far a turn banks him, degrees, for the body and the camera.
    double bank(double wanted) const { return std::clamp(wrapDegrees(wanted - heading) * .5, -25., 25.) * inflation(); }
};

}  // namespace r1::arrival
