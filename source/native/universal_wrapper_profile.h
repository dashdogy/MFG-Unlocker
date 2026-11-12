#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace universal_wrapper_profile
{
inline constexpr size_t kPatternSize = 10;
inline constexpr size_t kMaximumOffset = 1;
inline constexpr size_t kPatchOffset = 7;

constexpr bool IsSupportedMaximum(uint32_t maximum) noexcept
{
    return maximum == 1 || maximum == 3 || maximum == 5;
}

constexpr uint32_t SafeMaximumMultiplier(uint32_t compiledMaximum) noexcept
{
    return IsSupportedMaximum(compiledMaximum)
        ? compiledMaximum + 1u : 2u;
}

inline bool Matches(const uint8_t* candidate, size_t available) noexcept
{
    if (!candidate || available < kPatternSize || candidate[0] != 0xBA)
        return false;

    uint32_t maximum = 0;
    std::memcpy(&maximum, candidate + kMaximumOffset, sizeof(maximum));
    return IsSupportedMaximum(maximum)
        && candidate[5] == 0x3B
        && candidate[6] == 0xCA
        && ((candidate[7] == 0x0F
                && candidate[8] == 0x42
                && candidate[9] == 0xD1)
            || (candidate[7] == 0x90
                && candidate[8] == 0x90
                && candidate[9] == 0x90));
}
}
