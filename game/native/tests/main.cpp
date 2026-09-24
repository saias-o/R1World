#include "check.hpp"

#include "gen/palette.hpp"

#include <chrono>
#include <cstdio>
#include <iostream>

std::string& r1test::gameRoot() {
    static std::string root = ".";
    return root;
}

int main(int argc, char** argv) {
    std::string filter;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--game" && i + 1 < argc) r1test::gameRoot() = argv[++i];
        else filter = a;
    }
    try {
        r1::loadPalette(r1test::gameRoot());
    } catch (const std::exception& e) {
        std::cerr << "cannot load the game's data: " << e.what() << "\n";
        return 2;
    }
    int passed = 0, failed = 0;
    const auto started = std::chrono::steady_clock::now();
    for (const auto& c : r1test::cases()) {
        if (!filter.empty() && c.name.find(filter) == std::string::npos) continue;
        try {
            c.run();
            ++passed;
        } catch (const std::exception& e) {
            ++failed;
            std::cout << "FAIL " << c.name << "\n     " << e.what() << "\n";
        }
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::printf("%d passed, %d failed in %.2f s\n", passed, failed, seconds);
    return failed ? 1 : 0;
}
