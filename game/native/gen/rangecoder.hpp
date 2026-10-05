// A binary range coder (LZMA's) over adaptive probabilities: what the world's
// compact layers are coded with (gen/canopy, gen/relief). Each bit is coded
// against a probability that learns from the bits coded with it, so a layer
// costs close to its entropy given the context its codec chooses.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <stdexcept>

namespace r1::rangecoder {

constexpr int kProbBits = 11;
constexpr int kMove = 4;  // how fast a probability follows its bits
// A probability no bit has taught yet: even odds.
constexpr uint16_t kHalf = 1 << (kProbBits - 1);
constexpr size_t kFlushBytes = sizeof(uint32_t) + 1;  // code word plus carry

struct Encoder {
    uint64_t low = 0;
    uint32_t range = 0xFFFFFFFFu;
    uint8_t cache = 0;
    uint64_t pending = 1;
    std::string out;
    void shift() {
        if (uint32_t(low) < 0xFF000000u || (low >> 32) != 0) {
            const uint8_t carry = uint8_t(low >> 32);
            uint8_t held = cache;
            do { out.push_back(char(uint8_t(held + carry))); held = 0xFF; } while (--pending);
            cache = uint8_t(low >> 24);
        }
        ++pending;
        low = (low & 0x00FFFFFFu) << 8;
    }
    void bit(uint16_t& p, int b) {
        const uint32_t bound = (range >> kProbBits) * p;
        if (!b) { range = bound; p += ((1 << kProbBits) - p) >> kMove; }
        else { low += bound; range -= bound; p -= p >> kMove; }
        while (range < (1u << 24)) { range <<= 8; shift(); }
    }
    void finish() { for (size_t i = 0; i < kFlushBytes; ++i) shift(); }
};

struct Decoder {
    const std::string& in;
    size_t at = 0;
    uint32_t range = 0xFFFFFFFFu, code = 0;
    Decoder(const std::string& s, size_t from) : in(s), at(from) {
        next();
        for (int i = 0; i < 4; ++i) code = (code << 8) | next();
    }
    uint32_t next() {
        if (at == in.size()) throw std::runtime_error("truncated range-coded layer");
        return uint8_t(in[at++]);
    }
    int bit(uint16_t& p) {
        const uint32_t bound = (range >> kProbBits) * p;
        int b;
        if (code < bound) { range = bound; p += ((1 << kProbBits) - p) >> kMove; b = 0; }
        else { code -= bound; range -= bound; p -= p >> kMove; b = 1; }
        while (range < (1u << 24)) { range <<= 8; code = (code << 8) | next(); }
        return b;
    }
};

}  // namespace r1::rangecoder
