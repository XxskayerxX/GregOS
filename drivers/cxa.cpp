/* ── C++ ABI stubs for freestanding kernel ───────────────────────────
   Required by g++ when using virtual functions / pure virtuals.
   We compile with -fno-exceptions -fno-rtti, so __gxx_personality_v0
   and typeinfo are not needed.                                         */

/* Required by __cxa_atexit / global destructor registration.
   In a freestanding kernel we never exit, so this can be null. */
void* __dso_handle = nullptr;

extern "C" {

/* Called through the vtable slot of a pure-virtual function if it is
   somehow invoked at runtime (should never happen in correct code).   */
void __cxa_pure_virtual(void) {
    for (;;) __asm__ volatile("hlt");
}

/* Called at exit to run static destructors — we never exit, so no-op. */
int __cxa_atexit(void (*)(void*), void*, void*) { return 0; }
void __cxa_finalize(void*) {}

} /* extern "C" */

/* Deleting destructor stub — virtual dtors reference operator delete even
   when the objects are never heap-allocated.  We never free, so this is
   safe: Widget/Window instances live in static storage.                  */
void operator delete(void*)   noexcept {}
void operator delete[](void*) noexcept {}
void operator delete(void*, unsigned int)   noexcept {}
void operator delete[](void*, unsigned int) noexcept {}
