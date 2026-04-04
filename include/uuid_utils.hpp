#pragma once

#include <format>
#include <random>
#include <string>

namespace utils {

inline std::string generate_uuid_v4() {
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    auto rand64 = [&]() { return gen(); };

    auto to_hex = [](uint64_t value, size_t width) {
        return std::format("{:0{}X}", value, width);
    };

    uint64_t high = rand64();
    uint64_t low = rand64();

    // Set version (4) and variant (10xx)
    high &= 0xFFFFFFFFFFFF0FFFULL;
    high |= 0x0000000000004000ULL;
    low &= 0x3FFFFFFFFFFFFFFFULL;
    low |= 0x8000000000000000ULL;

    return std::format(
        "{}-{}-{}-{}-{}", to_hex((high >> 32) & 0xFFFFFFFFULL, 8),
        to_hex((high >> 16) & 0xFFFFULL, 4), to_hex(high & 0xFFFFULL, 4),
        to_hex((low >> 48) & 0xFFFFULL, 4),
        to_hex(low & 0xFFFFFFFFFFFFULL, 12));
}

} // namespace utils
