#pragma once

#include <array>
#include <chrono>
#include <optional>
#include <string_view>

namespace r1 {

class MenuIntro {
public:
    using Clock = std::chrono::steady_clock;

    bool takeStart(Clock::time_point now = Clock::now()) {
        if (!shown_) shown_ = now;
        if (attempted_ || now - *shown_ < kStartupDelay) return false;
        attempted_ = true;
        return true;
    }

    void start(Clock::time_point now = Clock::now()) {
        if (!started_) started_ = now;
    }

    std::string_view subtitle(Clock::time_point now = Clock::now()) const {
        return started_ ? subtitleAt(std::chrono::duration<double>(now - *started_).count()) : std::string_view{};
    }

    static std::string_view subtitleAt(double seconds) {
        for (const auto& cue : kCues)
            if (seconds >= cue.begin && seconds < cue.end) return cue.text;
        return {};
    }

private:
    static constexpr auto kStartupDelay = std::chrono::seconds(3);
    struct Cue { double begin, end; std::string_view text; };
    static constexpr std::array<Cue, 3> kCues{{
        {0.0, 2.0, "Welcome to Earth."},
        {2.0, 4.5, "The true beauty of life is measured by the vastness"},
        {4.5, 8.0, "of all that is left to discover, learn, and explore."},
    }};
    std::optional<Clock::time_point> started_;
    std::optional<Clock::time_point> shown_;
    bool attempted_ = false;
};

} // namespace r1
