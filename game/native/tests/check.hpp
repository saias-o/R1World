// The smallest test harness that says what failed and where.
//
//   TEST(Suite, what_it_holds) { CHECK(x > 0); NEAR(a, b, 1e-9); }
//
// `r1test [--game <root>] [filter]` runs every test whose "Suite.name"
// contains the filter, with the game's data files loaded as the game loads them.
#pragma once

#include <cmath>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace r1test {

struct Case { std::string name; std::function<void()> run; };
inline std::vector<Case>& cases() { static std::vector<Case> all; return all; }
struct Register { Register(const char* suite, const char* name, std::function<void()> f) { cases().push_back({std::string(suite) + "." + name, std::move(f)}); } };

struct Failure : std::runtime_error { using std::runtime_error::runtime_error; };

inline std::string where(const char* file, int line) {
    std::string f = file;
    const auto slash = f.find_last_of("/\\");
    return (slash == std::string::npos ? f : f.substr(slash + 1)) + ":" + std::to_string(line);
}

// The game's root, for tests that read shipped data or cached observations.
std::string& gameRoot();

}  // namespace r1test

#define R1_CAT2(a, b) a##b
#define R1_CAT(a, b) R1_CAT2(a, b)
#define TEST(suite, name)                                                                        \
    static void R1_CAT(test_, R1_CAT(suite, R1_CAT(_, name)))();                                 \
    static const r1test::Register R1_CAT(reg_, R1_CAT(suite, R1_CAT(_, name)))(                  \
        #suite, #name, R1_CAT(test_, R1_CAT(suite, R1_CAT(_, name))));                           \
    static void R1_CAT(test_, R1_CAT(suite, R1_CAT(_, name)))()

#define CHECK(cond)                                                                              \
    do {                                                                                         \
        if (!(cond)) throw r1test::Failure(r1test::where(__FILE__, __LINE__) + ": " #cond);      \
    } while (0)

#define CHECK_MSG(cond, message)                                                                 \
    do {                                                                                         \
        if (!(cond)) {                                                                           \
            std::ostringstream said_;                                                            \
            said_ << r1test::where(__FILE__, __LINE__) << ": " #cond " -- " << message;          \
            throw r1test::Failure(said_.str());                                                  \
        }                                                                                        \
    } while (0)

#define NEAR(a, b, tolerance)                                                                    \
    do {                                                                                         \
        const double a_ = (a), b_ = (b);                                                         \
        if (!(std::abs(a_ - b_) <= (tolerance))) {                                               \
            std::ostringstream said_;                                                            \
            said_.precision(17);                                                                 \
            said_ << r1test::where(__FILE__, __LINE__) << ": " #a " = " << a_ << ", expected "   \
                  << b_ << " within " << (tolerance);                                            \
            throw r1test::Failure(said_.str());                                                  \
        }                                                                                        \
    } while (0)
