#pragma once

#include <cstdint>

namespace Zeta {
namespace Utils {

constexpr uint32_t FNV_PRIME = 16777619u;
constexpr uint32_t FNV_OFFSET_BASIS = 2166136261u;

inline uint32_t hashString(const char* str, uint32_t length) {
    uint32_t hash = FNV_OFFSET_BASIS;
    for (uint32_t i = 0; i < length; ++i) {
        hash ^= static_cast<uint8_t>(str[i]);
        hash *= FNV_PRIME;
    }
    return hash;
}

}
}
