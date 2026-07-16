#ifndef GREG_OWNPTR_HPP
#define GREG_OWNPTR_HPP

#include "New.hpp"
#include "Utility.hpp"

namespace Greg {

/* ── OwnPtr<T> — exclusive (unique) ownership ───────────────────────────
   Inspired by SerenityOS AK::OwnPtr / C++ std::unique_ptr.
   Move-only: cannot be copied. The pointee is destroyed (delete) on scope exit.

   Usage:
       auto w = make_own<Window>(x, y, w, h);
       w->draw();                              // works like raw pointer
       OwnPtr<Window> other = move(w);        // ownership transfer
       // w is now null; other owns the Window                          */

template<typename T>
class OwnPtr {
public:
    OwnPtr()               = default;
    explicit OwnPtr(T* p)  : m_ptr(p) {}
    OwnPtr(OwnPtr&& o)     : m_ptr(o.leak_ptr()) {}
    OwnPtr(const OwnPtr&)  = delete;

    ~OwnPtr() { clear(); }

    OwnPtr& operator=(OwnPtr&& o) {
        if (this != &o) { clear(); m_ptr = o.leak_ptr(); }
        return *this;
    }
    OwnPtr& operator=(const OwnPtr&) = delete;

    OwnPtr& operator=(T* p) {
        if (m_ptr != p) { clear(); m_ptr = p; }
        return *this;
    }

    /* Access */
    T*       ptr()              { return m_ptr; }
    const T* ptr()        const { return m_ptr; }
    T*       operator->()       { return m_ptr; }
    const T* operator->() const { return m_ptr; }
    T&       operator*()        { return *m_ptr; }
    const T& operator*()  const { return *m_ptr; }

    bool is_null()          const { return m_ptr == nullptr; }
    explicit operator bool() const { return m_ptr != nullptr; }

    /* Release ownership without destroying the pointee */
    [[nodiscard]] T* leak_ptr() {
        T* p = m_ptr;
        m_ptr = nullptr;
        return p;
    }

    void clear() {
        delete m_ptr;
        m_ptr = nullptr;
    }

private:
    T* m_ptr { nullptr };
};

/* Factory — equivalent to SerenityOS adopt_own(*new T(args...)) */
template<typename T, typename... Args>
OwnPtr<T> make_own(Args&&... args) {
    return OwnPtr<T>(new T(forward<Args>(args)...));
}

} // namespace Greg

#endif /* GREG_OWNPTR_HPP */
