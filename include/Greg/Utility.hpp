#ifndef GREG_UTILITY_HPP
#define GREG_UTILITY_HPP

#include "Types.h"

namespace Greg {

/* ── Type traits ─────────────────────────────────────────────────────── */
template<typename T> struct RemoveReference       { using Type = T; };
template<typename T> struct RemoveReference<T&>   { using Type = T; };
template<typename T> struct RemoveReference<T&&>  { using Type = T; };

template<bool Cond, typename T = void> struct EnableIf {};
template<typename T>                   struct EnableIf<true, T> { using Type = T; };

template<typename T> struct IsLvalueReference     { static constexpr bool value = false; };
template<typename T> struct IsLvalueReference<T&> { static constexpr bool value = true;  };

/* ── Move & forward (std::move / std::forward equivalents) ──────────── */
template<typename T>
constexpr typename RemoveReference<T>::Type&& move(T&& val) {
    return static_cast<typename RemoveReference<T>::Type&&>(val);
}

template<typename T>
constexpr T&& forward(typename RemoveReference<T>::Type& val) {
    return static_cast<T&&>(val);
}

template<typename T>
constexpr T&& forward(typename RemoveReference<T>::Type&& val) {
    static_assert(!IsLvalueReference<T>::value,
                  "forward: cannot forward an rvalue as an lvalue");
    return static_cast<T&&>(val);
}

/* ── Swap ────────────────────────────────────────────────────────────── */
template<typename T>
void swap(T& a, T& b) {
    T tmp = move(a);
    a = move(b);
    b = move(tmp);
}

} // namespace Greg

#endif /* GREG_UTILITY_HPP */
