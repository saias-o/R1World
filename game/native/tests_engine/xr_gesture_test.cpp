#include "xr_gesture_input.hpp"
#include <cstdio>
#include <cstdlib>

using namespace saida;
using namespace saida::xrgesture;
static int checks = 0, failures = 0;
static void check(bool ok, const char* what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", what);
    }
}

static Frame tracked() {
    Frame frame;
    frame.focused = true;
    frame.head.tracked = true;
    frame.head.position = {0, 1.7f, 0};
    auto& hand = frame.hands[1];
    hand.native.supported = hand.native.active = true;
    hand.skeleton.active = true;
    for (auto joint : {XRHandJoint::Wrist, XRHandJoint::IndexProximal, XRHandJoint::IndexIntermediate,
                       XRHandJoint::IndexDistal, XRHandJoint::IndexTip}) {
        auto& state = hand.skeleton.joints[static_cast<int>(joint)];
        state.valid = state.pose.tracked = true;
        state.radius = .004f;
    }
    hand.skeleton.joints[static_cast<int>(XRHandJoint::IndexProximal)].pose.position = {0, .06f, 0};
    hand.skeleton.joints[static_cast<int>(XRHandJoint::IndexIntermediate)].pose.position = {0, .085f, 0};
    hand.skeleton.joints[static_cast<int>(XRHandJoint::IndexDistal)].pose.position = {0, .085f, .018f};
    hand.skeleton.joints[static_cast<int>(XRHandJoint::IndexTip)].pose.position = {0, .085f, .033f};
    return frame;
}

static XRHandSkeletonState curled(XRHand side) {
    XRHandSkeletonState hand;
    hand.active = true;
    for (auto& joint : hand.joints) {
        joint.valid = joint.pose.tracked = true;
        joint.radius = .004f;
    }
    const float mirror = side == XRHand::Right ? 1.f : -1.f;
    for (int finger = 0; finger < 4; ++finger) {
        const int start = static_cast<int>(XRHandJoint::IndexProximal) + finger * 5;
        const glm::vec3 root((.0275f - finger * .018333333f) * mirror, .06f, 0);
        hand.joints[start].pose.position = root;
        hand.joints[start + 1].pose.position = root + glm::vec3(0, .025f, 0);
        hand.joints[start + 2].pose.position = hand.joints[start + 1].pose.position + glm::vec3(0, 0, .018f);
        hand.joints[start + 3].pose.position = hand.joints[start + 2].pose.position + glm::vec3(0, 0, .015f);
    }
    return hand;
}

static void sensitivityContracts() {
    for (XRHand side : {XRHand::Left, XRHand::Right}) {
        for (glm::vec2 travel :
             {glm::vec2(.005f, 0), glm::vec2(-.005f, 0), glm::vec2(0, .005f), glm::vec2(0, -.005f)}) {
            r1::XrGestureInput input;
            Frame frame;
            frame.focused = frame.head.tracked = true;
            frame.head.position = {0, 1.7f, 0};
            auto stroke = [&](double time, float outward, glm::vec2 offset) {
                auto hand = curled(side);
                const float mirror = side == XRHand::Right ? 1.f : -1.f;
                hand.joints[static_cast<int>(XRHandJoint::ThumbTip)].pose.position = {
                    (.0275f + outward) * mirror, .085f - offset.y, .009f + offset.x};
                frame.hands[xrHandIndex(side)].skeleton = hand;
                frame.time = time;
                input.submit(frame, true);
            };
            stroke(0, .045f, {0, 0});
            stroke(.045, .045f, {0, 0});
            stroke(.06, .013f, {0, 0});
            stroke(.085, .013f, {0, 0});
            stroke(.10, .045f, {0, 0});
            check(input.active(), "short optical thumb tap arms walking with the sensitive profile");
            stroke(.15, .045f, {0, 0});
            stroke(.17, .013f, {0, 0});
            stroke(.195, .013f, travel);
            check(input.consumeTurnDegrees() == 0 && glm::length(input.advance(.02f)) == 0,
                  "short optical stroke cannot move or turn before thumb release");
            stroke(.21, .045f, travel);
            if (travel.x != 0) {
                const bool left = (travel.x > 0) == (side == XRHand::Right);
                check(input.consumeTurnDegrees() == (left ? -15.f : 15.f) &&
                          glm::length(input.advance(.02f)) == 0,
                      "five-millimetre optical lateral stroke turns with correct handedness and no "
                      "translation");
            } else {
                glm::vec3 moved(0);
                for (int i = 0; i < 10; ++i)
                    moved += input.advance(.02f);
                check(glm::length(moved - glm::vec3(0, 0, travel.y > 0 ? -.5f : .5f)) < 1e-5f,
                      "five-millimetre optical forward/backward stroke produces a full step");
            }
        }
    }
}

int main() {
    r1::XrGestureInput input;
    auto frame = tracked();
    auto sample = [&](double time, uint32_t mask, bool permitted = true) {
        // Stay within the sample-gap bound and beyond the production cooldown
        // between repeated gestures of the same kind.
        frame.time = time * 4;
        frame.hands[1].native.downMask = frame.hands[1].native.pressedMask = mask;
        input.submit(frame, permitted);
    };
    const auto tap = xrMicrogestureBit(XRMicrogesture::ThumbTap);
    const auto forward = xrMicrogestureBit(XRMicrogesture::SwipeForward);
    const auto left = xrMicrogestureBit(XRMicrogesture::SwipeLeft);
    const auto right = xrMicrogestureBit(XRMicrogesture::SwipeRight);
    sample(0, 0);
    sample(.02, forward);
    check(glm::length(input.advance(.02f)) == 0, "unarmed hand never moves the world");
    sample(.04, 0);
    sample(.06, tap);
    check(input.active(), "native thumb tap enables R1World walking");
    sample(.08, 0);
    sample(.10, forward);
    glm::vec3 moved(0);
    for (int i = 0; i < 10; ++i)
        moved += input.advance(.02f);
    check(glm::length(moved - glm::vec3(0, 0, -.5f)) < 1e-5f,
          "forward swipe supplies exactly half a metre to the character solver");
    sample(.12, 0);
    sample(.14, left);
    moved = glm::vec3(0);
    for (int i = 0; i < 10; ++i)
        moved += input.advance(.02f);
    check(glm::length(moved) == 0, "lateral swipe does not translate the player");
    check(input.consumeTurnDegrees() == -15, "left swipe turns the player left by fifteen degrees");
    check(input.consumeTurnDegrees() == 0, "turn is consumed once without replay");
    sample(.15, 0);
    sample(.155, right);
    check(input.consumeTurnDegrees() == 15, "right swipe turns the player right by fifteen degrees");
    sample(.16, 0);
    sample(.18, forward);
    check(glm::length(input.advance(.02f, true)) == 0, "keyboard walking cancels queued gesture motion");
    check(glm::length(input.advance(.02f)) == 0, "keyboard priority leaves no deferred step");
    sample(.20, 0);
    sample(.22, forward);
    input.rejectStep();
    check(glm::length(input.advance(.02f)) == 0, "unloaded ground rejection consumes the queued step");
    sample(.24, 0);
    sample(.26, forward);
    sample(.28, 0, false);
    check(!input.active() && glm::length(input.advance(.02f)) == 0,
          "menu, vehicle and gameplay gates stop locomotion immediately");
    sample(.30, 0);
    sample(.32, tap);
    check(input.active(), "walking may be explicitly rearmed after a gameplay gate");
    frame.focused = false;
    sample(.34, left);
    check(input.consumeTurnDegrees() == 0, "focus loss cannot leave a queued turn");
    check(!input.active() && glm::length(input.advance(.02f)) == 0,
          "headset focus loss cancels world walking");
    // Native completions 80 ms apart must survive the game's 60 ms cooldown.
    r1::XrGestureInput quick;
    frame = tracked();
    auto fast = [&](double time, uint32_t mask, bool permitted = true) {
        frame.time = time;
        frame.hands[1].native.downMask = frame.hands[1].native.pressedMask = mask;
        quick.submit(frame, permitted);
    };
    fast(0, 0);
    fast(.02, tap);
    fast(.04, 0);
    fast(.06, forward);
    fast(.08, 0);
    fast(.10, left);
    check(quick.consumeTurnDegrees() == -15 && glm::length(quick.advance(.02f)) == 0,
          "turn cancels an unfinished forward step without sideways drift");
    fast(.12, 0);
    fast(.18, left);
    check(quick.consumeTurnDegrees() == -15, "rapid distinct lateral completions are not swallowed");
    fast(.20, 0);
    fast(.22, right);
    fast(.24, 0, false);
    check(quick.consumeTurnDegrees() == 0 && !quick.active(),
          "gameplay suspension clears an unconsumed turn");
    sensitivityContracts();
    std::printf("%s R1World XRGesture: %d checks, %d failures\n", failures ? "FAIL" : "PASS", checks,
                failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
