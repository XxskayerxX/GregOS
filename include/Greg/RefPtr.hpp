#ifndef GREG_REFPTR_HPP
#define GREG_REFPTR_HPP

#include "RefCounted.hpp"
#include "Utility.hpp"

namespace Greg {

/* ── RefPtr<T> — nullable shared reference-counted pointer ──────────────
   T must publicly inherit from RefCounted<T>.
   Inspired by SerenityOS AK::RefPtr.

   Usage:
       class Obj : public RefCounted<Obj> { ... };
       RefPtr<Obj> a = make_ref<Obj>();   // ref_count == 1
       RefPtr<Obj> b = a;                 // ref_count == 2
       a.clear();                         // ref_count == 1
       // b goes out of scope → ref_count == 0 → delete                 */

template<typename T>
class RefPtr {
public:
    RefPtr() = default;

    RefPtr(T* ptr) : m_ptr(ptr) {
        if (m_ptr) m_ptr->ref();
    }
    RefPtr(const RefPtr& o) : m_ptr(o.m_ptr) {
        if (m_ptr) m_ptr->ref();
    }
    RefPtr(RefPtr&& o) : m_ptr(o.m_ptr) {
        o.m_ptr = nullptr;
    }
    ~RefPtr() { clear(); }

    /* ── Covariant conversion from RefPtr<Derived> ───────────────────────
       Allows:  RefPtr<Base> b = Greg::move(derived_ptr);
                RefPtr<Base> b = derived_ptr;           (copies ref)
       Requires Derived* to be implicitly convertible to T* (is-a check). */
    template<typename U>
    RefPtr(RefPtr<U>&& o) : m_ptr(o.leak_ref()) {}   /* steal: no ref() change */

    template<typename U>
    RefPtr(const RefPtr<U>& o) : m_ptr(o.ptr()) {
        if (m_ptr) m_ptr->ref();
    }

    RefPtr& operator=(const RefPtr& o) {
        if (m_ptr != o.m_ptr) {
            if (m_ptr) m_ptr->unref();
            m_ptr = o.m_ptr;
            if (m_ptr) m_ptr->ref();
        }
        return *this;
    }
    RefPtr& operator=(RefPtr&& o) {
        if (this != &o) {
            if (m_ptr) m_ptr->unref();
            m_ptr = o.m_ptr;
            o.m_ptr = nullptr;
        }
        return *this;
    }
    RefPtr& operator=(T* ptr) {
        if (m_ptr != ptr) {
            if (m_ptr) m_ptr->unref();
            m_ptr = ptr;
            if (m_ptr) m_ptr->ref();
        }
        return *this;
    }

    /* Access */
    T*       ptr()              { return m_ptr; }
    const T* ptr()        const { return m_ptr; }
    T*       operator->()       { return m_ptr; }
    const T* operator->() const { return m_ptr; }
    T&       operator*()        { return *m_ptr; }
    const T& operator*()  const { return *m_ptr; }

    bool is_null()           const { return m_ptr == nullptr; }
    explicit operator bool() const { return m_ptr != nullptr; }

    bool operator==(const RefPtr& o) const { return m_ptr == o.m_ptr; }
    bool operator!=(const RefPtr& o) const { return m_ptr != o.m_ptr; }

    /* Release reference without decrementing — caller takes ownership */
    [[nodiscard]] T* leak_ref() {
        T* p = m_ptr;
        m_ptr = nullptr;
        return p;
    }

    void clear() {
        if (m_ptr) { m_ptr->unref(); m_ptr = nullptr; }
    }

private:
    T* m_ptr { nullptr };
};

/* ── NonnullRefPtr<T> — RefPtr guaranteed non-null after construction ────
   No is_null() check — use when null is architecturally impossible.
   Converts cleanly to RefPtr<T> when nullable context is needed.      */
template<typename T>
class NonnullRefPtr {
public:
    explicit NonnullRefPtr(T& ref) : m_ptr(&ref) { m_ptr->ref(); }

    NonnullRefPtr(const NonnullRefPtr& o) : m_ptr(o.m_ptr) { m_ptr->ref(); }
    NonnullRefPtr(NonnullRefPtr&& o) : m_ptr(o.m_ptr)      { o.m_ptr = nullptr; }

    ~NonnullRefPtr() { if (m_ptr) m_ptr->unref(); }

    T*       operator->()       { return m_ptr; }
    const T* operator->() const { return m_ptr; }
    T&       operator*()        { return *m_ptr; }
    const T& operator*()  const { return *m_ptr; }
    T*       ptr()              { return m_ptr; }

    /* Downgrade to nullable */
    RefPtr<T> as_nullable() const { return RefPtr<T>(m_ptr); }

private:
    T* m_ptr;
};

/* ── Factories ───────────────────────────────────────────────────────── */
template<typename T, typename... Args>
RefPtr<T> make_ref(Args&&... args) {
    return RefPtr<T>(new T(forward<Args>(args)...));
}

template<typename T, typename... Args>
NonnullRefPtr<T> make_nonnull(Args&&... args) {
    return NonnullRefPtr<T>(*new T(forward<Args>(args)...));
}

} // namespace Greg

#endif /* GREG_REFPTR_HPP */
