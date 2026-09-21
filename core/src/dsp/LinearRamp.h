#pragma once

namespace guitarfx
{
/// A value that glides to a new target in equal steps instead of jumping, so a parameter
/// change does not click. The owner keeps one sample counter for a whole group of ramps:
/// Begin() them together, Advance() each sample while the counter runs, and Finish() when
/// it reaches zero to land exactly on the target however the steps rounded.
///
/// T needs `T + T`, `T - T` and `T * double`: a double, or a set of coefficients such as
/// BiquadCoefficients.
template <typename T> struct LinearRamp
{
    T current{};
    T target{};
    T step{};

    /// Starts moving `current` to `target` over `samples` samples (at least one).
    void Begin(int samples) noexcept
    {
        step = (target - current) * (1.0 / static_cast<double>(samples));
    }

    void Advance() noexcept
    {
        current = current + step;
    }

    void Finish() noexcept
    {
        current = target;
    }
};
} // namespace guitarfx
