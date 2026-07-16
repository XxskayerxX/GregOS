/* Kernel::MemoryManager — C++ wrapper over the kernel heap.
   Freestanding: no libc, no exceptions.                                   */

#include "../include/Kernel/MemoryManager.hpp"

extern "C" void*        kmalloc(unsigned int size);
extern "C" void         kfree(void* ptr);
extern "C" unsigned int kmalloc_used(void);
extern "C" unsigned int kmalloc_total(void);

namespace Kernel {

MemoryManager MemoryManager::s_instance;
MemoryManager& MemoryManager::instance() { return s_instance; }

void MemoryManager::initialize(Greg::u32 heap_base, Greg::u32 heap_size)
{
    m_heap_base   = heap_base;
    m_heap_size   = heap_size;
    m_initialized = true;
}

void* MemoryManager::allocate(Greg::usize sz)
{
    void* p = kmalloc(static_cast<unsigned int>(sz));
    if (p) ++m_alloc_count;
    return p;
}

void MemoryManager::free(void* ptr)
{
    if (!ptr) return;
    kfree(ptr);
    ++m_free_count;
}

Greg::u32 MemoryManager::used_bytes()  const
{
    return static_cast<Greg::u32>(kmalloc_used());
}

Greg::u32 MemoryManager::total_bytes() const
{
    return static_cast<Greg::u32>(kmalloc_total());
}

} /* namespace Kernel */

/* ── C bridges for kernel.c ─────────────────────────────────────────────── */
extern "C" {

void mm_initialize(unsigned int heap_base, unsigned int heap_size)
{
    Kernel::MemoryManager::instance().initialize(heap_base, heap_size);
}

unsigned int mm_used_bytes(void)
{
    return static_cast<unsigned int>(
        Kernel::MemoryManager::instance().used_bytes());
}

unsigned int mm_total_bytes(void)
{
    return static_cast<unsigned int>(
        Kernel::MemoryManager::instance().total_bytes());
}

unsigned int mm_alloc_count(void)
{
    return static_cast<unsigned int>(
        Kernel::MemoryManager::instance().alloc_count());
}

unsigned int mm_free_count(void)
{
    return static_cast<unsigned int>(
        Kernel::MemoryManager::instance().free_count());
}

unsigned int mm_live_count(void)
{
    return static_cast<unsigned int>(
        Kernel::MemoryManager::instance().live_count());
}

} /* extern "C" */
