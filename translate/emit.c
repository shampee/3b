// Applies the config's rules -- renames, exclusions, constant grouping,
// force-opaque, outparam tags, name-collision validation -- and emits 3b source
// for a CUnit in one pass, to one output file. See translate.h's CUnit and
// CType for the shapes being translated.
#include "translate.h"
#include "../layout.h" // verify_record_layouts reuses layout_of for 3b primitive sizes,
                        // rather than restating them and risking drift from the compiler
#include <stdarg.h>

////////////////////////////////
//~ Small helpers

static String8
str8_pushf(Arena* arena, const char* fmt, ...) {
  va_list args, args2;
  va_start(args, fmt);
  va_copy(args2, args);
  int needed = vsnprintf(NULL, 0, fmt, args);
  va_end(args);
  if (needed < 0) { va_end(args2); return str8_lit(""); }
  char* buf = push_array(arena, char, (u64)needed + 1);
  vsnprintf(buf, (u64)needed + 1, fmt, args2);
  va_end(args2);
  return str8((u8*)buf, (u64)needed);
}

static String8
strip_prefix_view(String8 name, String8 prefix) {
  if (prefix.size > 0 && str8_starts_with(name, prefix, 0)) {
    return str8(name.str + prefix.size, name.size - prefix.size);
  }
  return name;
}

typedef enum CharKind { CharKind_Lower, CharKind_Upper, CharKind_Digit, CharKind_Other } CharKind;

static CharKind
char_kind(u8 c) {
  if (c >= 'a' && c <= 'z') return CharKind_Lower;
  if (c >= 'A' && c <= 'Z') return CharKind_Upper;
  if (c >= '0' && c <= '9') return CharKind_Digit;
  return CharKind_Other;
}

// PascalCase/camelCase -> lowercased kebab-case. Acronym-aware: a run of
// uppercase stays one word, with the dash before the run's last letter, where
// the next word starts ("XMLParser" -> "xml-parser"). Digits attach to what
// precedes them ("TexImage3D" -> "tex-image3d"), keeping GL's `1D`/`2D`/`3D`
// suffixes as single tokens.
static String8
camel_to_kebab(Arena* arena, String8 s) {
  if (s.size == 0) return s;
  u8* buf = push_array(arena, u8, s.size * 2); // worst case: a dash before every char
  u64 out = 0;
  CharKind prev_kind = CharKind_Other;
  foreach_index(i, s.size) {
    u8       c    = s.str[i];
    CharKind kind = char_kind(c);
    if (kind == CharKind_Other) {
      if (out > 0 && buf[out - 1] != '-') { buf[out] = '-'; out += 1; }
      prev_kind = CharKind_Other;
      continue;
    }
    if (kind == CharKind_Upper && out > 0 && buf[out - 1] != '-') {
      CharKind next_kind = (i + 1 < s.size) ? char_kind(s.str[i + 1]) : CharKind_Other;
      b32 boundary = (prev_kind == CharKind_Lower) ||
                     (prev_kind == CharKind_Upper && next_kind == CharKind_Lower);
      if (boundary) { buf[out] = '-'; out += 1; }
    }
    buf[out] = (kind == CharKind_Upper) ? (u8)(c - 'A' + 'a') : c;
    out += 1;
    prev_kind = kind;
  }
  while (out > 0 && buf[out - 1] == '-') out -= 1; // from a trailing non-alnum char
  return str8(buf, out);
}

////////////////////////////////
//~ Field names

// C headers spell fields `snake_case`, or `camelCase` in the SDL- and
// Apple-flavoured ones. Every other name this tool emits is kebab-case, so a
// field carried over verbatim is the one place a binding makes its users switch
// conventions mid-expression -- `(. ctx pix_fmt)` sitting next to
// `(libav/get-pix-fmt-name ...)`. camel_to_kebab already reads both spellings,
// so fields go through exactly what function names go through.
//
// Renaming is free here in a way it is not for a function: nothing downstream
// of emit_record knows what a field is called. The mirror's layout is
// positional (see "Layout verification"), and the by-value bridge memcpy's
// whole structs rather than naming members. Reserved words are no obstacle
// either -- a field named `flags` emits today and 3b reads it back fine, since
// a field name only ever appears in field position.
//
// A leading underscore is dropped (`_reserved` -> `reserved`), there being no
// 3b identifier starting with `-` to map it to. That, and a `fooBar` sitting
// next to a `foo_bar`, are the two ways C fields can want one 3b name;
// field_name_3b is what resolves them.
static String8
field_name_kebab(Arena* arena, String8 c_name) {
  String8 kebab = camel_to_kebab(arena, c_name);
  return kebab.size > 0 ? kebab : c_name; // a name of nothing but underscores
}

// Field `index`'s emitted name. On a collision the FIRST field to claim a kebab
// name keeps it and the later claimant stays at its C spelling, so a record
// always emits one field per C field and the outcome does not depend on which
// caller asked. `out_claimed_by`, when given, receives the index of the earlier
// field or (u64)-1; emit_record is the one caller that reports.
//
// Pure rather than computed once and cached, so emit_record and
// zero_value_record agree without sharing state. Quadratic in a record's field
// count, which for a one-shot tool over records this size is nothing.
static String8
field_name_3b(Arena* arena, CRecord* r, u64 index, u64* out_claimed_by) {
  if (out_claimed_by) *out_claimed_by = (u64)-1;
  String8 name = field_name_kebab(arena, r->fields[index].name);
  foreach_index(i, index) {
    if (str8_match(field_name_kebab(arena, r->fields[i].name), name, 0)) {
      if (out_claimed_by) *out_claimed_by = i;
      return r->fields[index].name;
    }
  }
  return name;
}

// Reserved 3b form heads, from lower_expr's str8_match_lit chain plus
// package/import/string-match/print, which the compiler recognizes elsewhere,
// and the primitive type names. A generated name colliding with one of these
// would shadow a form or a type, so it is treated as a naming collision and
// reported rather than emitted.
static const char* g_reserved_words[] = {
  "addr", "alias", "align", "alignof", "alloc", "and", "bit-and", "bit-or", "break", "cast", "commit",
  "const", "continue", "cstring", "deref", "do", "dyn-push", "else", "enum", "extern", "false", "flags", "fn", "for",
  "get", "get-in", "if", "let", "match", "member-offset", "member-type", "nil", "not", "nth",
  "or", "packed", "private", "push", "push0", "push-zero", "reinterpret", "return", "scratch", "set",
  "sizeof", "some->", "struct", "true", "type-name", "union", "val", "var", "when", "while", "zero",
  "package", "import", "string-match", "string-len", "print",
  "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "f32", "f64",
  "bool", "char", "string", "void", "any", "arena", "ArenaMark",
};

static b32
is_reserved_name(String8 name) {
  foreach_index(i, ArrayCount(g_reserved_words)) {
    if (str8_match(name, str8_cstring((char*)g_reserved_words[i]), 0)) return true;
  }
  return false;
}

////////////////////////////////
//~ Emit state: the single flat top-level namespace this tool emits into --
// functions, records, aliases, group names and plain vals all share it (see
// try_register_name) -- plus the naming-collision diagnostics.

typedef struct EmitState {
  Config*  cfg;
  CUnit*   unit;
  FILE*    out;
  FILE*    byval_out;   // the generated C bridge header -- see "By-value struct bridge"
  u32      byval_count; // shims written to it; 0 means the file is not worth keeping
  String8* used_names; // dyn array, ctx_perm()
  u32      collision_count;
} EmitState;

// Registers one emitted top-level name. Post-mangling collisions, and
// collisions with a 3b keyword, are errors telling the config author to add a
// rename; nothing is ever silently shadowed.
//
// One flat namespace covers every top-level kind. checker.c keeps separate
// fn_table/struct_table, so 3b may tolerate some cross-kind reuse, but nothing
// here depends on that, and one namespace matches what a reader of the
// generated package would expect.
//
// Returns false, having printed a diagnostic and bumped collision_count, if
// `name` cannot be registered; callers then skip emitting it.
static b32
try_register_name(EmitState* es, String8 name, const char* what) {
  if (is_reserved_name(name)) {
    fprintf(stderr, "translate: '%.*s' collides with a reserved 3b keyword/type name -- add a rename (skipping this %s)\n",
            str8_varg(name), what);
    es->collision_count += 1;
    return false;
  }
  foreach_index(i, dyn_count(es->used_names)) {
    if (str8_match(es->used_names[i], name, 0)) {
      fprintf(stderr, "translate: '%.*s' collides with an earlier declaration -- add a rename (skipping this %s)\n",
              str8_varg(name), what);
      es->collision_count += 1;
      return false;
    }
  }
  dyn_push(ctx_perm(), es->used_names, str8_copy(ctx_perm(), name));
  return true;
}

static b32
is_force_opaque(Config* cfg, String8 name) {
  foreach_index(i, dyn_count(cfg->force_opaque)) {
    if (str8_match(cfg->force_opaque[i], name, 0)) return true;
  }
  return false;
}

// The 3b primitive a `pin-type` entry stands on, or empty when `c_name` is not
// pinned. Consulted ahead of `type-map` and the builtin table, both of which
// would otherwise substitute the primitive at the use site and lose the C name.
static String8
lookup_pin_type(Config* cfg, String8 c_name) {
  foreach_index(i, dyn_count(cfg->pin_type)) {
    if (str8_match(cfg->pin_type[i].c_name, c_name, 0)) return cfg->pin_type[i].b3_name;
  }
  return str8_lit("");
}

static String8
lookup_type_map(Config* cfg, String8 c_name) {
  foreach_index(i, dyn_count(cfg->type_map)) {
    if (str8_match(cfg->type_map[i].c_name, c_name, 0)) return cfg->type_map[i].b3_name;
  }
  return str8_lit("");
}

// Where a final 3b name came from. Only emit_name_report reads this, but it is
// threaded through the name-computing functions rather than recomputed
// alongside them, so the report cannot disagree with what was emitted.
typedef enum NameOrigin {
  NameOrigin_Default, // strip-prefix + the tool's own default casing
  NameOrigin_Rename,  // an explicit `(rename-* ...)` entry
  NameOrigin_Pattern, // a `(rename-*-pattern ...)` rule
} NameOrigin;

// `logical` is a #define constant's or C enum member's name after
// strip-const-prefix, the convention `(rename-const [...])` uses. An empty
// return means no override, so `logical` stands. Explicit renames are checked
// before patterns, which is what lets a hand-written entry correct the names a
// pattern gets wrong (see translate.h's RenamePattern).
static String8
lookup_const_rename_tracking_origin(Config* cfg, String8 logical, NameOrigin* origin) {
  *origin = NameOrigin_Default;
  foreach_index(i, dyn_count(cfg->const_renames)) {
    if (str8_match(cfg->const_renames[i].from, logical, 0)) {
      *origin = NameOrigin_Rename;
      return cfg->const_renames[i].to;
    }
  }
  String8 patterned = rename_patterns_apply(ctx_perm(), cfg->const_rename_patterns, logical);
  if (patterned.size > 0) *origin = NameOrigin_Pattern;
  return patterned;
}

static String8
lookup_const_rename(Config* cfg, String8 logical) {
  NameOrigin origin;
  return lookup_const_rename_tracking_origin(cfg, logical, &origin);
}

// Final 3b spelling for a struct/union/enum/typedef. One function rather than a
// strip_prefix call per site, because the declaration sites
// (emit_record/emit_enum/emit_typedef) and the reference site (type_to_3b_rec's
// Named case) must agree, or the package references types it never declares.
static String8
compute_type_name_tracking_origin(Arena* arena, Config* cfg, String8 c_name, NameOrigin* origin) {
  *origin = NameOrigin_Default;
  foreach_index(i, dyn_count(cfg->type_renames)) {
    if (str8_match(cfg->type_renames[i].from, c_name, 0)) {
      *origin = NameOrigin_Rename;
      return cfg->type_renames[i].to;
    }
  }
  String8 patterned = rename_patterns_apply(arena, cfg->type_rename_patterns, c_name);
  if (patterned.size > 0) {
    *origin = NameOrigin_Pattern;
    return patterned;
  }
  return strip_prefix_view(c_name, cfg->strip_struct_prefix);
}

static String8
compute_type_name(Arena* arena, Config* cfg, String8 c_name) {
  NameOrigin origin;
  return compute_type_name_tracking_origin(arena, cfg, c_name, &origin);
}

////////////////////////////////
//~ CType -> 3b type text

// Builtin C spellings -> 3b primitives, for names no `type-map` entry covers:
// plain `int`/`float` as headers spell them, not through a project typedef.
//
// The `<stdint.h>` family is here rather than left to each config's
// `type-map` because a header reaching one is not a choice its config made:
// khrplatform.h writes `typedef int32_t khronos_int32_t;` only when stdint.h
// was available, so whether `int32_t` surfaces depends on the machine the tool
// ran on. Unmapped, such a name emits `(alias khronos_int32_t int32_t)` -- a 3b
// alias to a name 3b never declares.
//
// `size_t`/`intptr_t` and friends are fixed at 64-bit here, the same LP64
// assumption the `long` entries below already make.
static String8
builtin_c_type_to_3b(String8 c_name) {
  static const struct { const char* c; const char* b3; } table[] = {
    {"void", "void"}, {"int", "i32"}, {"unsigned int", "u32"},
    {"float", "f32"}, {"double", "f64"},
    {"char", "char"}, {"signed char", "i8"}, {"unsigned char", "u8"},
    {"short", "i16"}, {"short int", "i16"}, {"unsigned short", "u16"}, {"unsigned short int", "u16"},
    {"long", "i64"}, {"long int", "i64"}, {"unsigned long", "u64"}, {"unsigned long int", "u64"},
    {"long long", "i64"}, {"long long int", "i64"}, {"unsigned long long", "u64"}, {"unsigned long long int", "u64"},
    {"_Bool", "bool"},
    {"int8_t", "i8"},   {"uint8_t", "u8"},   {"int16_t", "i16"},  {"uint16_t", "u16"},
    {"int32_t", "i32"}, {"uint32_t", "u32"}, {"int64_t", "i64"},  {"uint64_t", "u64"},
    {"size_t", "u64"},  {"ptrdiff_t", "i64"}, {"intptr_t", "i64"}, {"uintptr_t", "u64"},
  };
  foreach_index(i, ArrayCount(table)) {
    if (str8_match(c_name, str8_cstring((char*)table[i].c), 0)) return str8_cstring((char*)table[i].b3);
  }
  return str8_lit("");
}

// True when `name` is a typedef captured as an opaque handle, by either idiom
// (`typedef struct Opaque *Handle;` or `typedef struct Foo Foo;` -- see
// capture_typedef). Both mean the name already stands for a pointer in full, so
// the Pointer case below must not wrap another `*` around a real C pointer to
// one (`SDL_Window *`), exactly as it does not for pointer-to-void.
static b32
is_opaque_handle_named(CUnit* unit, String8 name) {
  foreach_index(i, dyn_count(unit->typedefs)) {
    if (str8_match(unit->typedefs[i].name, name, 0)) return unit->typedefs[i].is_opaque_handle;
  }
  return false;
}

// True when `t` is a function pointer, either directly or as a typedef that
// names one (`GLVULKANPROCNV`). The fn-return case below has to reject both:
// `(fn ...)` may not return a `fn` type, and a name that resolves to one is
// still one as far as lower_fn_type is concerned.
static b32
is_fn_pointer_type(CUnit* unit, CType* t) {
  if (t->kind == CTypeKind_FunctionPointer) return true;
  if (t->kind != CTypeKind_Named) return false;
  foreach_index(i, dyn_count(unit->typedefs)) {
    if (str8_match(unit->typedefs[i].name, t->name, 0)) {
      return unit->typedefs[i].underlying.kind == CTypeKind_FunctionPointer;
    }
  }
  return false;
}

// Whether a pointer to `pointee` collapses to 3b's `any` -- pointer-to-void, or
// a pointer to a type the config or the header made opaque, where the name
// already stands for the whole pointer. Both cases swallow the pointer level,
// which the const handling in the Pointer case has to account for.
static b32
ctype_collapses_to_any(Config* cfg, CUnit* unit, CType* pointee) {
  if (pointee->kind == CTypeKind_Void) return true;
  return pointee->kind == CTypeKind_Named
      && (is_force_opaque(cfg, pointee->name) || is_opaque_handle_named(unit, pointee->name));
}

// `(const T*)` is one self-contained pointer-type expression, and 3b has no
// postfix-pointer syntax for a parenthesized type, so `(const T*)*` cannot be
// written. The sugar therefore only ever goes on the OUTERMOST pointer level --
// which is enough, because lower_type_node makes `(const T*)` mark the
// innermost pointee, matching how C reads `const T *p`. `const AVInputFormat **`
// comes out as `(const InputFormat**)`, i.e. `const AVInputFormat**`, correctly.
//
// What still cannot be written is a qualifier on an INTERMEDIATE level:
// glShaderSource's `const GLchar *const *` keeps its inner const but loses the
// `* const`. That one is benign -- C accepts `const char**` where
// `const char* const*` is wanted, adding a qualifier at the second level being
// legal as long as the first level already carries it.
//
// `is_outermost` tracks the level. Every external caller -- param, field,
// return, array element, alias underlying -- is outermost; the only `false`
// call is the recursive descent into a Pointer's pointee.
static String8
type_to_3b_rec(Arena* arena, Config* cfg, CUnit* unit, CType* t, b32 is_outermost) {
  switch (t->kind) {
    case CTypeKind_Void: return str8_lit("void");
    case CTypeKind_Named: {
      // force-opaque overrides everything, including names extraction never
      // captured a definition for, such as a type from a header outside
      // cfg->headers.
      if (is_force_opaque(cfg, t->name)) return str8_lit("any");
      // Pinned before both substituting lookups: `pin-type` exists so the C
      // name survives to the use site as its own alias.
      if (lookup_pin_type(cfg, t->name).size > 0) return compute_type_name(arena, cfg, t->name);
      String8 mapped = lookup_type_map(cfg, t->name);
      if (mapped.size > 0) return mapped;
      String8 builtin = builtin_c_type_to_3b(t->name);
      if (builtin.size > 0) return builtin;
      // an already-emitted record/enum/alias/opaque-typedef name, renamed and
      // prefix-stripped exactly as its declaration site was
      return compute_type_name(arena, cfg, t->name);
    }
    case CTypeKind_Array: {
      String8 elem = type_to_3b_rec(arena, cfg, unit, t->pointee, true); // fresh top-level type position
      return str8_pushf(arena, "[%.*s %llu]", str8_varg(elem), (unsigned long long)t->array_count);
    }
    case CTypeKind_Pointer: {
      // `any` is C's void*, and a pointer to an opaque handle is the handle.
      // Either way this pointer level is consumed here, so a const on the
      // pointee has nowhere else to go and would otherwise be dropped --
      // `(const any)` is 3b's `const void*`, which is what GLDEBUGPROC's
      // userParam and every `const AVCodecTag*` need to match their C
      // declarations.
      if (ctype_collapses_to_any(cfg, unit, t->pointee)) {
        // Only the outermost level may wrap: `(const any)*` is not writable
        // (no postfix `*` on a parenthesized type), so a deeper level hands
        // back a bare `any` and lets the outermost wrapper below carry the
        // const for the whole chain -- `const void**` is `(const any*)`.
        if (is_outermost && t->pointee->is_const) return str8_lit("(const any)");
        return str8_lit("any");
      }
      String8 pointee_text = type_to_3b_rec(arena, cfg, unit, t->pointee, false); // one level deeper, no longer outermost
      // 3b's `(const T*)` marks the INNERMOST pointee const, the way C reads
      // `const T *p` (lower_type_node), so the wrapper belongs on the OUTERMOST
      // pointer and the flag to consult is the innermost pointee's, not the
      // immediately-next one. Consulting only one level down loses the const on
      // `const AVInputFormat **` and `const char **`, which then warns at every
      // call site taking one.
      //
      // "Innermost" stops early at a level that collapses to `any`, since that
      // is where the chain bottoms out as far as 3b is concerned.
      if (is_outermost) {
        CType* p        = t->pointee;
        b32    innermost_is_const = false;
        for (;;) {
          if (p->kind != CTypeKind_Pointer || p->pointee == NULL) { innermost_is_const = p->is_const; break; }
          if (ctype_collapses_to_any(cfg, unit, p->pointee))      { innermost_is_const = p->pointee->is_const; break; }
          p = p->pointee;
        }
        if (innermost_is_const) return str8_pushf(arena, "(const %.*s*)", str8_varg(pointee_text));
      }
      return str8_pushf(arena, "%.*s*", str8_varg(pointee_text));
    }
    case CTypeKind_FunctionPointer: {
      // `(fn [a0 T0 a1 T1 ...] Ret)` is 3b's function-pointer type
      // (TypeKind_Fn), reusing the `[name type ...]` shape of a real `fn`
      // declaration's params vector. The names are syntactically required but
      // carry no meaning, as in a C function pointer's type, so synthesized
      // `a0`/`a1` names serve. Unsupported signatures -- variadic, or
      // referencing an unresolvable type -- are steered away by
      // type_references_unresolvable before reaching here.
      String8 params = str8_lit("");
      foreach_index(i, t->fn_param_count) {
        String8 ptype = type_to_3b_rec(arena, cfg, unit, &t->fn_params[i], true);
        params = (i == 0)
          ? str8_pushf(arena, "a%llu %.*s", (unsigned long long)i, str8_varg(ptype))
          : str8_pushf(arena, "%.*s a%llu %.*s", str8_varg(params), (unsigned long long)i, str8_varg(ptype));
      }
      // A function pointer returning another function pointer directly -- rare
      // but real C, as in GL's `PFNGLGETVKPROCADDRNVPROC` -- has no 3b
      // spelling. A `fn` type may not return a bare `fn` type (lower_fn_type),
      // and wrapping it in `T*` fails too: codegen appends a trailing `*` to
      // the pointee's text, which for a Fn pointee is already a full
      // `Ret (*)(Args)` declarator, giving malformed C. `any` collapses it as
      // pointer-to-void is collapsed above; callers can cast at the use site.
      // PFNGLGETVKPROCADDRNVPROC returns it via the typedef GLVULKANPROCNV, so
      // this has to see through a name, not just match the structural kind.
      String8 ret = is_fn_pointer_type(unit, t->fn_return)
        ? str8_lit("any")
        : type_to_3b_rec(arena, cfg, unit, t->fn_return, true);
      return str8_pushf(arena, "(fn [%.*s] %.*s)", str8_varg(params), str8_varg(ret));
    }
  }
  return str8_lit("any");
}

static String8
type_to_3b(Arena* arena, Config* cfg, CUnit* unit, CType* t) {
  return type_to_3b_rec(arena, cfg, unit, t, true);
}

////////////////////////////////
//~ Constants

typedef struct GroupMember {
  String8 variant_name;
  String8 decimal_value; // enum/flags values are converted from hex here
} GroupMember;

typedef struct GroupBucket {
  GroupMember* members; // dyn array
} GroupBucket;

typedef struct PlainVal {
  String8 name;
  String8 value_text;
  b32     is_hex;
} PlainVal;

static u64
parse_int_value(String8 text, b32 is_hex) {
  return (u64)strtoull(cstr_from_str8_temp(text), NULL, is_hex ? 16 : 10);
}

// An enum/flags variant's value, an i64 throughout (EnumVariant.value in 3b.h,
// and lower.c's strtoll when it reads the emitted text back). Returns false for
// a constant that cannot survive that trip -- a float, or a bit pattern past
// i64's top like 0xFFFFFFFFFFFFFFFF -- so the caller can degrade it to a plain
// `val` rather than emit a variant whose value saturates.
//
// Signedness matters even where the value fits: rendering through a u64 turns
// `#define FOO -1` into 18446744073709551615, which lower.c's strtoll then
// saturates to i64's max.
static b32
group_member_value(String8 text, b32 is_hex, i64* out) {
  if (atom_classify_numeric(text).is_float) return false;
  const char* c = cstr_from_str8_temp(text);
  errno         = 0;
  if (c[0] == '-') {
    *out = (i64)strtoll(c, NULL, is_hex ? 16 : 10);
    return errno != ERANGE;
  }
  u64 v = (u64)strtoull(c, NULL, is_hex ? 16 : 10);
  if (errno == ERANGE || v > 0x7FFFFFFFFFFFFFFFull) return false;
  *out = (i64)v;
  return true;
}

// The 3b type for a plain `val`, chosen from the value's magnitude rather than
// hardcoded per spelling: a fixed `u32`/`i32` guess is wrong for any constant
// needing more than 32 bits, such as GL_TIMEOUT_IGNORED's
// `0xFFFFFFFFFFFFFFFF`, and the checker rejects the too-narrow annotation.
//
// Signedness still comes from the spelling, which is the intent a C header
// encodes: hex means a bit pattern (u32 -> u64), decimal a number (i32 -> i64).
// So this only ever widens, and every constant that already fit keeps its type.
// A negative hex literal (`-0x1`) takes the signed ladder despite being hex:
// the sign is authored, not part of a bit pattern.
//
// `out_literal` is normally `value_text` unchanged. The exception is a decimal
// constant above i64's max: 3b carries an integer literal as a signed i64, so
// it can only round-trip as a hex bit pattern (int_literal_fits' is_hex case in
// checker.c), and emitting the decimal spelling would produce a binding that
// fails to compile.
//
// A floating-point `#define` is captured too, since atom_looks_numeric accepts
// it, and gets `f64` -- the type C gives an unsuffixed floating constant -- so
// the value survives at full precision rather than being annotated `i32`.
//
// Returns NULL for a value 3b has no integer type wide enough to hold. strtoull
// answers those with u64's max, so typing them anyway would emit a constant
// that compiles cleanly and is silently the wrong number -- the one outcome a
// binding must never produce. The caller skips it instead.
static const char*
plain_val_type(Arena* arena, String8 value_text, b32 is_hex, String8* out_literal) {
  *out_literal = value_text;
  if (atom_classify_numeric(value_text).is_float) return "f64";
  const char* c        = cstr_from_str8_temp(value_text);
  b32         negative = c[0] == '-';
  errno                = 0;
  if (is_hex && !negative) {
    u64 v = parse_int_value(value_text, true);
    if (errno == ERANGE) return NULL;
    return v > 0xFFFFFFFFull ? "u64" : "u32";
  }
  if (negative) {
    i64 v = (i64)strtoll(c, NULL, is_hex ? 16 : 10);
    if (errno == ERANGE) return NULL;
    return v < -2147483648LL ? "i64" : "i32";
  }
  u64 v = (u64)strtoull(c, NULL, 10);
  if (errno == ERANGE) return NULL;
  if (v > 0x7FFFFFFFFFFFFFFFull) { // past i64's top: unsigned, and only hex survives
    *out_literal = str8_pushf(arena, "0x%llX", (unsigned long long)v);
    return "u64";
  }
  return v > 2147483647ull ? "i64" : "i32";
}

// Resolution order for a constant: `exclude-const` first, then enum-group /
// flags-group membership (explicit member list or `match` pattern; matching two
// groups is an error, never a silent pick), then everything still unclaimed
// falls through to a plain `val`.
static void
emit_constants(EmitState* es) {
  Arena*  a      = ctx_perm();
  CUnit*  unit   = es->unit;
  Config* cfg    = es->cfg;
  u64     ngroups = dyn_count(cfg->const_groups);

  GroupBucket* buckets = push_array_zero(a, GroupBucket, ngroups > 0 ? ngroups : 1);
  PlainVal*    plain   = NULL;

  foreach_index(ci, dyn_count(unit->constants)) {
    CConstant* c = &unit->constants[ci];

    b32 excluded = false;
    foreach_index(ei, dyn_count(cfg->excluded_consts)) {
      if (str8_match(cfg->excluded_consts[ei], c->name, 0)) { excluded = true; break; }
    }
    if (excluded) continue;

    String8 logical = strip_prefix_view(c->name, cfg->strip_const_prefix);

    i64 matched_group = -1;
    b32 multi_match    = false;
    foreach_index(gi, ngroups) {
      ConstGroup* g   = &cfg->const_groups[gi];
      b32         hit = false;
      if (g->has_pattern) {
        hit = rename_regex_matches(g->pattern, logical);
      } else {
        foreach_index(mi, dyn_count(g->members)) {
          if (str8_match(g->members[mi], logical, 0)) { hit = true; break; }
        }
      }
      if (hit) {
        if (matched_group >= 0) { multi_match = true; break; }
        matched_group = (i64)gi;
      }
    }

    if (multi_match) {
      fprintf(stderr, "translate: constant '%.*s' matches more than one enum-group/flags-group -- "
                       "skipping (fix your config's match patterns, doc requires no silent pick)\n",
              str8_varg(c->name));
      es->collision_count += 1;
      continue;
    }

    String8 renamed = lookup_const_rename(cfg, logical);
    String8 final_name = renamed.size > 0 ? renamed : logical;

    i64 member_value = 0;
    if (matched_group >= 0 && group_member_value(c->value_text, c->is_hex, &member_value)) {
      GroupMember m   = {0};
      m.variant_name   = str8_copy(a, final_name);
      m.decimal_value  = str8_pushf(a, "%lld", (long long)member_value);
      dyn_push(a, buckets[matched_group].members, m);
    } else {
      // A group match whose value no enum/flags variant can hold (see
      // group_member_value) degrades to a plain `val` rather than being
      // dropped or silently truncated.
      if (matched_group >= 0) {
        fprintf(stderr, "translate: constant '%.*s' (= %.*s) can't be an enum/flags variant -- "
                         "variant values are i64, this is wider or not a whole number; "
                         "emitting it as a plain `val` outside '%.*s'\n",
                str8_varg(c->name), str8_varg(c->value_text),
                str8_varg(cfg->const_groups[matched_group].name));
      }
      PlainVal pv   = {0};
      pv.name        = str8_copy(a, final_name);
      pv.value_text  = str8_copy(a, c->value_text);
      pv.is_hex      = c->is_hex;
      dyn_push(a, plain, pv);
    }
  }

  foreach_index(gi, ngroups) {
    ConstGroup* g = &cfg->const_groups[gi];
    if (dyn_count(buckets[gi].members) == 0) {
      fprintf(stderr, "translate: %s '%.*s' matched 0 constants -- check its member list/pattern\n",
              g->kind == ConstGroupKind_Enum ? "enum-group" : "flags-group", str8_varg(g->name));
      continue;
    }
    if (!try_register_name(es, g->name, g->kind == ConstGroupKind_Enum ? "enum group" : "flags group")) continue;
    fprintf(es->out, "(%s %.*s\n  [", g->kind == ConstGroupKind_Enum ? "enum" : "flags", str8_varg(g->name));
    foreach_index(mi, dyn_count(buckets[gi].members)) {
      if (mi > 0) fprintf(es->out, "\n   ");
      GroupMember* m = &buckets[gi].members[mi];
      fprintf(es->out, "%.*s %.*s", str8_varg(m->variant_name), str8_varg(m->decimal_value));
    }
    fprintf(es->out, "])\n\n");
  }

  foreach_index(pi, dyn_count(plain)) {
    PlainVal* pv = &plain[pi];
    if (!try_register_name(es, pv->name, "constant")) continue;
    // Suffix the literal whenever its type isn't i32, the type a bare literal
    // already defaults to, so the annotation and the literal agree instead of
    // leaving the checker to reconcile them.
    String8     literal  = {0};
    const char* type     = plain_val_type(a, pv->value_text, pv->is_hex, &literal);
    if (type == NULL) {
      fprintf(stderr, "translate: constant '%.*s' (= %.*s) is too wide for any 3b integer type "
                       "-- skipping it\n",
               str8_varg(pv->name), str8_varg(pv->value_text));
      continue;
    }
    b32         suffixed = strcmp(type, "i32") != 0;
    fprintf(es->out, "(val %.*s %s %.*s%s)\n", str8_varg(pv->name), type,
            str8_varg(literal), suffixed ? type : "");
  }
  fprintf(es->out, "\n");
}

////////////////////////////////
//~ Structs / unions / typedefs (aliases, opaque handles)

// Both forward decls resolve further down, among emit_function's helpers.
//
// emit_record reuses type_references_unresolvable to give a field whose type
// name was never declared anywhere the same diagnostic emit_function gives an
// unresolvable param or return. A field, unlike a function, has no "skip this
// one" fallback -- it gets emitted regardless, so this is a warning, and the
// config author hears it from this tool rather than from gcc several steps
// later. A function-pointer field whose signature is supported (non-variadic,
// every param and the return resolvable) is not unresolvable.
static b32 type_references_unresolvable(CUnit* unit, Config* cfg, CType* t);
// See its own comment: only for deciding whether a function-pointer TYPEDEF is
// safe to alias at the top level, never for inline emission.
static b32 type_references_struct_or_enum(CUnit* unit, CType* t);

// How C itself spells the type `name` names, for the pin emit_record/emit_enum
// attach to the mirror they write. A typedef of the same name stands alone; a
// bare tag needs its keyword back, exactly as c_type_text reasons about the
// same question at a reference site. Empty means there is no usable spelling
// (an anonymous record), and no pin is written.
//
// A record captured through `typedef struct { ... } Name;` has no `struct
// Name` tag to fall back on -- writing one would name a fresh incomplete type
// rather than the record -- so CRecord.has_typedef_name is checked too, not
// just the typedef list, which capture_typedef deliberately leaves empty for
// that idiom.
static String8
c_decl_spelling(Arena* arena, CUnit* unit, String8 name, const char* tag_kw) {
  foreach_index(i, dyn_count(unit->typedefs)) {
    if (str8_match(unit->typedefs[i].name, name, 0)) return str8_copy(arena, name);
  }
  foreach_index(i, dyn_count(unit->records)) {
    if (str8_match(unit->records[i].name, name, 0) && unit->records[i].has_typedef_name) {
      return str8_copy(arena, name);
    }
  }
  return str8_pushf(arena, "%s %.*s", tag_kw, str8_varg(name));
}

// The `"c-spelling"` operand, with its leading space, or empty. Every mirror
// gets one: it is what lets codegen cast at the FFI boundary instead of
// handing gcc a `gltf_data*` where the library declared `cgltf_data*`. See
// cg_ffi_c_type.
static String8
pin_operand(Arena* arena, CUnit* unit, String8 name, const char* tag_kw) {
  String8 spelling = c_decl_spelling(arena, unit, name, tag_kw);
  if (spelling.size == 0) return spelling;
  return str8_pushf(arena, " \"%.*s\"", str8_varg(spelling));
}

static void
emit_record(EmitState* es, CRecord* r) {
  String8 name = compute_type_name(ctx_perm(), es->cfg, r->name);
  if (is_force_opaque(es->cfg, r->name)) {
    if (!try_register_name(es, name, "opaque type")) return;
    fprintf(es->out, "(alias %.*s any)\n\n", str8_varg(name));
    return;
  }
  if (!r->is_complete) return; // forward-declared only -- opaqueness handled at the pointer/typedef site
  if (!try_register_name(es, name, r->is_union ? "union" : "struct")) return;

  const char* kw = r->is_union ? "union" : "struct";
  String8 pin = r->is_anonymous ? (String8){0} : pin_operand(ctx_perm(), es->unit, r->name, kw);
  if (r->is_packed) fprintf(es->out, "(packed ");
  fprintf(es->out, "(%s %.*s%.*s\n  [", kw, str8_varg(name), str8_varg(pin));
  foreach_index(i, dyn_count(r->fields)) {
    CField* f     = &r->fields[i];
    if (type_references_unresolvable(es->unit, es->cfg, &f->type)) {
      fprintf(stderr, "translate: %.*s field '%.*s' has an unresolvable type -- "
                       "emitted anyway, but the generated package likely won't compile "
                       "(force-opaque the type, or add the header that declares it)\n",
              str8_varg(r->name), str8_varg(f->name));
    }
    u64     claimed_by = 0;
    String8 fname      = field_name_3b(ctx_perm(), r, i, &claimed_by);
    if (claimed_by != (u64)-1) {
      fprintf(stderr, "translate: %.*s fields '%.*s' and '%.*s' both kebab-case to '%.*s' -- "
                       "'%.*s' keeps its C spelling\n",
              str8_varg(r->name), str8_varg(r->fields[claimed_by].name), str8_varg(f->name),
              str8_varg(field_name_kebab(ctx_perm(), f->name)), str8_varg(fname));
    }
    String8 ftype = type_to_3b(ctx_perm(), es->cfg, es->unit, &f->type);
    if (i > 0) fprintf(es->out, "\n   ");
    fprintf(es->out, "%.*s %.*s", str8_varg(fname), str8_varg(ftype));
  }
  fprintf(es->out, "])");
  if (r->is_packed) fprintf(es->out, ")");
  fprintf(es->out, "\n\n");
}

////////////////////////////////
//~ Layout verification
//
// A translated struct is a MIRROR, not the C type itself: emit_record writes a
// 3b struct that codegen compiles to its own package-prefixed C type
// (`bf_Flags`), and the generated wrapper hands a pointer to THAT straight to a
// C function expecting the original `Flags*`. Every translated binding with a
// struct in its interface therefore rests on one assumption: that the mirror
// and the original have identical layouts.
//
// A violation is silent. The mirror compiles, links, and reads every field past
// the divergence point from the wrong offset. Three constructs break it:
// bitfields (no 3b spelling, so each gets a whole field's storage), C11
// anonymous members (no name to put in the field vector, so they are dropped
// outright), and any type whose 3b mapping is a different width than the C
// original.
//
// The check is a whitelist, not a blocklist: it computes what the emitted
// mirror lays out as and compares that against libclang's measurement of the
// real record, field by field, so an unanticipated construct fails the same way
// a bitfield does. The three named above are recognized only to turn "these
// numbers disagree" into a message naming the construct to go look at.
//
// The 3b side reuses layout.c's own layout_of for primitives rather than
// restating their sizes, so it cannot drift from what the compiler does
// downstream. `cache`/`ck` are unused for non-Named types, which is every type
// reaching that call.

typedef struct MirrorLayout {
  u64 size;
  u64 align;
  b32 known; // false = some type in here has no computable 3b layout, so there
               // is nothing to compare and the check stands down
} MirrorLayout;

#define MIRROR_MAX_DEPTH 32 // a C type graph nested past this is pathological;
                              // also the cycle backstop for typedef chains

static MirrorLayout mirror_layout_of_record(Config* cfg, CUnit* unit, CRecord* r, u32 depth, u64* out_offsets);

static u64
round_up_to(u64 value, u64 multiple) {
  if (multiple <= 1) return value;
  u64 rem = value % multiple;
  return rem == 0 ? value : value + (multiple - rem);
}

// Size/align of a 3b type named by a bare atom (`i32`, `any`, `string`), or
// unknown if the name is not a primitive. `void` is a primitive name but not a
// value type -- layout_of asserts on it -- so it is rejected here first.
static MirrorLayout
mirror_layout_of_3b_primitive(String8 name) {
  MirrorLayout out = {0};
  if (str8_match_lit("void", name, 0) || !is_primitive_type_name(name)) return out;
  Layout l  = layout_of(NULL, NULL, type_ref_from_atom(ctx_perm(), name));
  out.size  = l.size;
  out.align = l.align;
  out.known = true;
  return out;
}

// What the 3b type emit_record will write for `t` lays out as. Mirrors
// type_to_3b_rec's dispatch exactly, case for case -- if that function's
// mapping changes, this one has to change with it, which is why they sit in the
// same file.
static MirrorLayout
mirror_layout_of_ctype(Config* cfg, CUnit* unit, CType* t, u32 depth) {
  MirrorLayout unknown = {0};
  if (depth > MIRROR_MAX_DEPTH) return unknown;
  switch (t->kind) {
    case CTypeKind_Void: return unknown; // not a value type; only valid as a return
    case CTypeKind_Pointer:
    case CTypeKind_FunctionPointer:
      return mirror_layout_of_3b_primitive(str8_lit("any")); // every 3b pointer shape is 8/8
    case CTypeKind_Array: {
      MirrorLayout elem = mirror_layout_of_ctype(cfg, unit, t->pointee, depth + 1);
      if (!elem.known) return unknown;
      MirrorLayout out = {0};
      out.align = elem.align;
      out.size  = round_up_to(elem.size, elem.align) * t->array_count; // stride, as layout_of does
      out.known = true;
      return out;
    }
    case CTypeKind_Named: {
      if (is_force_opaque(cfg, t->name)) return mirror_layout_of_3b_primitive(str8_lit("any"));
      String8 mapped = lookup_type_map(cfg, t->name);
      if (mapped.size > 0) return mirror_layout_of_3b_primitive(mapped);
      String8 builtin = builtin_c_type_to_3b(t->name);
      if (builtin.size > 0) return mirror_layout_of_3b_primitive(builtin);
      // A name extraction captured: recurse into the record, or take an enum's
      // fixed representation (layout.c sizes every 3b enum as int, same as gcc).
      foreach_index(i, dyn_count(unit->records)) {
        if (str8_match(unit->records[i].name, t->name, 0)) {
          return mirror_layout_of_record(cfg, unit, &unit->records[i], depth + 1, NULL);
        }
      }
      foreach_index(i, dyn_count(unit->enums)) {
        if (str8_match(unit->enums[i].name, t->name, 0)) return mirror_layout_of_3b_primitive(str8_lit("i32"));
      }
      foreach_index(i, dyn_count(unit->typedefs)) {
        if (str8_match(unit->typedefs[i].name, t->name, 0)) {
          if (unit->typedefs[i].is_opaque_handle) return mirror_layout_of_3b_primitive(str8_lit("any"));
          return mirror_layout_of_ctype(cfg, unit, &unit->typedefs[i].underlying, depth + 1);
        }
      }
      return unknown; // never captured -- emit_record already warns about this one
    }
  }
  return unknown;
}

// The System V walk layout.c performs on the struct emit_record writes: over
// the CAPTURED fields only, which is the point -- a dropped anonymous member is
// absent here exactly as it will be absent from the emitted 3b. `out_offsets`,
// when given, receives one byte offset per captured field.
static MirrorLayout
mirror_layout_of_record(Config* cfg, CUnit* unit, CRecord* r, u32 depth, u64* out_offsets) {
  MirrorLayout unknown = {0};
  if (depth > MIRROR_MAX_DEPTH || !r->is_complete) return unknown;

  u64 running   = 0;
  u64 max_align = 1;
  u64 max_size  = 0; // unions only
  foreach_index(i, dyn_count(r->fields)) {
    MirrorLayout fl = mirror_layout_of_ctype(cfg, unit, &r->fields[i].type, depth + 1);
    if (!fl.known) return unknown;
    u64 field_align = r->is_packed ? 1 : fl.align;
    if (field_align > max_align) max_align = field_align;
    u64 offset = r->is_union ? 0 : round_up_to(running, field_align);
    if (out_offsets) out_offsets[i] = offset;
    if (r->is_union) {
      if (fl.size > max_size) max_size = fl.size;
    } else {
      running = offset + fl.size;
    }
  }
  u64 align = r->is_packed ? 1 : max_align;
  if (r->align > align) align = r->align;
  MirrorLayout out = {0};
  out.size  = round_up_to(r->is_union ? max_size : running, align);
  out.align = align;
  out.known = true;
  return out;
}

// Renders `a@0 b@4 tail@8` for one side of the comparison.
static String8
field_offset_summary(Arena* arena, CRecord* r, u64* offsets) {
  String8 out = str8_lit("");
  foreach_index(i, dyn_count(r->fields)) {
    out = str8_pushf(arena, "%.*s%s%.*s@%llu", str8_varg(out), (i == 0 ? "" : " "),
                     str8_varg(r->fields[i].name), (unsigned long long)offsets[i]);
  }
  return out;
}

// The cause line, when a recognized construct explains the divergence. Empty
// when none does -- the numbers still stand on their own, and an unexplained
// mismatch is exactly the case this check exists to catch.
static String8
mirror_failure_cause(Arena* arena, CRecord* r) {
  String8 bits = str8_lit("");
  foreach_index(i, dyn_count(r->fields)) {
    if (!r->fields[i].is_bitfield) continue;
    bits = str8_pushf(arena, "%.*s%s'%.*s'", str8_varg(bits), (bits.size == 0 ? "" : ", "),
                      str8_varg(r->fields[i].name));
  }
  if (bits.size > 0) {
    return str8_pushf(arena, "Field(s) %.*s are bitfields, which 3b cannot represent.", str8_varg(bits));
  }
  if (r->anon_member_count > 0) {
    return str8_pushf(arena, "%u C11 anonymous member(s) were dropped -- 3b's field vector has no way "
                              "to name them.", r->anon_member_count);
  }
  return str8_lit("");
}

// One record's mirror-vs-original comparison: the arithmetic both callers want,
// separated from the reporting so the by-value bridge below can ask the same
// question without printing anything.
//
// `compared` false means there was nothing to compare -- the record is never
// emitted as a 3b struct at all, libclang declined to measure it, or no 3b
// layout is computable for it. That is NOT a pass: verify_record_layouts stands
// down on it (the real failure surfaces elsewhere, loudly), while the bridge
// treats it as "not known to match" and declines to forward the struct.
typedef struct MirrorCheck {
  b32  compared;
  b32  matches;
  MirrorLayout mirror;
  u64* offsets; // one per captured field, valid when `compared`
} MirrorCheck;

static MirrorCheck
check_record_mirror(Arena* a, Config* cfg, CUnit* unit, CRecord* r) {
  MirrorCheck out = {0};
  // Nothing to check: never emitted as a struct in the first place (opaque or
  // forward-declared only), or libclang declined to measure the original.
  if (!r->is_complete || r->layout_unknown || is_force_opaque(cfg, r->name)) return out;
  if (dyn_count(r->fields) == 0 && r->anon_member_count == 0) return out;

  out.offsets = push_array(a, u64, dyn_count(r->fields) == 0 ? 1 : dyn_count(r->fields));
  out.mirror  = mirror_layout_of_record(cfg, unit, r, 0, out.offsets);
  // An unresolvable field type leaves nothing to compare. emit_record already
  // warns per field in that case, and the generated package won't compile
  // anyway -- a loud failure, which is what this check is for.
  if (!out.mirror.known) return out;

  out.compared = true;
  out.matches  = out.mirror.size == r->c_size && out.mirror.align == r->c_align;
  foreach_index(fi, dyn_count(r->fields)) {
    if (r->fields[fi].is_bitfield) { out.matches = false; break; } // c_offset isn't a byte boundary
    if (out.offsets[fi] != r->fields[fi].c_offset) { out.matches = false; break; }
  }
  return out;
}

// See this section's own note for what is being verified and why it is a hard
// error rather than a warning: a binding that fails here does not misbehave
// visibly, it silently reads and writes the wrong bytes, so emitting it at all
// is worse than refusing to. Returns false if any record failed; translate.c
// then declines to write the package.
b32
verify_record_layouts(Config* cfg, CUnit* unit) {
  Arena* a       = ctx_perm();
  u32    failures = 0;
  foreach_index(i, dyn_count(unit->records)) {
    CRecord*    r     = &unit->records[i];
    MirrorCheck check = check_record_mirror(a, cfg, unit, r);
    if (!check.compared || check.matches) continue;

    MirrorLayout mirror  = check.mirror;
    u64*         offsets = check.offsets;
    String8 c_summary = str8_lit("");
    foreach_index(fi, dyn_count(r->fields)) {
      CField* f = &r->fields[fi];
      c_summary = f->is_bitfield
        ? str8_pushf(a, "%.*s%s%.*s@bits", str8_varg(c_summary), (fi == 0 ? "" : " "), str8_varg(f->name))
        : str8_pushf(a, "%.*s%s%.*s@%llu", str8_varg(c_summary), (fi == 0 ? "" : " "), str8_varg(f->name),
                     (unsigned long long)f->c_offset);
    }
    String8 cause = mirror_failure_cause(a, r);
    fprintf(stderr,
            "translate: %s '%.*s' cannot be mirrored -- C layout is %llu bytes, align %llu (%.*s); "
            "the generated 3b %s would be %llu bytes, align %llu (%.*s).%s%.*s "
            "Add `(force-opaque [%.*s])` to use it as an opaque pointer instead.\n",
            r->is_union ? "union" : "struct", str8_varg(r->name),
            (unsigned long long)r->c_size, (unsigned long long)r->c_align, str8_varg(c_summary),
            r->is_union ? "union" : "struct",
            (unsigned long long)mirror.size, (unsigned long long)mirror.align,
            str8_varg(field_offset_summary(a, r, offsets)),
            cause.size > 0 ? " " : "", str8_varg(cause),
            str8_varg(r->name)); // `force-opaque` takes a vector, not a bare name -- see config.c
    failures += 1;
  }
  if (failures > 0) {
    fprintf(stderr, "translate: %u struct/union layout mismatch(es) -- refusing to write the package, "
                     "since a mirror that doesn't match its C original corrupts memory silently rather "
                     "than failing visibly\n", failures);
  }
  return failures == 0;
}

////////////////////////////////
//~ Generated-name collision check, the naming sibling of the layout check
// above. That one asks whether the mirror matches its C original byte for
// byte; this one asks whether the two can coexist in one translation unit at
// all.
//
// `3b build` compiles a generated package's C with the real header force-fed
// ahead of it (`(include-first ...)`, see any build.cfg.3b -- it is what stops
// a `private extern` call falling back to a K&R implicit declaration and
// silently promoting its `float` arguments). So the real library's type names
// and this package's mangled `<pkg>_<name>` ones share a scope. Normally they
// cannot clash, since the mangling adds a prefix the C names do not have.
//
// They clash when the package is named after the very prefix the library
// already puts on its own types, and the config then strips that prefix back
// off: `stbtt` + `(strip-struct-prefix "stbtt_")` turns `stbtt_bakedchar` into
// `bakedchar` and then straight back into `stbtt_bakedchar`, which the real
// header has already defined -- `error: conflicting types`, out of a generated
// file naming a type the config author never typed. cgltf hit this in 2026-07
// and was resolved by renaming the whole package to `gltf`.
//
// Fatal, like the layout check, and for the same reason: the package it would
// write cannot be compiled, so writing it is strictly worse than refusing.
static b32
c_type_name_exists(CUnit* unit, String8 name) {
  foreach_index(i, dyn_count(unit->records))  { if (str8_match(unit->records[i].name,  name, 0)) return true; }
  foreach_index(i, dyn_count(unit->enums))    { if (str8_match(unit->enums[i].name,    name, 0)) return true; }
  foreach_index(i, dyn_count(unit->typedefs)) { if (str8_match(unit->typedefs[i].name, name, 0)) return true; }
  return false;
}

// One emitted type: is its mangled C spelling already taken by a real C name?
// `origin` names the config knob responsible, so the message can point at the
// line to change rather than at the symptom.
static u32
check_one_type_name(Config* cfg, CUnit* unit, String8 c_name, const char* what) {
  Arena*     a = ctx_perm();
  NameOrigin origin;
  String8    b3_name = compute_type_name_tracking_origin(a, cfg, c_name, &origin);
  if (b3_name.size == 0) return 0;
  String8 mangled = str8_pushf(a, "%.*s_%.*s", str8_varg(cfg->package_name), str8_varg(b3_name));
  if (!c_type_name_exists(unit, mangled)) {
    // Not a collision, but the same root cause one step short of it: the 3b
    // name still carries the package's own name, so every gcc message about it
    // reads `stbtt_stbtt_bakedchar`. Worth a note, since the obvious next move
    // is `strip-struct-prefix`, which is exactly the collision above.
    String8 self_prefix = str8_pushf(a, "%.*s_", str8_varg(cfg->package_name));
    if (b3_name.size > self_prefix.size && str8_match(str8_prefix(b3_name, self_prefix.size), self_prefix, 0)) {
      fprintf(stderr,
              "translate: note: %s '%.*s' is emitted as C `%.*s`, doubling the package's own name. "
              "`(rename-type [%.*s SomeName])` reads better. Do NOT reach for "
              "`(strip-struct-prefix \"%.*s\")` here -- it would rebuild '%.*s' exactly and collide "
              "with the real header.\n",
              what, str8_varg(c_name), str8_varg(mangled), str8_varg(c_name),
              str8_varg(self_prefix), str8_varg(c_name));
    }
    return 0;
  }
  b32         stripped = origin == NameOrigin_Default && cfg->strip_struct_prefix.size > 0;
  const char* knob = origin == NameOrigin_Rename  ? "the `(rename-type ...)` entry for it"
                   : origin == NameOrigin_Pattern ? "the `(rename-type-pattern ...)` rule matching it"
                   : stripped                     ? "`(strip-struct-prefix ...)`"
                                                  : "the `(package ...)` name";
  fprintf(stderr,
          "translate: %s '%.*s' is emitted as 3b `%.*s`, which mangles to C `%.*s` -- a name the "
          "translated headers already define. `include-first` puts both in one translation unit, so "
          "this is a `conflicting types` compile error, not a warning. Change %s so the two differ.%s\n",
          what, str8_varg(c_name), str8_varg(b3_name), str8_varg(mangled), knob,
          stripped ? " It strips the very prefix `(package ...)` then puts back." : "");
  return 1;
}

// Companion to verify_record_layouts, run over every type the package emits.
// Returns false if any name collided; translate.c then writes no package.
b32
verify_type_names(Config* cfg, CUnit* unit) {
  u32 failures = 0;
  // Each loop mirrors its emit_* function's own decision about whether a
  // declaration is produced at all. A type that is never declared cannot
  // collide with anything, and reporting one would be a false alarm on a config
  // that is already correct.
  foreach_index(i, dyn_count(unit->records)) {
    CRecord* r = &unit->records[i];
    if (r->name.size == 0) continue;
    if (!r->is_complete && !is_force_opaque(cfg, r->name)) continue; // forward-declared only
    failures += check_one_type_name(cfg, unit, r->name, r->is_union ? "union" : "struct");
  }
  foreach_index(i, dyn_count(unit->enums)) {
    failures += check_one_type_name(cfg, unit, unit->enums[i].name, "enum");
  }
  foreach_index(i, dyn_count(unit->typedefs)) {
    CTypedef* td = &unit->typedefs[i];
    if (lookup_type_map(cfg, td->name).size > 0) continue; // substituted at use sites, never declared
    failures += check_one_type_name(cfg, unit, td->name, "typedef");
  }
  if (failures > 0) {
    fprintf(stderr, "translate: %u generated type name(s) collide with the C names they bind to -- "
                     "refusing to write the package, since it could not be compiled\n", failures);
  }
  return failures == 0;
}

// A real C `enum Name { ... }`, emitted in the same shape as a config-driven
// `enum-group`. Anonymous enums never reach here: their enumerators are already
// loose CConstants by the time emit_constants runs (translate.h, CEnum).
static void
emit_enum(EmitState* es, CEnum* e) {
  String8 name = compute_type_name(ctx_perm(), es->cfg, e->name);
  if (is_force_opaque(es->cfg, e->name)) {
    if (!try_register_name(es, name, "opaque type")) return;
    fprintf(es->out, "(alias %.*s any)\n\n", str8_varg(name));
    return;
  }
  if (!try_register_name(es, name, "enum")) return;
  String8 pin = pin_operand(ctx_perm(), es->unit, e->name, "enum");
  fprintf(es->out, "(enum %.*s%.*s\n  [", str8_varg(name), str8_varg(pin));
  foreach_index(i, dyn_count(e->enumerators)) {
    if (i > 0) fprintf(es->out, "\n   ");
    CEnumerator* m       = &e->enumerators[i];
    String8      logical = strip_prefix_view(m->name, es->cfg->strip_const_prefix);
    String8      renamed = lookup_const_rename(es->cfg, logical);
    fprintf(es->out, "%.*s %.*s", str8_varg(renamed.size > 0 ? renamed : logical), str8_varg(m->value_text));
  }
  fprintf(es->out, "])\n\n");
}

static void
emit_typedef(EmitState* es, CTypedef* td) {
  String8 name = compute_type_name(ctx_perm(), es->cfg, td->name);
  if (is_force_opaque(es->cfg, td->name)) {
    if (!try_register_name(es, name, "opaque type")) return;
    fprintf(es->out, "(alias %.*s any)\n\n", str8_varg(name));
    return;
  }
  if (td->is_opaque_handle) {
    if (!try_register_name(es, name, "opaque handle")) return;
    fprintf(es->out, "(alias %.*s any)\n\n", str8_varg(name));
    return;
  }
  if (lookup_type_map(es->cfg, td->name).size > 0) return; // pure naming normalization, substituted at every use site instead
  if (lookup_pin_type(es->cfg, td->name).size > 0) return; // already written, with its C spelling, by emit_pinned_type_aliases
  if (td->underlying.kind == CTypeKind_FunctionPointer &&
      (type_references_unresolvable(es->unit, es->cfg, &td->underlying) ||
       type_references_struct_or_enum(es->unit, &td->underlying))) {
    // Variadic, references a type this tool never captured, or names a
    // struct/enum this unit did capture -- see type_references_struct_or_enum
    // for why that last one is fatal here but harmless inline. None of the
    // three has a `(fn ...)` alias that can safely be emitted. Falling back to
    // an opaque alias rather than dropping the typedef keeps callers that
    // reference the name as a plain Named type compiling.
    if (!try_register_name(es, name, "opaque type (unsupported function pointer)")) return;
    fprintf(stderr, "translate: '%.*s' has an unsupported function-pointer signature (variadic, "
                     "references a type with no captured definition, or names a struct/enum -- "
                     "aliases can't forward-reference those) -- aliased to `any` instead\n",
            str8_varg(td->name));
    fprintf(es->out, "(alias %.*s any)\n\n", str8_varg(name));
    return;
  }

  String8 underlying_text = type_to_3b(ctx_perm(), es->cfg, es->unit, &td->underlying);
  if (!try_register_name(es, name, "type alias")) return;
  fprintf(es->out, "(alias %.*s %.*s)\n\n", str8_varg(name), str8_varg(underlying_text));
}

// A `force-opaque` entry names a type the config author wants to hand around as
// a pointer without a mirror, and emit_record/emit_enum/emit_typedef each write
// its `(alias Name any)` when they reach one. But a name can be force-opaque and
// never reach any of them: a C type declared only as `struct SwsContext;`, with
// its definition in a private header, is dropped by capture_record (forward decl
// only), so nothing was left to walk over and the alias went unwritten.
//
// Every USE still translated correctly -- type_to_3b_rec's Named and Pointer
// cases both consult is_force_opaque directly, so the bindings took and returned
// `any` and worked. Only the name was missing, which meant a caller could not
// name the thing those bindings pass around: `libav/SwsContext` resolved to
// nothing, and the first report of that was gcc rejecting the generated header,
// several steps downstream of the config that caused it.
//
// So the alias is emitted here for whatever is left, which is exactly the
// forward-declared-only case. Names another site already registered are skipped
// silently: they either got their alias there, or lost a collision that site has
// already reported.
static void
emit_unreached_opaques(EmitState* es) {
  foreach_index(i, dyn_count(es->cfg->force_opaque)) {
    String8 c_name = es->cfg->force_opaque[i];
    String8 name   = compute_type_name(ctx_perm(), es->cfg, c_name);
    b32     taken  = false;
    foreach_index(j, dyn_count(es->used_names)) {
      if (str8_match(es->used_names[j], name, 0)) { taken = true; break; }
    }
    if (taken) continue;
    if (!try_register_name(es, name, "opaque type")) continue;
    fprintf(es->out, "(alias %.*s any)\n\n", str8_varg(name));
  }
}

////////////////////////////////
//~ Functions -- a raw (private) extern binding plus a public wrapper. Every
// translated function gets both, uniformly, even a plain rename with no
// out-param transformation: the raw extern's name must match the real C symbol
// for linking to work, which for most real APIs (mixed case, no dashes) rules
// it out as the ergonomic kebab-case public name. So the public name is always
// a separate wrapper calling the raw extern.

// `raw_name` is the C name unmodified -- what both `(rename-func ...)` and
// `(rename-func-pattern ...)` match on. A matching pattern REPLACES the default
// path (strip-func-prefix, then kebab-case) rather than composing with it, so
// `"^av_(.*)$" "{1:kebab}"` and `strip-func-prefix "av_"` do not stack.
static String8
compute_public_fn_name_tracking_origin(EmitState* es, String8 raw_name, NameOrigin* origin) {
  *origin = NameOrigin_Default;
  foreach_index(i, dyn_count(es->cfg->func_renames)) {
    if (str8_match(es->cfg->func_renames[i].from, raw_name, 0)) {
      *origin = NameOrigin_Rename;
      return es->cfg->func_renames[i].to;
    }
  }
  String8 patterned = rename_patterns_apply(ctx_perm(), es->cfg->func_rename_patterns, raw_name);
  if (patterned.size > 0) {
    *origin = NameOrigin_Pattern;
    return patterned;
  }
  String8 stripped = strip_prefix_view(raw_name, es->cfg->strip_func_prefix);
  return camel_to_kebab(ctx_perm(), stripped);
}

static String8
compute_public_fn_name(EmitState* es, String8 raw_name) {
  NameOrigin origin;
  return compute_public_fn_name_tracking_origin(es, raw_name, &origin);
}

static OutparamRule*
find_outparam_rule(Config* cfg, String8 public_name) {
  foreach_index(i, dyn_count(cfg->outparam_rules)) {
    if (str8_match(cfg->outparam_rules[i].func_name, public_name, 0)) return &cfg->outparam_rules[i];
  }
  return NULL;
}

static String8
param_name_or_synth(Arena* arena, CFunction* fn, u64 i) {
  if (fn->params[i].name.size > 0) return fn->params[i].name;
  return str8_pushf(arena, "arg%llu", (unsigned long long)i); // C allows unnamed prototype params; 3b's param list needs a name
}

// Builds `(raw_name arg1 arg2 ...)`, substituting `replace_expr` in for the
// param at `replace_idx` (pass (u32)-1 for "no substitution", used by the
// mutate wrapper which forwards every param unchanged).
static String8
build_call_expr(Arena* arena, CFunction* fn, u32 replace_idx, String8 replace_expr) {
  String8List parts   = {0};
  StringJoin  no_join = {0};
  str8_list_push(arena, &parts, str8_lit("("));
  str8_list_push(arena, &parts, fn->name);
  foreach_index(i, dyn_count(fn->params)) {
    str8_list_push(arena, &parts, str8_lit(" "));
    if (i == replace_idx) {
      str8_list_push(arena, &parts, replace_expr);
    } else {
      str8_list_push(arena, &parts, param_name_or_synth(arena, fn, i));
    }
  }
  str8_list_push(arena, &parts, str8_lit(")"));
  return str8_list_join(arena, &parts, &no_join);
}

enum { FN_HANG = 2 }; // columns the param vector of a wrapped signature is indented by

// Prints `(fn name [p T ...] ret`, up to but not including the body, in the
// shape `3b format` gives the same code: a lone param stays on the signature
// line, two or more drop the whole vector onto its own line indented FN_HANG,
// so no param sits at a column that depends on the function's name length.
//
// `skip_idx` omits one C parameter the wrapper hides (an out-param); pass
// (u32)-1 to keep them all. `leading_arena` prepends the `arena arena`
// parameter the arena wrapper adds, which has no C parameter behind it.
// `wrap` false keeps everything on one line however long it gets, which is
// what a bodyless extern signature wants (see emit_function).
static void
emit_fn_header(EmitState* es, CFunction* fn, String8 fn_name, String8 ret_type, u32 skip_idx,
               b32 leading_arena, b32 wrap) {
  Arena* a     = ctx_perm();
  u32    count = leading_arena ? 1 : 0;
  foreach_index(i, dyn_count(fn->params)) {
    if (i != skip_idx) count += 1;
  }
  b32 multiline = wrap && count > 1;

  fprintf(es->out, "(fn %.*s", str8_varg(fn_name));
  if (multiline) {
    fprintf(es->out, "\n%*s", FN_HANG, "");
  } else {
    fprintf(es->out, " ");
  }
  fprintf(es->out, "[");

  b32 first = true;
  if (leading_arena) {
    fprintf(es->out, "arena arena");
    first = false;
  }
  foreach_index(i, dyn_count(fn->params)) {
    if (i == skip_idx) continue;
    if (!first) {
      if (multiline) fprintf(es->out, "\n%*s", FN_HANG + 1, ""); // +1 clears the `[`
      else            fprintf(es->out, " ");
    }
    first = false;
    String8 ptype = type_to_3b(a, es->cfg, es->unit, &fn->params[i].type);
    String8 pname = param_name_or_synth(a, fn, i);
    fprintf(es->out, "%.*s %.*s", str8_varg(pname), str8_varg(ptype));
  }
  fprintf(es->out, "] %.*s", str8_varg(ret_type));
}

static void
emit_plain_wrapper(EmitState* es, CFunction* fn, String8 public_name, String8 ret_type) {
  Arena* a = ctx_perm();
  emit_fn_header(es, fn, public_name, ret_type, (u32)-1, false, true);
  String8 call_expr = build_call_expr(a, fn, (u32)-1, str8_lit(""));
  fprintf(es->out, "\n  %.*s)\n\n", str8_varg(call_expr));
}

// "construct": the out-param produces a genuinely new value. The wrapper hides
// the pointer and returns the bare value, or `{status value}` when the real
// function also returns a status code.
static void
emit_construct_wrapper(EmitState* es, CFunction* fn, String8 public_name, String8 ret_type, OutparamRule* rule) {
  Arena* a       = ctx_perm();
  u32    out_idx = rule->param_index - 1; // config is 1-indexed
  if (out_idx >= dyn_count(fn->params) || fn->params[out_idx].type.kind != CTypeKind_Pointer) {
    fprintf(stderr, "translate: construct-outparam '%.*s' [param %u] is invalid -- skipping wrapper (raw extern still emitted)\n",
            str8_varg(public_name), rule->param_index);
    return;
  }
  CType*  value_type      = fn->params[out_idx].type.pointee;
  String8 value_type_text = type_to_3b(a, es->cfg, es->unit, value_type);

  b32     has_status         = fn->return_type.kind != CTypeKind_Void;
  String8 result_struct_name = {0};
  if (has_status) {
    result_struct_name = str8_pushf(a, "%.*sResult", str8_varg(public_name));
    if (try_register_name(es, result_struct_name, "out-param result struct")) {
      fprintf(es->out, "(struct %.*s\n  [status %.*s\n   value  %.*s])\n\n",
              str8_varg(result_struct_name), str8_varg(ret_type), str8_varg(value_type_text));
    } else {
      has_status = false; // registration already logged the collision -- fall back to dropping status
    }
  }

  String8 call_expr = build_call_expr(a, fn, out_idx, str8_lit("(addr out)"));
  String8 wrapper_ret = has_status ? result_struct_name : value_type_text;

  emit_fn_header(es, fn, public_name, wrapper_ret, out_idx, false, true);
  fprintf(es->out, "\n  (let [out %.*s nil]\n", str8_varg(value_type_text));
  if (has_status) {
    fprintf(es->out, "    (let [status %.*s]\n      (%.*s {:status status :value out}))))\n\n",
            str8_varg(call_expr), str8_varg(result_struct_name));
  } else {
    fprintf(es->out, "    %.*s\n    out))\n\n", str8_varg(call_expr));
  }
}

// "mutate": the out-param IS an existing object being modified in place. The
// wrapper takes the object, writes through it via the raw extern, and returns
// the same pointer so it chains through `->`/`->>`/`some->`.
static void
emit_mutate_wrapper(EmitState* es, CFunction* fn, String8 public_name, OutparamRule* rule) {
  Arena* a       = ctx_perm();
  u32    obj_idx = rule->param_index - 1;
  if (obj_idx >= dyn_count(fn->params)) {
    fprintf(stderr, "translate: mutate-outparam '%.*s' [param %u] is out of range -- skipping wrapper (raw extern still emitted)\n",
            str8_varg(public_name), rule->param_index);
    return;
  }
  String8 obj_type_text = type_to_3b(a, es->cfg, es->unit, &fn->params[obj_idx].type);
  String8 obj_pname     = param_name_or_synth(a, fn, obj_idx);
  String8 call_expr     = build_call_expr(a, fn, (u32)-1, str8_lit(""));

  emit_fn_header(es, fn, public_name, obj_type_text, (u32)-1, false, true);
  fprintf(es->out, "\n  %.*s\n  %.*s)\n\n", str8_varg(call_expr), str8_varg(obj_pname));
}

// A type annotation resolves aliases; `push`/`alloc`'s type ARGUMENT does not.
// `(alias GLuint u32)` with `(let [buf GLuint* (alloc GLuint n)] ...)` fails to
// type-check for exactly that reason: the binding's declared `GLuint*` resolves
// to `u32*` while the constructed value stays literally `GLuint*`. So a `push`
// type argument must be walked past every `(alias X Y)` this tool emitted, down
// to Y's ultimate primitive spelling, instead of using the nicer alias name
// every other position accepts.
static String8
resolve_push_elem_type_text(Arena* arena, Config* cfg, CUnit* unit, CType* t) {
  if (t->kind != CTypeKind_Named) return type_to_3b(arena, cfg, unit, t);
  String8 mapped = lookup_type_map(cfg, t->name);
  if (mapped.size > 0) return mapped;
  String8 builtin = builtin_c_type_to_3b(t->name);
  if (builtin.size > 0) return builtin;
  foreach_index(i, dyn_count(unit->typedefs)) {
    if (str8_match(unit->typedefs[i].name, t->name, 0)) {
      if (unit->typedefs[i].is_opaque_handle) break; // opaque -- not a push()-able element type anyway
      return resolve_push_elem_type_text(arena, cfg, unit, &unit->typedefs[i].underlying);
    }
  }
  return compute_type_name(arena, cfg, t->name); // a real struct/union name, or a
                                                                  // genuinely unmapped typedef -- best effort
}

// "arena": a batch out-param. The wrapper takes a count plus a caller-provided
// arena, allocates the buffer with `push` (see lower_push), calls the real
// extern, and returns the buffer.
//
// The arena parameter's own type is the bare primitive `arena`, never `Arena*`:
// 3b's `arena` is a small by-value handle and is never referenced through a
// pointer at the language level (3b.h, TypeKind_Arena).
static void
emit_arena_wrapper(EmitState* es, CFunction* fn, String8 public_name, OutparamRule* rule) {
  Arena* a         = ctx_perm();
  u32    count_idx = rule->count_param_index - 1;
  u32    out_idx   = rule->out_param_index - 1;
  if (count_idx >= dyn_count(fn->params) || out_idx >= dyn_count(fn->params) ||
      fn->params[out_idx].type.kind != CTypeKind_Pointer) {
    fprintf(stderr, "translate: arena-outparam '%.*s' has an invalid count-param/out-param index -- skipping wrapper (raw extern still emitted)\n",
            str8_varg(public_name));
    return;
  }
  CType*  elem_type       = fn->params[out_idx].type.pointee;
  String8 elem_type_text  = type_to_3b(a, es->cfg, es->unit, elem_type);
  String8 push_elem_text  = resolve_push_elem_type_text(a, es->cfg, es->unit, elem_type);
  String8 count_pname     = param_name_or_synth(a, fn, count_idx);
  String8 call_expr       = build_call_expr(a, fn, out_idx, str8_lit("buf"));

  emit_fn_header(es, fn, public_name, str8_pushf(a, "%.*s*", str8_varg(elem_type_text)), out_idx, true, true);
  fprintf(es->out, "\n  (let [buf (push arena %.*s %.*s)]\n    %.*s\n    buf))\n\n",
          str8_varg(push_elem_text), str8_varg(count_pname), str8_varg(call_expr));
}

static b32
is_resolvable_named(CUnit* unit, Config* cfg, String8 name) {
  if (is_force_opaque(cfg, name)) return true;
  if (lookup_pin_type(cfg, name).size > 0) return true;
  if (lookup_type_map(cfg, name).size > 0) return true;
  if (builtin_c_type_to_3b(name).size > 0) return true;
  foreach_index(i, dyn_count(unit->records)) {
    if (str8_match(unit->records[i].name, name, 0)) return true;
  }
  foreach_index(i, dyn_count(unit->typedefs)) {
    if (str8_match(unit->typedefs[i].name, name, 0)) return true;
  }
  foreach_index(i, dyn_count(unit->enums)) {
    if (str8_match(unit->enums[i].name, name, 0)) return true;
  }
  return false;
}

// True when `t` names a type extraction never captured a definition for at all:
// one declared in a header outside cfg->headers (khrplatform.h's
// `khronos_ssize_t`, pulled in transitively by glext.h but never configured),
// or a forward-declared-only struct with no definition anywhere in the
// translation unit (`_cl_context`/`_cl_event`, which headers expect
// `<CL/cl.h>` to supply). Emitting a reference to an undeclared name is broken
// output, so catching it here skips the offending function with a clear reason
// instead of producing a C file that fails to compile.
static b32
type_references_unresolvable(CUnit* unit, Config* cfg, CType* t) {
  switch (t->kind) {
    case CTypeKind_Named:   return !is_resolvable_named(unit, cfg, t->name);
    case CTypeKind_Pointer: return t->pointee->kind != CTypeKind_Void && type_references_unresolvable(unit, cfg, t->pointee);
    case CTypeKind_Array:   return type_references_unresolvable(unit, cfg, t->pointee);
    case CTypeKind_FunctionPointer: {
      // Variadic has no 3b equivalent -- a `fn` TYPE takes no `...`, only an
      // `extern` signature does (3b.h, is_variadic). Otherwise resolvable
      // exactly when every param and the return type are.
      if (t->fn_is_variadic) return true;
      if (type_references_unresolvable(unit, cfg, t->fn_return)) return true;
      foreach_index(i, t->fn_param_count) {
        if (type_references_unresolvable(unit, cfg, &t->fn_params[i])) return true;
      }
      return false;
    }
    default: return false;
  }
}

// True when `t` names a struct/union or a real C enum this unit captured,
// searched recursively through Pointer/Array and a FunctionPointer's own params
// and return.
//
// Only meaningful for deciding whether a function-pointer TYPEDEF is safe to
// emit as a top-level `(alias Name (fn ...))`: cg_program emits every alias as
// one block ahead of the structs and enums, so an alias whose `fn` body names a
// struct/enum by pointer would reference a C typedef that does not exist yet at
// that point in the generated file. A function pointer used INLINE -- as a
// struct field's type, or a function's param or return -- sits inside an
// already correctly ordered declaration and has no such problem. This check is
// therefore narrower than type_references_unresolvable and must never be
// applied to inline emission, only to emit_typedef's alias path.
static b32
type_references_struct_or_enum(CUnit* unit, CType* t) {
  switch (t->kind) {
    case CTypeKind_Named: {
      foreach_index(i, dyn_count(unit->records)) {
        if (str8_match(unit->records[i].name, t->name, 0)) return true;
      }
      foreach_index(i, dyn_count(unit->enums)) {
        if (str8_match(unit->enums[i].name, t->name, 0)) return true;
      }
      return false;
    }
    case CTypeKind_Pointer: return type_references_struct_or_enum(unit, t->pointee);
    case CTypeKind_Array:   return type_references_struct_or_enum(unit, t->pointee);
    case CTypeKind_FunctionPointer: {
      if (type_references_struct_or_enum(unit, t->fn_return)) return true;
      foreach_index(i, t->fn_param_count) {
        if (type_references_struct_or_enum(unit, &t->fn_params[i])) return true;
      }
      return false;
    }
    default: return false;
  }
}

// True when `t` is a struct/union the wrapper would have to pass or return BY
// VALUE -- the outermost type only, since a pointer to one is fine and is how
// every translated binding to date works.
//
// A pointer is fine because C is lenient about them: the wrapper hands a
// `libav_Rational*` to a function declared to take `AVRational*` and gcc says
// -Wincompatible-pointer-types, a warning. By value there is no leniency and
// no conversion at all -- `error: incompatible types when returning type
// 'AVRational' but 'libav_Rational' was expected` -- so the whole generated
// package fails to compile. Nothing about the value is wrong; the two struct
// definitions are (verify_record_layouts having just proved it) laid out
// identically. C simply will not let one stand in for the other by name.
//
// Which is what the bridge below exists to get around; see its own note.
// Everything this predicate finds is either bridged or skipped, never emitted
// as a direct call.
//
// `out_name` receives the name as WRITTEN (a typedef's own name, not the tag it
// resolves to), which is what a diagnostic should say; `out_record`, when
// given, receives the record the name ultimately resolves to, which is what the
// bridge needs.
//
// `depth` bounds a typedef chain that loops; MIRROR_MAX_DEPTH is the same
// backstop the layout walk uses, for the same reason.
static b32
type_is_struct_by_value(CUnit* unit, Config* cfg, CType* t, u32 depth, String8* out_name,
                        CRecord** out_record) {
  if (t->kind != CTypeKind_Named || depth > MIRROR_MAX_DEPTH) return false;
  // Each of these is emitted as something other than a 3b struct, so there is
  // no mangled mirror type to mismatch. Checked in type_to_3b_rec's own order.
  if (is_force_opaque(cfg, t->name)) return false;
  if (lookup_pin_type(cfg, t->name).size > 0) return false;
  if (lookup_type_map(cfg, t->name).size > 0) return false;
  if (builtin_c_type_to_3b(t->name).size > 0) return false;
  foreach_index(i, dyn_count(unit->records)) {
    if (!str8_match(unit->records[i].name, t->name, 0)) continue;
    *out_name = t->name;
    if (out_record) *out_record = &unit->records[i];
    return true;
  }
  foreach_index(i, dyn_count(unit->typedefs)) {
    if (!str8_match(unit->typedefs[i].name, t->name, 0)) continue;
    if (unit->typedefs[i].is_opaque_handle) return false;
    // Only on the way back out, and only on a true: `*out_name` must stay
    // untouched when the chain lands on something that is not a struct at all,
    // since callers read it as "was one found?". The outermost spelling
    // overwrites the tag the recursion found, being the one the C signature
    // actually says and the one a `typedef struct X X;` pair can spell bare.
    String8 inner = {0};
    if (!type_is_struct_by_value(unit, cfg, &unit->typedefs[i].underlying, depth + 1, &inner, out_record)) {
      return false;
    }
    *out_name = t->name;
    return true;
  }
  return false; // an enum (a plain int either way), or a name nothing captured
}

////////////////////////////////
//~ By-value struct bridge
//
// The gap type_is_struct_by_value names is real but narrow: the VALUES are
// interchangeable (verify_record_layouts proves the mirror and the original are
// laid out identically, byte for byte), it is only C's by-name type identity
// that refuses to carry one across. So the fix is to stop asking C to carry it
// by name.
//
// `3b translate` writes a second file next to the package, `<pkg>_byval.h`, and
// build.c `-include`s it into that package's generated C. It holds one
// `static inline` shim per bridged function, taking and returning every
// by-value struct THROUGH A `void*` instead:
//
//   static inline int64_t libav_byval_av_rescale_q(int64_t a0_, const void *a1_, const void *a2_) {
//     AVRational v1_; memcpy(&v1_, a1_, sizeof v1_);
//     AVRational v2_; memcpy(&v2_, a2_, sizeof v2_);
//     return av_rescale_q(a0_, v1_, v2_);
//   }
//
// The shim is compiled against the REAL header, so it names the real type and
// C's own by-value rules apply to the real call unchanged. The 3b wrapper
// passes `(addr param)` -- a `libav_Rational*`, which converts to `void*`
// implicitly and silently -- and reads a struct return back out of a local the
// shim memcpy'd into.
//
// The bridge cannot live in the wrapper body instead: the wrapper is 3b source,
// and 3b cannot spell a call whose argument type is a C name it never declared.
//
// It rests on the mirror and the original really being the same bytes, which is
// what verify_record_layouts checks. The bridge asks per-record
// (check_record_mirror) rather than assuming the package-wide pass covered this
// one: a record whose layout cannot be COMPUTED passes the package-wide check by
// standing down, and is not a struct to memcpy blind. Everything else it needs
// (spelling the C signature, a zero value for the return local) either works or
// takes the function back to being skipped, so bridging only ever adds
// functions.

// The C spelling of `t`, for the bridge header -- the one place this tool
// writes C rather than 3b. Empty means "cannot be spelled from a CType", which
// takes the whole function back to being skipped:
//   - an array or function-pointer parameter, whose C declarator wraps around
//     the parameter NAME instead of sitting to its left, so `<text> <name>` is
//     not how it is written
//   - an anonymous record, which has no C name at all (its 3b mirror gets a
//     synthesized `AnonN`, which means nothing to a C compiler)
// A name that resolves to nothing captured is spelled bare and left alone:
// type_references_unresolvable has already skipped any function reaching one.
static String8
c_type_text(Arena* arena, CUnit* unit, CType* t) {
  switch (t->kind) {
    case CTypeKind_Void: return str8_lit("void");
    case CTypeKind_Pointer: {
      String8 pointee = c_type_text(arena, unit, t->pointee);
      if (pointee.size == 0) return pointee;
      return str8_pushf(arena, "%.*s *", str8_varg(pointee));
    }
    case CTypeKind_Named: {
      const char* qual = t->is_const ? "const " : "";
      // A typedef name stands alone; a bare tag needs its keyword back, since
      // capture stripped it (see cwalk.c's strip_tag_keyword). Typedefs are
      // checked first: `typedef struct AVRational { ... } AVRational;` produces
      // both, and the bare name is the valid spelling there.
      foreach_index(i, dyn_count(unit->typedefs)) {
        if (str8_match(unit->typedefs[i].name, t->name, 0)) {
          return str8_pushf(arena, "%s%.*s", qual, str8_varg(t->name));
        }
      }
      foreach_index(i, dyn_count(unit->records)) {
        if (!str8_match(unit->records[i].name, t->name, 0)) continue;
        if (unit->records[i].is_anonymous) return str8_lit("");
        // `typedef struct { ... } Name;` leaves no tag to write -- see
        // CRecord.has_typedef_name.
        if (unit->records[i].has_typedef_name) return str8_pushf(arena, "%s%.*s", qual, str8_varg(t->name));
        return str8_pushf(arena, "%s%s %.*s", qual, unit->records[i].is_union ? "union" : "struct",
                          str8_varg(t->name));
      }
      foreach_index(i, dyn_count(unit->enums)) {
        if (str8_match(unit->enums[i].name, t->name, 0)) {
          return str8_pushf(arena, "%senum %.*s", qual, str8_varg(t->name));
        }
      }
      return str8_pushf(arena, "%s%.*s", qual, str8_varg(t->name)); // a builtin: "unsigned int"
    }
    case CTypeKind_Array:
    case CTypeKind_FunctionPointer: return str8_lit("");
  }
  return str8_lit("");
}

static String8 zero_value_3b(Arena* arena, Config* cfg, CUnit* unit, CType* t, u32 depth);

// The zero value of the 3b type `t` maps to, written as 3b source. Empty means
// there is no spelling for one, which takes the function back to being skipped.
//
// It exists only because a `let` binding needs an initializer and 3b has no
// "zeroed value of T" form: the shim overwrites every byte of this local before
// anything reads it, so what is in it beforehand is irrelevant -- it just has
// to be writable 3b. Which is also why the unsupported cases are simply given
// up on rather than worked around: an enum field would need a variant name to
// be legal, an array field a literal per element, and both would only ever be
// written to be immediately overwritten.
static String8
zero_value_3b_primitive(String8 b3_name) {
  // Suffixed: a suffixless int literal adopts the type its context expects, but
  // a float literal is f32 unless told otherwise, so a bare `0.0` in an f64
  // field is a type error rather than a widening.
  if (str8_match_lit("f32", b3_name, 0)) return str8_lit("0.0f32");
  if (str8_match_lit("f64", b3_name, 0)) return str8_lit("0.0f64");
  if (str8_match_lit("bool", b3_name, 0)) return str8_lit("false");
  if (str8_match_lit("any", b3_name, 0) || str8_match_lit("string", b3_name, 0)) return str8_lit("nil");
  static const char* ints[] = {"i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64"};
  foreach_index(i, ArrayCount(ints)) {
    if (str8_match(str8_cstring((char*)ints[i]), b3_name, 0)) return str8_lit("0");
  }
  return str8_lit(""); // `char`, `void`, a Vector/Map/Set shorthand -- none reachable as a
                         // translated field type, and none worth guessing at
}

// `(Rational {:num 0 :den 0})`, recursively for a nested struct field.
static String8
zero_value_record(Arena* arena, Config* cfg, CUnit* unit, CRecord* r, u32 depth) {
  if (!r->is_complete || dyn_count(r->fields) == 0) return str8_lit("");
  if (r->is_union) return str8_lit(""); // a literal naming every field is not what a union means
  String8 out = str8_pushf(arena, "(%.*s {", str8_varg(compute_type_name(arena, cfg, r->name)));
  foreach_index(i, dyn_count(r->fields)) {
    String8 fz = zero_value_3b(arena, cfg, unit, &r->fields[i].type, depth + 1);
    if (fz.size == 0) return str8_lit("");
    out = str8_pushf(arena, "%.*s%s:%.*s %.*s", str8_varg(out), (i == 0 ? "" : " "),
                     str8_varg(field_name_3b(arena, r, i, NULL)), str8_varg(fz));
  }
  return str8_pushf(arena, "%.*s})", str8_varg(out));
}

// Dispatches exactly as type_to_3b_rec does, so the value produced is always of
// the type that function names.
static String8
zero_value_3b(Arena* arena, Config* cfg, CUnit* unit, CType* t, u32 depth) {
  if (depth > MIRROR_MAX_DEPTH) return str8_lit("");
  switch (t->kind) {
    case CTypeKind_Pointer:
    case CTypeKind_FunctionPointer: return str8_lit("nil");
    case CTypeKind_Array:           return str8_lit(""); // needs one literal per element
    case CTypeKind_Void:            return str8_lit("");
    case CTypeKind_Named: break;
  }
  if (is_force_opaque(cfg, t->name)) return str8_lit("nil");
  String8 mapped = lookup_type_map(cfg, t->name);
  if (mapped.size > 0) return zero_value_3b_primitive(mapped);
  String8 builtin = builtin_c_type_to_3b(t->name);
  if (builtin.size > 0) return zero_value_3b_primitive(builtin);
  foreach_index(i, dyn_count(unit->records)) {
    if (str8_match(unit->records[i].name, t->name, 0)) {
      return zero_value_record(arena, cfg, unit, &unit->records[i], depth);
    }
  }
  foreach_index(i, dyn_count(unit->typedefs)) {
    if (!str8_match(unit->typedefs[i].name, t->name, 0)) continue;
    if (unit->typedefs[i].is_opaque_handle) return str8_lit("nil");
    return zero_value_3b(arena, cfg, unit, &unit->typedefs[i].underlying, depth + 1);
  }
  return str8_lit(""); // an enum (no zero variant to name), or nothing captured
}

// Everything the bridge needs to write both halves of one function, or
// `ok == false` with `why` saying which requirement was not met.
typedef struct ByValuePlan {
  b32         ok;
  const char* why;         // set when !ok
  String8     subject;      // what `why` is about: `'Rat'`, `parameter 'q'`, ...
  String8     shim_name;    // <pkg>_byval_<c name>
  b32         ret_by_value;
  String8     ret_zero;     // 3b zero value for the return local, when ret_by_value
  String8     ret_local;    // its name, chosen not to shadow any parameter
  b32*        param_by_value; // one per param
} ByValuePlan;

// Only a record whose mirror is KNOWN to match the original may be memcpy'd
// through a void*; see this section's note.
static b32
byval_record_is_safe(Config* cfg, CUnit* unit, CRecord* r) {
  MirrorCheck check = check_record_mirror(ctx_perm(), cfg, unit, r);
  return check.compared && check.matches;
}

static ByValuePlan
plan_by_value_bridge(EmitState* es, CFunction* fn) {
  Arena*      a    = ctx_perm();
  ByValuePlan plan = {0};
  plan.param_by_value = push_array(a, b32, dyn_count(fn->params) == 0 ? 1 : dyn_count(fn->params));
  if (!es->byval_out) { // translating without a bridge header: nowhere to put the shim
    plan.why     = "this run has no bridge header to write the C shim into";
    plan.subject = str8_lit("it");
    return plan;
  }

  // Return first, then each parameter: every one has to be spellable in C, and
  // every by-value struct among them has to be layout-verified.
  for (u64 i = 0; i <= dyn_count(fn->params); i += 1) {
    b32    is_ret = (i == 0);
    CType* t      = is_ret ? &fn->return_type : &fn->params[i - 1].type;

    String8  by_value_name = {0};
    CRecord* record         = NULL;
    b32      by_value       = type_is_struct_by_value(es->unit, es->cfg, t, 0, &by_value_name, &record);
    if (by_value) {
      if (!record || !byval_record_is_safe(es->cfg, es->unit, record)) {
        plan.why     = "its layout could not be verified against the C original, so its bytes cannot be copied across";
        plan.subject = str8_pushf(a, "'%.*s'", str8_varg(by_value_name));
        return plan;
      }
      if (is_ret) {
        plan.ret_by_value = true;
        plan.ret_zero     = zero_value_3b(a, es->cfg, es->unit, t, 0);
        if (plan.ret_zero.size == 0) {
          plan.why     = "the wrapper has no way to spell a value of it to receive the result in";
          plan.subject = str8_pushf(a, "'%.*s'", str8_varg(by_value_name));
          return plan;
        }
      } else {
        plan.param_by_value[i - 1] = true;
      }
    }
    if (c_type_text(a, es->unit, t).size == 0) {
      plan.why     = "its C spelling cannot be reconstructed (an array or function-pointer type, "
                      "or an anonymous struct)";
      plan.subject = is_ret ? str8_lit("the return type")
                            : str8_pushf(a, "parameter '%.*s'", str8_varg(param_name_or_synth(a, fn, i - 1)));
      return plan;
    }
  }

  // A shim for a function with no by-value struct anywhere in it would be a
  // pure indirection, and a sign the caller's own by-value test disagrees with
  // the one above. Refusing keeps the two from drifting silently: a function
  // that reaches here wrongly goes back to being emitted as a direct call,
  // which is what it should have been all along.
  b32 any_by_value = plan.ret_by_value;
  foreach_index(i, dyn_count(fn->params)) {
    if (plan.param_by_value[i]) any_by_value = true;
  }
  if (!any_by_value) {
    plan.why     = "nothing in it is actually passed by value";
    plan.subject = str8_lit("it");
    return plan;
  }

  plan.shim_name = str8_pushf(a, "%.*s_byval_%.*s", str8_varg(es->cfg->package_name), str8_varg(fn->name));
  // The receiving local must not shadow a parameter the call still has to pass;
  // a C function really can have a parameter named `out`.
  plan.ret_local = str8_lit("out");
  for (u32 attempt = 0; attempt < 100; attempt += 1) {
    b32 taken = false;
    foreach_index(i, dyn_count(fn->params)) {
      if (str8_match(param_name_or_synth(a, fn, i), plan.ret_local, 0)) { taken = true; break; }
    }
    if (!taken) break;
    plan.ret_local = str8_pushf(a, "out%u", attempt + 2);
  }
  plan.ok = true;
  return plan;
}

// One `static inline` forwarder in <pkg>_byval.h. Shim parameters are named
// `a0_`, `a1_`, ... and the unpacked struct locals `v1_`, `v2_`, ... rather
// than reusing the C header's own parameter names, which are not guaranteed to
// exist (C allows unnamed prototype parameters) or to be free of macro
// collisions with the library's own headers.
static void
emit_byval_shim(EmitState* es, CFunction* fn, ByValuePlan* plan) {
  Arena*  a       = ctx_perm();
  String8 ret_c   = c_type_text(a, es->unit, &fn->return_type);
  FILE*   h       = es->byval_out;

  // A by-value return is handed back through `ret_` instead, so the shim's own
  // return type becomes void.
  String8 shim_ret = plan->ret_by_value ? str8_lit("void") : ret_c;
  fprintf(h, "static inline %.*s %.*s(", str8_varg(shim_ret), str8_varg(plan->shim_name));
  b32 first = true;
  if (plan->ret_by_value) { fprintf(h, "void *ret_"); first = false; }
  foreach_index(i, dyn_count(fn->params)) {
    if (!first) fprintf(h, ", ");
    first = false;
    if (plan->param_by_value[i]) {
      fprintf(h, "const void *a%llu_", (unsigned long long)i);
    } else {
      String8 ptype = c_type_text(a, es->unit, &fn->params[i].type);
      fprintf(h, "%.*s a%llu_", str8_varg(ptype), (unsigned long long)i);
    }
  }
  if (first) fprintf(h, "void");
  fprintf(h, ") {\n");

  foreach_index(i, dyn_count(fn->params)) {
    if (!plan->param_by_value[i]) continue;
    String8 ptype = c_type_text(a, es->unit, &fn->params[i].type);
    fprintf(h, "  %.*s v%llu_; memcpy(&v%llu_, a%llu_, sizeof v%llu_);\n",
            str8_varg(ptype), (unsigned long long)i, (unsigned long long)i,
            (unsigned long long)i, (unsigned long long)i);
  }

  if (plan->ret_by_value) {
    fprintf(h, "  %.*s r_ = ", str8_varg(ret_c)); // memcpy'd out through ret_ below
  } else {
    fprintf(h, "%s", fn->return_type.kind == CTypeKind_Void ? "  " : "  return ");
  }
  fprintf(h, "%.*s(", str8_varg(fn->name));
  foreach_index(i, dyn_count(fn->params)) {
    if (i > 0) fprintf(h, ", ");
    if (plan->param_by_value[i]) fprintf(h, "v%llu_", (unsigned long long)i);
    else                          fprintf(h, "a%llu_", (unsigned long long)i);
  }
  fprintf(h, ");\n");
  if (plan->ret_by_value) fprintf(h, "  memcpy(ret_, &r_, sizeof r_);\n");
  fprintf(h, "}\n\n");
  es->byval_count += 1;
}

// The 3b half: the shim's own extern, then a public wrapper with the signature
// the C function really has -- by-value structs in and out, `(addr ...)` and a
// receiving local hidden inside.
static void
emit_byval_wrapper(EmitState* es, CFunction* fn, String8 public_name, ByValuePlan* plan) {
  Arena*  a        = ctx_perm();
  String8 ret_type = type_to_3b(a, es->cfg, es->unit, &fn->return_type);

  // Extern signature: a by-value struct becomes a pointer to the mirror. C
  // takes that as the `void*` the shim declares, implicitly and without a
  // diagnostic -- unlike the mirror-pointer-for-real-pointer substitution the
  // rest of a binding relies on, which warns.
  fprintf(es->out, "(private (extern (fn %.*s [", str8_varg(plan->shim_name));
  b32 first = true;
  if (plan->ret_by_value) {
    fprintf(es->out, "%.*s %.*s*", str8_varg(plan->ret_local), str8_varg(ret_type));
    first = false;
  }
  foreach_index(i, dyn_count(fn->params)) {
    if (!first) fprintf(es->out, " ");
    first = false;
    String8 ptype = type_to_3b(a, es->cfg, es->unit, &fn->params[i].type);
    fprintf(es->out, "%.*s %.*s%s", str8_varg(param_name_or_synth(a, fn, i)),
            str8_varg(ptype), plan->param_by_value[i] ? "*" : "");
  }
  fprintf(es->out, "] %.*s)))\n\n",
          str8_varg(plan->ret_by_value ? str8_lit("void") : ret_type));

  // Wrapper: the signature a caller wants, unchanged from the C one.
  emit_fn_header(es, fn, public_name, ret_type, (u32)-1, false, true);

  String8List parts   = {0};
  StringJoin  no_join = {0};
  str8_list_push(a, &parts, str8_pushf(a, "(%.*s", str8_varg(plan->shim_name)));
  if (plan->ret_by_value) {
    str8_list_push(a, &parts, str8_pushf(a, " (addr %.*s)", str8_varg(plan->ret_local)));
  }
  foreach_index(i, dyn_count(fn->params)) {
    String8 pname = param_name_or_synth(a, fn, i);
    str8_list_push(a, &parts, plan->param_by_value[i]
      ? str8_pushf(a, " (addr %.*s)", str8_varg(pname))
      : str8_pushf(a, " %.*s", str8_varg(pname)));
  }
  str8_list_push(a, &parts, str8_lit(")"));
  String8 call_expr = str8_list_join(a, &parts, &no_join);

  if (plan->ret_by_value) {
    fprintf(es->out, "\n  (let [%.*s %.*s %.*s]\n    %.*s\n    %.*s))\n\n",
            str8_varg(plan->ret_local), str8_varg(ret_type), str8_varg(plan->ret_zero),
            str8_varg(call_expr), str8_varg(plan->ret_local));
  } else {
    fprintf(es->out, "\n  %.*s)\n\n", str8_varg(call_expr));
  }
}

static void
emit_function(EmitState* es, CFunction* fn) {
  foreach_index(i, dyn_count(es->cfg->excluded_funcs)) {
    if (str8_match(es->cfg->excluded_funcs[i], fn->name, 0)) return;
  }
  // Silent, unlike the skips below: `(skip-deprecated)` is a deliberate blanket
  // choice, and a library mid-deprecation-cycle would bury the real diagnostics
  // under dozens of these.
  if (es->cfg->skip_deprecated && fn->is_deprecated) return;
  // A variadic C function is skipped whole, extern included. `...` is legal
  // only in an extern signature, never in a real `fn` body (lower.c), so the
  // public wrapper every translated function gets would have no way to forward
  // the arguments on.
  if (fn->is_variadic) {
    fprintf(stderr, "translate: skipping variadic function '%.*s' (unsupported, doc: add `exclude-func` explicitly)\n", str8_varg(fn->name));
    return;
  }
  b32 unresolvable = type_references_unresolvable(es->unit, es->cfg, &fn->return_type);
  foreach_index(i, dyn_count(fn->params)) {
    if (unresolvable) break;
    if (type_references_unresolvable(es->unit, es->cfg, &fn->params[i].type)) unresolvable = true;
  }
  if (unresolvable) {
    fprintf(stderr, "translate: skipping '%.*s' -- references a type with no captured definition "
                     "(likely declared in a header outside this config's `headers` list; add it, "
                     "`force-opaque` the type, or `exclude-func` this)\n", str8_varg(fn->name));
    return;
  }
  // See type_is_struct_by_value: a direct call here would fail in the C
  // compiler, not here, and take the whole package's build down with it. The
  // bridge routes it through a generated shim instead; anything the bridge
  // cannot handle is still skipped, with the reason it gave.
  String8     byval_type = {0};
  const char* byval_what = "returns";
  b32         by_value    = type_is_struct_by_value(es->unit, es->cfg, &fn->return_type, 0, &byval_type, NULL);
  if (!by_value) {
    byval_what = "takes";
    foreach_index(i, dyn_count(fn->params)) {
      by_value = type_is_struct_by_value(es->unit, es->cfg, &fn->params[i].type, 0, &byval_type, NULL);
      if (by_value) break;
    }
  }

  String8 public_name = compute_public_fn_name(es, fn->name);

  ByValuePlan plan = {0};
  if (by_value) {
    // An out-param rule rewrites the wrapper's whole shape, and combining that
    // with the bridge's own rewriting is a case nothing has needed yet.
    if (find_outparam_rule(es->cfg, public_name)) {
      fprintf(stderr, "translate: skipping '%.*s' -- it %s '%.*s' by value AND has an outparam rule, "
                       "which cannot be combined -- drop the rule to get the by-value bridge, or "
                       "`exclude-func` it\n", str8_varg(fn->name), byval_what, str8_varg(byval_type));
      return;
    }
    plan = plan_by_value_bridge(es, fn);
    if (!plan.ok) {
      fprintf(stderr, "translate: skipping '%.*s' -- it %s '%.*s' by value, and %.*s cannot be "
                       "bridged: %s -- reimplement it in 3b, or `exclude-func` it to silence this\n",
              str8_varg(fn->name), byval_what, str8_varg(byval_type), str8_varg(plan.subject), plan.why);
      return;
    }
    if (!try_register_name(es, plan.shim_name, "by-value bridge")) return;
    if (!try_register_name(es, public_name, "function")) return;
    emit_byval_shim(es, fn, &plan);
    emit_byval_wrapper(es, fn, public_name, &plan);
    return;
  }

  if (!try_register_name(es, fn->name, "extern binding")) return;
  if (!try_register_name(es, public_name, "function")) return;

  Arena* a = ctx_perm();

  // ---- raw extern: private, under the exact C name so it links ----
  // One line no matter how many params it has, matching what `3b format`
  // does with a bodyless `fn` (fmt_fn's count == 4 case): nobody reads these
  // -- the wrapper right below is the readable declaration -- and wrapping
  // them would only make `3b format` reflow every generated binding.
  String8 ret_type = type_to_3b(a, es->cfg, es->unit, &fn->return_type);
  fprintf(es->out, "(private (extern ");
  emit_fn_header(es, fn, fn->name, ret_type, (u32)-1, false, false);
  fprintf(es->out, ")))\n\n");

  // ---- public wrapper ----
  OutparamRule* rule = find_outparam_rule(es->cfg, public_name);
  if (!rule) {
    emit_plain_wrapper(es, fn, public_name, ret_type);
  } else if (rule->kind == OutparamKind_Construct) {
    emit_construct_wrapper(es, fn, public_name, ret_type, rule);
  } else if (rule->kind == OutparamKind_Mutate) {
    emit_mutate_wrapper(es, fn, public_name, rule);
  } else {
    emit_arena_wrapper(es, fn, public_name, rule);
  }
}

////////////////////////////////
//~ Top level

// The bridge header's fixed top and bottom. It includes the config's own
// headers rather than relying on the consuming project's `include-first`: the
// shims name the library's real types, and a header that only compiles when
// something else was `-include`d first in the right order is a trap. `<string.h>`
// is for memcpy, which every shim uses.
//
// Written unconditionally, before any function is looked at, and thrown away by
// translate.c if no shim ever followed -- the alternative is buffering every
// shim in memory to find out whether the file is worth opening.
static void
emit_byval_header_preamble(EmitState* es) {
  if (!es->byval_out) return;
  Arena*  a     = ctx_perm();
  String8 pkg   = es->cfg->package_name;
  u8*     buf   = push_array(a, u8, pkg.size == 0 ? 1 : pkg.size);
  foreach_index(i, pkg.size) buf[i] = char_to_upper(pkg.str[i]);
  String8 guard = str8(buf, pkg.size);

  fprintf(es->byval_out,
          "// Generated by `3b translate` -- do not edit.\n"
          "//\n"
          "// One `static inline` shim per C function that passes or returns a struct BY\n"
          "// VALUE. `%.*s.3b` cannot call those directly: its own mirror struct is a\n"
          "// different C TYPE from the library's, laid out identically but not\n"
          "// interchangeable by name. Each shim takes the value through a `void*`\n"
          "// instead, unpacks it into the real type with memcpy, and makes the by-value\n"
          "// call itself -- see translate/emit.c's \"By-value struct bridge\".\n"
          "//\n"
          "// `3b build` -include's this into the package's generated C automatically,\n"
          "// having found it next to `%.*s.3b`.\n"
          "#ifndef %.*s_BYVAL_H\n"
          "#define %.*s_BYVAL_H\n"
          "#include <string.h>\n",
          str8_varg(pkg), str8_varg(pkg), str8_varg(guard), str8_varg(guard));
  foreach_index(i, dyn_count(es->cfg->headers)) {
    fprintf(es->byval_out, "#include \"%.*s\"\n", str8_varg(es->cfg->headers[i]));
  }
  fprintf(es->byval_out, "\n");
}

static void
emit_byval_header_epilogue(EmitState* es) {
  if (!es->byval_out) return;
  fprintf(es->byval_out, "#endif\n");
}

// One `(alias NAME PRIM "NAME")` per `pin-type` entry, ahead of every other
// declaration so records, typedefs and functions can all name them.
//
// A C typedef is transparent: `sdl_size_t*` is compatible with the `size_t*` a
// real function wants only if both bottom out in the same C type. 3b's u64 is
// `unsigned long long` (bbb_prelude.h spells it that way so Windows' LLP64
// `long` cannot silently shrink to 32 bits), while glibc's size_t, int64_t and
// everything built on them are `long`. Same width and representation, distinct C
// type -- which is a pointer-incompatibility warning at every call site that
// passes one by address. Pinning keeps 3b's view (a plain u64, with every
// numeric rule intact) while letting the emitted C name the real type.
//
// The C spelling an entry stands on must be visible in the generated *header*,
// which includes only 3b_runtime.h -- so <stddef.h>/<stdint.h> names (size_t,
// int64_t, ptrdiff_t) qualify, and library names like GLint64 or cgltf_size do
// not: `include-first` is a `-include` flag on the .c, and the .h is read by
// other packages without it. That is not the limitation it sounds like,
// because C typedefs are transparent all the way down: pinning the <stdint.h>
// leaf a library typedef is built from fixes every alias stacked on top of it.
//
// Which leaves the case where the library's own NAME is the one that has to be
// pinned, because the pin has to beat a `type-map` entry keyed on it. An entry
// may then give the leaf explicitly: `[Uint64 u64 "uint64_t"]` emits
// `typedef uint64_t sdl_Uint64;`, so the 3b side names Uint64 (which is what
// SDL's own signatures reference, and what makes the pin reachable) while the
// C side stands on a spelling the header can actually see.
//
// Pinning only earns its keep for types passed by address; a by-value `size_t`
// argument converts silently and needs nothing.
static void
emit_pinned_type_aliases(EmitState* es) {
  if (dyn_count(es->cfg->pin_type) == 0) return;
  foreach_index(i, dyn_count(es->cfg->pin_type)) {
    PinTypeEntry* e        = &es->cfg->pin_type[i];
    String8       name     = compute_type_name(ctx_perm(), es->cfg, e->c_name);
    String8       spelling = e->c_spelling.size > 0 ? e->c_spelling : e->c_name;
    if (!try_register_name(es, name, "pinned type")) continue;
    fprintf(es->out, "(alias %.*s %.*s \"%.*s\")\n\n",
            str8_varg(name), str8_varg(e->b3_name), str8_varg(spelling));
  }
}

b32
emit_package(Config* cfg, CUnit* unit, FILE* out, FILE* byval_out, u32* out_byval_count) {
  EmitState es = {0};
  es.cfg = cfg;
  es.unit = unit;
  es.out = out;
  es.byval_out = byval_out;

  fprintf(out, "(package %.*s)\n\n", str8_varg(cfg->package_name));
  emit_byval_header_preamble(&es);

  emit_constants(&es);
  emit_pinned_type_aliases(&es); // before every declaration that might name one
  foreach_index(i, dyn_count(unit->enums)) emit_enum(&es, &unit->enums[i]);
  foreach_index(i, dyn_count(unit->records)) emit_record(&es, &unit->records[i]);
  foreach_index(i, dyn_count(unit->typedefs)) emit_typedef(&es, &unit->typedefs[i]);
  emit_unreached_opaques(&es); // after every declaration site has had its chance at the name
  foreach_index(i, dyn_count(unit->functions)) emit_function(&es, &unit->functions[i]);

  emit_byval_header_epilogue(&es);
  if (out_byval_count) *out_byval_count = es.byval_count;

  if (es.collision_count > 0) {
    fprintf(stderr, "translate: %u naming collision(s)/error(s) -- see warnings above; "
                     "add rename-func/exclude-func/exclude-const entries to resolve\n",
            es.collision_count);
  }
  return es.collision_count == 0;
}

////////////////////////////////
//~ Name report (`3b translate --names`) -- translate.h's emit_name_report
// comment covers why this exists. Everything here runs through the same
// compute_*_name functions emit_package uses, so the report is a view of the
// real naming decisions rather than a second implementation of them.

typedef struct ReportRow {
  String8    c_name;
  String8    b3_name;
  NameOrigin origin;
} ReportRow;

static const char*
origin_label(NameOrigin origin) {
  switch (origin) {
    case NameOrigin_Rename:  return "rename";
    case NameOrigin_Pattern: return "pattern";
    default:                 return "default";
  }
}

static void
report_push(Arena* arena, ReportRow** rows, String8 c_name, String8 b3_name, NameOrigin origin) {
  ReportRow row = {0};
  row.c_name    = c_name;
  row.b3_name   = b3_name;
  row.origin    = origin;
  dyn_push(arena, *rows, row);
}

static void
report_print_section(FILE* out, const char* title, ReportRow* rows) {
  fprintf(out, ";; --- %s (%llu) ---\n", title, (unsigned long long)dyn_count(rows));
  foreach_index(i, dyn_count(rows)) {
    fprintf(out, "  %-44.*s %-32.*s %s\n",
            str8_varg(rows[i].c_name), str8_varg(rows[i].b3_name), origin_label(rows[i].origin));
  }
  fprintf(out, "\n");
}

// Two declarations landing on one 3b name is already a hard error at emit time,
// but the message there names only the second one and arrives interleaved with
// everything else on stderr. Patterns make these easy to create in bulk -- two
// C names differing only in separators collapse to the same PascalCase spelling
// -- so the report pairs both culprits up explicitly.
static void
report_print_collisions(FILE* out, ReportRow* all) {
  u64 reported = 0;
  foreach_index(i, dyn_count(all)) {
    b32 first_of_group = true;
    foreach_index(j, i) {
      if (str8_match(all[j].b3_name, all[i].b3_name, 0)) { first_of_group = false; break; }
    }
    if (!first_of_group) continue;

    u64 dup_count = 0;
    foreach_index(j, dyn_count(all)) {
      if (j != i && str8_match(all[j].b3_name, all[i].b3_name, 0)) dup_count += 1;
    }
    if (dup_count == 0) continue;

    if (reported == 0) fprintf(out, ";; --- collisions ---\n");
    reported += 1;
    fprintf(out, "  %.*s  <-  %.*s", str8_varg(all[i].b3_name), str8_varg(all[i].c_name));
    foreach_index(j, dyn_count(all)) {
      if (j != i && str8_match(all[j].b3_name, all[i].b3_name, 0)) fprintf(out, ", %.*s", str8_varg(all[j].c_name));
    }
    fprintf(out, "\n");
  }
  if (reported > 0) {
    fprintf(out, "\n;; %llu name(s) claimed by more than one declaration -- add a `rename-*` entry\n"
                  ";; for all but one of each group, or exclude the extras.\n",
            (unsigned long long)reported);
  }
}

void
emit_name_report(Config* cfg, CUnit* unit, FILE* out) {
  Arena*    a  = ctx_perm();
  EmitState es = {0};
  es.cfg       = cfg;
  es.unit      = unit;
  es.out       = out;

  ReportRow* constants = NULL;
  ReportRow* types     = NULL;
  ReportRow* functions = NULL;

  foreach_index(i, dyn_count(unit->constants)) {
    CConstant* c = &unit->constants[i];
    String8    logical = strip_prefix_view(c->name, cfg->strip_const_prefix);
    NameOrigin origin;
    String8    renamed = lookup_const_rename_tracking_origin(cfg, logical, &origin);
    report_push(a, &constants, c->name, renamed.size > 0 ? renamed : logical, origin);
  }

  // A real C enum's members are named on the same rules as loose `#define`
  // constants -- emit_enum and emit_constants share lookup_const_rename -- so
  // they belong in the constants section, prefixed with the enum they came
  // from. That prefix is the only thing telling a reader which
  // `(rename-const ...)` family a given member falls into.
  foreach_index(ei, dyn_count(unit->enums)) {
    CEnum*     e = &unit->enums[ei];
    NameOrigin type_origin;
    String8    type_name = compute_type_name_tracking_origin(a, cfg, e->name, &type_origin);
    report_push(a, &types, e->name, type_name, type_origin);
    foreach_index(mi, dyn_count(e->enumerators)) {
      CEnumerator* m       = &e->enumerators[mi];
      String8      logical = strip_prefix_view(m->name, cfg->strip_const_prefix);
      NameOrigin   origin;
      String8      renamed = lookup_const_rename_tracking_origin(cfg, logical, &origin);
      report_push(a, &constants, str8_pushf(a, "%.*s.%.*s", str8_varg(e->name), str8_varg(m->name)),
                  renamed.size > 0 ? renamed : logical, origin);
    }
  }

  foreach_index(i, dyn_count(unit->records)) {
    if (!unit->records[i].is_complete) continue;
    NameOrigin origin;
    String8    name = compute_type_name_tracking_origin(a, cfg, unit->records[i].name, &origin);
    report_push(a, &types, unit->records[i].name, name, origin);
  }
  foreach_index(i, dyn_count(unit->typedefs)) {
    if (lookup_type_map(cfg, unit->typedefs[i].name).size > 0) continue; // substituted at use sites, never declared
    NameOrigin origin;
    String8    name = compute_type_name_tracking_origin(a, cfg, unit->typedefs[i].name, &origin);
    report_push(a, &types, unit->typedefs[i].name, name, origin);
  }

  foreach_index(i, dyn_count(unit->functions)) {
    CFunction* fn       = &unit->functions[i];
    b32        excluded = false;
    foreach_index(ei, dyn_count(cfg->excluded_funcs)) {
      if (str8_match(cfg->excluded_funcs[ei], fn->name, 0)) { excluded = true; break; }
    }
    if (excluded) continue;
    NameOrigin origin;
    String8    name = compute_public_fn_name_tracking_origin(&es, fn->name, &origin);
    report_push(a, &functions, fn->name, name, origin);
  }

  fprintf(out, ";; `3b translate --names` for package %.*s -- C name, 3b name, and which\n"
                ";; config rule decided it. Not a package: nothing here is meant to compile.\n\n",
          str8_varg(cfg->package_name));
  report_print_section(out, "constants", constants);
  report_print_section(out, "types", types);
  report_print_section(out, "functions", functions);

  // Collisions are checked across the whole report, not per section:
  // try_register_name puts every top-level name in one flat namespace, so a
  // struct and a function claiming the same name collide just as surely as two
  // structs would.
  ReportRow* all = NULL;
  foreach_index(i, dyn_count(constants)) dyn_push(a, all, constants[i]);
  foreach_index(i, dyn_count(types)) dyn_push(a, all, types[i]);
  foreach_index(i, dyn_count(functions)) dyn_push(a, all, functions[i]);
  report_print_collisions(out, all);
}
