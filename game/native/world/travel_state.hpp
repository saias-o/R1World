#pragma once

namespace r1::world {
enum class TravelMode { Foot, Swim, Car, Boat, Aircraft };
class TravelState {
public:
    TravelMode mode() const { return mode_; }
    static bool vehicle(TravelMode mode) {
        return mode==TravelMode::Car||mode==TravelMode::Boat||mode==TravelMode::Aircraft;
    }
    // Detach while the old owner is still identifiable, commit one exclusive
    // mode, then activate/attach to the new owner. Failed entry validation
    // happens before this operation and leaves possession untouched.
    template<class Leave,class Enter>
    void transition(TravelMode next,Leave&& leave,Enter&& enter) {
        const auto previous=mode_;
        leave(previous,next);
        mode_=next;
        enter(previous,next);
    }
private:
    TravelMode mode_=TravelMode::Foot;
};
}
