// Type checking: the TypedAst is walked, every node's type resolved into
// ck->resolved_types, and anything the lowerer had to defer is decided here.
//
// Lowering runs without type information, so it leaves placeholder nodes
// behind -- a PositionalAccess that does not yet know whether it indexes a
// struct or an array, a DotHop that could be a field or a Map key, a `for`
// with no step literal, a bare `{...}` argument with no struct name. This pass
// rewrites each in place once the surrounding types are known, which is why
// check_expr both returns a type and mutates nodes.
//
// Function signatures are gathered up front, so calls may precede definitions.
// Scopes are a single stack with mark/restore rather than per-block tables.
//
// The invariant most easily broken: resolved_types may be written only through
// resolved_types_ensure_capacity, since checking itself creates new nodes.

#include "3b.h"

// Forward-declared so the type predicates below can share its O(1) lookup
// rather than each scanning ck->enums linearly.
EnumEntry* enum_table_lookup(Checker* ck, String8 name);

void
type_error(Checker* ck, Token tok, const char* fmt, ...) {
  ck->had_error = true;
  va_list args;
  va_start(args, fmt);
  diag_errorv(tok, fmt, args);
  va_end(args);
}

// resolved_types is sized to the node count at the start of checking, but
// checking synthesizes further nodes -- PositionalAccess's array branch pushes
// an IntLiteral per destructured slot -- and those are check_expr'd too.
//
// Every write to resolved_types[idx] must come through here. Writing past the
// original allocation corrupts whatever ctx_perm() handed out next, silently:
// the arena is one allocation as far as ASAN is concerned, so nothing traps.
static void
resolved_types_ensure_capacity(Checker* ck, TypedIndex idx) {
  if ((u64)idx < ck->resolved_types_cap) return;
  u64 new_cap = ck->resolved_types_cap ? ck->resolved_types_cap * 2 : 16;
  while (new_cap <= (u64)idx) new_cap *= 2;
  TypeRef* new_arr = push_array_zero(ctx_perm(), TypeRef, new_cap);
  if (ck->resolved_types_cap) {
    MemoryCopy(new_arr, ck->resolved_types, ck->resolved_types_cap * sizeof(TypeRef));
  }
  ck->resolved_types     = new_arr;
  ck->resolved_types_cap = new_cap;
}

TypeRef
type_ref_unresolved(void) {
  TypeRef t = {0};
  t.kind    = TypeKind_Unresolved;
  return t;
}

b32
type_ref_equal(TypeRef a, TypeRef b) {
  if (a.kind != b.kind) return false;
  if (a.kind == TypeKind_Named) return str8_match(a.name, b.name, 0);
  if (a.kind == TypeKind_Handle) return str8_match(a.name, b.name, 0); // distinct pools never equal,
                                                                        // even if structurally identical
  if (a.kind == TypeKind_Pointer) {
    if (a.pointee == NULL || b.pointee == NULL) return true; // nil is compatible with any pointer type
    return type_ref_equal(*a.pointee, *b.pointee);
  }
  if (a.kind == TypeKind_Array) return a.count == b.count && type_ref_equal(*a.pointee, *b.pointee);
  if (a.kind == TypeKind_Vector) return type_ref_equal(*a.pointee, *b.pointee);
  if (a.kind == TypeKind_Set) return type_ref_equal(*a.pointee, *b.pointee);
  if (a.kind == TypeKind_Map) return type_ref_equal(*a.map_key, *b.map_key) && type_ref_equal(*a.pointee, *b.pointee);
  if (a.kind == TypeKind_Fn) {
    if (a.fn_param_count != b.fn_param_count) return false;
    if (!type_ref_equal(*a.fn_return, *b.fn_return)) return false;
    foreach_index(i, a.fn_param_count) {
      if (!type_ref_equal(a.fn_params[i], b.fn_params[i])) return false;
    }
    return true;
  }
  return true;
}

// The recursive half of type_ref_const_compatible. Past one level of
// indirection, `from` const and `to` not is a discarded-qualifier violation:
// the assignment creates a new alias able to mutate memory the original
// pointer promised not to. Deeper nesting only matters for something like
// `char**`, since `(const T)` marks the innermost base and never an
// intermediate pointer level.
static b32
type_ref_pointee_const_compatible(TypeRef from, TypeRef to) {
  if (from.is_const && !to.is_const) return false;
  if (from.kind == TypeKind_Pointer) {
    if (from.pointee == NULL || to.pointee == NULL) return true; // nil -- compatible with anything
    return type_ref_pointee_const_compatible(*from.pointee, *to.pointee);
  }
  if (from.kind == TypeKind_Array) {
    return type_ref_pointee_const_compatible(*from.pointee, *to.pointee);
  }
  return true;
}

// The directional, assignment-only counterpart to type_ref_equal, which
// ignores is_const because symmetric contexts -- binary operands, `=` -- do
// not care about constness, as in C.
//
// `from` may flow into `to` only if `to` is at least as const at every pointer
// nesting level, C's own rule: `T*` decays into `const T*`, never the reverse
// without an explicit cast. It does not apply above the first pointer, since a
// by-value copy of a const value creates no new alias.
//
// Callers reach here only after type_ref_equal or the array-decay branch has
// confirmed a matching structural shape, so recursion always walks two trees
// of identical shape and needs no kind-mismatch handling.
static b32
type_ref_const_compatible(TypeRef from, TypeRef to) {
  if (from.kind == TypeKind_Pointer) {
    if (from.pointee == NULL || to.pointee == NULL) return true; // nil -- compatible with anything
    return type_ref_pointee_const_compatible(*from.pointee, *to.pointee);
  }
  if (from.kind == TypeKind_Array) {
    return type_ref_pointee_const_compatible(*from.pointee, *to.pointee);
  }
  return true; // no top-level indirection; a by-value copy's constness is its own business
}

// A `[T N]` array may be used wherever a `T*` is expected -- C's own
// array-to-pointer decay. A nested `[[T N] M]` decays the same way to any
// depth, down to whichever element type the pointer names: C guarantees
// contiguous elements regardless of nesting, so the address is the same one
// `&mat[0][0]` would give.
//
// One-directional: a pointer is never usable where an array is expected.
// Codegen needs nothing for this, since the generated C hands the same array
// identifier to a pointer-typed slot and decays it there too.
b32
type_ref_assignable(TypeRef from, TypeRef to) {
  if (type_ref_equal(from, to)) return type_ref_const_compatible(from, to);
  if (to.kind != TypeKind_Pointer || to.pointee == NULL) return false;
  while (from.kind == TypeKind_Array) {
    if (type_ref_equal(*from.pointee, *to.pointee)) return type_ref_const_compatible(*from.pointee, *to.pointee);
    from = *from.pointee;
  }
  return false;
}

b32
type_kind_is_numeric(TypeKind k) {
  switch (k) {
    case TypeKind_I8:  case TypeKind_I16: case TypeKind_I32: case TypeKind_I64:
    case TypeKind_U8:  case TypeKind_U16: case TypeKind_U32: case TypeKind_U64:
    case TypeKind_F32: case TypeKind_F64:
      return true;
    default:
      return false;
  }
}

// type_kind_is_numeric minus the floats, for validating `sizeof`/`alignof`'s
// optional result-type override: it replaces a u64, so only another integer
// width or signedness makes sense.
b32
type_kind_is_integer(TypeKind k) {
  switch (k) {
    case TypeKind_I8:  case TypeKind_I16: case TypeKind_I32: case TypeKind_I64:
    case TypeKind_U8:  case TypeKind_U16: case TypeKind_U32: case TypeKind_U64:
      return true;
    default:
      return false;
  }
}

// What `print` and `str` can render: every numeric kind plus bool, char and
// string, each mapping to a fixed printf specifier via cg_print_specifier.
// Pointers, structs, arena, any and void are excluded rather than guessed at.
b32
type_kind_is_printable(TypeKind k) {
  return type_kind_is_numeric(k) || k == TypeKind_Bool || k == TypeKind_Char || k == TypeKind_String;
}

// type_kind_is_printable plus the one printable pointer shape: `char*`, a
// nul-terminated C string from `cstring` or an extern binding, rendered with
// `%s`. Any other pointee or depth stays unprintable.
b32
type_ref_is_printable(TypeRef t) {
  if (type_kind_is_printable(t.kind)) return true;
  return t.kind == TypeKind_Pointer && t.pointee != NULL && t.pointee->kind == TypeKind_Char;
}

// Counts `{}` placeholders in a `print` template. `{{`/`}}` escape to a
// literal brace; a lone `{` or `}` is malformed. Reports its own type_error
// and returns -1 in that case.
//
// The caller requires the template to be a string literal, so codegen can
// repeat this walk to dispatch each placeholder on its argument's static
// type -- no runtime type tags anywhere.
static i64
count_template_placeholders(Checker* ck, Token tok, String8 tmpl) {
  i64 count = 0;
  for (u64 i = 0; i < tmpl.size; i += 1) {
    u8 c = tmpl.str[i];
    if (c == '{') {
      if (i + 1 < tmpl.size && tmpl.str[i + 1] == '{') { i += 1; continue; }
      if (i + 1 < tmpl.size && tmpl.str[i + 1] == '}') { count += 1; i += 1; continue; }
      type_error(ck, tok, "`print` template has a `{` that isn't part of a `{}` placeholder or a `{{` escape");
      return -1;
    }
    if (c == '}') {
      if (i + 1 < tmpl.size && tmpl.str[i + 1] == '}') { i += 1; continue; }
      type_error(ck, tok, "`print` template has a `}` that isn't part of a `}}` escape");
      return -1;
    }
  }
  return count;
}

// `|`/`bit-and` take numeric operands, or a Named type resolving to an
// `enum`/`flags`. C enums are int-compatible, so `WindowFlags/Resizable |
// WindowFlags/Hidden` stays typed as WindowFlags rather than decaying to i32.
b32
type_ref_is_bitwise_ok(Checker* ck, TypeRef t) {
  if (type_kind_is_numeric(t.kind)) return true;
  if (t.kind != TypeKind_Named) return false;
  return enum_table_lookup(ck, t.name) != NULL;
}

// Equality and ordering share one rule set: numeric, bool, char, pointer,
// enum/flags, `string`, and any struct whose fields are recursively all
// comparable. type_ref_is_deep_comparable (3b.c) is the shared predicate,
// and also decides whether codegen synthesizes a struct's `_eq`/`_cmp`
// helpers (cg_emit_struct_comparators).
b32
type_ref_is_comparable(Checker* ck, TypeRef t) {
  return type_ref_is_deep_comparable(ck->tast, ck->structs, dyn_count(ck->structs), t);
}

// `cast`'s legality check. Codegen emits a literal C cast
// `(TargetType)value`, so whatever C would reject as a scalar cast is
// rejected here instead, where the message can name 3b types.
//
// Castable, and mutually so: numeric (int<->float included), bool, char,
// any pointer, `any` (== `void*`), and enum/flags (integers in C). Struct,
// string (a fat pointer), array, arena/ArenaMark and handle are not.
static b32
type_is_castable_scalar(Checker* ck, TypeRef t) {
  if (type_kind_is_numeric(t.kind)) return true;
  if (t.kind == TypeKind_Bool || t.kind == TypeKind_Char
      || t.kind == TypeKind_Pointer || t.kind == TypeKind_Any) return true;
  if (t.kind != TypeKind_Named) return false;
  return enum_table_lookup(ck, t.name) != NULL;
}

// `reinterpret`'s legality check: the same types as type_is_castable_scalar
// minus `bool`. A C `_Bool` object may only hold 0 or 1 (C11 6.2.5p6), so
// copying an arbitrary bit pattern into one is undefined behavior; `cast`
// and its truthiness coercion is the tool for that.
//
// Returns 0 when not eligible, else the type's byte width. check_expr's
// BinaryReinterpret case requires both sides to match, since a width change
// is truncation or padding rather than a reinterpretation. Pointer/Any/enum
// widths assume a 64-bit target and an int-sized C enum, true for every
// target in build.c.
static u32
type_reinterpret_byte_size(Checker* ck, TypeRef t) {
  switch (t.kind) {
    case TypeKind_I8:  case TypeKind_U8:  case TypeKind_Char: return 1;
    case TypeKind_I16: case TypeKind_U16:                     return 2;
    case TypeKind_I32: case TypeKind_U32: case TypeKind_F32:  return 4;
    case TypeKind_I64: case TypeKind_U64: case TypeKind_F64:
    case TypeKind_Pointer: case TypeKind_Any:                 return 8;
    case TypeKind_Named:
      return enum_table_lookup(ck, t.name) != NULL ? 4 : 0;
    default: return 0;
  }
}

// Returns the new entry's slot index, which callers that want to read
// ScopeEntry.was_read back out after checking a body hold onto: `scope->entries`
// is a dyn array that the body's own bindings may reallocate, so an index
// survives where a `ScopeEntry*` would not.
u64
scope_bind_with_mutability(Scope* scope, String8 name, TypeRef type, b32 is_mutable, Token decl_token) {
  ScopeEntry e = {0};
  e.name       = name;
  e.type       = type;
  e.is_mutable = is_mutable;
  e.decl_token = decl_token;
  dyn_push(ctx_scratch(), scope->entries, e);
  return dyn_count(scope->entries) - 1;
}
// Mutable: covers var/let/fn-params, i.e. everything except `val`.
static inline u64
scope_bind_mutable(Scope* scope, String8 name, TypeRef type, Token decl_token) {
  return scope_bind_with_mutability(scope, name, type, true, decl_token);
}

// Immutable: `val` locals and top-level consts.
static inline u64
scope_bind_immutable(Scope* scope, String8 name, TypeRef type, Token decl_token) {
  return scope_bind_with_mutability(scope, name, type, false, decl_token);
}

// Reverse scan, so the innermost binding of a shadowed name wins. Records the
// hit on the entry (see ScopeEntry.was_read) -- every path that resolves a name
// to a local goes through here, which is exactly what makes "nothing ever named
// this binding" reliable to answer.
//
// Skips Scope's hidden range, which is empty everywhere except inside a
// `parallel` body.
ScopeEntry*
scope_lookup_entry(Scope* scope, String8 name) {
  for (u64 i = dyn_count(scope->entries); i-- > 0; ) {
    if (i >= scope->hidden_lo && i < scope->hidden_hi) { i = scope->hidden_lo; continue; }
    if (str8_match(scope->entries[i].name, name, 0)) {
      scope->entries[i].was_read = true;
      return &scope->entries[i];
    }
  }
  return NULL;
}

// The same scan restricted to what scope_lookup_entry just skipped, so a failed
// lookup inside a `parallel` body can say "this name exists, but not here"
// instead of "undefined identifier". Deliberately does NOT set was_read: the
// name did not resolve, and the only consumer of that flag is codegen's
// unused-binding suppression.
static ScopeEntry*
scope_lookup_hidden(Scope* scope, String8 name) {
  for (u64 i = scope->hidden_hi; i-- > scope->hidden_lo; ) {
    if (str8_match(scope->entries[i].name, name, 0)) return &scope->entries[i];
  }
  return NULL;
}

// Also duplicated in lib3b.c, build.c and translate.c; too small to share.
static String8
str8_basename(String8 path) {
  u64 after_slash = str8_find_needle_reverse(path, 0, str8_lit("/"), 0);
  return str8_skip(path, after_slash);
}

// check_expr's ScopeQuery hook (see ScopeQuery, 3b.h). Runs on every node
// check_expr enters, and captures at most once: when a node's token ends
// exactly at the query position, `scope->entries` holds precisely the names
// visible there.
//
// The capture needs no walk of its own to find the enclosing function.
// check_expr's recursion already is that walk -- params, let-bindings and
// loop vars are pushed and popped around exactly the subtree they cover.
// Shadowing resolves through scope_lookup_entry's reverse scan, where the
// innermost entry for a name wins; lib3b.c dedups the same way.
static void
scope_query_try_capture(Checker* ck, Scope* scope, TypedNode* n) {
  if (ck->scope_query_done) return; // first match wins
  Token tok  = n->token;
  u32   span = (u32)tok.text.size;
  if (span == 0) span = 1;
  if (tok.line != ck->scope_query->line || tok.col + span != ck->scope_query->col) return;
  SourceFile* sf = source_file_get(tok.file_id);
  if (!str8_match(str8_basename(sf->path), ck->scope_query->file_basename, 0)) return;

  ck->scope_query_done  = true;
  u64 count             = dyn_count(scope->entries);
  ck->scope_query_result = count > 0 ? malloc(count * sizeof(ScopeEntry)) : NULL;
  if (count > 0) memcpy(ck->scope_query_result, scope->entries, count * sizeof(ScopeEntry));
  ck->scope_query_count = count;
}

TypeRef
scope_lookup(Scope* scope, String8 name) {
  ScopeEntry* e = scope_lookup_entry(scope, name);
  return e ? e->type : type_ref_unresolved();
}

u64  scope_mark(Scope* scope)             { return dyn_count(scope->entries); }
void scope_pop_to(Scope* scope, u64 mark) { if (scope->entries) dyn_hdr(scope->entries)->count = mark; }

// O(1) through ck->fns_by_name/structs_by_name/enums_by_name, built by
// check_program once its pass-1 gathering loop finishes. Entries point
// straight into ck->fns/structs/enums, which is safe only because nothing
// pushes to those arrays after that pass.
FnEntry*
fn_table_lookup(Checker* ck, String8 name) {
  return (FnEntry*)hashtable_lookup(&ck->fns_by_name, name);
}

// The TypeKind_Fn TypeRef a top-level `fn`'s name resolves to when used as
// a value rather than called, e.g. `(takes-fn some-toplevel-fn)`. Codegen
// needs no counterpart: a bare C function name decays to a pointer already.
//
// Param types (never names -- see TypeKind_Fn, 3b.h) are copied into
// ctx_perm(), so the result can be stored in resolved_types or a scope
// entry indefinitely.
static TypeRef
type_ref_from_fn_decl(TypedAst* tast, TypedNode* decl) {
  TypeRef t = {0};
  t.kind    = TypeKind_Fn;
  if (decl->func.param_count > 0) {
    t.fn_params = push_array(ctx_perm(), TypeRef, decl->func.param_count);
    foreach_index(i, decl->func.param_count) {
      t.fn_params[i] = tast->params[decl->func.param_first + i].type;
    }
  }
  t.fn_param_count = decl->func.param_count;
  t.fn_return       = push_one(ctx_perm(), TypeRef);
  *t.fn_return       = decl->func.return_type;
  return t;
}

StructEntry*
struct_table_lookup(Checker* ck, String8 name) {
  return (StructEntry*)hashtable_lookup(&ck->structs_by_name, name);
}

EnumEntry*
enum_table_lookup(Checker* ck, String8 name) {
  return (EnumEntry*)hashtable_lookup(&ck->enums_by_name, name);
}

// Finds the first `TypeKind_Named` component of `t` that names no declared
// struct or enum, writing it to `*out_bad`. Only Named needs the lookup:
// primitives carry their own kind, aliases are substituted away during
// lowering, and `T^` is validated against the handle-pool table there too.
//
// Recurses through every type-former that can carry a component type, so
// `Missing*`, `[Missing]`, `{string Missing}` and `(fn [a Missing] i32)` are
// all reachable. Stops at the first hit -- one diagnostic per annotation
// reads better than one per level of nesting, and a name that resolves
// nowhere is a single mistake however deeply it is wrapped.
static b32
type_ref_has_undeclared_name(Checker* ck, TypeRef t, String8* out_bad) {
  switch (t.kind) {
    case TypeKind_Named: {
      if (struct_table_lookup(ck, t.name) || enum_table_lookup(ck, t.name)) return false;
      *out_bad = t.name;
      return true;
    }
    case TypeKind_Pointer:
    case TypeKind_Array:
    case TypeKind_Vector:
    case TypeKind_Set: {
      // A Pointer's NULL pointee is `nil`, which names nothing to resolve.
      return t.pointee != NULL && type_ref_has_undeclared_name(ck, *t.pointee, out_bad);
    }
    case TypeKind_Map: {
      if (t.map_key && type_ref_has_undeclared_name(ck, *t.map_key, out_bad)) return true;
      return t.pointee != NULL && type_ref_has_undeclared_name(ck, *t.pointee, out_bad);
    }
    case TypeKind_Fn: {
      foreach_index(i, t.fn_param_count) {
        if (type_ref_has_undeclared_name(ck, t.fn_params[i], out_bad)) return true;
      }
      return t.fn_return != NULL && type_ref_has_undeclared_name(ck, *t.fn_return, out_bad);
    }
    default: return false;
  }
}

// Rejects every type annotation naming a type this package never declares and
// never imports.
//
// The lowerer records one TypeAnnotation per atom in type position (see
// lower_type_node), which is exactly the set of places a name can be written:
// struct fields, function parameters and return types, `val`/`var`/`let`
// annotations, and each component of a compound type, since those are lowered
// by recursing back through lower_type_node. Checking that one list therefore
// covers all of them at once, and reports against the annotation's own token
// rather than its enclosing declaration.
//
// Without this an unresolved name reaches codegen and comes back as gcc's
// "unknown type name" against the generated C, pointing at output/*.h instead
// of the .3b line -- or, for a function-pointer parameter, does not come back
// at all: `i32 (*cb)(Missing)` is a valid old-style C identifier list.
//
// Imported types resolve because a dependency's public decls are spliced into
// this package's typed AST under their "pkg/Member" names, which is what a
// qualified annotation lowers to (see splice_public_decl, compiler.c). The
// dependency's own annotations are not spliced, and do not need to be: each
// package is checked when it is compiled.
static void
check_type_annotations(Checker* ck) {
  foreach_index(i, dyn_count(ck->tast->type_annotations)) {
    TypeAnnotation* ann = &ck->tast->type_annotations[i];
    String8         bad = {0};
    if (!type_ref_has_undeclared_name(ck, ann->type, &bad)) continue;
    type_error(ck, ann->token, "unknown type `%.*s`", str8_varg(bad));
  }
}

// C11 anonymous-member lookup: searches `decl`'s field list for
// `field_name`, then recurses into any anonymous (`_`) struct/union member.
// A direct field at this level wins over one reached through an anonymous
// member, which also means two anonymous members declaring the same name
// never has to be reported as an ambiguity.
//
// Nothing else in the checker needs to know anonymous members exist:
// codegen emits the same nested C members, so the generated `base.field`
// resolves through C's own identical lookup (cg_emit_anon_member_body).
static Param*
find_field_recursive(Checker* ck, TypedNode* decl, String8 field_name) {
  foreach_index(j, decl->struct_decl.field_count) {
    Param* f = &ck->tast->params[decl->struct_decl.field_first + j];
    if (!f->is_anon && str8_match(f->name, field_name, 0)) return f;
  }
  foreach_index(j, decl->struct_decl.field_count) {
    Param* f = &ck->tast->params[decl->struct_decl.field_first + j];
    if (!f->is_anon || f->type.kind != TypeKind_Named) continue;
    StructEntry* nested = struct_table_lookup(ck, f->type.name);
    if (!nested) continue; // already reported by check_program's StructDecl validation
    Param* found = find_field_recursive(ck, &ck->tast->nodes[nested->decl], field_name);
    if (found) return found;
  }
  return NULL;
}

// Checks that every `_` (anonymous member) field resolves to a struct or
// union type. cg_emit_anon_member_body has to inline-expand that type's
// field list unconditionally, so this cannot be left to be caught lazily at
// a field access: a struct nobody ever accesses through would otherwise
// reach codegen with an unchecked field type.
//
// Only the anonymous ones: an ordinary field's type is a type annotation like
// any other, and check_type_annotations has already rejected it if the name
// resolves to nothing. What is specific to `_` is the extra demand that the
// name resolve to a struct or union in particular, an enum or a primitive
// having no field list to expand.
static void
check_struct_decl(Checker* ck, TypedNode* n) {
  foreach_index(i, n->struct_decl.field_count) {
    Param* f = &ck->tast->params[n->struct_decl.field_first + i];
    if (!f->is_anon) continue;
    if (f->type.kind != TypeKind_Named || !struct_table_lookup(ck, f->type.name)) {
      type_error(ck, n->token, "anonymous member `_` must be a struct or union type, got %.*s",
                 str8_varg(type_ref_display(ctx_scratch(), f->type)));
    }
  }
}

// True if `t` embeds `target` by value, directly or through any chain of
// by-value struct fields. A pointer ends the walk -- `A*` inside `A` is the
// legal way to build a recursive type, and is the fix this check's diagnostic
// suggests -- but a fixed-size array does not, since `[N]A` is N copies of A
// laid out inline.
//
// `depth` terminates the walk on a cycle that does not run through `target`
// (checking A when B contains itself, say). A cycle-free chain visits each
// struct at most once, so anything longer than the package's struct count has
// revisited one; that cycle gets its own diagnostic when its own struct is
// checked, so stopping here loses nothing.
static b32
struct_embeds_by_value(Checker* ck, String8 target, TypeRef t, u64 depth) {
  if (t.kind == TypeKind_Array) return struct_embeds_by_value(ck, target, *t.pointee, depth);
  if (t.kind != TypeKind_Named) return false;
  if (str8_match(t.name, target, 0)) return true;
  if (depth > dyn_count(ck->structs)) return false;
  StructEntry* e = struct_table_lookup(ck, t.name);
  if (!e) return false; // an enum, or a name that failed to resolve -- reported elsewhere
  TypedNode* decl = &ck->tast->nodes[e->decl];
  foreach_index(i, decl->struct_decl.field_count) {
    Param* f = &ck->tast->params[decl->struct_decl.field_first + i];
    if (struct_embeds_by_value(ck, target, f->type, depth + 1)) return true;
  }
  return false;
}

// Rejects a struct that contains itself by value, which has no finite size and
// no valid layout. Every consumer of the typed AST walks a struct's fields
// transitively -- layout, both backends' struct copies, and
// type_ref_is_deep_comparable's synthesized comparators -- so an unchecked
// cycle here is an infinite recursion later, in whichever of them runs first.
//
// Runs as a pre-pass over the whole struct table rather than from
// check_struct_decl, because those same walks are reachable from a function
// body, and a package is free to declare its functions before its structs.
//
// Mutual recursion reports once per participating struct: each one is a real
// declaration needing a real fix, and each message names the field to change.
static void
check_struct_cycles(Checker* ck) {
  foreach_index(i, dyn_count(ck->structs)) {
    TypedNode* decl = &ck->tast->nodes[ck->structs[i].decl];
    String8    name = ck->structs[i].name;
    foreach_index(fi, decl->struct_decl.field_count) {
      Param* f = &ck->tast->params[decl->struct_decl.field_first + fi];
      if (!struct_embeds_by_value(ck, name, f->type, 0)) continue;
      type_error(ck, decl->token,
                 "`struct %.*s` contains itself by value through field `%.*s` (of type %.*s), which has no "
                 "finite size -- use a pointer (`%.*s*`) instead",
                 str8_varg(name), str8_varg(f->name), str8_varg(type_ref_display(ctx_scratch(), f->type)), str8_varg(name));
      break; // one diagnostic per struct: the rest of its fields would repeat it
    }
  }
}

// Rejects two variants of one enum/flags sharing a name. check_expr's
// EnumAccess case resolves a variant by scanning the decl's variant list and
// stopping at the first match, so without this a duplicate would silently
// resolve to the earlier one. Variant counts are small; a pairwise scan is
// enough.
static void
check_enum_decl(Checker* ck, TypedNode* n) {
  foreach_index(i, n->enum_decl.variant_count) {
    EnumVariant* a = &ck->tast->enum_variants[n->enum_decl.variant_first + i];
    for (u64 j = i + 1; j < n->enum_decl.variant_count; j += 1) {
      EnumVariant* b = &ck->tast->enum_variants[n->enum_decl.variant_first + j];
      if (str8_match(a->name, b->name, 0)) {
        type_error(ck, n->token, "%s `%.*s` declares variant `%.*s` more than once",
                   n->enum_decl.is_flags ? "flags" : "enum",
                   str8_varg(n->enum_decl.name), str8_varg(a->name));
      }
    }
  }
}

// Array literals carry no element type in the source, so checking one needs
// an expected type from the caller. Only reachable from the places that
// have one in hand (val/var/let bindings, struct-literal fields), never from
// check_expr's bottom-up dispatch.
TypeRef
check_array_literal(Checker* ck, Scope* scope, TypedIndex idx, TypeRef expected) {
  TypedNode* n = &ck->tast->nodes[idx];
  xassert(n->kind == TypedNodeKind_ArrayLiteral);
  if (expected.kind != TypeKind_Array) {
    type_error(ck, n->token, "array literal used where %.*s was expected", str8_varg(type_ref_display(ctx_scratch(), expected)));
    resolved_types_ensure_capacity(ck, idx);
    ck->resolved_types[idx] = type_ref_unresolved();
    return type_ref_unresolved();
  }
  if (n->array_lit.element_count != expected.count) {
    type_error(ck, n->token, "array literal has %u element(s), expected %llu",
               (u32)n->array_lit.element_count, (unsigned long long)expected.count);
  }
  foreach_index(i, n->array_lit.element_count) {
    TypedIndex elem_idx = ck->tast->array_elements[n->array_lit.element_first + i];
    // check_init_expr, not check_expr: a nested array literal element needs
    // an expected type of its own.
    TypeRef    elem_ty  = check_init_expr(ck, scope, elem_idx, *expected.pointee);
    if (elem_ty.kind != TypeKind_Unresolved && !type_ref_equal(elem_ty, *expected.pointee)) {
      type_error(ck, n->token, "array literal element %u: expected %.*s, got %.*s", (u32)(i + 1),
                 str8_varg(type_ref_display(ctx_scratch(), *expected.pointee)), str8_varg(type_ref_display(ctx_scratch(), elem_ty)));
    }
  }
  resolved_types_ensure_capacity(ck, idx);
  ck->resolved_types[idx] = expected;
  return expected;
}

// Gives a suffixless integer literal the type its context expects, instead of
// leaving it at the i32 default and reporting a mismatch the source never
// wrote: `(val n i64 5)`, `(takes-u8 200)`, `(fn f [] i64 5)`. The same patch
// BinaryAdd applies from an inferred peer type.
//
// Must run BEFORE the node is checked, not as a retry after a mismatch: the
// range check in check_expr's IntLiteral case would already have reported
// `0x8000000000000000` against i32 on the first pass, and a diagnostic cannot
// be taken back. Every caller therefore sits ahead of its own check_expr.
//
// Only a bare literal, and only for an integer target. A float target would
// need the node itself to become a FloatLiteral, as ForRangeExpr's synthesized
// step does; leaving that out keeps `(val f f32 5)` rejected exactly as
// before. An authored suffix always wins, so `(val n i64 5u8)` still fails.
static void
adopt_int_literal_type(Checker* ck, TypedIndex idx, TypeRef expected) {
  if (idx == TYPED_NIL || !type_kind_is_integer(expected.kind)) return;
  TypedNode* n = &ck->tast->nodes[idx];
  if (n->kind == TypedNodeKind_IntLiteral && n->int_lit.explicit_type == TypeKind_Unresolved) {
    n->int_lit.explicit_type = expected.kind;
  }
}

// adopt_int_literal_type through the forms that yield their value from a
// sub-expression rather than being one, so a function whose declared return
// type is the only thing naming i64 still reaches the literal that becomes its
// value: `(fn f [] i64 5)` is a Block around the 5, and `(let [...] 5)` or
// `(if c 1 2)` in tail position are the same situation one level in.
//
// Only tail position, where the value flows out. A literal anywhere earlier in
// a block is a statement whose value is discarded and has no expected type.
// Constructs not listed here (`match`, `while`) simply don't adopt; they fall
// back to the i32 default and the pre-existing mismatch error.
static void
adopt_int_literal_in_tail(Checker* ck, TypedIndex idx, TypeRef expected) {
  if (idx == TYPED_NIL || !type_kind_is_integer(expected.kind)) return;
  TypedNode* n = &ck->tast->nodes[idx];
  switch (n->kind) {
    case TypedNodeKind_Block: {
      if (n->block.stmt_count == 0) return;
      TypedIndex last = ck->tast->extra[n->block.stmt_first + n->block.stmt_count - 1];
      adopt_int_literal_in_tail(ck, last, expected);
    } break;
    case TypedNodeKind_LetExpr:     adopt_int_literal_in_tail(ck, n->let_expr.body, expected);     break;
    case TypedNodeKind_ScratchExpr: adopt_int_literal_in_tail(ck, n->scratch_expr.body, expected); break;
    case TypedNodeKind_IfExpr: {
      // Both branches: the whole `if` is the value, so each arm is in tail
      // position, and check_expr's IfExpr case compares the two against each
      // other -- adopting only one would turn a match into a mismatch.
      adopt_int_literal_in_tail(ck, n->if_expr.then_branch, expected);
      adopt_int_literal_in_tail(ck, n->if_expr.else_branch, expected);
    } break;
    default: adopt_int_literal_type(ck, idx, expected); break;
  }
}

// Checks an initializer against the declared type it binds to. Shared by
// ConstDecl, VarDecl and LetExpr so the ArrayLiteral case above and the
// omitted-initializer case (TYPED_NIL, arrays only, zero-initialized by
// codegen) each live in one place. StructLiteral's per-field check comes
// through here too, so a field initializer adopts its field's type.
TypeRef
check_init_expr(Checker* ck, Scope* scope, TypedIndex init, TypeRef declared_type) {
  if (init == TYPED_NIL) {
    return declared_type; // omitted -- valid only for arrays, enforced at lowering time
  }
  TypedNode* n = &ck->tast->nodes[init];
  if (n->kind == TypedNodeKind_ArrayLiteral) {
    return check_array_literal(ck, scope, init, declared_type);
  }
  adopt_int_literal_type(ck, init, declared_type);
  return check_expr(ck, scope, init);
}

// Checks a `val`/`var` initializer and binds the name into `scope`. Top-level
// (check_program) and nested (check_block) declarations differ only in which
// scope the caller passes.
static void
check_decl_and_bind(Checker* ck, Scope* scope, TypedNode* n) {
  if (n->kind == TypedNodeKind_ConstDecl) {
    TypeRef init_ty = check_init_expr(ck, scope, n->const_decl.init, n->const_decl.type);
    if (init_ty.kind != TypeKind_Unresolved && !type_ref_assignable(init_ty, n->const_decl.type)) {
      type_error(ck, n->token, "`val %.*s` declared as %.*s but initializer is %.*s",
                 str8_varg(n->const_decl.name), str8_varg(type_ref_display(ctx_scratch(), n->const_decl.type)),
                 str8_varg(type_ref_display(ctx_scratch(), init_ty)));
    }
    scope_bind_immutable(scope, n->const_decl.name, n->const_decl.type, n->token);
  } else {
    xassert(n->kind == TypedNodeKind_VarDecl);
    TypeRef init_ty = check_init_expr(ck, scope, n->var_decl.init, n->var_decl.type);
    if (init_ty.kind != TypeKind_Unresolved && !type_ref_assignable(init_ty, n->var_decl.type)) {
      type_error(ck, n->token, "`var %.*s` declared as %.*s but initializer is %.*s",
                 str8_varg(n->var_decl.name), str8_varg(type_ref_display(ctx_scratch(), n->var_decl.type)),
                 str8_varg(type_ref_display(ctx_scratch(), init_ty)));
    }
    scope_bind_mutable(scope, n->var_decl.name, n->var_decl.type, n->token);
  }
}

// Checks a block's statements in order. A nested `val`/`var` binds for the
// rest of the block only; the mark/pop keeps it from leaking out, and nests
// LIFO with LetExpr's own mark/pop.
//
// A declaration is not a value, so it never becomes the block's `result`. A
// block ending in one falls back to void, which surfaces through the
// existing return-type mismatch check in check_program.
TypeRef
check_block(Checker* ck, Scope* scope, TypedIndex block_idx) {
  TypedNode* n = &ck->tast->nodes[block_idx];
  xassert(n->kind == TypedNodeKind_Block);
  u64     mark   = scope_mark(scope);
  TypeRef result = {0}; // kind 0 is Unresolved, so set Void explicitly
  result.kind    = TypeKind_Void;
  foreach_index(i, n->block.stmt_count) {
    TypedIndex stmt      = ck->tast->extra[n->block.stmt_first + i];
    TypedNode* stmt_node = &ck->tast->nodes[stmt];
    if (stmt_node->kind == TypedNodeKind_ConstDecl || stmt_node->kind == TypedNodeKind_VarDecl) {
      check_decl_and_bind(ck, scope, stmt_node);
      result.kind = TypeKind_Void;
    } else {
      result = check_expr(ck, scope, stmt);
    }
  }
  scope_pop_to(scope, mark);
  ck->resolved_types[block_idx] = result;
  return result;
}

// True for the expression shapes `swap` can write through: the ones that are
// C lvalues and that `set` also accepts as targets -- a mutable identifier,
// `(get base field)`, `(nth base index)`, `(deref ptr)`. Anything else is an
// rvalue that Swap's expansion cannot assign into.
static b32
swap_target_is_valid(Scope* scope, TypedNode* node) {
  switch (node->kind) {
    case TypedNodeKind_Identifier: {
      ScopeEntry* e = scope_lookup_entry(scope, node->ident.name);
      return e && e->is_mutable;
    }
    case TypedNodeKind_FieldAccess:
    case TypedNodeKind_IndexAccess:
    case TypedNodeKind_UnaryDeref:
      return true;
    default:
      return false;
  }
}

// `&expr`'s operand check: swap_target_is_valid's addressable kinds without
// the mutability requirement. Taking the address of an immutable `val` is
// legal C -- only `set` through the name is barred -- whereas `swap` has to
// rebind the name itself. Everything else is a temporary with no address.
static b32
addr_operand_is_valid(TypedNode* node) {
  switch (node->kind) {
    case TypedNodeKind_Identifier:
    case TypedNodeKind_FieldAccess:
    case TypedNodeKind_IndexAccess:
    case TypedNodeKind_UnaryDeref:
      return true;
    default:
      return false;
  }
}

// Range check for an integer literal's resolved kind. Float overflow is
// fuzzier and isn't attempted. IntLiteral's check_expr case is the only
// caller, but it sees every way a kind gets finalized: an explicit suffix
// (`300u8`), the i32 default, and the peer-inference patches applied by
// BinaryAdd/Sub/Mul/Div and ForRangeExpr's synthesized step.
//
// U64 is the odd one out. `value` is a signed i64, so any literal in the top
// half of u64's range -- `0xFFFFFFFFFFFFFFFFu64` (GL_TIMEOUT_IGNORED) or
// `18446744073709551615u64` -- round-trips as negative. Every 64-bit pattern
// is a valid u64, so those pass; an authored negative like `-5u64` still
// fails, and int_lit.unsigned_bits (3b.h) is what separates the two.
static b32
int_literal_fits(i64 value, TypeKind kind, b32 unsigned_bits) {
  switch (kind) {
    case TypeKind_I8:  return value >= -128 && value <= 127;
    case TypeKind_I16: return value >= -32768 && value <= 32767;
    case TypeKind_I32: return value >= -2147483648LL && value <= 2147483647LL;
    case TypeKind_I64: return true; // the widest signed type `value` can hold
    case TypeKind_U8:  return value >= 0 && value <= 255;
    case TypeKind_U16: return value >= 0 && value <= 65535;
    case TypeKind_U32: return value >= 0 && value <= 4294967295LL;
    case TypeKind_U64: return unsigned_bits || value >= 0; // see above
    default:           return true; // non-integer kinds are not this function's concern
  }
}

// Checks a loop body with Checker.loop_depth raised, which is what makes
// `break` and `continue` legal inside it. Every loop form goes through this
// instead of calling check_expr on its body directly, so a new loop kind
// cannot forget to admit them.
//
// It also CLEARS in_loop_header, which is what keeps `(for [i 0 (do (while c
// (break)) 3)] ...)` legal: the jump sits in an outer loop's header, but in
// its own loop's body, and the body is what counts.
static TypeRef
check_loop_body(Checker* ck, Scope* scope, TypedIndex body) {
  b32 saved_header   = ck->in_loop_header;
  ck->in_loop_header = false;
  ck->loop_depth += 1;
  TypeRef body_ty = check_expr(ck, scope, body);
  ck->loop_depth -= 1;
  ck->in_loop_header = saved_header;
  return body_ty;
}

// The counterpart for a loop's HEADER expressions -- a `while` condition, a
// range `for`'s begin/end/step, a `for`'s collection, a `parallel-for`'s
// count. `break` and `continue` are rejected inside these because the two
// backends cannot agree on what they mean, and neither reading is one anybody
// would want:
//
//   (for [i 0 3] (while (do (break) true) ...))
//
// Natively the header is not part of the loop's C body, so the `break` binds
// to the ENCLOSING `for`. bcgen opens its loop context before compiling the
// header, so it binds to the inner `while` -- and a `continue` in a `while`
// condition jumps back to that same condition and spins forever. Rejecting
// the shape is what makes the question moot on both backends.
static TypeRef
check_loop_header(Checker* ck, Scope* scope, TypedIndex expr) {
  b32 saved_header   = ck->in_loop_header;
  ck->in_loop_header = true;
  TypeRef ty         = check_expr(ck, scope, expr);
  ck->in_loop_header = saved_header;
  return ty;
}

// Binds a `for`'s loop variables, checks its body under them, and records which
// of the two the body actually named (see ScopeEntry.was_read) back onto the
// node for codegen. Shared by the Array/Vector, Set and Map cases of
// TypedNodeKind_ForEachExpr, which differ only in the two bound types.
static void
check_for_each_body(Checker* ck, Scope* scope, TypedIndex idx, TypeRef index_ty, TypeRef elem_ty) {
  TypedNode* n          = &ck->tast->nodes[idx];
  u64        mark       = scope_mark(scope);
  u64        index_slot = 0;
  if (n->for_each.has_index) index_slot = scope_bind_mutable(scope, n->for_each.index_name, index_ty, n->token);
  u64 elem_slot = scope_bind_mutable(scope, n->for_each.elem_name, elem_ty, n->token);

  check_loop_body(ck, scope, n->for_each.body);

  n = &ck->tast->nodes[idx]; // typed_push may have moved tast->nodes
  n->for_each.index_is_read = n->for_each.has_index && scope->entries[index_slot].was_read;
  n->for_each.elem_is_read  = scope->entries[elem_slot].was_read;
  scope_pop_to(scope, mark);
}

TypeRef
check_expr(Checker* ck, Scope* scope, TypedIndex idx) {
  if (idx == TYPED_NIL) return type_ref_unresolved();
  TypedNode* n      = &ck->tast->nodes[idx];
  if (ck->scope_query) scope_query_try_capture(ck, scope, n);
  TypeRef    result = type_ref_unresolved();
  switch (n->kind) {
    case TypedNodeKind_IntLiteral: {
      TypeKind kind = n->int_lit.explicit_type != TypeKind_Unresolved ? n->int_lit.explicit_type : TypeKind_I32;
      if (!int_literal_fits(n->int_lit.value, kind, n->int_lit.unsigned_bits)) {
        TypeRef kind_ref = { .kind = kind };
        // Printed back the way it was READ, or the message contradicts the
        // source: `18446744073709551615u8` is out of range for a u8, but it
        // is not the number -1.
        if (n->int_lit.unsigned_bits) {
          type_error(ck, n->token, "literal %llu doesn't fit in %.*s",
                     (unsigned long long)n->int_lit.value, str8_varg(type_ref_display(ctx_scratch(), kind_ref)));
        } else {
          type_error(ck, n->token, "literal %lld doesn't fit in %.*s",
                     (long long)n->int_lit.value, str8_varg(type_ref_display(ctx_scratch(), kind_ref)));
        }
      }
      result.kind = kind;
    } break;
    case TypedNodeKind_FloatLiteral:
      result.kind = n->float_lit.explicit_type != TypeKind_Unresolved ? n->float_lit.explicit_type : TypeKind_F32;
      break;
    case TypedNodeKind_StringLiteral: result.kind = TypeKind_String; break;
    case TypedNodeKind_BoolLiteral:   result.kind = TypeKind_Bool;   break;
    case TypedNodeKind_NilLiteral: {
      // The wildcard nil pointer -- see TypeRef's note on `pointee == NULL`.
      // type_ref_equal treats it as compatible with any pointer type, so no
      // expected type has to be threaded down to here.
      result.kind    = TypeKind_Pointer;
      result.pointee = NULL;
    } break;
    case TypedNodeKind_Identifier: {
      // scope_lookup_entry rather than the scope_lookup wrapper, to get the
      // matched entry's decl_token. Stashing it on the reference node lets
      // lib3b.c's goto-definition read back where this resolved without
      // re-deriving scope and shadowing (see decl_token, 3b.h).
      ScopeEntry* e = scope_lookup_entry(scope, n->ident.name);
      if (e) {
        result               = e->type;
        n->ident.decl_token = e->decl_token;
      } else {
        // Not a local or param: it may name a top-level `fn` used as a value
        // rather than called, e.g. `(takes-fn some-fn)`. Calls resolve their
        // callee in TypedNodeKind_Call and never reach here.
        FnEntry* fn = fn_table_lookup(ck, n->ident.name);
        if (fn) {
          result = type_ref_from_fn_decl(ck->tast, &ck->tast->nodes[fn->decl]);
        } else if (scope_lookup_hidden(scope, n->ident.name)) {
          // In scope textually, but hidden: a `parallel` body naming one of the
          // enclosing function's params or locals. "Undefined" would be
          // actively misleading, the name being right there a line above.
          type_error(ck, n->token,
                     "`%.*s` is a local of the enclosing function, which a `parallel` body cannot see -- "
                     "add it to the capture list, `(parallel [%.*s %.*s] ...)`",
                     str8_varg(n->ident.name), str8_varg(n->ident.name), str8_varg(n->ident.name));
        } else {
          type_error(ck, n->token, "undefined identifier `%.*s`", str8_varg(n->ident.name));
        }
      }
    } break;
    case TypedNodeKind_BinaryAdd:
    case TypedNodeKind_BinarySub:
    case TypedNodeKind_BinaryMul:
    case TypedNodeKind_BinaryDiv: {
      TypeRef lt = check_expr(ck, scope, n->binary.lhs);
      // lower_incdec marks the `1` it synthesizes for `++`/`--` as
      // infer_from_peer rather than hardcoding i32. With `lt` now known,
      // patch that literal to match before checking it, so `(++ x)` works on
      // any numeric type. Float targets need the node to actually become a
      // FloatLiteral -- explicit_type alone can't make an IntLiteral float,
      // the same reason ForRangeExpr's synthesized step branches this way.
      // Nothing else sets the flag, so ordinary `(+ a b)` is unaffected.
      TypedNode* rhs_node = &ck->tast->nodes[n->binary.rhs];
      if (rhs_node->kind == TypedNodeKind_IntLiteral && rhs_node->int_lit.infer_from_peer
          && lt.kind != TypeKind_Unresolved) {
        if (lt.kind == TypeKind_F32 || lt.kind == TypeKind_F64) {
          f64 value          = (f64)rhs_node->int_lit.value;
          rhs_node->kind     = TypedNodeKind_FloatLiteral;
          rhs_node->float_lit.value         = value;
          rhs_node->float_lit.explicit_type = lt.kind;
        } else {
          rhs_node->int_lit.explicit_type = lt.kind;
        }
      }
      TypeRef rt = check_expr(ck, scope, n->binary.rhs);
      if (lt.kind != TypeKind_Unresolved && rt.kind != TypeKind_Unresolved) {
        if (!type_ref_equal(lt, rt)) {
          type_error(ck, n->token, "`%s` operands have mismatched types: %.*s vs %.*s",
                     binary_op_display_name(n->kind), str8_varg(type_ref_display(ctx_scratch(), lt)), str8_varg(type_ref_display(ctx_scratch(), rt)));
        } else if (!type_kind_is_numeric(lt.kind)) {
          type_error(ck, n->token, "`%s` requires numeric operands, got %.*s",
                     binary_op_display_name(n->kind), str8_varg(type_ref_display(ctx_scratch(), lt)));
        } else {
          result = lt;
        }
      }
    } break;
    case TypedNodeKind_BinaryMod: {
      TypeRef lt = check_expr(ck, scope, n->binary.lhs);
      TypeRef rt = check_expr(ck, scope, n->binary.rhs);
      if (lt.kind != TypeKind_Unresolved && rt.kind != TypeKind_Unresolved) {
        if (!type_ref_equal(lt, rt)) {
          type_error(ck, n->token, "`%s` operands have mismatched types: %.*s vs %.*s",
                     binary_op_display_name(n->kind), str8_varg(type_ref_display(ctx_scratch(), lt)), str8_varg(type_ref_display(ctx_scratch(), rt)));
        } else if (!type_kind_is_integer(lt.kind)) {
          type_error(ck, n->token, "`%s` requires integer operands, got %.*s",
                     binary_op_display_name(n->kind), str8_varg(type_ref_display(ctx_scratch(), lt)));
        } else {
          result = lt;
        }
      }
    } break;
    case TypedNodeKind_BinaryEq:
    case TypedNodeKind_BinaryNeq: {
      TypeRef lt = check_expr(ck, scope, n->binary.lhs);
      TypeRef rt = check_expr(ck, scope, n->binary.rhs);
      if (lt.kind != TypeKind_Unresolved && rt.kind != TypeKind_Unresolved) {
        if (!type_ref_equal(lt, rt)) {
          type_error(ck, n->token, "`%s` operands have mismatched types: %.*s vs %.*s",
                     binary_op_display_name(n->kind), str8_varg(type_ref_display(ctx_scratch(), lt)), str8_varg(type_ref_display(ctx_scratch(), rt)));
        } else if (!type_ref_is_comparable(ck, lt)) {
          type_error(ck, n->token, "`%s` requires a comparable type, got %.*s",
                     binary_op_display_name(n->kind), str8_varg(type_ref_display(ctx_scratch(), lt)));
        } else {
          result.kind = TypeKind_Bool;
        }
      }
    } break;
    case TypedNodeKind_BinaryLt:
    case TypedNodeKind_BinaryLe:
    case TypedNodeKind_BinaryGt:
    case TypedNodeKind_BinaryGe: {
      TypeRef lt = check_expr(ck, scope, n->binary.lhs);
      TypeRef rt = check_expr(ck, scope, n->binary.rhs);
      if (lt.kind != TypeKind_Unresolved && rt.kind != TypeKind_Unresolved) {
        if (!type_ref_equal(lt, rt)) {
          type_error(ck, n->token, "`%s` operands have mismatched types: %.*s vs %.*s",
                     binary_op_display_name(n->kind), str8_varg(type_ref_display(ctx_scratch(), lt)), str8_varg(type_ref_display(ctx_scratch(), rt)));
        } else if (!type_ref_is_comparable(ck, lt)) {
          type_error(ck, n->token, "`%s` requires an ordered type, got %.*s",
                     binary_op_display_name(n->kind), str8_varg(type_ref_display(ctx_scratch(), lt)));
        } else {
          result.kind = TypeKind_Bool;
        }
      }
    } break;
    case TypedNodeKind_LogicalAnd:
    case TypedNodeKind_LogicalOr: {
      check_expr(ck, scope, n->binary.lhs); // any type: C-style truthiness, as with `if`
      check_expr(ck, scope, n->binary.rhs);
      result.kind = TypeKind_Bool;
    } break;
    case TypedNodeKind_BinaryBitOr:
    case TypedNodeKind_BinaryBitAnd:
    case TypedNodeKind_BinaryBitXor: {
      TypeRef lt = check_expr(ck, scope, n->binary.lhs);
      TypeRef rt = check_expr(ck, scope, n->binary.rhs);
      if (lt.kind != TypeKind_Unresolved && rt.kind != TypeKind_Unresolved) {
        if (!type_ref_equal(lt, rt)) {
          type_error(ck, n->token, "`%s` operands have mismatched types: %.*s vs %.*s",
                     binary_op_display_name(n->kind), str8_varg(type_ref_display(ctx_scratch(), lt)), str8_varg(type_ref_display(ctx_scratch(), rt)));
        } else if (!type_ref_is_bitwise_ok(ck, lt)) {
          type_error(ck, n->token, "`%s` requires numeric or enum/flags operands, got %.*s",
                     binary_op_display_name(n->kind), str8_varg(type_ref_display(ctx_scratch(), lt)));
        } else {
          result = lt;
        }
      }
    } break;
    case TypedNodeKind_BinaryShl:
    case TypedNodeKind_BinaryShr: {
      // Unlike bit-or/and/xor, the operand types need not match: as in C,
      // `i32 << u8` is fine. Only the left operand must be bitwise-ok, the
      // right merely numeric, and the result follows the left.
      TypeRef lt = check_expr(ck, scope, n->binary.lhs);
      TypeRef rt = check_expr(ck, scope, n->binary.rhs);
      if (lt.kind != TypeKind_Unresolved && rt.kind != TypeKind_Unresolved) {
        if (!type_ref_is_bitwise_ok(ck, lt)) {
          type_error(ck, n->token, "`%s` requires a numeric or enum/flags left operand, got %.*s",
                     binary_op_display_name(n->kind), str8_varg(type_ref_display(ctx_scratch(), lt)));
        } else if (!type_kind_is_numeric(rt.kind)) {
          type_error(ck, n->token, "`%s` requires a numeric shift amount, got %.*s",
                     binary_op_display_name(n->kind), str8_varg(type_ref_display(ctx_scratch(), rt)));
        } else {
          result = lt;
        }
      }
    } break;
    case TypedNodeKind_EnumAccess: {
      EnumEntry* ee = enum_table_lookup(ck, n->enum_access.enum_name);
      if (!ee) {
        type_error(ck, n->token, "unknown enum/flags type `%.*s`", str8_varg(n->enum_access.enum_name));
        break;
      }
      TypedNode* decl  = &ck->tast->nodes[ee->decl];
      b32        found = false;
      foreach_index(i, decl->enum_decl.variant_count) {
        EnumVariant* v = &ck->tast->enum_variants[decl->enum_decl.variant_first + i];
        if (str8_match(v->name, n->enum_access.variant_name, 0)) { found = true; break; }
      }
      if (!found) {
        type_error(ck, n->token, "`%.*s` has no variant `%.*s`",
                   str8_varg(n->enum_access.enum_name), str8_varg(n->enum_access.variant_name));
        break;
      }
      result.kind = TypeKind_Named;
      result.name = n->enum_access.enum_name;
    } break;
    case TypedNodeKind_ReturnExpr: {
      if (!ck->in_function_body) {
        type_error(ck, n->token, "`return` used outside of a function body");
        break;
      }
      if (n->unary.expr == TYPED_NIL) {
        // Bare `(return)` -- only valid inside a `void`-returning fn.
        if (ck->current_fn_return_type.kind != TypeKind_Unresolved
            && ck->current_fn_return_type.kind != TypeKind_Void) {
          type_error(ck, n->token, "bare `return` needs a value -- the enclosing `fn` returns %.*s",
                     str8_varg(type_ref_display(ctx_scratch(), ck->current_fn_return_type)));
        }
        break;
      }
      adopt_int_literal_type(ck, n->unary.expr, ck->current_fn_return_type);
      TypeRef value_ty = check_expr(ck, scope, n->unary.expr);
      if (value_ty.kind != TypeKind_Unresolved && ck->current_fn_return_type.kind != TypeKind_Unresolved
          && !type_ref_assignable(value_ty, ck->current_fn_return_type)) {
        type_error(ck, n->token, "`return` value has type %.*s, but the enclosing `fn` returns %.*s",
                   str8_varg(type_ref_display(ctx_scratch(), value_ty)), str8_varg(type_ref_display(ctx_scratch(), ck->current_fn_return_type)));
      }
      // result stays Unresolved: a `return` yields no value to its
      // surrounding context. See TypedNodeKind_ReturnExpr, 3b.h.
    } break;
    case TypedNodeKind_IfExpr: {
      check_expr(ck, scope, n->if_expr.cond); // any type: C-style truthiness
      TypeRef tt = check_expr(ck, scope, n->if_expr.then_branch);
      if (n->if_expr.else_branch != TYPED_NIL) {
        TypeRef et = check_expr(ck, scope, n->if_expr.else_branch);
        if (tt.kind == TypeKind_Unresolved) {
          // The then-branch either diverges (a `return`) or already errored.
          // Either way any value this `if` produces comes from else, so the
          // else type is the right one to propagate: a real divergence needs
          // it for the enclosing return-type check, and an already-reported
          // error gains no second error from reusing it.
          result = et;
        } else if (et.kind == TypeKind_Unresolved) {
          result = tt; // symmetric: else diverges or already errored
        } else if (!type_ref_equal(tt, et)) {
          type_error(ck, n->token, "`if` branches have mismatched types: %.*s vs %.*s",
                     str8_varg(type_ref_display(ctx_scratch(), tt)), str8_varg(type_ref_display(ctx_scratch(), et)));
        } else {
          result = tt;
        }
      } else if (tt.kind == TypeKind_Unresolved) {
        // No else, and the then-branch diverges. Void, not Unresolved: on a
        // false condition this `if` produces nothing, exactly like the plain
        // no-else case below. Void is equally harmless mid-body (check_block
        // only reads the last statement's type) but does reach check_program's
        // body-vs-declared-return check when this is the last statement, so
        // `(fn f [] i32 (if cond (return 1)))` is caught rather than passing
        // with zero errors.
        result.kind = TypeKind_Void;
      } else {
        result.kind = TypeKind_Void; // no else -- no value when the condition is false
      }
    } break;
    case TypedNodeKind_BreakExpr:
    case TypedNodeKind_ContinueExpr: {
      // `result` stays Unresolved for both: control leaves, so no value
      // reaches the surrounding context. Exactly ReturnExpr's contract.
      const char* word = n->kind == TypedNodeKind_BreakExpr ? "break" : "continue";
      // The header test comes first: there IS an enclosing loop in that case,
      // so "outside of a loop" would be the wrong thing to say.
      if (ck->in_loop_header) {
        type_error(ck, n->token,
                   "`%s` cannot appear in a loop's header -- move it into the loop body", word);
      } else if (ck->loop_depth == 0) {
        type_error(ck, n->token, "`%s` used outside of a loop -- it needs an enclosing `while` or `for`", word);
      }
    } break;
    case TypedNodeKind_WhileExpr: {
      check_loop_header(ck, scope, n->while_expr.cond); // any type accepted -- C-style truthiness, same as `if`
      check_loop_body(ck, scope, n->while_expr.body); // Block -- result discarded; `while` always produces void
      result.kind = TypeKind_Void;
    } break;
    case TypedNodeKind_ForRangeExpr: {
      TypeRef begin_ty = check_loop_header(ck, scope, n->for_range.begin);
      TypeRef end_ty   = check_loop_header(ck, scope, n->for_range.end);
      // A TYPED_NIL step means the source wrote none (see lower_for).
      // Synthesizing the implicit `1` here, with `begin`'s type known, types
      // it to match the range instead of defaulting to i32 and tripping the
      // mismatch check below on every non-i32 range.
      if (n->for_range.step == TYPED_NIL) {
        TypedNode one_lit = {0};
        one_lit.token     = n->token;
        if (begin_ty.kind == TypeKind_F32 || begin_ty.kind == TypeKind_F64) {
          one_lit.kind                     = TypedNodeKind_FloatLiteral;
          one_lit.float_lit.value          = 1.0;
          one_lit.float_lit.explicit_type  = begin_ty.kind;
        } else {
          one_lit.kind                   = TypedNodeKind_IntLiteral;
          one_lit.int_lit.value          = 1;
          one_lit.int_lit.explicit_type  = begin_ty.kind;
        }
        TypedIndex step_idx = typed_push(ck->tast, one_lit);
        n = &ck->tast->nodes[idx]; // typed_push may have moved tast->nodes
        n->for_range.step = step_idx;
      }
      TypeRef step_ty = check_loop_header(ck, scope, n->for_range.step);
      if (begin_ty.kind == TypeKind_Unresolved) {
        // already reported deeper -- don't cascade
      } else if (!type_kind_is_numeric(begin_ty.kind)) {
        type_error(ck, n->token, "`for` range bounds must be numeric, got %.*s", str8_varg(type_ref_display(ctx_scratch(), begin_ty)));
      } else {
        if (end_ty.kind != TypeKind_Unresolved && !type_ref_equal(begin_ty, end_ty)) {
          type_error(ck, n->token, "`for` range bounds have mismatched types: %.*s vs %.*s",
                     str8_varg(type_ref_display(ctx_scratch(), begin_ty)), str8_varg(type_ref_display(ctx_scratch(), end_ty)));
        }
        if (step_ty.kind != TypeKind_Unresolved && !type_ref_equal(begin_ty, step_ty)) {
          type_error(ck, n->token, "`for` step has type %.*s, expected %.*s (same as the range bounds)",
                     str8_varg(type_ref_display(ctx_scratch(), step_ty)), str8_varg(type_ref_display(ctx_scratch(), begin_ty)));
        }
        // The loop variable's type is inferred from `begin`. Loop variables
        // are the only bindings never written with a type; val/var/fn params
        // are always explicit.
        u64 mark = scope_mark(scope);
        scope_bind_mutable(scope, n->for_range.var_name, begin_ty, n->token);
        check_loop_body(ck, scope, n->for_range.body);
        scope_pop_to(scope, mark);
      }
      result.kind = TypeKind_Void;
    } break;
    case TypedNodeKind_ForEachExpr: {
      TypeRef coll_ty = check_loop_header(ck, scope, n->for_each.collection);
      if (coll_ty.kind == TypeKind_Unresolved) {
        // already reported deeper -- don't cascade
      } else if (coll_ty.kind == TypeKind_Array || coll_ty.kind == TypeKind_Vector
              || coll_ty.kind == TypeKind_Set) {
        // Loop variables are inferred from the collection: index is u64 (the
        // position, matching `len` -- for a Set it is a slot-walk position with
        // no ordering guarantee), element is the declared element type. Array,
        // Vector and Set all keep that in .pointee, so one branch covers them.
        TypeRef idx_ty = {0};
        idx_ty.kind    = TypeKind_U64;
        check_for_each_body(ck, scope, idx, idx_ty, *coll_ty.pointee);
      } else if (coll_ty.kind == TypeKind_Map) {
        // Map requires the two-binding form: a key/value pair has no single
        // natural element to bind with `[item coll]`.
        if (!n->for_each.has_index) {
          type_error(ck, n->token,
            "Map iteration needs `[[k v] m]`, not `[item m]` -- there's no single natural element for a key/value pair");
        } else {
          check_for_each_body(ck, scope, idx, *coll_ty.map_key, *coll_ty.pointee);
        }
      } else {
        type_error(ck, n->token, "`for` can't iterate %.*s -- expected an array, Vector, Set, or Map",
                   str8_varg(type_ref_display(ctx_scratch(), coll_ty)));
      }
      result.kind = TypeKind_Void;
    } break;
    case TypedNodeKind_ParallelForExpr: {
      if (!ck->in_parallel_block) {
        type_error(ck, n->token, "`parallel-for` used outside of a `parallel` block");
        break;
      }
      TypeRef count_ty = check_loop_header(ck, scope, n->parallel_for.count);
      if (count_ty.kind == TypeKind_Unresolved) {
        // already reported deeper -- don't cascade
      } else if (!type_kind_is_integer(count_ty.kind)) {
        type_error(ck, n->token, "`parallel-for` count must be an integer, got %.*s",
                   str8_varg(type_ref_display(ctx_scratch(), count_ty)));
      } else {
        // Loop variable inferred from `count`, as in ForRangeExpr above.
        u64 mark = scope_mark(scope);
        scope_bind_mutable(scope, n->parallel_for.var_name, count_ty, n->token);
        TypeRef body_ty = check_loop_body(ck, scope, n->parallel_for.body);
        scope_pop_to(scope, mark);
        if (body_ty.kind != TypeKind_Unresolved && body_ty.kind != TypeKind_Void) {
          type_error(ck, n->token,
                     "`parallel-for` body must be void, got %.*s -- wrap the last expression in `(void ...)` if needed",
                     str8_varg(type_ref_display(ctx_scratch(), body_ty)));
        }
      }
      result.kind = TypeKind_Void;
    } break;
    case TypedNodeKind_Block: {
      result = check_block(ck, scope, idx);
    } break;
    case TypedNodeKind_LetExpr: {
      // `let` is the only binding form with type inference; val/var/fn params
      // are always explicitly typed. A binding arrives Unresolved three ways:
      // a 2-slot `[name init]` the user wrote without a type (lower_let's
      // let_slot_looks_like_type), a destructured local
      // (lower_destructure_map/_vector), or a synthesized binding
      // (lower_some_thread_step's `tmp`). All three infer from the checked
      // initializer instead of validating against a type never written.
      u64 mark = scope_mark(scope);
      foreach_index(i, n->let_expr.binding_count) {
        Binding* b       = &ck->tast->bindings[n->let_expr.binding_first + i];
        TypeRef  init_ty = check_init_expr(ck, scope, b->init, b->type);
        if (b->type.kind == TypeKind_Unresolved) {
          // Mutate the Binding in place: cg_declare reads b->type directly,
          // so the resolved type has to land there, not only in the return.
          b->type = init_ty;
        } else if (init_ty.kind != TypeKind_Unresolved && !type_ref_assignable(init_ty, b->type)) {
          type_error(ck, n->token, "`let` binding `%.*s` declared as %.*s but initializer is %.*s",
                     str8_varg(b->name), str8_varg(type_ref_display(ctx_scratch(), b->type)), str8_varg(type_ref_display(ctx_scratch(), init_ty)));
        }
        scope_bind_mutable(scope, b->name, b->type, n->token);
      }
      result = check_expr(ck, scope, n->let_expr.body);
      n      = &ck->tast->nodes[idx]; // typed_push may have moved tast->nodes
      // Bindings occupy slots mark..mark+count in order (the loop above binds
      // exactly one apiece, and check_init_expr pops whatever it pushed), so
      // each one's was_read reads back positionally. codegen wants this for the
      // bindings it synthesizes -- destructuring, multi-return `let` -- which
      // routinely go unmentioned by the body. Re-index rather than reusing the
      // `b` above: check_expr may have grown either dyn array.
      foreach_index(i, n->let_expr.binding_count) {
        ck->tast->bindings[n->let_expr.binding_first + i].is_read = scope->entries[mark + i].was_read;
      }
      scope_pop_to(scope, mark); // bindings end with the let, as in the generated C
    } break;
    case TypedNodeKind_ScratchExpr: {
      // The bound name has no initializer to check -- it is always the
      // scratch arena (see cg_scratch_expr) -- so unlike LetExpr this only
      // binds, checks the body, and pops.
      u64 mark = scope_mark(scope);
      scope_bind_mutable(scope, n->scratch_expr.var_name, (TypeRef){ .kind = TypeKind_Arena }, n->token);
      result = check_expr(ck, scope, n->scratch_expr.body);
      scope_pop_to(scope, mark);
    } break;
    case TypedNodeKind_ParallelExpr: {
      // Captures are explicit `name init` pairs (see lower_parallel). Each
      // init is checked in the outer scope, since the caller evaluates it
      // once before forking; the names are then bound for the body.
      if (ck->in_parallel_block) {
        type_error(ck, n->token, "nested `parallel` blocks are not supported");
        break;
      }
      u64 mark = scope_mark(scope);
      foreach_index(i, n->parallel_expr.capture_count) {
        Binding* b       = &ck->tast->bindings[n->parallel_expr.capture_first + i];
        TypeRef  init_ty = check_init_expr(ck, scope, b->init, b->type);
        b->type          = init_ty; // always Unresolved coming in; see lower_parallel
        scope_bind_mutable(scope, b->name, b->type, n->token);
      }

      // The body is checked seeing ONLY its captures, its own bindings, and
      // globals -- never an enclosing param or local, which do not exist on
      // the lane thread the body actually runs on. Until this hid them, such a
      // reference type-checked and then failed in the C compiler
      // ("`outer` undeclared" inside `__3b_parallel_fn_N`), and would have
      // silently WORKED on the bytecode VM, whose serial fallback compiles the
      // body inline. See Scope.hidden_lo.
      ck->in_parallel_block = true;
      ck->has_parallel       = true;
      scope->hidden_lo      = ck->fn_scope_base;
      scope->hidden_hi      = mark;
      // The body compiles to a separate C function natively, so an enclosing
      // loop is not reachable from inside it -- `break`/`continue` here must
      // see zero loops even when the `parallel` itself sits in one.
      u32 outer_loop_depth  = ck->loop_depth;
      b32 outer_loop_header = ck->in_loop_header; // a `parallel` can itself sit in a loop header;
      ck->loop_depth        = 0;                  // its body is a fresh function either way
      ck->in_loop_header    = false;
      TypeRef body_ty = check_expr(ck, scope, n->parallel_expr.body);
      ck->loop_depth        = outer_loop_depth;
      ck->in_loop_header    = outer_loop_header;
      scope->hidden_lo      = 0; // nesting is rejected above, so there is never an outer
      scope->hidden_hi      = 0; // hidden range to restore
      ck->in_parallel_block = false; // nesting is rejected above, so false is always right

      if (body_ty.kind != TypeKind_Unresolved && body_ty.kind != TypeKind_Void) {
        type_error(ck, n->token,
                   "`parallel` block body must be void, got %.*s -- wrap the last expression in `(void ...)` if needed",
                   str8_varg(type_ref_display(ctx_scratch(), body_ty)));
      }
      scope_pop_to(scope, mark);
      result.kind = TypeKind_Void;
    } break;
    case TypedNodeKind_StructLiteral: {
      if (n->struct_lit.type_name.size == 0) {
        // lower_bare_map_literal's sentinel: a `{...}` call argument whose
        // position didn't resolve to a named-struct parameter -- unknown
        // callee, arity mismatch, indirect call, or a non-struct param (see
        // the TypedNodeKind_Call prelude). A call argument is the only place
        // a bare map literal's struct type can be inferred.
        type_error(ck, n->token, "a bare `{...}` map literal here has no inferable struct type -- "
                   "it's only valid as a call argument to a parameter whose declared type is a struct");
        break;
      }
      StructEntry* se = struct_table_lookup(ck, n->struct_lit.type_name);
      if (!se) {
        type_error(ck, n->token, "unknown struct type `%.*s`", str8_varg(n->struct_lit.type_name));
        break;
      }
      TypedNode* decl = &ck->tast->nodes[se->decl];
      // A struct or union with an anonymous member has no meaningful declared
      // field count to check against: overlapping union views are not all
      // filled at once, and an anonymous member's leaf fields don't appear in
      // the outer field_count. Skip the count check there and accept whatever
      // valid fields the literal names, matching find_field_recursive.
      b32 has_anon_field = false;
      foreach_index(j, decl->struct_decl.field_count) {
        if (ck->tast->params[decl->struct_decl.field_first + j].is_anon) { has_anon_field = true; break; }
      }
      if (!has_anon_field && n->struct_lit.field_count != decl->struct_decl.field_count) {
        type_error(ck, n->token, "`%.*s` literal has %u field(s), struct declares %u",
                   str8_varg(n->struct_lit.type_name), (u32)n->struct_lit.field_count,
                   (u32)decl->struct_decl.field_count);
      }
      foreach_index(i, n->struct_lit.field_count) {
        FieldInit* fi        = &ck->tast->field_inits[n->struct_lit.field_first + i];
        Param*     matched   = find_field_recursive(ck, decl, fi->name);
        TypeRef value_ty = matched ? check_init_expr(ck, scope, fi->value, matched->type)
                                    : check_expr(ck, scope, fi->value);
        if (!matched) {
          type_error(ck, n->token, "`%.*s` has no field `%.*s`",
                     str8_varg(n->struct_lit.type_name), str8_varg(fi->name));
        } else if (value_ty.kind != TypeKind_Unresolved && !type_ref_assignable(value_ty, matched->type)) {
          type_error(ck, n->token, "field `%.*s` of `%.*s`: expected %.*s, got %.*s",
                     str8_varg(fi->name), str8_varg(n->struct_lit.type_name),
                     str8_varg(type_ref_display(ctx_scratch(), matched->type)), str8_varg(type_ref_display(ctx_scratch(), value_ty)));
        }
      }
      result.kind = TypeKind_Named;
      result.name = n->struct_lit.type_name;
    } break;
    case TypedNodeKind_PushAlloc: {
      TypeRef arena_ty = check_expr(ck, scope, n->push_alloc.arena);
      if (arena_ty.kind != TypeKind_Unresolved && arena_ty.kind != TypeKind_Arena) {
        type_error(ck, n->token, "`push` expects an arena, got %.*s", str8_varg(type_ref_display(ctx_scratch(), arena_ty)));
      }
      TypeRef count_ty = check_expr(ck, scope, n->push_alloc.count);
      if (count_ty.kind != TypeKind_Unresolved && !type_kind_is_numeric(count_ty.kind)) {
        type_error(ck, n->token, "`push` count must be numeric, got %.*s", str8_varg(type_ref_display(ctx_scratch(), count_ty)));
      }
      TypeRef* boxed = push_one(ctx_perm(), TypeRef);
      *boxed          = n->push_alloc.elem_type;
      result.kind     = TypeKind_Pointer;
      result.pointee  = boxed;
    } break;
    case TypedNodeKind_AllocExpr: {
      TypeRef count_ty = check_expr(ck, scope, n->alloc_expr.count);
      if (count_ty.kind != TypeKind_Unresolved && !type_kind_is_numeric(count_ty.kind)) {
        type_error(ck, n->token, "`alloc` count must be numeric, got %.*s", str8_varg(type_ref_display(ctx_scratch(), count_ty)));
      }
      TypeRef* boxed = push_one(ctx_perm(), TypeRef);
      *boxed          = n->alloc_expr.elem_type;
      result.kind     = TypeKind_Pointer;
      result.pointee  = boxed;
    } break;
    case TypedNodeKind_PushCopy: {
      TypeRef arena_ty = check_expr(ck, scope, n->push_copy.arena);
      if (arena_ty.kind != TypeKind_Unresolved && arena_ty.kind != TypeKind_Arena) {
        type_error(ck, n->token, "`push` expects an arena, got %.*s", str8_varg(type_ref_display(ctx_scratch(), arena_ty)));
      }
      TypeRef value_ty = check_expr(ck, scope, n->push_copy.value);
      if (value_ty.kind != TypeKind_Unresolved) {
        TypeRef* boxed = push_one(ctx_perm(), TypeRef);
        *boxed          = value_ty;
        result.kind     = TypeKind_Pointer;
        result.pointee  = boxed;
      }
    } break;
    case TypedNodeKind_DynPush: {
      // Shared by `dyn-push` (any mutable pointer-typed local) and
      // `vector-push`. A Vector is a dyn-push-grown pointer (see
      // TypeKind_Vector, 3b.h), so both lower to this node.
      TypeRef arena_ty = check_expr(ck, scope, n->dyn_push.arena);
      if (arena_ty.kind != TypeKind_Unresolved && arena_ty.kind != TypeKind_Arena) {
        type_error(ck, n->token, "`dyn-push`/`vector-push` expects an arena, got %.*s", str8_varg(type_ref_display(ctx_scratch(), arena_ty)));
      }
      TypeRef value_ty = check_expr(ck, scope, n->dyn_push.value);
      if (n->dyn_push.is_field_target) {
        // `(vector-push arena (. base field ...) value)`. target_expr is a
        // FieldAccess node from lower_dot, checked as a read and then given
        // the same Pointer/Vector checks as the identifier path below, the
        // way SetTargetKind_Field handles `set`. Mutability needs no separate
        // check here: it follows from whatever checking the base expression
        // already requires.
        TypeRef target_ty = check_expr(ck, scope, n->dyn_push.target_expr);
        b32 target_pointer_like = target_ty.kind == TypeKind_Pointer || target_ty.kind == TypeKind_Vector;
        if (target_ty.kind != TypeKind_Unresolved && !target_pointer_like) {
          type_error(ck, n->token, "`dyn-push`/`vector-push` target must be a pointer or Vector field, got %.*s",
                     str8_varg(type_ref_display(ctx_scratch(), target_ty)));
        } else if (target_pointer_like && target_ty.pointee != NULL
                && value_ty.kind != TypeKind_Unresolved && !type_ref_equal(value_ty, *target_ty.pointee)) {
          type_error(ck, n->token, "`dyn-push`/`vector-push` target holds %.*s, value is %.*s",
                     str8_varg(type_ref_display(ctx_scratch(), *target_ty.pointee)), str8_varg(type_ref_display(ctx_scratch(), value_ty)));
        }
      } else {
        ScopeEntry* target = scope_lookup_entry(scope, n->dyn_push.arr_name);
        b32 target_pointer_like = target && (target->type.kind == TypeKind_Pointer || target->type.kind == TypeKind_Vector);
        if (!target) {
          type_error(ck, n->token, "`%.*s` is undefined", str8_varg(n->dyn_push.arr_name));
        } else if (!target->is_mutable) {
          type_error(ck, n->token, "`%.*s` must be mutable -- declare it with `var`, not `val`",
                     str8_varg(n->dyn_push.arr_name));
        } else if (target->type.kind != TypeKind_Unresolved && !target_pointer_like) {
          type_error(ck, n->token, "`%.*s` must be a pointer or Vector type (e.g. `i32*` or `(Vector i32)`), got %.*s",
                     str8_varg(n->dyn_push.arr_name), str8_varg(type_ref_display(ctx_scratch(), target->type)));
        } else if (target_pointer_like && target->type.pointee != NULL
                && value_ty.kind != TypeKind_Unresolved && !type_ref_equal(value_ty, *target->type.pointee)) {
          type_error(ck, n->token, "`%.*s` holds %.*s, value is %.*s",
                     str8_varg(n->dyn_push.arr_name), str8_varg(type_ref_display(ctx_scratch(), *target->type.pointee)),
                     str8_varg(type_ref_display(ctx_scratch(), value_ty)));
        }
      }
      result.kind = TypeKind_Void;
    } break;
    case TypedNodeKind_CommitExpr: {
      TypeRef dst_ty = check_expr(ck, scope, n->commit_expr.dst_arena);
      if (dst_ty.kind != TypeKind_Unresolved && dst_ty.kind != TypeKind_Arena) {
        type_error(ck, n->token, "`commit`'s first argument must be an arena, got %.*s", str8_varg(type_ref_display(ctx_scratch(), dst_ty)));
      }
      TypeRef src_ty = check_expr(ck, scope, n->commit_expr.src);
      if (src_ty.kind == TypeKind_Unresolved) {
        // already reported deeper -- don't cascade
      } else if (src_ty.kind != TypeKind_Pointer) {
        type_error(ck, n->token, "`commit` expects a dyn-push-grown array (a pointer), got %.*s",
                   str8_varg(type_ref_display(ctx_scratch(), src_ty)));
      } else {
        result = src_ty; // same element type, now living in the destination arena
      }
    } break;
    case TypedNodeKind_SetExpr: {
      TypeRef value_ty = check_expr(ck, scope, n->set_expr.value);
      if (n->set_expr.target_kind == SetTargetKind_Deref) {
        TypeRef ptr_ty = check_expr(ck, scope, n->set_expr.target_expr);
        if (ptr_ty.kind == TypeKind_Unresolved) {
          // already reported deeper -- don't cascade
        } else if (ptr_ty.kind != TypeKind_Pointer) {
          type_error(ck, n->token, "cannot `set` through `deref` of non-pointer type %.*s",
                     str8_varg(type_ref_display(ctx_scratch(), ptr_ty)));
        } else if (value_ty.kind != TypeKind_Unresolved && !type_ref_assignable(value_ty, *ptr_ty.pointee)) {
          type_error(ck, n->token, "cannot `set` through pointer: pointee is %.*s, value is %.*s",
                     str8_varg(type_ref_display(ctx_scratch(), *ptr_ty.pointee)), str8_varg(type_ref_display(ctx_scratch(), value_ty)));
        } else {
          result = *ptr_ty.pointee;
        }
      } else if (n->set_expr.target_kind == SetTargetKind_Index) {
        TypeRef base_ty  = check_expr(ck, scope, n->set_expr.index_base);
        TypeRef index_ty = check_expr(ck, scope, n->set_expr.index_index);
        if (index_ty.kind != TypeKind_Unresolved && !type_kind_is_numeric(index_ty.kind)) {
          type_error(ck, n->token, "`nth` index must be numeric, got %.*s", str8_varg(type_ref_display(ctx_scratch(), index_ty)));
        }
        if (base_ty.kind == TypeKind_Unresolved) {
          // already reported deeper -- don't cascade
        } else if (base_ty.kind != TypeKind_Array && base_ty.kind != TypeKind_Pointer && base_ty.kind != TypeKind_Vector) {
          type_error(ck, n->token, "cannot `set` through `nth` of non-array, non-pointer, non-Vector type %.*s",
                     str8_varg(type_ref_display(ctx_scratch(), base_ty)));
        } else if (value_ty.kind != TypeKind_Unresolved && !type_ref_assignable(value_ty, *base_ty.pointee)) {
          type_error(ck, n->token, "cannot `set` through `nth`: element type is %.*s, value is %.*s",
                     str8_varg(type_ref_display(ctx_scratch(), *base_ty.pointee)), str8_varg(type_ref_display(ctx_scratch(), value_ty)));
        } else {
          result = *base_ty.pointee;
        }
      } else if (n->set_expr.target_kind == SetTargetKind_Field) {
        // `target_expr` is a lowered FieldAccess chain (lower_set_target).
        // Checking it as a read resolves auto-deref, struct lookup and
        // unknown-field errors; only the type comparison is left.
        TypeRef field_ty = check_expr(ck, scope, n->set_expr.target_expr);
        if (field_ty.kind == TypeKind_Unresolved) {
          // already reported deeper -- don't cascade
        } else if (value_ty.kind != TypeKind_Unresolved && !type_ref_assignable(value_ty, field_ty)) {
          type_error(ck, n->token, "cannot `set` field: declared as %.*s, value is %.*s",
                     str8_varg(type_ref_display(ctx_scratch(), field_ty)), str8_varg(type_ref_display(ctx_scratch(), value_ty)));
        } else {
          result = field_ty;
        }
      } else {
        ScopeEntry* target = scope_lookup_entry(scope, n->set_expr.target_name);
        if (!target) {
          type_error(ck, n->token, "cannot `set` undefined identifier `%.*s`", str8_varg(n->set_expr.target_name));
        } else if (!target->is_mutable) {
          type_error(ck, n->token, "cannot `set` `%.*s` -- it was declared with `val` (immutable)",
                     str8_varg(n->set_expr.target_name));
        } else if (value_ty.kind != TypeKind_Unresolved && !type_ref_assignable(value_ty, target->type)) {
          type_error(ck, n->token, "cannot `set` `%.*s`: declared as %.*s, value is %.*s",
                     str8_varg(n->set_expr.target_name), str8_varg(type_ref_display(ctx_scratch(), target->type)),
                     str8_varg(type_ref_display(ctx_scratch(), value_ty)));
        } else {
          result = target->type; // `set` is an expression, like C's `=`
        }
      }
    } break;
    case TypedNodeKind_IndexAccess: {
      TypeRef base_ty  = check_expr(ck, scope, n->index_access.base);
      TypeRef index_ty = check_expr(ck, scope, n->index_access.index);
      if (index_ty.kind != TypeKind_Unresolved && !type_kind_is_numeric(index_ty.kind)) {
        type_error(ck, n->token, "`nth` index must be numeric, got %.*s", str8_varg(type_ref_display(ctx_scratch(), index_ty)));
      }
      if (base_ty.kind == TypeKind_Array || base_ty.kind == TypeKind_Pointer || base_ty.kind == TypeKind_Vector) {
        result = *base_ty.pointee;
      } else if (base_ty.kind != TypeKind_Unresolved) {
        type_error(ck, n->token, "`nth` requires an array, pointer, or Vector, got %.*s", str8_varg(type_ref_display(ctx_scratch(), base_ty)));
      }
    } break;
    case TypedNodeKind_ArrayLiteral: {
      type_error(ck, n->token,
        "array literal needs a known element type from context (e.g. a `let`/`val`/`var` type annotation)");
    } break;
    case TypedNodeKind_PositionalAccess: {
      // One slot of a `[a b] source` destructure. Whether slot N means a
      // struct field or an array/pointer index depends on `base`'s type,
      // unknown at lowering time, so this rewrites the node in place into a
      // FieldAccess or IndexAccess. Codegen never sees this kind.
      TypedIndex base_idx = n->positional_access.base;
      u32        slot     = n->positional_access.slot;
      TypeRef    base_ty  = check_expr(ck, scope, base_idx);
      if (base_ty.kind == TypeKind_Unresolved) {
        break; // already reported deeper -- don't cascade
      }
      if (base_ty.kind == TypeKind_Array || base_ty.kind == TypeKind_Pointer) {
        if (base_ty.kind == TypeKind_Array && slot >= base_ty.count) {
          type_error(ck, n->token, "positional destructure slot %u is out of range -- array has %u element(s)",
                     (u32)(slot + 1), (u32)base_ty.count);
          break;
        }
        TypedNode idx_lit     = {0};
        idx_lit.kind          = TypedNodeKind_IntLiteral;
        idx_lit.token         = n->token;
        idx_lit.int_lit.value = (i64)slot;
        TypedIndex idx_idx    = typed_push(ck->tast, idx_lit);
        n = &ck->tast->nodes[idx]; // typed_push may have moved tast->nodes
        n->kind                = TypedNodeKind_IndexAccess;
        n->index_access.base   = base_idx;
        n->index_access.index  = idx_idx;
        result = *base_ty.pointee;
      } else if (base_ty.kind == TypeKind_Named) {
        StructEntry* se = struct_table_lookup(ck, base_ty.name);
        if (!se) {
          type_error(ck, n->token, "unknown struct type `%.*s`", str8_varg(base_ty.name));
          break;
        }
        TypedNode* decl = &ck->tast->nodes[se->decl];
        if (slot >= decl->struct_decl.field_count) {
          type_error(ck, n->token, "positional destructure slot %u is out of range -- `%.*s` only has %u field(s)",
                     (u32)(slot + 1), str8_varg(base_ty.name), (u32)decl->struct_decl.field_count);
          break;
        }
        Param* field                 = &ck->tast->params[decl->struct_decl.field_first + slot];
        n->kind                      = TypedNodeKind_FieldAccess;
        n->field_access.base         = base_idx;
        n->field_access.field        = field->name;
        n->field_access.auto_deref   = false;
        result = field->type;
      } else {
        type_error(ck, n->token, "positional destructuring requires an array, pointer, or struct, got %.*s",
                   str8_varg(type_ref_display(ctx_scratch(), base_ty)));
      }
    } break;
    case TypedNodeKind_DotHop: {
      // One hop of `(. base f1 f2 ...)`/`(& base f1 f2 ...)`. Same
      // rewrite-once-the-type-is-known approach as PositionalAccess above,
      // forking on struct field vs Map key rather than field vs index.
      //
      // Every field is copied into a local first, `token` included: the first
      // check_expr below can already move tast->nodes, so `n` is stale from
      // there on and every read goes through `tok`, never `n->token`.
      TypedIndex base_idx    = n->dot_hop.base;
      b32        auto_deref  = n->dot_hop.auto_deref;
      String8    field_name  = n->dot_hop.field_name;
      Token      field_token = n->dot_hop.field_token;
      TypedIndex key_expr    = n->dot_hop.key_expr;
      Token      tok         = n->token;

      TypeRef raw_base_ty = check_expr(ck, scope, base_idx);
      if (raw_base_ty.kind == TypeKind_Unresolved) break; // already reported deeper -- don't cascade
      b32     deref_needed = auto_deref && raw_base_ty.kind == TypeKind_Pointer;
      TypeRef base_ty       = deref_needed ? *raw_base_ty.pointee : raw_base_ty;

      if (base_ty.kind == TypeKind_Map) {
        // Duplicates map-get's key validation instead of rewriting and
        // recursing through check_expr: recursing would check `base_idx`
        // twice, and since each hop of a `.` chain has the previous hop as
        // its base, that cost compounds down the chain. Keep the duplicated
        // messages and semantics in step with the `map-get` Call case.
        TypeRef key_ty = check_expr(ck, scope, key_expr);
        if (key_ty.kind != TypeKind_Unresolved && base_ty.map_key
            && !type_ref_equal(key_ty, *base_ty.map_key)) {
          type_error(ck, tok, "map key: expected %.*s, got %.*s",
                     str8_varg(type_ref_display(ctx_scratch(), *base_ty.map_key)), str8_varg(type_ref_display(ctx_scratch(), key_ty)));
        }
        // Map codegen takes the Map by value and takes its address itself, so
        // a `Map*` base needs a real UnaryDeref node spliced in to hand the
        // rewritten `map-get` a plain Map. FieldAccess needs no such node --
        // it resolves a pointer base syntactically, `.` versus `->`.
        TypedIndex map_arg_idx = base_idx;
        if (deref_needed) {
          TypedNode deref_n   = {0};
          deref_n.kind        = TypedNodeKind_UnaryDeref;
          deref_n.token       = tok;
          deref_n.unary.expr  = base_idx;
          map_arg_idx = typed_push(ck->tast, deref_n);
          n = &ck->tast->nodes[idx]; // typed_push may have moved tast->nodes
          check_expr(ck, scope, map_arg_idx); // fill in resolved_types for the synthesized deref;
                                               // codegen reads it like any other arg
        }
        u32 arg_first = (u32)dyn_count(ck->tast->extra);
        dyn_push(ck->tast->arena, ck->tast->extra, map_arg_idx);
        dyn_push(ck->tast->arena, ck->tast->extra, key_expr);
        n = &ck->tast->nodes[idx]; // dyn_push above may have moved tast->nodes
        n->kind               = TypedNodeKind_Call;
        n->call.callee        = str8_lit("map-get");
        n->call.callee_token  = tok;
        n->call.arg_first     = arg_first;
        n->call.arg_count     = 2;
        if (base_ty.pointee) {
          TypeRef* boxed = push_one(ctx_perm(), TypeRef);
          *boxed          = *base_ty.pointee;
          result.kind     = TypeKind_Pointer;
          result.pointee  = boxed;
        }
      } else if (base_ty.kind == TypeKind_Named) {
        if (field_name.size == 0) {
          type_error(ck, tok, "`.` needs a plain field name here -- `%.*s` is a struct, not a Map",
                     str8_varg(base_ty.name));
          break;
        }
        StructEntry* se = struct_table_lookup(ck, base_ty.name);
        if (!se) {
          type_error(ck, tok, "unknown struct type `%.*s`", str8_varg(base_ty.name));
          break;
        }
        TypedNode* decl    = &ck->tast->nodes[se->decl];
        Param*     matched = find_field_recursive(ck, decl, field_name);
        if (!matched) {
          type_error(ck, tok, "`%.*s` has no field `%.*s`", str8_varg(base_ty.name), str8_varg(field_name));
          break;
        }
        n = &ck->tast->nodes[idx]; // the check_expr(base_idx) above may have moved tast->nodes
        n->kind                     = TypedNodeKind_FieldAccess;
        n->field_access.base        = base_idx; // not map_arg_idx: codegen re-derives the deref from
                                                 // `auto_deref` and the base's un-peeled type
        n->field_access.field       = field_name;
        n->field_access.field_token = field_token;
        n->field_access.auto_deref  = auto_deref;
        // The struct reading won, so `key_expr` -- the same hop lowered a second time, as the
        // Map-key expression it might have been -- is now unreachable, and was never checked, so
        // it keeps an Unresolved type. Retiring it to Nil keeps it from answering LSP position
        // queries for this hop, where it would otherwise beat the FieldAccess to the field name's
        // own token and report "<inferred>". Nothing references it once the rewrite above lands.
        ck->tast->nodes[key_expr].kind = TypedNodeKind_Nil;
        result = matched->type;
      } else {
        type_error(ck, tok, "`.` requires a struct or Map here, got %.*s", str8_varg(type_ref_display(ctx_scratch(), base_ty)));
      }
    } break;
    case TypedNodeKind_ParseNumber: {
      // `(string-to-i32 s)` and friends. lower_parse_number interned
      // `result_struct_name` and spliced the struct in via pending_toplevel,
      // so check_program's struct pass has already registered it and only the
      // argument type is left to check.
      TypeRef arg_ty = check_expr(ck, scope, n->parse_number.arg);
      if (arg_ty.kind != TypeKind_Unresolved && arg_ty.kind != TypeKind_String) {
        TypeRef target_display_ty = {0};
        target_display_ty.kind    = n->parse_number.target_kind;
        type_error(ck, n->token, "`string-to-%.*s` requires a string argument, got %.*s",
                   str8_varg(type_ref_display(ctx_scratch(), target_display_ty)), str8_varg(type_ref_display(ctx_scratch(), arg_ty)));
      }
      result.kind = TypeKind_Named;
      result.name = n->parse_number.result_struct_name;
    } break;
    case TypedNodeKind_IndexOf: {
      // `(vector-index-of v x)`. lower_index_of interned
      // `result_struct_name` as the fixed `(bool u64)` shape; the operands
      // get the same type_ref_equal + type_ref_is_comparable gate as
      // `vector-contains?`.
      TypeRef vec_ty    = check_expr(ck, scope, n->index_of.vec);
      TypeRef needle_ty = check_expr(ck, scope, n->index_of.needle);
      if (vec_ty.kind != TypeKind_Unresolved && vec_ty.kind != TypeKind_Vector) {
        type_error(ck, n->token, "`vector-index-of` requires a Vector as its first argument, got %.*s",
                   str8_varg(type_ref_display(ctx_scratch(), vec_ty)));
      } else if (vec_ty.kind == TypeKind_Vector && needle_ty.kind != TypeKind_Unresolved) {
        if (!type_ref_equal(*vec_ty.pointee, needle_ty)) {
          type_error(ck, n->token, "`vector-index-of`'s second argument must match the Vector's element type %.*s, got %.*s",
                     str8_varg(type_ref_display(ctx_scratch(), *vec_ty.pointee)), str8_varg(type_ref_display(ctx_scratch(), needle_ty)));
        } else if (!type_ref_is_comparable(ck, needle_ty)) {
          type_error(ck, n->token, "`vector-index-of` requires a comparable element type, got %.*s",
                     str8_varg(type_ref_display(ctx_scratch(), needle_ty)));
        }
      }
      result.kind = TypeKind_Named;
      result.name = n->index_of.result_struct_name;
    } break;
    case TypedNodeKind_CheckedMath: {
      // `(sqrt-checked x)` and friends. lower_checked_math interned both
      // `f32_struct_name` and `f64_struct_name`; the arguments get the same
      // checks as the unchecked libm builtins below, and the result picks
      // whichever struct matches.
      TypeRef arg_ty = check_expr(ck, scope, n->checked_math.arg);
      if (arg_ty.kind == TypeKind_Unresolved) break;
      if (arg_ty.kind != TypeKind_F32 && arg_ty.kind != TypeKind_F64) {
        type_error(ck, n->token, "`%.*s-checked` requires an f32 or f64 argument, got %.*s",
                   str8_varg(n->checked_math.libm_name), str8_varg(type_ref_display(ctx_scratch(), arg_ty)));
        break;
      }
      if (n->checked_math.arg2 != TYPED_NIL) {
        TypeRef arg2_ty = check_expr(ck, scope, n->checked_math.arg2);
        if (arg2_ty.kind == TypeKind_Unresolved) break;
        if (!type_ref_equal(arg_ty, arg2_ty)) {
          type_error(ck, n->token, "`%.*s-checked` operands have mismatched types: %.*s vs %.*s",
                     str8_varg(n->checked_math.libm_name), str8_varg(type_ref_display(ctx_scratch(), arg_ty)),
                     str8_varg(type_ref_display(ctx_scratch(), arg2_ty)));
          break;
        }
      }
      result.kind = TypeKind_Named;
      result.name = (arg_ty.kind == TypeKind_F32) ? n->checked_math.f32_struct_name : n->checked_math.f64_struct_name;
    } break;
    case TypedNodeKind_ConstDecl:
    case TypedNodeKind_VarDecl: {
      // check_block handles the legitimate block-statement position without
      // calling check_expr, so reaching here means the `val`/`var` sits
      // somewhere it cannot, e.g. `(+ 1 (var x i32 5))`.
      type_error(ck, n->token, "`%s` can only appear as a statement inside a block "
                 "(a function body, `do`, `let` body, `while`/`for` body), not as a value here",
                 n->kind == TypedNodeKind_ConstDecl ? "val" : "var");
    } break;
    case TypedNodeKind_UnaryDeref: {
      TypeRef inner_ty = check_expr(ck, scope, n->unary.expr);
      if (inner_ty.kind == TypeKind_Unresolved) {
        // already reported deeper -- don't cascade
      } else if (inner_ty.kind != TypeKind_Pointer) {
        type_error(ck, n->token, "cannot `deref` non-pointer type %.*s", str8_varg(type_ref_display(ctx_scratch(), inner_ty)));
      } else {
        result = *inner_ty.pointee;
      }
    } break;
    case TypedNodeKind_UnaryAddr: {
      // The operand must be an lvalue (addr_operand_is_valid, above). `&5`,
      // `&(a + b)` and the like are temporaries; catching them here beats
      // letting gcc reject the generated C and report a location in it.
      TypedIndex operand_idx = n->unary.expr;
      TypeRef    inner_ty    = check_expr(ck, scope, operand_idx);
      TypedNode* operand     = &ck->tast->nodes[operand_idx]; // check_expr may have moved tast->nodes
      if (operand_idx != TYPED_NIL && !addr_operand_is_valid(operand)) {
        type_error(ck, n->token, "'&' requires an addressable value (a variable, field access, "
                   "index access, or deref) -- this is a temporary with no address");
      } else if (inner_ty.kind != TypeKind_Unresolved) {
        TypeRef* boxed = push_one(ctx_perm(), TypeRef);
        *boxed          = inner_ty;
        result.kind     = TypeKind_Pointer;
        result.pointee  = boxed;
      }
    } break;
    case TypedNodeKind_LogicalNot: {
      check_expr(ck, scope, n->unary.expr); // any type: C-style truthiness
      result.kind = TypeKind_Bool;
    } break;
    case TypedNodeKind_UnaryBitNot: {
      TypeRef inner_ty = check_expr(ck, scope, n->unary.expr);
      if (inner_ty.kind != TypeKind_Unresolved) {
        if (!type_ref_is_bitwise_ok(ck, inner_ty)) {
          type_error(ck, n->token, "`bit-not` requires a numeric or enum/flags operand, got %.*s",
                     str8_varg(type_ref_display(ctx_scratch(), inner_ty)));
        } else {
          result = inner_ty; // keeps the operand's type, unlike `not`
        }
      }
    } break;
    case TypedNodeKind_UnaryNeg: {
      TypeRef inner_ty = check_expr(ck, scope, n->unary.expr);
      if (inner_ty.kind != TypeKind_Unresolved) {
        if (!type_kind_is_numeric(inner_ty.kind)) {
          type_error(ck, n->token, "`-` (unary) requires a numeric operand, got %.*s",
                     str8_varg(type_ref_display(ctx_scratch(), inner_ty)));
        } else {
          result = inner_ty; // keeps the operand's type, as `bit-not` does
        }
      }
    } break;
    case TypedNodeKind_UnaryPos: {
      TypeRef inner_ty = check_expr(ck, scope, n->unary.expr);
      if (inner_ty.kind != TypeKind_Unresolved) {
        if (!type_kind_is_numeric(inner_ty.kind)) {
          type_error(ck, n->token, "`+` (unary) requires a numeric operand, got %.*s",
                     str8_varg(type_ref_display(ctx_scratch(), inner_ty)));
        } else {
          result = inner_ty; // an identity: keeps the operand's type
        }
      }
    } break;
    case TypedNodeKind_CstrExpr: {
      TypeRef inner_ty = check_expr(ck, scope, n->unary.expr);
      if (inner_ty.kind != TypeKind_Unresolved && inner_ty.kind != TypeKind_String) {
        type_error(ck, n->token, "`cstring` requires a `string`, got %.*s", str8_varg(type_ref_display(ctx_scratch(), inner_ty)));
      }
      TypeRef* boxed = push_one(ctx_perm(), TypeRef);
      boxed->kind    = TypeKind_Char;
      result.kind    = TypeKind_Pointer;
      result.pointee = boxed;
    } break;
    case TypedNodeKind_StringLenExpr: {
      TypeRef inner_ty = check_expr(ck, scope, n->unary.expr);
      if (inner_ty.kind != TypeKind_Unresolved && inner_ty.kind != TypeKind_String) {
        type_error(ck, n->token, "`string-len` requires a `string`, got %.*s", str8_varg(type_ref_display(ctx_scratch(), inner_ty)));
      }
      result.kind = TypeKind_U64;
    } break;
    case TypedNodeKind_BinaryCast: {
      // The lhs names a type, not a value, so unlike every other binary form
      // it skips scope lookup, which would report "undefined identifier
      // `i32`".
      TypedNode* lhs_node = &ck->tast->nodes[n->binary.lhs];
      if (lhs_node->kind != TypedNodeKind_Identifier) {
        type_error(ck, n->token, "`cast` target must be a type name");
      } else {
        result = type_ref_from_atom(ctx_perm(), lhs_node->ident.name);
        // Not covered by check_type_annotations: a cast target is lowered as
        // an expression, not through lower_type_node, so it has no
        // TypeAnnotation. `cast Missing` is caught below anyway (a Named type
        // is not a castable scalar), but `cast Missing*` is not -- pointer to
        // pointer is legal whatever it points at.
        String8 bad = {0};
        if (type_ref_has_undeclared_name(ck, result, &bad)) {
          type_error(ck, lhs_node->token, "unknown type `%.*s`", str8_varg(bad));
          result = type_ref_unresolved();
        }
      }
      TypeRef value_ty = check_expr(ck, scope, n->binary.rhs);
      // A cast to the same type is a no-op and always allowed, whatever the
      // category; only a real conversion needs both sides castable-scalar.
      // `void` is always a legal target too, mirroring C's `(void)expr;`
      // discard -- `(void ...)` (lower_void_do) compiles to exactly this
      // cast to force an `if`/`match` arm void.
      if (result.kind != TypeKind_Unresolved && value_ty.kind != TypeKind_Unresolved
          && result.kind != TypeKind_Void
          && !type_ref_equal(value_ty, result)
          && (!type_is_castable_scalar(ck, value_ty) || !type_is_castable_scalar(ck, result))) {
        type_error(ck, n->token,
                   "cannot `cast` %.*s to %.*s -- casts only support numeric<->numeric, "
                   "pointer<->pointer/any, or enum<->numeric conversions (not structs, "
                   "strings, arrays, arenas, or handles)",
                   str8_varg(type_ref_display(ctx_scratch(), value_ty)), str8_varg(type_ref_display(ctx_scratch(), result)));
      }
    } break;
    case TypedNodeKind_BinaryReinterpret: {
      // Same "lhs is a type name, not a value" shape as BinaryCast above.
      TypedNode* lhs_node = &ck->tast->nodes[n->binary.lhs];
      if (lhs_node->kind != TypedNodeKind_Identifier) {
        type_error(ck, n->token, "`reinterpret` target must be a type name");
      } else {
        result = type_ref_from_atom(ctx_perm(), lhs_node->ident.name);
        String8 bad = {0};
        if (type_ref_has_undeclared_name(ck, result, &bad)) { // see BinaryCast above
          type_error(ck, lhs_node->token, "unknown type `%.*s`", str8_varg(bad));
          result = type_ref_unresolved();
        }
      }
      TypeRef value_ty = check_expr(ck, scope, n->binary.rhs);
      if (result.kind != TypeKind_Unresolved && value_ty.kind != TypeKind_Unresolved) {
        u32 dst_size = type_reinterpret_byte_size(ck, result);
        u32 src_size = type_reinterpret_byte_size(ck, value_ty);
        if (dst_size == 0 || src_size == 0) {
          type_error(ck, n->token,
                     "cannot `reinterpret` %.*s as %.*s -- reinterpret only supports numeric, "
                     "char, pointer/any, or enum/flags types (not bool, void, structs, strings, "
                     "arrays, arenas, or handles)",
                     str8_varg(type_ref_display(ctx_scratch(), value_ty)), str8_varg(type_ref_display(ctx_scratch(), result)));
        } else if (dst_size != src_size) {
          type_error(ck, n->token,
                     "cannot `reinterpret` %.*s (%u bytes) as %.*s (%u bytes) -- both sides must "
                     "be the same size",
                     str8_varg(type_ref_display(ctx_scratch(), value_ty)), src_size,
                     str8_varg(type_ref_display(ctx_scratch(), result)), dst_size);
        }
      }
    } break;
    case TypedNodeKind_SizeofExpr:
    case TypedNodeKind_AlignofExpr: {
      TypeRef result_ty = n->type_query.result_type;
      if (result_ty.kind == TypeKind_Unresolved) {
        result.kind = TypeKind_U64;
      } else if (!type_kind_is_integer(result_ty.kind)) {
        type_error(ck, n->token, "`%s`'s result-type argument must be an integer type, got %.*s",
                   n->kind == TypedNodeKind_SizeofExpr ? "sizeof" : "alignof",
                   str8_varg(type_ref_display(ctx_scratch(), result_ty)));
      } else {
        result = result_ty;
      }
    } break;
    case TypedNodeKind_TypeNameExpr: {
      result.kind = TypeKind_String;
    } break;
    case TypedNodeKind_ZeroExpr: {
      if (n->type_query.type.kind == TypeKind_Void) {
        type_error(ck, n->token, "cannot `zero` `void` -- there's no value of that type");
      } else {
        result = n->type_query.type;
      }
    } break;
    case TypedNodeKind_HandleAlloc: {
      // `pool_alloc` mutates a shared free list with no locking (see
      // DEFINE_HANDLE_POOL), and a handle pool is always globally shared --
      // there is no per-lane equivalent of `lane-arena` -- so this is a flat
      // rejection. `handle-deref`/`handle-valid?` stay ungated: they are
      // reads, safe from any lane given that this gate rules out concurrent
      // alloc/free.
      if (ck->in_parallel_block) {
        type_error(ck, n->token, "`handle-alloc` is not safe to call from inside a `parallel` block (or a `lane-fn`) -- pool alloc/free isn't synchronized across lanes");
      }
      TypeRef pooled = n->type_query.type;
      if (pooled.kind != TypeKind_Named || hashtable_lookup(&ck->handle_pool_types, pooled.name) == NULL) {
        type_error(ck, n->token, "`handle-alloc` requires a type with a `(handle ...)` declaration, got %.*s",
                   str8_varg(type_ref_display(ctx_scratch(), pooled)));
      } else {
        result.kind = TypeKind_Handle;
        result.name = pooled.name;
      }
    } break;
    case TypedNodeKind_HandlePoolInit: {
      if (ck->in_parallel_block) {
        type_error(ck, n->token, "`handle-pool-init` is not safe to call from inside a `parallel` block (or a `lane-fn`) -- initialize pools before forking, not inside");
      }
      TypeRef pooled = n->handle_pool_init.type;
      if (pooled.kind != TypeKind_Named || hashtable_lookup(&ck->handle_pool_types, pooled.name) == NULL) {
        type_error(ck, n->token, "`handle-pool-init` requires a type with a `(handle ...)` declaration, got %.*s",
                   str8_varg(type_ref_display(ctx_scratch(), pooled)));
      }
      TypeRef cap_ty = check_expr(ck, scope, n->handle_pool_init.capacity);
      if (cap_ty.kind != TypeKind_Unresolved && !type_kind_is_numeric(cap_ty.kind)) {
        type_error(ck, n->token, "`handle-pool-init`'s capacity must be numeric, got %.*s", str8_varg(type_ref_display(ctx_scratch(), cap_ty)));
      }
      TypeRef arena_ty = check_expr(ck, scope, n->handle_pool_init.arena);
      if (arena_ty.kind != TypeKind_Unresolved && arena_ty.kind != TypeKind_Arena) {
        type_error(ck, n->token, "`handle-pool-init`'s last argument must be an arena, got %.*s", str8_varg(type_ref_display(ctx_scratch(), arena_ty)));
      }
      result.kind = TypeKind_Void;
    } break;
    case TypedNodeKind_MemberOffsetExpr: {
      if (n->member_offset.type.kind != TypeKind_Named) {
        type_error(ck, n->token, "`member-offset` requires a struct type, got %.*s",
                   str8_varg(type_ref_display(ctx_scratch(), n->member_offset.type)));
        break;
      }
      StructEntry* se = struct_table_lookup(ck, n->member_offset.type.name);
      if (!se) {
        type_error(ck, n->token, "`member-offset`: unknown struct type `%.*s`",
                   str8_varg(n->member_offset.type.name));
        break;
      }
      TypedNode* decl    = &ck->tast->nodes[se->decl];
      Param*     matched = find_field_recursive(ck, decl, n->member_offset.field);
      if (!matched) {
        type_error(ck, n->token, "`member-offset`: `%.*s` has no field `%.*s`",
                   str8_varg(n->member_offset.type.name), str8_varg(n->member_offset.field));
        break;
      }
      result.kind = TypeKind_U64;
    } break;
    case TypedNodeKind_FieldAccess: {
      TypeRef base_ty = check_expr(ck, scope, n->field_access.base);
      if (base_ty.kind == TypeKind_Unresolved) {
        break; // already reported deeper -- don't cascade
      }
      // `.`/`&` auto-deref exactly one level of pointer, Go/Rust-style;
      // `get`/`get-in` never do, and arrive with auto_deref false.
      if (n->field_access.auto_deref && base_ty.kind == TypeKind_Pointer) {
        base_ty = *base_ty.pointee;
      }
      if (base_ty.kind != TypeKind_Named) {
        type_error(ck, n->token, "cannot access field `%.*s` on non-struct type %.*s",
                   str8_varg(n->field_access.field), str8_varg(type_ref_display(ctx_scratch(), base_ty)));
        break;
      }
      StructEntry* se = struct_table_lookup(ck, base_ty.name);
      if (!se) {
        type_error(ck, n->token, "unknown struct type `%.*s`", str8_varg(base_ty.name));
        break;
      }
      TypedNode* decl    = &ck->tast->nodes[se->decl];
      Param*     matched = find_field_recursive(ck, decl, n->field_access.field);
      if (!matched) {
        type_error(ck, n->token, "`%.*s` has no field `%.*s`",
                   str8_varg(base_ty.name), str8_varg(n->field_access.field));
        break;
      }
      // `get-in` chains for free: this field's type becomes the `base_ty` of
      // whichever FieldAccess has this node as its base.
      result = matched->type;
    } break;
    case TypedNodeKind_Call: {
      // Prelude: a bare `{...}` map literal argument (lower_bare_map_literal)
      // carries no type name, and must be given one from the callee's
      // parameter type before the argument loop below runs, so that loop sees
      // an ordinary StructLiteral as if the source had written
      // `(ThatStruct {...})`. The callee lookup here duplicates the one in
      // the call branches further down, rather than restructuring the switch
      // to share it.
      //
      // Indirect calls through a fn-pointer local are not covered: a
      // function-pointer TypeRef carries only parallel `fn_params` TypeRefs
      // (see TypeRef.fn_params, 3b.h), no Param names, so a bare map literal
      // there falls through to the "no inferable struct type" error.
      FnEntry* infer_fn = scope_lookup_entry(scope, n->call.callee) == NULL
                        ? fn_table_lookup(ck, n->call.callee)
                        : NULL;
      if (infer_fn) {
        TypedNode* infer_decl = &ck->tast->nodes[infer_fn->decl];
        u16        infer_param_count = infer_decl->func.param_count;
        foreach_index(i, n->call.arg_count) {
          if (i >= infer_param_count) break;
          TypedIndex arg_idx = ck->tast->extra[n->call.arg_first + i];
          TypedNode* arg_n   = &ck->tast->nodes[arg_idx];
          if (arg_n->kind == TypedNodeKind_StructLiteral && arg_n->struct_lit.type_name.size == 0) {
            TypeRef expected = ck->tast->params[infer_decl->func.param_first + i].type;
            if (expected.kind == TypeKind_Named) {
              arg_n->struct_lit.type_name = expected.name;
            }
          }
          adopt_int_literal_type(ck, arg_idx, ck->tast->params[infer_decl->func.param_first + i].type);
        }
      }
      // Same adoption for an indirect call through a fn-pointer local, which
      // the bare-map-literal pass above cannot serve: a function-pointer
      // TypeRef carries parallel `fn_params` TypeRefs and no Param names, and
      // a type is all an integer literal needs.
      ScopeEntry* infer_local = scope_lookup_entry(scope, n->call.callee);
      if (infer_local && infer_local->type.kind == TypeKind_Fn) {
        foreach_index(i, n->call.arg_count) {
          if (i >= infer_local->type.fn_param_count) break;
          adopt_int_literal_type(ck, ck->tast->extra[n->call.arg_first + i], infer_local->type.fn_params[i]);
        }
      }
      // Arguments are checked even when the callee doesn't resolve, both for
      // their own diagnostics and to populate resolved_types.
      foreach_index(i, n->call.arg_count) {
        check_expr(ck, scope, ck->tast->extra[n->call.arg_first + i]);
      }
      // Looked up once here and used by the indirect fn-pointer branch far
      // below. No builtin branch consults it, so a local shadowing a builtin
      // name has no effect.
      ScopeEntry* callee_local = scope_lookup_entry(scope, n->call.callee);
      if (str8_match_lit("print", n->call.callee, 0) || str8_match_lit("println", n->call.callee, 0)) {
        // builtin: `(print "template with {} placeholders" v1 v2 ...)`, and
        // `println`, which differs only by the `\n` cg_call appends. Both
        // become one `printf`. The template must be an actual
        // StringLiteral, not merely `string`-typed, so the `{}` count can be
        // checked against the argument count and each placeholder's
        // specifier chosen from its argument's static type.
        //
        // An optional leading `stream` argument redirects the call to
        // bbb_stream_printf (see cg_call), so writing a formatted line to a
        // file reuses this machinery instead of hand-built strings and raw
        // `os/write-string`. Discriminating on arg0's type is unambiguous,
        // since the no-stream form requires a string literal there.
        // `tmpl_arg` is the index everything below counts from, so both
        // forms share one validation path.
        b32         is_println = str8_match_lit("println", n->call.callee, 0);
        const char* builtin    = is_println ? "println" : "print";
        result.kind            = TypeKind_Void;
        u32     tmpl_arg = 0;
        TypeRef arg0_ty  = {0}; // stays Unresolved for a zero-argument call
        if (n->call.arg_count > 0) {
          arg0_ty = ck->resolved_types[ck->tast->extra[n->call.arg_first + 0]];
          if (arg0_ty.kind == TypeKind_Stream) tmpl_arg = 1;
        }
        if (n->call.arg_count <= tmpl_arg) {
          type_error(ck, n->token, "`%s` requires a template string as its %s argument, e.g. `(%s %s\"count: {}\" n)`",
                     builtin, tmpl_arg == 0 ? "first" : "second", builtin, tmpl_arg == 0 ? "" : "s ");
        } else {
          TypedNode* tmpl_node = &ck->tast->nodes[ck->tast->extra[n->call.arg_first + tmpl_arg]];
          if (tmpl_node->kind != TypedNodeKind_StringLiteral) {
            // With tmpl_arg still 0 the argument could have been meant as
            // either shape, so name both -- the stream form is invisible
            // otherwise, and reads as "templates must be literals" to
            // someone who wrote `(println f "x")` with a mistyped `f`. An
            // argument that failed to resolve already carries its own
            // diagnostic and gives nothing to discriminate on, so it gets
            // no guess piled on top.
            if (tmpl_arg == 1) {
              type_error(ck, n->token, "`%s`'s second argument must be a string literal template, e.g. `(%s s \"count: {}\" n)`",
                         builtin, builtin);
            } else if (arg0_ty.kind == TypeKind_Fn && arg0_ty.fn_return
                    && arg0_ty.fn_return->kind == TypeKind_Stream && arg0_ty.fn_param_count == 0) {
              // `(println os/stdout ...)`: the stream form was intended, but
              // the stream-producing function was named instead of called.
              type_error(ck, n->token, "`%s`'s first argument names a function returning a `stream` -- call it, e.g. `(%s (os/stdout) \"count: {}\" n)`",
                         builtin, builtin);
            } else if (arg0_ty.kind != TypeKind_Unresolved) {
              type_error(ck, n->token, "`%s`'s first argument must be a string literal template, or a `stream` to print to followed by one, e.g. `(%s \"count: {}\" n)` or `(%s s \"count: {}\" n)`",
                         builtin, builtin, builtin);
            }
          } else {
            i64 placeholder_count = count_template_placeholders(ck, n->token, tmpl_node->string_lit.value);
            u32 value_count       = (u32)(n->call.arg_count - 1 - tmpl_arg);
            if (placeholder_count >= 0 && (u32)placeholder_count != value_count) {
              type_error(ck, n->token, "`%s` template has %lld `{}` placeholder(s) but got %u value(s)",
                         builtin, (long long)placeholder_count, value_count);
            } else if (placeholder_count >= 0) {
              foreach_index(i, value_count) {
                TypeRef arg_ty = ck->resolved_types[ck->tast->extra[n->call.arg_first + tmpl_arg + 1 + i]];
                if (arg_ty.kind != TypeKind_Unresolved && !type_ref_is_printable(arg_ty)) {
                  type_error(ck, n->token, "`%s` argument %u: %.*s can't be printed with `{}`",
                             builtin, (u32)(i + 1), str8_varg(type_ref_display(ctx_scratch(), arg_ty)));
                }
              }
            }
          }
        }
      } else if (str8_match_lit("str", n->call.callee, 0)) {
        // builtin: `(str arena v1 v2 ...)` -- Clojure's `str` under this
        // language's arena-first convention. Stringifies each value by its
        // static type, concatenates with no separator, and returns an owned
        // `string`. Zero values yields "", as in Clojure.
        result.kind = TypeKind_String;
        if (n->call.arg_count == 0) {
          type_error(ck, n->token, "`str` requires an arena as its first argument, e.g. `(str a \"n=\" 7)`");
        } else {
          TypeRef arena_ty = ck->resolved_types[ck->tast->extra[n->call.arg_first + 0]];
          if (arena_ty.kind != TypeKind_Unresolved && arena_ty.kind != TypeKind_Arena) {
            type_error(ck, n->token, "`str` argument 1 must be an arena, got %.*s", str8_varg(type_ref_display(ctx_scratch(), arena_ty)));
          }
          foreach_index(i, n->call.arg_count - 1) {
            TypeRef arg_ty = ck->resolved_types[ck->tast->extra[n->call.arg_first + 1 + i]];
            if (arg_ty.kind != TypeKind_Unresolved && !type_ref_is_printable(arg_ty)) {
              type_error(ck, n->token, "`str` argument %u: %.*s can't be stringified",
                         (u32)(i + 2), str8_varg(type_ref_display(ctx_scratch(), arg_ty)));
            }
          }
        }
      } else if (str8_match_lit("create", n->call.callee, 0)) {
        // builtin: `(create)` / `(create reserve-size)` -- a VM-backed arena;
        // no heap-backed option is exposed. The no-arg form's default reserve
        // size is a literal in cg_call, not synthesized here.
        if (n->call.arg_count > 1) {
          type_error(ck, n->token, "`create` takes at most one argument (a reserve size), got %u",
                     (u32)n->call.arg_count);
        } else if (n->call.arg_count == 1) {
          TypeRef size_ty = ck->resolved_types[ck->tast->extra[n->call.arg_first + 0]];
          if (size_ty.kind != TypeKind_Unresolved && !type_kind_is_numeric(size_ty.kind)) {
            type_error(ck, n->token, "`create`'s reserve size must be numeric, got %.*s",
                       str8_varg(type_ref_display(ctx_scratch(), size_ty)));
          }
        }
        result.kind = TypeKind_Arena;
      } else if (str8_match_lit("destroy", n->call.callee, 0)
              || str8_match_lit("reset", n->call.callee, 0)
              || str8_match_lit("release", n->call.callee, 0)) {
        // builtin: `(destroy a)` / `(reset a)` / `(release a)` -- one arena
        // argument, mutated or freed through its address (cg_call), no value.
        if (n->call.arg_count != 1) {
          type_error(ck, n->token, "`%.*s` takes exactly one argument (an arena), got %u",
                     str8_varg(n->call.callee), (u32)n->call.arg_count);
        } else {
          TypeRef arena_ty = ck->resolved_types[ck->tast->extra[n->call.arg_first + 0]];
          if (arena_ty.kind != TypeKind_Unresolved && arena_ty.kind != TypeKind_Arena) {
            type_error(ck, n->token, "`%.*s` expects an arena, got %.*s",
                       str8_varg(n->call.callee), str8_varg(type_ref_display(ctx_scratch(), arena_ty)));
          }
        }
        result.kind = TypeKind_Void;
      } else if (str8_match_lit("mark", n->call.callee, 0)) {
        // builtin: `(mark a)` -- returns an ArenaMark holding `a`'s current
        // cursor for a later `pop`. Unlike reset/release/destroy above, `a`
        // is not mutated (arena_mark, base.h).
        if (n->call.arg_count != 1) {
          type_error(ck, n->token, "`mark` takes exactly one argument (an arena), got %u",
                     (u32)n->call.arg_count);
        } else {
          TypeRef arena_ty = ck->resolved_types[ck->tast->extra[n->call.arg_first + 0]];
          if (arena_ty.kind != TypeKind_Unresolved && arena_ty.kind != TypeKind_Arena) {
            type_error(ck, n->token, "`mark` expects an arena, got %.*s", str8_varg(type_ref_display(ctx_scratch(), arena_ty)));
          }
        }
        result.kind = TypeKind_ArenaMark;
      } else if (str8_match_lit("pop", n->call.callee, 0)) {
        // builtin: `(pop a m)` -- rewinds `a` to where `m` was taken. `a` and
        // `m` are not checked to belong together, matching arena_pop itself.
        if (n->call.arg_count != 2) {
          type_error(ck, n->token, "`pop` takes exactly two arguments (an arena and a mark), got %u",
                     (u32)n->call.arg_count);
        } else {
          TypeRef arena_ty = ck->resolved_types[ck->tast->extra[n->call.arg_first + 0]];
          TypeRef mark_ty  = ck->resolved_types[ck->tast->extra[n->call.arg_first + 1]];
          if (arena_ty.kind != TypeKind_Unresolved && arena_ty.kind != TypeKind_Arena) {
            type_error(ck, n->token, "`pop` argument 1 must be an arena, got %.*s", str8_varg(type_ref_display(ctx_scratch(), arena_ty)));
          }
          if (mark_ty.kind != TypeKind_Unresolved && mark_ty.kind != TypeKind_ArenaMark) {
            type_error(ck, n->token, "`pop` argument 2 must be an ArenaMark, got %.*s", str8_varg(type_ref_display(ctx_scratch(), mark_ty)));
          }
        }
        result.kind = TypeKind_Void;
      } else if (str8_match_lit("string-match", n->call.callee, 0)) {
        // builtin: `(string-match a b flags)` -- the escape hatch for when
        // `=`'s exact equality isn't enough. `flags` is a raw numeric
        // StringMatchFlags bitmask (base.h); no named constants are exposed
        // to source yet, so callers pass literal ints.
        if (n->call.arg_count != 3) {
          type_error(ck, n->token, "`string-match` expects 3 arguments (String, String, flags), got %u",
                     (u32)n->call.arg_count);
        } else {
          TypeRef a_ty = ck->resolved_types[ck->tast->extra[n->call.arg_first + 0]];
          TypeRef b_ty = ck->resolved_types[ck->tast->extra[n->call.arg_first + 1]];
          TypeRef f_ty = ck->resolved_types[ck->tast->extra[n->call.arg_first + 2]];
          if (a_ty.kind != TypeKind_Unresolved && a_ty.kind != TypeKind_String) {
            type_error(ck, n->token, "`string-match` argument 1 must be a String, got %.*s",
                       str8_varg(type_ref_display(ctx_scratch(), a_ty)));
          }
          if (b_ty.kind != TypeKind_Unresolved && b_ty.kind != TypeKind_String) {
            type_error(ck, n->token, "`string-match` argument 2 must be a String, got %.*s",
                       str8_varg(type_ref_display(ctx_scratch(), b_ty)));
          }
          if (f_ty.kind != TypeKind_Unresolved && !type_kind_is_numeric(f_ty.kind)) {
            type_error(ck, n->token, "`string-match` argument 3 (flags) must be numeric, got %.*s",
                       str8_varg(type_ref_display(ctx_scratch(), f_ty)));
          }
        }
        result.kind = TypeKind_Bool;
      } else if (str8_match_lit("free", n->call.callee, 0)) {
        // builtin: `(free p)` -- the other half of `alloc`. Any pointer, and
        // unlike `alloc` no type argument.
        if (n->call.arg_count != 1) {
          type_error(ck, n->token, "`free` takes exactly one argument (a pointer), got %u",
                     (u32)n->call.arg_count);
        } else {
          TypeRef p_ty = ck->resolved_types[ck->tast->extra[n->call.arg_first + 0]];
          if (p_ty.kind != TypeKind_Unresolved && p_ty.kind != TypeKind_Pointer) {
            type_error(ck, n->token, "`free` expects a pointer, got %.*s", str8_varg(type_ref_display(ctx_scratch(), p_ty)));
          }
        }
        result.kind = TypeKind_Void;
      } else if (str8_match_lit("handle-deref", n->call.callee, 0)) {
        // builtin: `(handle-deref h)` -- h's static type `T^` names the pool;
        // returns `T*`, or NULL if h is stale, out of range or zero (the
        // checked-nilable convention of base.h's Prefix_pool_get).
        if (n->call.arg_count != 1) {
          type_error(ck, n->token, "`handle-deref` takes exactly one argument (a handle), got %u",
                     (u32)n->call.arg_count);
        } else {
          TypeRef h_ty = ck->resolved_types[ck->tast->extra[n->call.arg_first + 0]];
          if (h_ty.kind != TypeKind_Unresolved && h_ty.kind != TypeKind_Handle) {
            type_error(ck, n->token, "`handle-deref` expects a handle, got %.*s", str8_varg(type_ref_display(ctx_scratch(), h_ty)));
          } else if (h_ty.kind == TypeKind_Handle) {
            TypeRef* boxed = push_one(ctx_perm(), TypeRef);
            boxed->kind    = TypeKind_Named;
            boxed->name    = h_ty.name;
            result.kind    = TypeKind_Pointer;
            result.pointee = boxed;
          }
        }
      } else if (str8_match_lit("handle-free", n->call.callee, 0)) {
        // builtin: `(handle-free h)` -- bumps the slot's generation and
        // returns it to the free list, so any other handle to that slot goes
        // stale (base.h's Prefix_pool_free).
        if (ck->in_parallel_block) {
          type_error(ck, n->token, "`handle-free` is not safe to call from inside a `parallel` block (or a `lane-fn`) -- pool alloc/free isn't synchronized across lanes");
        }
        if (n->call.arg_count != 1) {
          type_error(ck, n->token, "`handle-free` takes exactly one argument (a handle), got %u",
                     (u32)n->call.arg_count);
        } else {
          TypeRef h_ty = ck->resolved_types[ck->tast->extra[n->call.arg_first + 0]];
          if (h_ty.kind != TypeKind_Unresolved && h_ty.kind != TypeKind_Handle) {
            type_error(ck, n->token, "`handle-free` expects a handle, got %.*s", str8_varg(type_ref_display(ctx_scratch(), h_ty)));
          }
        }
        result.kind = TypeKind_Void;
      } else if (str8_match_lit("handle-valid?", n->call.callee, 0)) {
        // builtin: `(handle-valid? h)` -- a real liveness check (in range,
        // generation matches), the same one `handle-deref` performs. Not
        // bbb_handle.h's `Prefix_handle_valid`, which only tests for the
        // zero handle and can't tell stale from live. See cg_call.
        if (n->call.arg_count != 1) {
          type_error(ck, n->token, "`handle-valid?` takes exactly one argument (a handle), got %u",
                     (u32)n->call.arg_count);
        } else {
          TypeRef h_ty = ck->resolved_types[ck->tast->extra[n->call.arg_first + 0]];
          if (h_ty.kind != TypeKind_Unresolved && h_ty.kind != TypeKind_Handle) {
            type_error(ck, n->token, "`handle-valid?` expects a handle, got %.*s", str8_varg(type_ref_display(ctx_scratch(), h_ty)));
          }
        }
        result.kind = TypeKind_Bool;
      } else if (str8_match_lit("align-pow2", n->call.callee, 0)
              || str8_match_lit("align-down-pow2", n->call.callee, 0)
              || str8_match_lit("align-pad-pow2", n->call.callee, 0)) {
        // builtin: base.h's AlignPow2/AlignDownPow2/AlignPadPow2. Arithmetic,
        // so operands must match and the result takes their type, as for
        // +/-/*//.
        if (n->call.arg_count != 2) {
          type_error(ck, n->token, "`%.*s` takes exactly two arguments (a value and a power-of-two), got %u",
                     str8_varg(n->call.callee), (u32)n->call.arg_count);
        } else {
          TypeRef x_ty = ck->resolved_types[ck->tast->extra[n->call.arg_first + 0]];
          TypeRef b_ty = ck->resolved_types[ck->tast->extra[n->call.arg_first + 1]];
          if (x_ty.kind != TypeKind_Unresolved && b_ty.kind != TypeKind_Unresolved) {
            if (!type_ref_equal(x_ty, b_ty)) {
              type_error(ck, n->token, "`%.*s` operands have mismatched types: %.*s vs %.*s",
                         str8_varg(n->call.callee), str8_varg(type_ref_display(ctx_scratch(), x_ty)), str8_varg(type_ref_display(ctx_scratch(), b_ty)));
            } else if (!type_kind_is_numeric(x_ty.kind)) {
              type_error(ck, n->token, "`%.*s` requires numeric operands, got %.*s",
                         str8_varg(n->call.callee), str8_varg(type_ref_display(ctx_scratch(), x_ty)));
            } else {
              result = x_ty;
            }
          }
        }
      } else if (str8_match_lit("swap", n->call.callee, 0)) {
        // builtin: `(swap a b)` -- base.h's `Swap(T, a, b)`, a temp-var swap
        // by assignment. It mutates its operands, so both must be lvalues of
        // the shapes `set` accepts (swap_target_is_valid). Any assignable
        // type works, not only numerics, except arrays: C arrays aren't
        // assignable with `=`, which the temp-var dance needs.
        //
        // `Swap` is a `do {} while (0)` statement macro with no value, so the
        // result is void rather than `a`'s type -- see the `({ ...; (void)0;
        // })` wrapping in cg_call.
        if (n->call.arg_count != 2) {
          type_error(ck, n->token, "`swap` takes exactly two arguments, got %u", (u32)n->call.arg_count);
        } else {
          TypedIndex a_idx  = ck->tast->extra[n->call.arg_first + 0];
          TypedIndex b_idx  = ck->tast->extra[n->call.arg_first + 1];
          TypedNode* a_node = &ck->tast->nodes[a_idx];
          TypedNode* b_node = &ck->tast->nodes[b_idx];
          b32        a_ok   = swap_target_is_valid(scope, a_node);
          b32        b_ok   = swap_target_is_valid(scope, b_node);
          if (!a_ok) {
            type_error(ck, n->token, "`swap` argument 1 must be a mutable variable, field, index, or deref -- not an arbitrary expression");
          }
          if (!b_ok) {
            type_error(ck, n->token, "`swap` argument 2 must be a mutable variable, field, index, or deref -- not an arbitrary expression");
          }
          if (a_ok && b_ok) {
            TypeRef a_ty = ck->resolved_types[a_idx];
            TypeRef b_ty = ck->resolved_types[b_idx];
            if (a_ty.kind != TypeKind_Unresolved && b_ty.kind != TypeKind_Unresolved) {
              if (!type_ref_equal(a_ty, b_ty)) {
                type_error(ck, n->token, "`swap` operands have mismatched types: %.*s vs %.*s",
                           str8_varg(type_ref_display(ctx_scratch(), a_ty)), str8_varg(type_ref_display(ctx_scratch(), b_ty)));
              } else if (a_ty.kind == TypeKind_Array) {
                type_error(ck, n->token, "`swap` can't swap array values (not assignable in C) -- swap elements instead");
              }
            }
          }
        }
        result.kind = TypeKind_Void;
      } else if (str8_match_lit("min", n->call.callee, 0)
              || str8_match_lit("max", n->call.callee, 0)) {
        // builtin: base.h's Min/Max macros. Same operand rules as align-pow2
        // above, and since the macro is a `<`/`>` ternary rather than a
        // fixed-width function, one form covers every numeric type.
        if (n->call.arg_count != 2) {
          type_error(ck, n->token, "`%.*s` takes exactly two arguments, got %u",
                     str8_varg(n->call.callee), (u32)n->call.arg_count);
        } else {
          TypeRef a_ty = ck->resolved_types[ck->tast->extra[n->call.arg_first + 0]];
          TypeRef b_ty = ck->resolved_types[ck->tast->extra[n->call.arg_first + 1]];
          if (a_ty.kind != TypeKind_Unresolved && b_ty.kind != TypeKind_Unresolved) {
            if (!type_ref_equal(a_ty, b_ty)) {
              type_error(ck, n->token, "`%.*s` operands have mismatched types: %.*s vs %.*s",
                         str8_varg(n->call.callee), str8_varg(type_ref_display(ctx_scratch(), a_ty)), str8_varg(type_ref_display(ctx_scratch(), b_ty)));
            } else if (!type_kind_is_numeric(a_ty.kind)) {
              type_error(ck, n->token, "`%.*s` requires numeric operands, got %.*s",
                         str8_varg(n->call.callee), str8_varg(type_ref_display(ctx_scratch(), a_ty)));
            } else {
              result = a_ty;
            }
          }
        }
      } else if (str8_match_lit("clamp", n->call.callee, 0)) {
        // builtin: `(clamp x lo hi)` -- base.h's Clamp, with the value first
        // per the usual convention rather than base.h's `Clamp(lo, x, hi)`;
        // cg_call reorders. All three operands share one numeric type.
        if (n->call.arg_count != 3) {
          type_error(ck, n->token, "`clamp` takes exactly three arguments (value, lo, hi), got %u",
                     (u32)n->call.arg_count);
        } else {
          TypeRef x_ty  = ck->resolved_types[ck->tast->extra[n->call.arg_first + 0]];
          TypeRef lo_ty = ck->resolved_types[ck->tast->extra[n->call.arg_first + 1]];
          TypeRef hi_ty = ck->resolved_types[ck->tast->extra[n->call.arg_first + 2]];
          if (x_ty.kind != TypeKind_Unresolved && lo_ty.kind != TypeKind_Unresolved && hi_ty.kind != TypeKind_Unresolved) {
            if (!type_ref_equal(x_ty, lo_ty) || !type_ref_equal(x_ty, hi_ty)) {
              type_error(ck, n->token, "`clamp` operands have mismatched types: %.*s, %.*s, %.*s",
                         str8_varg(type_ref_display(ctx_scratch(), x_ty)), str8_varg(type_ref_display(ctx_scratch(), lo_ty)), str8_varg(type_ref_display(ctx_scratch(), hi_ty)));
            } else if (!type_kind_is_numeric(x_ty.kind)) {
              type_error(ck, n->token, "`clamp` requires numeric operands, got %.*s",
                         str8_varg(type_ref_display(ctx_scratch(), x_ty)));
            } else {
              result = x_ty;
            }
          }
        }
      } else if (str8_match_lit("clamp-top", n->call.callee, 0)
              || str8_match_lit("clamp-bot", n->call.callee, 0)) {
        // builtin: base.h's single-bound ClampTop/ClampBot. Same operand
        // rules as min/max above.
        if (n->call.arg_count != 2) {
          type_error(ck, n->token, "`%.*s` takes exactly two arguments, got %u",
                     str8_varg(n->call.callee), (u32)n->call.arg_count);
        } else {
          TypeRef x_ty = ck->resolved_types[ck->tast->extra[n->call.arg_first + 0]];
          TypeRef b_ty = ck->resolved_types[ck->tast->extra[n->call.arg_first + 1]];
          if (x_ty.kind != TypeKind_Unresolved && b_ty.kind != TypeKind_Unresolved) {
            if (!type_ref_equal(x_ty, b_ty)) {
              type_error(ck, n->token, "`%.*s` operands have mismatched types: %.*s vs %.*s",
                         str8_varg(n->call.callee), str8_varg(type_ref_display(ctx_scratch(), x_ty)), str8_varg(type_ref_display(ctx_scratch(), b_ty)));
            } else if (!type_kind_is_numeric(x_ty.kind)) {
              type_error(ck, n->token, "`%.*s` requires numeric operands, got %.*s",
                         str8_varg(n->call.callee), str8_varg(type_ref_display(ctx_scratch(), x_ty)));
            } else {
              result = x_ty;
            }
          }
        }
      } else if (str8_match_lit("abs", n->call.callee, 0)) {
        // builtin: `(abs x)` -- base.h's Abs ternary. `x < 0 ? -x : x` rather
        // than a fixed-width libc function, so it covers every numeric type.
        if (n->call.arg_count != 1) {
          type_error(ck, n->token, "`abs` takes exactly one argument, got %u", (u32)n->call.arg_count);
        } else {
          TypeRef x_ty = ck->resolved_types[ck->tast->extra[n->call.arg_first + 0]];
          if (x_ty.kind != TypeKind_Unresolved) {
            if (!type_kind_is_numeric(x_ty.kind)) {
              type_error(ck, n->token, "`abs` requires a numeric operand, got %.*s", str8_varg(type_ref_display(ctx_scratch(), x_ty)));
            } else {
              result = x_ty;
            }
          }
        }
      } else if (str8_match_lit("sin",   n->call.callee, 0) || str8_match_lit("cos",   n->call.callee, 0)
              || str8_match_lit("tan",   n->call.callee, 0) || str8_match_lit("asin",  n->call.callee, 0)
              || str8_match_lit("acos",  n->call.callee, 0) || str8_match_lit("atan",  n->call.callee, 0)
              || str8_match_lit("sinh",  n->call.callee, 0) || str8_match_lit("cosh",  n->call.callee, 0)
              || str8_match_lit("tanh",  n->call.callee, 0) || str8_match_lit("sqrt",  n->call.callee, 0)
              || str8_match_lit("cbrt",  n->call.callee, 0) || str8_match_lit("ceil",  n->call.callee, 0)
              || str8_match_lit("floor", n->call.callee, 0) || str8_match_lit("round", n->call.callee, 0)) {
        // builtin: one-argument libm functions. Trig takes and returns
        // radians, following C rather than base.h's turn-based
        // sin_f32/cos_f32/tan_f32 wrappers. cg_call emits `sinf`/`sqrtf`/...
        // for f32 and `sin`/`sqrt`/... for f64. No integer overload: libm
        // has none, and an implicit promotion would hide a likely mistake.
        if (n->call.arg_count != 1) {
          type_error(ck, n->token, "`%.*s` takes exactly one argument, got %u",
                     str8_varg(n->call.callee), (u32)n->call.arg_count);
        } else {
          TypeRef x_ty = ck->resolved_types[ck->tast->extra[n->call.arg_first + 0]];
          if (x_ty.kind != TypeKind_Unresolved) {
            if (x_ty.kind != TypeKind_F32 && x_ty.kind != TypeKind_F64) {
              type_error(ck, n->token, "`%.*s` requires an f32 or f64 argument, got %.*s",
                         str8_varg(n->call.callee), str8_varg(type_ref_display(ctx_scratch(), x_ty)));
            } else {
              result = x_ty;
            }
          }
        }
      } else if (str8_match_lit("atan2", n->call.callee, 0)
              || str8_match_lit("pow",   n->call.callee, 0)
              || str8_match_lit("mod",   n->call.callee, 0)) {
        // builtin: two-argument libm functions. `mod` is `fmod`/`fmodf`, the
        // float complement to the `%` operator's integer remainder (see
        // TypedNodeKind_BinaryMod). Both operands share one float type, which
        // the result takes.
        if (n->call.arg_count != 2) {
          type_error(ck, n->token, "`%.*s` takes exactly two arguments, got %u",
                     str8_varg(n->call.callee), (u32)n->call.arg_count);
        } else {
          TypeRef a_ty = ck->resolved_types[ck->tast->extra[n->call.arg_first + 0]];
          TypeRef b_ty = ck->resolved_types[ck->tast->extra[n->call.arg_first + 1]];
          if (a_ty.kind != TypeKind_Unresolved && b_ty.kind != TypeKind_Unresolved) {
            if (!type_ref_equal(a_ty, b_ty)) {
              type_error(ck, n->token, "`%.*s` operands have mismatched types: %.*s vs %.*s",
                         str8_varg(n->call.callee), str8_varg(type_ref_display(ctx_scratch(), a_ty)), str8_varg(type_ref_display(ctx_scratch(), b_ty)));
            } else if (a_ty.kind != TypeKind_F32 && a_ty.kind != TypeKind_F64) {
              type_error(ck, n->token, "`%.*s` requires f32 or f64 operands, got %.*s",
                         str8_varg(n->call.callee), str8_varg(type_ref_display(ctx_scratch(), a_ty)));
            } else {
              result = a_ty;
            }
          }
        }
      } else if (str8_match_lit("nth-checked", n->call.callee, 0)) {
        // builtin: `(nth-checked base index)` -- `nth` with a bounds check,
        // returning a checked-nilable `T*` that is NULL when out of range.
        // Plain `nth` emits a bare C `[]` and trusts the index (lower_nth).
        //
        // Only Array (a static `.count`) and Vector (a runtime `dyn-count`)
        // carry a length to check, so a plain pointer is rejected rather than
        // silently never failing. Unlike `string-to-i32` or `sqrt-checked`,
        // no struct is synthesized at lowering time: `T*` is an ordinary
        // Pointer TypeRef built here.
        if (n->call.arg_count != 2) {
          type_error(ck, n->token, "`nth-checked` takes exactly a base and an index, got %u",
                     (u32)n->call.arg_count);
        } else {
          TypeRef base_ty  = ck->resolved_types[ck->tast->extra[n->call.arg_first + 0]];
          TypeRef index_ty = ck->resolved_types[ck->tast->extra[n->call.arg_first + 1]];
          if (index_ty.kind != TypeKind_Unresolved && !type_kind_is_numeric(index_ty.kind)) {
            type_error(ck, n->token, "`nth-checked` index must be numeric, got %.*s", str8_varg(type_ref_display(ctx_scratch(), index_ty)));
          }
          if (base_ty.kind == TypeKind_Array || base_ty.kind == TypeKind_Vector) {
            TypeRef* boxed = push_one(ctx_perm(), TypeRef);
            *boxed          = *base_ty.pointee;
            result.kind     = TypeKind_Pointer;
            result.pointee  = boxed;
          } else if (base_ty.kind != TypeKind_Unresolved) {
            type_error(ck, n->token,
                       "`nth-checked` requires an array or Vector (needs a known length to check "
                       "against -- a plain pointer has none), got %.*s", str8_varg(type_ref_display(ctx_scratch(), base_ty)));
          }
        }
      } else if (str8_match_lit("len", n->call.callee, 0)) {
        // builtin: `(len x)` -- container length, u64 in every case (as with
        // `string-len`/`sizeof`). Three shapes:
        //  - `string`: `(x).size`, same as `string-len`.
        //  - `[T N]`: N from the static type (TypeRef.count), not a runtime
        //    `sizeof(x)/sizeof(x[0])`. That stays correct where C would have
        //    decayed the array to a pointer, e.g. a function parameter, since
        //    the 3b type still carries the count.
        //  - `Vector T`: a runtime read, as `dyn-count` does.
        // A bare pointer carries no length -- there is no slice type yet --
        // so it is not accepted. `dyn-count` below is the untyped equivalent
        // of the Vector case.
        if (n->call.arg_count != 1) {
          type_error(ck, n->token, "`len` takes exactly one argument, got %u", (u32)n->call.arg_count);
        } else {
          TypeRef x_ty = ck->resolved_types[ck->tast->extra[n->call.arg_first + 0]];
          if (x_ty.kind != TypeKind_Unresolved && x_ty.kind != TypeKind_String
              && x_ty.kind != TypeKind_Array && x_ty.kind != TypeKind_Vector) {
            type_error(ck, n->token, "`len` requires a `string`, fixed-size array, or Vector, got %.*s",
                       str8_varg(type_ref_display(ctx_scratch(), x_ty)));
          }
        }
        result.kind = TypeKind_U64;
      } else if (str8_match_lit("lane-index", n->call.callee, 0)) {
        // builtin: `(lane-index)` -- this lane's 0-based index in the pool,
        // meaningful only inside a `parallel` body.
        if (!ck->in_parallel_block) {
          type_error(ck, n->token, "`lane-index` used outside of a `parallel` block");
        }
        if (n->call.arg_count != 0) {
          type_error(ck, n->token, "`lane-index` takes no arguments, got %u", (u32)n->call.arg_count);
        }
        result.kind = TypeKind_U32;
      } else if (str8_match_lit("lane-count", n->call.callee, 0)) {
        // builtin: `(lane-count)` -- total lanes in the pool. A fixed,
        // process-wide number that doesn't depend on running on a lane
        // thread, so unlike lane-index/lane-sync/lane-arena it is not gated
        // on in_parallel_block; sizing an output array before a `parallel`
        // block is the intended use.
        if (n->call.arg_count != 0) {
          type_error(ck, n->token, "`lane-count` takes no arguments, got %u", (u32)n->call.arg_count);
        }
        result.kind = TypeKind_U32;
      } else if (str8_match_lit("lane-sync", n->call.callee, 0)) {
        // builtin: `(lane-sync)` -- barrier: every lane waits here until
        // all lanes arrive. Only meaningful inside a `parallel` block's body.
        if (!ck->in_parallel_block) {
          type_error(ck, n->token, "`lane-sync` used outside of a `parallel` block");
        }
        if (n->call.arg_count != 0) {
          type_error(ck, n->token, "`lane-sync` takes no arguments, got %u", (u32)n->call.arg_count);
        }
        result.kind = TypeKind_Void;
      } else if (str8_match_lit("lane-arena", n->call.callee, 0)) {
        // builtin: `(lane-arena)` -- this lane's private scratch arena, one
        // per OS thread (ctx_scratch/tls_ctx in cg_write_runtime_source), so
        // pushing into it can't race another lane. Only meaningful inside a
        // `parallel` body.
        if (!ck->in_parallel_block) {
          type_error(ck, n->token, "`lane-arena` used outside of a `parallel` block");
        }
        if (n->call.arg_count != 0) {
          type_error(ck, n->token, "`lane-arena` takes no arguments, got %u", (u32)n->call.arg_count);
        }
        result.kind = TypeKind_Arena;
      } else if (str8_match_lit("dyn-count", n->call.callee, 0)) {
        // builtin: `(dyn-count p)` -- base.h's `dyn_count`, reading the hidden
        // `DynHdr` that sits just before `p`. Nothing distinguishes a
        // dyn-push-grown pointer from any other at the type level, so this
        // isn't checked to be one: calling it on a `push`/`alloc` pointer is
        // undefined behavior, the same trust `commit` extends. Kept separate
        // from `len` so `len` stays unconditionally safe.
        if (n->call.arg_count != 1) {
          type_error(ck, n->token, "`dyn-count` takes exactly one argument, got %u", (u32)n->call.arg_count);
        } else {
          TypeRef p_ty = ck->resolved_types[ck->tast->extra[n->call.arg_first + 0]];
          if (p_ty.kind != TypeKind_Unresolved && p_ty.kind != TypeKind_Pointer) {
            type_error(ck, n->token, "`dyn-count` requires a pointer, got %.*s", str8_varg(type_ref_display(ctx_scratch(), p_ty)));
          }
        }
        result.kind = TypeKind_U64;
      } else if (str8_match_lit("vector-clear", n->call.callee, 0)) {
        // builtin: `(vector-clear v)` -- truncates a Vector's count to 0 in
        // place. A header write (base.h's dyn_hdr), never a realloc, so `v`
        // needs no address.
        if (n->call.arg_count != 1) {
          type_error(ck, n->token, "`vector-clear` takes exactly one argument (a Vector), got %u",
                     (u32)n->call.arg_count);
        } else {
          TypeRef vec_ty = ck->resolved_types[ck->tast->extra[n->call.arg_first + 0]];
          if (vec_ty.kind != TypeKind_Unresolved && vec_ty.kind != TypeKind_Vector) {
            type_error(ck, n->token, "`vector-clear` requires a Vector, got %.*s", str8_varg(type_ref_display(ctx_scratch(), vec_ty)));
          }
        }
        result.kind = TypeKind_Void;
      } else if (str8_match_lit("vector-swap-remove", n->call.callee, 0) || str8_match_lit("vector-remove-at", n->call.callee, 0)) {
        // builtin: `(vector-swap-remove v index)` removes in O(1) by
        // overwriting `index` with the last element, reordering the Vector;
        // `(vector-remove-at v index)` shifts the following elements down
        // instead, preserving order in O(n). Both are checked -- they return
        // false and mutate nothing when `index` is out of range -- rather
        // than trusting the index the way `nth` does, since a bad mutating
        // index is worse than a bad read.
        const char* callee_name = str8_match_lit("vector-swap-remove", n->call.callee, 0) ? "vector-swap-remove" : "vector-remove-at";
        if (n->call.arg_count != 2) {
          type_error(ck, n->token, "`%s` takes exactly a Vector and an index, got %u",
                     callee_name, (u32)n->call.arg_count);
        } else {
          TypeRef vec_ty   = ck->resolved_types[ck->tast->extra[n->call.arg_first + 0]];
          TypeRef index_ty = ck->resolved_types[ck->tast->extra[n->call.arg_first + 1]];
          if (vec_ty.kind != TypeKind_Unresolved && vec_ty.kind != TypeKind_Vector) {
            type_error(ck, n->token, "`%s` requires a Vector, got %.*s", callee_name, str8_varg(type_ref_display(ctx_scratch(), vec_ty)));
          }
          if (index_ty.kind != TypeKind_Unresolved && !type_kind_is_numeric(index_ty.kind)) {
            type_error(ck, n->token, "`%s`'s index must be numeric, got %.*s", callee_name, str8_varg(type_ref_display(ctx_scratch(), index_ty)));
          }
        }
        result.kind = TypeKind_Bool;
      } else if (str8_match_lit("vector-contains?", n->call.callee, 0)) {
        // builtin: `(vector-contains? v x)` -- linear search. `x` must match
        // `v`'s element type and be comparable, the same gate `=`/`!=` use;
        // cg_emit_field_eq_expr emits the per-element comparison.
        if (n->call.arg_count != 2) {
          type_error(ck, n->token, "`vector-contains?` takes exactly a Vector and a value, got %u", (u32)n->call.arg_count);
        } else {
          TypeRef vec_ty    = ck->resolved_types[ck->tast->extra[n->call.arg_first + 0]];
          TypeRef needle_ty = ck->resolved_types[ck->tast->extra[n->call.arg_first + 1]];
          if (vec_ty.kind != TypeKind_Unresolved && vec_ty.kind != TypeKind_Vector) {
            type_error(ck, n->token, "`vector-contains?` requires a Vector, got %.*s", str8_varg(type_ref_display(ctx_scratch(), vec_ty)));
          } else if (vec_ty.kind == TypeKind_Vector && needle_ty.kind != TypeKind_Unresolved) {
            if (!type_ref_equal(*vec_ty.pointee, needle_ty)) {
              type_error(ck, n->token, "`vector-contains?`'s second argument must match the Vector's element type %.*s, got %.*s",
                         str8_varg(type_ref_display(ctx_scratch(), *vec_ty.pointee)), str8_varg(type_ref_display(ctx_scratch(), needle_ty)));
            } else if (!type_ref_is_comparable(ck, needle_ty)) {
              type_error(ck, n->token, "`vector-contains?` requires a comparable element type, got %.*s",
                         str8_varg(type_ref_display(ctx_scratch(), needle_ty)));
            }
          }
        }
        result.kind = TypeKind_Bool;
      } else if (str8_match_lit("map-set", n->call.callee, 0)) {
        // builtin: `(map-set arena m key value)` -- inserts or overwrites in
        // the monomorphized hash table `m` (TypeKind_Map, 3b.h). The slot
        // array is allocated from `arena` on first insert, so a
        // zero-initialized Map is a valid empty one.
        if (n->call.arg_count != 4) {
          type_error(ck, n->token, "`map-set` takes exactly an arena, a Map, a key, and a value, got %u",
                     (u32)n->call.arg_count);
        } else {
          TypeRef arena_ty = ck->resolved_types[ck->tast->extra[n->call.arg_first + 0]];
          TypeRef m_ty     = ck->resolved_types[ck->tast->extra[n->call.arg_first + 1]];
          TypeRef key_ty   = ck->resolved_types[ck->tast->extra[n->call.arg_first + 2]];
          TypeRef value_ty = ck->resolved_types[ck->tast->extra[n->call.arg_first + 3]];
          if (arena_ty.kind != TypeKind_Unresolved && arena_ty.kind != TypeKind_Arena) {
            type_error(ck, n->token, "`map-set`'s first argument must be an arena, got %.*s", str8_varg(type_ref_display(ctx_scratch(), arena_ty)));
          }
          if (m_ty.kind != TypeKind_Unresolved && m_ty.kind != TypeKind_Map) {
            type_error(ck, n->token, "`map-set` requires a Map, got %.*s", str8_varg(type_ref_display(ctx_scratch(), m_ty)));
          } else if (m_ty.kind == TypeKind_Map) {
            if (key_ty.kind != TypeKind_Unresolved && !type_ref_equal(key_ty, *m_ty.map_key)) {
              type_error(ck, n->token, "`map-set` key: expected %.*s, got %.*s",
                         str8_varg(type_ref_display(ctx_scratch(), *m_ty.map_key)), str8_varg(type_ref_display(ctx_scratch(), key_ty)));
            }
            if (value_ty.kind != TypeKind_Unresolved && !type_ref_equal(value_ty, *m_ty.pointee)) {
              type_error(ck, n->token, "`map-set` value: expected %.*s, got %.*s",
                         str8_varg(type_ref_display(ctx_scratch(), *m_ty.pointee)), str8_varg(type_ref_display(ctx_scratch(), value_ty)));
            }
          }
        }
        result.kind = TypeKind_Void;
      } else if (str8_match_lit("map-get", n->call.callee, 0)) {
        // builtin: `(map-get m key)` -- a nilable pointer to the value, NULL
        // when absent, as `handle-deref` does.
        if (n->call.arg_count != 2) {
          type_error(ck, n->token, "`map-get` takes exactly a Map and a key, got %u", (u32)n->call.arg_count);
        } else {
          TypeRef m_ty   = ck->resolved_types[ck->tast->extra[n->call.arg_first + 0]];
          TypeRef key_ty = ck->resolved_types[ck->tast->extra[n->call.arg_first + 1]];
          if (m_ty.kind != TypeKind_Unresolved && m_ty.kind != TypeKind_Map) {
            type_error(ck, n->token, "`map-get` requires a Map, got %.*s", str8_varg(type_ref_display(ctx_scratch(), m_ty)));
          } else if (m_ty.kind == TypeKind_Map) {
            if (key_ty.kind != TypeKind_Unresolved && !type_ref_equal(key_ty, *m_ty.map_key)) {
              type_error(ck, n->token, "`map-get` key: expected %.*s, got %.*s",
                         str8_varg(type_ref_display(ctx_scratch(), *m_ty.map_key)), str8_varg(type_ref_display(ctx_scratch(), key_ty)));
            }
            TypeRef* boxed = push_one(ctx_perm(), TypeRef);
            *boxed         = *m_ty.pointee;
            result.kind    = TypeKind_Pointer;
            result.pointee = boxed;
          }
        }
      } else if (str8_match_lit("map-remove", n->call.callee, 0) || str8_match_lit("map-contains?", n->call.callee, 0)) {
        // builtin: `(map-remove m key)` / `(map-contains? m key)` -- bool.
        const char* callee_name = str8_match_lit("map-remove", n->call.callee, 0) ? "map-remove" : "map-contains?";
        if (n->call.arg_count != 2) {
          type_error(ck, n->token, "`%s` takes exactly a Map and a key, got %u", callee_name, (u32)n->call.arg_count);
        } else {
          TypeRef m_ty   = ck->resolved_types[ck->tast->extra[n->call.arg_first + 0]];
          TypeRef key_ty = ck->resolved_types[ck->tast->extra[n->call.arg_first + 1]];
          if (m_ty.kind != TypeKind_Unresolved && m_ty.kind != TypeKind_Map) {
            type_error(ck, n->token, "`%s` requires a Map, got %.*s", callee_name, str8_varg(type_ref_display(ctx_scratch(), m_ty)));
          } else if (m_ty.kind == TypeKind_Map && key_ty.kind != TypeKind_Unresolved && !type_ref_equal(key_ty, *m_ty.map_key)) {
            type_error(ck, n->token, "`%s` key: expected %.*s, got %.*s", callee_name,
                       str8_varg(type_ref_display(ctx_scratch(), *m_ty.map_key)), str8_varg(type_ref_display(ctx_scratch(), key_ty)));
          }
        }
        result.kind = TypeKind_Bool;
      } else if (str8_match_lit("set-add", n->call.callee, 0)) {
        // builtin: `(set-add arena s value)` -- true if newly added, false if
        // already present. Allocates from `arena` on first add, as `map-set`
        // does.
        if (n->call.arg_count != 3) {
          type_error(ck, n->token, "`set-add` takes exactly an arena, a Set, and a value, got %u", (u32)n->call.arg_count);
        } else {
          TypeRef arena_ty = ck->resolved_types[ck->tast->extra[n->call.arg_first + 0]];
          TypeRef s_ty     = ck->resolved_types[ck->tast->extra[n->call.arg_first + 1]];
          TypeRef value_ty = ck->resolved_types[ck->tast->extra[n->call.arg_first + 2]];
          if (arena_ty.kind != TypeKind_Unresolved && arena_ty.kind != TypeKind_Arena) {
            type_error(ck, n->token, "`set-add`'s first argument must be an arena, got %.*s", str8_varg(type_ref_display(ctx_scratch(), arena_ty)));
          }
          if (s_ty.kind != TypeKind_Unresolved && s_ty.kind != TypeKind_Set) {
            type_error(ck, n->token, "`set-add` requires a Set, got %.*s", str8_varg(type_ref_display(ctx_scratch(), s_ty)));
          } else if (s_ty.kind == TypeKind_Set && value_ty.kind != TypeKind_Unresolved && !type_ref_equal(value_ty, *s_ty.pointee)) {
            type_error(ck, n->token, "`set-add` value: expected %.*s, got %.*s",
                       str8_varg(type_ref_display(ctx_scratch(), *s_ty.pointee)), str8_varg(type_ref_display(ctx_scratch(), value_ty)));
          }
        }
        result.kind = TypeKind_Bool;
      } else if (str8_match_lit("set-contains?", n->call.callee, 0) || str8_match_lit("set-remove", n->call.callee, 0)) {
        // builtin: `(set-contains? s value)` / `(set-remove s value)` -- bool.
        const char* callee_name = str8_match_lit("set-remove", n->call.callee, 0) ? "set-remove" : "set-contains?";
        if (n->call.arg_count != 2) {
          type_error(ck, n->token, "`%s` takes exactly a Set and a value, got %u", callee_name, (u32)n->call.arg_count);
        } else {
          TypeRef s_ty     = ck->resolved_types[ck->tast->extra[n->call.arg_first + 0]];
          TypeRef value_ty = ck->resolved_types[ck->tast->extra[n->call.arg_first + 1]];
          if (s_ty.kind != TypeKind_Unresolved && s_ty.kind != TypeKind_Set) {
            type_error(ck, n->token, "`%s` requires a Set, got %.*s", callee_name, str8_varg(type_ref_display(ctx_scratch(), s_ty)));
          } else if (s_ty.kind == TypeKind_Set && value_ty.kind != TypeKind_Unresolved && !type_ref_equal(value_ty, *s_ty.pointee)) {
            type_error(ck, n->token, "`%s` value: expected %.*s, got %.*s", callee_name,
                       str8_varg(type_ref_display(ctx_scratch(), *s_ty.pointee)), str8_varg(type_ref_display(ctx_scratch(), value_ty)));
          }
        }
        result.kind = TypeKind_Bool;
      } else if (str8_match_lit("pow2?", n->call.callee, 0)
              || str8_match_lit("pow2-or-zero?", n->call.callee, 0)) {
        // builtin: `(pow2? x)` / `(pow2-or-zero? x)` -- base.h's
        // IsPow2/IsPow2OrZero macros, one numeric argument, always bool.
        if (n->call.arg_count != 1) {
          type_error(ck, n->token, "`%.*s` takes exactly one argument, got %u",
                     str8_varg(n->call.callee), (u32)n->call.arg_count);
        } else {
          TypeRef x_ty = ck->resolved_types[ck->tast->extra[n->call.arg_first + 0]];
          if (x_ty.kind != TypeKind_Unresolved && !type_kind_is_numeric(x_ty.kind)) {
            type_error(ck, n->token, "`%.*s` requires a numeric argument, got %.*s",
                       str8_varg(n->call.callee), str8_varg(type_ref_display(ctx_scratch(), x_ty)));
          }
        }
        result.kind = TypeKind_Bool;
      } else if (str8_match_lit("mem-set", n->call.callee, 0)) {
        // builtin: `(mem-set dst byte size)` -- base.h's MemorySet
        // (memset). `dst` any pointer, `byte`/`size` numeric.
        if (n->call.arg_count != 3) {
          type_error(ck, n->token, "`mem-set` expects 3 arguments (pointer, byte, size), got %u",
                     (u32)n->call.arg_count);
        } else {
          TypeRef dst_ty  = ck->resolved_types[ck->tast->extra[n->call.arg_first + 0]];
          TypeRef byte_ty = ck->resolved_types[ck->tast->extra[n->call.arg_first + 1]];
          TypeRef size_ty = ck->resolved_types[ck->tast->extra[n->call.arg_first + 2]];
          if (dst_ty.kind != TypeKind_Unresolved && dst_ty.kind != TypeKind_Pointer) {
            type_error(ck, n->token, "`mem-set` argument 1 must be a pointer, got %.*s", str8_varg(type_ref_display(ctx_scratch(), dst_ty)));
          }
          if (byte_ty.kind != TypeKind_Unresolved && !type_kind_is_numeric(byte_ty.kind)) {
            type_error(ck, n->token, "`mem-set` argument 2 (byte) must be numeric, got %.*s", str8_varg(type_ref_display(ctx_scratch(), byte_ty)));
          }
          if (size_ty.kind != TypeKind_Unresolved && !type_kind_is_numeric(size_ty.kind)) {
            type_error(ck, n->token, "`mem-set` argument 3 (size) must be numeric, got %.*s", str8_varg(type_ref_display(ctx_scratch(), size_ty)));
          }
        }
        result.kind = TypeKind_Void;
      } else if (str8_match_lit("mem-copy", n->call.callee, 0)) {
        // builtin: `(mem-copy dst src size)` -- base.h's MemoryCopy, a
        // memmove, so overlap-safe. `dst` and `src` are any pointers and need
        // not match: copying between, say, `u8*` and a struct pointer is the
        // point of a raw byte copy.
        if (n->call.arg_count != 3) {
          type_error(ck, n->token, "`mem-copy` expects 3 arguments (dst pointer, src pointer, size), got %u",
                     (u32)n->call.arg_count);
        } else {
          TypeRef dst_ty  = ck->resolved_types[ck->tast->extra[n->call.arg_first + 0]];
          TypeRef src_ty  = ck->resolved_types[ck->tast->extra[n->call.arg_first + 1]];
          TypeRef size_ty = ck->resolved_types[ck->tast->extra[n->call.arg_first + 2]];
          if (dst_ty.kind != TypeKind_Unresolved && dst_ty.kind != TypeKind_Pointer) {
            type_error(ck, n->token, "`mem-copy` argument 1 must be a pointer, got %.*s", str8_varg(type_ref_display(ctx_scratch(), dst_ty)));
          }
          if (src_ty.kind != TypeKind_Unresolved && src_ty.kind != TypeKind_Pointer) {
            type_error(ck, n->token, "`mem-copy` argument 2 must be a pointer, got %.*s", str8_varg(type_ref_display(ctx_scratch(), src_ty)));
          }
          if (size_ty.kind != TypeKind_Unresolved && !type_kind_is_numeric(size_ty.kind)) {
            type_error(ck, n->token, "`mem-copy` argument 3 (size) must be numeric, got %.*s", str8_varg(type_ref_display(ctx_scratch(), size_ty)));
          }
        }
        result.kind = TypeKind_Void;
      } else if (str8_match_lit("mem-zero", n->call.callee, 0)) {
        // builtin: `(mem-zero dst size)` -- base.h's MemoryZero (memset 0).
        if (n->call.arg_count != 2) {
          type_error(ck, n->token, "`mem-zero` expects 2 arguments (pointer, size), got %u",
                     (u32)n->call.arg_count);
        } else {
          TypeRef dst_ty  = ck->resolved_types[ck->tast->extra[n->call.arg_first + 0]];
          TypeRef size_ty = ck->resolved_types[ck->tast->extra[n->call.arg_first + 1]];
          if (dst_ty.kind != TypeKind_Unresolved && dst_ty.kind != TypeKind_Pointer) {
            type_error(ck, n->token, "`mem-zero` argument 1 must be a pointer, got %.*s", str8_varg(type_ref_display(ctx_scratch(), dst_ty)));
          }
          if (size_ty.kind != TypeKind_Unresolved && !type_kind_is_numeric(size_ty.kind)) {
            type_error(ck, n->token, "`mem-zero` argument 2 (size) must be numeric, got %.*s", str8_varg(type_ref_display(ctx_scratch(), size_ty)));
          }
        }
        result.kind = TypeKind_Void;
      } else if (str8_match_lit("mem-compare", n->call.callee, 0)) {
        // builtin: `(mem-compare a b size)` -- base.h's MemoryCompare.
        // Returns memcmp's i32, not a bool, so equality is
        // `(= (mem-compare a b size) 0)`.
        if (n->call.arg_count != 3) {
          type_error(ck, n->token, "`mem-compare` expects 3 arguments (pointer, pointer, size), got %u",
                     (u32)n->call.arg_count);
        } else {
          TypeRef a_ty    = ck->resolved_types[ck->tast->extra[n->call.arg_first + 0]];
          TypeRef b_ty    = ck->resolved_types[ck->tast->extra[n->call.arg_first + 1]];
          TypeRef size_ty = ck->resolved_types[ck->tast->extra[n->call.arg_first + 2]];
          if (a_ty.kind != TypeKind_Unresolved && a_ty.kind != TypeKind_Pointer) {
            type_error(ck, n->token, "`mem-compare` argument 1 must be a pointer, got %.*s", str8_varg(type_ref_display(ctx_scratch(), a_ty)));
          }
          if (b_ty.kind != TypeKind_Unresolved && b_ty.kind != TypeKind_Pointer) {
            type_error(ck, n->token, "`mem-compare` argument 2 must be a pointer, got %.*s", str8_varg(type_ref_display(ctx_scratch(), b_ty)));
          }
          if (size_ty.kind != TypeKind_Unresolved && !type_kind_is_numeric(size_ty.kind)) {
            type_error(ck, n->token, "`mem-compare` argument 3 (size) must be numeric, got %.*s", str8_varg(type_ref_display(ctx_scratch(), size_ty)));
          }
        }
        result.kind = TypeKind_I32;
      } else if (str8_match_lit("string-prefix", n->call.callee, 0)
              || str8_match_lit("string-skip", n->call.callee, 0)
              || str8_match_lit("string-postfix", n->call.callee, 0)
              || str8_match_lit("string-chop", n->call.callee, 0)) {
        // builtin: base.h's str8_prefix/str8_skip/str8_postfix/str8_chop.
        // Each returns a view into `s`, so there is no arena argument.
        if (n->call.arg_count != 2) {
          type_error(ck, n->token, "`%.*s` expects 2 arguments (a string, a count), got %u",
                     str8_varg(n->call.callee), (u32)n->call.arg_count);
        } else {
          TypeRef s_ty = ck->resolved_types[ck->tast->extra[n->call.arg_first + 0]];
          TypeRef n_ty = ck->resolved_types[ck->tast->extra[n->call.arg_first + 1]];
          if (s_ty.kind != TypeKind_Unresolved && s_ty.kind != TypeKind_String) {
            type_error(ck, n->token, "`%.*s` argument 1 must be a string, got %.*s",
                       str8_varg(n->call.callee), str8_varg(type_ref_display(ctx_scratch(), s_ty)));
          }
          if (n_ty.kind != TypeKind_Unresolved && !type_kind_is_numeric(n_ty.kind)) {
            type_error(ck, n->token, "`%.*s` argument 2 must be numeric, got %.*s",
                       str8_varg(n->call.callee), str8_varg(type_ref_display(ctx_scratch(), n_ty)));
          }
        }
        result.kind = TypeKind_String;
      } else if (str8_match_lit("string-substr", n->call.callee, 0)) {
        // builtin: `(string-substr s start end)` -- base.h's str8_substr over
        // a [start, end) range, `end` exclusive as in `for` ranges. A view,
        // like prefix/skip/postfix/chop above.
        if (n->call.arg_count != 3) {
          type_error(ck, n->token, "`string-substr` expects 3 arguments (a string, start, end), got %u",
                     (u32)n->call.arg_count);
        } else {
          TypeRef s_ty     = ck->resolved_types[ck->tast->extra[n->call.arg_first + 0]];
          TypeRef start_ty = ck->resolved_types[ck->tast->extra[n->call.arg_first + 1]];
          TypeRef end_ty   = ck->resolved_types[ck->tast->extra[n->call.arg_first + 2]];
          if (s_ty.kind != TypeKind_Unresolved && s_ty.kind != TypeKind_String) {
            type_error(ck, n->token, "`string-substr` argument 1 must be a string, got %.*s", str8_varg(type_ref_display(ctx_scratch(), s_ty)));
          }
          if (start_ty.kind != TypeKind_Unresolved && !type_kind_is_numeric(start_ty.kind)) {
            type_error(ck, n->token, "`string-substr` argument 2 (start) must be numeric, got %.*s", str8_varg(type_ref_display(ctx_scratch(), start_ty)));
          }
          if (end_ty.kind != TypeKind_Unresolved && !type_kind_is_numeric(end_ty.kind)) {
            type_error(ck, n->token, "`string-substr` argument 3 (end) must be numeric, got %.*s", str8_varg(type_ref_display(ctx_scratch(), end_ty)));
          }
        }
        result.kind = TypeKind_String;
      } else if (str8_match_lit("string-find", n->call.callee, 0)
              || str8_match_lit("string-find-reverse", n->call.callee, 0)) {
        // builtin: base.h's str8_find_needle/str8_find_needle_reverse,
        // bridged through 3b_runtime (cg_write_runtime_source) as
        // `string-match` is, and taking the same raw StringMatchFlags
        // bitmask. Returns the 0-based match offset, or `s.size` if absent.
        if (n->call.arg_count != 4) {
          type_error(ck, n->token, "`%.*s` expects 4 arguments (string, needle, start-pos, flags), got %u",
                     str8_varg(n->call.callee), (u32)n->call.arg_count);
        } else {
          TypeRef s_ty     = ck->resolved_types[ck->tast->extra[n->call.arg_first + 0]];
          TypeRef needle_ty = ck->resolved_types[ck->tast->extra[n->call.arg_first + 1]];
          TypeRef start_ty = ck->resolved_types[ck->tast->extra[n->call.arg_first + 2]];
          TypeRef flags_ty = ck->resolved_types[ck->tast->extra[n->call.arg_first + 3]];
          if (s_ty.kind != TypeKind_Unresolved && s_ty.kind != TypeKind_String) {
            type_error(ck, n->token, "`%.*s` argument 1 must be a string, got %.*s",
                       str8_varg(n->call.callee), str8_varg(type_ref_display(ctx_scratch(), s_ty)));
          }
          if (needle_ty.kind != TypeKind_Unresolved && needle_ty.kind != TypeKind_String) {
            type_error(ck, n->token, "`%.*s` argument 2 (needle) must be a string, got %.*s",
                       str8_varg(n->call.callee), str8_varg(type_ref_display(ctx_scratch(), needle_ty)));
          }
          if (start_ty.kind != TypeKind_Unresolved && !type_kind_is_numeric(start_ty.kind)) {
            type_error(ck, n->token, "`%.*s` argument 3 (start-pos) must be numeric, got %.*s",
                       str8_varg(n->call.callee), str8_varg(type_ref_display(ctx_scratch(), start_ty)));
          }
          if (flags_ty.kind != TypeKind_Unresolved && !type_kind_is_numeric(flags_ty.kind)) {
            type_error(ck, n->token, "`%.*s` argument 4 (flags) must be numeric, got %.*s",
                       str8_varg(n->call.callee), str8_varg(type_ref_display(ctx_scratch(), flags_ty)));
          }
        }
        result.kind = TypeKind_U64;
      } else if (str8_match_lit("string-starts-with", n->call.callee, 0)
              || str8_match_lit("string-ends-with", n->call.callee, 0)) {
        // builtin: base.h's str8_starts_with/str8_ends_with, bridged as
        // `string-find` above, with the same raw flags bitmask.
        if (n->call.arg_count != 3) {
          type_error(ck, n->token, "`%.*s` expects 3 arguments (string, affix, flags), got %u",
                     str8_varg(n->call.callee), (u32)n->call.arg_count);
        } else {
          TypeRef s_ty      = ck->resolved_types[ck->tast->extra[n->call.arg_first + 0]];
          TypeRef affix_ty  = ck->resolved_types[ck->tast->extra[n->call.arg_first + 1]];
          TypeRef flags_ty  = ck->resolved_types[ck->tast->extra[n->call.arg_first + 2]];
          if (s_ty.kind != TypeKind_Unresolved && s_ty.kind != TypeKind_String) {
            type_error(ck, n->token, "`%.*s` argument 1 must be a string, got %.*s",
                       str8_varg(n->call.callee), str8_varg(type_ref_display(ctx_scratch(), s_ty)));
          }
          if (affix_ty.kind != TypeKind_Unresolved && affix_ty.kind != TypeKind_String) {
            type_error(ck, n->token, "`%.*s` argument 2 must be a string, got %.*s",
                       str8_varg(n->call.callee), str8_varg(type_ref_display(ctx_scratch(), affix_ty)));
          }
          if (flags_ty.kind != TypeKind_Unresolved && !type_kind_is_numeric(flags_ty.kind)) {
            type_error(ck, n->token, "`%.*s` argument 3 (flags) must be numeric, got %.*s",
                       str8_varg(n->call.callee), str8_varg(type_ref_display(ctx_scratch(), flags_ty)));
          }
        }
        result.kind = TypeKind_Bool;
      } else if (str8_match_lit("string-cat", n->call.callee, 0)) {
        // builtin: `(string-cat arena a b)` -- base.h's str8_cat. Allocates
        // the concatenation, so it takes an arena first, as `push`/`alloc` do.
        if (n->call.arg_count != 3) {
          type_error(ck, n->token, "`string-cat` expects 3 arguments (an arena, a string, a string), got %u",
                     (u32)n->call.arg_count);
        } else {
          TypeRef arena_ty = ck->resolved_types[ck->tast->extra[n->call.arg_first + 0]];
          TypeRef a_ty     = ck->resolved_types[ck->tast->extra[n->call.arg_first + 1]];
          TypeRef b_ty     = ck->resolved_types[ck->tast->extra[n->call.arg_first + 2]];
          if (arena_ty.kind != TypeKind_Unresolved && arena_ty.kind != TypeKind_Arena) {
            type_error(ck, n->token, "`string-cat` argument 1 must be an arena, got %.*s", str8_varg(type_ref_display(ctx_scratch(), arena_ty)));
          }
          if (a_ty.kind != TypeKind_Unresolved && a_ty.kind != TypeKind_String) {
            type_error(ck, n->token, "`string-cat` argument 2 must be a string, got %.*s", str8_varg(type_ref_display(ctx_scratch(), a_ty)));
          }
          if (b_ty.kind != TypeKind_Unresolved && b_ty.kind != TypeKind_String) {
            type_error(ck, n->token, "`string-cat` argument 3 must be a string, got %.*s", str8_varg(type_ref_display(ctx_scratch(), b_ty)));
          }
        }
        result.kind = TypeKind_String;
      } else if (str8_match_lit("string-copy", n->call.callee, 0)) {
        // builtin: `(string-copy arena s)` -- base.h's str8_copy, an
        // independent copy of `s` rather than an alias of its bytes.
        if (n->call.arg_count != 2) {
          type_error(ck, n->token, "`string-copy` expects 2 arguments (an arena, a string), got %u",
                     (u32)n->call.arg_count);
        } else {
          TypeRef arena_ty = ck->resolved_types[ck->tast->extra[n->call.arg_first + 0]];
          TypeRef s_ty     = ck->resolved_types[ck->tast->extra[n->call.arg_first + 1]];
          if (arena_ty.kind != TypeKind_Unresolved && arena_ty.kind != TypeKind_Arena) {
            type_error(ck, n->token, "`string-copy` argument 1 must be an arena, got %.*s", str8_varg(type_ref_display(ctx_scratch(), arena_ty)));
          }
          if (s_ty.kind != TypeKind_Unresolved && s_ty.kind != TypeKind_String) {
            type_error(ck, n->token, "`string-copy` argument 2 must be a string, got %.*s", str8_varg(type_ref_display(ctx_scratch(), s_ty)));
          }
        }
        result.kind = TypeKind_String;
      } else if (str8_match_lit("cstring-copy", n->call.callee, 0)) {
        // builtin: `(cstring-copy arena s)` -- base.h's cstring_str8,
        // allocating a fresh nul-terminated copy. The `cstring` special form
        // instead reinterprets an already-nul-terminated string's `.str` as
        // `char*` without copying, so this is what a computed string needs
        // before reaching a C API expecting `char*`.
        if (n->call.arg_count != 2) {
          type_error(ck, n->token, "`cstring-copy` expects 2 arguments (an arena, a string), got %u",
                     (u32)n->call.arg_count);
        } else {
          TypeRef arena_ty = ck->resolved_types[ck->tast->extra[n->call.arg_first + 0]];
          TypeRef s_ty     = ck->resolved_types[ck->tast->extra[n->call.arg_first + 1]];
          if (arena_ty.kind != TypeKind_Unresolved && arena_ty.kind != TypeKind_Arena) {
            type_error(ck, n->token, "`cstring-copy` argument 1 must be an arena, got %.*s", str8_varg(type_ref_display(ctx_scratch(), arena_ty)));
          }
          if (s_ty.kind != TypeKind_Unresolved && s_ty.kind != TypeKind_String) {
            type_error(ck, n->token, "`cstring-copy` argument 2 must be a string, got %.*s", str8_varg(type_ref_display(ctx_scratch(), s_ty)));
          }
        }
        TypeRef* boxed = push_one(ctx_perm(), TypeRef);
        boxed->kind    = TypeKind_Char;
        result.kind    = TypeKind_Pointer;
        result.pointee = boxed;
      } else if (callee_local && callee_local->type.kind == TypeKind_Fn) {
        // Indirect call through a local or param of function-pointer type,
        // e.g. `printer` in `(fn takes-fn [printer (fn [str string] void)]
        // void (printer "hello"))`. Checked ahead of fn_table_lookup below,
        // since a local shadows a top-level `fn` of the same name. Codegen
        // needs nothing special for the syntax -- C calls a function pointer
        // as `name(args)` too -- only to resolve the name as a local rather
        // than mangling it as a top-level symbol (cg_call's fallback).
        TypeRef fnty = callee_local->type;
        if (n->call.arg_count != fnty.fn_param_count) {
          type_error(ck, n->token, "`%.*s` expects %u argument(s), got %u",
                     str8_varg(n->call.callee), fnty.fn_param_count, (u32)n->call.arg_count);
        } else {
          foreach_index(i, fnty.fn_param_count) {
            TypedIndex arg_idx = ck->tast->extra[n->call.arg_first + i];
            TypeRef    arg_ty  = ck->resolved_types[arg_idx];
            if (arg_ty.kind != TypeKind_Unresolved && !type_ref_assignable(arg_ty, fnty.fn_params[i])) {
              type_error(ck, n->token, "`%.*s` argument %u: expected %.*s, got %.*s",
                         str8_varg(n->call.callee), (u32)(i + 1),
                         str8_varg(type_ref_display(ctx_scratch(), fnty.fn_params[i])), str8_varg(type_ref_display(ctx_scratch(), arg_ty)));
            }
          }
        }
        result = *fnty.fn_return;
      } else {
        // By now ck.fns holds every legitimate call target: this package's
        // `fn`/`extern fn` from pass 1, plus each imported package's public
        // fns, spliced in as bodyless FunctionDecls before check_program runs
        // (main.c's compile_package/splice_public_decl). A miss is a genuine
        // undefined call, not an unresolved cross-package reference.
        FnEntry* fn = fn_table_lookup(ck, n->call.callee);
        if (!fn) {
          type_error(ck, n->token, "call to undefined function `%.*s`", str8_varg(n->call.callee));
          result = type_ref_unresolved();
        } else {
          TypedNode* callee_decl = &ck->tast->nodes[fn->decl];
          if (callee_decl->func.is_lane_fn && !ck->in_parallel_block) {
            type_error(ck, n->token,
                       "`%.*s` is a `lane-fn` -- only callable from inside a `parallel` block (or another `lane-fn`)",
                       str8_varg(n->call.callee));
          }
          // Variadic externs (`args ...`) require only a minimum arg count,
          // and only the fixed prefix is type-checked: as with C's
          // printf family, the trailing args have no static types to check.
          b32 arity_ok = callee_decl->func.is_variadic
                       ? n->call.arg_count >= callee_decl->func.param_count
                       : n->call.arg_count == callee_decl->func.param_count;
          if (!arity_ok) {
            type_error(ck, n->token, "`%.*s` expects %s%u argument(s), got %u",
                       str8_varg(n->call.callee), callee_decl->func.is_variadic ? "at least " : "",
                       (u32)callee_decl->func.param_count, (u32)n->call.arg_count);
          } else {
            foreach_index(i, callee_decl->func.param_count) {
              TypedIndex arg_idx = ck->tast->extra[n->call.arg_first + i];
              TypeRef    arg_ty  = ck->resolved_types[arg_idx];
              Param*     param   = &ck->tast->params[callee_decl->func.param_first + i];
              if (arg_ty.kind != TypeKind_Unresolved && !type_ref_assignable(arg_ty, param->type)) {
                type_error(ck, n->token, "`%.*s` argument %u: expected %.*s, got %.*s",
                           str8_varg(n->call.callee), (u32)(i + 1),
                           str8_varg(type_ref_display(ctx_scratch(), param->type)), str8_varg(type_ref_display(ctx_scratch(), arg_ty)));
              } else if (param->type.kind == TypeKind_Vector
                         && !addr_operand_is_valid(&ck->tast->nodes[arg_idx])) {
                // A Vector parameter is passed by reference
                // (cg_declare_param/cg_expr_decay_to) so a vector-push in the
                // callee grows the caller's Vector rather than a local copy
                // of its pointer. That needs a real address, so the argument
                // must satisfy the same addressability `&` requires.
                type_error(ck, n->token,
                           "`%.*s` argument %u: a Vector parameter must be a plain variable/field/index "
                           "(it's passed by reference so the callee can grow it) -- got a temporary value",
                           str8_varg(n->call.callee), (u32)(i + 1));
              }
            }
          }
          result = callee_decl->func.return_type;
        }
      }
    } break;
    default: break;
  }
  resolved_types_ensure_capacity(ck, idx);
  ck->resolved_types[idx] = result;
  return result;
}

// `main` is the one name codegen treats specially, becoming the program's
// literal C `int main(...)` instead of a package-prefixed function (see
// cg_symbol_name). Enforcing the signature here keeps codegen from emitting
// something C rejects: zero parameters or exactly two -- `i32` argc then
// `string*` argv, named whatever the author likes -- an `i32` return type,
// and public, since a `static int main` would never be seen by the runtime
// that calls it.
//
// Only the root package's `main` gets this. In a package pulled in as a
// dependency, a public `fn main` is an ordinary function with whatever
// signature it likes, package-prefixed like any other public symbol -- see
// the matching gate in cg_symbol_name/cg_function/cg_function_prototype.
// Without the gate, importing such a package would fail to link with
// "multiple definition of `main`".
static void
check_main_signature(Checker* ck, TypedNode* stmt_node) {
  if (!str8_match_lit("main", stmt_node->func.name, 0)) return;
  if (!ck->is_root_package) return; // an ordinary public fn, not the entry point
  if (stmt_node->is_private) {
    type_error(ck, stmt_node->token,
               "`main` cannot be `private` -- it is the program's C entry point and must be externally linkable");
  }
  if (stmt_node->func.return_type.kind != TypeKind_I32) {
    type_error(ck, stmt_node->token, "`fn main` must return i32, got %.*s",
               str8_varg(type_ref_display(ctx_scratch(), stmt_node->func.return_type)));
  }
  u16 pc = stmt_node->func.param_count;
  if (pc != 0 && pc != 2) {
    type_error(ck, stmt_node->token,
               "`fn main` must take either no parameters or exactly two (argc, argv), got %u", (u32)pc);
    return;
  }
  if (pc == 2) {
    Param* argc_p = &ck->tast->params[stmt_node->func.param_first + 0];
    Param* argv_p = &ck->tast->params[stmt_node->func.param_first + 1];
    if (argc_p->type.kind != TypeKind_I32) {
      type_error(ck, stmt_node->token, "`fn main`'s first parameter (%.*s) must be `i32`, got %.*s",
                 str8_varg(argc_p->name), str8_varg(type_ref_display(ctx_scratch(), argc_p->type)));
    }
    b32 argv_ok = argv_p->type.kind == TypeKind_Pointer && argv_p->type.pointee != NULL
               && argv_p->type.pointee->kind == TypeKind_String;
    if (!argv_ok) {
      type_error(ck, stmt_node->token, "`fn main`'s second parameter (%.*s) must be `string*`, got %.*s",
                 str8_varg(argv_p->name), str8_varg(type_ref_display(ctx_scratch(), argv_p->type)));
    }
  }
}

// Pass 1 gathers every top-level `fn` so calls resolve regardless of
// declaration order, mutual recursion included. Pass 2 checks top-level
// forms in source order: `val`/`var` join one shared global scope as they
// are reached, and each function body is checked against that scope plus its
// own params, popped afterward so params don't leak into what follows.
Checker
check_program(TypedAst* tast, TypedIndex root, b32 is_root_package, const ScopeQuery* scope_query) {
  Checker ck = {0};
  ck.tast            = tast;
  ck.resolved_types     = push_array_zero(ctx_perm(), TypeRef, dyn_count(tast->nodes));
  ck.resolved_types_cap = dyn_count(tast->nodes);
  ck.is_root_package = is_root_package;
  ck.scope_query     = scope_query;

  TypedNode* n = &tast->nodes[root];
  xassert(n->kind == TypedNodeKind_Block);

  // Membership only, with no per-entry data, so unlike fns/structs/enums
  // below it can be filled directly in the pass-1 loop -- there are no
  // element pointers for a dyn_push reallocation to invalidate.
  hashtable_init(ctx_perm(), &ck.handle_pool_types, 64);

  foreach_index(i, n->block.stmt_count) {
    TypedIndex stmt = tast->extra[n->block.stmt_first + i];
    if (stmt == TYPED_NIL) continue;
    TypedNode* stmt_node = &tast->nodes[stmt];
    if (stmt_node->kind == TypedNodeKind_FunctionDecl) {
      FnEntry e = {0};
      e.name    = stmt_node->func.name;
      e.decl    = stmt;
      dyn_push(ctx_perm(), ck.fns, e);
    } else if (stmt_node->kind == TypedNodeKind_StructDecl) {
      StructEntry e = {0};
      e.name        = stmt_node->struct_decl.name;
      e.decl        = stmt;
      dyn_push(ctx_perm(), ck.structs, e);
    } else if (stmt_node->kind == TypedNodeKind_EnumDecl) {
      EnumEntry e = {0};
      e.name      = stmt_node->enum_decl.name;
      e.decl      = stmt;
      e.is_flags  = stmt_node->enum_decl.is_flags;
      dyn_push(ctx_perm(), ck.enums, e);
    } else if (stmt_node->kind == TypedNodeKind_HandlePoolDecl) {
      hashtable_insert(ctx_perm(), &ck.handle_pool_types, stmt_node->handle_pool_decl.type_name, (void*)1, true);
    }
  }

  // Built only once the arrays above have stopped growing: dyn_push may
  // reallocate, invalidating any &ck.fns[i] taken mid-loop. Nothing pushes to
  // them after this point, so these pointers stay valid for the rest of
  // check_program.
  //
  // A lookup hit is always a real duplicate declaration within this package.
  // An imported package's spliced-in decls carry package-qualified names
  // ("pkgname/member" -- see is_imported, 3b.h), so they collide neither with
  // local names nor with each other. Each name is inserted anyway after the
  // error, so later checking resolves against some entry rather than a hole.
  hashtable_init(ctx_perm(), &ck.fns_by_name, 64);
  foreach_index(i, dyn_count(ck.fns)) {
    if (hashtable_lookup(&ck.fns_by_name, ck.fns[i].name) != NULL) {
      type_error(&ck, tast->nodes[ck.fns[i].decl].token, "`fn %.*s` is declared more than once in this package",
                 str8_varg(ck.fns[i].name));
    }
    hashtable_insert(ctx_perm(), &ck.fns_by_name, ck.fns[i].name, &ck.fns[i], true);
  }
  hashtable_init(ctx_perm(), &ck.structs_by_name, 64);
  foreach_index(i, dyn_count(ck.structs)) {
    if (hashtable_lookup(&ck.structs_by_name, ck.structs[i].name) != NULL) {
      type_error(&ck, tast->nodes[ck.structs[i].decl].token, "`struct %.*s` is declared more than once in this package",
                 str8_varg(ck.structs[i].name));
    }
    hashtable_insert(ctx_perm(), &ck.structs_by_name, ck.structs[i].name, &ck.structs[i], true);
  }
  // Needs the whole table in place (it follows field types across structs), and
  // has to precede every function body -- see check_struct_cycles' own note.
  check_struct_cycles(&ck);
  hashtable_init(ctx_perm(), &ck.enums_by_name, 64);
  foreach_index(i, dyn_count(ck.enums)) {
    if (hashtable_lookup(&ck.enums_by_name, ck.enums[i].name) != NULL) {
      type_error(&ck, tast->nodes[ck.enums[i].decl].token, "`%s %.*s` is declared more than once in this package",
                 ck.enums[i].is_flags ? "flags" : "enum", str8_varg(ck.enums[i].name));
    }
    hashtable_insert(ctx_perm(), &ck.enums_by_name, ck.enums[i].name, &ck.enums[i], true);
  }
  // Needs both name tables above, and runs before any body so a misspelled
  // type is reported once at its annotation instead of once per use.
  check_type_annotations(&ck);

  Scope global_scope = {0};
  // Name-gathering pre-pass for top-level `val`/`var`, for the same reason
  // ck.fns/ck.structs/ck.enums have one: the loop below binds each global as
  // it reaches it, so without this a global would only be visible to code
  // later in the package's alphabetical file order.
  //
  // Only the name and declared type are bound -- initializers are still
  // checked in order by the loop below -- so this affects visibility, not
  // evaluation order. That loop binds each name a second time when it reaches
  // the real declaration; scope_lookup_entry scans newest-first, so the later
  // identical entry simply shadows this one.
  foreach_index(i, n->block.stmt_count) {
    TypedIndex stmt = tast->extra[n->block.stmt_first + i];
    if (stmt == TYPED_NIL) continue;
    TypedNode* stmt_node = &tast->nodes[stmt];
    if (stmt_node->kind == TypedNodeKind_ConstDecl) {
      scope_bind_immutable(&global_scope, stmt_node->const_decl.name, stmt_node->const_decl.type,
                           stmt_node->token);
    } else if (stmt_node->kind == TypedNodeKind_VarDecl) {
      scope_bind_mutable(&global_scope, stmt_node->var_decl.name, stmt_node->var_decl.type, stmt_node->token);
    }
  }
  foreach_index(i, n->block.stmt_count) {
    TypedIndex stmt = tast->extra[n->block.stmt_first + i];
    if (stmt == TYPED_NIL) continue;
    TypedNode* stmt_node = &tast->nodes[stmt];
    switch (stmt_node->kind) {
      case TypedNodeKind_ConstDecl:
      case TypedNodeKind_VarDecl: {
        check_decl_and_bind(&ck, &global_scope, stmt_node);
      } break;
      case TypedNodeKind_StructDecl: check_struct_decl(&ck, stmt_node); break;
      case TypedNodeKind_EnumDecl: check_enum_decl(&ck, stmt_node); break;
      case TypedNodeKind_AliasDecl: break;  // fully resolved during lowering
      case TypedNodeKind_HandlePoolDecl: {
        if (struct_table_lookup(&ck, stmt_node->handle_pool_decl.type_name) == NULL) {
          type_error(&ck, stmt_node->token, "`handle` requires a declared struct, got `%.*s`",
                     str8_varg(stmt_node->handle_pool_decl.type_name));
        }
      } break;
      case TypedNodeKind_FunctionDecl: {
        if (stmt_node->func.body == TYPED_NIL) {
          // extern: no body to check. The pass-1 loop ignores func.body, so
          // it is registered in ck.fns and calls to it are still validated.
          break;
        }
        check_main_signature(&ck, stmt_node);
        u64 mark = scope_mark(&global_scope);
        ck.fn_scope_base = mark; // everything bound from here on is this function's own
        foreach_index(pi, stmt_node->func.param_count) {
          Param* p = &tast->params[stmt_node->func.param_first + pi];
          scope_bind_mutable(&global_scope, p->name, p->type, stmt_node->token);
        }
        // Not a stack: check_expr has no FunctionDecl case, so this loop is
        // the only thing that enters a body and only one is ever in flight
        // (see the Checker fields, 3b.h).
        ck.current_fn_return_type = stmt_node->func.return_type;
        ck.in_function_body       = true;
        // `lane-fn` checks its body as if already inside a `parallel` block,
        // so it can use lane-index/parallel-for and call other lane-fns. One
        // body in flight makes the unconditional set and reset correct; a
        // `parallel` written inside a lane-fn still hits ParallelExpr's
        // no-nesting rejection.
        ck.in_parallel_block      = stmt_node->func.is_lane_fn;
        ck.loop_depth             = 0; // a body always starts outside every loop; balanced
                                       // increments in check_loop_body make the reset belt-and-braces
        // The declared return type is often the only thing naming the width an
        // implicit-return literal should have -- `(fn f [] i64 5)`, whose body
        // is a Block around the 5. An explicit `(return 5)` is handled at
        // ReturnExpr instead, where the same type is on the Checker.
        adopt_int_literal_in_tail(&ck, stmt_node->func.body, stmt_node->func.return_type);
        TypeRef body_ty = check_expr(&ck, &global_scope, stmt_node->func.body);
        stmt_node = &tast->nodes[stmt]; // typed_push may have moved tast->nodes
        ck.in_function_body       = false;
        ck.in_parallel_block      = false;
        // Params occupy slots mark..mark+count in the order bound above; see
        // the same read-back in check_expr's LetExpr case.
        foreach_index(pi, stmt_node->func.param_count) {
          tast->params[stmt_node->func.param_first + pi].is_read = global_scope.entries[mark + pi].was_read;
        }
        if (stmt_node->func.return_type.kind != TypeKind_Void &&
            body_ty.kind != TypeKind_Unresolved &&
            !type_ref_equal(body_ty, stmt_node->func.return_type)) {
          type_error(&ck, stmt_node->token, "`fn %.*s` declared to return %.*s but body evaluates to %.*s",
                     str8_varg(stmt_node->func.name),
                     str8_varg(type_ref_display(ctx_scratch(), stmt_node->func.return_type)),
                     str8_varg(type_ref_display(ctx_scratch(), body_ty)));
        }
        scope_pop_to(&global_scope, mark);
      } break;
      default: break;
    }
  }

  return ck;
}

// Reproduces cg_symbol_name's mangling for one FunctionDecl without a live
// Codegen, so compiler.c can detect collisions right after check_program and
// before codegen runs. Must be kept in lockstep with cg_symbol_name by hand:
// `extern` (bodyless) and `private` fns mangle to the bare name, only a
// public non-extern fn takes the package prefix, and the root package's
// `main` is left alone as C's entry point.
static String8
mangled_c_fn_name(Arena* arena, String8 pkg_name, b32 is_root_package,
                   String8 fn_name, b32 is_extern, b32 is_private) {
  if (!is_extern && !is_private && is_root_package && str8_match_lit("main", fn_name, 0)) {
    return fn_name;
  }
  if (is_extern || is_private || pkg_name.size == 0) return c_mangle_name(arena, fn_name);
  String8 mangled_pkg  = c_mangle_name(arena, pkg_name);
  String8 mangled_name = c_mangle_name(arena, fn_name);
  return str8_cat(arena, str8_cat(arena, mangled_pkg, str8_lit("_")), mangled_name);
}

// Two different top-level fn names -- an `extern` bound to a hand-picked C
// name and a public wrapper, say -- can mangle to the same C symbol. Codegen
// emits one declaration at a time and never cross-checks, so the bodyless
// extern and the real definition become one symbol and calls meant for the
// external function reach the wrapper instead, with no diagnostic from
// either the C compiler or the linker.
//
// Only ck->fns is checked. An imported package's spliced-in surface mangles
// during its own compilation, so it can't collide here. Exact duplicate
// names are skipped, since the fns_by_name pass in check_program reports
// those.
void
check_mangled_name_collisions(Checker* ck, String8 pkg_name, b32 is_root_package) {
  ArenaTemp temp = arena_temp_begin(ctx_scratch());
  HashTable seen;
  hashtable_init(temp.arena, &seen, 64);
  foreach_index(i, dyn_count(ck->fns)) {
    FnEntry*   e  = &ck->fns[i];
    TypedNode* dn = &ck->tast->nodes[e->decl];
    if (dn->is_imported) continue;
    b32     is_extern = dn->func.body == TYPED_NIL;
    String8 mangled   = mangled_c_fn_name(temp.arena, pkg_name, is_root_package, e->name,
                                           is_extern, dn->is_private);
    FnEntry* prior = (FnEntry*)hashtable_lookup(&seen, mangled);
    if (prior != NULL && !str8_match(prior->name, e->name, 0)) {
      type_error(ck, dn->token,
                 "`fn %.*s` compiles to the same C symbol (`%.*s`) as `fn %.*s` in this package -- "
                 "one will silently shadow or self-call the other; rename one of them",
                 str8_varg(e->name), str8_varg(mangled), str8_varg(prior->name));
    } else {
      hashtable_insert(temp.arena, &seen, mangled, e, true);
    }
  }
  arena_temp_end(&temp);
}
