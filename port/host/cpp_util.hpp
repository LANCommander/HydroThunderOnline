#pragma once
/* Small C++ Core Guidelines helpers for the host's C++ files (the GSL isn't a dependency). */
#include <stdexcept>
#include <type_traits>

namespace hy {

/* Checked narrowing conversion, like gsl::narrow (ES.46): throws if the value doesn't survive. */
template <typename To, typename From>
[[gsl::suppress("type.1")]] constexpr To narrow(From from)
{
    static_assert(std::is_arithmetic_v<To> && std::is_arithmetic_v<From>);
    const auto to = static_cast<To>(from);
    if (static_cast<From>(to) != from || ((to < To{}) != (from < From{})))
        throw std::range_error("narrowing conversion changed the value");
    return to;
}

/* Narrowing the caller has already range-checked, like gsl::narrow_cast. */
template <typename To, typename From>
[[gsl::suppress("type.1")]] constexpr To narrow_cast(From from) noexcept
{
    return static_cast<To>(from);
}

/* HRESULT tests without the C-style casts in the FAILED/SUCCEEDED macros. */
constexpr bool failed(long hr) noexcept
{
    return hr < 0;
}

constexpr bool succeeded(long hr) noexcept
{
    return hr >= 0;
}

} // namespace hy
