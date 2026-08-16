// Struct and union byte layout -- size, alignment, and field offsets --
// computed here rather than deferred to a downstream C compiler.
//
// The native backend has no need for this: codegen.c emits literal
// sizeof/offsetof/_Alignof text and lets gcc answer at its own compile time.
// The bytecode VM has no downstream compiler, so a field-access opcode needs
// a concrete offset baked in (`LOAD_FIELD <12>`, not `LOAD_FIELD "position"`).
//
// The algorithm is System V layout: sequential fields, natural alignment, no
// reordering. Matching C exactly is a requirement, not a convenience --
// translated bindings like gl and sdl share structs with real C code. The
// language's own `packed`/`align`/`union` attributes map onto the GNU
// attributes cg_struct_decl emits: a field sits at the running offset rounded
// up to its own alignment (unrounded when packed), a union puts every field
// at 0, `align_bytes` can only raise the result, and the final size is
// rounded up to the struct's alignment so arrays stride correctly.
//
// 3b has no generics, so every TypeRef reaching layout_of is a closed,
// concrete shape; layouts are memoized per struct name in LayoutCache.
//
// test/layout_test.c guards against drift from what gcc actually produces,
// cross-checking every case here against both hand-derived values and a
// generated C probe over the struct shape cg_struct_decl would emit.
#include "layout.h"

static u64
round_up_to_multiple(u64 value, u64 multiple) {
  if (multiple <= 1) return value;
  u64 rem = value % multiple;
  return rem == 0 ? value : value + (multiple - rem);
}

// Advances `*running_offset` past one field and returns that field's offset.
// The only copy of this arithmetic: layout_of_struct_body needs the running
// totals and layout_find_field needs one specific field's offset, and sharing
// this keeps the two from ever disagreeing.
static u64
layout_next_field_offset(u64* running_offset, Layout field_layout, b32 is_union, b32 is_packed) {
  if (is_union) return 0; // union members all share storage at offset 0
  u64 field_align = is_packed ? 1 : field_layout.align;
  u64 offset = round_up_to_multiple(*running_offset, field_align);
  *running_offset = offset + field_layout.size;
  return offset;
}

static Layout
layout_of_struct_body(LayoutCache* cache, Checker* ck, TypedNode* decl) {
  u64 running_offset  = 0;
  u64 max_align       = 1;
  u64 max_field_size  = 0; // unions only; for a struct, running_offset already tracks this
  foreach_index(i, decl->struct_decl.field_count) {
    Param* f  = &ck->tast->params[decl->struct_decl.field_first + i];
    Layout fl = layout_of(cache, ck, f->type);
    u64    field_align = decl->struct_decl.is_packed ? 1 : fl.align;
    if (field_align > max_align) max_align = field_align;
    layout_next_field_offset(&running_offset, fl, decl->struct_decl.is_union, decl->struct_decl.is_packed);
    if (decl->struct_decl.is_union && fl.size > max_field_size) max_field_size = fl.size;
  }
  u64 struct_align = decl->struct_decl.is_packed ? 1 : max_align;
  // `aligned(N)` is a floor, not an override: it can only raise alignment,
  // so it still applies on top of packed's alignment of 1.
  if (decl->struct_decl.align_bytes > struct_align) struct_align = decl->struct_decl.align_bytes;
  u64 raw_size   = decl->struct_decl.is_union ? max_field_size : running_offset;
  u64 final_size = round_up_to_multiple(raw_size, struct_align);
  return (Layout){ final_size, struct_align };
}

// Sentinel whose address is the marker; its contents are never read. Parked
// in the cache while a struct's body is being computed, so a cyclic by-value
// embedding asserts instead of recursing forever. Such a struct has no finite
// size and cannot occur in valid code, but the checker doesn't reject it.
static Layout g_layout_in_progress;

static Layout
layout_of_named(LayoutCache* cache, Checker* ck, String8 name) {
  Layout* existing = (Layout*)hashtable_lookup(&cache->by_struct_name, name);
  if (existing == &g_layout_in_progress) {
    xassert(!"layout_of: cyclic by-value struct embedding -- impossible in valid C, this input is malformed");
    return (Layout){0, 0};
  }
  if (existing) return *existing;

  hashtable_insert(cache->arena, &cache->by_struct_name, name, &g_layout_in_progress, /*overwrite*/ true);

  // TypeKind_Named covers structs and enums/flags alike, and the checker
  // keeps them in separate tables, so both have to be consulted. An enum
  // lowers to a plain C `typedef enum`, which gcc sizes as int for any
  // enumerator range 3b currently supports -- hence {4, 4} rather than
  // replaying cg_enum_decl's auto-assignment to find the true maximum. An
  // enum needing a wider representation would need that work done here.
  StructEntry* se = struct_table_lookup(ck, name);
  Layout result;
  if (se) {
    result = layout_of_struct_body(cache, ck, &ck->tast->nodes[se->decl]);
  } else {
    EnumEntry* ee = enum_table_lookup(ck, name);
    xassert(ee); // the checker already required Named types to resolve
    result = (Layout){4, 4};
  }

  Layout* boxed = push_one(cache->arena, Layout);
  *boxed = result;
  hashtable_insert(cache->arena, &cache->by_struct_name, name, boxed, /*overwrite*/ true);
  return result;
}

void
layout_cache_init(LayoutCache* cache, Arena* arena) {
  cache->arena = arena;
  hashtable_init(arena, &cache->by_struct_name, 64);
}

Layout
layout_of(LayoutCache* cache, Checker* ck, TypeRef type) {
  switch (type.kind) {
    case TypeKind_I8: case TypeKind_U8: case TypeKind_Bool: case TypeKind_Char:
      return (Layout){1, 1};
    case TypeKind_I16: case TypeKind_U16:
      return (Layout){2, 2};
    case TypeKind_I32: case TypeKind_U32: case TypeKind_F32:
      return (Layout){4, 4};
    case TypeKind_I64: case TypeKind_U64: case TypeKind_F64:
      return (Layout){8, 8};
    case TypeKind_Pointer:
    case TypeKind_Any:
    case TypeKind_Fn:        // a plain C function pointer
    case TypeKind_Vector:    // compiles to a bare ElementType*
    case TypeKind_ArenaMark: // base.h ArenaMark: {u8* at;}
    case TypeKind_Stream:    // bbb_file.h bbb_Stream: a typedef'd pointer
      return (Layout){8, 8};
    case TypeKind_Handle: // base.h Handle: {u32 index; u32 generation;}
      return (Layout){8, 4};
    case TypeKind_String: // base.h String8: {u8* str; u64 size;}
      return (Layout){16, 8};
    case TypeKind_Arena: // base.h Arena: {void* backend; const ArenaOps* ops;}
      return (Layout){16, 8};
    case TypeKind_Map:
    case TypeKind_Set:
      // The bbb_DEFINE_HASHMAP/HASHSET instance struct: {Slot* slots; u64
      // capacity, count, tombstones;}. Independent of key and value type --
      // those appear only in the slot array, reached through the pointer.
      return (Layout){32, 8};
    case TypeKind_Array: {
      Layout elem   = layout_of(cache, ck, *type.pointee);
      // Tail padding, so the element stays aligned at every index, not just 0.
      u64    stride = round_up_to_multiple(elem.size, elem.align);
      return (Layout){ stride * type.count, elem.align };
    }
    case TypeKind_Named:
      return layout_of_named(cache, ck, type.name);
    default:
      xassert(!"layout_of: not a concrete, sized type (Void/Unresolved is a caller bug)");
      return (Layout){0, 0};
  }
}

// Mirrors checker.c's find_field_recursive: a direct field wins over one
// reached through an anonymous `_` member, hence the two full passes. Shares
// layout_next_field_offset with layout_of_struct_body so a field's offset
// always agrees with the struct size computed from the same arithmetic.
static FieldLayout
layout_find_field(LayoutCache* cache, Checker* ck, TypedNode* decl, String8 field_name, u64 base_offset) {
  u64 running_offset = 0;
  foreach_index(i, decl->struct_decl.field_count) {
    Param* f  = &ck->tast->params[decl->struct_decl.field_first + i];
    Layout fl = layout_of(cache, ck, f->type);
    u64 offset = layout_next_field_offset(&running_offset, fl, decl->struct_decl.is_union, decl->struct_decl.is_packed);
    if (!f->is_anon && str8_match(f->name, field_name, 0)) {
      return (FieldLayout){ true, base_offset + offset, f->type };
    }
  }
  running_offset = 0;
  foreach_index(i, decl->struct_decl.field_count) {
    Param* f  = &ck->tast->params[decl->struct_decl.field_first + i];
    Layout fl = layout_of(cache, ck, f->type);
    u64 offset = layout_next_field_offset(&running_offset, fl, decl->struct_decl.is_union, decl->struct_decl.is_packed);
    if (f->is_anon && f->type.kind == TypeKind_Named) {
      StructEntry* nested = struct_table_lookup(ck, f->type.name);
      if (!nested) continue; // the checker's anon-member validation already reported this
      FieldLayout found = layout_find_field(cache, ck, &ck->tast->nodes[nested->decl], field_name, base_offset + offset);
      if (found.found) return found;
    }
  }
  return (FieldLayout){0};
}

FieldLayout
layout_field_offset(LayoutCache* cache, Checker* ck, StructEntry* se, String8 field_name) {
  return layout_find_field(cache, ck, &ck->tast->nodes[se->decl], field_name, 0);
}
