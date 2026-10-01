#pragma once

#include <cmath>
#include <limits>
#include <type_traits>

namespace luabind { namespace detail {

// A floating-to-narrow-integer cast outside the representable range is UB.
// The installed Windows ChangeTeam byte boundary retains the low bits. ARM64
// can pass the full register when the compiler assumes the cast was valid.
// Define truncation and low-bit wrapping before the final representable cast;
// word wrapping follows the same explicit policy, without a Windows claim.
// Nonfinite Lua numbers have no integer bits and deliberately become zero.
template <typename T>
T narrow_numeric_cast(double value)
{
    static_assert(std::is_integral_v<T> && !std::is_same_v<T, bool> && sizeof(T) <= 2u,
        "narrow_numeric_cast is restricted to byte/word integral Lua arguments");
    if (!std::isfinite(value))
        return T{0};
    const double integer = std::trunc(value);
    constexpr double minimum = static_cast<double>((std::numeric_limits<T>::min)());
    constexpr double maximum = static_cast<double>((std::numeric_limits<T>::max)());
    if (integer >= minimum && integer <= maximum)
        return static_cast<T>(integer);

    using Unsigned = std::make_unsigned_t<T>;
    constexpr double modulus = static_cast<double>((std::numeric_limits<Unsigned>::max)()) + 1.0;
    double wrapped = std::fmod(integer, modulus);
    if (wrapped < 0.0)
        wrapped += modulus;
    if constexpr (std::numeric_limits<T>::is_signed)
        if (wrapped > maximum)
            wrapped -= modulus;
    return static_cast<T>(wrapped);
}

}} // namespace luabind::detail
