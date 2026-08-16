////////////////////////////////
//~ Monomorphized hash tables (Map<K,V> / Set<T>)
//
// Open-addressing with linear probing + tombstone deletes -- the exact same
// algorithm the COMPILER'S OWN internal (String8-key-only, compile-time)
// HashTable uses, reimplemented here as a pair of MACROS so it can be
// instantiated per concrete (K,V)/(K) pair, since C has no generics. This
// mirrors bbb_DEFINE_HANDLE_POOL's own shape: one generic macro, expanded
// once per compiler-chosen Prefix, at the 3b compiler's codegen time (see
// codegen.c's Map/Set instantiation handling).
//
// KeyHashFn/KeyEqFn are function NAMES (not expressions) -- the 3b compiler
// emits a tiny key-specific hash+equality function pair immediately before
// each macro invocation, since hashing/comparing a key genuinely depends on
// its concrete type in a way this macro can't express generically. Only
// numeric-ish keys (integers, bool, char) and `string` are supported --
// see checker.c's Map/Set key-type validation.

typedef enum { bbb_HashSlotState_Empty, bbb_HashSlotState_Occupied, bbb_HashSlotState_Tombstone } bbb_HashSlotState;

// A general-purpose 64-bit mixing hash (splitmix64's finalizer) -- shared by
// every numeric-keyed Map/Set instantiation regardless of the key's exact
// width/signedness (the compiler-emitted KeyHashFn just casts to u64 first).
static inline u64
bbb_hash_mix_u64(u64 x) {
  x ^= x >> 33; x *= 0xff51afd7ed558ccdull;
  x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ull;
  x ^= x >> 33;
  return x;
}

#define bbb_DEFINE_HASHMAP(K, V, Prefix, KeyHashFn, KeyEqFn)                                          \
  typedef struct Prefix##Slot { K key; V value; bbb_HashSlotState state; } Prefix##Slot;              \
  typedef struct Prefix { Prefix##Slot* slots; u64 capacity; u64 count; u64 tombstones; } Prefix;     \
                                                                                                       \
  static inline b32 Prefix##_set(bbb_Arena* arena, Prefix* m, K key, V value);                        \
                                                                                                       \
  static inline void                                                                                  \
  Prefix##_ensure_init(bbb_Arena* arena, Prefix* m) {                                                 \
    if (m->slots) return;                                                                             \
    m->capacity = 8;                                                                                  \
    m->slots    = bbb_push_array(arena, Prefix##Slot, m->capacity);                                   \
    bbb_foreach_index(i, m->capacity) m->slots[i].state = bbb_HashSlotState_Empty;                     \
    m->count      = 0;                                                                                \
    m->tombstones = 0;                                                                                \
  }                                                                                                    \
                                                                                                       \
  static inline void                                                                                  \
  Prefix##_resize(bbb_Arena* arena, Prefix* m, u64 new_cap) {                                         \
    Prefix##Slot* old_slots = m->slots;                                                               \
    u64           old_cap   = m->capacity;                                                            \
    m->slots      = bbb_push_array(arena, Prefix##Slot, new_cap);                                     \
    bbb_foreach_index(i, new_cap) m->slots[i].state = bbb_HashSlotState_Empty;                         \
    m->capacity   = new_cap;                                                                          \
    m->count      = 0;                                                                                \
    m->tombstones = 0;                                                                                \
    bbb_foreach_index(i, old_cap) {                                                                   \
      if (old_slots[i].state == bbb_HashSlotState_Occupied) {                                         \
        Prefix##_set(arena, m, old_slots[i].key, old_slots[i].value);                                 \
      }                                                                                                \
    }                                                                                                  \
  }                                                                                                    \
                                                                                                       \
  static inline b32                                                                                   \
  Prefix##_set(bbb_Arena* arena, Prefix* m, K key, V value) {                                          \
    Prefix##_ensure_init(arena, m);                                                                   \
    if ((m->count + m->tombstones) * 2 >= m->capacity) Prefix##_resize(arena, m, m->capacity * 2);    \
    u64 h                = KeyHashFn(key);                                                            \
    u64 idx              = h % m->capacity;                                                           \
    i64 first_tombstone  = -1;                                                                        \
    bbb_foreach_index(i, m->capacity) {                                                               \
      Prefix##Slot* slot = &m->slots[(idx + i) % m->capacity];                                        \
      if (slot->state == bbb_HashSlotState_Empty) {                                                   \
        if (first_tombstone >= 0) slot = &m->slots[first_tombstone];                                  \
        slot->key   = key;                                                                            \
        slot->value = value;                                                                          \
        slot->state = bbb_HashSlotState_Occupied;                                                     \
        m->count   += 1;                                                                              \
        return true;                                                                                  \
      } else if (slot->state == bbb_HashSlotState_Tombstone) {                                        \
        if (first_tombstone < 0) first_tombstone = (i64)((idx + i) % m->capacity);                    \
      } else if (KeyEqFn(slot->key, key)) {                                                           \
        slot->value = value;                                                                          \
        return true;                                                                                  \
      }                                                                                                \
    }                                                                                                  \
    return false;                                                                                     \
  }                                                                                                    \
                                                                                                       \
  static inline V*                                                                                    \
  Prefix##_get(Prefix* m, K key) {                                                                    \
    if (!m->slots) return NULL;                                                                       \
    u64 h   = KeyHashFn(key);                                                                         \
    u64 idx = h % m->capacity;                                                                        \
    bbb_foreach_index(i, m->capacity) {                                                               \
      Prefix##Slot* slot = &m->slots[(idx + i) % m->capacity];                                        \
      if (slot->state == bbb_HashSlotState_Empty) return NULL;                                        \
      if (slot->state == bbb_HashSlotState_Occupied && KeyEqFn(slot->key, key)) return &slot->value;  \
    }                                                                                                  \
    return NULL;                                                                                      \
  }                                                                                                    \
                                                                                                       \
  static inline b32                                                                                   \
  Prefix##_contains(Prefix* m, K key) { return Prefix##_get(m, key) != NULL; }                        \
                                                                                                       \
  static inline b32                                                                                   \
  Prefix##_remove(Prefix* m, K key) {                                                                 \
    if (!m->slots) return false;                                                                      \
    u64 h   = KeyHashFn(key);                                                                         \
    u64 idx = h % m->capacity;                                                                        \
    bbb_foreach_index(i, m->capacity) {                                                               \
      Prefix##Slot* slot = &m->slots[(idx + i) % m->capacity];                                        \
      if (slot->state == bbb_HashSlotState_Empty) return false;                                       \
      if (slot->state == bbb_HashSlotState_Occupied && KeyEqFn(slot->key, key)) {                     \
        slot->state    = bbb_HashSlotState_Tombstone;                                                 \
        m->count      -= 1;                                                                           \
        m->tombstones += 1;                                                                           \
        return true;                                                                                  \
      }                                                                                                \
    }                                                                                                  \
    return false;                                                                                     \
  }

// Same algorithm as bbb_DEFINE_HASHMAP, minus the value slot -- a Set<T> is
// a Map<T, no-value> (see TypeKind_Set's own comment in 3b.h).
#define bbb_DEFINE_HASHSET(K, Prefix, KeyHashFn, KeyEqFn)                                             \
  typedef struct Prefix##Slot { K key; bbb_HashSlotState state; } Prefix##Slot;                       \
  typedef struct Prefix { Prefix##Slot* slots; u64 capacity; u64 count; u64 tombstones; } Prefix;     \
                                                                                                       \
  static inline b32 Prefix##_add(bbb_Arena* arena, Prefix* m, K key);                                 \
                                                                                                       \
  static inline void                                                                                  \
  Prefix##_ensure_init(bbb_Arena* arena, Prefix* m) {                                                 \
    if (m->slots) return;                                                                             \
    m->capacity = 8;                                                                                  \
    m->slots    = bbb_push_array(arena, Prefix##Slot, m->capacity);                                   \
    bbb_foreach_index(i, m->capacity) m->slots[i].state = bbb_HashSlotState_Empty;                     \
    m->count      = 0;                                                                                \
    m->tombstones = 0;                                                                                \
  }                                                                                                    \
                                                                                                       \
  static inline void                                                                                  \
  Prefix##_resize(bbb_Arena* arena, Prefix* m, u64 new_cap) {                                         \
    Prefix##Slot* old_slots = m->slots;                                                               \
    u64           old_cap   = m->capacity;                                                            \
    m->slots      = bbb_push_array(arena, Prefix##Slot, new_cap);                                     \
    bbb_foreach_index(i, new_cap) m->slots[i].state = bbb_HashSlotState_Empty;                         \
    m->capacity   = new_cap;                                                                          \
    m->count      = 0;                                                                                \
    m->tombstones = 0;                                                                                \
    bbb_foreach_index(i, old_cap) {                                                                   \
      if (old_slots[i].state == bbb_HashSlotState_Occupied) Prefix##_add(arena, m, old_slots[i].key); \
    }                                                                                                  \
  }                                                                                                    \
                                                                                                       \
  static inline b32                                                                                   \
  Prefix##_add(bbb_Arena* arena, Prefix* m, K key) {                                                   \
    Prefix##_ensure_init(arena, m);                                                                   \
    if ((m->count + m->tombstones) * 2 >= m->capacity) Prefix##_resize(arena, m, m->capacity * 2);    \
    u64 h                = KeyHashFn(key);                                                            \
    u64 idx              = h % m->capacity;                                                           \
    i64 first_tombstone  = -1;                                                                        \
    bbb_foreach_index(i, m->capacity) {                                                               \
      Prefix##Slot* slot = &m->slots[(idx + i) % m->capacity];                                        \
      if (slot->state == bbb_HashSlotState_Empty) {                                                   \
        if (first_tombstone >= 0) slot = &m->slots[first_tombstone];                                  \
        slot->key   = key;                                                                            \
        slot->state = bbb_HashSlotState_Occupied;                                                     \
        m->count   += 1;                                                                              \
        return true;                                                                                  \
      } else if (slot->state == bbb_HashSlotState_Tombstone) {                                        \
        if (first_tombstone < 0) first_tombstone = (i64)((idx + i) % m->capacity);                    \
      } else if (KeyEqFn(slot->key, key)) {                                                           \
        return false; /* already present */                                                           \
      }                                                                                                \
    }                                                                                                  \
    return false;                                                                                     \
  }                                                                                                    \
                                                                                                       \
  static inline b32                                                                                   \
  Prefix##_contains(Prefix* m, K key) {                                                               \
    if (!m->slots) return false;                                                                      \
    u64 h   = KeyHashFn(key);                                                                         \
    u64 idx = h % m->capacity;                                                                        \
    bbb_foreach_index(i, m->capacity) {                                                               \
      Prefix##Slot* slot = &m->slots[(idx + i) % m->capacity];                                        \
      if (slot->state == bbb_HashSlotState_Empty) return false;                                       \
      if (slot->state == bbb_HashSlotState_Occupied && KeyEqFn(slot->key, key)) return true;          \
    }                                                                                                  \
    return false;                                                                                     \
  }                                                                                                    \
                                                                                                       \
  static inline b32                                                                                   \
  Prefix##_remove(Prefix* m, K key) {                                                                 \
    if (!m->slots) return false;                                                                      \
    u64 h   = KeyHashFn(key);                                                                         \
    u64 idx = h % m->capacity;                                                                        \
    bbb_foreach_index(i, m->capacity) {                                                               \
      Prefix##Slot* slot = &m->slots[(idx + i) % m->capacity];                                        \
      if (slot->state == bbb_HashSlotState_Empty) return false;                                       \
      if (slot->state == bbb_HashSlotState_Occupied && KeyEqFn(slot->key, key)) {                     \
        slot->state    = bbb_HashSlotState_Tombstone;                                                 \
        m->count      -= 1;                                                                           \
        m->tombstones += 1;                                                                           \
        return true;                                                                                  \
      }                                                                                                \
    }                                                                                                  \
    return false;                                                                                     \
  }
