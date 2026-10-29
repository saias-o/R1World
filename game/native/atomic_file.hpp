#pragma once

#include <atomic>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#pragma push_macro("NEAR")
#include <windows.h>
#pragma pop_macro("NEAR")
#else
#include <unistd.h>
#endif

namespace r1 {
// A failed write/close/replacement never removes the last complete file.
inline void atomicWrite(const std::filesystem::path& path, const std::string& bytes) {
    static std::atomic<uint64_t> serial{0};
    std::filesystem::create_directories(path.parent_path());
#ifdef _WIN32
    const auto process = GetCurrentProcessId();
#else
    const auto process = getpid();
#endif
    const auto temporary = std::filesystem::path(path.string() + ".tmp-" + std::to_string(process) + "-" + std::to_string(++serial));
    try {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out.exceptions(std::ios::badbit | std::ios::failbit);
        out.write(bytes.data(), std::streamsize(bytes.size()));
        out.flush();
        out.close();
#ifdef _WIN32
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("cannot replace " + path.string() + " (Windows " + std::to_string(GetLastError()) + ")");
#else
        std::filesystem::rename(temporary, path);
#endif
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}
} // namespace r1
