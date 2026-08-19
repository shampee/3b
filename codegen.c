// codegen.c -- the native backend: typed AST in, C source out.
//
// Emits one .c (and, for a non-root package, one .h) per package, which the
// driver then hands to the system C compiler. The generated code sits on top
// of base.h's runtime (arenas, String8, dynamic arrays, hash tables), embedded
// as text by runtime_embed.h.
//
// Three concerns dominate this file:
//   naming      3b identifiers are not C identifiers (`-`, `/`, trailing `?`)
//               and packages share one flat C namespace, so every top-level
//               reference is mangled and package-prefixed -- see
//               cg_symbol_name, the one place that happens.
//   declarators C puts part of a type after the name (`T x[N]`, `R (*f)(A)`),
//               so declarations go through cg_declare, not bare type text.
//   statement/  3b is expression-oriented and C is not; block-valued
//   expression  expressions become GCC statement-expressions (`({ ... })`).
//               See cg_block_expr.
//
// The parallel backend for the bytecode VM is bcgen.c.

#include "3b.h"
#include "runtime_embed.h" // g_embed_* string constants -- see cg_write_runtime_header/source

// Reaching this means codegen was handed a typed node or type it has no case
// for. That is a bug in THIS file (a new TypedNodeKind/TypeKind whose emission
// was never written), not in the program being compiled, and it has exactly
// one safe response: stop.
//
// The switches below used to degrade instead -- emitting the literal `0` in
// place of an expression, or a `// not supported yet` comment in place of a
// whole top-level declaration. Both produce C that compiles, so the failure
// surfaces as the program quietly doing the wrong thing at runtime, arbitrarily
// far from the construct that was dropped. A trap at the point of the gap costs
// the same to fix and cannot be missed.
//
// AssertAlways, not xassert: xassert compiles to `(void)` without XDEBUG (see
// base.h), and "the backend may silently emit wrong code in a release build"
// is precisely the property being removed here. The diagnostic goes out first
// so there is something to read besides a trap.
//
// A zero `tok.line` means no source location was reachable at the failing site
// (the type-name printers take a bare TypeRef, with no node behind it); the
// message then stands alone rather than pointing at a bogus 0:0 span.
static void
cg_internal_error(Token tok, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  String8 what = str8fv(ctx_perm(), (char*)fmt, args);
  va_end(args);

  if (tok.line == 0) {
    fprintf(stderr, "error: internal compiler error: %.*s\n", str8_varg(what));
  } else {
    diag_error(tok, "internal compiler error: %.*s", str8_varg(what));
  }
  fprintf(stderr, "this is a bug in 3b's C backend (codegen.c), not in the program being "
                   "compiled -- the `.3bs` bytecode backend reports its own gaps as ordinary "
                   "diagnostics instead, see bcgen.c's bc_unsupported\n");
  AssertAlways(false);
}

String8
c_mangle_name(Arena* arena, String8 name) {
  // A trailing `?` marks a predicate (`positive?`); it becomes a `_p` suffix
  // so it stays distinguishable from the general '-'/'/' -> '_' fallback.
  b32 is_predicate = name.size > 0 && name.str[name.size - 1] == '?';
  u64 core_size    = is_predicate ? name.size - 1 : name.size;
  u64 out_size     = core_size + (is_predicate ? 2 : 0);

  u8* buf = push_array(arena, u8, out_size == 0 ? 1 : out_size);
  foreach_index(i, core_size) {
    u8 c = name.str[i];
    // Genuine package-qualified references (`gl/gen-textures`) are resolved to
    // their prefixed C name during lowering, so '/' here is only a fallback
    // for an unrecognized "namespace/thing" -- it yields a valid C identifier,
    // hence a link error rather than invalid C.
    buf[i] = (c == '-' || c == '/') ? '_' : c;
  }
  if (is_predicate) {
    buf[core_size]     = '_';
    buf[core_size + 1] = 'p';
  }
  return str8(buf, out_size);
}

// Same mangling as c_mangle_name, but upper-cased -- used only for building
// `#ifndef`-style include guards out of a package name.
static String8
c_mangle_name_upper(Arena* arena, String8 name) {
  String8 mangled = c_mangle_name(arena, name);
  u8*     buf      = push_array(arena, u8, mangled.size == 0 ? 1 : mangled.size);
  foreach_index(i, mangled.size) { buf[i] = char_to_upper(mangled.str[i]); }
  return str8(buf, mangled.size);
}

// Builds cg->public_toplevel_names, the set backing
// cg_is_own_public_toplevel_name. Cached because the alternative -- a full
// top-level scan per symbol reference -- dominated compile time on packages
// with a large top-level surface, such as a GL binding.
//
// `extern` signatures (bodyless, never is_imported) are excluded: they name
// something that already exists in real C and must never be prefixed.
static void
cg_build_public_toplevel_names(Codegen* cg) {
  hashtable_init(ctx_perm(), &cg->public_toplevel_names, 64);
  if (cg->program_root == TYPED_NIL) return;
  TypedNode* root_n = &cg->tast->nodes[cg->program_root];
  foreach_index(i, root_n->block.stmt_count) {
    TypedIndex stmt = cg->tast->extra[root_n->block.stmt_first + i];
    if (stmt == TYPED_NIL) continue;
    TypedNode* sn = &cg->tast->nodes[stmt];
    if (sn->is_private || sn->is_imported) continue;
    String8 decl_name;
    switch (sn->kind) {
      case TypedNodeKind_FunctionDecl:
        if (sn->func.body == TYPED_NIL) continue; // real `extern` -- never prefixed
        decl_name = sn->func.name;
        break;
      case TypedNodeKind_StructDecl: decl_name = sn->struct_decl.name; break;
      case TypedNodeKind_EnumDecl:   decl_name = sn->enum_decl.name;   break;
      case TypedNodeKind_AliasDecl:  decl_name = sn->alias_decl.name;  break;
      case TypedNodeKind_ConstDecl:  decl_name = sn->const_decl.name;  break;
      case TypedNodeKind_VarDecl:    decl_name = sn->var_decl.name;    break;
      default: continue;
    }
    hashtable_insert(ctx_perm(), &cg->public_toplevel_names, decl_name, (void*)1, true);
  }
}

// Does `name` (exact, unqualified text -- no '/') name a public top-level
// declaration of THIS package? O(1) after the first call: program_root never
// changes for the lifetime of a Codegen (one per package), so the set built
// from it stays valid.
static b32
cg_is_own_public_toplevel_name(Codegen* cg, String8 name) {
  if (!cg->public_toplevel_names_built) {
    cg_build_public_toplevel_names(cg);
    cg->public_toplevel_names_built = true;
  }
  return hashtable_lookup(&cg->public_toplevel_names, name) != NULL;
}

// Builds cg->ffi_c_names from this package's `(struct data "cgltf_data" [...])`
// and `(enum PixelFormat "enum AVPixelFormat" [...])` pins. Only translated
// bindings have any, so the table is usually empty and every lookup below
// misses immediately.
static void
cg_build_ffi_c_names(Codegen* cg) {
  hashtable_init(ctx_perm(), &cg->ffi_c_names, 64);
  if (cg->program_root == TYPED_NIL) return;
  TypedNode* root_n = &cg->tast->nodes[cg->program_root];
  foreach_index(i, root_n->block.stmt_count) {
    TypedIndex stmt = cg->tast->extra[root_n->block.stmt_first + i];
    if (stmt == TYPED_NIL) continue;
    TypedNode* sn = &cg->tast->nodes[stmt];
    String8 name = {0}, c_name = {0};
    if (sn->kind == TypedNodeKind_StructDecl) { name = sn->struct_decl.name; c_name = sn->struct_decl.c_name; }
    else if (sn->kind == TypedNodeKind_EnumDecl) { name = sn->enum_decl.name; c_name = sn->enum_decl.c_name; }
    else continue;
    if (c_name.size == 0) continue;
    String8* boxed = push_one(ctx_perm(), String8);
    *boxed         = c_name;
    hashtable_insert(ctx_perm(), &cg->ffi_c_names, name, boxed, true);
  }
}

// The C type a 3b struct/enum name mirrors, or an empty String8 when it
// mirrors nothing -- which is the answer for everything outside a translated
// binding.
//
// A translated binding declares a MIRROR of each C struct it needs: a real,
// independent C struct with the same layout (translate/emit.c verifies that,
// and refuses to emit a mirror it cannot prove matches). Layout-compatible is
// not the same as type-compatible, though, so at the one point where the two
// meet -- an `extern` call into the real library -- gcc sees `cgltf_data*`
// where it wants `gltf_data*` and warns at every argument. See cg_ffi_cast_open
// for what this is used to emit there.
static String8
cg_ffi_c_type(Codegen* cg, String8 name) {
  if (!cg->ffi_c_names_built) {
    cg_build_ffi_c_names(cg);
    cg->ffi_c_names_built = true;
  }
  String8* found = (String8*)hashtable_lookup(&cg->ffi_c_names, name);
  return found ? *found : (String8){0};
}

// The one place package-prefixing happens. Every reference to a top-level
// symbol -- its definition and every use site, qualified (`gl/gen-textures`)
// or not (`gen-textures`, from within gl itself) -- routes through here, so
// all of them land on the same C name. Params, locals, `let` bindings and
// struct fields are not part of the package symbol space and call
// c_mangle_name directly instead.
//
// INVARIANT, enforced by the CALL SITES rather than here: this is a plain
// text match against declared top-level names, with no scope awareness. Every
// value-reference site (Identifier, Call callee, SetExpr target, dyn-push
// array name) checks cg_scope_lookup first and only falls back to this
// function when the name is not a visible local -- which is what keeps a
// param named `helper` from resolving to a top-level `helper`. Any new
// value-reference call site must repeat that guard. Type and declaration
// references need no guard, being unshadowable by a local.
static String8
cg_symbol_name(Codegen* cg, String8 name) {
  // `main` goes unprefixed only in the root package (or the single-file demo
  // path), where it is C's real entry point -- see cg_function_main. In a
  // merely-imported package it is an ordinary function that happens to be
  // named "main" and must be prefixed, or two packages each defining one
  // would collide at link time. The cg_is_own_public_toplevel_name guard
  // keeps this from firing for anything but a real top-level `main`.
  if (str8_match_lit("main", name, 0) && cg_is_own_public_toplevel_name(cg, name)
      && (cg->is_root_package || cg->package_name.size == 0)) return name;
  if (cg->package_name.size == 0) return c_mangle_name(ctx_scratch(), name); // demo path -- no packages at all
  b32 has_slash = false;
  foreach_index(i, name.size) { if (name.str[i] == '/') { has_slash = true; break; } }
  if (has_slash) return c_mangle_name(ctx_scratch(), name); // cross-package ref, already qualified by lowering:
                                              // mangling '/'->'_' yields the other package's prefix
  if (!cg_is_own_public_toplevel_name(cg, name)) return c_mangle_name(ctx_scratch(), name); // private/local/param
  String8 mangled_pkg  = c_mangle_name(ctx_scratch(), cg->package_name);
  String8 mangled_name = c_mangle_name(ctx_scratch(), name);
  return str8_cat(ctx_scratch(), str8_cat(ctx_scratch(), mangled_pkg, str8_lit("_")), mangled_name);
}

// C name for a Map/Set instantiation. Not package-prefixed, unlike
// cg_symbol_name: `(Map string i32)` must name the same C type in every
// package, or a value returned from one package's public fn and assigned in
// another would be two structurally identical but nominally distinct structs.
// Cross-package collisions are handled instead by cg_hashtable_instance_decl,
// which wraps the definition in an include guard keyed by this name -- so
// re-emission is a no-op, and since generation is deterministic it does not
// matter which package's identical copy wins.
static String8
cg_hashtable_c_name(Codegen* cg, TypeRef key_type, TypeRef* value_type) {
  (void)cg;
  return hashtable_mangled_name(ctx_perm(), key_type, value_type);
}

// cg_scope_reserve/register/lookup/mark/pop_to track every local binding (fn
// param, let binding, for-loop var, local val/var) so its C declaration never
// collides with an outer same-named one. Mirrors checker.c's Scope, but for C
// identifier disambiguation rather than type lookup.
//
// It exists because C's declarator scoping does not match 3b's: in
// `T x = f(x);` C treats the right-hand `x` as the new, uninitialized one,
// whereas 3b evaluates an init before binding its name, so the init of a
// shadowing `(let [x i32 (+ x 1)] ...)` must read the OUTER `x`. Mangling
// straight from source text produces `({ i32 x = 5; ({ i32 x = (x + 1); }); })`,
// where the inner read silently yields garbage.
//
// Reserve computes the C identifier without registering it -- see
// cg_scope_register for why the split matters.
String8
cg_scope_reserve(Codegen* cg, String8 name) {
  String8 plain = c_mangle_name(ctx_perm(), name); // perm, not scratch: outlives every later lookup
  b32 collides = false;
  foreach_index(i, dyn_count(cg->scope)) {
    if (str8_match(cg->scope[i].c_name, plain, 0)) { collides = true; break; }
  }
  if (!collides) return plain;
  String8 chosen = str8f(ctx_perm(), "%.*s__%u", str8_varg(plain), cg->next_disambig_id);
  cg->next_disambig_id += 1;
  return chosen;
}

// Pushes a binding (already named via cg_scope_reserve) onto the scope stack.
// The split from reserve is what makes the mechanism work: a binding's own
// init must resolve against the OUTER scope, so every call site reserves,
// emits the declaration and init under the reserved name, THEN registers.
// Registering first only moves the bug to the disambiguated name --
// `T x__0 = (x__0 + 1);` is as wrong as `T x = (x + 1);`.
void
cg_scope_register(Codegen* cg, String8 source_name, String8 c_name) {
  CgScopeEntry e = {0};
  e.source_name  = source_name;
  e.c_name        = c_name;
  dyn_push(ctx_perm(), cg->scope, e);
}

// Innermost-first (matches shadowing semantics): the most recently pushed
// entry with this source name wins. NULL means "not a local" -- callers
// fall back to cg_symbol_name for top-level/package resolution.
String8*
cg_scope_lookup(Codegen* cg, String8 name) {
  for (u64 i = dyn_count(cg->scope); i-- > 0; ) {
    if (str8_match(cg->scope[i].source_name, name, 0)) return &cg->scope[i].c_name;
  }
  return NULL;
}

// Innermost-first like cg_scope_lookup, but answers "is this a by-reference
// Vector parameter" (see CgScopeEntry) -- TypedNodeKind_Identifier uses it to
// decide whether to wrap the read in `(*...)`. Returns false, not "not found",
// for a name that is not in scope at all.
b32
cg_scope_is_vector_ref_param(Codegen* cg, String8 name) {
  for (u64 i = dyn_count(cg->scope); i-- > 0; ) {
    if (str8_match(cg->scope[i].source_name, name, 0)) return cg->scope[i].is_vector_ref_param;
  }
  return false;
}

u64
cg_scope_mark(Codegen* cg) { return dyn_count(cg->scope); }

void
cg_scope_pop_to(Codegen* cg, u64 mark) {
  if (cg->scope) dyn_hdr(cg->scope)->count = mark;
}

static String8 c_type_from_typeref_base_spelled(Codegen* cg, TypeRef t, b32 ffi);
// Forward-declared for cg_call's handle-deref/handle-free/handle-valid?;
// defined near cg_handle_pool_decl.
static String8 cg_handle_pool_global_name(Codegen* cg, String8 struct_name);

// `ffi` renders the type as the REAL C library spells it rather than as this
// package spells it: every mirrored struct/enum name (cg_ffi_c_type) is
// replaced by the C type it mirrors, at any pointer or callback depth. The two
// renderings are identical for everything that is not a translated mirror,
// which is how cg_ffi_cast_open decides whether a cast is needed at all.
static String8
c_type_from_typeref_spelled(Codegen* cg, TypeRef t, b32 ffi) {
  String8 base = c_type_from_typeref_base_spelled(cg, t, ffi);
  if (t.is_const) return str8f(ctx_scratch(), "const %.*s", str8_varg(base));
  return base;
}

// The type as THIS package spells it.
static String8
c_type_from_typeref(Codegen* cg, TypeRef t) {
  return c_type_from_typeref_spelled(cg, t, false);
}

// The type as the REAL C library spells it -- every mirrored struct/enum name
// replaced by the C type it mirrors, at any pointer or callback depth.
static String8
c_type_from_typeref_ffi(Codegen* cg, TypeRef t) {
  return c_type_from_typeref_spelled(cg, t, true);
}

// The unqualified type text; c_type_from_typeref above adds `const ` when
// t.is_const. Const composes through the recursive Pointer case for free
// because lower_type_node marks the innermost pointee rather than an outer
// pointer level: "pointer to const u8" is Pointer -> U8{is_const}, and the
// recursive call re-enters c_type_from_typeref, which picks up that bit and
// yields "const u8" before this level appends "*".
static String8
c_type_from_typeref_base_spelled(Codegen* cg, TypeRef t, b32 ffi) {
  // A type resolved from an `alias` prints under its own name, so `newi32`
  // appears as `newi32` at use sites like a hand-written C typedef.
  if (t.alias_name.size > 0) {
    if (ffi) {
      String8 pinned = cg_ffi_c_type(cg, t.alias_name);
      if (pinned.size > 0) return pinned;
      // A C typedef is transparent, so an alias of a primitive already spells
      // the same type the library does and must be left alone -- substituting
      // its underlying `u64` for a pinned `size_t` would undo exactly what
      // cg_alias_decl's pinned spelling exists to achieve. An alias of a
      // MIRROR is the opposite case: transparent down to the mirror, which is
      // the wrong C type. Telling the two apart is a matter of asking whether
      // the underlying renders differently under `ffi` at all.
      TypeRef under      = t;
      under.alias_name   = (String8){0};
      String8 under_ffi   = c_type_from_typeref_base_spelled(cg, under, true);
      String8 under_plain = c_type_from_typeref_base_spelled(cg, under, false);
      if (!str8_match(under_ffi, under_plain, 0)) return under_ffi;
    }
    return cg_symbol_name(cg, t.alias_name);
  }
  switch (t.kind) {
    case TypeKind_Unresolved: return str8_lit("i32"); // unreachable: def/var/fn/let are always explicitly typed
    case TypeKind_I8:         return str8_lit("i8");
    case TypeKind_I16:        return str8_lit("i16");
    case TypeKind_I32:        return str8_lit("i32");
    case TypeKind_I64:        return str8_lit("i64");
    case TypeKind_U8:         return str8_lit("u8");
    case TypeKind_U16:        return str8_lit("u16");
    case TypeKind_U32:        return str8_lit("u32");
    case TypeKind_U64:        return str8_lit("u64");
    case TypeKind_F32:        return str8_lit("f32");
    case TypeKind_F64:        return str8_lit("f64");
    // Real C `bool` (1 byte), NOT base.h's 4-byte `b32`: `3b translate` maps a
    // C header's `_Bool` fields straight to 3b `bool` (translate/emit.c), so
    // anything wider misplaces every field declared after a bool one in a
    // translated struct. <stdbool.h> arrives transitively.
    case TypeKind_Bool:       return str8_lit("bool");
    case TypeKind_Char:       return str8_lit("char");
    case TypeKind_String:     return str8_lit("bbb_String8");
    case TypeKind_Void:       return str8_lit("void");
    case TypeKind_Any:        return str8_lit("void*");
    case TypeKind_Arena:      return str8_lit("bbb_Arena");     // by-value handle, never `bbb_Arena*` at the language level
    case TypeKind_ArenaMark:  return str8_lit("bbb_ArenaMark");
    case TypeKind_Stream:     return str8_lit("bbb_Stream");    // a typedef'd pointer, never `bbb_Stream*` at the language level
    case TypeKind_Pointer: {
      // A NULL pointee is the wildcard `nil` -- a pointer to nothing in
      // particular, assignable to any pointer type (see TypeRef's own note).
      // `void*` is what that is in C.
      if (t.pointee == NULL) return str8_lit("void*");
      // Recursion handles arbitrary depth: T** is (T + "*") + "*".
      String8 inner = c_type_from_typeref_spelled(cg, *t.pointee, ffi);
      return str8f(ctx_scratch(), "%.*s*", str8_varg(inner));
    }
    case TypeKind_Handle: {
      // `T##Handle`, the typedef base.h's DEFINE_HANDLE_POOL generates (see
      // cg_handle_pool_decl); prefixed like the backing struct's own name so
      // the two agree.
      String8 struct_c_name = cg_symbol_name(cg, t.name);
      return str8f(ctx_scratch(), "%.*sHandle", str8_varg(struct_c_name));
    }
    case TypeKind_Array: {
      // Only valid as an abstract type-name (`sizeof(i32[10])`); "i32[10] x;"
      // is not legal C, so named declarations go through cg_declare, which
      // puts the `[N]` after the name.
      String8 inner = c_type_from_typeref_spelled(cg, *t.pointee, ffi);
      return str8f(ctx_scratch(), "%.*s[%llu]", str8_varg(inner), (unsigned long long)t.count);
    }
    case TypeKind_Vector: {
      // `(Vector T)` is just `T*`, a dyn-push-grown pointer -- see
      // TypeKind_Vector in 3b.h. No monomorphized struct, no new runtime type.
      String8 inner = c_type_from_typeref(cg, *t.pointee);
      return str8f(ctx_scratch(), "%.*s*", str8_varg(inner));
    }
    case TypeKind_Map:  return cg_hashtable_c_name(cg, *t.map_key, t.pointee);
    case TypeKind_Set:  return cg_hashtable_c_name(cg, *t.pointee, NULL);
    case TypeKind_Named: {
      // Matches how struct/enum decls emit their own names -- except under
      // `ffi`, where a mirror gives way to the C type it mirrors.
      if (ffi) {
        String8 pinned = cg_ffi_c_type(cg, t.name);
        if (pinned.size > 0) return pinned;
      }
      return cg_symbol_name(cg, t.name);
    }
    case TypeKind_Fn: {
      // Abstract type-name only, like Array above: `R (*)(A)` has no room for
      // a name. cg_declare puts the name inside the `(*name)`.
      String8 ret = c_type_from_typeref_spelled(cg, *t.fn_return, ffi);
      if (t.fn_param_count == 0) return str8f(ctx_scratch(), "%.*s (*)(void)", str8_varg(ret));
      String8 params = str8_lit("");
      foreach_index(i, t.fn_param_count) {
        String8 p = c_type_from_typeref_spelled(cg, t.fn_params[i], ffi);
        params    = (i == 0) ? p : str8f(ctx_scratch(), "%.*s, %.*s", str8_varg(params), str8_varg(p));
      }
      return str8f(ctx_scratch(), "%.*s (*)(%.*s)", str8_varg(ret), str8_varg(params));
    }
  }
  // Unreachable for any TypeKind the switch above names, and -Wall's -Wswitch
  // (no `default:` here, deliberately) flags a newly added one at compile time.
  // This catches the case that warning cannot: a TypeRef carrying a kind
  // outside the enum. It used to `return str8_lit("i32")`, which turned a
  // pointer or a struct into an int in the emitted declaration.
  cg_internal_error((Token){0}, "no C type text for TypeKind %d", (int)t.kind);
  return str8_lit("");
}

// Innermost non-array type: "array of 4 arrays of 3 i32" yields i32.
static TypeRef
cg_array_base_type(TypeRef t) {
  while (t.kind == TypeKind_Array) t = *t.pointee;
  return t;
}

// Whether `t` carries a `const` anywhere. Cheap structural check, used to skip
// the rendering work in cg_needs_const_bridge_cast for the overwhelming majority
// of types, which have no const in them at all.
static b32
cg_type_contains_const(TypeRef t, u32 depth) {
  if (t.is_const) return true;
  if (depth > 8) return false;
  if ((t.kind == TypeKind_Pointer || t.kind == TypeKind_Array || t.kind == TypeKind_Vector)
      && t.pointee != NULL) return cg_type_contains_const(*t.pointee, depth + 1);
  return false;
}

// Whether emitting `actual` where `expected` is wanted needs a cast for the
// sake of C alone -- i.e. whether `expected` is const-qualified somewhere and
// the two render as different C types.
//
// TypeRef.is_const is a codegen-only flag: the checker ignores it entirely, so
// it happily passes a `u8**` argument to a `(const u8**)` parameter, or a
// `char**` to a `(const GLchar**)` one. C is stricter -- adding const below the
// FIRST pointer level is not an implicit conversion, so `unsigned char**` into
// `const unsigned char**` warns even though 3b considers the two the same type,
// and 3b offers no way to write the cast by hand (`cast` takes a bare type
// name, which cannot carry a qualifier). Bridging it here keeps codegen from
// manufacturing a C type error out of a program the checker deliberately
// accepted.
//
// The cast is sound because every pair reaching this point already passed
// checker.c's type_ref_assignable; the const qualifier is the only part of the
// conversion the checker was not consulted about. Narrowing to const-qualified
// `expected` is what keeps this from silently papering over an ordinary
// pointer-type mismatch, which is still gcc's to report.
static b32
cg_needs_const_bridge_cast(Codegen* cg, TypeRef expected, TypeRef actual) {
  if (!cg_type_contains_const(expected, 0)) return false; // the common case, and cheap
  return !str8_match(c_type_from_typeref(cg, expected), c_type_from_typeref(cg, actual), 0);
}

// Emits `idx`, wrapped in a cast to `expected` when this is an array- or
// Vector-to-pointer decay, or when the two differ only in constness. checker.c's
// type_ref_assignable already validated the conversion; this only detects the
// shape.
//
// C strips one array level per use, so `[T N]` -> `T*` needs no cast, but
// `[[T N] M]` flattened to `T*` (a Mat4's `v` field) does. Emitting the cast
// unconditionally is a no-op in the shallow case and keeps one code path
// correct at any depth. A Vector is already a pointer, but a `val`-declared
// one is a const-qualified pointer (see cg_declare_and_init), and the cast
// strips that qualifier rather than warning at, say, cg_foreach_expr's
// `_3b_foreach_coll` temp.
//
// Three callers: a for-each collection, a val/var/let initializer, and a `set`
// target's value. `expected` is legitimately TypeKind_Vector in the latter two
// -- storing a returned Vector into a Vector slot is a plain pointer copy --
// so this function must not special-case Vector. cg_call_arg_decay_to below
// does that, and only at a call-argument site.
static void
cg_expr_decay_to(Codegen* cg, TypedIndex idx, TypeRef expected) {
  TypeRef actual = cg->resolved_types[idx];
  if (((actual.kind == TypeKind_Array || actual.kind == TypeKind_Vector)
       && expected.kind == TypeKind_Pointer && expected.pointee != NULL)
      || cg_needs_const_bridge_cast(cg, expected, actual)) {
    fprintf(cg->out, "(%.*s)(", str8_varg(c_type_from_typeref(cg, expected)));
    cg_expr(cg, idx);
    fprintf(cg->out, ")");
  } else {
    cg_expr(cg, idx);
  }
}

// Whether `t` is or contains `any` -- at any pointer, array or callback depth.
// `any` is what a translated binding writes when the C type is deliberately not
// modelled: `force-opaque`, or one of C's opaque-handle idioms (see
// capture_typedef). See cg_ffi_cast_open for why that matters at an FFI call.
static b32
cg_type_contains_any(TypeRef t, u32 depth) {
  if (depth > 8) return false; // far past any real signature; guards a malformed cycle
  if (t.kind == TypeKind_Any) return true;
  if ((t.kind == TypeKind_Pointer || t.kind == TypeKind_Array || t.kind == TypeKind_Vector)
      && t.pointee != NULL && cg_type_contains_any(*t.pointee, depth + 1)) return true;
  if (t.kind == TypeKind_Fn) {
    if (t.fn_return != NULL && cg_type_contains_any(*t.fn_return, depth + 1)) return true;
    foreach_index(i, t.fn_param_count) {
      if (cg_type_contains_any(t.fn_params[i], depth + 1)) return true;
    }
  }
  return false;
}

// Emits the open half of an FFI cast -- `(const cgltf_data*)` -- for a value
// crossing between a translated binding's mirror types and the real C library,
// and returns whether it wrote anything. `to_c` picks the direction: true for
// an argument going into the library, false for a result coming back out.
//
// Two things make a value need one, and nothing else does:
//
//  1. A MIRROR type. The binding declares its own struct/enum with the C
//     original's layout, so `gltf_data*` and `cgltf_data*` are the same bytes
//     under different C type names. The two renderings of `t` differ exactly
//     when a mirror is involved, which is the test. The cast is sound because
//     translate/emit.c refuses to emit a mirror whose layout it cannot prove
//     identical to the original's -- see its mirror_layout_of_* checks.
//
//  2. An `any` type BELOW a pointer. `any` is `void*`, which C already converts
//     to and from any object pointer implicitly -- but `any*` is `void**`,
//     which converts to `AVDictionary**` neither way. There is nothing to
//     reconstruct the real type from, `any` being precisely the binding saying
//     it does not model this type, so the cast goes through `void*` and lets
//     C's own void-pointer conversion land it. A bare `any` argument is left
//     alone: that one already converts implicitly, and a cast would be noise.
//     A bare `any` RETURN still gets one, since dropping a `const T*` result
//     into `void*` is a discarded-qualifier warning that the cast answers.
//
// Note this deliberately casts the ARGUMENTS rather than the callee's function
// pointer: casting the pointer would silence the whole prototype, including a
// genuine arity or scalar-type mismatch against a header that has moved on
// since the binding was generated.
//
// The caller must emit the matching `)` -- wrapping the argument or the whole
// call -- when this returns true.
static b32
cg_ffi_cast_open(Codegen* cg, TypeRef t, b32 to_c) {
  String8 as_3b = c_type_from_typeref(cg, t);
  String8 as_c  = c_type_from_typeref_ffi(cg, t);
  if (!str8_match(as_3b, as_c, 0)) {
    fprintf(cg->out, "(%.*s)(", str8_varg(to_c ? as_c : as_3b));
    return true;
  }
  if (!cg_type_contains_any(t, 0)) return false;
  if (to_c && t.kind != TypeKind_Pointer) return false;
  fprintf(cg->out, "(%.*s)(", str8_varg(to_c ? str8_lit("void*") : as_3b));
  return true;
}

// Call-argument counterpart to cg_expr_decay_to, used only on cg_call's
// user-function path. A Vector parameter is by reference (cg_declare_param's
// T**), so the argument is wrapped in `&(...)`. checker.c requires such an
// argument be addressable -- an identifier/field/index/deref, never a
// temporary -- so the address-of is always valid here, unlike in
// cg_expr_decay_to's other two Vector callers, which assign a fresh Vector
// value. Forwarding one by-reference Vector param to another needs no special
// case: cg_expr already emits it as `(*name)`, and `&(*name)` is `name`.
static void
cg_call_arg_decay_to(Codegen* cg, TypedIndex idx, TypeRef expected) {
  if (expected.kind == TypeKind_Vector) {
    fprintf(cg->out, "(&");
    cg_expr(cg, idx);
    fprintf(cg->out, ")");
  } else {
    cg_expr_decay_to(cg, idx, expected);
  }
}

// The general C declarator form, "TypePrefix Name TypeSuffix". Arrays and
// function pointers are the cases where C puts part of the type after (or
// around) the name -- `T name[N]`, `R (*name)(A)` -- so every declaration site
// (params, struct fields, let bindings, val/var) goes through this rather than
// concatenating c_type_from_typeref with a name. Nested arrays emit
// outer-to-inner: Array{4, Array{3, i32}} gives "i32 name[4][3]".
static void
cg_declare(Codegen* cg, TypeRef t, String8 name) {
  if (t.kind == TypeKind_Fn) {
    // The other C declarator that puts part of the type AFTER the name --
    // `RetType (*name)(ParamTypes)`, name wrapped in `(*...)` so it binds
    // to the POINTER rather than (per C's usual "declarator mirrors use"
    // reading) to a function returning a pointer. Params go through the
    // ordinary c_type_from_typeref (not cg_declare) since C's function-
    // pointer syntax has no room for a name per parameter here anyway --
    // matches every real C function-pointer typedef/param.
    fprintf(cg->out, "%.*s (*%.*s)(", str8_varg(c_type_from_typeref(cg, *t.fn_return)), str8_varg(name));
    if (t.fn_param_count == 0) {
      fprintf(cg->out, "void");
    } else {
      foreach_index(i, t.fn_param_count) {
        if (i != 0) fprintf(cg->out, ", ");
        fprintf(cg->out, "%.*s", str8_varg(c_type_from_typeref(cg, t.fn_params[i])));
      }
    }
    fprintf(cg->out, ")");
    return;
  }
  if (t.kind != TypeKind_Array) {
    fprintf(cg->out, "%.*s %.*s", str8_varg(c_type_from_typeref(cg, t)), str8_varg(name));
    return;
  }
  fprintf(cg->out, "%.*s %.*s", str8_varg(c_type_from_typeref(cg, cg_array_base_type(t))), str8_varg(name));
  TypeRef cur = t;
  while (cur.kind == TypeKind_Array) {
    fprintf(cg->out, "[%llu]", (unsigned long long)cur.count);
    cur = *cur.pointee;
  }
}

// Where a `val` binding's C `const` belongs, if anywhere. `val` makes the NAME
// immutable and says nothing about what the name refers to, but C's `const T x`
// says both at once for some types and the wrong one for others, so the
// qualifier cannot simply be prefixed. Note this is about the `val` FORM;
// TypeRef.is_const, from an explicit `(const T)` in type position, is a
// separate thing that c_type_from_typeref handles.
typedef enum CgValConst {
  // No `const` at all. These are by-value HANDLES to mutable state -- the
  // handle is bound immutably, but the state behind it is the whole point.
  // Every runtime entry point takes `&handle` as a plain `T*`, read-only ones
  // (Set_contains, Map_get) included, so const-qualifying the handle does not
  // make it read-only, it makes it unusable. checker.c is what enforces `val`
  // for these; C `const` cannot express the distinction.
  CgValConst_None,
  // `const T name`. T is the whole object and there is nothing further to
  // reach through, so C's meaning and `val`'s coincide.
  CgValConst_Prefix,
  // `T* const name`, not `const T* name`. The latter is C for "pointer to
  // const T" -- which is what 3b spells `(const T)*`, a different type, and
  // which every caller passing the pointer to a mutating function then trips
  // over. The qualifier belongs to the pointer.
  CgValConst_Suffix,
} CgValConst;

static CgValConst
cg_val_const_placement(TypeRef t) {
  switch (t.kind) {
    case TypeKind_Arena:
    case TypeKind_Vector:
    case TypeKind_Map:
    case TypeKind_Set:
    case TypeKind_Stream:  return CgValConst_None;
    case TypeKind_Pointer:
    case TypeKind_Any:     // `void*`
    case TypeKind_Fn:      // a function POINTER: `R (*const name)(...)`
      return CgValConst_Suffix;
    default:               return CgValConst_Prefix;
  }
}

// cg_declare for a `val` binding, at any scope. Every site that declares one
// goes through this, including the forward `extern`/`static` declarations,
// which have to match the definition exactly or C rejects the pair.
//
// Suffix placement needs no special declarator handling: cg_declare emits the
// name immediately after the `*` in both the pointer form (`i32* NAME`) and the
// function-pointer form (`R (*NAME)(...)`), so prepending `const ` to the name
// lands it in the right place either way.
static void
cg_declare_val(Codegen* cg, TypeRef t, String8 name) {
  switch (cg_val_const_placement(t)) {
    case CgValConst_None:   cg_declare(cg, t, name); break;
    case CgValConst_Prefix: fprintf(cg->out, "const "); cg_declare(cg, t, name); break;
    case CgValConst_Suffix: cg_declare(cg, t, str8f(ctx_scratch(), "const %.*s", str8_varg(name))); break;
  }
}

// cg_declare for a function parameter. A Vector parameter is declared one
// pointer level deeper (T**, not T*) than anywhere else, which is what lets
// vector-push grow the CALLER's Vector rather than this function's copy of the
// pointer -- see CgScopeEntry. Every other type declares identically to
// cg_declare. Only cg_function_impl and cg_function_prototype call this.
static void
cg_declare_param(Codegen* cg, TypeRef t, String8 name) {
  if (t.kind == TypeKind_Vector) {
    fprintf(cg->out, "%.*s* %.*s", str8_varg(c_type_from_typeref(cg, t)), str8_varg(name));
    return;
  }
  cg_declare(cg, t, name);
}

// Same dimension-walking as cg_declare, but for C99 compound-literal casts
// (`(i32[4][3]){...}`), which have no name to put the dimensions after.
static void
cg_array_type_cast_text(Codegen* cg, TypeRef t) {
  fprintf(cg->out, "%.*s", str8_varg(c_type_from_typeref(cg, cg_array_base_type(t))));
  TypeRef cur = t;
  while (cur.kind == TypeKind_Array) {
    fprintf(cg->out, "[%llu]", (unsigned long long)cur.count);
    cur = *cur.pointee;
  }
}

// Same dimension-walking again, but declaring a POINTER to the array rather
// than the array: `i32 (*name)[4][3]`. Only the array-typed `set` in cg_expr
// wants this -- see there for why a pointer to the whole array beats a pointer
// to its base type.
static void
cg_declare_array_ptr(Codegen* cg, TypeRef t, String8 name) {
  fprintf(cg->out, "%.*s (*%.*s)", str8_varg(c_type_from_typeref(cg, cg_array_base_type(t))),
          str8_varg(name));
  TypeRef cur = t;
  while (cur.kind == TypeKind_Array) {
    fprintf(cg->out, "[%llu]", (unsigned long long)cur.count);
    cur = *cur.pointee;
  }
}

// Some valid C expression of the given type, holding zero. Used only as the
// dummy trailing value of `return`'s statement-expression (see
// TypedNodeKind_ReturnExpr) so it has a well-defined non-void type; the real
// `return` ahead of it always exits first, so the value is never observed. `0`
// covers numeric, bool and pointer types; a zero compound literal covers Named
// ones, structs and enums alike. Never reached for Array -- a fn cannot return
// one.
static void
cg_zero_value_for_type(Codegen* cg, TypeRef t) {
  if (t.kind == TypeKind_Void) { fprintf(cg->out, "(void)0"); return; }
  if (t.kind == TypeKind_Named) { fprintf(cg->out, "(%.*s){0}", str8_varg(c_type_from_typeref(cg, t))); return; }
  fprintf(cg->out, "0");
}

// Defined further down, near cg_struct_literal.
static void cg_array_literal_braces(Codegen* cg, TypedIndex idx);
static void cg_init_value(Codegen* cg, TypedIndex value_idx, TypeRef declared_type);
static void cg_declare_and_init(Codegen* cg, TypeRef type, String8 c_name, TypedIndex init_idx);
static void cg_declare_and_init_val(Codegen* cg, TypeRef type, String8 c_name, TypedIndex init_idx);
// Defined near cg_struct_decl. cg_expr's equality and ordering cases need to
// know whether a Named type is a struct (use its synthesized `_eq`/`_cmp`) or
// not (use a bare C operator); cg_call's vector-contains?/vector-index-of need
// the field comparator.
static StructEntry* cg_struct_lookup(Codegen* cg, String8 name);
static void cg_emit_field_eq_expr(Codegen* cg, TypeRef t, String8 a_expr, String8 b_expr);

// One arm of an `if` emitted as a C ternary. The two arms must agree in type
// there, which an arm that DIVERGES cannot do on its own: `(break)`,
// `(continue)` and bare `(return)` all emit a `({ ... (void)0; })` whose type
// is void, so pairing one with a valued arm -- `(+= total (if done (break) i))`
// -- gave gcc "void value not ignored as it ought to be", a C-level message
// about generated code the author never wrote.
//
// The fix is the one TypedNodeKind_ReturnExpr already applies to itself, with
// the type coming from the sibling arm rather than from the function: wrap the
// diverging arm and append a zero of the whole `if`'s type. The jump ahead of
// it always leaves first, so the value is emitted purely to satisfy C's type
// rules and is never observed -- which is also why the wrap is skipped unless
// it's needed, keeping the common case's output unchanged.
//
// `(return x)` needs no help and gets none: its own dummy already carries the
// returned value's type. Only VALUELESS jumps land here.
static void
cg_if_branch(Codegen* cg, TypedIndex branch, TypeRef if_ty) {
  b32 branch_diverges = cg->resolved_types[branch].kind == TypeKind_Unresolved;
  b32 value_wanted    = if_ty.kind != TypeKind_Unresolved && if_ty.kind != TypeKind_Void;
  if (!branch_diverges || !value_wanted) {
    cg_expr(cg, branch);
    return;
  }
  fprintf(cg->out, "({ ");
  cg_expr(cg, branch);
  fprintf(cg->out, "; ");
  cg_zero_value_for_type(cg, if_ty);
  fprintf(cg->out, "; })");
}

// Emits one statement of a block (fn body, `do`, `let` body, loop body). Every
// site that iterates a Block's statements goes through this rather than
// cg_expr, because a nested `val`/`var` must emit a plain non-static C
// declaration, which is not an expression as cg_expr's contract requires.
// Unlike `while`/`for`, a declaration has no sensible throwaway value, so it
// is not wrapped in `({ ...; (void)0; })`; check_block instead forbids a
// declaration from being a block's result.
static void
cg_stmt(Codegen* cg, TypedIndex stmt_idx) {
  TypedNode* n = &cg->tast->nodes[stmt_idx];
  if (n->kind == TypedNodeKind_ConstDecl) {
    String8 c_name = cg_scope_reserve(cg, n->const_decl.name);
    cg_declare_and_init_val(cg, n->const_decl.type, c_name, n->const_decl.init);
    cg_scope_register(cg, n->const_decl.name, c_name);
  } else if (n->kind == TypedNodeKind_VarDecl) {
    String8 c_name = cg_scope_reserve(cg, n->var_decl.name);
    cg_declare_and_init(cg, n->var_decl.type, c_name, n->var_decl.init); // no qualifier -- ordinary mutable local
    cg_scope_register(cg, n->var_decl.name, c_name);
  } else {
    cg_expr(cg, stmt_idx);
  }
}

void
cg_write_c_escaped(Codegen* cg, String8 s) {
  foreach_index(i, s.size) {
    u8 c = s.str[i];
    switch (c) {
      case '\\': fprintf(cg->out, "\\\\"); break;
      case '"':  fprintf(cg->out, "\\\""); break;
      case '\n': fprintf(cg->out, "\\n");  break;
      case '\t': fprintf(cg->out, "\\t");  break;
      case '\r': fprintf(cg->out, "\\r");  break;
      default:   fputc(c, cg->out);        break;
    }
  }
}

// `.` binds tighter than unary `*`/`&` or a cast, so `*ptr.field` parses as
// `*(ptr.field)` rather than the intended `(*ptr).field`. Every other node kind
// that can be a FieldAccess base is already safe: binary ops, if and set
// self-parenthesize, `({ })` is inherently grouped, and calls, struct literals,
// further field accesses and atoms are postfix-safe.
b32
cg_needs_parens_before_dot(Codegen* cg, TypedIndex idx) {
  TypedNode* n = &cg->tast->nodes[idx];
  return n->kind == TypedNodeKind_UnaryDeref
      || n->kind == TypedNodeKind_UnaryAddr
      || n->kind == TypedNodeKind_BinaryCast;
}

// Emits a Block node as a GNU statement expression: `({ s1; s2; last; })`.
void
cg_block_as_expr(Codegen* cg, TypedIndex block_idx) {
  TypedNode* n = &cg->tast->nodes[block_idx];
  xassert(n->kind == TypedNodeKind_Block);
  u64 mark = cg_scope_mark(cg);
  fprintf(cg->out, "({ ");
  foreach_index(i, n->block.stmt_count) {
    TypedIndex stmt = cg->tast->extra[n->block.stmt_first + i];
    cg_stmt(cg, stmt);
    fprintf(cg->out, "; ");
  }
  fprintf(cg->out, "})");
  cg_scope_pop_to(cg, mark);
}

// Some bindings are written by the compiler rather than by the programmer -- a
// destructuring pattern's fields, a multi-return `let`'s second slot, a `for`'s
// index -- and the body is under no obligation to mention them. Those still
// have to be declared, because the name is part of what the source asked for
// and skipping it would leave the generated C not resembling the 3b it came
// from, so the redundant-looking read is what tells GCC the omission was
// deliberate. `is_read` comes from the checker, which resolves names exactly
// (see ScopeEntry.was_read); it errs toward "read", so a spurious `(void)x;`
// is possible but a missed one is not.
static void
cg_mark_used_if_unread(Codegen* cg, String8 c_name, b32 is_read) {
  if (!is_read) fprintf(cg->out, " (void)%.*s;", str8_varg(c_name));
}

// `let`'s bindings and body share one statement-expression scope, so the body
// sees the bindings without a second nested `({ })` from cg_block_as_expr.
void
cg_let_expr(Codegen* cg, TypedIndex idx) {
  TypedNode* n = &cg->tast->nodes[idx];
  u64 mark = cg_scope_mark(cg);
  fprintf(cg->out, "({ ");
  foreach_index(i, n->let_expr.binding_count) {
    Binding* b      = &cg->tast->bindings[n->let_expr.binding_first + i];
    String8  c_name = cg_scope_reserve(cg, b->name);
    cg_declare_and_init(cg, b->type, c_name, b->init);
    cg_mark_used_if_unread(cg, c_name, b->is_read);
    fprintf(cg->out, " ");
    cg_scope_register(cg, b->name, c_name); // AFTER the init -- see cg_scope_register's comment
  }
  TypedNode* body = &cg->tast->nodes[n->let_expr.body];
  xassert(body->kind == TypedNodeKind_Block);
  foreach_index(i, body->block.stmt_count) {
    TypedIndex stmt = cg->tast->extra[body->block.stmt_first + i];
    cg_stmt(cg, stmt);
    fprintf(cg->out, "; ");
  }
  fprintf(cg->out, "})");
  cg_scope_pop_to(cg, mark);
}

// End the `scratch` scopes open at this point down to `floor`, innermost
// first, for a jump that is about to leave all of them. Falling off the end of
// a scratch block rewinds it; a jump from inside one used to sail straight past
// that bbb_arena_temp_end, leaving the thread-local scratch arena at its
// high-water mark until something unrelated reset it. Ending only the innermost
// would restore the same arena position, but ending each in turn keeps every
// ArenaTemp balanced, which is what the arena's own debug checking expects.
//
// `floor` is what distinguishes the two jump kinds, and it is not optional.
// `return` leaves the FUNCTION, so it unwinds everything: floor 0. `break` and
// `continue` leave only the innermost LOOP, so they unwind down to
// Codegen.loop_scratch_depth -- the depth that loop started at. A `scratch`
// that ENCLOSES the loop is still live after the jump, and ending it here would
// both rewind memory still in use and leave its own arena_temp_end at the
// block's end running a second time.
static void
cg_unwind_scratch_scopes(Codegen* cg, u32 floor) {
  for (u32 depth = cg->scratch_depth; depth > floor; depth -= 1) {
    fprintf(cg->out, "bbb_arena_temp_end(&_3b_scratch_temp_%u); ", depth - 1);
  }
}

// `(scratch [t] body...)`. arena_temp_end has to run after the body's value is
// computed but before the expression yields it, so this cannot follow
// cg_let_expr's "last statement is the value" shape -- that would make the
// cleanup call the result and discard what the body computed. Instead the body
// value goes into a temp local, cleanup runs, then the temp is yielded.
//
// `_3b_scratch_result` needs no unique-name generation: each scratch is its own
// `({ })`, hence its own C block scope. The ArenaTemp does, and carries the
// nesting depth, because cg_unwind_scratch_scopes has to name an OUTER scope's
// temp from inside an inner one -- a shared name would leave it shadowed.
void
cg_scratch_expr(Codegen* cg, TypedIndex idx) {
  TypedNode* n    = &cg->tast->nodes[idx];
  TypedNode* body = &cg->tast->nodes[n->scratch_expr.body];
  xassert(body->kind == TypedNodeKind_Block);
  TypeRef body_ty = cg->resolved_types[n->scratch_expr.body];

  u32 depth = cg->scratch_depth;
  u64 mark  = cg_scope_mark(cg);
  fprintf(cg->out, "({ bbb_ArenaTemp _3b_scratch_temp_%u = bbb_arena_temp_begin(bbb_ctx_scratch()); ", depth);
  String8 c_name = cg_scope_reserve(cg, n->scratch_expr.var_name);
  fprintf(cg->out, "bbb_Arena %.*s = *bbb_ctx_scratch(); ", str8_varg(c_name));
  cg_scope_register(cg, n->scratch_expr.var_name, c_name); // AFTER the decl, same as cg_let_expr's bindings
  cg->scratch_depth = depth + 1;

  // Unresolved joins Void here: it means the body DIVERGES -- its last
  // statement is a `return`, `break` or `continue` -- so control never reaches
  // the capture and there is no value for it to hold. Declaring the temp
  // anyway made gcc reject `(scratch [t] ... (break))` outright ("void value
  // not ignored as it ought to be"), a bare `(return)` being the same shape;
  // `(return x)` only slipped through because its dummy trailing value gave
  // the assignment something typed to read.
  if (body_ty.kind == TypeKind_Void || body_ty.kind == TypeKind_Unresolved) {
    // Nothing to capture: run the body for effect, clean up, yield `(void)0`
    // -- the convention cg_for_range_expr and cg_loop_body_block also use.
    foreach_index(i, body->block.stmt_count) {
      cg_stmt(cg, cg->tast->extra[body->block.stmt_first + i]);
      fprintf(cg->out, "; ");
    }
    fprintf(cg->out, "bbb_arena_temp_end(&_3b_scratch_temp_%u); (void)0; })", depth);
  } else {
    String8 c_ty = c_type_from_typeref(cg, body_ty);
    fprintf(cg->out, "%.*s _3b_scratch_result = ({ ", str8_varg(c_ty));
    foreach_index(i, body->block.stmt_count) {
      cg_stmt(cg, cg->tast->extra[body->block.stmt_first + i]);
      fprintf(cg->out, "; ");
    }
    fprintf(cg->out, "}); bbb_arena_temp_end(&_3b_scratch_temp_%u); _3b_scratch_result; })", depth);
  }
  cg->scratch_depth = depth;
  cg_scope_pop_to(cg, mark);
}

// The `{ stmt; ...; }` body shared by the loop forms below. A plain C compound
// statement, not a `({ })` statement expression, since it follows a
// `while`/`for` header rather than sitting in expression position.
//
// This and cg_foreach_body_stmts are the two places a loop body is emitted, so
// they are also where Codegen.loop_scratch_depth is set -- the checker's
// check_loop_body plays the same chokepoint role for loop_depth, for the same
// reason: a loop form added later cannot forget.
static void
cg_loop_body_block(Codegen* cg, TypedIndex block_idx) {
  TypedNode* body = &cg->tast->nodes[block_idx];
  xassert(body->kind == TypedNodeKind_Block);
  u64 mark               = cg_scope_mark(cg);
  u32 saved_loop_floor   = cg->loop_scratch_depth;
  cg->loop_scratch_depth = cg->scratch_depth; // `break`/`continue` unwind only down to here
  fprintf(cg->out, "{ ");
  foreach_index(i, body->block.stmt_count) {
    cg_stmt(cg, cg->tast->extra[body->block.stmt_first + i]);
    fprintf(cg->out, "; ");
  }
  fprintf(cg->out, "}");
  cg->loop_scratch_depth = saved_loop_floor;
  cg_scope_pop_to(cg, mark);
}

// `while` has no meaningful value, but every TypedNode must still emit as one
// valid C expression, so the statement is wrapped in a statement expression
// ending in a throwaway `(void)0`.
void
cg_while_expr(Codegen* cg, TypedIndex idx) {
  TypedNode* n = &cg->tast->nodes[idx];
  fprintf(cg->out, "({ while (");
  cg_expr(cg, n->while_expr.cond);
  fprintf(cg->out, ") ");
  cg_loop_body_block(cg, n->while_expr.body);
  fprintf(cg->out, " (void)0; })");
}

void
cg_for_c_expr(Codegen* cg, TypedIndex idx) {
  TypedNode* n         = &cg->tast->nodes[idx];
  TypeRef    var_ty    = cg->resolved_types[n->for_c.init]; // init's checked type == loop var's type
  String8    c_ty      = c_type_from_typeref(cg, var_ty);
  u64        mark      = cg_scope_mark(cg);
  String8    var_name  = cg_scope_reserve(cg, n->for_c.var_name);
  fprintf(cg->out, "({ for (%.*s %.*s = ", str8_varg(c_ty), str8_varg(var_name));
  cg_expr(cg, n->for_c.init);
  fprintf(cg->out, "; ");
  cg_expr(cg, n->for_c.cond);
  fprintf(cg->out, "; ");
  cg_expr(cg, n->for_c.expr);
  fprintf(cg->out, ") ");

  cg_scope_register(cg, n->for_c.var_name, var_name);
  cg_loop_body_block(cg, n->for_c.body);
  fprintf(cg->out, " (void)0; })");
  cg_scope_pop_to(cg, mark);
}

// Range `for`, wrapped like `while` above. `end` is exclusive and iteration is
// ascending only: the emitted guard is always `i < end`, so a negative step
// runs zero times rather than counting down. Supporting one would need a
// compile-time-constant step to pick the comparison direction, or a guard that
// tests the sign at runtime.
void
cg_for_range_expr(Codegen* cg, TypedIndex idx) {
  TypedNode* n         = &cg->tast->nodes[idx];
  TypeRef    var_ty     = cg->resolved_types[n->for_range.begin]; // begin's checked type == loop var's type
  String8    c_ty       = c_type_from_typeref(cg, var_ty);
  u64        mark       = cg_scope_mark(cg);
  String8    var_name   = cg_scope_reserve(cg, n->for_range.var_name);
  fprintf(cg->out, "({ for (%.*s %.*s = ", str8_varg(c_ty), str8_varg(var_name));
  cg_expr(cg, n->for_range.begin); // begin/end/step all see the OUTER scope -- var_name isn't
  fprintf(cg->out, "; %.*s < ", str8_varg(var_name)); // registered until just before the body,
  cg_expr(cg, n->for_range.end);                      // matching the checker's own order (see
  fprintf(cg->out, "; %.*s += ", str8_varg(var_name)); // ForRangeExpr in checker.c)
  cg_expr(cg, n->for_range.step);
  fprintf(cg->out, ") ");
  cg_scope_register(cg, n->for_range.var_name, var_name);
  cg_loop_body_block(cg, n->for_range.body);
  fprintf(cg->out, " (void)0; })");
  cg_scope_pop_to(cg, mark);
}

// The body statements shared by every cg_foreach_expr branch. The other half
// of cg_loop_body_block's loop_scratch_depth chokepoint -- see there.
static void
cg_foreach_body_stmts(Codegen* cg, TypedIndex body_idx) {
  TypedNode* body = &cg->tast->nodes[body_idx];
  xassert(body->kind == TypedNodeKind_Block);
  u32 saved_loop_floor   = cg->loop_scratch_depth;
  cg->loop_scratch_depth = cg->scratch_depth;
  foreach_index(i, body->block.stmt_count) {
    cg_stmt(cg, cg->tast->extra[body->block.stmt_first + i]);
    fprintf(cg->out, "; ");
  }
  cg->loop_scratch_depth = saved_loop_floor;
}

// `(for [item coll] body...)` / `(for [[i item] coll] body...)`. `collection`
// is captured into a temp and evaluated exactly once: it may be an arbitrary
// expression, so re-evaluating it per iteration would repeat side effects.
void
cg_foreach_expr(Codegen* cg, TypedIndex idx) {
  TypedNode* n       = &cg->tast->nodes[idx];
  TypeRef    coll_ty = cg->resolved_types[n->for_each.collection];

  if (coll_ty.kind == TypeKind_Array || coll_ty.kind == TypeKind_Vector) {
    // cg_expr_decay_to handles both kinds uniformly: a real decay for Array,
    // a qualifier-stripping no-op for the already-pointer-shaped Vector.
    u64     mark      = cg_scope_mark(cg);
    String8 elem_c_ty = c_type_from_typeref(cg, *coll_ty.pointee);
    String8 idx_name  = n->for_each.has_index
      ? cg_scope_reserve(cg, n->for_each.index_name)
      : str8_lit("_3b_foreach_i");

    TypeRef ptr_ty = {0};
    ptr_ty.kind    = TypeKind_Pointer;
    ptr_ty.pointee = coll_ty.pointee;

    fprintf(cg->out, "({ %.*s* _3b_foreach_coll = ", str8_varg(elem_c_ty));
    cg_expr_decay_to(cg, n->for_each.collection, ptr_ty);
    if (coll_ty.kind == TypeKind_Array) {
      fprintf(cg->out, "; for (u64 %.*s = 0; %.*s < %lluu; %.*s += 1) { ",
              str8_varg(idx_name), str8_varg(idx_name), (unsigned long long)coll_ty.count, str8_varg(idx_name));
    } else {
      fprintf(cg->out, "; for (u64 %.*s = 0; %.*s < bbb_dyn_count(_3b_foreach_coll); %.*s += 1) { ",
              str8_varg(idx_name), str8_varg(idx_name), str8_varg(idx_name));
    }
    // The index needs no `(void)` when unused: it is the C `for` loop's own
    // control variable here, which the condition and step always read.
    if (n->for_each.has_index) cg_scope_register(cg, n->for_each.index_name, idx_name);

    String8 elem_name = cg_scope_reserve(cg, n->for_each.elem_name);
    fprintf(cg->out, "%.*s %.*s = _3b_foreach_coll[%.*s];", str8_varg(elem_c_ty), str8_varg(elem_name), str8_varg(idx_name));
    cg_mark_used_if_unread(cg, elem_name, n->for_each.elem_is_read);
    fprintf(cg->out, " ");
    cg_scope_register(cg, n->for_each.elem_name, elem_name);

    cg_foreach_body_stmts(cg, n->for_each.body);
    fprintf(cg->out, "} (void)0; })");
    cg_scope_pop_to(cg, mark);
    return;
  }

  xassert((coll_ty.kind == TypeKind_Set || coll_ty.kind == TypeKind_Map)
          && "cg_foreach_expr: unsupported collection kind");

  // Set/Map: walk the monomorphized slot array, skipping empty and tombstone
  // slots. `collection` must be addressable -- the same requirement
  // map-set/map-get/set-add place on their first argument -- so a Map returned
  // by value from a call is not supported here.
  //
  // `_3b_foreach_i` is the internal slot index, always u64 and never
  // user-visible. The declared bindings are separate locals copied from the
  // slot: Set gives elem_name = key and index_name = the raw position, Map
  // gives index_name = key and elem_name = value. Set slots have no `.value`.
  u64     mark = cg_scope_mark(cg);
  String8 c_ty = c_type_from_typeref(cg, coll_ty);

  fprintf(cg->out, "({ %.*s* _3b_foreach_ht = &(", str8_varg(c_ty));
  cg_expr(cg, n->for_each.collection);
  fprintf(cg->out,
    "); for (u64 _3b_foreach_i = 0; _3b_foreach_i < _3b_foreach_ht->capacity; _3b_foreach_i += 1) { "
    "if (_3b_foreach_ht->slots[_3b_foreach_i].state == bbb_HashSlotState_Occupied) { ");

  // Unlike the Array/Vector loop above, both of these are locals copied out of
  // the slot rather than the loop's own control variable (that is
  // `_3b_foreach_i`), so either can genuinely go unread.
  if (n->for_each.has_index) {
    String8 index_name = cg_scope_reserve(cg, n->for_each.index_name);
    if (coll_ty.kind == TypeKind_Set) {
      fprintf(cg->out, "u64 %.*s = _3b_foreach_i;", str8_varg(index_name));
    } else {
      String8 key_c_ty = c_type_from_typeref(cg, *coll_ty.map_key);
      fprintf(cg->out, "%.*s %.*s = _3b_foreach_ht->slots[_3b_foreach_i].key;", str8_varg(key_c_ty), str8_varg(index_name));
    }
    cg_mark_used_if_unread(cg, index_name, n->for_each.index_is_read);
    fprintf(cg->out, " ");
    cg_scope_register(cg, n->for_each.index_name, index_name);
  }

  String8 elem_name = cg_scope_reserve(cg, n->for_each.elem_name);
  String8 elem_c_ty = c_type_from_typeref(cg, *coll_ty.pointee);
  if (coll_ty.kind == TypeKind_Set) {
    fprintf(cg->out, "%.*s %.*s = _3b_foreach_ht->slots[_3b_foreach_i].key;", str8_varg(elem_c_ty), str8_varg(elem_name));
  } else {
    fprintf(cg->out, "%.*s %.*s = _3b_foreach_ht->slots[_3b_foreach_i].value;", str8_varg(elem_c_ty), str8_varg(elem_name));
  }
  cg_mark_used_if_unread(cg, elem_name, n->for_each.elem_is_read);
  fprintf(cg->out, " ");
  cg_scope_register(cg, n->for_each.elem_name, elem_name);

  cg_foreach_body_stmts(cg, n->for_each.body);
  fprintf(cg->out, "} } (void)0; })");
  cg_scope_pop_to(cg, mark);
}

// `(parallel-for [i n] body...)`, valid only inside a `parallel` block
// (checker-enforced). `lane_range` partitions `[0, n)`, so unlike
// cg_for_range_expr there is no begin or step to emit -- one `lane_range` call
// and an ascending loop over its `.min`/`.max`. `__pr` needs no per-occurrence
// uniqueness: each of these is its own `({ })`, hence its own C block scope,
// so even directly nested ones just shadow.
void
cg_parallel_for_expr(Codegen* cg, TypedIndex idx) {
  TypedNode* n       = &cg->tast->nodes[idx];
  TypeRef    var_ty   = cg->resolved_types[n->parallel_for.count]; // count's checked type == loop var's type
  String8    c_ty     = c_type_from_typeref(cg, var_ty);
  u64        mark     = cg_scope_mark(cg);
  String8    var_name = cg_scope_reserve(cg, n->parallel_for.var_name);

  fprintf(cg->out, "({ bbb_Rng1u64 __pr = bbb_lane_range((u64)(");
  cg_expr(cg, n->parallel_for.count);
  fprintf(cg->out, ")); for (%.*s %.*s = (%.*s)__pr.min; %.*s < (%.*s)__pr.max; %.*s += 1) ",
          str8_varg(c_ty), str8_varg(var_name), str8_varg(c_ty),
          str8_varg(var_name), str8_varg(c_ty), str8_varg(var_name));
  cg_scope_register(cg, n->parallel_for.var_name, var_name);
  cg_loop_body_block(cg, n->parallel_for.body);
  fprintf(cg->out, " (void)0; })");
  cg_scope_pop_to(cg, mark);
}

// `(parallel [name init ...] body...)` compiles to two pieces -- see
// cg_function for why hoisting is needed at all:
//
//  1. A capture-struct typedef and a `static void(void)` trampoline, written
//     to cg->parallel_prelude_out, which cg_function flushes just before the
//     enclosing fn's signature. The runtime's async_run_phase calls the
//     trampoline on every lane thread. Captures are reached as
//     `__cap-><field>`, registered into a fresh scope -- cg->scope is swapped
//     to NULL for the duration -- so the body can only see explicit captures
//     and its own bindings, never an enclosing local that does not exist on
//     another thread.
//  2. A fork/join statement expression at the original site: build the capture
//     struct, evaluating each init once in the caller's scope like a call
//     argument, point the shared g_3b_lane_job slot at it, run the phase, wait.
//
// Both pieces are named from `idx`, which is stable and unique by
// construction, so they need no shared counter and no discovery walk: whatever
// order the enclosing function's ordinary walk reaches `parallel` blocks in is
// the order they hoist in.
void
cg_parallel_expr(Codegen* cg, TypedIndex idx) {
  TypedNode* n = &cg->tast->nodes[idx];
  xassert(cg->parallel_prelude_out
          && "cg_parallel_expr fired without a live parallel_prelude_out -- "
             "cg_function should have set Codegen.has_parallel and wrapped this fn's emission");

  u32 capture_first = n->parallel_expr.capture_first;
  u16 capture_count = n->parallel_expr.capture_count;
  u32 id            = (u32)idx;

  // -- 1a. Capture-struct typedef. Fields are positional (`f0`, `f1`, ...)
  // rather than the user's capture names, so two captures sharing a source
  // name, or one colliding with a C keyword, cannot produce an invalid member.
  fprintf(cg->parallel_prelude_out, "typedef struct __3b_parallel_captures_%u {\n", id);
  if (capture_count == 0) {
    fprintf(cg->parallel_prelude_out, "  char __3b_unused; // a struct can't be empty in standard C\n");
  } else {
    foreach_index(i, capture_count) {
      Binding* b    = &cg->tast->bindings[capture_first + i];
      String8  c_ty = c_type_from_typeref(cg, b->type);
      fprintf(cg->parallel_prelude_out, "  %.*s f%u;\n", str8_varg(c_ty), (u32)i);
    }
  }
  fprintf(cg->parallel_prelude_out, "} __3b_parallel_captures_%u;\n\n", id);

  // -- 1b. Trampoline function --
  fprintf(cg->parallel_prelude_out,
          "static void\n__3b_parallel_fn_%u(void) {\n"
          "  __3b_parallel_captures_%u* __cap = (__3b_parallel_captures_%u*)bbb_g_3b_lane_job;\n"
          "  (void)__cap;\n",
          id, id, id);

  FILE*         saved_out           = cg->out;
  CgScopeEntry* saved_scope         = cg->scope;
  u32           saved_scratch_depth = cg->scratch_depth;
  cg->out                           = cg->parallel_prelude_out;
  cg->scope                         = NULL; // fresh scope -- see this fn's comment
  cg->scratch_depth                 = 0;    // and the enclosing fn's `_3b_scratch_temp_N`s are
                                            // likewise not in scope in the trampoline
  foreach_index(i, capture_count) {
    Binding* b         = &cg->tast->bindings[capture_first + i];
    String8  field_ref = str8f(ctx_perm(), "__cap->f%u", (u32)i);
    cg_scope_register(cg, b->name, field_ref);
  }
  TypedNode* body = &cg->tast->nodes[n->parallel_expr.body];
  xassert(body->kind == TypedNodeKind_Block);
  foreach_index(i, body->block.stmt_count) {
    cg_stmt(cg, cg->tast->extra[body->block.stmt_first + i]);
    fprintf(cg->out, "; ");
  }
  cg->scope         = saved_scope;
  cg->out           = saved_out;
  cg->scratch_depth = saved_scratch_depth;

  fprintf(cg->parallel_prelude_out, "}\n\n");

  // -- 2. Call site --
  fprintf(cg->out, "({ __3b_parallel_captures_%u __cap_%u = { ", id, id);
  if (capture_count == 0) {
    fprintf(cg->out, "0");
  } else {
    foreach_index(i, capture_count) {
      Binding* b = &cg->tast->bindings[capture_first + i];
      if (i != 0) fprintf(cg->out, ", ");
      fprintf(cg->out, ".f%u = ", (u32)i);
      cg_expr(cg, b->init);
    }
  }
  fprintf(cg->out, " };\n");
  fprintf(cg->out, "  bbb_g_3b_lane_job = &__cap_%u;\n", id);
  fprintf(cg->out, "  u64 __gen_%u = bbb_async_run_phase(__3b_parallel_fn_%u);\n", id, id);
  fprintf(cg->out, "  bbb_async_phase_wait(__gen_%u);\n", id);
  fprintf(cg->out, "  (void)0; })");
}

// The printf specifier for one `print`/`str` value, from its static type
// (already validated by type_ref_is_printable). A lookup with no emission, so
// callers can finish the format string before emitting arguments -- C wants
// the format textually first.
// `tok` is only read on the impossible path below, to place its diagnostic.
static String8
cg_print_specifier(TypeRef t, Token tok) {
  // `char*` is a nul-terminated C string, so bare `%s` reads it directly.
  // type_ref_is_printable already rejected every other pointer shape.
  if (t.kind == TypeKind_Pointer) return str8_lit("%s");
  switch (t.kind) {
    case TypeKind_I8:  case TypeKind_I16: case TypeKind_I32: return str8_lit("%d");
    case TypeKind_U8:  case TypeKind_U16: case TypeKind_U32: return str8_lit("%u");
    case TypeKind_I64:                                       return str8_lit("%lld");
    case TypeKind_U64:                                       return str8_lit("%llu");
    case TypeKind_F32: case TypeKind_F64:                    return str8_lit("%g");
    case TypeKind_Bool:                                      return str8_lit("%s");
    case TypeKind_Char:                                      return str8_lit("%c");
    // Explicit length, never bare `%s`: a `string` is a counted String8 with
    // no nul guarantee. base.h's zero-copy views (str8_range, str8_substr,
    // str8_skip, str8_chop) all return a String8 with no nul at `.size`, and
    // `%s` would read past it to whatever stray zero byte came next.
    case TypeKind_String:                                    return str8_lit("%.*s");
    // type_ref_is_printable already rejected everything else, so this is a
    // disagreement between that predicate and this switch. Returning an empty
    // specifier silently drops the value from the output while its argument
    // stays in the printf call, shifting every later `%` onto the wrong one.
    default:
      cg_internal_error(tok, "no printf specifier for TypeKind %d, which the checker accepted "
                              "as printable", (int)t.kind);
      return str8_lit("");
  }
}

// ~~ ARGUMENT EVALUATION ORDER for print/println/str. This is the language
// rule, not a codegen convenience, so it is stated once here and honoured by
// bcgen.c's bytecode backend too (see its own `print` case, which points
// back at this comment).
//
// 3b GUARANTEES that these builtins evaluate every value argument
// LEFT-TO-RIGHT, and all of them BEFORE any output is produced. C promises
// neither for a printf call: the order among printf's arguments is
// unspecified, and gcc in fact evaluates them right-to-left. Neither
// backend can be left to inherit that, because a print argument can itself
// print -- `(println "a={} b={}" (f) (g))` where both f and g print -- and
// then "unspecified" is directly visible in the output, and DIFFERENT on
// each backend. So each value argument is hoisted into its own C binding
// first: declarations are sequenced, which pins the order down, and the
// printf call that follows sees only already-computed values.
//
// Hoisting is also what makes `%.*s` correct at all -- it needs two C
// arguments per `string` value, its `.size` and its `.str`, and re-emitting
// the argument expression for each would evaluate it twice.
//
// The bindings are named by VALUE-ARGUMENT POSITION rather than a global
// counter, which is safe because each hoisting site opens its own C block
// scope: nested calls like `(print "{}" (str a ...))` shadow rather than
// collide.
//
// `lead` covers the one argument that comes BEFORE the template/values in
// source and so must be evaluated first, even though C wants it textually
// inside the call. `first_value_extra` is the tast->extra index of value
// argument 0 -- always the argument just past the template or the arena.
//
// Returns whether anything was emitted. If so the caller must close the
// `({ ...; call(...); })`, since this emits only the opening `({ ` and the
// bindings.
typedef enum CgPrintLead {
  CgPrintLead_None,      // `(print "..." v...)` / `(println "..." v...)`
  CgPrintLead_Stream,    // `(print s "..." v...)` -- hoisted BY VALUE; `stream` is a
                         // typedef'd pointer that bbb_stream_printf takes by value too
  CgPrintLead_ArenaAddr, // `(str a v...)` -- hoisted AS ITS ADDRESS. bbb_str8f wants
                         // an `Arena*` and BUMPS it, so a by-value copy would allocate
                         // out of a temporary and leave the caller's own arena untouched.
} CgPrintLead;

static b32
cg_print_hoist_args(Codegen* cg, CgPrintLead lead, TypedIndex lead_idx,
                     u32 first_value_extra, u16 value_count) {
  if (lead == CgPrintLead_None && value_count == 0) return false;
  fprintf(cg->out, "({ ");
  if (lead != CgPrintLead_None) {
    String8 c_type = c_type_from_typeref(cg, cg->resolved_types[lead_idx]);
    b32     addr   = (lead == CgPrintLead_ArenaAddr);
    fprintf(cg->out, "%.*s%s __3b_plead = %s(", str8_varg(c_type), addr ? "*" : "", addr ? "&" : "");
    cg_expr(cg, lead_idx);
    fprintf(cg->out, "); ");
  }
  foreach_index(i, value_count) {
    TypedIndex arg_idx = cg->tast->extra[first_value_extra + i];
    // Every printable type (type_ref_is_printable: the primitives, `string`,
    // and `char*`) is declarable as a plain C local under its own name -- none
    // of the array/function shapes that would need cg_declare's name-in-the-
    // middle spelling can reach here.
    String8    c_type  = c_type_from_typeref(cg, cg->resolved_types[arg_idx]);
    fprintf(cg->out, "%.*s __3b_pval%llu = (", str8_varg(c_type), (unsigned long long)i);
    cg_expr(cg, arg_idx);
    fprintf(cg->out, "); ");
  }
  return true;
}

// The C argument expression matching cg_print_specifier's choice for the same
// type, reading the binding cg_print_hoist_args made for value argument
// `position`. Integers are cast to the specifier's exact width rather than
// leaning on default argument promotion, bool becomes a `"true"`/`"false"`
// ternary since printf has no bool specifier, and string spreads into the two
// arguments `%.*s` wants.
static void
cg_print_wrapped_arg(Codegen* cg, TypedIndex arg_idx, u64 position) {
  TypeRef            ty = cg->resolved_types[arg_idx];
  unsigned long long p  = (unsigned long long)position;
  if (ty.kind == TypeKind_Pointer) {
    fprintf(cg->out, "__3b_pval%llu", p); // char*: already what `%s` wants, no cast
    return;
  }
  switch (ty.kind) {
    case TypeKind_I8: case TypeKind_I16: case TypeKind_I32:
      fprintf(cg->out, "(int)__3b_pval%llu", p);
      break;
    case TypeKind_U8: case TypeKind_U16: case TypeKind_U32:
      fprintf(cg->out, "(unsigned int)__3b_pval%llu", p);
      break;
    case TypeKind_I64:
      fprintf(cg->out, "(long long)__3b_pval%llu", p);
      break;
    case TypeKind_U64:
      fprintf(cg->out, "(unsigned long long)__3b_pval%llu", p);
      break;
    case TypeKind_F32: case TypeKind_F64:
      fprintf(cg->out, "(double)__3b_pval%llu", p);
      break;
    case TypeKind_Bool:
      fprintf(cg->out, "(__3b_pval%llu ? \"true\" : \"false\")", p);
      break;
    case TypeKind_Char:
      fprintf(cg->out, "__3b_pval%llu", p);
      break;
    case TypeKind_String:
      fprintf(cg->out, "(int)__3b_pval%llu.size, (char*)__3b_pval%llu.str", p, p);
      break;
    // The mirror of cg_print_specifier's own default, and worse: that one
    // emits a format string with a `%` and no argument behind it, this one
    // emits nothing where printf is expecting a value.
    default:
      cg_internal_error(cg->tast->nodes[arg_idx].token,
                        "no printf argument form for TypeKind %d, which the checker accepted "
                        "as printable", (int)ty.kind);
      break;
  }
}

// ~~ File-scope-initializer call inlining.
//
// C requires a constant expression to initialize an object with static storage
// duration, and a function call never qualifies -- so a top-level
// `(val fov f32 (radians 45.0))` fails in GCC with "initializer element is not
// constant", even though `(val x f32 (/ tau 8.0))` compiles: GCC accepts
// arithmetic against another const global as an extension, but never a call.
//
// Rather than let `val` initialize at runtime, which would change what a `val`
// means everywhere else, the call is never emitted: cg_call_is_const_foldable
// decides whether a site can be inlined, and cg_emit_folded_call substitutes
// the callee's single return-expression with this call's arguments in place of
// its params. Both are reached only from cg_call's ordinary-call fallback,
// gated on Codegen.in_static_init.
//
// A non-foldable call (multi-statement body, extern, builtin, a reference to a
// non-const global) falls through to plain `name(args)`, which GCC then rejects
// as a non-constant initializer. Diagnosing that in 3b terms is unhandled.
static b32 cg_call_is_const_foldable(Codegen* cg, TypedIndex call_idx);

// True iff a `reinterpret` can come out as a plain C cast rather than the
// memcpy in cg_expr's BinaryReinterpret case: both sides plain integers, where
// checker.c has already established equal width and a same-width integer
// conversion is bit-preserving. `char` counts, being an integer type in C.
//
// Deliberately narrow. Floats are the case reinterpret exists for and a cast
// would convert their value instead of their bits; pointers and `any` are
// bit-preserving in practice but their casts are not constant expressions at
// file scope, which is half the point of taking this path; and a Named type
// here is an enum, whose C width follows the compiler's enum choice rather
// than the TypeRef. Each of those keeps the memcpy.
static b32
cg_reinterpret_is_plain_cast(TypeRef dst, TypeRef src) {
  TypeKind kinds[2] = { dst.kind, src.kind };
  foreach_index(i, 2) {
    switch (kinds[i]) {
      case TypeKind_I8:  case TypeKind_I16: case TypeKind_I32: case TypeKind_I64:
      case TypeKind_U8:  case TypeKind_U16: case TypeKind_U32: case TypeKind_U64:
      case TypeKind_Char:
        break;
      default:
        return false;
    }
  }
  return true;
}

// Recursively true iff `idx` is built only from pieces that stay legal in a
// file-scope initializer once any nested foldable call is inlined away:
// literals, identifiers, a cast's value operand (`binary.lhs` holds the type
// name, never a value), arithmetic/bitwise/comparison operators, and other
// foldable calls. An identifier is either a param of the enclosing foldable
// call, due for substitution, or another top-level global, whose own constness
// is not re-checked here -- hence the non-const `var` gap noted above.
// Everything else (field access, indexing, string ops, struct literals) is
// conservatively rejected.
static b32
cg_expr_is_const_foldable(Codegen* cg, TypedIndex idx) {
  if (idx == TYPED_NIL) return false;
  TypedNode* n = &cg->tast->nodes[idx];
  switch (n->kind) {
    case TypedNodeKind_IntLiteral:
    case TypedNodeKind_FloatLiteral:
    case TypedNodeKind_BoolLiteral:
    case TypedNodeKind_Identifier:
      return true;
    case TypedNodeKind_UnaryNeg:
    case TypedNodeKind_UnaryPos:
    case TypedNodeKind_UnaryBitNot:
      return cg_expr_is_const_foldable(cg, n->unary.expr);
    case TypedNodeKind_BinaryAdd:
    case TypedNodeKind_BinarySub:
    case TypedNodeKind_BinaryMul:
    case TypedNodeKind_BinaryDiv:
    case TypedNodeKind_BinaryMod:
    case TypedNodeKind_BinaryBitOr:
    case TypedNodeKind_BinaryBitAnd:
    case TypedNodeKind_BinaryBitXor:
    case TypedNodeKind_BinaryShl:
    case TypedNodeKind_BinaryShr:
    case TypedNodeKind_BinaryEq:
    case TypedNodeKind_BinaryNeq:
    case TypedNodeKind_BinaryLt:
    case TypedNodeKind_BinaryLe:
    case TypedNodeKind_BinaryGt:
    case TypedNodeKind_BinaryGe:
      return cg_expr_is_const_foldable(cg, n->binary.lhs) && cg_expr_is_const_foldable(cg, n->binary.rhs);
    case TypedNodeKind_BinaryCast:
      return cg_expr_is_const_foldable(cg, n->binary.rhs);
    case TypedNodeKind_BinaryReinterpret:
      // Only in its plain-cast form; the memcpy form is a statement expression,
      // which no file-scope initializer can hold however it is nested.
      return cg_reinterpret_is_plain_cast(cg->resolved_types[idx], cg->resolved_types[n->binary.rhs])
             && cg_expr_is_const_foldable(cg, n->binary.rhs);
    case TypedNodeKind_Call:
      return cg_call_is_const_foldable(cg, idx);
    default:
      return false;
  }
}

// True iff `call_idx` names a top-level 3b function -- not a local fn pointer
// (no params to substitute into) and not `extern` (no body) -- whose entire
// body is one statement, a return-expression or an implicit-return last
// statement, built from foldable pieces; and every argument at this site is
// independently foldable, since a non-foldable argument cannot go into a
// file-scope initializer either.
static b32
cg_call_is_const_foldable(Codegen* cg, TypedIndex call_idx) {
  TypedNode* call = &cg->tast->nodes[call_idx];
  if (cg_scope_lookup(cg, call->call.callee)) return false; // local fn-pointer -- nothing to inline
  FnEntry* callee_fn = (FnEntry*)hashtable_lookup(&cg->fns_by_name, call->call.callee);
  if (!callee_fn) return false; // a builtin -- cg_call's earlier string-matched
                                  // cases handle those before this fallback
  TypedNode* callee_decl = &cg->tast->nodes[callee_fn->decl];
  if (callee_decl->func.body == TYPED_NIL) return false; // extern -- no body to inline
  TypedNode* body = &cg->tast->nodes[callee_decl->func.body];
  if (body->kind != TypedNodeKind_Block || body->block.stmt_count != 1) return false;
  TypedIndex stmt   = cg->tast->extra[body->block.stmt_first + 0];
  TypedNode* stmt_n = &cg->tast->nodes[stmt];
  TypedIndex ret_expr = stmt_n->kind == TypedNodeKind_ReturnExpr ? stmt_n->unary.expr : stmt;
  if (!cg_expr_is_const_foldable(cg, ret_expr)) return false;
  foreach_index(i, call->call.arg_count) {
    if (!cg_expr_is_const_foldable(cg, cg->tast->extra[call->call.arg_first + i])) return false;
  }
  return true;
}

// Emits `call_idx`, already confirmed foldable, as a parenthesized inline copy
// of the callee's single return-expression, with each param reference swapped
// for that argument's expression via Codegen.const_inline_subs. Substitutions
// are pushed here and popped before returning, so a sibling call later in the
// same initializer starts clean; a nested foldable call pushes its own params
// on top during the inner cg_expr and unwinds them the same way.
static void
cg_emit_folded_call(Codegen* cg, TypedIndex call_idx) {
  TypedNode* call        = &cg->tast->nodes[call_idx];
  FnEntry*   callee_fn   = (FnEntry*)hashtable_lookup(&cg->fns_by_name, call->call.callee);
  TypedNode* callee_decl = &cg->tast->nodes[callee_fn->decl];
  TypedNode* body        = &cg->tast->nodes[callee_decl->func.body];
  TypedIndex stmt        = cg->tast->extra[body->block.stmt_first + 0];
  TypedNode* stmt_n      = &cg->tast->nodes[stmt];
  TypedIndex ret_expr    = stmt_n->kind == TypedNodeKind_ReturnExpr ? stmt_n->unary.expr : stmt;

  u32 old_count = cg->const_inline_sub_count;
  foreach_index(i, call->call.arg_count) {
    if (cg->const_inline_sub_count >= ArrayCount(cg->const_inline_subs)) break; // see that field's comment
    Param* p = &cg->tast->params[callee_decl->func.param_first + i];
    cg->const_inline_subs[cg->const_inline_sub_count].name = p->name;
    cg->const_inline_subs[cg->const_inline_sub_count].expr = cg->tast->extra[call->call.arg_first + i];
    cg->const_inline_sub_count += 1;
  }
  fprintf(cg->out, "(");
  cg_expr(cg, ret_expr);
  fprintf(cg->out, ")");
  cg->const_inline_sub_count = old_count;
}

void
cg_call(Codegen* cg, TypedIndex idx) {
  TypedNode* n = &cg->tast->nodes[idx];
  if (str8_match_lit("print", n->call.callee, 0) || str8_match_lit("println", n->call.callee, 0)) {
    // The checker already validated that the template is a string literal with
    // as many `{}` placeholders as trailing arguments, each a printable
    // primitive. This becomes one printf call: walk the template synthesizing
    // an escaped C format string -- literal `%` doubled so user text is never
    // read as a spec, `{{`/`}}` unescaped to a brace, each `{}` replaced by the
    // matching argument's specifier -- and only then emit the arguments, since
    // C wants the format textually first but choosing specifiers means walking
    // the whole template. `println` appends a `\n` to that format string.
    //
    // The value arguments are evaluated into bindings BEFORE that call, in
    // source order -- see cg_print_hoist_args for the language rule that
    // pins down, and why C's own unspecified argument order can't be inherited.
    //
    // A leading `stream` argument changes only the callee and adds one leading
    // C argument; the template walk, specifier choice and argument hoisting are
    // identical, which is why formatted file output routes through `print`
    // rather than a separate builtin.
    b32     is_println         = str8_match_lit("println", n->call.callee, 0);
    u32     tmpl_arg           = (n->call.arg_count > 0
                                  && cg->resolved_types[cg->tast->extra[n->call.arg_first + 0]].kind == TypeKind_Stream)
                                 ? 1 : 0;
    String8 tmpl               = cg->tast->nodes[cg->tast->extra[n->call.arg_first + tmpl_arg]].string_lit.value;
    u32     first_value_extra  = n->call.arg_first + tmpl_arg + 1;
    u16     value_count        = (u16)(n->call.arg_count - 1 - tmpl_arg);
    b32     hoisted            = cg_print_hoist_args(cg,
                                                     tmpl_arg == 1 ? CgPrintLead_Stream : CgPrintLead_None,
                                                     cg->tast->extra[n->call.arg_first + 0],
                                                     first_value_extra, value_count);
    if (tmpl_arg == 1) {
      fprintf(cg->out, "bbb_stream_printf(__3b_plead, \"");
    } else {
      fprintf(cg->out, "printf(\"");
    }
    u32 arg_i = tmpl_arg + 1;
    for (u64 i = 0; i < tmpl.size; i += 1) {
      u8 c = tmpl.str[i];
      if (c == '{' && i + 1 < tmpl.size && tmpl.str[i + 1] == '{') {
        fputc('{', cg->out);
        i += 1;
      } else if (c == '}' && i + 1 < tmpl.size && tmpl.str[i + 1] == '}') {
        fputc('}', cg->out);
        i += 1;
      } else if (c == '{') {
        TypedIndex arg_idx = cg->tast->extra[n->call.arg_first + arg_i];
        String8    spec    = cg_print_specifier(cg->resolved_types[arg_idx], cg->tast->nodes[arg_idx].token);
        fprintf(cg->out, "%.*s", str8_varg(spec));
        arg_i += 1;
        i += 1; // skip the matching `}` -- checker already guaranteed it's there
      } else if (c == '%') {
        fprintf(cg->out, "%%%%");
      } else {
        switch (c) {
          case '\\': fprintf(cg->out, "\\\\"); break;
          case '"':  fprintf(cg->out, "\\\""); break;
          case '\n': fprintf(cg->out, "\\n");  break;
          case '\t': fprintf(cg->out, "\\t");  break;
          case '\r': fprintf(cg->out, "\\r");  break;
          default:   fputc(c, cg->out);        break;
        }
      }
    }
    if (is_println) fprintf(cg->out, "\\n");
    fprintf(cg->out, "\"");
    foreach_index(i, value_count) {
      fprintf(cg->out, ", ");
      cg_print_wrapped_arg(cg, cg->tast->extra[first_value_extra + i], i);
    }
    fprintf(cg->out, ")");
    if (hoisted) fprintf(cg->out, "; })");
    return;
  }
  if (str8_match_lit("str", n->call.callee, 0)) {
    // `(str arena v1 v2 ...)`, Clojure's `str`: no template, just each value's
    // specifier back to back with no separator, fed to base.h's str8f, which
    // allocates the result into `arena`. `arena` is by value at the language
    // level but str8f wants `Arena*`, hence CgPrintLead_ArenaAddr hoisting
    // its ADDRESS -- which is also what puts it ahead of the values, the order
    // it has in the source.
    u32 first_value_extra = n->call.arg_first + 1;
    u16 value_count       = (u16)(n->call.arg_count - 1);
    b32 hoisted           = cg_print_hoist_args(cg, CgPrintLead_ArenaAddr,
                                                cg->tast->extra[n->call.arg_first + 0],
                                                first_value_extra, value_count);
    fprintf(cg->out, "bbb_str8f(__3b_plead, \"");
    foreach_index(i, value_count) {
      TypedIndex arg_idx = cg->tast->extra[first_value_extra + i];
      String8    spec    = cg_print_specifier(cg->resolved_types[arg_idx], cg->tast->nodes[arg_idx].token);
      fprintf(cg->out, "%.*s", str8_varg(spec));
    }
    fprintf(cg->out, "\"");
    foreach_index(i, value_count) {
      fprintf(cg->out, ", ");
      cg_print_wrapped_arg(cg, cg->tast->extra[first_value_extra + i], i);
    }
    fprintf(cg->out, ")");
    if (hoisted) fprintf(cg->out, "; })");
    return;
  }
  // `(create)` / `(create reserve-size)` -- VM-backed arena, by value. The
  // default reserves 64 MB of address space without committing it; a VM arena
  // only pays for pages it touches (arena_vm_push_op).
  if (str8_match_lit("create", n->call.callee, 0)) {
    fprintf(cg->out, "bbb_arena_create_vm(");
    if (n->call.arg_count == 1) {
      cg_expr(cg, cg->tast->extra[n->call.arg_first + 0]);
    } else {
      fprintf(cg->out, "bbb_MB(64)");
    }
    fprintf(cg->out, ")");
    return;
  }
  // `(destroy a)` / `(reset a)` / `(release a)` -- each base.h op takes
  // `Arena*` and `a` is a by-value `arena`, so take its address.
  if (str8_match_lit("destroy", n->call.callee, 0) && n->call.arg_count == 1) {
    fprintf(cg->out, "bbb_arena_destroy(&(");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 0]);
    fprintf(cg->out, "))");
    return;
  }
  if (str8_match_lit("reset", n->call.callee, 0) && n->call.arg_count == 1) {
    fprintf(cg->out, "bbb_arena_reset(&(");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 0]);
    fprintf(cg->out, "))");
    return;
  }
  if (str8_match_lit("release", n->call.callee, 0) && n->call.arg_count == 1) {
    fprintf(cg->out, "bbb_arena_release(&(");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 0]);
    fprintf(cg->out, "))");
    return;
  }
  // `(mark a)` -- read-only, but arena_mark still takes `Arena*`.
  if (str8_match_lit("mark", n->call.callee, 0) && n->call.arg_count == 1) {
    fprintf(cg->out, "bbb_arena_mark(&(");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 0]);
    fprintf(cg->out, "))");
    return;
  }
  // `(pop a m)` -- rewinds `a` to `m`. arena_pop takes the mark by value, so
  // only `a` needs the `&`.
  if (str8_match_lit("pop", n->call.callee, 0) && n->call.arg_count == 2) {
    fprintf(cg->out, "bbb_arena_pop(&(");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 0]);
    fprintf(cg->out, "), ");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 1]);
    fprintf(cg->out, ")");
    return;
  }
  if (str8_match_lit("string-match", n->call.callee, 0) && n->call.arg_count == 3) {
    fprintf(cg->out, "bbb_string_match(");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 0]);
    fprintf(cg->out, ", ");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 1]);
    fprintf(cg->out, ", ");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 2]);
    fprintf(cg->out, ")");
    return;
  }
  // `(free p)` -- the other half of `alloc`.
  if (str8_match_lit("free", n->call.callee, 0) && n->call.arg_count == 1) {
    fprintf(cg->out, "free(");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 0]);
    fprintf(cg->out, ")");
    return;
  }
  // `(handle-deref h)` -- `Prefix_pool_get(pool, h)`; NULL if h is
  // stale/out-of-range/zero (base.h's own checked-nilable convention).
  if (str8_match_lit("handle-deref", n->call.callee, 0) && n->call.arg_count == 1) {
    TypedIndex arg_idx     = cg->tast->extra[n->call.arg_first + 0];
    String8    struct_name = cg->resolved_types[arg_idx].name; // TypeKind_Handle's backing struct name
    String8    c_name      = cg_symbol_name(cg, struct_name);
    fprintf(cg->out, "%.*s_pool_get(&%.*s, ", str8_varg(c_name), str8_varg(cg_handle_pool_global_name(cg, struct_name)));
    cg_expr(cg, arg_idx);
    fprintf(cg->out, ")");
    return;
  }
  // `(handle-free h)` -- `Prefix_pool_free(pool, h)`.
  if (str8_match_lit("handle-free", n->call.callee, 0) && n->call.arg_count == 1) {
    TypedIndex arg_idx     = cg->tast->extra[n->call.arg_first + 0];
    String8    struct_name = cg->resolved_types[arg_idx].name;
    String8    c_name      = cg_symbol_name(cg, struct_name);
    fprintf(cg->out, "%.*s_pool_free(&%.*s, ", str8_varg(c_name), str8_varg(cg_handle_pool_global_name(cg, struct_name)));
    cg_expr(cg, arg_idx);
    fprintf(cg->out, ")");
    return;
  }
  // `(handle-valid? h)` -- `Prefix_pool_get(pool, h) != NULL`, the same
  // index-range and generation check handle-deref performs. Not base.h's
  // `Prefix_handle_valid`, which never sees the pool and so only reports
  // whether the handle is non-zero: that stays true for a stale handle whose
  // slot has since been freed.
  if (str8_match_lit("handle-valid?", n->call.callee, 0) && n->call.arg_count == 1) {
    TypedIndex arg_idx     = cg->tast->extra[n->call.arg_first + 0];
    String8    struct_name = cg->resolved_types[arg_idx].name; // TypeKind_Handle's backing struct name
    String8    c_name      = cg_symbol_name(cg, struct_name);
    fprintf(cg->out, "(%.*s_pool_get(&%.*s, ", str8_varg(c_name), str8_varg(cg_handle_pool_global_name(cg, struct_name)));
    cg_expr(cg, arg_idx);
    fprintf(cg->out, ") != NULL)");
    return;
  }
  // `(align-pow2 x b)` / `(align-down-pow2 x b)` / `(align-pad-pow2 x b)`
  // -- base.h's AlignPow2/AlignDownPow2/AlignPadPow2 macros.
  if (str8_match_lit("align-pow2", n->call.callee, 0) && n->call.arg_count == 2) {
    fprintf(cg->out, "bbb_AlignPow2(");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 0]);
    fprintf(cg->out, ", ");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 1]);
    fprintf(cg->out, ")");
    return;
  }
  if (str8_match_lit("align-down-pow2", n->call.callee, 0) && n->call.arg_count == 2) {
    fprintf(cg->out, "bbb_AlignDownPow2(");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 0]);
    fprintf(cg->out, ", ");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 1]);
    fprintf(cg->out, ")");
    return;
  }
  if (str8_match_lit("align-pad-pow2", n->call.callee, 0) && n->call.arg_count == 2) {
    fprintf(cg->out, "bbb_AlignPadPow2(");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 0]);
    fprintf(cg->out, ", ");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 1]);
    fprintf(cg->out, ")");
    return;
  }
  // `(swap a b)` -- base.h's `Swap(T, a, b)` is a `do {} while (0)` statement
  // macro, so it self-wraps in `({ ...; (void)0; })` like cg_dyn_push. The
  // checker guarantees both operands are same-typed lvalues, which emit as
  // valid C lvalues already; `T` comes from their shared resolved type.
  if (str8_match_lit("swap", n->call.callee, 0) && n->call.arg_count == 2) {
    TypedIndex a_idx = cg->tast->extra[n->call.arg_first + 0];
    fprintf(cg->out, "({ bbb_Swap(%.*s, ", str8_varg(c_type_from_typeref(cg, cg->resolved_types[a_idx])));
    cg_expr(cg, a_idx);
    fprintf(cg->out, ", ");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 1]);
    fprintf(cg->out, "); (void)0; })");
    return;
  }
  // `(min a b)` / `(max a b)` -- base.h's Min/Max macros.
  if (str8_match_lit("min", n->call.callee, 0) && n->call.arg_count == 2) {
    fprintf(cg->out, "bbb_Min(");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 0]);
    fprintf(cg->out, ", ");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 1]);
    fprintf(cg->out, ")");
    return;
  }
  if (str8_match_lit("max", n->call.callee, 0) && n->call.arg_count == 2) {
    fprintf(cg->out, "bbb_Max(");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 0]);
    fprintf(cg->out, ", ");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 1]);
    fprintf(cg->out, ")");
    return;
  }
  // `(clamp x lo hi)` -- base.h's Clamp(A, X, B) takes the value in the
  // middle, so the arguments are reordered to (lo, x, hi).
  if (str8_match_lit("clamp", n->call.callee, 0) && n->call.arg_count == 3) {
    fprintf(cg->out, "bbb_Clamp(");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 1]);
    fprintf(cg->out, ", ");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 0]);
    fprintf(cg->out, ", ");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 2]);
    fprintf(cg->out, ")");
    return;
  }
  // `(clamp-top x top)` / `(clamp-bot x bot)` -- base.h's single-bound
  // ClampTop/ClampBot.
  if (str8_match_lit("clamp-top", n->call.callee, 0) && n->call.arg_count == 2) {
    fprintf(cg->out, "bbb_ClampTop(");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 0]);
    fprintf(cg->out, ", ");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 1]);
    fprintf(cg->out, ")");
    return;
  }
  if (str8_match_lit("clamp-bot", n->call.callee, 0) && n->call.arg_count == 2) {
    fprintf(cg->out, "bbb_ClampBot(");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 0]);
    fprintf(cg->out, ", ");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 1]);
    fprintf(cg->out, ")");
    return;
  }
  // `(abs x)` -- base.h's Abs macro, so no f32/f64 dispatch, same as min/max.
  if (str8_match_lit("abs", n->call.callee, 0) && n->call.arg_count == 1) {
    fprintf(cg->out, "bbb_Abs(");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 0]);
    fprintf(cg->out, ")");
    return;
  }
  // One-argument libm functions, dispatched on x's type: f32 takes the
  // `f`-suffixed variant (sinf, sqrtf), f64 the bare name. The 3b name is
  // already the libm base name -- checker.c accepts only this set -- so no
  // name table is needed.
  if (n->call.arg_count == 1 &&
      (str8_match_lit("sin",   n->call.callee, 0) || str8_match_lit("cos",   n->call.callee, 0) ||
       str8_match_lit("tan",   n->call.callee, 0) || str8_match_lit("asin",  n->call.callee, 0) ||
       str8_match_lit("acos",  n->call.callee, 0) || str8_match_lit("atan",  n->call.callee, 0) ||
       str8_match_lit("sinh",  n->call.callee, 0) || str8_match_lit("cosh",  n->call.callee, 0) ||
       str8_match_lit("tanh",  n->call.callee, 0) || str8_match_lit("sqrt",  n->call.callee, 0) ||
       str8_match_lit("cbrt",  n->call.callee, 0) || str8_match_lit("ceil",  n->call.callee, 0) ||
       str8_match_lit("floor", n->call.callee, 0) || str8_match_lit("round", n->call.callee, 0))) {
    TypedIndex x_idx = cg->tast->extra[n->call.arg_first + 0];
    b32        is_f32 = cg->resolved_types[x_idx].kind == TypeKind_F32;
    fprintf(cg->out, "%.*s%s(", str8_varg(n->call.callee), is_f32 ? "f" : "");
    cg_expr(cg, x_idx);
    fprintf(cg->out, ")");
    return;
  }
  // `(atan2 y x)` / `(pow base exp)` -- two-argument libm, same dispatch.
  if ((str8_match_lit("atan2", n->call.callee, 0) || str8_match_lit("pow", n->call.callee, 0))
      && n->call.arg_count == 2) {
    TypedIndex a_idx  = cg->tast->extra[n->call.arg_first + 0];
    TypedIndex b_idx  = cg->tast->extra[n->call.arg_first + 1];
    b32        is_f32 = cg->resolved_types[a_idx].kind == TypeKind_F32;
    fprintf(cg->out, "%.*s%s(", str8_varg(n->call.callee), is_f32 ? "f" : "");
    cg_expr(cg, a_idx);
    fprintf(cg->out, ", ");
    cg_expr(cg, b_idx);
    fprintf(cg->out, ")");
    return;
  }
  // `(mod a b)` -- `fmod`/`fmodf`, spelled out since the 3b name differs.
  if (str8_match_lit("mod", n->call.callee, 0) && n->call.arg_count == 2) {
    TypedIndex a_idx  = cg->tast->extra[n->call.arg_first + 0];
    TypedIndex b_idx  = cg->tast->extra[n->call.arg_first + 1];
    b32        is_f32 = cg->resolved_types[a_idx].kind == TypeKind_F32;
    fprintf(cg->out, "fmod%s(", is_f32 ? "f" : "");
    cg_expr(cg, a_idx);
    fprintf(cg->out, ", ");
    cg_expr(cg, b_idx);
    fprintf(cg->out, ")");
    return;
  }
  // `(len x)`. String: `.size`. Fixed array: the declared count as a literal,
  // with `x` still evaluated through a discarding comma so its side effects
  // are not skipped. Vector: a runtime `dyn_count` read.
  if (str8_match_lit("len", n->call.callee, 0) && n->call.arg_count == 1) {
    TypedIndex x_idx = cg->tast->extra[n->call.arg_first + 0];
    TypeRef    x_ty  = cg->resolved_types[x_idx];
    if (x_ty.kind == TypeKind_Array) {
      fprintf(cg->out, "((void)(");
      cg_expr(cg, x_idx);
      fprintf(cg->out, "), (u64)%llu)", (unsigned long long)x_ty.count);
    } else if (x_ty.kind == TypeKind_Vector) {
      fprintf(cg->out, "bbb_dyn_count(");
      cg_expr(cg, x_idx);
      fprintf(cg->out, ")");
    } else {
      fprintf(cg->out, "(");
      cg_expr(cg, x_idx);
      fprintf(cg->out, ").size");
    }
    return;
  }
  // `(dyn-count p)` -- reads the hidden DynHdr just before `p`; see checker.c
  // for what that assumes about `p`.
  if (str8_match_lit("dyn-count", n->call.callee, 0) && n->call.arg_count == 1) {
    fprintf(cg->out, "bbb_dyn_count(");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 0]);
    fprintf(cg->out, ")");
    return;
  }
  // `(nth-checked base index)` -> a checked-nilable `T*`. For an Array the
  // bound is a compile-time constant, so `base` is emitted once, inside the
  // index expression. For a Vector the bound is a runtime `dyn_count(base)`
  // read needed alongside that expression, so `base` is hoisted into a local
  // rather than evaluated twice.
  if (str8_match_lit("nth-checked", n->call.callee, 0) && n->call.arg_count == 2) {
    TypedIndex base_idx  = cg->tast->extra[n->call.arg_first + 0];
    TypedIndex idx_idx   = cg->tast->extra[n->call.arg_first + 1];
    TypeRef    base_ty   = cg->resolved_types[base_idx];
    String8    elem_c_ty = c_type_from_typeref(cg, *base_ty.pointee);
    fprintf(cg->out, "({ u64 _3b_i = (u64)(");
    cg_expr(cg, idx_idx);
    fprintf(cg->out, ");");
    if (base_ty.kind == TypeKind_Array) {
      fprintf(cg->out, " (_3b_i < %llu) ? &(", (unsigned long long)base_ty.count);
      cg_expr(cg, base_idx);
      fprintf(cg->out, ")[_3b_i] : (%.*s*)0; })", str8_varg(elem_c_ty));
    } else { // Vector
      fprintf(cg->out, " %.*s* _3b_base = ", str8_varg(elem_c_ty));
      cg_expr(cg, base_idx);
      fprintf(cg->out, "; (_3b_i < bbb_dyn_count(_3b_base)) ? &_3b_base[_3b_i] : (%.*s*)0; })", str8_varg(elem_c_ty));
    }
    return;
  }
  // `(vector-clear v)` -- truncates the hidden DynHdr's count to 0 in place.
  // `v` is hoisted like nth-checked's `_3b_base`, and null-guarded: a
  // never-pushed Vector is NULL, and `dyn_hdr(NULL)` is not a valid pointer.
  if (str8_match_lit("vector-clear", n->call.callee, 0) && n->call.arg_count == 1) {
    TypedIndex vec_idx  = cg->tast->extra[n->call.arg_first + 0];
    TypeRef    vec_ty   = cg->resolved_types[vec_idx];
    String8    elem_c_ty = c_type_from_typeref(cg, *vec_ty.pointee);
    fprintf(cg->out, "({ %.*s* _3b_v = ", str8_varg(elem_c_ty));
    cg_expr(cg, vec_idx);
    fprintf(cg->out, "; if (_3b_v) { bbb_dyn_hdr(_3b_v)->count = 0; } })");
    return;
  }
  // `(vector-swap-remove v index)` removes in O(1) by overwriting `index` with
  // the last element; `(vector-remove-at v index)` preserves order by shifting
  // the rest down. Both return false without mutating if `index` is out of
  // range -- see checker.c for why these are checked while `nth` is not. `v`
  // and `index` are each hoisted, as in nth-checked.
  if ((str8_match_lit("vector-swap-remove", n->call.callee, 0) || str8_match_lit("vector-remove-at", n->call.callee, 0))
      && n->call.arg_count == 2) {
    b32        is_swap   = str8_match_lit("vector-swap-remove", n->call.callee, 0);
    TypedIndex vec_idx   = cg->tast->extra[n->call.arg_first + 0];
    TypedIndex index_idx = cg->tast->extra[n->call.arg_first + 1];
    TypeRef    vec_ty    = cg->resolved_types[vec_idx];
    String8    elem_c_ty = c_type_from_typeref(cg, *vec_ty.pointee);
    fprintf(cg->out, "({ %.*s* _3b_v = ", str8_varg(elem_c_ty));
    cg_expr(cg, vec_idx);
    fprintf(cg->out, "; u64 _3b_i = (u64)(");
    cg_expr(cg, index_idx);
    fprintf(cg->out, "); u64 _3b_n = bbb_dyn_count(_3b_v); b32 _3b_ok = _3b_i < _3b_n; if (_3b_ok) { ");
    if (is_swap) {
      fprintf(cg->out, "_3b_v[_3b_i] = _3b_v[_3b_n - 1]; ");
    } else {
      fprintf(cg->out, "for (u64 _3b_j = _3b_i; _3b_j + 1 < _3b_n; _3b_j += 1) { _3b_v[_3b_j] = _3b_v[_3b_j + 1]; } ");
    }
    fprintf(cg->out, "bbb_dyn_hdr(_3b_v)->count = _3b_n - 1; } _3b_ok; })");
    return;
  }
  // `(vector-contains? v x)` -- linear search. Per-element comparison goes
  // through cg_emit_field_eq_expr, the same helper `=`/`!=` use on a struct
  // field, so scalars, strings and comparable structs work with no new
  // equality logic here.
  if (str8_match_lit("vector-contains?", n->call.callee, 0) && n->call.arg_count == 2) {
    TypedIndex vec_idx  = cg->tast->extra[n->call.arg_first + 0];
    TypedIndex x_idx    = cg->tast->extra[n->call.arg_first + 1];
    TypeRef    vec_ty   = cg->resolved_types[vec_idx];
    TypeRef    elem_ty  = *vec_ty.pointee;
    String8    elem_c_ty = c_type_from_typeref(cg, elem_ty);
    fprintf(cg->out, "({ %.*s* _3b_v = ", str8_varg(elem_c_ty));
    cg_expr(cg, vec_idx);
    fprintf(cg->out, "; %.*s _3b_needle = ", str8_varg(elem_c_ty));
    cg_expr(cg, x_idx);
    fprintf(cg->out, "; u64 _3b_n = bbb_dyn_count(_3b_v); b32 _3b_found = 0; "
                      "for (u64 _3b_i = 0; _3b_i < _3b_n; _3b_i += 1) { if (");
    cg_emit_field_eq_expr(cg, elem_ty, str8_lit("_3b_v[_3b_i]"), str8_lit("_3b_needle"));
    fprintf(cg->out, ") { _3b_found = 1; break; } } _3b_found; })");
    return;
  }
  // `(map-set arena m key value)` -> `<Prefix>_set(&arena, &m, key, value)`.
  if (str8_match_lit("map-set", n->call.callee, 0) && n->call.arg_count == 4) {
    TypedIndex m_idx  = cg->tast->extra[n->call.arg_first + 1];
    TypeRef    m_ty   = cg->resolved_types[m_idx];
    String8    prefix = cg_hashtable_c_name(cg, *m_ty.map_key, m_ty.pointee);
    fprintf(cg->out, "%.*s_set(&(", str8_varg(prefix));
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 0]);
    fprintf(cg->out, "), &(");
    cg_expr(cg, m_idx);
    fprintf(cg->out, "), ");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 2]);
    fprintf(cg->out, ", ");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 3]);
    fprintf(cg->out, ")");
    return;
  }
  // `(map-get m key)` -> `<Prefix>_get(&m, key)`.
  if (str8_match_lit("map-get", n->call.callee, 0) && n->call.arg_count == 2) {
    TypedIndex m_idx  = cg->tast->extra[n->call.arg_first + 0];
    TypeRef    m_ty   = cg->resolved_types[m_idx];
    String8    prefix = cg_hashtable_c_name(cg, *m_ty.map_key, m_ty.pointee);
    fprintf(cg->out, "%.*s_get(&(", str8_varg(prefix));
    cg_expr(cg, m_idx);
    fprintf(cg->out, "), ");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 1]);
    fprintf(cg->out, ")");
    return;
  }
  // `(map-remove m key)` / `(map-contains? m key)` -> `<Prefix>_remove(&m, key)`
  // / `<Prefix>_contains(&m, key)`.
  if ((str8_match_lit("map-remove", n->call.callee, 0) || str8_match_lit("map-contains?", n->call.callee, 0))
      && n->call.arg_count == 2) {
    b32        is_remove = str8_match_lit("map-remove", n->call.callee, 0);
    TypedIndex m_idx     = cg->tast->extra[n->call.arg_first + 0];
    TypeRef    m_ty      = cg->resolved_types[m_idx];
    String8    prefix    = cg_hashtable_c_name(cg, *m_ty.map_key, m_ty.pointee);
    fprintf(cg->out, "%.*s_%s(&(", str8_varg(prefix), is_remove ? "remove" : "contains");
    cg_expr(cg, m_idx);
    fprintf(cg->out, "), ");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 1]);
    fprintf(cg->out, ")");
    return;
  }
  // `(set-add arena s value)` -> `<Prefix>_add(&arena, &s, value)`.
  if (str8_match_lit("set-add", n->call.callee, 0) && n->call.arg_count == 3) {
    TypedIndex s_idx  = cg->tast->extra[n->call.arg_first + 1];
    TypeRef    s_ty   = cg->resolved_types[s_idx];
    String8    prefix = cg_hashtable_c_name(cg, *s_ty.pointee, NULL);
    fprintf(cg->out, "%.*s_add(&(", str8_varg(prefix));
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 0]);
    fprintf(cg->out, "), &(");
    cg_expr(cg, s_idx);
    fprintf(cg->out, "), ");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 2]);
    fprintf(cg->out, ")");
    return;
  }
  // `(set-contains? s value)` / `(set-remove s value)` -> `<Prefix>_contains(&s, value)`
  // / `<Prefix>_remove(&s, value)`.
  if ((str8_match_lit("set-contains?", n->call.callee, 0) || str8_match_lit("set-remove", n->call.callee, 0))
      && n->call.arg_count == 2) {
    b32        is_remove = str8_match_lit("set-remove", n->call.callee, 0);
    TypedIndex s_idx     = cg->tast->extra[n->call.arg_first + 0];
    TypeRef    s_ty      = cg->resolved_types[s_idx];
    String8    prefix    = cg_hashtable_c_name(cg, *s_ty.pointee, NULL);
    fprintf(cg->out, "%.*s_%s(&(", str8_varg(prefix), is_remove ? "remove" : "contains");
    cg_expr(cg, s_idx);
    fprintf(cg->out, "), ");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 1]);
    fprintf(cg->out, ")");
    return;
  }
  // Zero-argument bridges onto the generated runtime's lane_idx/lane_count/
  // lane_sync (see cg_write_runtime_source's Lanes section) and
  // `*ctx_scratch()`, this lane's private thread-local arena, by value like
  // cg_scratch_expr. lane-index, lane-sync and lane-arena are valid only
  // inside a `parallel` block; lane-count is a pool-wide constant readable
  // anywhere. Both rules live in the checker, not here.
  if (str8_match_lit("lane-index", n->call.callee, 0)) {
    fprintf(cg->out, "bbb_lane_idx()");
    return;
  }
  if (str8_match_lit("lane-count", n->call.callee, 0)) {
    fprintf(cg->out, "bbb_lane_count()");
    return;
  }
  if (str8_match_lit("lane-sync", n->call.callee, 0)) {
    fprintf(cg->out, "bbb_lane_sync()");
    return;
  }
  if (str8_match_lit("lane-arena", n->call.callee, 0)) {
    fprintf(cg->out, "(*bbb_ctx_scratch())");
    return;
  }
  // `(pow2? x)` / `(pow2-or-zero? x)` -- base.h's IsPow2/IsPow2OrZero.
  if (str8_match_lit("pow2?", n->call.callee, 0) && n->call.arg_count == 1) {
    fprintf(cg->out, "bbb_IsPow2(");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 0]);
    fprintf(cg->out, ")");
    return;
  }
  if (str8_match_lit("pow2-or-zero?", n->call.callee, 0) && n->call.arg_count == 1) {
    fprintf(cg->out, "bbb_IsPow2OrZero(");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 0]);
    fprintf(cg->out, ")");
    return;
  }
  // `(mem-set dst byte size)` / `(mem-copy dst src size)` /
  // `(mem-zero dst size)` / `(mem-compare a b size)` -- the generated
  // runtime's bbb_MemorySet/MemoryCopy/MemoryZero/MemoryCompare macros
  // (runtime/bbb_prelude.h). NOT the compiler's own base/base.h, which
  // defines unprefixed versions that emitted code cannot resolve.
  if (str8_match_lit("mem-set", n->call.callee, 0) && n->call.arg_count == 3) {
    fprintf(cg->out, "bbb_MemorySet(");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 0]);
    fprintf(cg->out, ", ");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 1]);
    fprintf(cg->out, ", ");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 2]);
    fprintf(cg->out, ")");
    return;
  }
  if (str8_match_lit("mem-copy", n->call.callee, 0) && n->call.arg_count == 3) {
    fprintf(cg->out, "bbb_MemoryCopy(");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 0]);
    fprintf(cg->out, ", ");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 1]);
    fprintf(cg->out, ", ");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 2]);
    fprintf(cg->out, ")");
    return;
  }
  if (str8_match_lit("mem-zero", n->call.callee, 0) && n->call.arg_count == 2) {
    fprintf(cg->out, "bbb_MemoryZero(");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 0]);
    fprintf(cg->out, ", ");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 1]);
    fprintf(cg->out, ")");
    return;
  }
  if (str8_match_lit("mem-compare", n->call.callee, 0) && n->call.arg_count == 3) {
    fprintf(cg->out, "bbb_MemoryCompare(");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 0]);
    fprintf(cg->out, ", ");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 1]);
    fprintf(cg->out, ", ");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 2]);
    fprintf(cg->out, ")");
    return;
  }
  // base.h's str8_prefix/str8_skip/str8_postfix/str8_chop. All four return a
  // view rather than allocating, so they emit directly with no runtime bridge.
  if (str8_match_lit("string-prefix", n->call.callee, 0) && n->call.arg_count == 2) {
    fprintf(cg->out, "bbb_str8_prefix(");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 0]);
    fprintf(cg->out, ", ");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 1]);
    fprintf(cg->out, ")");
    return;
  }
  if (str8_match_lit("string-skip", n->call.callee, 0) && n->call.arg_count == 2) {
    fprintf(cg->out, "bbb_str8_skip(");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 0]);
    fprintf(cg->out, ", ");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 1]);
    fprintf(cg->out, ")");
    return;
  }
  if (str8_match_lit("string-postfix", n->call.callee, 0) && n->call.arg_count == 2) {
    fprintf(cg->out, "bbb_str8_postfix(");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 0]);
    fprintf(cg->out, ", ");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 1]);
    fprintf(cg->out, ")");
    return;
  }
  if (str8_match_lit("string-chop", n->call.callee, 0) && n->call.arg_count == 2) {
    fprintf(cg->out, "bbb_str8_chop(");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 0]);
    fprintf(cg->out, ", ");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 1]);
    fprintf(cg->out, ")");
    return;
  }
  // `(string-substr s start end)` -- str8_substr takes a Rng1u64, which is not
  // a language type, so it is built here as a compound literal.
  if (str8_match_lit("string-substr", n->call.callee, 0) && n->call.arg_count == 3) {
    fprintf(cg->out, "bbb_str8_substr(");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 0]);
    fprintf(cg->out, ", (bbb_Rng1u64){ .min = ");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 1]);
    fprintf(cg->out, ", .max = ");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 2]);
    fprintf(cg->out, " })");
    return;
  }
  // `(string-cat arena a b)` -- str8_cat, with the usual `&` on the
  // by-value arena.
  if (str8_match_lit("string-cat", n->call.callee, 0) && n->call.arg_count == 3) {
    fprintf(cg->out, "bbb_str8_cat(&(");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 0]);
    fprintf(cg->out, "), ");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 1]);
    fprintf(cg->out, ", ");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 2]);
    fprintf(cg->out, ")");
    return;
  }
  // `(string-copy arena s)` -- base.h's str8_copy.
  if (str8_match_lit("string-copy", n->call.callee, 0) && n->call.arg_count == 2) {
    fprintf(cg->out, "bbb_str8_copy(&(");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 0]);
    fprintf(cg->out, "), ");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 1]);
    fprintf(cg->out, ")");
    return;
  }
  // `(cstring-copy arena s)` -- cstring_str8 already returns `char*`, like
  // the `cstring` special form's cast (TypedNodeKind_CstrExpr below).
  if (str8_match_lit("cstring-copy", n->call.callee, 0) && n->call.arg_count == 2) {
    fprintf(cg->out, "bbb_cstring_str8(&(");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 0]);
    fprintf(cg->out, "), ");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 1]);
    fprintf(cg->out, ")");
    return;
  }
  // string-find, string-find-reverse, string-starts-with and string-ends-with
  // all bridge through 3b_runtime (see cg_write_runtime_header/source) like
  // string-match, which keeps StringMatchFlags out of the generated code's view
  // of these signatures.
  if (str8_match_lit("string-find", n->call.callee, 0) && n->call.arg_count == 4) {
    fprintf(cg->out, "bbb_string_find(");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 0]);
    fprintf(cg->out, ", ");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 1]);
    fprintf(cg->out, ", ");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 2]);
    fprintf(cg->out, ", ");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 3]);
    fprintf(cg->out, ")");
    return;
  }
  if (str8_match_lit("string-find-reverse", n->call.callee, 0) && n->call.arg_count == 4) {
    fprintf(cg->out, "bbb_string_find_reverse(");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 0]);
    fprintf(cg->out, ", ");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 1]);
    fprintf(cg->out, ", ");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 2]);
    fprintf(cg->out, ", ");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 3]);
    fprintf(cg->out, ")");
    return;
  }
  if (str8_match_lit("string-starts-with", n->call.callee, 0) && n->call.arg_count == 3) {
    fprintf(cg->out, "bbb_string_starts_with(");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 0]);
    fprintf(cg->out, ", ");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 1]);
    fprintf(cg->out, ", ");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 2]);
    fprintf(cg->out, ")");
    return;
  }
  if (str8_match_lit("string-ends-with", n->call.callee, 0) && n->call.arg_count == 3) {
    fprintf(cg->out, "bbb_string_ends_with(");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 0]);
    fprintf(cg->out, ", ");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 1]);
    fprintf(cg->out, ", ");
    cg_expr(cg, cg->tast->extra[n->call.arg_first + 2]);
    fprintf(cg->out, ")");
    return;
  }
  // A real call is never legal C in a file-scope initializer, so try to inline
  // it away first -- see cg_call_is_const_foldable. A non-foldable callee falls
  // through to ordinary emission below.
  if (cg->in_static_init && cg_call_is_const_foldable(cg, idx)) {
    cg_emit_folded_call(cg, idx);
    return;
  }
  // Ordinary call: a top-level function, or an indirect call through a local or
  // param of function-pointer type. Same lookup-local-first pattern as the
  // Identifier case, since C calls a function pointer with the same
  // `name(args)` syntax and no dereference. The checker guarantees the callee
  // names one or the other, so anything that is not a local is assumed to be
  // the mangled top-level name.
  String8* local = cg_scope_lookup(cg, n->call.callee);
  // A local fn-pointer callee has no FnEntry -- fns_by_name holds only
  // top-level fns -- and its declared param types are not available in codegen
  // at all, so its arguments fall back to plain cg_expr. A variadic extern's
  // trailing arguments have no static param type either, and take the same path.
  FnEntry*   callee_fn   = local ? NULL : (FnEntry*)hashtable_lookup(&cg->fns_by_name, n->call.callee);
  TypedNode* callee_decl = callee_fn ? &cg->tast->nodes[callee_fn->decl] : NULL;
  // A bodyless, non-imported callee is a real `extern`: it names a function
  // that exists in the C library, declared by the header `include-first` puts
  // in scope, so its prototype is spelled in the library's own types rather
  // than this package's. That is the one boundary cg_ffi_cast_open exists for.
  // An IMPORTED bodyless decl is only a placeholder for another 3b package's
  // function, whose prototype does use this package's spelling, so it is
  // excluded.
  b32 is_ffi_call = callee_decl && callee_decl->func.body == TYPED_NIL && !callee_decl->is_imported;
  b32 ret_cast    = is_ffi_call && cg_ffi_cast_open(cg, callee_decl->func.return_type, false);
  fprintf(cg->out, "%.*s(", str8_varg(local ? *local : cg_symbol_name(cg, n->call.callee)));
  foreach_index(i, n->call.arg_count) {
    if (i != 0) fprintf(cg->out, ", ");
    TypedIndex arg_idx = cg->tast->extra[n->call.arg_first + i];
    if (callee_decl && i < callee_decl->func.param_count) {
      TypeRef param_ty = cg->tast->params[callee_decl->func.param_first + i].type;
      b32     arg_cast = is_ffi_call && cg_ffi_cast_open(cg, param_ty, true);
      cg_call_arg_decay_to(cg, arg_idx, param_ty);
      if (arg_cast) fprintf(cg->out, ")");
    } else {
      cg_expr(cg, arg_idx);
    }
  }
  fprintf(cg->out, ")");
  if (ret_cast) fprintf(cg->out, ")");
}

// Whether cg_init_value below writes a bare brace list for this initializer.
// A brace list is the ONLY initializer form C accepts for an array-typed slot:
// `T x[N] = expr;` is rejected outright, and `.field = expr` inside a struct
// literal is worse, silently initializing the field's first scalar from the
// decayed pointer. So every array-typed initialization site has to ask this and
// fall back to a byte copy when the answer is no -- cg_declare_and_init_maybe_val for a
// binding, cg_struct_literal for a field.
static b32
cg_init_is_braced(Codegen* cg, TypedIndex value_idx) {
  TypedNode* n = &cg->tast->nodes[value_idx];
  return n->kind == TypedNodeKind_ArrayLiteral
      || (n->kind == TypedNodeKind_ZeroExpr && n->type_query.type.kind == TypeKind_Array);
}

// Emits an initializer value, routing array literals and array-typed `zero`
// through the bare-braces path rather than ordinary cg_expr dispatch: C cannot
// initialize an array via `=` from a cast-prefixed compound literal, only from
// a brace list, and that holds everywhere an array literal can appear. Every
// declaration-initializer site should go through this rather than cg_expr.
//
// `declared_type` is the slot's declared type. It only matters on the cg_expr
// fallback -- the brace cases are always arrays, where decay cannot apply. See
// cg_expr_decay_to for why a plain value may still need a cast.
static void
cg_init_value(Codegen* cg, TypedIndex value_idx, TypeRef declared_type) {
  if (cg_init_is_braced(cg, value_idx)) {
    TypedNode* n = &cg->tast->nodes[value_idx];
    if (n->kind == TypedNodeKind_ArrayLiteral) cg_array_literal_braces(cg, value_idx);
    else                                       fprintf(cg->out, "{0}");
  } else {
    cg_expr_decay_to(cg, value_idx, declared_type);
  }
}

// Shared by cg_stmt's ConstDecl/VarDecl and cg_let_expr's bindings:
// "TYPE name = init;", except for an array-typed binding whose initializer is
// not brace-friendly. `T name[N] = expr;` is invalid C unless `expr` is itself
// a brace list -- `f32 name[4] = some_struct.some_array_field;` does not
// compile -- so that case declares bare and memcpys the value in, matching an
// array's flat representation. `init_idx == TYPED_NIL` means no initializer was
// written and defaults to `{0}`; only const/var can hit it, as every `let`
// binding has one.
static void
cg_declare_and_init_maybe_val(Codegen* cg, TypeRef type, String8 c_name, TypedIndex init_idx, b32 is_val) {
  if (is_val) cg_declare_val(cg, type, c_name);
  else        cg_declare(cg, type, c_name);
  if (init_idx == TYPED_NIL) {
    fprintf(cg->out, " = {0};");
    return;
  }
  if (type.kind != TypeKind_Array || cg_init_is_braced(cg, init_idx)) {
    fprintf(cg->out, " = ");
    cg_init_value(cg, init_idx, type);
    fprintf(cg->out, ";");
  } else {
    // The `(void*)` cast is needed for a const-qualified array (ConstDecl),
    // where passing the destination would otherwise warn about discarding
    // const -- this being the one-time initialization, the only moment writing
    // into a `val` is legitimate.
    fprintf(cg->out, "; bbb_MemoryCopy((void*)(%.*s), (", str8_varg(c_name));
    cg_expr(cg, init_idx);
    fprintf(cg->out, "), sizeof(%.*s));", str8_varg(c_name));
  }
}

// `var` and `let`: no qualifier, an ordinary mutable local.
static void
cg_declare_and_init(Codegen* cg, TypeRef type, String8 c_name, TypedIndex init_idx) {
  cg_declare_and_init_maybe_val(cg, type, c_name, init_idx, false);
}

// `val`: qualifier placed by cg_declare_val.
static void
cg_declare_and_init_val(Codegen* cg, TypeRef type, String8 c_name, TypedIndex init_idx) {
  cg_declare_and_init_maybe_val(cg, type, c_name, init_idx, true);
}

// checker.c's find_field_recursive, re-derived off cg->structs/cg->tast rather
// than a Checker. Only cg_struct_literal uses it, to find a field's declared
// type for the array-decay cast -- cg_init_value has no other way to learn that
// type at a struct-literal site.
static Param*
cg_find_field_recursive(Codegen* cg, TypedNode* decl, String8 field_name) {
  foreach_index(j, decl->struct_decl.field_count) {
    Param* f = &cg->tast->params[decl->struct_decl.field_first + j];
    if (!f->is_anon && str8_match(f->name, field_name, 0)) return f;
  }
  foreach_index(j, decl->struct_decl.field_count) {
    Param* f = &cg->tast->params[decl->struct_decl.field_first + j];
    if (!f->is_anon || f->type.kind != TypeKind_Named) continue;
    StructEntry* nested = NULL;
    foreach_index(i, dyn_count(cg->structs)) {
      if (str8_match(cg->structs[i].name, f->type.name, 0)) { nested = &cg->structs[i]; break; }
    }
    if (!nested) continue;
    Param* found = cg_find_field_recursive(cg, &cg->tast->nodes[nested->decl], field_name);
    if (found) return found;
  }
  return NULL;
}

// A struct literal field's declared type, or the value's own checked type when
// the struct is not in cg->structs (an imported or synthesized one). Split out
// because cg_struct_literal now walks the field list twice.
static TypeRef
cg_struct_literal_field_type(Codegen* cg, TypedNode* decl, FieldInit* fi) {
  Param* matched = decl ? cg_find_field_recursive(cg, decl, fi->name) : NULL;
  return matched ? matched->type : cg->resolved_types[fi->value];
}

// Struct construction becomes a C99 designated-initializer compound literal,
// `(Creature){ .name = ..., .health = ... }`. Designated initializers are
// order-independent, so fields emit in whatever order the literal used, with no
// need to match the declared order. Values go through cg_init_value so
// array-typed fields get the bare-braces treatment.
//
// An array-typed field whose value is NOT brace-friendly -- another array,
// typically a local built up in a loop -- cannot be written as a designated
// initializer at all. `.items = other_array` compiles, but the array decays and
// C initializes `items[0]`'s first scalar from the pointer, leaving the rest
// zero: a silent, warning-only miscompile (that warning is how this was found).
// So the whole literal moves into a statement expression, the copyable fields
// still initializing a temp in one go and the array fields memcpy'd in after.
// The temp needs no per-occurrence unique name for the same reason
// cg_parallel_for_expr's `__pr` does not -- each is its own `({ })`, hence its
// own C block scope, so a nested one just shadows.
//
// The cost is evaluation ORDER: array fields now run after every other field,
// rather than in the order written. Nothing else in the emitted C reorders
// evaluation, so this is the one place a caller could notice.
void
cg_struct_literal(Codegen* cg, TypedIndex idx) {
  TypedNode*   n  = &cg->tast->nodes[idx];
  StructEntry* se = NULL;
  foreach_index(i, dyn_count(cg->structs)) {
    if (str8_match(cg->structs[i].name, n->struct_lit.type_name, 0)) { se = &cg->structs[i]; break; }
  }
  TypedNode* decl   = se ? &cg->tast->nodes[se->decl] : NULL;
  String8    c_name = cg_symbol_name(cg, n->struct_lit.type_name);

  b32 has_array_copy = false;
  foreach_index(i, n->struct_lit.field_count) {
    FieldInit* fi = &cg->tast->field_inits[n->struct_lit.field_first + i];
    if (cg_struct_literal_field_type(cg, decl, fi).kind == TypeKind_Array
        && !cg_init_is_braced(cg, fi->value)) {
      has_array_copy = true;
      break;
    }
  }

  if (has_array_copy) {
    if (cg->in_static_init) {
      // Same reasoning as the `reinterpret` case in cg_expr: a statement
      // expression is not a C constant expression, so emitting one here would
      // surface as "braced-group within expression allowed only inside a
      // function" against generated code, naming no 3b at all.
      diag_error(n->token,
                 "a top-level `val`/`var` of type `%.*s` cannot fill its array-typed field(s) from "
                 "another array -- C requires a constant expression there, and copying an array "
                 "compiles to a memcpy. Use an inline array literal, or build the value in a function",
                 str8_varg(n->struct_lit.type_name));
      cg->had_error = true;
    }
    fprintf(cg->out, "({ %.*s _3b_slit = ", str8_varg(c_name));
  }

  fprintf(cg->out, "(%.*s){ ", str8_varg(c_name));
  b32 wrote_field = false;
  foreach_index(i, n->struct_lit.field_count) {
    FieldInit* fi       = &cg->tast->field_inits[n->struct_lit.field_first + i];
    TypeRef    field_ty = cg_struct_literal_field_type(cg, decl, fi);
    if (field_ty.kind == TypeKind_Array && !cg_init_is_braced(cg, fi->value)) continue; // memcpy'd below
    if (wrote_field) fprintf(cg->out, ", ");
    wrote_field = true;
    fprintf(cg->out, ".%.*s = ", str8_varg(c_mangle_name(ctx_scratch(), fi->name)));
    cg_init_value(cg, fi->value, field_ty);
  }
  if (!wrote_field) fprintf(cg->out, "0"); // every field was an array copy; `{ }` is not valid C
  fprintf(cg->out, " }");

  if (has_array_copy) {
    fprintf(cg->out, ";");
    foreach_index(i, n->struct_lit.field_count) {
      FieldInit* fi       = &cg->tast->field_inits[n->struct_lit.field_first + i];
      TypeRef    field_ty = cg_struct_literal_field_type(cg, decl, fi);
      if (field_ty.kind != TypeKind_Array || cg_init_is_braced(cg, fi->value)) continue;
      String8 c_field = c_mangle_name(ctx_scratch(), fi->name);
      fprintf(cg->out, " bbb_MemoryCopy(_3b_slit.%.*s, (", str8_varg(c_field));
      cg_expr(cg, fi->value);
      fprintf(cg->out, "), sizeof(_3b_slit.%.*s));", str8_varg(c_field));
    }
    fprintf(cg->out, " _3b_slit; })");
  }
}

// Just the brace-list part of an array literal, with no `(Type[N])` cast.
// Structs have ordinary value semantics in C, so a nested struct compound
// literal is fine as a field value, but an array inside another aggregate's
// initializer can only be initialized from a brace list. So a nested array
// literal -- a struct field's value, or another array literal's element --
// never carries its own cast; only the outermost one does (cg_array_literal).
static void
cg_array_literal_braces(Codegen* cg, TypedIndex idx) {
  TypedNode* n = &cg->tast->nodes[idx];
  fprintf(cg->out, "{ ");
  foreach_index(i, n->array_lit.element_count) {
    if (i != 0) fprintf(cg->out, ", ");
    TypedIndex elem = cg->tast->array_elements[n->array_lit.element_first + i];
    if (cg->tast->nodes[elem].kind == TypedNodeKind_ArrayLiteral) {
      cg_array_literal_braces(cg, elem); // nested array literal -- braces only, same reasoning
    } else {
      cg_expr(cg, elem);
    }
  }
  fprintf(cg->out, " }");
}

// Standalone array literal: the full `(Type[N]){...}` compound literal, correct
// only for the outermost literal in a nested chain or one that is not nested at
// all. The cast type comes from resolved_types, set by check_array_literal.
void
cg_array_literal(Codegen* cg, TypedIndex idx) {
  TypeRef arr_ty = cg->resolved_types[idx];
  fprintf(cg->out, "(");
  cg_array_type_cast_text(cg, arr_ty);
  fprintf(cg->out, ")");
  cg_array_literal_braces(cg, idx);
}

// `(push arena Type)` / `(push arena Type Count)`, unified into one typed node
// by lowering, so this always emits push_array/push_array_zero with an explicit
// count. push_array(a, T, 1) and push_one(a, T) expand to the same C once
// sizeof(T)*1 folds, so count==1 needs no separate macro.
void
cg_push_alloc(Codegen* cg, TypedIndex idx) {
  TypedNode* n = &cg->tast->nodes[idx];
  fprintf(cg->out, "%s(&(", n->push_alloc.zeroed ? "bbb_push_array_zero" : "bbb_push_array");
  cg_expr(cg, n->push_alloc.arena);
  fprintf(cg->out, "), %.*s, ", str8_varg(c_type_from_typeref(cg, n->push_alloc.elem_type)));
  cg_expr(cg, n->push_alloc.count);
  fprintf(cg->out, ")");
}

// `(alloc Type)` / `(alloc Type Count)` -- malloc, cast to `Type*`. Same
// always-present-count shape as cg_push_alloc, lowering having synthesized the
// literal `1`, but malloc-backed and with no zeroed variant. `free` is the
// other half of the pair.
void
cg_alloc_expr(Codegen* cg, TypedIndex idx) {
  TypedNode* n     = &cg->tast->nodes[idx];
  String8    c_ty  = c_type_from_typeref(cg, n->alloc_expr.elem_type);
  fprintf(cg->out, "(%.*s*)malloc(sizeof(%.*s) * (", str8_varg(c_ty), str8_varg(c_ty));
  cg_expr(cg, n->alloc_expr.count);
  fprintf(cg->out, "))");
}

// `(push arena value)` -- arena-allocates one element sized to `value`'s
// checked type and copies `value` in. `_3b_push_tmp` is reusable across nested
// pushes: each lives in its own `({ })`, hence its own C block scope.
void
cg_push_copy(Codegen* cg, TypedIndex idx) {
  TypedNode* n        = &cg->tast->nodes[idx];
  TypeRef    value_ty  = cg->resolved_types[n->push_copy.value];
  String8    c_ty       = c_type_from_typeref(cg, value_ty);
  fprintf(cg->out, "({ %.*s* _3b_push_tmp = bbb_push_one(&(", str8_varg(c_ty));
  cg_expr(cg, n->push_copy.arena);
  fprintf(cg->out, "), %.*s); *_3b_push_tmp = ", str8_varg(c_ty));
  cg_expr(cg, n->push_copy.value);
  fprintf(cg->out, "; _3b_push_tmp; })");
}

// `(dyn-push arena arr value)`. base.h's `dyn_push` is a `do { } while (0)`
// statement macro -- it reseats `arr` itself on growth -- rather than an
// expression like push_array/push_one, so this wraps it in
// `({ ...; (void)0; })` to satisfy cg_expr's contract that every node emits one
// valid C expression wherever it lands.
void
cg_dyn_push(Codegen* cg, TypedIndex idx) {
  TypedNode* n = &cg->tast->nodes[idx];
  fprintf(cg->out, "({ bbb_dyn_push(&(");
  cg_expr(cg, n->dyn_push.arena);
  fprintf(cg->out, "), ");
  if (n->dyn_push.is_field_target) {
    // A FieldAccess node: cg_expr emits a repeatable C lvalue, which is what
    // the macro needs in order to reseat it on growth.
    cg_expr(cg, n->dyn_push.target_expr);
  } else {
    String8* local  = cg_scope_lookup(cg, n->dyn_push.arr_name);
    String8  c_name = local ? *local : cg_symbol_name(cg, n->dyn_push.arr_name);
    // This resolves `arr_name` straight from the scope table rather than
    // through the Identifier dispatch, so it must repeat that case's
    // by-reference-Vector deref by hand (see CgScopeEntry). Without it, the
    // reseat inside the macro body would rewrite the parameter's own T** copy
    // instead of growing the caller's T* through it.
    b32 is_ref = local && cg_scope_is_vector_ref_param(cg, n->dyn_push.arr_name);
    fprintf(cg->out, "%s%.*s%s", is_ref ? "(*" : "", str8_varg(c_name), is_ref ? ")" : "");
  }
  fprintf(cg->out, ", (");
  // Parenthesized because bbb_dyn_push is a macro and the preprocessor only
  // stops treating a comma as an argument separator inside parens -- braces do
  // not protect it. A multi-field struct literal value, `(T){.a=1, .b=2}`, would
  // otherwise read as extra macro arguments.
  cg_expr(cg, n->dyn_push.value);
  fprintf(cg->out, ")); (void)0; })");
}

// `(commit dst-arena src)` -- copies a dyn-push-grown `src`, sized by its
// hidden DynHdr count, into `dst-arena` as a right-sized allocation.
// Reimplements base.h's `dyn_commit_from_temp` inline instead of calling it,
// because that helper ends an ArenaTemp on the empty-array path, which would
// double-end a `scratch` block's temp region -- see TypedNodeKind_CommitExpr.
// Its empty-array result is mirrored anyway: NULL rather than a zero-length
// allocation, the ternary skipping the `arena_push` entirely.
void
cg_commit_expr(Codegen* cg, TypedIndex idx) {
  TypedNode* n      = &cg->tast->nodes[idx];
  TypeRef    src_ty = cg->resolved_types[n->commit_expr.src];
  String8    elem_c_ty = (src_ty.kind == TypeKind_Pointer && src_ty.pointee)
    ? c_type_from_typeref(cg, *src_ty.pointee)
    : str8_lit("i32"); // unreachable: the checker requires a Pointer here

  fprintf(cg->out, "({ u64 _3b_commit_cnt = bbb_dyn_count(");
  cg_expr(cg, n->commit_expr.src);
  fprintf(cg->out, "); %.*s* _3b_commit_dst = _3b_commit_cnt == 0 ? NULL : ({ ", str8_varg(elem_c_ty));
  fprintf(cg->out, "bbb_DynHdr* _3b_commit_hdr = (bbb_DynHdr*)bbb_arena_push(&(");
  cg_expr(cg, n->commit_expr.dst_arena);
  // Alignment is Max(element, DynHdr): the header sits in front of the
  // elements, so the allocation must meet its 8-byte alignment regardless of
  // the element type. Element alignment alone under-aligns whenever it is
  // below 8, as when committing a u8 or bool array. bbb_arena_dyn_grow carries
  // the same reasoning.
  fprintf(cg->out, "), sizeof(bbb_DynHdr) + _3b_commit_cnt * sizeof(%.*s), bbb_Max(bbb_AlignOf(%.*s), bbb_AlignOf(bbb_DynHdr))); ",
          str8_varg(elem_c_ty), str8_varg(elem_c_ty));
  fprintf(cg->out, "_3b_commit_hdr->capacity = _3b_commit_cnt; _3b_commit_hdr->count = _3b_commit_cnt; ");
  fprintf(cg->out, "%.*s* _3b_commit_data = (%.*s*)((u8*)_3b_commit_hdr + sizeof(bbb_DynHdr)); ",
          str8_varg(elem_c_ty), str8_varg(elem_c_ty));
  fprintf(cg->out, "bbb_MemoryCopy(_3b_commit_data, ");
  cg_expr(cg, n->commit_expr.src);
  fprintf(cg->out, ", _3b_commit_cnt * sizeof(%.*s)); _3b_commit_data; }); _3b_commit_dst; })", str8_varg(elem_c_ty));
}

// Just the C lvalue a `set` writes through, with no `=` and no surrounding
// parentheses -- one per SetTargetKind. Split out of cg_expr's SetExpr case
// because the array-typed form needs the same lvalue in a different shape
// (`&(lvalue)`) than the ordinary assignment does.
static void
cg_set_target_lvalue(Codegen* cg, TypedNode* n) {
  switch (n->set_expr.target_kind) {
    case SetTargetKind_Deref: {
      fprintf(cg->out, "*");
      cg_expr(cg, n->set_expr.target_expr);
    } break;
    case SetTargetKind_Index: {
      b32 parens = cg_needs_parens_before_dot(cg, n->set_expr.index_base);
      if (parens) fprintf(cg->out, "(");
      cg_expr(cg, n->set_expr.index_base);
      if (parens) fprintf(cg->out, ")");
      fprintf(cg->out, "[");
      cg_expr(cg, n->set_expr.index_index);
      fprintf(cg->out, "]");
    } break;
    case SetTargetKind_Field: {
      // cg_expr's FieldAccess case already emits a valid C lvalue, so there is
      // nothing target-kind-specific to do beyond emitting it.
      cg_expr(cg, n->set_expr.target_expr);
    } break;
    default: {
      // The same by-reference-Vector deref the Identifier read case does:
      // `(set xs new-vec)` must write through the T** so the caller sees the
      // rebind, rather than reseating this function's own copy.
      String8* local  = cg_scope_lookup(cg, n->set_expr.target_name);
      b32      is_ref = local && cg_scope_is_vector_ref_param(cg, n->set_expr.target_name);
      fprintf(cg->out, "%s%.*s%s", is_ref ? "(*" : "",
              str8_varg(local ? *local : cg_symbol_name(cg, n->set_expr.target_name)),
              is_ref ? ")" : "");
    } break;
  }
}

void
cg_expr(Codegen* cg, TypedIndex idx) {
  if (idx == TYPED_NIL) {
    fprintf(cg->out, "/* <lowering error> */ 0");
    return;
  }
  TypedNode* n = &cg->tast->nodes[idx];
  switch (n->kind) {
    case TypedNodeKind_IntLiteral: {
      // Only i64/u64 need an explicit suffix; anything narrower fits in
      // `int`/`long` and takes its real width from the slot it flows into.
      //
      // The two 64-bit cases print the value themselves, because `value` is an
      // i64 holding what may be an unsigned bit pattern (see int_lit.value):
      //   - u64 prints unsigned, or a pattern with the top bit set comes out as
      //     `-9223372036854775808ULL`, whose `-` negates an already-unsigned
      //     constant to arrive back at the same bits by a confusing route.
      //   - i64's most negative value has no literal spelling in C at all:
      //     9223372036854775808 exceeds LLONG_MAX, so `-9223372036854775808LL`
      //     is a negated unsigned constant and gcc warns on every build. This
      //     is the spelling <stdint.h> itself uses for INT64_MIN.
      if (n->int_lit.explicit_type == TypeKind_U64) {
        fprintf(cg->out, "%lluULL", (unsigned long long)n->int_lit.value);
      } else if (n->int_lit.explicit_type == TypeKind_I64 && n->int_lit.value == INT64_MIN) {
        fprintf(cg->out, "(-9223372036854775807LL - 1)");
      } else {
        const char* suffix = n->int_lit.explicit_type == TypeKind_I64 ? "LL" : "";
        fprintf(cg->out, "%lld%s", (long long)n->int_lit.value, suffix);
      }
    } break;
    case TypedNodeKind_FloatLiteral: {
      // %g strips a trailing ".0" (100.0 -> "100"), and "100f" is not a valid
      // C float literal -- the `f` suffix needs a decimal point or exponent
      // already there -- so one is added back before appending it. Digit counts
      // are the round-trip precision of each type; f64 needs no suffix, a bare
      // C literal already being a double.
      b32  is_f64 = n->float_lit.explicit_type == TypeKind_F64;
      char buf[64];
      snprintf(buf, sizeof(buf), "%.*g", is_f64 ? 17 : 9, n->float_lit.value);
      b32 has_dot_or_exp = false;
      for (char* p = buf; *p; p += 1) {
        if (*p == '.' || *p == 'e' || *p == 'E') { has_dot_or_exp = true; break; }
      }
      fprintf(cg->out, "%s%s%s", buf, has_dot_or_exp ? "" : ".0", is_f64 ? "" : "f");
    } break;
    case TypedNodeKind_StringLiteral: {
      // In a file-scope initializer the String8's fields are emitted as a
      // braced initializer rather than the usual bbb_str8_lit(...), which
      // expands to a call and so cannot initialize an object with static
      // storage duration.
      //
      // Braces specifically, not a `(bbb_String8){...}` compound literal. GCC
      // extends static initialization to an outermost compound literal, which
      // every struct-typed top-level `val` relies on, but a nested one is still
      // not a constant expression, so `(Named){ .name = (bbb_String8){...} }`
      // fails on gcc and clang alike. A braced initializer nests fine, and a
      // string literal's address and length are genuine constant expressions.
      if (cg->in_static_init) {
        fprintf(cg->out, "{ (u8*)\"");
        cg_write_c_escaped(cg, n->string_lit.value);
        fprintf(cg->out, "\", %llu }", (unsigned long long)n->string_lit.value.size);
      } else {
        fprintf(cg->out, "bbb_str8_lit(\"");
        cg_write_c_escaped(cg, n->string_lit.value);
        fprintf(cg->out, "\")");
      }
    } break;
    case TypedNodeKind_NilLiteral: {
      fprintf(cg->out, "NULL"); // from <stddef.h>, transitively included via 3b_runtime.h
    } break;
    case TypedNodeKind_BoolLiteral: {
      fprintf(cg->out, n->bool_lit.value ? "true" : "false"); // from <stdbool.h>, also via 3b_runtime.h
    } break;
    case TypedNodeKind_SizeofExpr: {
      if (n->type_query.result_type.kind != TypeKind_Unresolved) {
        fprintf(cg->out, "(%.*s)", str8_varg(c_type_from_typeref(cg, n->type_query.result_type)));
      }
      fprintf(cg->out, "sizeof(%.*s)", str8_varg(c_type_from_typeref(cg, n->type_query.type)));
    } break;
    case TypedNodeKind_AlignofExpr: {
      if (n->type_query.result_type.kind != TypeKind_Unresolved) {
        fprintf(cg->out, "(%.*s)", str8_varg(c_type_from_typeref(cg, n->type_query.result_type)));
      }
      fprintf(cg->out, "_Alignof(%.*s)", str8_varg(c_type_from_typeref(cg, n->type_query.type)));
    } break;
    case TypedNodeKind_TypeNameExpr: {
      fprintf(cg->out, "bbb_str8_lit(\"");
      cg_write_c_escaped(cg, c_type_from_typeref(cg, n->type_query.type));
      fprintf(cg->out, "\")");
    } break;
    case TypedNodeKind_ZeroExpr: {
      // `(T){0}` zeroes every unspecified member, recursively for aggregates,
      // and is valid for scalars, pointers and structs alike. Arrays need
      // their dimensions after the base type, hence cg_array_type_cast_text.
      fprintf(cg->out, "(");
      if (n->type_query.type.kind == TypeKind_Array) cg_array_type_cast_text(cg, n->type_query.type);
      else fprintf(cg->out, "%.*s", str8_varg(c_type_from_typeref(cg, n->type_query.type)));
      fprintf(cg->out, "){0}");
    } break;
    case TypedNodeKind_HandleAlloc: {
      // `Prefix_pool_alloc(pool, data)`. `data` is always NULL -- there is no
      // `(handle-alloc Mesh value)` form -- so the slot comes back zeroed,
      // pool_init having used push_array_zero, rather than copy-initialized.
      String8 c_name = cg_symbol_name(cg, n->type_query.type.name);
      fprintf(cg->out, "%.*s_pool_alloc(&%.*s, NULL)", str8_varg(c_name),
              str8_varg(cg_handle_pool_global_name(cg, n->type_query.type.name)));
    } break;
    case TypedNodeKind_HandlePoolInit: {
      // `Prefix_pool_init(arena, pool, capacity)`, with the usual `&` on the
      // by-value arena.
      String8 c_name = cg_symbol_name(cg, n->handle_pool_init.type.name);
      fprintf(cg->out, "%.*s_pool_init(&(", str8_varg(c_name));
      cg_expr(cg, n->handle_pool_init.arena);
      fprintf(cg->out, "), &%.*s, (u32)(", str8_varg(cg_handle_pool_global_name(cg, n->handle_pool_init.type.name)));
      cg_expr(cg, n->handle_pool_init.capacity);
      fprintf(cg->out, "))");
    } break;
    case TypedNodeKind_MemberOffsetExpr: {
      fprintf(cg->out, "offsetof(%.*s, %.*s)", str8_varg(c_type_from_typeref(cg, n->member_offset.type)),
              str8_varg(c_mangle_name(ctx_scratch(), n->member_offset.field)));
    } break;
    case TypedNodeKind_ParseNumber: {
      // `(string-to-i32 s)` and friends. The runtime is compiled once ahead of
      // time and cannot know this program's dynamically-named AnonReturn
      // struct, so it returns a fixed shape per width
      // (`bbb_ParseI32Result{b32 ok; i32 value;}`), which a statement
      // expression converts into the `(bool T)` struct lower_parse_number
      // interned.
      const char* fn_name;
      const char* result_c_type;
      switch (n->parse_number.target_kind) {
        case TypeKind_I32: fn_name = "bbb_string_to_i32"; result_c_type = "bbb_ParseI32Result"; break;
        case TypeKind_I64: fn_name = "bbb_string_to_i64"; result_c_type = "bbb_ParseI64Result"; break;
        case TypeKind_U32: fn_name = "bbb_string_to_u32"; result_c_type = "bbb_ParseU32Result"; break;
        case TypeKind_U64: fn_name = "bbb_string_to_u64"; result_c_type = "bbb_ParseU64Result"; break;
        case TypeKind_F32: fn_name = "bbb_string_to_f32"; result_c_type = "bbb_ParseF32Result"; break;
        case TypeKind_F64: fn_name = "bbb_string_to_f64"; result_c_type = "bbb_ParseF64Result"; break;
        // f64 used to be the default arm, so a target width nobody wrote a
        // parser for would parse as a double and then be stored through the
        // wrong result struct.
        default:
          cg_internal_error(n->token, "no string-to-number parser for TypeKind %d",
                            (int)n->parse_number.target_kind);
          return;
      }
      String8 struct_c_name = cg_symbol_name(cg, n->parse_number.result_struct_name);
      fprintf(cg->out, "({ %s _3b_pr = %s(", result_c_type, fn_name);
      cg_expr(cg, n->parse_number.arg);
      fprintf(cg->out, "); (%.*s){ ._0 = _3b_pr.ok, ._1 = _3b_pr.value }; })", str8_varg(struct_c_name));
    } break;
    case TypedNodeKind_IndexOf: {
      // `(vector-index-of v x)` -- cg_call's `vector-contains?` loop, also
      // tracking the matching index and yielding the interned `(bool u64)`
      // struct instead of a bare bool.
      TypeRef vec_ty        = cg->resolved_types[n->index_of.vec];
      TypeRef elem_ty       = *vec_ty.pointee;
      String8 elem_c_ty     = c_type_from_typeref(cg, elem_ty);
      String8 struct_c_name = cg_symbol_name(cg, n->index_of.result_struct_name);
      fprintf(cg->out, "({ %.*s* _3b_v = ", str8_varg(elem_c_ty));
      cg_expr(cg, n->index_of.vec);
      fprintf(cg->out, "; %.*s _3b_needle = ", str8_varg(elem_c_ty));
      cg_expr(cg, n->index_of.needle);
      fprintf(cg->out, "; u64 _3b_n = bbb_dyn_count(_3b_v); u64 _3b_idx = _3b_n; b32 _3b_found = 0; "
                        "for (u64 _3b_i = 0; _3b_i < _3b_n; _3b_i += 1) { if (");
      cg_emit_field_eq_expr(cg, elem_ty, str8_lit("_3b_v[_3b_i]"), str8_lit("_3b_needle"));
      fprintf(cg->out, ") { _3b_found = 1; _3b_idx = _3b_i; break; } } "
                        "(%.*s){ ._0 = _3b_found, ._1 = _3b_idx }; })", str8_varg(struct_c_name));
    } break;
    case TypedNodeKind_CheckedMath: {
      // `(sqrt-checked x)` and friends call the same libm function the
      // unchecked builtin would, with the same "f" suffix dispatch, then report
      // `ok = isfinite(result)`. isfinite is type-generic, so unlike the call
      // itself it needs no f32/f64 split. It rejects NaN -- a genuine domain
      // error, such as a negative sqrt or out-of-range asin -- and infinities
      // alike, a checked variant's point being to return a usable number or
      // admit it cannot.
      b32     is_f32        = cg->resolved_types[n->checked_math.arg].kind == TypeKind_F32;
      String8 result_struct = is_f32 ? n->checked_math.f32_struct_name : n->checked_math.f64_struct_name;
      String8 struct_c_name = cg_symbol_name(cg, result_struct);
      fprintf(cg->out, "({ %s _3b_r = %.*s%s(", is_f32 ? "f32" : "f64",
              str8_varg(n->checked_math.libm_name), is_f32 ? "f" : "");
      cg_expr(cg, n->checked_math.arg);
      if (n->checked_math.arg2 != TYPED_NIL) {
        fprintf(cg->out, ", ");
        cg_expr(cg, n->checked_math.arg2);
      }
      fprintf(cg->out, "); (%.*s){ ._0 = isfinite(_3b_r), ._1 = _3b_r }; })", str8_varg(struct_c_name));
    } break;
    case TypedNodeKind_Identifier: {
      // cg_emit_folded_call's substitutions win first, innermost pushed first,
      // so a nested foldable call's param shadows an outer one of the same
      // name. The list is non-empty only while emitting an inlined callee body.
      b32 substituted = false;
      for (u32 si = cg->const_inline_sub_count; si > 0; si -= 1) {
        if (str8_match(cg->const_inline_subs[si - 1].name, n->ident.name, 0)) {
          fprintf(cg->out, "(");
          cg_expr(cg, cg->const_inline_subs[si - 1].expr);
          fprintf(cg->out, ")");
          substituted = true;
          break;
        }
      }
      if (substituted) break;

      String8* local = cg_scope_lookup(cg, n->ident.name);
      // A by-reference Vector parameter (see CgScopeEntry) reads as `(*name)`.
      // Every other use of the identifier -- len, nth, for, vector-push,
      // forwarding it to another Vector parameter -- routes through this same
      // path, so handling it once here covers all of them.
      if (local && cg_scope_is_vector_ref_param(cg, n->ident.name)) {
        fprintf(cg->out, "(*%.*s)", str8_varg(*local));
      } else {
        fprintf(cg->out, "%.*s", str8_varg(local ? *local : cg_symbol_name(cg, n->ident.name)));
      }
    } break;
    case TypedNodeKind_BinaryEq:
    case TypedNodeKind_BinaryNeq: {
      // String8 is a struct, so a bare `==` would compare pointers rather than
      // contents; str8_match with flags 0 compares exactly. `=`/`!=` are always
      // exact, for the same reason floats are not epsilon-compared: `match` and
      // everything else desugaring to `=` must behave like writing `=` by hand.
      // `(string-match a b flags)` is the escape hatch for anything fuzzier.
      //
      // A comparable struct -- the checker having required every field to be,
      // via type_ref_is_deep_comparable -- routes through its synthesized `_eq`
      // (cg_emit_struct_comparators). C has no operator overloading and `==` on
      // a struct means something else entirely, so that is not optional the way
      // it might look for a scalar. type_ref_is_comparable keeps anything else
      // needing special codegen from reaching here.
      TypeRef lhs_ty = cg->resolved_types[n->binary.lhs];
      if (lhs_ty.kind == TypeKind_String) {
        fprintf(cg->out, "%sbbb_str8_match(", n->kind == TypedNodeKind_BinaryNeq ? "!" : "");
        cg_expr(cg, n->binary.lhs);
        fprintf(cg->out, ", ");
        cg_expr(cg, n->binary.rhs);
        fprintf(cg->out, ", 0)");
      } else if (lhs_ty.kind == TypeKind_Named && cg_struct_lookup(cg, lhs_ty.name)) {
        fprintf(cg->out, "%s%.*s_eq(", n->kind == TypedNodeKind_BinaryNeq ? "!" : "",
                str8_varg(cg_symbol_name(cg, lhs_ty.name)));
        cg_expr(cg, n->binary.lhs);
        fprintf(cg->out, ", ");
        cg_expr(cg, n->binary.rhs);
        fprintf(cg->out, ")");
      } else {
        fprintf(cg->out, "(");
        cg_expr(cg, n->binary.lhs);
        fprintf(cg->out, " %s ", binary_op_symbol(n->kind));
        cg_expr(cg, n->binary.rhs);
        fprintf(cg->out, ")");
      }
    } break;
    case TypedNodeKind_BinaryLt:
    case TypedNodeKind_BinaryLe:
    case TypedNodeKind_BinaryGt:
    case TypedNodeKind_BinaryGe: {
      // Same shape as BinaryEq/Neq: `string` routes through the strcmp-style
      // three-way `bbb_str8_compare` against 0, and a comparable struct through
      // its synthesized `_cmp`, which orders lexicographically by declaration
      // field order (cg_emit_struct_comparators).
      TypeRef lhs_ty = cg->resolved_types[n->binary.lhs];
      if (lhs_ty.kind == TypeKind_String) {
        fprintf(cg->out, "(bbb_str8_compare(");
        cg_expr(cg, n->binary.lhs);
        fprintf(cg->out, ", ");
        cg_expr(cg, n->binary.rhs);
        fprintf(cg->out, ") %s 0)", binary_op_symbol(n->kind));
      } else if (lhs_ty.kind == TypeKind_Named && cg_struct_lookup(cg, lhs_ty.name)) {
        fprintf(cg->out, "(%.*s_cmp(", str8_varg(cg_symbol_name(cg, lhs_ty.name)));
        cg_expr(cg, n->binary.lhs);
        fprintf(cg->out, ", ");
        cg_expr(cg, n->binary.rhs);
        fprintf(cg->out, ") %s 0)", binary_op_symbol(n->kind));
      } else {
        fprintf(cg->out, "(");
        cg_expr(cg, n->binary.lhs);
        fprintf(cg->out, " %s ", binary_op_symbol(n->kind));
        cg_expr(cg, n->binary.rhs);
        fprintf(cg->out, ")");
      }
    } break;
    case TypedNodeKind_BinaryAdd:
    case TypedNodeKind_BinarySub:
    case TypedNodeKind_BinaryMul:
    case TypedNodeKind_BinaryDiv:
    case TypedNodeKind_BinaryMod:
    case TypedNodeKind_BinaryBitOr:
    case TypedNodeKind_BinaryBitAnd:
    case TypedNodeKind_BinaryBitXor:
    case TypedNodeKind_BinaryShl:
    case TypedNodeKind_BinaryShr:
    case TypedNodeKind_LogicalAnd:
    case TypedNodeKind_LogicalOr: {
      fprintf(cg->out, "(");
      cg_expr(cg, n->binary.lhs);
      fprintf(cg->out, " %s ", binary_op_symbol(n->kind));
      cg_expr(cg, n->binary.rhs);
      fprintf(cg->out, ")");
    } break;
    case TypedNodeKind_EnumAccess: {
      fprintf(cg->out, "%.*s_%.*s", str8_varg(cg_symbol_name(cg, n->enum_access.enum_name)),
              str8_varg(c_mangle_name(ctx_scratch(), n->enum_access.variant_name)));
    } break;
    case TypedNodeKind_IfExpr: {
      // A C ternary needs both arms to agree in type, and a diverging arm has
      // none -- see cg_if_branch, which supplies one where it matters.
      TypeRef if_ty = cg->resolved_types[idx];
      fprintf(cg->out, "(");
      cg_expr(cg, n->if_expr.cond);
      fprintf(cg->out, " ? ");
      cg_if_branch(cg, n->if_expr.then_branch, if_ty);
      fprintf(cg->out, " : ");
      if (n->if_expr.else_branch != TYPED_NIL) {
        cg_if_branch(cg, n->if_expr.else_branch, if_ty);
      } else {
        fprintf(cg->out, "(void)0 /* no else branch -- no value when the condition is false */");
      }
      fprintf(cg->out, ")");
    } break;
    case TypedNodeKind_Block: {
      cg_block_as_expr(cg, idx);
    } break;
    case TypedNodeKind_BreakExpr:
    case TypedNodeKind_ContinueExpr: {
      // The C keyword inside a statement expression, the same trick
      // TypedNodeKind_ReturnExpr uses: `({ })` is not a loop, so the jump
      // binds to the enclosing `while`/`for` this sits in, which is exactly
      // what the checker already verified exists. `match` lowers to nested
      // ifs and nothing here emits a C `switch`, so there is no other
      // construct a bare `break` could bind to by accident.
      //
      // The trailing `(void)0` is what keeps this a valid C expression, as it
      // does for a bare `(return)`. It carries no useful TYPE, though, so an
      // `if` pairing this with a valued arm needs the extra wrap cg_if_branch
      // adds -- a plain C ternary would reject the two arms as mismatched.
      //
      // Any `scratch` opened INSIDE the loop is rewound on the way out, since
      // the jump would otherwise skip its arena_temp_end. Only down to the
      // loop's own depth, though: a `scratch` wrapping the whole loop outlives
      // this jump. That floor is the one thing that differs from `return`.
      fprintf(cg->out, "({ ");
      cg_unwind_scratch_scopes(cg, cg->loop_scratch_depth);
      fprintf(cg->out, "%s; (void)0; })", n->kind == TypedNodeKind_BreakExpr ? "break" : "continue");
    } break;
    case TypedNodeKind_WhileExpr: {
      cg_while_expr(cg, idx);
    } break;
    case TypedNodeKind_ForCExpr: {
      cg_for_c_expr(cg, idx);
    } break;
    case TypedNodeKind_ForRangeExpr: {
      cg_for_range_expr(cg, idx);
    } break;
    case TypedNodeKind_ForEachExpr: {
      cg_foreach_expr(cg, idx);
    } break;
    case TypedNodeKind_ParallelExpr: {
      cg_parallel_expr(cg, idx);
    } break;
    case TypedNodeKind_ParallelForExpr: {
      cg_parallel_for_expr(cg, idx);
    } break;
    case TypedNodeKind_LetExpr: {
      cg_let_expr(cg, idx);
    } break;
    case TypedNodeKind_ScratchExpr: {
      cg_scratch_expr(cg, idx);
    } break;
    case TypedNodeKind_StructLiteral: {
      cg_struct_literal(cg, idx);
    } break;
    case TypedNodeKind_PushAlloc: {
      cg_push_alloc(cg, idx);
    } break;
    case TypedNodeKind_PushCopy: {
      cg_push_copy(cg, idx);
    } break;
    case TypedNodeKind_AllocExpr: {
      cg_alloc_expr(cg, idx);
    } break;
    case TypedNodeKind_DynPush: {
      cg_dyn_push(cg, idx);
    } break;
    case TypedNodeKind_CommitExpr: {
      cg_commit_expr(cg, idx);
    } break;
    case TypedNodeKind_SetExpr: {
      // The SetExpr's own resolved type is whichever target kind's declared
      // type applies -- pointee, element, field or identifier -- so it is
      // already the expected type cg_expr_decay_to wants, for all four kinds.
      TypeRef target_ty = cg->resolved_types[idx];
      if (target_ty.kind == TypeKind_Array) {
        // C has no whole-array assignment, so `(set (. r xs) other)` used to
        // reach gcc as `r.xs = other` and stop the build with "assignment to
        // expression with array type" -- a message about generated C, naming
        // no 3b. It is a byte copy instead. `set` is an expression whose value
        // is what was assigned, though, so the copy sits inside a statement
        // expression that hands the destination back.
        //
        // `_3b_setdst` is a pointer to the WHOLE array (`i32 (*p)[4]`), not to
        // its base type, for two reasons: the destination lvalue is then
        // evaluated exactly once however compound it is (`sizeof` does not
        // evaluate its operand), and `*_3b_setdst` keeps the full array type,
        // so a nested `[[T N] M]` result still indexes a row at a time rather
        // than a flattened element. Fixed name, same block-scope shadowing
        // argument as cg_parallel_for_expr's `__pr`.
        fprintf(cg->out, "({ ");
        cg_declare_array_ptr(cg, target_ty, str8_lit("_3b_setdst"));
        fprintf(cg->out, " = &(");
        cg_set_target_lvalue(cg, n);
        fprintf(cg->out, "); bbb_MemoryCopy(_3b_setdst, (");
        cg_expr(cg, n->set_expr.value);
        fprintf(cg->out, "), sizeof(*_3b_setdst)); *_3b_setdst; })");
        break;
      }
      fprintf(cg->out, "(");
      cg_set_target_lvalue(cg, n);
      fprintf(cg->out, " = ");
      cg_expr_decay_to(cg, n->set_expr.value, target_ty);
      fprintf(cg->out, ")");
    } break;
    case TypedNodeKind_IndexAccess: {
      b32 parens = cg_needs_parens_before_dot(cg, n->index_access.base);
      if (parens) fprintf(cg->out, "(");
      cg_expr(cg, n->index_access.base);
      if (parens) fprintf(cg->out, ")");
      fprintf(cg->out, "[");
      cg_expr(cg, n->index_access.index);
      fprintf(cg->out, "]");
    } break;
    case TypedNodeKind_ArrayLiteral: {
      cg_array_literal(cg, idx);
    } break;
    case TypedNodeKind_UnaryDeref: {
      fprintf(cg->out, "*");
      cg_expr(cg, n->unary.expr);
    } break;
    case TypedNodeKind_UnaryAddr: {
      fprintf(cg->out, "&");
      cg_expr(cg, n->unary.expr);
    } break;
    case TypedNodeKind_LogicalNot: {
      fprintf(cg->out, "!");
      cg_expr(cg, n->unary.expr);
    } break;
    case TypedNodeKind_UnaryBitNot: {
      fprintf(cg->out, "~");
      cg_expr(cg, n->unary.expr);
    } break;
    case TypedNodeKind_UnaryPos: {
      // A true no-op: no wrapping, not even parens. Unlike UnaryNeg there is no
      // token-merge risk, `+ +x` not being a C token.
      cg_expr(cg, n->unary.expr);
    } break;
    case TypedNodeKind_UnaryNeg: {
      // Parenthesized, unlike `~`/`!`, so nested negation `(- (- x))` cannot
      // merge into C's `--` decrement token.
      fprintf(cg->out, "-(");
      cg_expr(cg, n->unary.expr);
      fprintf(cg->out, ")");
    } break;
    case TypedNodeKind_CstrExpr: {
      // For a string-literal operand this emits a real C string literal rather
      // than a str8_lit(...) call: `"..."` is a constant expression, usable in
      // a top-level `val`'s initializer, and a call is not.
      TypedNode* inner = &cg->tast->nodes[n->unary.expr];
      if (inner->kind == TypedNodeKind_StringLiteral) {
        fprintf(cg->out, "(char*)\"");
        cg_write_c_escaped(cg, inner->string_lit.value);
        fprintf(cg->out, "\"");
      } else {
        fprintf(cg->out, "(char*)(");
        cg_expr(cg, n->unary.expr);
        fprintf(cg->out, ").str");
      }
    } break;
    case TypedNodeKind_StringLenExpr: {
      fprintf(cg->out, "(");
      cg_expr(cg, n->unary.expr);
      fprintf(cg->out, ").size");
    } break;
    case TypedNodeKind_ReturnExpr: {
      // A `return` inside a statement expression exits the enclosing function,
      // not just the `({ })` -- well-defined GNU C, and what makes `(return x)`
      // work however deeply nested it is.
      //
      // The dummy trailing value is not optional. Without it GCC types
      // `({ return X; })` as void, its trailing statement being a jump. That
      // breaks the motivating case `(if cond (return a) (return b))`: two
      // void-typed branches make the ternary void, and an enclosing
      // `return (void-typed-ternary);` is a hard error. The dummy is never
      // reached and carries the same type as the real return value, so it stays
      // compatible with whatever surrounds it.
      //
      // Bare `(return)`, valid only in a void fn, has no value to reuse, so it
      // emits `(void)0` rather than consulting cg_zero_value_for_type or
      // resolved_types, neither of which has an entry for TYPED_NIL.
      if (n->unary.expr == TYPED_NIL) {
        fprintf(cg->out, "({ ");
        cg_unwind_scratch_scopes(cg, 0);
        fprintf(cg->out, "return; (void)0; })");
        break;
      }
      TypeRef ret_ty = cg->resolved_types[n->unary.expr];
      // Returning from inside one or more `scratch` blocks has to rewind them
      // on the way out, and the returned value has to be computed BEFORE that
      // rewind -- it may well be what the scratch arena was for. So the value
      // lands in a local first. A function cannot return an array in C, hence
      // not in 3b either, so a plain `T name = ...` declarator covers every
      // type that can reach here.
      if (cg->scratch_depth != 0) {
        String8 c_ty = c_type_from_typeref(cg, ret_ty);
        fprintf(cg->out, "({ %.*s _3b_ret_value = ", str8_varg(c_ty));
        cg_expr(cg, n->unary.expr);
        fprintf(cg->out, "; ");
        cg_unwind_scratch_scopes(cg, 0);
        fprintf(cg->out, "return _3b_ret_value; ");
        cg_zero_value_for_type(cg, ret_ty);
        fprintf(cg->out, "; })");
        break;
      }
      fprintf(cg->out, "({ return ");
      cg_expr(cg, n->unary.expr);
      fprintf(cg->out, "; ");
      cg_zero_value_for_type(cg, ret_ty);
      fprintf(cg->out, "; })");
    } break;
    case TypedNodeKind_BinaryCast: {
      // n->binary.lhs is an Identifier holding the type name (lower_binary_cast),
      // so emitting it through cg_expr would print the source spelling verbatim
      // and skip the mangling and package-prefixing a Named type needs. The
      // checker's resolved cast type is used instead, as for sizeof and zero.
      fprintf(cg->out, "(%.*s)", str8_varg(c_type_from_typeref(cg, cg->resolved_types[idx])));
      cg_expr(cg, n->binary.rhs);
    } break;
    case TypedNodeKind_BinaryReinterpret: {
      // `(reinterpret Type value)`. Between two integers of the same width a
      // plain cast IS the bit pattern -- conversion is modulo 2^N, which on a
      // two's-complement target (C23 mandates it, and every target in build.c
      // was one already) is the identity on the bits -- so that form is emitted
      // as a cast. It reads better, and unlike the statement expression below
      // it is a constant expression, which is what a file-scope `val` needs:
      // `(val NOPTS i64 (reinterpret i64 0x8000000000000000u64))` is the shape
      // C's own AV_NOPTS_VALUE uses.
      //
      // Everything else goes through memcpy between two same-sized temps. A
      // cast is the wrong tool once a float is involved -- `(i32)f` converts
      // the value, not the bits -- and memcpy is the construct ISO C
      // guarantees copies the pattern, an aliased union read being only a
      // de-facto-portable extension. checker.c's equal-byte-width rule makes
      // this always exactly `sizeof(dst)`. Compilers reduce it to a register
      // move at -O1 and above. A statement expression is inherently grouped, so
      // unlike BinaryCast that path needs no entry in cg_needs_parens_before_dot.
      TypeRef dst_ty = cg->resolved_types[idx];
      TypeRef src_ty = cg->resolved_types[n->binary.rhs];
      if (cg_reinterpret_is_plain_cast(dst_ty, src_ty)) {
        fprintf(cg->out, "(%.*s)", str8_varg(c_type_from_typeref(cg, dst_ty)));
        cg_expr(cg, n->binary.rhs);
      } else {
        // A statement expression is not a constant expression, so at file scope
        // this would reach the C compiler as "braced-group within expression
        // allowed only inside a function" -- a message about generated code,
        // naming none of the 3b that produced it. Only the float-involving
        // forms can land here; the integer ones took the cast above.
        if (cg->in_static_init) {
          diag_error(n->token,
                     "`reinterpret` between %.*s and %.*s cannot initialize a top-level `val`/`var` "
                     "-- C requires a constant expression there, and reinterpreting a float's bits "
                     "compiles to a memcpy. Move it into a function, or initialize from the integer "
                     "bit pattern directly",
                     str8_varg(type_ref_display(ctx_scratch(), src_ty)), str8_varg(type_ref_display(ctx_scratch(), dst_ty)));
          cg->had_error = true;
        }
        fprintf(cg->out, "({ %.*s _3b_reinterpret_src = ", str8_varg(c_type_from_typeref(cg, src_ty)));
        cg_expr(cg, n->binary.rhs);
        fprintf(cg->out, "; %.*s _3b_reinterpret_dst; memcpy(&_3b_reinterpret_dst, &_3b_reinterpret_src, sizeof(_3b_reinterpret_dst)); _3b_reinterpret_dst; })",
                str8_varg(c_type_from_typeref(cg, dst_ty)));
      }
    } break;
    case TypedNodeKind_FieldAccess: {
      // Two independent reasons for the same parens, either sufficient:
      // `needs_deref`, where `.`/`&` insert their own `*` because the base's
      // resolved type is a pointer; and cg_needs_parens_before_dot, where an
      // explicit `(deref ptr)` base would otherwise emit `*ptr.field`, which C
      // parses as `*(ptr.field)`.
      b32 base_is_pointer = cg->resolved_types[n->field_access.base].kind == TypeKind_Pointer;
      b32 needs_deref     = n->field_access.auto_deref && base_is_pointer;
      b32 parens          = needs_deref || cg_needs_parens_before_dot(cg, n->field_access.base);
      if (parens) fprintf(cg->out, "(");
      if (needs_deref) fprintf(cg->out, "*");
      cg_expr(cg, n->field_access.base);
      if (parens) fprintf(cg->out, ")");
      fprintf(cg->out, ".%.*s", str8_varg(c_mangle_name(ctx_scratch(), n->field_access.field)));
    } break;
    case TypedNodeKind_Call: {
      cg_call(cg, idx);
    } break;
    default: {
      // Every statement-only TypedNodeKind lands here, so this default has to
      // exist -- but arriving at it means the checker typed something as an
      // expression that this switch never learned to emit. It used to write a
      // bare `0`, which compiles fine and is silently the answer.
      cg_internal_error(n->token, "no C expression for typed node kind %d", (int)n->kind);
    } break;
  }
}

// TODO: Figure out lane shutdown
// Emits every statement of a function body, whose brace the caller already
// opened. Shared by cg_function and cg_function_main: `main` needs its own
// signature and prelude but the same last-statement handling.
static void
cg_function_body_stmts(Codegen* cg, TypedIndex func_idx, b32 is_void, b32 is_main) {
  TypedNode* n    = &cg->tast->nodes[func_idx];
  TypedNode* body = &cg->tast->nodes[n->func.body];
  xassert(body->kind == TypedNodeKind_Block);
  foreach_index(i, body->block.stmt_count) {
    b32        is_last   = (i + 1 == body->block.stmt_count);
    TypedIndex stmt      = cg->tast->extra[body->block.stmt_first + i];
    TypedNode* stmt_node = &cg->tast->nodes[stmt];
    b32        is_decl   = stmt_node->kind == TypedNodeKind_ConstDecl || stmt_node->kind == TypedNodeKind_VarDecl;
    // Covers a bare `(return ...)` and anything that returns unconditionally
    // with no `else` to fall through to. The checker marks every diverging
    // expression Unresolved, and codegen runs only after it reports zero
    // errors, so Unresolved here can only mean divergence.
    b32 is_diverging = cg->resolved_types[stmt].kind == TypeKind_Unresolved;
    fprintf(cg->out, "  ");
    // A declaration cannot be returned -- `return i32 x = 5;` is not valid C --
    // so a trailing one emits bare rather than syntactically broken. (The
    // checker already rejects such a program: a block ending in a declaration
    // is void.) A diverging last statement skips the prefix too:
    // `return ({ return x; });` is redundant at best, and for a diverging `if`
    // with no `else` it is ill-typed C, one ternary arm being `(void)0`.
    if (is_last && !is_void && !is_decl && !is_diverging) {
      /* if (cg->has_parallel) fprintf(cg->out, "bbb_async_shutdown();\n  "); */
      if (is_main) fprintf(cg->out, "bbb_ctx_free();\n  ");
      fprintf(cg->out, "return ");
    }
    cg_stmt(cg, stmt);
    fprintf(cg->out, ";\n");
  }
}

// `(fn main ...)` becomes C's real entry point, `int main(int, char**)`,
// rather than an ordinary generated function -- see cg_symbol_name for why it
// is also never package-prefixed. check_main_signature has already guaranteed
// the shape: zero params, or exactly two, `i32` then `string*`.
//
// The raw C parameters are named `_3b_argc`/`_3b_argv` rather than the user's
// names. If the user picked "argc"/"argv", the obvious choice, then declaring
// their binding as `i32 argc = argc;` would read the uninitialized inner
// declaration instead of the C parameter -- the same bug cg_scope_reserve and
// cg_scope_register exist to prevent for ordinary shadowing.
//
// main is also responsible for two things every other generated function may
// assume: `ctx_init`, since ctx_scratch()/ctx_perm() need a live Context before
// any generated code touches them, and converting `char** argv` into a
// permanent-arena `String8*` so the body sees the `string*` it declared.
static void
cg_function_main(Codegen* cg, TypedIndex idx) {
  TypedNode* n = &cg->tast->nodes[idx];
  xassert(n->func.return_type.kind == TypeKind_I32); // enforced by check_main_signature

  fprintf(cg->out, "int main(int _3b_argc, char** _3b_argv) {\n");
  fprintf(cg->out, "  bbb_Context _3b_ctx;\n");
  fprintf(cg->out, "  bbb_ctx_init(&_3b_ctx, bbb_MB(16));\n");
  // No thread-pool spin-up here: it is lazy, triggered by
  // async_run_phase/lane_count on first use via the runtime prelude's
  // g_3b_lanes_once, so a program that never reaches a `parallel` block at
  // runtime never pays for one even if the declaring package is linked in.

  u64 mark = cg_scope_mark(cg);
  if (n->func.param_count == 2) {
    Param* argc_p = &cg->tast->params[n->func.param_first + 0];
    Param* argv_p = &cg->tast->params[n->func.param_first + 1];

    // These two are the one place a 3b parameter becomes a C LOCAL, which is
    // also the one place an unused parameter can draw -Wunused-variable --
    // every other function's unused param stays a param, and C says nothing
    // about those. Hence the `(void)` treatment ordinary params never need.
    String8 argc_c_name = cg_scope_reserve(cg, argc_p->name);
    fprintf(cg->out, "  i32 %.*s = _3b_argc;", str8_varg(argc_c_name));
    cg_mark_used_if_unread(cg, argc_c_name, argc_p->is_read);
    fprintf(cg->out, "\n");
    cg_scope_register(cg, argc_p->name, argc_c_name);

    String8 argv_c_name = cg_scope_reserve(cg, argv_p->name);
    fprintf(cg->out, "  bbb_String8* %.*s = bbb_push_array(bbb_ctx_perm(), bbb_String8, _3b_argc);\n", str8_varg(argv_c_name));
    fprintf(cg->out,
            "  for (i32 _3b_argv_i = 0; _3b_argv_i < _3b_argc; _3b_argv_i += 1) "
            "%.*s[_3b_argv_i] = bbb_str8_cstring(_3b_argv[_3b_argv_i]);\n",
            str8_varg(argv_c_name));
    cg_scope_register(cg, argv_p->name, argv_c_name);
  }

  cg_function_body_stmts(cg, idx, /*is_void*/ false, true); // main always returns i32 -- checked above
  fprintf(cg->out, "}\n\n");
  cg_scope_pop_to(cg, mark);
}

static void
cg_function_impl(Codegen* cg, TypedIndex idx) {
  TypedNode* n        = &cg->tast->nodes[idx];
  TypeKind   ret_kind = n->func.return_type.kind;

  // Only the root package's `main` -- or the single-file demo path's -- becomes
  // the real entry point. This gate must match cg_symbol_name's exactly: a
  // non-root package's "main" is an ordinary function of whatever signature it
  // likes, check_main_signature having skipped it too, and cg_function_main
  // assumes the enforced shape and would trip its xassert on anything else.
  if (str8_match_lit("main", n->func.name, 0) && (cg->is_root_package || cg->package_name.size == 0)) {
    cg_function_main(cg, idx);
    return;
  }

  if (n->is_private) fprintf(cg->out, "static ");
  fprintf(cg->out, "%.*s %.*s(", str8_varg(c_type_from_typeref(cg, n->func.return_type)),
          str8_varg(cg_symbol_name(cg, n->func.name)));
  if (n->func.param_count == 0) {
    fprintf(cg->out, "void");
  }
  u64 mark = cg_scope_mark(cg);
  foreach_index(i, n->func.param_count) {
    Param* p = &cg->tast->params[n->func.param_first + i];
    if (i != 0) fprintf(cg->out, ", ");
    String8 c_name = cg_scope_reserve(cg, p->name);
    cg_declare_param(cg, p->type, c_name);
    cg_scope_register(cg, p->name, c_name);
    if (p->type.kind == TypeKind_Vector) cg->scope[dyn_count(cg->scope) - 1].is_vector_ref_param = true;
  }
  fprintf(cg->out, ") {\n");

  b32 is_void = (ret_kind == TypeKind_Void);
  cg_function_body_stmts(cg, idx, is_void, false);
  fprintf(cg->out, "}\n\n");
  cg_scope_pop_to(cg, mark);
}

// Public entry point for emitting one top-level `fn`. With no `parallel` blocks
// in the package (Codegen.has_parallel) this is a passthrough to
// cg_function_impl.
//
// Otherwise it buffers the whole function -- signature and body -- into a
// memstream. A `parallel` block anywhere in the body, however deeply nested,
// must hoist a capture-struct typedef and trampoline ahead of this function's
// signature, which is impossible once that signature has gone to an
// append-only stream. So cg_parallel_expr writes them to
// cg->parallel_prelude_out, a second memstream set up here, and this flushes
// prelude before body to the real stream.
void
cg_function(Codegen* cg, TypedIndex idx) {
  if (!cg->has_parallel) {
    cg_function_impl(cg, idx);
    return;
  }

  FILE* real_out       = cg->out;
  FILE* saved_prelude  = cg->parallel_prelude_out;

  FILE* prelude_mem = mem_stream_open();
  FILE* body_mem    = mem_stream_open();

  cg->parallel_prelude_out = prelude_mem;
  cg->out                  = body_mem;
  cg_function_impl(cg, idx);
  cg->out                  = real_out;
  cg->parallel_prelude_out = saved_prelude;

  char* prelude_buf; u64 prelude_size;
  char* body_buf;    u64 body_size;
  mem_stream_close(prelude_mem, &prelude_buf, &prelude_size);
  mem_stream_close(body_mem, &body_buf, &body_size);
  if (prelude_size > 0) fwrite(prelude_buf, 1, prelude_size, real_out);
  fwrite(body_buf, 1, body_size, real_out);
  free(prelude_buf);
  free(body_buf);
}

// Just the signature, terminated with `;`. Emitted for every function up front
// (cg_program) so forward and mutually-recursive calls have the prototype C
// requires and 3b's own checker does not.
void
cg_function_prototype(Codegen* cg, TypedIndex idx) {
  TypedNode* n = &cg->tast->nodes[idx];
  if (str8_match_lit("main", n->func.name, 0) && (cg->is_root_package || cg->package_name.size == 0)) {
    // Matches cg_function_main's real C signature, not the source-level
    // `[argc i32 argv string*]` shape: nothing forward-calls `main`, but a
    // prototype disagreeing with the definition would be a C error if anything
    // did. Same root-package gate as cg_function and cg_symbol_name.
    fprintf(cg->out, "int main(int argc, char** argv);\n");
    return;
  }
  if (n->is_private) fprintf(cg->out, "static "); // must match the definition's linkage: a
                                                   // non-static prototype ahead of a static
                                                   // definition is a C error
  fprintf(cg->out, "%.*s %.*s(", str8_varg(c_type_from_typeref(cg, n->func.return_type)),
          str8_varg(cg_symbol_name(cg, n->func.name)));
  if (n->func.param_count == 0) {
    fprintf(cg->out, "void");
  }
  foreach_index(i, n->func.param_count) {
    Param* p = &cg->tast->params[n->func.param_first + i];
    if (i != 0) fprintf(cg->out, ", ");
    cg_declare_param(cg, p->type, c_mangle_name(ctx_scratch(), p->name));
  }
  fprintf(cg->out, ");\n");
}

// Forward-declared for the mutual recursion below: an anonymous member's fields
// can include a further anonymous member, arbitrarily deep.
static void cg_emit_anon_member_body(Codegen* cg, TypeRef field_type);
// Defined further down; cg_struct_decl calls it right after a struct's body.
static void cg_emit_struct_comparators(Codegen* cg, TypedIndex idx);

// Emits one struct or union field at any nesting depth: an ordinary
// `Type name;` declarator, or, for an anonymous (`_`) member, the nested type's
// body inlined with no name at all -- C11's anonymous-member syntax. Shared by
// cg_struct_decl's field loop and cg_emit_anon_member_body's nested one.
static void
cg_emit_struct_field(Codegen* cg, Param* f) {
  fprintf(cg->out, "  ");
  if (f->is_anon) cg_emit_anon_member_body(cg, f->type);
  else             cg_declare(cg, f->type, c_mangle_name(ctx_scratch(), f->name));
  fprintf(cg->out, ";\n");
}

// `struct { ... }` / `union { ... }` with no trailing name. This is what lets
// cg_expr's ordinary FieldAccess case reach a field through an anonymous member
// with no extra codegen: the checker validated the lookup via
// find_field_recursive, and the C compiler repeats the identical recursive
// lookup, the emitted struct having the same nested-anonymous shape.
static void
cg_emit_anon_member_body(Codegen* cg, TypeRef field_type) {
  StructEntry* se = NULL;
  foreach_index(i, dyn_count(cg->structs)) {
    if (str8_match(cg->structs[i].name, field_type.name, 0)) { se = &cg->structs[i]; break; }
  }
  xassert(se); // checker's check_struct_decl already required this to resolve
  TypedNode* decl = &cg->tast->nodes[se->decl];
  fprintf(cg->out, decl->struct_decl.is_union ? "union {\n" : "struct {\n");
  foreach_index(i, decl->struct_decl.field_count) {
    cg_emit_struct_field(cg, &cg->tast->params[decl->struct_decl.field_first + i]);
  }
  fprintf(cg->out, "}");
}

// An incomplete forward declaration, `typedef struct Name Name;`, emitted for
// every struct and union before any of their bodies. It lets a field of type
// `Other*` resolve whether `Other`'s definition comes earlier or later: the
// checker permits structs to reference each other in any order (check_program's
// two-pass registration), and a pointer field needs only the tag.
//
// The tag name matches cg_struct_decl's body below. C's tag namespace is
// separate from the typedef namespace, so it does not collide with the `Name`
// typedef, and C11 6.7p3 permits redeclaring a typedef name to the same type,
// so the two typedefs coexist.
void
cg_struct_forward_decl(Codegen* cg, TypedIndex idx) {
  TypedNode* n      = &cg->tast->nodes[idx];
  String8    c_name = cg_symbol_name(cg, n->struct_decl.name);
  fprintf(cg->out, n->struct_decl.is_union ? "typedef union %.*s %.*s;\n" : "typedef struct %.*s %.*s;\n",
          str8_varg(c_name), str8_varg(c_name));
}

// `typedef struct Name { ... } Name;`, or `union` for `(union ...)`. C's union
// already gives every member offset-0 storage, so is_union changes only the
// keyword. The body is tagged rather than anonymous so it matches the
// incomplete forward declaration from cg_struct_forward_decl.
void
cg_struct_decl(Codegen* cg, TypedIndex idx) {
  TypedNode* n      = &cg->tast->nodes[idx];
  String8    c_name = cg_symbol_name(cg, n->struct_decl.name);
  fprintf(cg->out, n->struct_decl.is_union ? "typedef union %.*s {\n" : "typedef struct %.*s {\n", str8_varg(c_name));
  foreach_index(i, n->struct_decl.field_count) {
    cg_emit_struct_field(cg, &cg->tast->params[n->struct_decl.field_first + i]);
  }
  fprintf(cg->out, "}");
  // GNU/Clang attributes rather than C11's `_Alignas`, since standard C has no
  // way to say "no inter-field padding" at all. `aligned` rides along on the
  // same attribute for consistency, though C11 could express it alone.
  if (n->struct_decl.is_packed)      fprintf(cg->out, " __attribute__((packed))");
  if (n->struct_decl.align_bytes > 0) fprintf(cg->out, " __attribute__((aligned(%u)))", n->struct_decl.align_bytes);
  fprintf(cg->out, " %.*s;\n\n", str8_varg(c_name));
  cg_emit_struct_comparators(cg, idx);
}

// `cg->structs` lookup by name. The comparator emitters below use it to decide
// whether a Named field type is a struct, needing a recursive `_eq`/`_cmp`, or
// something else such as an enum, which stays a bare C `==`/`<`.
static StructEntry*
cg_struct_lookup(Codegen* cg, String8 name) {
  foreach_index(i, dyn_count(cg->structs)) {
    if (str8_match(cg->structs[i].name, name, 0)) return &cg->structs[i];
  }
  return NULL;
}

// Forward-declared for the recursion: each of these recurses into itself (an
// array of arrays) and composes with the other through a struct field's own
// `_eq`/`_cmp`.
static void cg_emit_field_eq_expr(Codegen* cg, TypeRef t, String8 a_expr, String8 b_expr);
static void cg_emit_field_cmp_expr(Codegen* cg, TypeRef t, String8 a_expr, String8 b_expr);

// One C boolean expression for `a_expr == b_expr`, embeddable directly into a
// `return ... && ...;` chain. `t` is always a shape type_ref_is_deep_comparable
// (3b.c) permits: a scalar -- numeric, bool, char, pointer, or a Named type
// that is not a registered struct, assumed an enum -- becomes a bare `==`;
// `string` routes through `bbb_str8_match`; a Named struct recurses into its own
// `_eq`; and a fixed-size array loops over its elements inside a statement
// expression, which is what lets a real `for` live in a single expression rather
// than forcing `_eq`'s body to treat array fields as a separate shape.
static void
cg_emit_field_eq_expr(Codegen* cg, TypeRef t, String8 a_expr, String8 b_expr) {
  if (t.kind == TypeKind_String) {
    fprintf(cg->out, "bbb_str8_match(%.*s, %.*s, 0)", str8_varg(a_expr), str8_varg(b_expr));
  } else if (t.kind == TypeKind_Array) {
    fprintf(cg->out, "({ b32 _3b_eq = 1; for (u64 _3b_i = 0; _3b_i < %llu; _3b_i += 1) { if (!(",
            (unsigned long long)t.count);
    String8 a_elem = str8f(ctx_scratch(), "%.*s[_3b_i]", str8_varg(a_expr));
    String8 b_elem = str8f(ctx_scratch(), "%.*s[_3b_i]", str8_varg(b_expr));
    cg_emit_field_eq_expr(cg, *t.pointee, a_elem, b_elem);
    fprintf(cg->out, ")) { _3b_eq = 0; break; } } _3b_eq; })");
  } else if (t.kind == TypeKind_Named && cg_struct_lookup(cg, t.name)) {
    fprintf(cg->out, "%.*s_eq(%.*s, %.*s)", str8_varg(cg_symbol_name(cg, t.name)), str8_varg(a_expr), str8_varg(b_expr));
  } else {
    fprintf(cg->out, "(%.*s == %.*s)", str8_varg(a_expr), str8_varg(b_expr));
  }
}

// cg_emit_field_eq_expr's shape, but yielding a strcmp-style three-way `int`
// instead of a boolean. Backs the `_cmp` behind `<`/`<=`/`>`/`>=` on a struct.
static void
cg_emit_field_cmp_expr(Codegen* cg, TypeRef t, String8 a_expr, String8 b_expr) {
  if (t.kind == TypeKind_String) {
    fprintf(cg->out, "bbb_str8_compare(%.*s, %.*s)", str8_varg(a_expr), str8_varg(b_expr));
  } else if (t.kind == TypeKind_Array) {
    fprintf(cg->out, "({ i32 _3b_c = 0; for (u64 _3b_i = 0; _3b_i < %llu; _3b_i += 1) { _3b_c = (",
            (unsigned long long)t.count);
    String8 a_elem = str8f(ctx_scratch(), "%.*s[_3b_i]", str8_varg(a_expr));
    String8 b_elem = str8f(ctx_scratch(), "%.*s[_3b_i]", str8_varg(b_expr));
    cg_emit_field_cmp_expr(cg, *t.pointee, a_elem, b_elem);
    fprintf(cg->out, "); if (_3b_c != 0) break; } _3b_c; })");
  } else if (t.kind == TypeKind_Named && cg_struct_lookup(cg, t.name)) {
    fprintf(cg->out, "%.*s_cmp(%.*s, %.*s)", str8_varg(cg_symbol_name(cg, t.name)), str8_varg(a_expr), str8_varg(b_expr));
  } else {
    fprintf(cg->out, "((%.*s < %.*s) ? -1 : (%.*s > %.*s) ? 1 : 0)",
            str8_varg(a_expr), str8_varg(b_expr), str8_varg(a_expr), str8_varg(b_expr));
  }
}

// Synthesizes `<Name>_eq` and `<Name>_cmp` right after `Name`'s struct
// definition, for every struct type_ref_is_deep_comparable (3b.c) accepts --
// the same predicate checker.c uses to decide whether `=`/`<` type-check on it.
//
// They are `static inline` not for inlining but because an unused `static
// inline` function does not trigger -Wunused-function, so every comparable
// struct can get one without tracking whether it was ever compared, the way
// Map/Set instantiations must.
//
// `_cmp` orders lexicographically by declaration order, first differing field
// deciding, like a derived tuple ordering elsewhere (Rust's derive(Ord),
// Python tuple comparison).
static void
cg_emit_struct_comparators(Codegen* cg, TypedIndex idx) {
  TypedNode* n = &cg->tast->nodes[idx];
  if (n->struct_decl.is_union) return; // ambiguous which member is even active
  TypeRef self = {0};
  self.kind = TypeKind_Named;
  self.name = n->struct_decl.name;
  if (!type_ref_is_deep_comparable(cg->tast, cg->structs, dyn_count(cg->structs), self)) return;
  String8 c_name = cg_symbol_name(cg, n->struct_decl.name);

  fprintf(cg->out, "static inline b32 %.*s_eq(%.*s a, %.*s b) {\n  return ",
          str8_varg(c_name), str8_varg(c_name), str8_varg(c_name));
  if (n->struct_decl.field_count == 0) {
    fprintf(cg->out, "1");
  } else {
    foreach_index(i, n->struct_decl.field_count) {
      Param* f = &cg->tast->params[n->struct_decl.field_first + i];
      if (i > 0) fprintf(cg->out, "\n      && ");
      String8 a_field = str8f(ctx_scratch(), "a.%.*s", str8_varg(c_mangle_name(ctx_scratch(), f->name)));
      String8 b_field = str8f(ctx_scratch(), "b.%.*s", str8_varg(c_mangle_name(ctx_scratch(), f->name)));
      cg_emit_field_eq_expr(cg, f->type, a_field, b_field);
    }
  }
  fprintf(cg->out, ";\n}\n\n");

  fprintf(cg->out, "static inline i32 %.*s_cmp(%.*s a, %.*s b) {\n  i32 c;\n",
          str8_varg(c_name), str8_varg(c_name), str8_varg(c_name));
  foreach_index(i, n->struct_decl.field_count) {
    Param* f = &cg->tast->params[n->struct_decl.field_first + i];
    String8 a_field = str8f(ctx_scratch(), "a.%.*s", str8_varg(c_mangle_name(ctx_scratch(), f->name)));
    String8 b_field = str8f(ctx_scratch(), "b.%.*s", str8_varg(c_mangle_name(ctx_scratch(), f->name)));
    fprintf(cg->out, "  c = ");
    cg_emit_field_cmp_expr(cg, f->type, a_field, b_field);
    fprintf(cg->out, "; if (c != 0) return c;\n");
  }
  fprintf(cg->out, "  return 0;\n}\n\n");
}

// Does a field of type `t` require another declaration's full body ahead of
// this one, rather than just its forward typedef? True for a by-value struct
// field, a fixed-size array of them (elements stored inline, so the same
// requirement one level down), and a Map/Set field, whose monomorphized struct
// is embedded by value and named by the same hashtable_mangled_name a
// HashTableInstanceDecl uses. False for Pointer and Handle fields, which need
// only the tag.
static b32
cg_field_requires_complete_type(TypeRef t, String8* out_name) {
  if (t.kind == TypeKind_Named) { *out_name = t.name; return true; }
  if (t.kind == TypeKind_Array)  return cg_field_requires_complete_type(*t.pointee, out_name);
  if (t.kind == TypeKind_Map)    { *out_name = hashtable_mangled_name(ctx_perm(), *t.map_key, t.pointee); return true; }
  if (t.kind == TypeKind_Set)    { *out_name = hashtable_mangled_name(ctx_perm(), *t.pointee, NULL);       return true; }
  return false;
}

// A StructDecl's bare 3b name, or a HashTableInstanceDecl's mangled C name --
// whichever identity cg_field_requires_complete_type's `out_name` must match to
// find this decl in a mixed batch. Bare 3b names and `Map_K_V`-shaped mangled
// ones do not collide, so one lookup can search a mixed batch for either.
static String8
cg_decl_identity_name(TypedNode* n) {
  if (n->kind == TypedNodeKind_HashTableInstanceDecl) return n->hashtable_instance.mangled_name;
  return n->struct_decl.name; // StructDecl
}

typedef struct CgStructSortCtx {
  Codegen*    cg;
  TypedIndex* stmts; // mixed: StructDecl and/or HashTableInstanceDecl nodes
  u64         count;
  u8*         state; // 0 unvisited, 1 visiting (cycle guard), 2 done
  TypedIndex* out;
  u64         out_count;
} CgStructSortCtx;

static void
cg_topo_visit_struct(CgStructSortCtx* ctx, u64 i) {
  if (ctx->state[i] != 0) return; // done, or already on the stack: by-value fields cannot form a
                                    // cycle in valid code, so stop rather than loop on bad input
  ctx->state[i] = 1;
  TypedNode* decl = &ctx->cg->tast->nodes[ctx->stmts[i]];
  // A HashTableInstanceDecl depends on its value type, and on a Map only: a
  // Set's key is always numeric, bool, char or string per lower_hashtable_type,
  // never a struct. A by-value struct value must be complete first, the same
  // requirement an ordinary field has, since cg_hashtable_instance_decl embeds
  // it by value.
  if (decl->kind == TypedNodeKind_HashTableInstanceDecl) {
    if (!decl->hashtable_instance.is_set && decl->hashtable_instance.value_type.kind == TypeKind_Named) {
      String8 dep_name = decl->hashtable_instance.value_type.name;
      foreach_index(k, ctx->count) {
        if (k == i) continue;
        if (str8_match(cg_decl_identity_name(&ctx->cg->tast->nodes[ctx->stmts[k]]), dep_name, 0)) {
          cg_topo_visit_struct(ctx, k);
          break;
        }
      }
    }
  } else {
    foreach_index(j, decl->struct_decl.field_count) {
      Param* f = &ctx->cg->tast->params[decl->struct_decl.field_first + j];
      String8 dep_name;
      if (!cg_field_requires_complete_type(f->type, &dep_name)) continue;
      foreach_index(k, ctx->count) {
        if (k == i) continue;
        if (str8_match(cg_decl_identity_name(&ctx->cg->tast->nodes[ctx->stmts[k]]), dep_name, 0)) {
          cg_topo_visit_struct(ctx, k);
          break;
        }
      }
    }
  }
  ctx->state[i] = 2;
  ctx->out[ctx->out_count] = ctx->stmts[i];
  ctx->out_count += 1;
}

// Reorders `stmts` in place so every entry follows any other entry in the same
// batch that it embeds by value -- C requiring a complete type for a struct
// member. cg_struct_forward_decl's typedefs do not cover this: they only make
// pointer and handle fields order-independent, and a Map/Set instantiation has
// no forward-declaration mechanism at all.
//
// A stable topological sort, falling back to the original relative order where
// two entries have no dependency. `stmts` is normally all StructDecls; only
// cg_program_header passes a batch mixed with HashTableInstanceDecls.
static void
cg_topo_sort_structs(Codegen* cg, TypedIndex* stmts, u64 count) {
  if (count == 0) return;
  ArenaTemp temp = arena_temp_begin(ctx_scratch());
  CgStructSortCtx ctx = {0};
  ctx.cg    = cg;
  ctx.stmts = stmts;
  ctx.count = count;
  ctx.state = push_array_zero(ctx_scratch(), u8, count);
  ctx.out   = push_array(ctx_scratch(), TypedIndex, count);
  foreach_index(i, count) cg_topo_visit_struct(&ctx, i);
  MemoryCopy(stmts, ctx.out, count * sizeof(TypedIndex));
  arena_temp_end(&temp);
}

// The one place the pool global's name is computed, shared by
// cg_handle_pool_storage and every handle builtin's call site, so they cannot
// drift. `struct_name` is the bare 3b name; cg_symbol_name is applied here.
static String8
cg_handle_pool_global_name(Codegen* cg, String8 struct_name) {
  String8 c_name = cg_symbol_name(cg, struct_name);
  return str8f(ctx_scratch(), "bbb_%.*s_pool", str8_varg(c_name));
}

// `typedef Handle T##Handle;` alone, emitted alongside cg_struct_forward_decl
// before any struct body, so a `T^` field resolves regardless of source order.
// cg_handle_pool_decl's full DEFINE_HANDLE_POOL cannot be hoisted this way: it
// also defines pool_init/alloc/get/free, which need `sizeof(T)` and so must
// follow every struct body. `Handle` is a complete 2-u32 struct regardless of
// T, so the typedef alone is safe here. DEFINE_HANDLE_POOL redeclares it later,
// which C11 6.7p3 permits.
void
cg_handle_pool_typedef(Codegen* cg, TypedIndex idx) {
  TypedNode* n      = &cg->tast->nodes[idx];
  String8    c_name = cg_symbol_name(cg, n->handle_pool_decl.type_name);
  fprintf(cg->out, "typedef bbb_Handle %.*sHandle;\n", str8_varg(c_name));
}

// `(handle Name)` -- base.h's `DEFINE_HANDLE_POOL(T, T)`, expanding to the
// `T##Handle`/`T##Pool` typedefs and the pool_init/alloc/get/free/handle_valid
// functions, with the struct's mangled C name for both T and Prefix. Goes
// wherever the struct's own typedef does, header if public and .c if private,
// since it is what makes `T^` a real C type. The pool's storage is separate and
// always private -- see cg_handle_pool_storage.
void
cg_handle_pool_decl(Codegen* cg, TypedIndex idx) {
  TypedNode* n      = &cg->tast->nodes[idx];
  String8    c_name = cg_symbol_name(cg, n->handle_pool_decl.type_name);
  fprintf(cg->out, "bbb_DEFINE_HANDLE_POOL(%.*s, %.*s)\n\n", str8_varg(c_name), str8_varg(c_name));
}

// The pool's backing storage, always private to the declaring package's .c even
// when the `handle` decl is public: only fn bodies compiled into that .c touch
// it directly, whether or not the handle type they pass around is public. See
// TypedNodeKind_HandlePoolDecl in 3b.h. Emitted from cg_program and
// cg_program_parallel, never the header, and not subject to the public/private
// split cg_handle_pool_decl's emission uses.
void
cg_handle_pool_storage(Codegen* cg, TypedIndex idx) {
  TypedNode* n      = &cg->tast->nodes[idx];
  String8    c_name = cg_symbol_name(cg, n->handle_pool_decl.type_name); // the T DEFINE_HANDLE_POOL used
  fprintf(cg->out, "static %.*sPool %.*s;\n\n",
          str8_varg(c_name), str8_varg(cg_handle_pool_global_name(cg, n->handle_pool_decl.type_name)));
}

// `typedef ExistingType NewName;`. 3b writes `(alias NewName ExistingType)`
// name-first; the emitted C uses C's own order. Goes through cg_declare rather
// than a plain type string so aliasing an array works: `typedef i32 Buffer[64];`.
//
// A pinned C spelling -- `(alias GLint64 i64 "GLint64")` -- emits
// `typedef GLint64 gl_GLint64;` instead of `typedef i64 gl_GLint64;`. Translated
// bindings need this because a C typedef is transparent: `gl_GLint64*` is
// compatible with the `GLint64*` the real function wants only when the two
// bottom out in the same C type. 3b's i64/u64 are `long long` (bbb_prelude.h
// spells them that way so Windows' LLP64 `long` cannot shrink them to 32 bits),
// while GLint64, size_t and cgltf_size are all `long` on LP64 -- same width,
// same representation, but a distinct C type, which is a pointer-compatibility
// warning at every FFI call site. Naming the C type directly sidesteps the
// question. The 3b side keeps the primitive it was declared with, so the
// checker, the bytecode VM and every numeric rule still see a plain integer.
//
// This is sound only because the pinned name is in scope in the generated C --
// translated packages `include-first` the header that defines it -- and because
// the pinned type really is the alias's own C type. `3b translate` is the only
// thing that writes the third operand, and it pins a typedef to its own C name.
void
cg_alias_decl(Codegen* cg, TypedIndex idx) {
  TypedNode* n = &cg->tast->nodes[idx];
  if (n->alias_decl.c_name.size > 0) {
    fprintf(cg->out, "typedef %.*s %.*s;\n\n", str8_varg(n->alias_decl.c_name),
            str8_varg(cg_symbol_name(cg, n->alias_decl.name)));
    return;
  }
  fprintf(cg->out, "typedef ");
  cg_declare(cg, n->alias_decl.type, cg_symbol_name(cg, n->alias_decl.name));
  fprintf(cg->out, ";\n\n");
}

// `typedef enum { Name_Variant = value, ... [Name_Count | Name_All] } Name;`
//
// Default values are assigned here rather than at lowering time: codegen is the
// only consumer of the numeric values, the checker needing variant identity
// only.
//
// `enum` is sequential and C-style -- an explicit value resets the counter and
// auto-assignment resumes at `value + 1`, so `A, B = 5, C` gives C == 6.
//
// `flags` advances the bit position by one per variant whether or not that
// variant was explicit, so an override never shifts later auto-assigned bits.
void
cg_enum_decl(Codegen* cg, TypedIndex idx) {
  TypedNode* n             = &cg->tast->nodes[idx];
  String8    mangled_name  = cg_symbol_name(cg, n->enum_decl.name);
  fprintf(cg->out, "typedef enum {\n");
  i64 next_auto = 0; // enum: next sequential value. flags: next bit position (not yet shifted).
  i64 all_mask  = 0; // flags only: running OR of every variant's final value.
  foreach_index(i, n->enum_decl.variant_count) {
    EnumVariant* v = &cg->tast->enum_variants[n->enum_decl.variant_first + i];
    i64 value;
    if (v->has_explicit_value) {
      value = v->value;
    } else if (n->enum_decl.is_flags) {
      value = ((i64)1) << next_auto;
    } else {
      value = next_auto;
    }
    fprintf(cg->out, "  %.*s_%.*s = %lld,\n", str8_varg(mangled_name),
            str8_varg(c_mangle_name(ctx_scratch(), v->name)), (long long)value);
    if (n->enum_decl.is_flags) {
      all_mask  |= value;
      next_auto += 1;
    } else {
      next_auto = value + 1;
    }
  }
  if (n->enum_decl.is_flags) {
    fprintf(cg->out, "  %.*s_All = %lld,\n", str8_varg(mangled_name), (long long)all_mask);
  } else {
    fprintf(cg->out, "  %.*s_Count = %lld,\n", str8_varg(mangled_name), (long long)next_auto);
  }
  fprintf(cg->out, "} %.*s;\n\n", str8_varg(mangled_name));
}

// A `(Map K V)`/`(Set T)` instantiation: a key-specific hash and equality pair,
// then one bbb_DEFINE_HASHMAP/bbb_DEFINE_HASHSET (runtime/bbb_hashtable.h)
// expanding to the struct and its set/get/remove/contains functions. The key
// type is already restricted by lower_hashtable_type to numeric primitives,
// bool, char or `string` -- exactly the two hash shapes below.
//
// No forward-declaration pass is needed, unlike ordinary structs: this is
// emitted after every real struct is defined, so a Map whose value type is some
// other struct always sees it complete.
//
// The include guard is keyed by the instantiation name, which
// cg_hashtable_c_name deliberately leaves unprefixed. Two packages each using
// `(Map string i32)` emit this into their own header, and a third importing
// both would otherwise see the same static function bodies twice -- a
// redefinition error. Generation being deterministic, whichever copy is
// included first wins and the rest are no-ops.
static void
cg_hashtable_instance_decl(Codegen* cg, TypedIndex idx) {
  TypedNode* n    = &cg->tast->nodes[idx];
  b32        is_set = n->hashtable_instance.is_set;
  String8    name  = cg_hashtable_c_name(cg, n->hashtable_instance.key_type,
                                          is_set ? NULL : &n->hashtable_instance.value_type);
  String8    guard = c_mangle_name_upper(ctx_scratch(), name);
  String8    key_c_ty = c_type_from_typeref(cg, n->hashtable_instance.key_type);
  b32        is_string_key = n->hashtable_instance.key_type.kind == TypeKind_String;

  fprintf(cg->out, "#ifndef BBB_HT_%.*s_DEFINED\n", str8_varg(guard));
  fprintf(cg->out, "#define BBB_HT_%.*s_DEFINED\n", str8_varg(guard));
  fprintf(cg->out, "static inline u64 %.*s_hash_key(%.*s key) { return %s; }\n",
          str8_varg(name), str8_varg(key_c_ty),
          is_string_key ? "bbb_str8_hash(key)" : "bbb_hash_mix_u64((u64)key)");
  fprintf(cg->out, "static inline b32 %.*s_keys_equal(%.*s a, %.*s b) { return %s; }\n",
          str8_varg(name), str8_varg(key_c_ty), str8_varg(key_c_ty),
          is_string_key ? "bbb_str8_match(a, b, 0)" : "a == b");

  if (is_set) {
    fprintf(cg->out, "bbb_DEFINE_HASHSET(%.*s, %.*s, %.*s_hash_key, %.*s_keys_equal)\n\n",
            str8_varg(key_c_ty), str8_varg(name), str8_varg(name), str8_varg(name));
  } else {
    String8 value_c_ty = c_type_from_typeref(cg, n->hashtable_instance.value_type);
    fprintf(cg->out, "bbb_DEFINE_HASHMAP(%.*s, %.*s, %.*s, %.*s_hash_key, %.*s_keys_equal)\n",
            str8_varg(key_c_ty), str8_varg(value_c_ty), str8_varg(name), str8_varg(name), str8_varg(name));
  }
  fprintf(cg->out, "#endif\n\n");
}

void
cg_toplevel(Codegen* cg, TypedIndex idx) {
  if (idx == TYPED_NIL) {
    fprintf(cg->out, "// <lowering error -- see stderr>\n\n");
    return;
  }
  TypedNode* n = &cg->tast->nodes[idx];
  if (n->is_imported) return; // a stand-in for another package's public decl
  switch (n->kind) {
    case TypedNodeKind_ConstDecl: {
      // `static` only when explicitly `private`; everything else is externally
      // linkable by default.
      if (n->is_private) fprintf(cg->out, "static ");
      cg_declare_val(cg, n->const_decl.type, cg_symbol_name(cg, n->const_decl.name));
      fprintf(cg->out, " = ");
      if (n->const_decl.init == TYPED_NIL) {
        fprintf(cg->out, "{0}"); // omitted initializer: array types only
      } else {
        // See Codegen.in_static_init: this is file scope, so the whole
        // initializer has to come out as a C constant expression.
        cg->in_static_init = true;
        cg_init_value(cg, n->const_decl.init, n->const_decl.type);
        cg->in_static_init = false;
      }
      fprintf(cg->out, ";\n\n");
    } break;
    case TypedNodeKind_VarDecl: {
      if (n->is_private) fprintf(cg->out, "static ");
      cg_declare(cg, n->var_decl.type, cg_symbol_name(cg, n->var_decl.name));
      fprintf(cg->out, " = ");
      if (n->var_decl.init == TYPED_NIL) {
        fprintf(cg->out, "{0}"); // omitted initializer: array types only
      } else {
        cg->in_static_init = true;
        cg_init_value(cg, n->var_decl.init, n->var_decl.type);
        cg->in_static_init = false;
      }
      fprintf(cg->out, ";\n\n");
    } break;
    case TypedNodeKind_FunctionDecl: {
      if (n->func.body != TYPED_NIL) cg_function(cg, idx); // bodyless means extern
    } break;
    case TypedNodeKind_StructDecl: break; // emitted up front by cg_program
    case TypedNodeKind_EnumDecl: break;   // emitted up front by cg_program
    case TypedNodeKind_AliasDecl: break;  // emitted up front by cg_program
    case TypedNodeKind_HandlePoolDecl: break; // emitted up front by cg_program
    case TypedNodeKind_HashTableInstanceDecl: break; // emitted up front by cg_program
    default: {
      // A whole top-level declaration with no emission. The comment this used
      // to write in its place left the .c file valid, so the first sign of a
      // dropped declaration was a link error naming a mangled symbol -- or, for
      // anything that only affects layout or initialization, no sign at all.
      cg_internal_error(n->token, "no C output for top-level node kind %d", (int)n->kind);
    } break;
  }
}

// ~~ The generated runtime.
//
// Support code every generated package's .c includes, named "3b_runtime" to
// keep it distinct from the compiler's own 3b.h/3b.c. It lives as editable C
// source under runtime/bbb_*.h and runtime/bbb_*.c in the compiler's source
// tree, every name bbb_-prefixed so it stays clear of whatever C/FFI code the
// generated program links against.
//
// Those files are baked into the `3b` binary at compiler-build time (see
// tools/embed_runtime.c and the Makefile rule producing runtime_embed.h's
// g_embed_* constants), so `3b` never reads runtime/ off disk once built.
// cg_write_runtime_header/cg_write_runtime_source write those constants, in
// dependency order, into a self-contained 3b_runtime.h/.c pair -- split into
// prototypes and definitions like any shared C library, so every package's .c
// can include the header without duplicating non-static definitions.

// Each g_embed_* constant is a NULL-terminated array of one-line string
// literals (tools/embed_runtime.c), written straight through in order.
static void
cg_fputs_lines(FILE* out, const char* const* lines) {
  for (; *lines; lines++) fputs(*lines, out);
}

void
cg_write_runtime_header(FILE* out) {
  fputs("// Generated by 3b -- do not edit by hand.\n", out);
  fputs("#ifndef BBB_3B_RUNTIME_H\n", out);
  fputs("#define BBB_3B_RUNTIME_H\n", out);
  fputs("\n", out);
  fputs("#ifndef _GNU_SOURCE\n", out);
  fputs("# define _GNU_SOURCE // MAP_ANONYMOUS (mman.h) -- needed regardless of the including project's own CFLAGS\n", out);
  fputs("#endif\n", out);
  fputs("\n", out);
  fputs("#include <stdarg.h>\n", out);
  fputs("#include <stdbool.h>\n", out);
  fputs("#include <stddef.h>\n", out);
  fputs("#include <stdint.h>\n", out);
  fputs("#include <stdio.h>\n", out);
  fputs("#include <stdlib.h>\n", out);
  fputs("#include <string.h>\n", out);
  fputs("#include <math.h>\n", out); // sinf/cosf/tanf/... -- see cg_call's trig builtins
  fputs("#include <sys/stat.h>\n", out); // bbb_os_file_mtime (bbb_file.c) -- portable to Windows via mingw too
  fputs("#if defined(_WIN32)\n", out);
  fputs("# define WIN32_LEAN_AND_MEAN\n", out);
  fputs("# include <windows.h>\n", out);
  fputs("#else\n", out);
  fputs("# include <sys/mman.h>\n", out);
  fputs("# include <unistd.h>\n", out);
  fputs("#endif\n", out);
  fputs("\n", out);
  fputs("// Assembled from runtime/bbb_*.h in the compiler's source tree (see\n", out);
  fputs("// cg_write_runtime_header in codegen.c): arenas, strings, dynamic arrays,\n", out);
  fputs("// handle pools, threads, and a few scalar-math macros. Every name is\n", out);
  fputs("// bbb_-prefixed to stay clear of whatever C/FFI code this program links\n", out);
  fputs("// against. Written fresh into each build's output directory, so output/ is\n", out);
  fputs("// self-contained; the contents are baked into the compiler binary.\n", out);
  fputs("\n", out);

  cg_fputs_lines(out, g_embed_runtime_bbb_prelude_h); fputs("\n", out);
  cg_fputs_lines(out, g_embed_runtime_bbb_arena_h);   fputs("\n", out);
  cg_fputs_lines(out, g_embed_runtime_bbb_thread_h);  fputs("\n", out);
  cg_fputs_lines(out, g_embed_runtime_bbb_context_h); fputs("\n", out);
  cg_fputs_lines(out, g_embed_runtime_bbb_handle_h);  fputs("\n", out);
  cg_fputs_lines(out, g_embed_runtime_bbb_string_h);  fputs("\n", out);
  cg_fputs_lines(out, g_embed_runtime_bbb_file_h);    fputs("\n", out);
  cg_fputs_lines(out, g_embed_runtime_bbb_os_h);      fputs("\n", out); // needs bbb_Arena/bbb_String8, after both
  cg_fputs_lines(out, g_embed_runtime_bbb_hashtable_h); fputs("\n", out); // needs bbb_Arena + bbb_str8_hash, after both

  fputs("#endif\n", out);
}

void
cg_write_runtime_source(FILE* out) {
  fputs("// Generated by 3b -- do not edit by hand.\n", out);
  fputs("#include \"3b_runtime.h\"\n", out);
  fputs("#if !defined(_WIN32)\n", out);
  fputs("# include <pthread.h>\n", out);
  fputs("# include <dirent.h>\n", out); // bbb_os_list_dir (bbb_file.c) -- windows.h already covers its own side
  fputs("#endif\n", out);
  fputs("#include <stdatomic.h>\n", out);
  fputs("#include <time.h>\n", out);
  fputs("\n", out);

  // Order matters: bbb_context.c defines the file-static bbb_tls_ctx that
  // bbb_thread.c's lane functions read, and everything after bbb_arena.c uses
  // bbb_Arena. It all lands in one translation unit, so the concatenation order
  // is what makes that legal.
  cg_fputs_lines(out, g_embed_runtime_bbb_arena_c);   fputs("\n", out);
  cg_fputs_lines(out, g_embed_runtime_bbb_context_c); fputs("\n", out);
  cg_fputs_lines(out, g_embed_runtime_bbb_thread_c);  fputs("\n", out);
  cg_fputs_lines(out, g_embed_runtime_bbb_string_c);  fputs("\n", out);
  cg_fputs_lines(out, g_embed_runtime_bbb_file_c);    fputs("\n", out);
  cg_fputs_lines(out, g_embed_runtime_bbb_os_c);      fputs("\n", out);
}

// This package's public surface: a prototype-only header, included both by the
// package's own .c (cg_program) and by every importing package. Anything
// `private` stays entirely inside the .c.
void
cg_program_header(Codegen* cg, TypedIndex root) {
  TypedNode* n = &cg->tast->nodes[root];
  xassert(n->kind == TypedNodeKind_Block);
  cg->program_root = root; // what cg_symbol_name resolves public names against

  String8 guard = c_mangle_name_upper(ctx_scratch(), cg->package_name);
  fprintf(cg->out, "// Generated by 3b -- do not edit by hand.\n");
  fprintf(cg->out, "#ifndef BBB_PKG_%.*s_H\n", str8_varg(guard));
  fprintf(cg->out, "#define BBB_PKG_%.*s_H\n\n", str8_varg(guard));
  fprintf(cg->out, "#include \"3b_runtime.h\"\n");
  foreach_index(i, dyn_count(cg->imported_pkg_names)) {
    fprintf(cg->out, "#include \"%.*s.h\"\n", str8_varg(c_mangle_name(ctx_scratch(), cg->imported_pkg_names[i])));
  }
  fprintf(cg->out, "\n");

  fprintf(cg->out, "// --- alias (typedef) declarations -------------------------------------\n");
  foreach_index(i, n->block.stmt_count) {
    TypedIndex stmt = cg->tast->extra[n->block.stmt_first + i];
    if (stmt != TYPED_NIL && !cg->tast->nodes[stmt].is_private && !cg->tast->nodes[stmt].is_imported
        && cg->tast->nodes[stmt].kind == TypedNodeKind_AliasDecl) {
      cg_alias_decl(cg, stmt);
    }
  }
  fprintf(cg->out, "// --- enum / flags declarations ----------------------------------------\n");
  foreach_index(i, n->block.stmt_count) {
    TypedIndex stmt = cg->tast->extra[n->block.stmt_first + i];
    if (stmt != TYPED_NIL && !cg->tast->nodes[stmt].is_private && !cg->tast->nodes[stmt].is_imported
        && cg->tast->nodes[stmt].kind == TypedNodeKind_EnumDecl) {
      cg_enum_decl(cg, stmt);
    }
  }
  fprintf(cg->out, "// --- struct declarations ---------------------------------------------\n");
  // Forward declarations first, bodies second (cg_struct_forward_decl), so a
  // struct field can point to any other struct in the package regardless of
  // source order, including itself. Handle typedefs ride along with the forward
  // decls so a `T^` field works too; the full DEFINE_HANDLE_POOL expansion,
  // which needs T complete, stays below.
  foreach_index(i, n->block.stmt_count) {
    TypedIndex stmt = cg->tast->extra[n->block.stmt_first + i];
    if (stmt != TYPED_NIL && !cg->tast->nodes[stmt].is_private && !cg->tast->nodes[stmt].is_imported
        && cg->tast->nodes[stmt].kind == TypedNodeKind_StructDecl) {
      cg_struct_forward_decl(cg, stmt);
    }
  }
  foreach_index(i, n->block.stmt_count) {
    TypedIndex stmt = cg->tast->extra[n->block.stmt_first + i];
    if (stmt != TYPED_NIL && !cg->tast->nodes[stmt].is_private && !cg->tast->nodes[stmt].is_imported
        && cg->tast->nodes[stmt].kind == TypedNodeKind_HandlePoolDecl) {
      cg_handle_pool_typedef(cg, stmt);
    }
  }
  fprintf(cg->out, "\n");
  // Struct bodies and Map/Set instantiations go through ONE topo sort over a
  // mixed batch, not two passes, because the dependency runs both directions: a
  // struct field can be a Map/Set, and a Map's value type can be a struct. See
  // cg_topo_visit_struct's two branches.
  //
  // Map/Set instantiations are emitted here regardless of the public/private
  // split, as handle pool typedefs are: the type must be complete wherever it
  // is referenced, including from a package importing a public fn that returns
  // one, so it cannot wait for cg_toplevel's later .c-only pass.
  {
    ArenaTemp   temp  = arena_temp_begin(ctx_scratch());
    TypedIndex* stmts = push_array(ctx_scratch(), TypedIndex, n->block.stmt_count);
    u64         count = 0;
    foreach_index(i, n->block.stmt_count) {
      TypedIndex stmt = cg->tast->extra[n->block.stmt_first + i];
      if (stmt != TYPED_NIL && !cg->tast->nodes[stmt].is_private && !cg->tast->nodes[stmt].is_imported
          && (cg->tast->nodes[stmt].kind == TypedNodeKind_StructDecl
              || cg->tast->nodes[stmt].kind == TypedNodeKind_HashTableInstanceDecl)) {
        stmts[count] = stmt;
        count += 1;
      }
    }
    cg_topo_sort_structs(cg, stmts, count);
    foreach_index(i, count) {
      if (cg->tast->nodes[stmts[i]].kind == TypedNodeKind_HashTableInstanceDecl) {
        cg_hashtable_instance_decl(cg, stmts[i]);
      } else {
        cg_struct_decl(cg, stmts[i]);
      }
    }
    arena_temp_end(&temp);
  }
  fprintf(cg->out, "// --- handle pool declarations ------------------------------------------\n");
  foreach_index(i, n->block.stmt_count) {
    TypedIndex stmt = cg->tast->extra[n->block.stmt_first + i];
    if (stmt != TYPED_NIL && !cg->tast->nodes[stmt].is_private && !cg->tast->nodes[stmt].is_imported
        && cg->tast->nodes[stmt].kind == TypedNodeKind_HandlePoolDecl) {
      cg_handle_pool_decl(cg, stmt);
    }
  }
  fprintf(cg->out, "// --- public global declarations ---------------------------------------\n");
  foreach_index(i, n->block.stmt_count) {
    TypedIndex stmt = cg->tast->extra[n->block.stmt_first + i];
    if (stmt == TYPED_NIL || cg->tast->nodes[stmt].is_private || cg->tast->nodes[stmt].is_imported) continue;
    TypedNode* sn = &cg->tast->nodes[stmt];
    if (sn->kind == TypedNodeKind_ConstDecl) {
      fprintf(cg->out, "extern ");
      cg_declare_val(cg, sn->const_decl.type, cg_symbol_name(cg, sn->const_decl.name));
      fprintf(cg->out, ";\n");
    } else if (sn->kind == TypedNodeKind_VarDecl) {
      fprintf(cg->out, "extern ");
      cg_declare(cg, sn->var_decl.type, cg_symbol_name(cg, sn->var_decl.name));
      fprintf(cg->out, ";\n");
    }
  }
  fprintf(cg->out, "\n// --- public function prototypes ---------------------------------------\n");
  foreach_index(i, n->block.stmt_count) {
    TypedIndex stmt = cg->tast->extra[n->block.stmt_first + i];
    // Bodyless `extern` fns name something that already exists elsewhere, so
    // they are not part of this package's public API and get no prototype.
    if (stmt != TYPED_NIL && !cg->tast->nodes[stmt].is_private && !cg->tast->nodes[stmt].is_imported
        && cg->tast->nodes[stmt].kind == TypedNodeKind_FunctionDecl
        && cg->tast->nodes[stmt].func.body != TYPED_NIL) {
      cg_function_prototype(cg, stmt);
    }
  }

  fprintf(cg->out, "\n#endif\n");
}

void
cg_program(Codegen* cg, TypedIndex root) {
  TypedNode* n = &cg->tast->nodes[root];
  xassert(n->kind == TypedNodeKind_Block);
  cg->program_root = root; // what cg_symbol_name resolves public names against
  fprintf(cg->out, "// Generated by 3b -- do not edit by hand.\n");
  if (cg->package_name.size > 0) {
    // Public declarations already live in the package's header, so include
    // that rather than duplicating what it brings in.
    String8 mangled_pkg = c_mangle_name(ctx_scratch(), cg->package_name);
    fprintf(cg->out, "#include \"%.*s.h\"\n", str8_varg(mangled_pkg));
  } else {
    fprintf(cg->out, "#include \"3b_runtime.h\"\n"); // demo path: no package, no header pair
  }
  foreach_index(i, dyn_count(cg->imported_pkg_names)) {
    fprintf(cg->out, "#include \"%.*s.h\"\n", str8_varg(c_mangle_name(ctx_scratch(), cg->imported_pkg_names[i])));
  }
  fprintf(cg->out, "#include \"3b_runtime.h\"\n\n");

  // C requires a struct typedef before any reference and a prototype before any
  // call site, while 3b's checker allows declarations in any order, forward
  // references and mutual recursion included. Reconciling the two: emit every
  // alias, then every struct, then every prototype up front regardless of
  // source position, then everything else, bodies included, in original order.
  //
  // Aliases come first because the common case, a primitive rename, has no
  // dependency on structs, and an alias is more often used as a field's type
  // than the reverse. Aliasing a struct that in turn uses that alias as a field
  // type is a genuine circular ordering problem, and is not handled -- too
  // narrow to justify a topological sort across structs, enums and aliases.
  b32 has_header = cg->package_name.size > 0; // public decls live in the .h in this mode

  fprintf(cg->out, "// --- alias (typedef) declarations -------------------------------------\n");
  foreach_index(i, n->block.stmt_count) {
    TypedIndex stmt = cg->tast->extra[n->block.stmt_first + i];
    if (stmt != TYPED_NIL && !cg->tast->nodes[stmt].is_imported && cg->tast->nodes[stmt].kind == TypedNodeKind_AliasDecl
        && !(has_header && !cg->tast->nodes[stmt].is_private)) {
      cg_alias_decl(cg, stmt);
    }
  }
  fprintf(cg->out, "// --- enum / flags declarations ----------------------------------------\n");
  foreach_index(i, n->block.stmt_count) {
    TypedIndex stmt = cg->tast->extra[n->block.stmt_first + i];
    if (stmt != TYPED_NIL && !cg->tast->nodes[stmt].is_imported && cg->tast->nodes[stmt].kind == TypedNodeKind_EnumDecl
        && !(has_header && !cg->tast->nodes[stmt].is_private)) {
      cg_enum_decl(cg, stmt);
    }
  }
  fprintf(cg->out, "// --- struct declarations ---------------------------------------------\n");
  // Forward declarations first, bodies second, as in cg_program_header.
  foreach_index(i, n->block.stmt_count) {
    TypedIndex stmt = cg->tast->extra[n->block.stmt_first + i];
    if (stmt != TYPED_NIL && !cg->tast->nodes[stmt].is_imported && cg->tast->nodes[stmt].kind == TypedNodeKind_StructDecl
        && !(has_header && !cg->tast->nodes[stmt].is_private)) {
      cg_struct_forward_decl(cg, stmt);
    }
  }
  foreach_index(i, n->block.stmt_count) {
    TypedIndex stmt = cg->tast->extra[n->block.stmt_first + i];
    if (stmt != TYPED_NIL && !cg->tast->nodes[stmt].is_imported && cg->tast->nodes[stmt].kind == TypedNodeKind_HandlePoolDecl
        && !(has_header && !cg->tast->nodes[stmt].is_private)) {
      cg_handle_pool_typedef(cg, stmt);
    }
  }
  fprintf(cg->out, "\n");
  {
    ArenaTemp   temp  = arena_temp_begin(ctx_scratch());
    TypedIndex* stmts = push_array(ctx_scratch(), TypedIndex, n->block.stmt_count);
    u64         count = 0;
    foreach_index(i, n->block.stmt_count) {
      TypedIndex stmt = cg->tast->extra[n->block.stmt_first + i];
      if (stmt != TYPED_NIL && !cg->tast->nodes[stmt].is_imported && cg->tast->nodes[stmt].kind == TypedNodeKind_StructDecl
          && !(has_header && !cg->tast->nodes[stmt].is_private)) {
        stmts[count] = stmt;
        count += 1;
      }
    }
    cg_topo_sort_structs(cg, stmts, count);
    foreach_index(i, count) cg_struct_decl(cg, stmts[i]);
    arena_temp_end(&temp);
  }
  fprintf(cg->out, "// --- handle pool declarations ------------------------------------------\n");
  foreach_index(i, n->block.stmt_count) {
    TypedIndex stmt = cg->tast->extra[n->block.stmt_first + i];
    if (stmt != TYPED_NIL && !cg->tast->nodes[stmt].is_imported && cg->tast->nodes[stmt].kind == TypedNodeKind_HandlePoolDecl
        && !(has_header && !cg->tast->nodes[stmt].is_private)) {
      cg_handle_pool_decl(cg, stmt);
    }
  }
  // Pool storage, unlike the typedefs above, is never guarded by has_header:
  // see cg_handle_pool_storage for why it is always private to this .c.
  foreach_index(i, n->block.stmt_count) {
    TypedIndex stmt = cg->tast->extra[n->block.stmt_first + i];
    if (stmt != TYPED_NIL && !cg->tast->nodes[stmt].is_imported && cg->tast->nodes[stmt].kind == TypedNodeKind_HandlePoolDecl) {
      cg_handle_pool_storage(cg, stmt);
    }
  }
  // Same up-front forward declaration as function prototypes below.
  // check_program gives every top-level val/var forward and cross-file
  // visibility via a pre-pass that binds name and type before checking any
  // body, so codegen must back that with a C declaration early enough, or an
  // earlier file's reference to a later file's global would type-check in 3b
  // and fail in C.
  //
  // A public one already has an `extern` in this package's header, included at
  // the top of this file; repeating it is harmless, since non-defining C
  // declarations may repeat. A private one has no header entry, so this is its
  // only declaration, and must be `static` to match the definition's linkage.
  fprintf(cg->out, "// --- global variable forward declarations ------------------------------\n");
  foreach_index(i, n->block.stmt_count) {
    TypedIndex stmt = cg->tast->extra[n->block.stmt_first + i];
    if (stmt == TYPED_NIL || cg->tast->nodes[stmt].is_imported) continue;
    TypedNode* sn = &cg->tast->nodes[stmt];
    if (sn->kind == TypedNodeKind_ConstDecl) {
      fprintf(cg->out, sn->is_private ? "static " : "extern ");
      cg_declare_val(cg, sn->const_decl.type, cg_symbol_name(cg, sn->const_decl.name));
      fprintf(cg->out, ";\n");
    } else if (sn->kind == TypedNodeKind_VarDecl) {
      fprintf(cg->out, sn->is_private ? "static " : "extern ");
      cg_declare(cg, sn->var_decl.type, cg_symbol_name(cg, sn->var_decl.name));
      fprintf(cg->out, ";\n");
    }
  }
  fprintf(cg->out, "\n");

  fprintf(cg->out, "// --- function prototypes ----------------------------------------------\n");
  foreach_index(i, n->block.stmt_count) {
    TypedIndex stmt = cg->tast->extra[n->block.stmt_first + i];
    // `extern` functions and imported placeholders get no prototype: there is
    // no definition in this program to forward-declare, and an imported one is
    // already prototyped in the other package's header, included above.
    if (stmt != TYPED_NIL && cg->tast->nodes[stmt].kind == TypedNodeKind_FunctionDecl
        && cg->tast->nodes[stmt].func.body != TYPED_NIL
        && !(has_header && !cg->tast->nodes[stmt].is_private)) {
      cg_function_prototype(cg, stmt);
    }
  }
  fprintf(cg->out, "\n");

  foreach_index(i, n->block.stmt_count) {
    cg_toplevel(cg, cg->tast->extra[n->block.stmt_first + i]);
  }
}

////////////////////////////////
//~ PROTOTYPE: lane-parallel definition emission (see cg_program_parallel).
//
// Measures whether base.h's fork-join lane system is worth wiring into codegen
// for real. Reached only via compiler.c's bench_codegen flag, never a normal
// build. Only cg_program's final per-top-level-item definition loop is
// parallelized; the alias/enum/struct/prototype passes ahead of it are cheap
// and order-sensitive, so they stay serial and are duplicated into
// cg_program_parallel.
//
// Each lane renders its items with a disposable by-value copy of `Codegen`, so
// each gets a fresh `scope` stack and its own `next_disambig_id` sequence --
// matching the serial version, which empties `scope` between top-level items.
// Everything else in the copy is read-only during emission and safe to share,
// except `public_toplevel_names`: its lazy build in
// cg_is_own_public_toplevel_name would race, so it is forced single-threaded
// before the phase starts.
//
// Lanes own disjoint contiguous ranges via lane_range, so concatenating their
// buffers by ascending lane index reproduces the serial layout, modulo one
// cosmetic difference: `next_disambig_id` restarts per item rather than
// counting across the file, so a local needing disambiguation gets a different
// suffix. Same uniqueness guarantee, different number.

typedef struct CgParallelJob {
  Codegen*   cg;
  TypedNode* block_node;
  String8*   lane_texts; // one entry per lane, not per item
} CgParallelJob;

static CgParallelJob g_cg_parallel_job;

// Renders this lane's whole range into one memstream rather than one per item.
// With thousands of small items, as in a package like gl.3b, per-item stdio and
// malloc overhead dominated the actual rendering once async_phase_wait took
// fork/join cost off the table. One stream per lane caps that at roughly the
// core count per phase.
static void
cg_parallel_phase_fn(void) {
  CgParallelJob* job   = &g_cg_parallel_job;
  u64            count = job->block_node->block.stmt_count;
  Rng1u64        range = lane_range(count);

  FILE* mem = mem_stream_open();

  Codegen lane_cg = *job->cg;
  lane_cg.out     = mem;
  for (u64 i = range.min; i < range.max; i += 1) {
    lane_cg.scope = NULL; // reset between items -- see the section comment
    TypedIndex stmt = job->cg->tast->extra[job->block_node->block.stmt_first + i];
    cg_toplevel(&lane_cg, stmt);
  }

  char* buf; u64 bufsize;
  mem_stream_close(mem, &buf, &bufsize);
  job->lane_texts[lane_idx()] = str8_copy(ctx_perm(), str8((u8*)buf, bufsize));
  free(buf);
}

// Drop-in replacement for cg_program's final definition loop.
static void
cg_program_defs_parallel(Codegen* cg, TypedNode* n) {
  cg_is_own_public_toplevel_name(cg, str8_lit("")); // force the lazy build now, single-threaded

  g_cg_parallel_job.cg         = cg;
  g_cg_parallel_job.block_node = n;
  // The real lane count, Max(1, core_count - 1) per async_threads_init, is not
  // known until the pool runs, so this over-allocates against the core count
  // rather than threading it through the job struct.
  g_cg_parallel_job.lane_texts = push_array_zero(ctx_scratch(), String8, os_get_core_count());

  u64 gen = async_run_phase(cg_parallel_phase_fn);
  async_phase_wait(gen);

  // Lanes own disjoint, contiguous, index-ordered ranges, so concatenating by
  // ascending lane index reproduces the original top-to-bottom order.
  foreach_index(lane, os_get_core_count()) {
    String8 t = g_cg_parallel_job.lane_texts[lane];
    if (t.size > 0) fwrite(t.str, 1, t.size, cg->out);
  }
}

// cg_program with its final definition-emission loop replaced by
// cg_program_defs_parallel. Not otherwise identical: it also omits cg_program's
// global-variable forward-declaration pass, so a package where one file
// references a global defined in a later file would fail to compile on this
// path. Harmless while this stays benchmark-only; it must be added back before
// the parallel path is ever used for a real build.
void
cg_program_parallel(Codegen* cg, TypedIndex root) {
  TypedNode* n = &cg->tast->nodes[root];
  xassert(n->kind == TypedNodeKind_Block);
  cg->program_root = root;
  fprintf(cg->out, "// Generated by 3b -- do not edit by hand.\n");
  if (cg->package_name.size > 0) {
    String8 mangled_pkg = c_mangle_name(ctx_scratch(), cg->package_name);
    fprintf(cg->out, "#include \"%.*s.h\"\n", str8_varg(mangled_pkg));
  } else {
    fprintf(cg->out, "#include \"3b_runtime.h\"\n");
  }
  foreach_index(i, dyn_count(cg->imported_pkg_names)) {
    fprintf(cg->out, "#include \"%.*s.h\"\n", str8_varg(c_mangle_name(ctx_scratch(), cg->imported_pkg_names[i])));
  }
  fprintf(cg->out, "#include \"3b_runtime.h\"\n\n");

  b32 has_header = cg->package_name.size > 0;

  fprintf(cg->out, "// --- alias (typedef) declarations -------------------------------------\n");
  foreach_index(i, n->block.stmt_count) {
    TypedIndex stmt = cg->tast->extra[n->block.stmt_first + i];
    if (stmt != TYPED_NIL && !cg->tast->nodes[stmt].is_imported && cg->tast->nodes[stmt].kind == TypedNodeKind_AliasDecl
        && !(has_header && !cg->tast->nodes[stmt].is_private)) {
      cg_alias_decl(cg, stmt);
    }
  }
  fprintf(cg->out, "// --- enum / flags declarations ----------------------------------------\n");
  foreach_index(i, n->block.stmt_count) {
    TypedIndex stmt = cg->tast->extra[n->block.stmt_first + i];
    if (stmt != TYPED_NIL && !cg->tast->nodes[stmt].is_imported && cg->tast->nodes[stmt].kind == TypedNodeKind_EnumDecl
        && !(has_header && !cg->tast->nodes[stmt].is_private)) {
      cg_enum_decl(cg, stmt);
    }
  }
  fprintf(cg->out, "// --- struct declarations ---------------------------------------------\n");
  // Forward declarations first, bodies second, as in cg_program_header.
  foreach_index(i, n->block.stmt_count) {
    TypedIndex stmt = cg->tast->extra[n->block.stmt_first + i];
    if (stmt != TYPED_NIL && !cg->tast->nodes[stmt].is_imported && cg->tast->nodes[stmt].kind == TypedNodeKind_StructDecl
        && !(has_header && !cg->tast->nodes[stmt].is_private)) {
      cg_struct_forward_decl(cg, stmt);
    }
  }
  foreach_index(i, n->block.stmt_count) {
    TypedIndex stmt = cg->tast->extra[n->block.stmt_first + i];
    if (stmt != TYPED_NIL && !cg->tast->nodes[stmt].is_imported && cg->tast->nodes[stmt].kind == TypedNodeKind_HandlePoolDecl
        && !(has_header && !cg->tast->nodes[stmt].is_private)) {
      cg_handle_pool_typedef(cg, stmt);
    }
  }
  fprintf(cg->out, "\n");
  {
    ArenaTemp   temp  = arena_temp_begin(ctx_scratch());
    TypedIndex* stmts = push_array(ctx_scratch(), TypedIndex, n->block.stmt_count);
    u64         count = 0;
    foreach_index(i, n->block.stmt_count) {
      TypedIndex stmt = cg->tast->extra[n->block.stmt_first + i];
      if (stmt != TYPED_NIL && !cg->tast->nodes[stmt].is_imported && cg->tast->nodes[stmt].kind == TypedNodeKind_StructDecl
          && !(has_header && !cg->tast->nodes[stmt].is_private)) {
        stmts[count] = stmt;
        count += 1;
      }
    }
    cg_topo_sort_structs(cg, stmts, count);
    foreach_index(i, count) cg_struct_decl(cg, stmts[i]);
    arena_temp_end(&temp);
  }
  fprintf(cg->out, "// --- handle pool declarations ------------------------------------------\n");
  foreach_index(i, n->block.stmt_count) {
    TypedIndex stmt = cg->tast->extra[n->block.stmt_first + i];
    if (stmt != TYPED_NIL && !cg->tast->nodes[stmt].is_imported && cg->tast->nodes[stmt].kind == TypedNodeKind_HandlePoolDecl
        && !(has_header && !cg->tast->nodes[stmt].is_private)) {
      cg_handle_pool_decl(cg, stmt);
    }
  }
  // Pool storage, unlike the typedefs above, is never guarded by has_header:
  // see cg_handle_pool_storage for why it is always private to this .c.
  foreach_index(i, n->block.stmt_count) {
    TypedIndex stmt = cg->tast->extra[n->block.stmt_first + i];
    if (stmt != TYPED_NIL && !cg->tast->nodes[stmt].is_imported && cg->tast->nodes[stmt].kind == TypedNodeKind_HandlePoolDecl) {
      cg_handle_pool_storage(cg, stmt);
    }
  }
  fprintf(cg->out, "// --- function prototypes ----------------------------------------------\n");
  foreach_index(i, n->block.stmt_count) {
    TypedIndex stmt = cg->tast->extra[n->block.stmt_first + i];
    if (stmt != TYPED_NIL && cg->tast->nodes[stmt].kind == TypedNodeKind_FunctionDecl
        && cg->tast->nodes[stmt].func.body != TYPED_NIL
        && !(has_header && !cg->tast->nodes[stmt].is_private)) {
      cg_function_prototype(cg, stmt);
    }
  }
  fprintf(cg->out, "\n");

  cg_program_defs_parallel(cg, n);
}
