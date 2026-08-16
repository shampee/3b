// libclang-based extraction -- see translate.h's CUnit/CType/... comments
// for the shapes this builds. One parse (cwalk_extract), one cursor-visitor
// pass, covering functions/typedefs/structs/unions AND `#define` constants
// alike -- constants are just another cursor kind (CXCursor_MacroDefinition)
// once the translation unit is parsed with
// CXTranslationUnit_DetailedPreprocessingRecord, so they get the exact same
// "is this cursor actually inside one of MY configured headers" location
// filter as every declaration, for free.
//
// (The doc's plan called for a separate `clang -E -dM` text-dump pass for
// constants, diffed against an empty-file baseline to isolate "defined by
// MY headers" -- tried that first, but macro text loses all location info
// after preprocessing, so the baseline-diff can only reject macros the
// COMPILER predefines, not ones a target header pulls in TRANSITIVELY from
// some other system header it #includes -- e.g. gl.h dragging in half of
// glibc's stdint.h/features.h macros right alongside the real GL_ constants,
// with no way to tell them apart after the fact. The cursor approach below
// doesn't have that problem: a macro cursor's location is real, so it gets
// filtered by cursor_in_target_headers exactly like a FunctionDecl would.)
#include "translate.h"
#include "clang-c/Index.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>

////////////////////////////////
//~ Shared helpers

static String8
str8_from_cxstring(Arena* arena, CXString s) {
  const char* c = clang_getCString(s);
  u64         len = c ? strlen(c) : 0;
  u8*         buf = push_array(arena, u8, len == 0 ? 1 : len);
  if (len) MemoryCopy(buf, c, len);
  clang_disposeString(s);
  return str8(buf, len);
}

// A type's C SPELLING includes both the tag keyword (`clang_getTypeSpelling`
// on a bare `struct Foo` always says "struct Foo" in C, unlike C++) AND,
// separately, a literal "const " prefix whenever the type is const-qualified
// (`clang_getTypeSpelling` on `const GLfloat` says exactly that, text and
// all) -- but CType already tracks constness as its OWN `is_const` flag
// (mirrors 3b's TypeRef.is_const), and config/type-map entries and record
// names are all bare names (`Foo`, `GLfloat`) with no qualifier baked in.
// So every Named CType strips BOTH prefixes on the way in: leaving "const "
// in the name string here would double up with the separately-tracked flag
// the moment anything (print_ctype, emit.c) prints "const " ITSELF based on
// is_const.
static String8
strip_tag_keyword(String8 spelling) {
  String8 const_prefix = str8_lit("const ");
  if (str8_starts_with(spelling, const_prefix, 0)) {
    spelling = str8(spelling.str + const_prefix.size, spelling.size - const_prefix.size);
  }
  String8 prefixes[3] = { str8_lit("struct "), str8_lit("union "), str8_lit("enum ") };
  foreach_index(i, 3) {
    if (str8_starts_with(spelling, prefixes[i], 0)) {
      return str8(spelling.str + prefixes[i].size, spelling.size - prefixes[i].size);
    }
  }
  return spelling;
}

// Builds the `-DNAME=VALUE` argv entries active for `platform` (`:common`
// always, plus the one matching `--platform`, if any) -- shared between
// the libclang parse args and the `clang -E -dM` command line so both
// passes see an identical preprocessor environment.
static void
push_active_defines(Arena* arena, Config* cfg, const char* platform, String8List* out) {
  foreach_index(gi, dyn_count(cfg->define_groups)) {
    DefineGroup* g = &cfg->define_groups[gi];
    b32 active = str8_match_lit("common", g->platform, StringMatchFlag_CaseInsensitive) ||
                 (platform && str8_match(g->platform, str8_cstring((char*)platform), StringMatchFlag_CaseInsensitive));
    if (!active) continue;
    foreach_index(di, dyn_count(g->defines)) {
      DefineKV* kv = &g->defines[di];
      str8_list_pushf(arena, out, "-D%.*s=%.*s", str8_varg(kv->name), str8_varg(kv->value));
    }
  }
}

static String8
build_synthetic_header(Arena* arena, Config* cfg) {
  String8List includes = {0};
  foreach_index(i, dyn_count(cfg->headers)) {
    str8_list_pushf(arena, &includes, "#include \"%.*s\"\n", str8_varg(cfg->headers[i]));
  }
  StringJoin no_join = {0};
  return str8_list_join(arena, &includes, &no_join);
}

////////////////////////////////
//~ cwalk_extract -- functions / typedefs / structs / unions

typedef struct WalkCtx {
  Arena*  arena;
  Config* cfg;
  CUnit*  unit;
} WalkCtx;

static CType type_from_cxtype(Arena* arena, CXType t, CUnit* unit);

static enum CXChildVisitResult
visit_field(CXCursor c, CXCursor parent, CXClientData client_data) {
  (void)parent;
  struct { Arena* arena; CUnit* unit; CRecord* rec; }* ctx = client_data;
  enum CXCursorKind kind = clang_getCursorKind(c);
  if (kind == CXCursor_FieldDecl) {
    CField f = {0};
    f.name        = str8_from_cxstring(ctx->arena, clang_getCursorSpelling(c));
    f.type        = type_from_cxtype(ctx->arena, clang_getCursorType(c), ctx->unit);
    f.is_bitfield = clang_Cursor_isBitField(c) != 0;
    if (f.is_bitfield) ctx->rec->bitfield_count += 1;
    // A C11 anonymous member (`union { int i; float f; };`) is a FieldDecl with
    // no name. It is counted, not captured: there is nothing to name it in the
    // emitted `[name type ...]` field vector, so its bytes silently vanish from
    // the mirror and every later field lands at the wrong offset. Counting it
    // is what lets verify_record_layouts name the cause rather than just report
    // a size that doesn't add up.
    if (f.name.size == 0) {
      ctx->rec->anon_member_count += 1;
      return CXChildVisit_Continue;
    }
    // Bits, not bytes -- and negative for the error cases (a dependent or
    // incomplete type), which layout_unknown already covers.
    long long bit_offset = clang_Cursor_getOffsetOfField(c);
    f.c_offset = (bit_offset >= 0) ? (u64)(bit_offset / 8) : 0;
    dyn_push(ctx->arena, ctx->rec->fields, f);
  } else if (kind == CXCursor_PackedAttr) {
    ctx->rec->is_packed = true;
  }
  return CXChildVisit_Continue;
}

// Records what libclang makes of the real C record's size and alignment, for
// verify_record_layouts to compare the emitted mirror against. Both calls
// return a negative CXTypeLayoutError rather than a size for a type clang
// won't measure; that is not an extraction failure, just nothing to check.
static void
capture_record_layout(CXCursor decl, CRecord* rec) {
  CXType    t    = clang_getCursorType(decl);
  long long size = clang_Type_getSizeOf(t);
  long long algn = clang_Type_getAlignOf(t);
  if (size < 0 || algn <= 0) {
    rec->layout_unknown = true;
    return;
  }
  rec->c_size  = (u64)size;
  rec->c_align = (u64)algn;
}

// NOTE: explicit `(align N ...)` extraction is a known gap -- libclang's
// stable C API doesn't expose an AlignedAttr's argument directly, only
// clang_Type_getAlignOf's EFFECTIVE alignment (which conflates
// user-requested alignment with the platform's natural one). `is_packed`
// is unambiguous (its own attribute cursor kind) so that one's captured;
// `align` is always left at 0 (unset) for now.
static void
collect_fields(Arena* arena, CXCursor decl, CUnit* unit, CRecord* rec) {
  struct { Arena* arena; CUnit* unit; CRecord* rec; } ctx = { arena, unit, rec };
  clang_visitChildren(decl, visit_field, &ctx);
}

static void
capture_record(Arena* arena, CXCursor decl, CUnit* unit) {
  if (!clang_isCursorDefinition(decl)) return; // forward decl only -- opaqueness is captured
                                                // at the POINTER site instead (pointee_is_incomplete)
  String8 name = str8_from_cxstring(arena, clang_getCursorSpelling(decl));
  foreach_index(i, dyn_count(unit->records)) {
    if (str8_match(unit->records[i].name, name, 0)) return; // already captured
  }
  // Cycle guard -- see CUnit.records_in_progress. `records` above can't
  // catch a record that reaches itself mid-capture, because nothing is
  // appended there until collect_fields has returned.
  foreach_index(i, dyn_count(unit->records_in_progress)) {
    if (str8_match(unit->records_in_progress[i], name, 0)) return; // capture already underway
  }
  dyn_push(arena, unit->records_in_progress, name);

  CRecord rec = {0};
  rec.name        = name;
  rec.is_union    = clang_getCursorKind(decl) == CXCursor_UnionDecl;
  rec.is_complete = true;
  collect_fields(arena, decl, unit, &rec);
  capture_record_layout(decl, &rec);
  dyn_push(arena, unit->records, rec);

  // Pop. Strictly LIFO: any nested capture collect_fields started has
  // already popped its own entry by now. (Leaving entries behind would be
  // harmless -- the `records` check above catches anything completed --
  // but then the list wouldn't mean what its name says.)
  dyn_hdr(unit->records_in_progress)->count -= 1;
}

static String8
capture_anon_record(Arena* arena, CXCursor decl, CUnit* unit) {
  CRecord rec = {0};
  rec.is_anonymous = true;
  char namebuf[32];
  int  namelen = snprintf(namebuf, sizeof(namebuf), "Anon%u", unit->next_anon_id);
  unit->next_anon_id += 1;
  u8* namecopy = push_array(arena, u8, (u64)namelen);
  MemoryCopy(namecopy, namebuf, (u64)namelen);
  rec.name        = str8(namecopy, (u64)namelen);
  rec.is_union    = clang_getCursorKind(decl) == CXCursor_UnionDecl;
  rec.is_complete = clang_isCursorDefinition(decl) != 0;
  if (rec.is_complete) {
    collect_fields(arena, decl, unit, &rec);
    capture_record_layout(decl, &rec);
  }
  dyn_push(arena, unit->records, rec);
  return rec.name;
}

// Ensures a NAMED (non-anonymous) record referenced from a target header
// is in unit->records even when it's actually DEFINED in some header
// outside cfg->headers (e.g. a shared platform base header) -- the
// top-level walk alone would otherwise miss it, since it only visits
// cursors physically located inside the configured headers.
// This was a byte-for-byte duplicate of capture_record, which meant the
// self-reference recursion bug documented there existed in two places and
// had to be fixed in both. Delegating keeps the two entry points (top-level
// walk vs. reference-site backfill) named for their callers' intent while
// leaving exactly one implementation to get right.
static void
ensure_named_record_captured(Arena* arena, CXCursor decl, CUnit* unit) {
  capture_record(arena, decl, unit);
}

static enum CXChildVisitResult
visit_enumerator(CXCursor c, CXCursor parent, CXClientData client_data) {
  (void)parent;
  struct { Arena* arena; CEnumerator** enumerators; }* ctx = client_data;
  if (clang_getCursorKind(c) == CXCursor_EnumConstantDecl) {
    CEnumerator m = {0};
    m.name          = str8_from_cxstring(ctx->arena, clang_getCursorSpelling(c));
    long long value = clang_getEnumConstantDeclValue(c); // signed -- covers negative enumerators too
    char      buf[32];
    int       len = snprintf(buf, sizeof(buf), "%lld", value);
    u8*       copy = push_array(ctx->arena, u8, (u64)len);
    MemoryCopy(copy, buf, (u64)len);
    m.value_text = str8(copy, (u64)len);
    dyn_push(ctx->arena, *ctx->enumerators, m);
  }
  return CXChildVisit_Continue;
}

// Named C `enum`s become a CEnum (emit.c turns these into a 3b `(enum Name
// [...])`, same shape the config's `enum-group` already produces from
// `#define`s); anonymous ones (no tag, no typedef name -- just a bare
// `enum { A, B };` used purely to define constants) have no type name for
// anything to reference, so their enumerators are pushed straight into
// unit->constants instead, same as a `#define`. Handles both the top-level
// walk (visit_top_level below) and the "referenced from a type position but
// defined outside cfg->headers" case (type_from_cxtype's ensure-captured
// call), same dual role ensure_named_record_captured/capture_record split
// serves for structs -- kept as one function here since the two bodies
// would otherwise be identical.
static void
capture_enum(Arena* arena, CXCursor decl, CUnit* unit) {
  if (!clang_isCursorDefinition(decl)) return;
  b32     anon = clang_Cursor_isAnonymous(decl) != 0;
  String8 name = anon ? str8_lit("") : str8_from_cxstring(arena, clang_getCursorSpelling(decl));
  if (!anon) {
    foreach_index(i, dyn_count(unit->enums)) {
      if (str8_match(unit->enums[i].name, name, 0)) return; // already captured
    }
  }
  CEnumerator* enumerators = NULL;
  struct { Arena* arena; CEnumerator** enumerators; } ctx = { arena, &enumerators };
  clang_visitChildren(decl, visit_enumerator, &ctx);

  if (anon) {
    foreach_index(i, dyn_count(enumerators)) {
      CConstant c  = {0};
      c.name       = enumerators[i].name;
      c.value_text = enumerators[i].value_text;
      c.is_hex     = false;
      dyn_push(arena, unit->constants, c);
    }
    return;
  }
  CEnum en = {0};
  en.name        = name;
  en.enumerators = enumerators;
  dyn_push(arena, unit->enums, en);
}

// Fills in a CTypeKind_FunctionPointer result's signature from the
// FunctionProto/FunctionNoProto CXType it points to (the callers below
// already unwrapped the pointer, or the bare-function-type guard case --
// either way `fn_type` here is the function type itself, not a pointer to
// it). `clang_getNumArgTypes` returns -1 for FunctionNoProto (K&R-style,
// no prototype info at all) -- treated as zero params, same as C itself
// would for `void (*)()` used as a declarator.
static void
fill_fn_pointer_signature(Arena* arena, CXType fn_type, CType* out, CUnit* unit) {
  out->fn_is_variadic = clang_isFunctionTypeVariadic(fn_type) != 0;
  out->fn_return       = push_array(arena, CType, 1);
  *out->fn_return       = type_from_cxtype(arena, clang_getResultType(fn_type), unit);

  int nargs = clang_getNumArgTypes(fn_type);
  if (nargs > 0) {
    out->fn_params      = push_array(arena, CType, (u64)nargs);
    out->fn_param_count = (u32)nargs;
    foreach_index(i, (u64)nargs) {
      out->fn_params[i] = type_from_cxtype(arena, clang_getArgType(fn_type, (unsigned)i), unit);
    }
  }

  // Second walk, canonical this time, for its CAPTURE side effects only --
  // the results are deliberately thrown away, since the sugared walk above
  // is what names the signature.
  //
  // type_from_cxtype back-fills a record it reaches (ensure_named_record
  // _captured) only where the reference is STRUCTURALLY a record, which a
  // sugared typedef spelling is not. For a type defined outside cfg->headers
  // a callback signature can be the only reference there is: SDL_HitTest's
  // `const SDL_Point *` is the whole reason SDL_Point lands in the unit, and
  // without it SDL_GetDisplayForPoint gets dropped as unresolvable. Reusing
  // type_from_cxtype rather than open-coding a capture-only walk keeps one
  // implementation of "which types does this type reference".
  CXType canon = clang_getCanonicalType(fn_type);
  type_from_cxtype(arena, clang_getResultType(canon), unit);
  int canon_nargs = clang_getNumArgTypes(canon);
  foreach_index(i, (u64)(canon_nargs > 0 ? canon_nargs : 0)) {
    type_from_cxtype(arena, clang_getArgType(canon, (unsigned)i), unit);
  }
}

// Recursively turns a CXType into a CType. Never canonicalizes for NAMING
// purposes (a `GLenum` param stays "GLenum", not "unsigned int" -- that's
// what lets emit.c's type-map lookup work at all); canonicalization is
// used ONLY internally, to decide which STRUCTURAL case (pointer / array /
// function-pointer / record) a type falls into, since sugar (typedefs)
// can hide any of those.
static CType
type_from_cxtype(Arena* arena, CXType t, CUnit* unit) {
  CType result   = {0};
  result.is_const = clang_isConstQualifiedType(t) != 0;

  if (t.kind == CXType_Void) {
    result.kind = CTypeKind_Void;
    return result;
  }

  if (t.kind == CXType_Pointer) {
    CXType pointee       = clang_getPointeeType(t);
    CXType pointee_canon = clang_getCanonicalType(pointee);
    if (pointee_canon.kind == CXType_FunctionProto || pointee_canon.kind == CXType_FunctionNoProto) {
      result.kind = CTypeKind_FunctionPointer;
      // The canonical form answered "is this a function type", and that is all
      // it is for -- the signature is read back off the SUGARED pointee, per
      // this function's naming rule. Handing the canonical type over instead
      // canonicalizes the return and parameter types too, so a callback
      // typedef'd as `SDL_EGLAttrib *(*)(void*)` came out as `i64*` rather
      // than `EGLAttrib*`, losing exactly the C names type-map/pin-type need.
      // libclang's getResultType/getArgType desugar on their own, so they read
      // a typedef'd function type fine.
      fill_fn_pointer_signature(arena, pointee, &result, unit);
      return result;
    }
    result.kind    = CTypeKind_Pointer;
    result.pointee = push_array(arena, CType, 1);
    *result.pointee = type_from_cxtype(arena, pointee, unit);
    if (pointee_canon.kind == CXType_Void) {
      result.pointee_is_incomplete = true;
    } else if (pointee_canon.kind == CXType_Record) {
      CXCursor decl = clang_getTypeDeclaration(pointee_canon);
      result.pointee_is_incomplete = clang_equalCursors(clang_getCursorDefinition(decl), clang_getNullCursor()) != 0;
    }
    return result;
  }

  if (t.kind == CXType_ConstantArray) {
    result.kind        = CTypeKind_Array;
    result.array_count = (u64)clang_getArraySize(t);
    result.pointee      = push_array(arena, CType, 1);
    *result.pointee      = type_from_cxtype(arena, clang_getArrayElementType(t), unit);
    return result;
  }
  if (t.kind == CXType_IncompleteArray || t.kind == CXType_VariableArray || t.kind == CXType_DependentSizedArray) {
    // No compile-time size (e.g. a `T arr[]` function parameter, which C
    // itself treats as `T*` anyway) -- decay to a pointer, same rule C uses.
    result.kind    = CTypeKind_Pointer;
    result.pointee = push_array(arena, CType, 1);
    *result.pointee = type_from_cxtype(arena, clang_getArrayElementType(t), unit);
    return result;
  }

  if (t.kind == CXType_FunctionProto || t.kind == CXType_FunctionNoProto) {
    result.kind = CTypeKind_FunctionPointer; // bare function type outside a pointer (shouldn't
    fill_fn_pointer_signature(arena, t, &result, unit); // occur in valid param/field/return position, but guard it)
    return result;
  }

  if (t.kind == CXType_Record || t.kind == CXType_Elaborated || t.kind == CXType_Enum) {
    CXCursor decl = clang_getTypeDeclaration(t);
    // CXType_Elaborated isn't exclusively "struct/union/enum spelled with
    // its keyword" -- empirically (this clang version, at least) it can
    // also show up for a plain typedef reference (e.g. `GLfloat`), in
    // which case clang_getTypeDeclaration hands back the TypedefDecl, not
    // a record decl at all. Only treat this as a real record/enum if the
    // decl actually IS one -- otherwise fall through to the generic Named
    // path below, same as any other typedef.
    enum CXCursorKind decl_kind = clang_getCursorKind(decl);
    b32 is_real_record = decl_kind == CXCursor_StructDecl || decl_kind == CXCursor_UnionDecl;
    b32 is_real_enum   = decl_kind == CXCursor_EnumDecl;
    if (is_real_record && clang_Cursor_isAnonymous(decl)) {
      // NOTE: clang_Cursor_isAnonymousRecordDecl is narrower than its name
      // suggests (empirically false even for a genuinely tagless nested
      // struct field) -- clang_Cursor_isAnonymous is the general "this tag
      // has no name" check and is what actually fires here.
      result.kind = CTypeKind_Named;
      result.name = capture_anon_record(arena, decl, unit);
      return result;
    }
    if (is_real_record) ensure_named_record_captured(arena, decl, unit);
    if (is_real_enum && !clang_Cursor_isAnonymous(decl)) capture_enum(arena, decl, unit);
    // fall through -- named records/enums still resolve via the generic
    // spelling path below, same as any other Named type. (A genuinely
    // anonymous enum reaching this position -- `void f(enum {A,B} x)` --
    // is vanishingly rare in real headers and isn't specially handled: it
    // falls through to a synthesized "enum (unnamed ...)" spelling, same
    // as any other unresolvable name, and the owning function gets skipped
    // by emit.c's unresolvable-type check with a clear diagnostic.)
  }

  result.kind = CTypeKind_Named;
  result.name = strip_tag_keyword(str8_from_cxstring(arena, clang_getTypeSpelling(t)));
  return result;
}

static void
capture_function(Arena* arena, CXCursor cursor, CUnit* unit) {
  String8 name = str8_from_cxstring(arena, clang_getCursorSpelling(cursor));
  foreach_index(i, dyn_count(unit->functions)) {
    if (str8_match(unit->functions[i].name, name, 0)) return; // dedupe (e.g. forward decl + definition)
  }
  CFunction fn = {0};
  fn.name          = name;
  fn.return_type   = type_from_cxtype(arena, clang_getCursorResultType(cursor), unit);
  fn.is_variadic   = clang_isFunctionTypeVariadic(clang_getCursorType(cursor)) != 0;
  fn.is_deprecated = clang_getCursorAvailability(cursor) == CXAvailability_Deprecated;

  int argc = clang_Cursor_getNumArguments(cursor);
  foreach_index(i, argc > 0 ? (u64)argc : 0) {
    CXCursor arg = clang_Cursor_getArgument(cursor, (unsigned)i);
    CParam   p    = {0};
    p.name        = str8_from_cxstring(arena, clang_getCursorSpelling(arg));
    p.type        = type_from_cxtype(arena, clang_getCursorType(arg), unit);
    dyn_push(arena, fn.params, p);
  }

  fn.has_function_pointer = fn.return_type.kind == CTypeKind_FunctionPointer;
  foreach_index(i, dyn_count(fn.params)) {
    if (fn.params[i].type.kind == CTypeKind_FunctionPointer) fn.has_function_pointer = true;
  }
  dyn_push(arena, unit->functions, fn);
}

static void
capture_typedef(Arena* arena, CXCursor cursor, CUnit* unit) {
  String8 name = str8_from_cxstring(arena, clang_getCursorSpelling(cursor));
  foreach_index(i, dyn_count(unit->typedefs)) {
    if (str8_match(unit->typedefs[i].name, name, 0)) return;
  }

  CXType underlying = clang_getTypedefDeclUnderlyingType(cursor);

  CTypedef td = {0};
  td.name             = name;
  td.underlying        = type_from_cxtype(arena, underlying, unit);
  td.is_opaque_handle  = td.underlying.kind == CTypeKind_Pointer && td.underlying.pointee_is_incomplete;

  // Two different C idioms both produce a Named CType whose name equals
  // this typedef's own name:
  //  1. `typedef struct { ... } Name;` -- clang names the anonymous record
  //     after Name in this exact pairing (empirically: its RecordDecl
  //     cursor comes back non-anonymous, spelled "Name"), so type_from_cxtype
  //     above already captured the record under `name` and returned a Named
  //     CType pointing right back at it. Emitting a CTypedef here too would
  //     be a redundant `(alias Name Name)` -- genuine dedup, skip it.
  //  2. `typedef struct Name Name;` with NO definition anywhere in the TU --
  //     the doc's OTHER opaque-handle idiom (SDL_Window, SDL_Renderer, ...:
  //     forward-declared only, always used as `Name*`), distinct from the
  //     `typedef struct Opaque *Handle;` pointer-typedef idiom already
  //     handled above via pointee_is_incomplete. No record capture exists
  //     for this one to defer to (ensure_named_record_captured is a no-op
  //     on a forward decl) -- if we skip here like case 1, the name
  //     disappears entirely and every function touching it gets dropped
  //     downstream as "unresolvable". This IS the only place that can
  //     detect the fact, so treat it as an opaque handle instead.
  if (td.underlying.kind == CTypeKind_Named && str8_match(td.underlying.name, name, 0)) {
    CXType canon                    = clang_getCanonicalType(underlying);
    b32    underlying_is_incomplete = false;
    if (canon.kind == CXType_Record) {
      CXCursor decl               = clang_getTypeDeclaration(canon);
      underlying_is_incomplete = clang_equalCursors(clang_getCursorDefinition(decl), clang_getNullCursor()) != 0;
    }
    if (!underlying_is_incomplete) {
      // Case 1 -- genuine dedup. Before dropping the typedef, record on the
      // captured record that its bare name is a legal C spelling: this is the
      // only point in the walk where that is observable. See CRecord.
      foreach_index(i, dyn_count(unit->records)) {
        if (str8_match(unit->records[i].name, name, 0)) { unit->records[i].has_typedef_name = true; break; }
      }
      return;
    }
    td.is_opaque_handle = true;             // case 2 -- fall through, emit as `(alias Name any)`
  }

  dyn_push(arena, unit->typedefs, td);
}

// True when `full_path` names one of the configured headers -- i.e. it IS
// `header`, or ends with `/header` (any directory prefix, since libclang
// resolves "GL/gl.h" to wherever its include search actually found it).
static b32
path_ends_with_header(String8 full_path, String8 header) {
  if (header.size == 0 || full_path.size < header.size) return false;
  String8 tail = str8(full_path.str + full_path.size - header.size, header.size);
  if (!str8_match(tail, header, StringMatchFlag_SlashInsensitive)) return false;
  if (full_path.size == header.size) return true;
  u8 prev = full_path.str[full_path.size - header.size - 1];
  return prev == '/' || prev == '\\';
}

static b32
cursor_in_target_headers(CXCursor c, Config* cfg) {
  CXSourceLocation loc = clang_getCursorLocation(c);
  CXFile           file;
  unsigned         line, col, offset;
  clang_getExpansionLocation(loc, &file, &line, &col, &offset);
  (void)line;
  (void)col;
  (void)offset;
  if (!file) return false;

  CXString filename_s = clang_getFileName(file);
  String8  filename    = str8_cstring((char*)clang_getCString(filename_s));
  b32      hit          = false;
  foreach_index(i, dyn_count(cfg->headers)) {
    if (path_ends_with_header(filename, cfg->headers[i])) {
      hit = true;
      break;
    }
  }
  clang_disposeString(filename_s);
  return hit;
}

// Trims a trailing C numeric suffix (`0xFFu`, `1UL`, `1.5f`, `1e10F`,
// `1.5L`, ...) so atom_is_hex_literal/atom_looks_numeric (which know
// nothing about C's suffix grammar) see a clean literal underneath.
//
// The float `f`/`F` is the one that can't be stripped unconditionally,
// because `f` is equally a hex DIGIT -- taking it off `0x1f` would turn
// one constant into a different, still-valid one (`0x1`), the worst kind
// of wrong. So it only counts as a suffix when what it's attached to
// isn't itself a hex literal, which is exactly the distinction between
// `1.5f` (suffix) and `0x1f` (digit). Integer suffixes need no such care:
// none of u/U/l/L is a hex digit, and `0xFULL` -> `0xF` is right.
static String8
strip_numeric_suffix(String8 s) {
  u64 end = s.size;
  while (end > 0) {
    u8 c = s.str[end - 1];
    if (c == 'u' || c == 'U' || c == 'l' || c == 'L') { end -= 1; } else { break; }
  }
  String8 body = str8(s.str, end);
  if (end > 0 && (s.str[end - 1] == 'f' || s.str[end - 1] == 'F') && !atom_is_hex_literal(body)) {
    body = str8(s.str, end - 1);
  }
  return body;
}

// Real headers routinely spell a wide-integer constant as a wrapper-macro
// CALL rather than a bare literal -- `#define SDL_WINDOW_FULLSCREEN
// SDL_UINT64_C(0x1)`, the same `UINT64_C`/`INT64_C`-family idiom stdint.h
// itself popularized. clang_tokenize gives the raw, UNexpanded replacement
// text (nested macro references aren't followed), so that shows up here as
// the literal source shape `IDENT ( INNER )` -- not "a single literal" by
// the plain-token check above, but a well-known enough wrapper that it's
// worth unwrapping one level rather than dropping the constant entirely.
// Deliberately narrow: exactly one balanced top-level `(...)`, immediately
// preceded by a bare identifier, consuming the ENTIRE remaining text (so
// `(1 << 3)` and `FOO(a)+BAR(b)` are correctly left alone -- neither is
// "one wrapper call around one literal").
static String8
unwrap_single_call_literal(String8 s) {
  u64 i = 0;
  while (i < s.size && (isalnum(s.str[i]) || s.str[i] == '_')) i += 1;
  if (i == 0 || i >= s.size || s.str[i] != '(') return s; // no leading `ident(`
  if (s.str[s.size - 1] != ')') return s;                  // doesn't end the call right at the close paren
  u64 open = i;
  return str8(s.str + open + 1, s.size - open - 2); // strip `ident(` prefix and trailing `)`
}

// `#define NAME ...` -- keeps only bare-literal ones (doc's rule): rejects
// function-like macros and builtins outright via libclang's own
// predicates, then rejects anything whose replacement text doesn't LOOK
// like a single literal (macro-to-macro aliases, string literals,
// parenthesized expressions like `(1 << 3)`) via the same atom_is_hex_literal
// / atom_looks_numeric classifiers the 3b compiler itself uses for numeric
// atoms -- so "does this look like a literal" is decided in exactly one
// place across the whole codebase, not reimplemented here. The one
// exception is unwrap_single_call_literal just above, for the
// `IDENT_C(literal)` wrapper idiom.
static void
capture_macro_constant(Arena* arena, CXCursor cursor, CUnit* unit) {
  if (clang_Cursor_isMacroFunctionLike(cursor)) return;
  if (clang_Cursor_isMacroBuiltin(cursor)) return;

  CXTranslationUnit tu     = clang_Cursor_getTranslationUnit(cursor);
  CXSourceRange      extent = clang_getCursorExtent(cursor);
  CXToken*            tokens = NULL;
  unsigned            num_tokens = 0;
  clang_tokenize(tu, extent, &tokens, &num_tokens);

  ArenaTemp   temp   = arena_temp_begin(ctx_scratch());
  String8List parts   = {0};
  // tokens[0] is the macro's own name -- everything after it is the
  // replacement list.
  for (unsigned i = 1; i < num_tokens; i += 1) {
    CXString sp = clang_getTokenSpelling(tu, tokens[i]);
    str8_list_pushf(temp.arena, &parts, "%s", clang_getCString(sp));
    clang_disposeString(sp);
  }
  StringJoin no_join = {0};
  String8    value    = str8_list_join(temp.arena, &parts, &no_join);

  b32 keep = false;
  b32 is_hex = false;
  String8 val = {0};
  if (value.size > 0) {
    val    = strip_numeric_suffix(value);
    is_hex = atom_is_hex_literal(val);
    keep   = is_hex || atom_looks_numeric(val);
    if (!keep) {
      String8 unwrapped = strip_numeric_suffix(unwrap_single_call_literal(value));
      is_hex = atom_is_hex_literal(unwrapped);
      keep   = is_hex || atom_looks_numeric(unwrapped);
      if (keep) val = unwrapped;
    }
  }
  if (keep) {
    CConstant c  = {0};
    c.name       = str8_from_cxstring(arena, clang_getCursorSpelling(cursor));
    c.value_text = str8_copy(arena, val);
    c.is_hex     = is_hex;
    dyn_push(arena, unit->constants, c);
  }

  clang_disposeTokens(tu, tokens, num_tokens);
  arena_temp_end(&temp);
}

static enum CXChildVisitResult
visit_top_level(CXCursor c, CXCursor parent, CXClientData client_data) {
  (void)parent;
  WalkCtx* ctx = client_data;
  if (!cursor_in_target_headers(c, ctx->cfg)) return CXChildVisit_Continue;
  switch (clang_getCursorKind(c)) {
    case CXCursor_FunctionDecl:     capture_function(ctx->arena, c, ctx->unit); break;
    case CXCursor_StructDecl:
    case CXCursor_UnionDecl:        capture_record(ctx->arena, c, ctx->unit); break;
    case CXCursor_TypedefDecl:      capture_typedef(ctx->arena, c, ctx->unit); break;
    case CXCursor_EnumDecl:         capture_enum(ctx->arena, c, ctx->unit); break;
    case CXCursor_MacroDefinition:  capture_macro_constant(ctx->arena, c, ctx->unit); break;
    default: break;
  }
  return CXChildVisit_Continue;
}

b32
cwalk_extract(Arena* arena, Config* cfg, const char* platform, CUnit* out_unit) {
  MemoryZeroStruct(out_unit);
  ArenaTemp temp = arena_temp_begin(ctx_scratch());

  String8 synthetic = build_synthetic_header(temp.arena, cfg);

  String8List arg_list = {0};
  str8_list_pushf(temp.arena, &arg_list, "-x");
  str8_list_pushf(temp.arena, &arg_list, "c");
  str8_list_pushf(temp.arena, &arg_list, "-I."); // so config-relative headers resolve
                                                     // regardless of the unsaved file's fake path
  push_active_defines(temp.arena, cfg, platform, &arg_list);

  const char** args = NULL;
  for (String8Node* n = arg_list.first; n != NULL; n = n->next) {
    const char* cs = (const char*)cstr_from_str8_temp(n->string); // outlives this loop by design -- see note below
    dyn_push(temp.arena, args, cs);
  }
  // NOTE: cstr_from_str8_temp allocates from ctx_scratch(), which is a
  // DIFFERENT arena from `temp` (itself also carved out of ctx_scratch(),
  // but that's fine -- nested/interleaved temp regions on the same arena
  // are legal as long as they're released in LIFO order, and nothing here
  // pops ctx_scratch() out from under `args` before clang_parseTranslationUnit2 runs below).

  struct CXUnsavedFile unsaved = {0};
  unsaved.Filename = "__3btranslate_unit.h";
  unsaved.Contents = (const char*)synthetic.str;
  unsaved.Length   = (unsigned long)synthetic.size;

  CXIndex index = clang_createIndex(/*excludeDeclsFromPCH*/ 1, /*displayDiagnostics*/ 1);
  CXTranslationUnit tu = NULL;
  enum CXErrorCode err = clang_parseTranslationUnit2(
    index, "__3btranslate_unit.h", args, (int)dyn_count(args),
    &unsaved, 1, CXTranslationUnit_SkipFunctionBodies | CXTranslationUnit_DetailedPreprocessingRecord, &tu);
  if (err != CXError_Success || !tu) {
    fprintf(stderr, "cwalk_extract: clang_parseTranslationUnit2 failed (%d)\n", err);
    if (tu) clang_disposeTranslationUnit(tu);
    clang_disposeIndex(index);
    arena_temp_end(&temp);
    return false;
  }

  WalkCtx ctx = { arena, cfg, out_unit };
  clang_visitChildren(clang_getTranslationUnitCursor(tu), visit_top_level, &ctx);

  clang_disposeTranslationUnit(tu);
  clang_disposeIndex(index);
  arena_temp_end(&temp);
  return true;
}

////////////////////////////////
//~ Debug printer

static void
print_ctype(CType* t) {
  if (t->is_const) printf("const ");
  switch (t->kind) {
    case CTypeKind_Void:            printf("void"); break;
    case CTypeKind_Named:           printf("%.*s", str8_varg(t->name)); break;
    case CTypeKind_Pointer:         print_ctype(t->pointee); printf("%s*", t->pointee_is_incomplete ? "/*opaque*/" : ""); break;
    case CTypeKind_Array:           print_ctype(t->pointee); printf("[%llu]", (unsigned long long)t->array_count); break;
    case CTypeKind_FunctionPointer: {
      printf("(*)(");
      foreach_index(i, t->fn_param_count) {
        if (i > 0) printf(", ");
        print_ctype(&t->fn_params[i]);
      }
      if (t->fn_is_variadic) printf("%s...", t->fn_param_count > 0 ? ", " : "");
      printf(") -> ");
      print_ctype(t->fn_return);
      break;
    }
  }
}

void
cwalk_print_unit(CUnit* unit) {
  printf("records (%llu):\n", (unsigned long long)dyn_count(unit->records));
  foreach_index(i, dyn_count(unit->records)) {
    CRecord* r = &unit->records[i];
    printf("  %s %.*s%s%s {\n", r->is_union ? "union" : "struct", str8_varg(r->name),
           r->is_anonymous ? " (anon)" : "", r->is_packed ? " packed" : "");
    if (!r->is_complete) {
      printf("    <incomplete>\n");
    } else {
      foreach_index(j, dyn_count(r->fields)) {
        printf("    %.*s: ", str8_varg(r->fields[j].name));
        print_ctype(&r->fields[j].type);
        printf("\n");
      }
    }
    printf("  }\n");
  }

  printf("typedefs (%llu):\n", (unsigned long long)dyn_count(unit->typedefs));
  foreach_index(i, dyn_count(unit->typedefs)) {
    CTypedef* t = &unit->typedefs[i];
    printf("  %.*s = ", str8_varg(t->name));
    print_ctype(&t->underlying);
    printf("%s\n", t->is_opaque_handle ? "  [opaque handle]" : "");
  }

  printf("enums (%llu):\n", (unsigned long long)dyn_count(unit->enums));
  foreach_index(i, dyn_count(unit->enums)) {
    CEnum* e = &unit->enums[i];
    printf("  enum %.*s [\n", str8_varg(e->name));
    foreach_index(j, dyn_count(e->enumerators)) {
      printf("    %.*s = %.*s\n", str8_varg(e->enumerators[j].name), str8_varg(e->enumerators[j].value_text));
    }
    printf("  ]\n");
  }

  printf("functions (%llu):\n", (unsigned long long)dyn_count(unit->functions));
  foreach_index(i, dyn_count(unit->functions)) {
    CFunction* f = &unit->functions[i];
    printf("  %.*s(", str8_varg(f->name));
    foreach_index(j, dyn_count(f->params)) {
      if (j > 0) printf(", ");
      printf("%.*s: ", str8_varg(f->params[j].name));
      print_ctype(&f->params[j].type);
    }
    if (f->is_variadic) printf(dyn_count(f->params) > 0 ? ", ..." : "...");
    printf(") -> ");
    print_ctype(&f->return_type);
    printf("%s\n", f->has_function_pointer ? "  [has function-pointer param/return]" : "");
  }

  printf("constants (%llu):\n", (unsigned long long)dyn_count(unit->constants));
  foreach_index(i, dyn_count(unit->constants)) {
    CConstant* c = &unit->constants[i];
    printf("  %.*s = %.*s%s\n", str8_varg(c->name), str8_varg(c->value_text), c->is_hex ? " (hex)" : "");
  }
}
