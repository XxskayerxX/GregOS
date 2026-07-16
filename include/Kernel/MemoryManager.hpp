#ifndef KERNEL_MEMORY_MANAGER_HPP
#define KERNEL_MEMORY_MANAGER_HPP

/* Kernel::MemoryManager — single C++ entry point for kernel heap management.
   Phase 4: wraps kmalloc/kfree, tracks lifetime alloc/free counts,
             records heap bounds for diagnostics.
   Phase 5: will own per-process page directories and virtual-memory maps.
   Freestanding: no libc, no exceptions.                                    */

#include "../Greg/Types.h"

namespace Kernel {

class MemoryManager {
public:
    static MemoryManager& instance();

    /* Register heap bounds once at kernel init. */
    void initialize(Greg::u32 heap_base, Greg::u32 heap_size);
    bool is_initialized() const { return m_initialized; }

    /* ── Allocation ──────────────────────────────────────────────────── */
    void* allocate(Greg::usize sz);
    void  free(void* ptr);

    /* ── Statistics — delegate to kmalloc_used / kmalloc_total ──────── */
    Greg::u32 used_bytes()  const;
    Greg::u32 total_bytes() const;

    /* Lifetime counters (never reset across kernel session). */
    Greg::u32 alloc_count() const { return m_alloc_count; }
    Greg::u32 free_count()  const { return m_free_count; }
    Greg::u32 live_count()  const {
        return (m_alloc_count > m_free_count)
             ? (m_alloc_count - m_free_count) : 0;
    }

private:
    Greg::u32 m_heap_base   { 0 };
    Greg::u32 m_heap_size   { 0 };
    Greg::u32 m_alloc_count { 0 };
    Greg::u32 m_free_count  { 0 };
    bool      m_initialized { false };

    static MemoryManager s_instance;
};

} /* namespace Kernel */

#endif /* KERNEL_MEMORY_MANAGER_HPP */
