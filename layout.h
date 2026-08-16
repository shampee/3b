#ifndef LAYOUT_H
#define LAYOUT_H
#include "3b.h"

// Byte size + alignment for a concrete (fully-resolved -- this language has
// no generics, so every type reaching here is a closed, known shape) 3b
// type. The bytecode backend is what needs it: the native backend emits
// sizeof/offsetof/alignof as literal C text for the downstream C compiler to
// resolve (codegen.c's SizeofExpr/AlignofExpr/MemberOffsetExpr cases), while
// a bytecode interpreter has no such compiler to punt to, so a struct
// field-access opcode needs a real byte offset baked in at bytecode-compile
// time. See layout.c's top-of-file note for the algorithm.
typedef struct Layout {
  u64 size;
  u64 align;
} Layout;

// Memoizes layout_of's per-struct-name results -- without this, a deeply
// nested struct graph would recompute its own leaves once per reference.
// Owned by the caller, one per compile (same convention as Checker itself).
typedef struct LayoutCache {
  HashTable by_struct_name; // name -> boxed Layout*, or (transiently, while
                              // that struct's own layout is being computed)
                              // a sentinel used to detect a cyclic by-value
                              // embedding -- see layout.c
  Arena*    arena;          // backs both the HashTable's own storage and
                              // every boxed Layout it holds
} LayoutCache;

void layout_cache_init(LayoutCache* cache, Arena* arena);

// Computes size/align for ANY resolved TypeRef -- primitives, pointer/
// handle/array/string/arena/map/set, and (recursively, memoized) a
// user-defined struct or union. `type` must already be a concrete, sized
// type (post-checker) -- TypeKind_Void/TypeKind_Unresolved reaching here is
// a caller bug, not a real value type, and asserts rather than guessing.
Layout layout_of(LayoutCache* cache, Checker* ck, TypeRef type);

typedef struct FieldLayout {
  b32     found;
  u64     offset; // byte offset from the start of the OUTER struct value --
                    // already summed through any anonymous-member chain, so
                    // this is the one number a bytecode LOAD_FIELD/
                    // STORE_FIELD opcode needs, regardless of how deep the
                    // matched field actually lives.
  TypeRef type;
} FieldLayout;

// Same field resolution checker.c's find_field_recursive does (a direct
// field wins over one reached through an anonymous `_` member -- see
// Param.is_anon), but returns the field's absolute byte offset instead of
// just its Param. Can't just call find_field_recursive directly: that
// function is checker.c-internal (static) and, more fundamentally, doesn't
// track offsets at all -- this has to redo the same recursion so the two
// can never disagree about where a given field actually lands (see
// layout.c's layout_find_field, which shares its per-field offset
// arithmetic with layout_of's own struct-body computation for exactly this
// reason).
FieldLayout layout_field_offset(LayoutCache* cache, Checker* ck, StructEntry* se, String8 field_name);

#endif
