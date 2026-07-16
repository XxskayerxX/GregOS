#ifndef GREG_NEW_HPP
#define GREG_NEW_HPP

#include "Types.h"

/* ── Global operator new — routes all C++ allocations through kmalloc ──
   kmalloc is the kernel bump allocator defined in kernel/kernel.c.
   kfree   is a no-op on that allocator; operator delete stubs live in
   drivers/cxa.cpp (they exist to satisfy the ABI, not to reclaim memory).

   We declare kmalloc as extern "C" so this header works in C++ TUs
   even though kmalloc itself is compiled as C.                         */
extern "C" void* kmalloc(unsigned int size);
extern "C" void  kfree(void* ptr);

/* Allocating new */
inline void* operator new(Greg::usize size) {
    return kmalloc(static_cast<unsigned int>(size));
}
inline void* operator new[](Greg::usize size) {
    return kmalloc(static_cast<unsigned int>(size));
}

/* Placement new — required by Vector<T> for in-place construction.
   Must NOT call kmalloc; just returns the supplied address.           */
inline void* operator new(Greg::usize, void* p)   noexcept { return p; }
inline void* operator new[](Greg::usize, void* p) noexcept { return p; }

#endif /* GREG_NEW_HPP */
