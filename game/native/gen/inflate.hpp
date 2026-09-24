// A zlib stream decoder (RFC 1950/1951), for the shipped sea prior's blocks.
// Small on purpose: the game only ever reads data it wrote itself.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace r1 {

// Throws std::runtime_error on a stream it cannot decode.
std::vector<uint8_t> inflateZlib(const uint8_t* data, size_t size);

}  // namespace r1
