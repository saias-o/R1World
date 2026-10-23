// The rules of a fight, kept out of the frame so the tests can hold them:
// hit points, what each weapon does and how fast, the rhythm of a punch, and
// how hard a car or a fall hits a body. The game draws what these decide
// (native/world.cpp: ragdolls, arms, sounds, HUD); nothing here touches the
// engine.
//
// The numbers are a game's, in the spirit of GTA III -- three punches put a
// passer-by down, a pistol round to the head ends it, a car at town speed
// throws someone over its bonnet -- not a model of injury.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace r1::combat {

struct Health {
    double hp = 100, max = 100;
    double shield = 0;  // seconds of invincibility left (native/cheats.hpp, PAINKILLER)
    bool dead() const { return hp <= 0; }
    bool invincible() const { return shield > 0; }
    // A second code while one runs gives the full time again, never less.
    void protect(double seconds) { shield = std::max(shield, seconds); }
    void tick(double dt) { shield = std::max(0.0, shield - std::max(0.0, dt)); }
    // Damage taken, and whether this was the blow that killed. Invincible,
    // nothing is taken.
    bool take(double amount) {
        if (dead() || invincible() || !(amount > 0)) return false;
        hp = std::max(0.0, hp - amount);
        return dead();
    }
    void heal(double amount) { if (!dead()) hp = std::min(max, hp + std::max(0.0, amount)); }
    void restore() { hp = max; }
    double fraction() const { return std::clamp(hp / max, 0.0, 1.0); }
};

enum class Weapon : uint8_t { Fists, Pistol };

struct WeaponSpec {
    const char* name;
    double damage;          // a body hit
    double headMultiplier;  // a head hit
    double range;           // metres
    double interval;        // seconds between two attacks
    double spread;          // radians of aim error at rest
    double reloadSeconds;
    int magazine;           // rounds per magazine; 0 for fists
    double impulse;         // N.s handed to whatever is hit
};

// A Glock-class pistol: 9 mm, 17 rounds; the impulse is a game's, enough to
// spin a body already falling, far above what a round really transfers.
constexpr WeaponSpec kFists{"Fists", 14, 1.4, 1.5, 0.38, 0, 0, 0, 55};
constexpr WeaponSpec kPistol{"Pistol", 34, 3.2, 80, 0.2, 0.012, 1.4, 17, 40};
inline const WeaponSpec& spec(Weapon w) { return w == Weapon::Pistol ? kPistol : kFists; }

// What the player carries: the weapon in hand, the rounds, and the clock of
// the next attack and of a reload under way.
class Arsenal {
public:
    enum class Trigger { None, Fired, Empty, Reloading };

    Weapon held() const { return held_; }
    int loaded() const { return loaded_; }
    int reserve() const { return reserve_; }
    bool reloading() const { return reloading_ > 0; }
    double reloadProgress() const {
        return reloading_ > 0 ? 1 - reloading_ / spec(Weapon::Pistol).reloadSeconds : 1;
    }

    // Changing weapon abandons a reload, never the rounds.
    void select(Weapon w) {
        if (w == held_) return;
        held_ = w;
        reloading_ = 0;
        cooldown_ = std::max(cooldown_, 0.25);
    }
    void give(int rounds) { reserve_ = std::min(kMostReserve, reserve_ + std::max(0, rounds)); }
    void disarm() { held_=Weapon::Fists; loaded_=reserve_=0; cooldown_=reloading_=0; }

    // The trigger pulled now. A pistol with an empty magazine clicks once
    // and reloads on its own, as long as any round is left.
    Trigger pull() {
        if (cooldown_ > 0 || reloading_ > 0) return reloading_ > 0 ? Trigger::Reloading : Trigger::None;
        cooldown_ = spec(held_).interval;
        if (held_ == Weapon::Fists) return Trigger::Fired;
        if (loaded_ == 0) {
            startReload();
            return Trigger::Empty;
        }
        --loaded_;
        if (loaded_ == 0) startReload();
        return Trigger::Fired;
    }
    bool reload() {
        if (held_ != Weapon::Pistol || reloading_ > 0 || loaded_ == kPistol.magazine || reserve_ == 0) return false;
        startReload();
        return reloading_ > 0;
    }
    void update(double dt) {
        cooldown_ = std::max(0.0, cooldown_ - dt);
        if (reloading_ <= 0) return;
        reloading_ -= dt;
        if (reloading_ > 0) return;
        reloading_ = 0;
        const int taken = std::min(reserve_, kPistol.magazine - loaded_);
        loaded_ += taken;
        reserve_ -= taken;
    }

    static constexpr int kMostReserve = 170;

private:
    void startReload() {
        if (reserve_ > 0 && loaded_ < kPistol.magazine) reloading_ = kPistol.reloadSeconds;
    }
    Weapon held_ = Weapon::Fists;
    int loaded_ = kPistol.magazine, reserve_ = kPistol.magazine * 4;
    double cooldown_ = 0, reloading_ = 0;
};

// A punch: the arm goes out (kReach), lands (kStrike), comes back
// (kRecover). Punches thrown within kComboWindow of the last alternate hands;
// the third of a run is a hook that hits harder and puts a body down.
class Punch {
public:
    static constexpr double kReach = 0.11, kStrike = 0.13, kRecover = 0.36, kComboWindow = 0.75;
    static constexpr int kComboLength = 3;
    static constexpr double kHookMultiplier = 1.8;

    // Starts a punch at `now`; false while one is still out.
    bool throwAt(double now) {
        if (active()) return false;
        combo_ = (now - last_ <= kComboWindow + kRecover && combo_ < kComboLength) ? combo_ + 1 : 1;
        hand_ = combo_ % 2;  // 1: right lead, 0: left
        last_ = now;
        t_ = 0;
        struck_ = false;
        return true;
    }
    // Advances; true on the update in which the fist lands.
    bool update(double dt) {
        if (!active()) return false;
        t_ += dt;
        if (!struck_ && t_ >= kStrike) { struck_ = true; return true; }
        return false;
    }
    bool active() const { return t_ >= 0 && t_ < kRecover; }
    bool hook() const { return combo_ == kComboLength; }
    int combo() const { return combo_; }
    bool rightHand() const { return hand_ == 1; }
    // How far the arm is out, 0..1: quick out, a beat held, eased back.
    double extension() const { return active() ? extensionAt(t_) : 0; }
    // The same, `t` seconds into any punch (a passer-by's included).
    static double extensionAt(double t) {
        if (t < 0 || t >= kRecover) return 0;
        if (t < kReach) return t / kReach;
        if (t < kStrike + 0.04) return 1;
        return std::max(0.0, 1 - (t - kStrike - 0.04) / (kRecover - kStrike - 0.04));
    }
    double damage() const { return kFists.damage * (hook() ? kHookMultiplier : 1); }

private:
    double t_ = -1, last_ = -1e9;
    int combo_ = 0, hand_ = 0;
    bool struck_ = false;
};

// A car meeting a body at `speed` m/s (closing, along the car's travel).
struct CarHit {
    bool knockDown = false;  // off their feet, into the ragdoll
    double damage = 0;
};
inline CarHit carImpact(double speed) {
    speed = std::abs(speed);
    // Below a brisk walk a bumper nudges; from about 10 km/h it takes the
    // legs; past 50 km/h it kills.
    if (speed < 2.5) return {false, 0};
    const double damage = speed >= 14 ? 100 : std::min(100.0, 6.0 * (speed - 1.5) + 0.35 * speed * speed);
    return {true, damage};
}

// Landing from a fall at `speed` m/s downward, under the walk's arcade
// gravity of 22 m/s2 (native/world.cpp): a jump lands at 8.1 m/s and a
// two-and-a-half-metre drop at 10.5, both nothing; about nine metres (20 m/s)
// kills.
inline double fallDamage(double speed) {
    speed = std::abs(speed);
    constexpr double kHarmless = 10.5, kLethal = 20.0;
    if (speed <= kHarmless) return 0;
    return std::min(100.0, 100.0 * (speed - kHarmless) / (kLethal - kHarmless));
}

// Below this fraction of their health, a passer-by hit is knocked down
// rather than staggered.
constexpr double kKnockDownBelow = 0.45;
// Seconds a knocked-down passer-by lies before getting up, once still.
constexpr double kLieSeconds = 2.2;
// Seconds a body that got up takes to stand (RagdollBehaviour::blendOut).
constexpr double kStandSeconds = 0.9;
constexpr double kLongestTumble = 6;
// The player, down and still, before he gets up; dead, before he wakes.
constexpr double kPlayerLieSeconds = 1.4, kWastedSeconds = 4.5;
// How far a shot or a fight carries: everyone this near runs.
constexpr double kGunshotHeard = 55, kFightSeen = 14;

}  // namespace r1::combat
