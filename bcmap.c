// bcmap.c -- generic runtime `Map<K,V>`/`Set<T>` implementation for the
// `.3bs` bytecode VM. See bcmap.h's own top-of-file note for the types
// this builds on.
//
// codegen.c's native backend instantiates a separately-compiled C hash table
// per (K,V) monomorphization, via runtime/bbb_hashtable.h's
// bbb_DEFINE_HASHMAP/HASHSET macros expanded once per concrete type pair, with
// an emitted key-specific hash/eq pair. The VM has no per-type codegen step at
// runtime -- bcgen.c already ran -- and needs none: lower_hashtable_type
// restricts a KEY to one of only 11 kinds (numeric-ish primitives, bool, char,
// string -- see BcMapKeyKind), a closed set small enough that one hash/equality
// function switching on a runtime BcMapKeyKind is exactly as correct as 11
// generated ones.
//
// The VALUE side needs no type-specific logic at all: map-set/map-get never
// hash or compare a value, only copy its bytes, so a memcpy of `value_size`
// -- computed once at bytecode-compile time via layout_of -- covers every
// value type, structs included.
//
// The open-addressing/linear-probing/tombstone algorithm below mirrors
// bbb_hashtable.h's macros field by field and branch by branch, parameterized
// by a runtime BcHashSlotLayout rather than compile-time C types.
#include "bcmap.h"
#include <string.h>

static u64
bc_round_up_to_multiple(u64 value, u64 multiple) {
  if (multiple == 0) return value;
  u64 rem = value % multiple;
  return rem == 0 ? value : value + (multiple - rem);
}

static BcMapKeyKind
bc_map_key_kind_from_type(TypeRef key_ty) {
  switch (key_ty.kind) {
    case TypeKind_I8:     return BcMapKeyKind_I8;
    case TypeKind_U8:     return BcMapKeyKind_U8;
    case TypeKind_I16:    return BcMapKeyKind_I16;
    case TypeKind_U16:    return BcMapKeyKind_U16;
    case TypeKind_I32:    return BcMapKeyKind_I32;
    case TypeKind_U32:    return BcMapKeyKind_U32;
    case TypeKind_I64:    return BcMapKeyKind_I64;
    case TypeKind_U64:    return BcMapKeyKind_U64;
    case TypeKind_Bool:   return BcMapKeyKind_Bool;
    case TypeKind_Char:   return BcMapKeyKind_Char;
    case TypeKind_String: return BcMapKeyKind_String;
    default:
      xassert(!"bc_map_key_kind_from_type: not one of the 11 kinds lower_hashtable_type allows as "
                "a Map/Set key -- a checker bug, not a real program, if reached");
      return BcMapKeyKind_I64;
  }
}

BcHashSlotLayout
bc_hash_slot_layout(LayoutCache* cache, Checker* ck, TypeRef key_ty, TypeRef* value_ty) {
  BcHashSlotLayout out = {0};
  out.is_set   = value_ty == NULL;
  out.key_kind = bc_map_key_kind_from_type(key_ty);

  Layout key_l = layout_of(cache, ck, key_ty);
  out.key_size   = key_l.size;
  out.key_offset = 0;
  u64 running    = key_l.size;
  u64 max_align  = key_l.align;

  if (!out.is_set) {
    Layout value_l = layout_of(cache, ck, *value_ty);
    out.value_size   = value_l.size;
    out.value_offset = bc_round_up_to_multiple(running, value_l.align);
    running           = out.value_offset + value_l.size;
    if (value_l.align > max_align) max_align = value_l.align;
  }

  // state is a plain 4-byte value (BcHashSlotState), 4-byte aligned --
  // see bcmap.h's own comment on why this doesn't need to match runtime/
  // bbb_hashtable.h's C enum representation.
  u64 state_align = 4;
  out.state_offset = bc_round_up_to_multiple(running, state_align);
  running            = out.state_offset + state_align;
  if (state_align > max_align) max_align = state_align;

  out.slot_align = max_align;
  out.slot_size  = bc_round_up_to_multiple(running, max_align); // tail padding, so slot N+1
                                                                     // starts correctly aligned too
  return out;
}

static u64
bc_hash_mix_u64(u64 x) {
  // splitmix64's finalizer -- same mixing function runtime/bbb_hashtable.h's
  // own bbb_hash_mix_u64 uses (read first, reimplemented here rather than
  // shared, same "not linked into the compiler itself" reasoning
  // BcOp_StrCmp's own comment already established for string comparison).
  x ^= x >> 33; x *= 0xff51afd7ed558ccdull;
  x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ull;
  x ^= x >> 33;
  return x;
}

static u64
bc_map_key_hash(const BcHashSlotLayout* layout, void* key_ptr) {
  if (layout->key_kind == BcMapKeyKind_String) {
    u8* str; u64 size;
    memcpy(&str, (u8*)key_ptr + 0, sizeof(str));
    memcpy(&size, (u8*)key_ptr + 8, sizeof(size));
    // FNV-1a over the raw bytes, then the same finalizer the numeric path
    // uses below -- any decent mixing is fine here (this is a HASH table,
    // not a cryptographic digest); FNV-1a is simple and well-known.
    u64 h = 0xcbf29ce484222325ull;
    foreach_index(i, size) { h ^= (u64)str[i]; h *= 0x100000001b3ull; }
    return bc_hash_mix_u64(h);
  }
  // Numeric-ish/bool/char key: a plain zero-extending read of the key's
  // OWN width into a u64 -- correct for hash/eq CONSISTENCY (the only
  // real requirement) even for a negative signed value, since the same
  // raw bytes are read the same way on both the hash side and the eq
  // side below; there's no cross-width comparison to get wrong (a Map/
  // Set's key type is fixed for its whole lifetime).
  u64 raw = 0;
  memcpy(&raw, key_ptr, layout->key_size);
  return bc_hash_mix_u64(raw);
}

static b32
bc_map_key_eq(const BcHashSlotLayout* layout, void* a_ptr, void* b_ptr) {
  if (layout->key_kind == BcMapKeyKind_String) {
    u8* str_a; u64 size_a; memcpy(&str_a, (u8*)a_ptr + 0, sizeof(str_a)); memcpy(&size_a, (u8*)a_ptr + 8, sizeof(size_a));
    u8* str_b; u64 size_b; memcpy(&str_b, (u8*)b_ptr + 0, sizeof(str_b)); memcpy(&size_b, (u8*)b_ptr + 8, sizeof(size_b));
    return size_a == size_b && (size_a == 0 || memcmp(str_a, str_b, size_a) == 0);
  }
  return memcmp(a_ptr, b_ptr, layout->key_size) == 0;
}

static u8*
bc_hash_slot_at(BcHashInstance* m, const BcHashSlotLayout* layout, u64 index) {
  return (u8*)m->slots + index * layout->slot_size;
}

static BcHashSlotState
bc_hash_slot_state(u8* slot, const BcHashSlotLayout* layout) {
  BcHashSlotState state;
  memcpy(&state, slot + layout->state_offset, sizeof(state));
  return state;
}

static void
bc_hash_slot_set_state(u8* slot, const BcHashSlotLayout* layout, BcHashSlotState state) {
  memcpy(slot + layout->state_offset, &state, sizeof(state));
}

static void
bc_map_ensure_init(Arena* arena, BcHashInstance* m, const BcHashSlotLayout* layout) {
  if (m->slots) return;
  m->capacity = 8;
  // arena_push_zero, not a per-slot state-init loop -- every slot's state
  // lands on BcHashSlotState_Empty for free since that variant is 0 (see
  // bcmap.h's own comment on why this is a real, load-bearing fact, not
  // an incidental convenience).
  m->slots      = arena_push_zero(arena, layout->slot_size * m->capacity, layout->slot_align);
  m->count      = 0;
  m->tombstones = 0;
}

static void bc_map_resize(Arena* arena, BcHashInstance* m, const BcHashSlotLayout* layout, u64 new_cap);

b32
bc_map_set(Arena* arena, BcHashInstance* m, const BcHashSlotLayout* layout, void* key_ptr, void* value_ptr) {
  bc_map_ensure_init(arena, m, layout);
  if ((m->count + m->tombstones) * 2 >= m->capacity) bc_map_resize(arena, m, layout, m->capacity * 2);

  u64 h   = bc_map_key_hash(layout, key_ptr);
  u64 idx = h % m->capacity;
  i64 first_tombstone = -1;
  foreach_index(i, m->capacity) {
    u64 slot_idx = (idx + i) % m->capacity;
    u8* slot     = bc_hash_slot_at(m, layout, slot_idx);
    BcHashSlotState state = bc_hash_slot_state(slot, layout);
    if (state == BcHashSlotState_Empty) {
      if (first_tombstone >= 0) slot = bc_hash_slot_at(m, layout, (u64)first_tombstone);
      memcpy(slot + layout->key_offset, key_ptr, layout->key_size);
      if (!layout->is_set) memcpy(slot + layout->value_offset, value_ptr, layout->value_size);
      bc_hash_slot_set_state(slot, layout, BcHashSlotState_Occupied);
      m->count += 1;
      return true;
    } else if (state == BcHashSlotState_Tombstone) {
      if (first_tombstone < 0) first_tombstone = (i64)slot_idx;
    } else if (bc_map_key_eq(layout, slot + layout->key_offset, key_ptr)) {
      // Already present -- Set: report failure, no mutation (matching
      // runtime/bbb_hashtable.h's own `_add`). Map: overwrite the value,
      // report success (matching its own `_set`) -- the ONE place these
      // two algorithms genuinely diverge, not just a naming difference.
      if (layout->is_set) return false;
      memcpy(slot + layout->value_offset, value_ptr, layout->value_size);
      return true;
    }
  }
  return false;
}

static void
bc_map_resize(Arena* arena, BcHashInstance* m, const BcHashSlotLayout* layout, u64 new_cap) {
  void* old_slots = m->slots;
  u64   old_cap    = m->capacity;
  m->slots      = arena_push_zero(arena, layout->slot_size * new_cap, layout->slot_align);
  m->capacity   = new_cap;
  m->count      = 0;
  m->tombstones = 0;
  foreach_index(i, old_cap) {
    u8* old_slot = (u8*)old_slots + i * layout->slot_size;
    if (bc_hash_slot_state(old_slot, layout) == BcHashSlotState_Occupied) {
      bc_map_set(arena, m, layout, old_slot + layout->key_offset,
                 layout->is_set ? NULL : old_slot + layout->value_offset);
    }
  }
}

void*
bc_map_get(BcHashInstance* m, const BcHashSlotLayout* layout, void* key_ptr) {
  if (!m->slots) return NULL;
  u64 h   = bc_map_key_hash(layout, key_ptr);
  u64 idx = h % m->capacity;
  foreach_index(i, m->capacity) {
    u8* slot = bc_hash_slot_at(m, layout, (idx + i) % m->capacity);
    BcHashSlotState state = bc_hash_slot_state(slot, layout);
    if (state == BcHashSlotState_Empty) return NULL;
    if (state == BcHashSlotState_Occupied && bc_map_key_eq(layout, slot + layout->key_offset, key_ptr)) {
      return slot + layout->value_offset; // meaningless for a Set (no value field) -- callers only
                                             // ever check non-NULL in that case, see bcmap.h's comment
    }
  }
  return NULL;
}

b32
bc_map_remove(BcHashInstance* m, const BcHashSlotLayout* layout, void* key_ptr) {
  if (!m->slots) return false;
  u64 h   = bc_map_key_hash(layout, key_ptr);
  u64 idx = h % m->capacity;
  foreach_index(i, m->capacity) {
    u8* slot = bc_hash_slot_at(m, layout, (idx + i) % m->capacity);
    BcHashSlotState state = bc_hash_slot_state(slot, layout);
    if (state == BcHashSlotState_Empty) return false;
    if (state == BcHashSlotState_Occupied && bc_map_key_eq(layout, slot + layout->key_offset, key_ptr)) {
      bc_hash_slot_set_state(slot, layout, BcHashSlotState_Tombstone);
      m->count      -= 1;
      m->tombstones += 1;
      return true;
    }
  }
  return false;
}
