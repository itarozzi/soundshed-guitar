#pragma once

/**
 * NonFiniteInputs.h - NaN and infinity made at run time, to feed the code under test.
 *
 * The tests build with the core's floating-point flags (they share its precompiled header), so a
 * Release build is -ffast-math on clang. Under it a NaN or infinity *constant* is something the
 * optimiser may assume away, and clang does: std::numeric_limits<double>::quiet_NaN() reached
 * the code under test as an arbitrary finite value, and its NaN guard looked broken when it had
 * never seen a NaN. Bits read through a volatile cannot be traced back to a constant.
 */

#include <bit>
#include <cstdint>

namespace test_support
{
[[nodiscard]] inline double RuntimeNaN()
{
    volatile std::uint64_t bits = 0x7ff8000000000000ull;
    return std::bit_cast<double>(static_cast<std::uint64_t>(bits));
}

[[nodiscard]] inline double RuntimeInfinity()
{
    volatile std::uint64_t bits = 0x7ff0000000000000ull;
    return std::bit_cast<double>(static_cast<std::uint64_t>(bits));
}

[[nodiscard]] inline float RuntimeNaNf()
{
    volatile std::uint32_t bits = 0x7fc00000u;
    return std::bit_cast<float>(static_cast<std::uint32_t>(bits));
}

[[nodiscard]] inline float RuntimeInfinityf()
{
    volatile std::uint32_t bits = 0x7f800000u;
    return std::bit_cast<float>(static_cast<std::uint32_t>(bits));
}
} // namespace test_support
