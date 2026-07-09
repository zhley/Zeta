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


// Load and store a trivially copyable type T from/to a pointer, ensuring strict aliasing rules are respected.
// template<typename T>
// inline T loadPtr(const void* ptr) {
//     static_assert(std::is_trivially_copyable<T>::value, "Type T must be trivially copyable");
//     T data;
//     std::memcpy(&data, ptr, sizeof(T));
//     return data;
// }

// template<typename T>
// inline void storePtr(void* ptr, T data) {
//     static_assert(std::is_trivially_copyable<T>::value, "Type T must be trivially copyable");
//     std::memcpy(ptr, &data, sizeof(T));
// }

}
}
