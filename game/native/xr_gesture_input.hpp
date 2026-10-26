#pragma once

#include "saida/xrgesture/Locomotion.hpp"
#include "saida/xrgesture/RuntimeInput.hpp"
#include "core/Log.hpp"

namespace r1 {

// Maps the optional engine plugin to walking requests. World still owns the
// geographic position, loaded-ground checks and CharacterBody collision solve.
class XrGestureInput {
public:
    explicit XrGestureInput(
        saida::xrgesture::MicrogestureMode mode = saida::xrgesture::MicrogestureMode::NativeOnly)
        : gestures_(microgestureConfig(mode)), gate_(walkingConfig()), mode_(mode) {}
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
                                 name(event.gesture), " source=", name(event.source), " t=", event.time);
        }
        reportAvailability(frame);
        const auto intent = gate_.update(frame, events, permitted);
        if (!gate_.active() || intent.discardPendingMotion)
            motion_.cancel();
        if (intent.suspended && !suspended_)
            saida::Log::info("[XRGesture] walking suspended reason=", name(intent.reason),
                             "; activation retained");
        if (!intent.suspended && suspended_ && gate_.active())
            saida::Log::info("[XRGesture] walking resumed");
        suspended_ = intent.suspended;
        if (intent.activated) {
            notice_ = "Hand movement enabled: swipe forward/back to step, left/right to turn. Tap to stop.";
            saida::Log::info("[XRGesture] walking enabled hand=",
                             gate_.hand() == saida::XRHand::Left ? "left" : "right");
        }
        if (intent.deactivated) {
            motion_.cancel();
            notice_ = "Hand movement stopped. Tap your thumb to enable it again.";
            saida::Log::info("[XRGesture] walking disabled reason=", name(intent.reason));
        }
        if (!gate_.active() && frame.focused && frame.time - lastActivationHint_ >= 1) {
            for (const auto& event : events) {
                if (event.phase == Phase::Performed && event.gesture >= gesture::SwipeLeft &&
                    event.gesture <= gesture::SwipeBackward) {
                    lastActivationHint_ = frame.time;
                    notice_ = "Tap your thumb once to enable movement, then swipe to walk or turn.";
                    saida::Log::info("[XRGesture] locomotion ignored reason=activation-required; thumb tap "
                                     "enables walking");
                    break;
                }
            }
        }
        // Game heading grows clockwise; the plugin's world +Y turn grows left.
        turnDegrees_ = -intent.turnDegrees;
        if (turnDegrees_ != 0)
            motion_.cancel();
        motion_.request(intent.step);
    }

    glm::vec3 advance(float seconds, bool keyboardWalking = false) {
        if (!gate_.active() || suspended_ || keyboardWalking) {
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
    static saida::xrgesture::MicrogestureConfig microgestureConfig(saida::xrgesture::MicrogestureMode mode) {
        saida::xrgesture::MicrogestureConfig config;
        config.mode = mode;
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
        config.mode = saida::xrgesture::LocomotionGateMode::Explicit;
        config.idleTimeoutSeconds = 0;
        return config;
    }
    void reportAvailability(const saida::xrgesture::Frame& frame) {
        int status = 0; // focus/menu suspension is not a missing capability
        if (frame.focused) {
            status = 1;
            for (const auto& hand : frame.hands) {
                if (mode_ != saida::xrgesture::MicrogestureMode::SkeletonOnly && hand.native.supported &&
                    hand.native.active)
                    status = 3;
                else if (mode_ != saida::xrgesture::MicrogestureMode::NativeOnly && hand.skeleton.active)
                    status = std::max(status, 2);
            }
        }
        if (status == status_)
            return;
        status_ = status;
        if (status == 1) {
            if (mode_ == saida::xrgesture::MicrogestureMode::NativeOnly) {
                notice_ = "Meta microgestures unavailable or hands not tracked in this VR session.";
                saida::Log::warn(
                    "[XRGesture] native Meta actions unavailable/inactive; geometric fallback disabled");
            } else {
                notice_ = "Hand movement unavailable: this VR session is not providing tracked fingers.";
                saida::Log::warn("[XRGesture] no optical hand skeleton; walking gestures unavailable");
            }
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
    bool suspended_ = false;
    double lastActivationHint_ = -1;
    saida::xrgesture::MicrogestureMode mode_;
    float turnDegrees_ = 0;
    const saida::xrgesture::RuntimeInputState* runtime_ = nullptr; // provider outlives World
};

} // namespace r1
