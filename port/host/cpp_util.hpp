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

/* An owning COM interface pointer, like WRL's ComPtr (WRL needs the Vista SDK target; the host targets XP).
 * put() hands out the address for an out parameter, releasing what was held. */
template <typename T>
class com_ptr {
public:
    com_ptr() noexcept = default;
    com_ptr(const com_ptr &) = delete;
    com_ptr(com_ptr &&) = delete;
    com_ptr &operator=(const com_ptr &) = delete;
    com_ptr &operator=(com_ptr &&) = delete;
    ~com_ptr() { reset(); }

    T *get() const noexcept { return ptr_; }
    T *operator->() const noexcept { return ptr_; }
    T **put() noexcept
    {
        reset();
        return &ptr_;
    }
    [[gsl::suppress("type.1")]] void **put_void() noexcept { return reinterpret_cast<void **>(put()); }
    /* IUnknown::Release isn't declared noexcept, but COM methods don't throw. */
    [[gsl::suppress("f.6")]] void reset() noexcept
    {
        if (ptr_)
            ptr_->Release();
        ptr_ = nullptr;
    }

private:
    T *ptr_ = nullptr;
};

} // namespace hy
