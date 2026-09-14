#pragma once

#include <bit>
#include <cstdint>

namespace guitarfx
{
/// True unless `value` is NaN or an infinity -- in every build configuration.
///
/// Release and RelWithDebInfo compile with fast floating-point semantics (/fp:fast on MSVC,
/// -ffast-math on clang, which is the Android build) and Debug does not, so a NaN check that
/// works in Debug proves nothing. What each compiler does, measured:
///
///  - clang's -ffast-math includes -ffinite-math-only, which lets the optimiser assume no value
///    is ever NaN or infinite. std::isnan, std::isinf and std::isfinite fold to constants with the
///    NDK's clang 18 and with clang 22 alike, and clang 22 also recognises the usual bit test --
///    std::bit_cast and a mask -- as a finiteness check and folds that too. Only bits read back
///    through a volatile integer survive: every access to a volatile object is observable, so the
///    optimiser can neither trace the bits back to a float nor assume anything about them.
///  - MSVC's /fp:fast keeps std::isfinite, as an _fdtest/_dtest call per check, and leaves the bit
///    test alone; what it drops is the unordered case in comparisons, so `a != b` is false when
///    `a` is NaN.
///
/// So the volatile read is used only where it is needed, as it costs a stack store and reload per
/// check. Everywhere else the plain bit test compiles to an inline mask and compare.
///
/// Use this where a NaN or infinity really has to be caught -- in a filter's state after a bad
/// input, in a value from outside the process. Never use a non-finite value to *mean* something:
/// a std::optional, an explicit flag or a finite value says the same thing without relying on any
/// of this, and under -ffinite-math-only even a NaN constant may not survive to be tested.
[[nodiscard]] inline bool IsFinite(float value) noexcept
{
#if defined(__FINITE_MATH_ONLY__) && __FINITE_MATH_ONLY__
    volatile std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
#else
    const auto bits = std::bit_cast<std::uint32_t>(value);
#endif
    return (bits & 0x7fffffffu) < 0x7f800000u;
}

/// See IsFinite(float).
[[nodiscard]] inline bool IsFinite(double value) noexcept
{
#if defined(__FINITE_MATH_ONLY__) && __FINITE_MATH_ONLY__
    volatile std::uint64_t bits = std::bit_cast<std::uint64_t>(value);
#else
    const auto bits = std::bit_cast<std::uint64_t>(value);
#endif
    return (bits & 0x7fffffffffffffffull) < 0x7ff0000000000000ull;
}
} // namespace guitarfx
