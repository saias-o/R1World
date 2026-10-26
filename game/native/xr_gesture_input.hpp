#pragma once

#include "saida/xrgesture/Locomotion.hpp"
#include "saida/xrgesture/RuntimeInput.hpp"
#include "core/Log.hpp"

namespace r1 {

// Maps the optional engine plugin to walking requests. World still owns the
// geographic position, loaded-ground checks and CharacterBody collision solve.
class XrGestureInput {
public:
    XrGestureInput() : gestures_(microgestureConfig()), gate_(walkingConfig()) {}
    void setRuntimeState(const saida::xrgesture::RuntimeInputState& state) { runtime_ = &state; }

    void sample(double monotonicSeconds, bool permitted) {
        if (runtime_)
            submit(saida::xrgesture::captureInput(monotonicSeconds, *runtime_), permitted);
    }

    void submit(saida::xrgesture::Frame frame, bool permitted) {
        using namespace saida::xrgesture;
        notice_ = nullptr;
        turnDegrees_ = 0;
        frame.focused = frame.focused && permitted;
        const auto events = gestures_.update(frame);
        for (const auto& event : events) {
            if (event.phase == Phase::Performed && event.gesture <= gesture::ThumbTap)
                saida::Log::info("[XRGesture] ", event.hand == saida::XRHand::Left ? "left " : "right ",
                                 name(event.gesture), " source=", name(event.source));
        }
        reportAvailability(frame);
        const auto intent = gate_.update(frame, events, permitted);
        if (!gate_.active())
            motion_.cancel();
        if (intent.activated) {
            notice_ = "Hand movement enabled: swipe forward/back to step, left/right to turn. Tap to stop.";
            saida::Log::info("[XRGesture] walking enabled");
        }
        if (intent.deactivated) {
            motion_.cancel();
            notice_ = "Hand movement stopped.";
            saida::Log::info("[XRGesture] walking disabled");
        }
        // Game heading grows clockwise; the plugin's world +Y turn grows left.
        turnDegrees_ = -intent.turnDegrees;
        if (turnDegrees_ != 0)
            motion_.cancel();
        motion_.request(intent.step);
    }

    glm::vec3 advance(float seconds, bool keyboardWalking = false) {
        if (!gate_.active() || keyboardWalking) {
            motion_.cancel();
            return glm::vec3(0);
        }
        return motion_.advance(seconds);
    }
    void rejectStep() { motion_.cancel(); }
    float consumeTurnDegrees() {
        const float result = turnDegrees_;
        turnDegrees_ = 0;
        return result;
    }
    const char* notice() const { return notice_; }
    bool active() const { return gate_.active(); }

private:
    static saida::xrgesture::MicrogestureConfig microgestureConfig() {
        saida::xrgesture::MicrogestureConfig config;
        config.neutralSeconds = .04;
        config.minimumContactSeconds = .02;
        config.cooldownSeconds = .06;
        config.contactMargin = .010f;
        config.swipeDistance = .004f;
        config.tapTravel = .003f;
        return config;
    }
    static saida::xrgesture::LocomotionConfig walkingConfig() {
        saida::xrgesture::LocomotionConfig config;
        config.lateral = saida::xrgesture::LateralAction::SnapTurn;
        config.snapTurnDegrees = 15;
        return config;
    }
    void reportAvailability(const saida::xrgesture::Frame& frame) {
        int status = 0; // focus/menu suspension is not a missing capability
        if (frame.focused) {
            status = 1;
            for (const auto& hand : frame.hands) {
                if (!hand.skeleton.active)
                    continue;
                status = std::max(status, hand.native.supported && hand.native.active ? 3 : 2);
            }
        }
        if (status == status_)
            return;
        status_ = status;
        if (status == 1) {
            notice_ = "Hand movement unavailable: this VR session is not providing tracked fingers.";
            saida::Log::warn("[XRGesture] no optical hand skeleton; walking gestures unavailable");
        } else if (status == 2) {
            notice_ =
                "Hand tracking ready. Tap your thumb on the side of your index finger to enable movement.";
            saida::Log::info(
                "[XRGesture] source=skeleton-heuristic; native Meta actions unavailable or inactive");
        } else if (status == 3) {
            notice_ =
                "Hand tracking ready. Tap your thumb on the side of your index finger to enable movement.";
            saida::Log::info("[XRGesture] source=meta-openxr; native completed gestures available");
        }
    }

    saida::xrgesture::GestureSystem gestures_;
    saida::xrgesture::LocomotionGate gate_;
    saida::xrgesture::StepMotion motion_;
    const char* notice_ = nullptr;
    int status_ = -1;
    float turnDegrees_ = 0;
    const saida::xrgesture::RuntimeInputState* runtime_ = nullptr; // provider outlives World
};

} // namespace r1
