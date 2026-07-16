#ifndef GREG_REFCOUNTED_HPP
#define GREG_REFCOUNTED_HPP

#include "Types.h"

namespace Greg {

/* ── RefCounted<T> ───────────────────────────────────────────────────────
   Base class for reference-counted objects. Inspired by SerenityOS AK.
   Usage:
       class MyObj : public RefCounted<MyObj> { ... };
       RefPtr<MyObj> ptr = make_ref<MyObj>();   // ref_count == 1

   Invariant: objects start at ref_count == 0.  RefPtr(T*) calls ref()
   immediately, so after make_ref the count is exactly 1.  Every copy of
   a RefPtr increments; every destruction decrements.  At 0 → delete.  */
template<typename T>
class RefCounted {
public:
    void ref() {
        ++m_ref_count;
    }

    void unref() {
        if (--m_ref_count == 0)
            delete static_cast<T*>(this);
    }

    u32 ref_count() const { return m_ref_count; }

protected:
    RefCounted()  = default;
    ~RefCounted() = default;

    /* Reference-counted objects must not be copied — only their pointers */
    RefCounted(const RefCounted&) = delete;
    RefCounted& operator=(const RefCounted&) = delete;

private:
    u32 m_ref_count { 0 };
};

} // namespace Greg

#endif /* GREG_REFCOUNTED_HPP */
