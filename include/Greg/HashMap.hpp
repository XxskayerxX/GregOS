#ifndef GREG_HASHMAP_HPP
#define GREG_HASHMAP_HPP

/* Greg::HashMap<K,V> — open-addressing hash map with linear probing.
   Freestanding: no libc, no exceptions.
   Auto-resizes when load factor exceeds 0.70.

   Provide a hash function by specialising Greg::hash<K>:
       template<> struct Greg::hash<MyType> {
           Greg::usize operator()(const MyType& k) const { return ...; }
       };

   Default specialisations are provided for:
       - int, unsigned int, long, unsigned long
       - const char*  (FNV-1a)
       - Greg::String (FNV-1a on characters)                            */

#include "Types.h"
#include "Utility.hpp"
#include "String.hpp"

extern "C" void* kmalloc(unsigned int size);
extern "C" void  kfree(void* ptr);

namespace Greg {

/* ── Default hash ───────────────────────────────────────────────────── */
template<typename K>
struct hash {
    usize operator()(const K& k) const {
        /* Generic fallback: treat as byte array. */
        const unsigned char* p = reinterpret_cast<const unsigned char*>(&k);
        usize h = 2166136261u;
        for (usize i = 0; i < sizeof(K); ++i)
            h = (h ^ p[i]) * 16777619u;
        return h;
    }
};

/* Integer specialisations — Knuth multiplicative hash */
template<> struct hash<int> {
    usize operator()(int k) const { return (usize)((unsigned)k * 2654435769u); }
};
template<> struct hash<unsigned int> {
    usize operator()(unsigned int k) const { return (usize)(k * 2654435769u); }
};
template<> struct hash<long> {
    usize operator()(long k) const { return (usize)((unsigned long)k * 2654435769u); }
};
template<> struct hash<unsigned long> {
    usize operator()(unsigned long k) const { return (usize)(k * 2654435769u); }
};

/* C-string: FNV-1a */
template<> struct hash<const char*> {
    usize operator()(const char* s) const {
        usize h = 2166136261u;
        while (*s) h = (h ^ (unsigned char)*s++) * 16777619u;
        return h;
    }
};

/* Greg::String: FNV-1a */
template<> struct hash<String> {
    usize operator()(const String& s) const {
        usize h = 2166136261u;
        const char* p = s.characters();
        while (p && *p) h = (h ^ (unsigned char)*p++) * 16777619u;
        return h;
    }
};

/* ── HashMap<K,V> ────────────────────────────────────────────────────── */

template<typename K, typename V, typename H = hash<K>>
class HashMap {
public:
    HashMap()  = default;
    ~HashMap() { clear_storage(); }

    /* Non-copyable, movable. */
    HashMap(const HashMap&) = delete;
    HashMap& operator=(const HashMap&) = delete;

    HashMap(HashMap&& o) noexcept
        : m_buckets(o.m_buckets), m_cap(o.m_cap), m_size(o.m_size)
    { o.m_buckets = nullptr; o.m_cap = 0; o.m_size = 0; }

    HashMap& operator=(HashMap&& o) noexcept {
        if (this != &o) {
            clear_storage();
            m_buckets = o.m_buckets; m_cap = o.m_cap; m_size = o.m_size;
            o.m_buckets = nullptr; o.m_cap = 0; o.m_size = 0;
        }
        return *this;
    }

    /* Insert or overwrite key→value. */
    void set(const K& key, const V& value) {
        ensure_capacity();
        usize idx = probe(key);
        if (!m_buckets[idx].used) {
            ++m_size;
            m_buckets[idx].used = true;
        }
        m_buckets[idx].key   = key;
        m_buckets[idx].value = value;
    }

    /* Returns pointer to value, or nullptr if not found. */
    V* get(const K& key) {
        if (!m_buckets) return nullptr;
        usize idx = find(key);
        return (idx != INVALID) ? &m_buckets[idx].value : nullptr;
    }
    const V* get(const K& key) const {
        if (!m_buckets) return nullptr;
        usize idx = find(key);
        return (idx != INVALID) ? &m_buckets[idx].value : nullptr;
    }

    bool contains(const K& key) const { return get(key) != nullptr; }

    /* Remove key. Returns true if the key existed. */
    bool remove(const K& key) {
        if (!m_buckets) return false;
        usize idx = find(key);
        if (idx == INVALID) return false;
        m_buckets[idx].used    = false;
        m_buckets[idx].deleted = true;
        --m_size;
        return true;
    }

    usize size()     const { return m_size; }
    bool  is_empty() const { return m_size == 0; }

    void clear() {
        /* Reset ALL buckets — including tombstones (deleted=true) —
           otherwise accumulated tombstones degrade future probe chains. */
        for (usize i = 0; i < m_cap; ++i)
            m_buckets[i] = Bucket{};
        m_size = 0;
    }

    /* Iterate over all live entries: fn(const K&, V&) */
    template<typename Fn>
    void for_each(Fn fn) {
        for (usize i = 0; i < m_cap; ++i)
            if (m_buckets[i].used)
                fn(m_buckets[i].key, m_buckets[i].value);
    }
    template<typename Fn>
    void for_each(Fn fn) const {
        for (usize i = 0; i < m_cap; ++i)
            if (m_buckets[i].used)
                fn(m_buckets[i].key, m_buckets[i].value);
    }

private:
    static constexpr usize INVALID       = ~(usize)0;
    static constexpr usize INITIAL_CAP   = 8;
    static constexpr usize LOAD_NUM      = 7;   /* load threshold = 0.70 */
    static constexpr usize LOAD_DEN      = 10;

    struct Bucket {
        K    key     {};
        V    value   {};
        bool used    { false };
        bool deleted { false };
    };

    Bucket* m_buckets { nullptr };
    usize   m_cap     { 0 };
    usize   m_size    { 0 };

    H       m_hasher  {};

    usize find(const K& key) const {
        usize idx = m_hasher(key) % m_cap;
        for (usize i = 0; i < m_cap; ++i) {
            usize b = (idx + i) % m_cap;
            if (!m_buckets[b].used && !m_buckets[b].deleted)
                return INVALID;        /* empty slot — key not present */
            if (m_buckets[b].used && m_buckets[b].key == key)
                return b;
        }
        return INVALID;
    }

    usize probe(const K& key) {
        usize idx = m_hasher(key) % m_cap;
        usize first_deleted = INVALID;
        for (usize i = 0; i < m_cap; ++i) {
            usize b = (idx + i) % m_cap;
            if (m_buckets[b].used && m_buckets[b].key == key)
                return b;  /* overwrite existing */
            if (!m_buckets[b].used && !m_buckets[b].deleted) {
                return (first_deleted != INVALID) ? first_deleted : b;
            }
            if (m_buckets[b].deleted && first_deleted == INVALID)
                first_deleted = b;
        }
        /* Map is full (shouldn't happen after ensure_capacity). */
        return (first_deleted != INVALID) ? first_deleted : (idx % m_cap);
    }

    void ensure_capacity() {
        if (m_cap == 0) {
            allocate(INITIAL_CAP);
            return;
        }
        if (m_size * LOAD_DEN >= m_cap * LOAD_NUM)
            rehash(m_cap * 2);
    }

    void allocate(usize cap) {
        m_buckets = static_cast<Bucket*>(kmalloc(sizeof(Bucket) * cap));
        if (!m_buckets) return;  /* OOM: m_cap stays 0, map stays empty */
        m_cap = cap;
        for (usize i = 0; i < m_cap; ++i)
            m_buckets[i] = Bucket{};
    }

    void rehash(usize new_cap) {
        Bucket* old = m_buckets;
        usize   old_cap = m_cap;
        allocate(new_cap);
        m_size = 0;
        for (usize i = 0; i < old_cap; ++i)
            if (old[i].used) set(old[i].key, old[i].value);
        kfree(old);
    }

    void clear_storage() {
        if (m_buckets) { kfree(m_buckets); m_buckets = nullptr; }
        m_cap = 0; m_size = 0;
    }
};

} /* namespace Greg */

#endif /* GREG_HASHMAP_HPP */
