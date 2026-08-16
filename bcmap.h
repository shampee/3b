#ifndef BCMAP_H
#define BCMAP_H
#include "3b.h"
#include "layout.h"

// Generic runtime implementation of `Map<K,V>`/`Set<T>` for the `.3bs`
// bytecode VM -- see bcmap.c's own top-of-file note for the full design
// (why this is ONE set of generic functions parameterized by a runtime
// BcHashSlotLayout descriptor, rather than a per-monomorphization
// reimplementation the way codegen.c's native backend generates real C
// code per (K,V) pair via runtime/bbb_hashtable.h's macros).

// The 11 key types checker.c's own lower_hashtable_type allows (numeric-
// ish primitives, bool, char, string -- never a struct/pointer/array/
// f32/f64). A small, closed set -- exactly what makes ONE generic hash/eq
// dispatch function correct without needing per-type generated code.
typedef enum BcMapKeyKind {
  BcMapKeyKind_I8, BcMapKeyKind_U8, BcMapKeyKind_I16, BcMapKeyKind_U16,
  BcMapKeyKind_I32, BcMapKeyKind_U32, BcMapKeyKind_I64, BcMapKeyKind_U64,
  BcMapKeyKind_Bool, BcMapKeyKind_Char, BcMapKeyKind_String,
} BcMapKeyKind;

// Empty MUST be 0 -- bc_map_ensure_init/bc_map_resize zero-init a fresh
// slot array via arena_push_zero rather than looping to set each slot's
// state individually, relying on this exact fact (matches
// runtime/bbb_hashtable.h's own bbb_HashSlotState, which is also
// {Empty,Occupied,Tombstone} with Empty first/zero, though this is a
// SEPARATE enum -- the bytecode VM's own Map/Set instances never cross
// into natively-compiled code, so there's no ABI reason these two need to
// literally be the same type, only the same VALUE convention).
typedef enum BcHashSlotState { BcHashSlotState_Empty, BcHashSlotState_Occupied, BcHashSlotState_Tombstone } BcHashSlotState;

// One hash-table slot is `{K key; V value (Map only); BcHashSlotState
// state;}` (Set has no value field at all -- see runtime/bbb_hashtable.h's
// own "Set<T> = Map<T, no-value>" framing) -- this describes ONE concrete
// (key_ty, value_ty) monomorphization's byte layout, computed once at
// bytecode-compile time (bc_hash_slot_layout, in bcmap.c) via ordinary
// sequential-fields/natural-alignment placement (the same algorithm
// layout.c already uses for real structs -- reimplemented here rather
// than shared, since a hash-table slot isn't a real 3b type layout.c's
// own API is scoped to). A compile-time-constant VALUE, not a pointer to
// anything checker/AST-owned -- baked as a boxed constant into the const
// pool the same way a string literal's header already is (see bcgen.c's
// bc_compile_string_literal), so it stays valid for the compiled
// program's entire run.
typedef struct BcHashSlotLayout {
  BcMapKeyKind key_kind;
  u64          key_size;    // bytes: 1/2/4/8 for numeric-ish/bool/char, 16 for string ({ptr,size})
  u64          value_size;  // Map only; 0 for Set (never read when is_set)
  u64          slot_size;
  u64          slot_align;
  u64          key_offset;  // always 0
  u64          value_offset; // Map only
  u64          state_offset;
  b32          is_set;
} BcHashSlotLayout;

// The Map/Set INSTANCE itself -- a real 32-byte-by-value struct at the 3b
// language level (see layout_of's own TypeKind_Map/Set case, layout.c),
// field-for-field identical to what runtime/bbb_hashtable.h's
// bbb_DEFINE_HASHMAP/HASHSET macros generate as their own `Prefix` struct
// ({Slot* slots; u64 capacity, count, tombstones;}) -- NOT shared code
// with that file (this is the bytecode VM's own, separate instantiation),
// but deliberately the SAME shape so a Map/Set-typed register's own
// address (this backend's usual by-value-struct convention -- see
// bcgen.c's top-of-file note) can be reinterpreted as a `BcHashInstance*`
// with no marshaling.
typedef struct BcHashInstance { void* slots; u64 capacity; u64 count; u64 tombstones; } BcHashInstance;

// Computes the slot layout for ONE (key_ty, value_ty) pair -- `value_ty`
// NULL means Set (no value field). `key_ty.kind` must already be one of
// the 11 BcMapKeyKind-representable kinds (checker.c's own
// lower_hashtable_type already enforces this at TYPE-declaration time, so
// by the time a real `map-set`/`set-add`/etc call reaches bcgen.c, the
// Map/Set's own key type is already guaranteed valid).
BcHashSlotLayout bc_hash_slot_layout(LayoutCache* cache, Checker* ck, TypeRef key_ty, TypeRef* value_ty);

// `(map-set arena m key value)` / `(set-add arena s value)` -- `value_ptr`
// is NULL for a Set (layout->is_set decides, not a separate parameter
// check, so bcgen.c can always pass the same call shape regardless of
// which builtin it's compiling). Returns true on success -- for a Map,
// always true (insert or overwrite); for a Set, false if `key_ptr`'s
// value was ALREADY present (matching runtime/bbb_hashtable.h's own
// `_add`'s "no overwrite, report already-present" semantics, genuinely
// different from Map's own "insert or overwrite" -- see bcmap.c's own
// implementation comment on why this ONE branch is the only place the
// two algorithms actually diverge).
b32 bc_map_set(Arena* arena, BcHashInstance* m, const BcHashSlotLayout* layout, void* key_ptr, void* value_ptr);

// `(map-get m key)` -- returns a pointer to the matching slot's VALUE
// (NULL if absent), matching `map-get`'s own checker-declared `T*`
// nilable-pointer return type directly, no wrapping needed. Also backs
// `map-contains?`/`set-contains?` (just checked against NULL, the
// returned pointer itself is meaningless for a Set, whose slots have no
// value field to point at) and the read half of `map-remove`/`set-
// remove`'s own probe (see bc_map_remove below, which re-probes rather
// than sharing this function, since removal ALSO needs to know which
// SLOT matched to mark its state, not just whether one did).
void* bc_map_get(BcHashInstance* m, const BcHashSlotLayout* layout, void* key_ptr);

// `(map-remove m key)` / `(set-remove s value)` -- true if `key_ptr`'s
// value was present (and is now a tombstone), false otherwise.
b32 bc_map_remove(BcHashInstance* m, const BcHashSlotLayout* layout, void* key_ptr);

#endif
