#ifndef GREG_FUNCTION_HPP
#define GREG_FUNCTION_HPP

/* Greg::Function<R(Args...)> — type-erased callable.
   Freestanding: no libc, no exceptions, no RTTI.
   Move-only. Heap-allocates callable storage via kmalloc/kfree.
   Unlike std::function, copy is deleted — use Greg::move() to transfer.
   Supports any callable: function pointer, lambda (with captures), functor. */

#include "Utility.hpp"

extern "C" void* kmalloc(unsigned int size);
extern "C" void  kfree(void* ptr);

namespace Greg {

/* ── DefaultReturnValue: handles void vs non-void return types ──────── */
namespace detail {
    template<typename R>              struct DRV { static R    get() { return R{}; } };
    template<>                        struct DRV<void> { static void get() {} };
}

/* ── Primary template — undefined (must use R(Args...) specialization) */
template<typename> class Function;

/* ── Partial specialization for Function<R(Args...)> ─────────────────── */
template<typename R, typename... Args>
class Function<R(Args...)> {
    using CallFn = R(*)(void*, Args...);
    using DtorFn = void(*)(void*);

public:
    /* Default-constructed: null (operator bool returns false). */
    Function() = default;

    /* Construct from any callable C (function ptr, lambda, functor).
       C is taken by value to ensure it owns its captures.              */
    template<typename C>
    Function(C c) {
        using T = typename RemoveReference<C>::Type;
        m_storage = kmalloc(sizeof(T));
        if (!m_storage) return;  /* OOM: leave Function in null/empty state */
        new(m_storage) T(Greg::move(c));
        /* Non-capturing lambdas used as function pointers — T is a
           template parameter (compile-time), not a captured variable.  */
        m_call = [](void* p, Args... a) -> R {
            return (*static_cast<T*>(p))(static_cast<Args&&>(a)...);
        };
        m_dtor = [](void* p) {
            static_cast<T*>(p)->~T();
            kfree(p);
        };
    }

    ~Function() { clear(); }

    /* Move — transfer ownership; source becomes null. */
    Function(Function&& o) noexcept
        : m_call(o.m_call), m_dtor(o.m_dtor), m_storage(o.m_storage)
    { o.m_call = nullptr; o.m_dtor = nullptr; o.m_storage = nullptr; }

    Function& operator=(Function&& o) noexcept {
        if (this != &o) {
            clear();
            m_call    = o.m_call;
            m_dtor    = o.m_dtor;
            m_storage = o.m_storage;
            o.m_call = nullptr; o.m_dtor = nullptr; o.m_storage = nullptr;
        }
        return *this;
    }

    /* Not copyable — use Greg::move(). */
    Function(const Function&) = delete;
    Function& operator=(const Function&) = delete;

    /* Assign from a new callable (drops the current one first). */
    template<typename C>
    Function& operator=(C c) {
        clear();
        Function tmp(Greg::move(c));
        *this = Greg::move(tmp);
        return *this;
    }

    explicit operator bool() const { return m_call != nullptr; }

    R operator()(Args... args) const {
        if (!m_call) return detail::DRV<R>::get();
        return m_call(m_storage, static_cast<Args&&>(args)...);
    }

    void clear() {
        if (m_dtor && m_storage) m_dtor(m_storage);
        m_call = nullptr; m_dtor = nullptr; m_storage = nullptr;
    }

private:
    CallFn m_call    { nullptr };
    DtorFn m_dtor    { nullptr };
    void*  m_storage { nullptr };
};

} /* namespace Greg */

#endif /* GREG_FUNCTION_HPP */
