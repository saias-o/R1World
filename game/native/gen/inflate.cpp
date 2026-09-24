#include "inflate.hpp"

#include <array>
#include <stdexcept>

namespace r1 {

namespace {
struct Bits {
    const uint8_t* data;
    size_t size, at = 0;
    uint32_t buffer = 0;
    int count = 0;
    int need(int n) {
        while (count < n) {
            if (at >= size) throw std::runtime_error("inflate: stream ends early");
            buffer |= uint32_t(data[at++]) << count;
            count += 8;
        }
        const int value = int(buffer & ((1u << n) - 1));
        buffer >>= n;
        count -= n;
        return value;
    }
    void align() { buffer = 0; count = 0; }
};

// Canonical Huffman code: symbol counts per length and symbols by code.
struct Huffman {
    std::array<uint16_t, 16> counts{};
    std::vector<uint16_t> symbols;
    Huffman(const uint8_t* lengths, int n) : symbols(size_t(n)) {
        for (int i = 0; i < n; ++i) ++counts[lengths[i]];
        counts[0] = 0;
        std::array<uint16_t, 16> offsets{};
        for (int len = 1; len < 16; ++len) offsets[size_t(len)] = uint16_t(offsets[size_t(len - 1)] + counts[size_t(len - 1)]);
        for (int i = 0; i < n; ++i)
            if (lengths[i]) symbols[offsets[lengths[i]]++] = uint16_t(i);
    }
    int decode(Bits& in) const {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; ++len) {
            code |= in.need(1);
            const int count = counts[size_t(len)];
            if (code - count < first) return symbols[size_t(index + (code - first))];
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        throw std::runtime_error("inflate: bad Huffman code");
    }
};

const uint16_t kLengthBase[] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t kLengthExtra[] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDistBase[] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const uint8_t kDistExtra[] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

void codes(Bits& in, std::vector<uint8_t>& out, const Huffman& lengths, const Huffman& distances) {
    for (;;) {
        const int symbol = lengths.decode(in);
        if (symbol < 256) { out.push_back(uint8_t(symbol)); continue; }
        if (symbol == 256) return;
        const int l = symbol - 257;
        if (l >= 29) throw std::runtime_error("inflate: bad length");
        const size_t length = size_t(kLengthBase[l] + in.need(kLengthExtra[l]));
        const int d = distances.decode(in);
        if (d >= 30) throw std::runtime_error("inflate: bad distance");
        const size_t distance = size_t(kDistBase[d] + in.need(kDistExtra[d]));
        if (distance > out.size()) throw std::runtime_error("inflate: distance too far back");
        const size_t from = out.size() - distance;
        for (size_t i = 0; i < length; ++i) out.push_back(out[from + i]);
    }
}
}  // namespace

std::vector<uint8_t> inflateZlib(const uint8_t* data, size_t size) {
    if (size < 2 || (data[0] & 0x0f) != 8 || ((data[0] << 8) | data[1]) % 31 != 0)
        throw std::runtime_error("inflate: not a zlib stream");
    Bits in{data + 2, size - 2};
    std::vector<uint8_t> out;
    int last = 0;
    while (!last) {
        last = in.need(1);
        const int type = in.need(2);
        if (type == 0) {
            in.align();
            if (in.at + 4 > in.size) throw std::runtime_error("inflate: stored block ends early");
            const size_t len = size_t(in.data[in.at] | (in.data[in.at + 1] << 8));
            in.at += 4;
            if (in.at + len > in.size) throw std::runtime_error("inflate: stored block ends early");
            out.insert(out.end(), in.data + in.at, in.data + in.at + len);
            in.at += len;
        } else if (type == 1) {
            static const auto fixed = [] {
                std::array<uint8_t, 288> l{};
                for (int i = 0; i < 144; ++i) l[size_t(i)] = 8;
                for (int i = 144; i < 256; ++i) l[size_t(i)] = 9;
                for (int i = 256; i < 280; ++i) l[size_t(i)] = 7;
                for (int i = 280; i < 288; ++i) l[size_t(i)] = 8;
                std::array<uint8_t, 30> d{};
                d.fill(5);
                return std::make_pair(Huffman(l.data(), 288), Huffman(d.data(), 30));
            }();
            codes(in, out, fixed.first, fixed.second);
        } else if (type == 2) {
            const int nlen = in.need(5) + 257, ndist = in.need(5) + 1, ncode = in.need(4) + 4;
            static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            uint8_t lengths[320] = {};
            for (int i = 0; i < ncode; ++i) lengths[order[i]] = uint8_t(in.need(3));
            const Huffman code(lengths, 19);
            uint8_t all[320] = {};
            for (int i = 0; i < nlen + ndist;) {
                const int symbol = code.decode(in);
                if (symbol < 16) { all[i++] = uint8_t(symbol); continue; }
                int repeat;
                uint8_t value = 0;
                if (symbol == 16) {
                    if (i == 0) throw std::runtime_error("inflate: repeat with nothing before");
                    value = all[i - 1];
                    repeat = 3 + in.need(2);
                } else if (symbol == 17) repeat = 3 + in.need(3);
                else repeat = 11 + in.need(7);
                if (i + repeat > nlen + ndist) throw std::runtime_error("inflate: too many lengths");
                while (repeat--) all[i++] = value;
            }
            codes(in, out, Huffman(all, nlen), Huffman(all + nlen, ndist));
        } else {
            throw std::runtime_error("inflate: bad block type");
        }
    }
    return out;
}

}  // namespace r1
