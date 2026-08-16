// Lowering: the raw AST becomes a TypedAst, the form every later pass reads.
//
// This is where surface syntax stops mattering. Sugar is expanded here rather
// than given its own node kind, so `when`, `match`, `->`, `get-in`, `+=` and
// the rest cost the checker and codegen nothing: they see only the handful of
// core kinds those forms desugar into.
//
// Lowering has no type information. Anything needing a type -- a `for`'s
// implicit step, a positional destructuring slot, a `.` hop that may be a
// field or a Map key -- is emitted as a placeholder node the checker resolves
// once types are known.
//
// Two invariants run through the file:
//
//   Names first. lower_program pre-gathers struct, enum and alias names before
//   lowering anything, so declarations may be referenced before they appear.
//
//   Scratch, then flush. Nodes appending to a shared TypedAst array collect
//   into a temp arena first and flush in one pass. Lowering recurses, and a
//   nested form pushing into the same array would otherwise land inside an
//   outer node's first..first+count range and break its contiguity.
//
// Anonymous struct types synthesized mid-expression cannot be appended to the
// top level from where they are discovered, so they go on low->pending_toplevel
// and lower_program splices each in just ahead of the form that triggered it.

#include "3b.h"

static void
lower_ensure_name_tables(Lowerer* low) {
  if (low->name_tables_init) return;
  hashtable_init(ctx_perm(), &low->struct_form_by_name, 64);
  hashtable_init(ctx_perm(), &low->enum_name_set, 64);
  hashtable_init(ctx_perm(), &low->alias_by_name, 64);
  hashtable_init(ctx_perm(), &low->handle_pool_type_set, 64);
  hashtable_init(ctx_perm(), &low->anon_struct_by_shape, 32);
  hashtable_init(ctx_perm(), &low->hashtable_instances_emitted, 16);
  hashtable_init(ctx_perm(), &low->multi_return_struct_set, 16);
  low->name_tables_init = true;
}

// The only sanctioned way to add a struct, enum or alias name, so the ordered
// arrays and their mirror HashTables cannot drift apart. See 3b.h.
void
lower_register_struct_name(Lowerer* low, String8 name, NodeIndex form) {
  lower_ensure_name_tables(low);
  dyn_push(low->tast->arena, low->struct_names, name);
  dyn_push(low->tast->arena, low->struct_decl_forms, form);
  NodeIndex* boxed = push_one(ctx_perm(), NodeIndex);
  *boxed           = form;
  hashtable_insert(ctx_perm(), &low->struct_form_by_name, name, boxed, false);
}

// The `[f1 t1 f2 t2 ...]` vector of a raw `(struct Name [...])` form, or
// NODE_NIL if the form is not one. A translated mirror carries an extra pinned
// C spelling -- `(struct data "cgltf_data" [...])` -- so the field vector is
// the last child rather than a fixed index. Callers that walk a struct's
// declared fields straight off the AST (member-type, positional construction)
// go through this rather than indexing children themselves.
static NodeIndex
lower_struct_form_fields(Lowerer* low, NodeIndex form) {
  if (form == NODE_NIL) return NODE_NIL;
  u16        sc;
  NodeIndex* schildren = ast_seq_children(low->ast, form, &sc);
  if (sc != 3 && sc != 4) return NODE_NIL;
  NodeIndex fields = schildren[sc - 1];
  if (ast_get(low->ast, fields)->kind != AstNodeKind_Vector) return NODE_NIL;
  return fields;
}

void
lower_register_enum_name(Lowerer* low, String8 name) {
  lower_ensure_name_tables(low);
  dyn_push(low->tast->arena, low->enum_names, name);
  hashtable_insert(ctx_perm(), &low->enum_name_set, name, (void*)1, false);
}

void
lower_register_alias(Lowerer* low, TypeAlias alias) {
  lower_ensure_name_tables(low);
  dyn_push(low->tast->arena, low->aliases, alias);
  TypeAlias* boxed = push_one(ctx_perm(), TypeAlias);
  *boxed           = alias;
  hashtable_insert(ctx_perm(), &low->alias_by_name, alias.name, boxed, false);
}

b32
is_known_struct_name(Lowerer* low, String8 name) {
  lower_ensure_name_tables(low);
  return hashtable_lookup(&low->struct_form_by_name, name) != NULL;
}

b32
is_known_enum_name(Lowerer* low, String8 name) {
  lower_ensure_name_tables(low);
  return hashtable_lookup(&low->enum_name_set, name) != NULL;
}

b32
is_known_handle_pool_type(Lowerer* low, String8 name) {
  lower_ensure_name_tables(low);
  return hashtable_lookup(&low->handle_pool_type_set, name) != NULL;
}

void
lower_register_handle_pool_type(Lowerer* low, String8 name) {
  lower_ensure_name_tables(low);
  hashtable_insert(ctx_perm(), &low->handle_pool_type_set, name, (void*)1, false);
}

static TypeAlias*
find_alias(Lowerer* low, String8 name) {
  lower_ensure_name_tables(low);
  return (TypeAlias*)hashtable_lookup(&low->alias_by_name, name);
}

// Disambiguates `push`'s second argument: `(push arena Asset)` allocates one
// of that type, `(push arena some-asset)` copies that value. Both are bare
// atoms, so the distinction can only come from a name lookup -- every other
// type position in the grammar is fixed by where it sits in the form.
static b32
is_known_type_atom(Lowerer* low, String8 name) {
  if (name.size > 0 && name.str[name.size - 1] == '^') { // handles: single-level only, no '*' peeling
    return is_known_handle_pool_type(low, str8_chop(name, 1));
  }
  String8 base = name;
  while (base.size > 0 && base.str[base.size - 1] == '*') base = str8_chop(base, 1);
  if (base.size == 0) return false;
  return is_primitive_type_name(base) || is_known_struct_name(low, base) || is_known_enum_name(low, base)
      || find_alias(low, base) != NULL;
}

// Decides whether a `let` binding's would-be type slot (`flat[i+1]`) holds a
// type, or is the initializer of a 2-slot `name init` entry whose type the
// checker infers. `let` is the only binding form that may omit its type.
//
// Accepts exactly the shapes lower_type_node accepts, so a real annotation is
// never misread as an initializer. Anything else -- a call, a construction, a
// literal, a bare value reference -- could not have been a type.
static b32
let_slot_looks_like_type(Lowerer* low, NodeIndex idx) {
  AstNode* node = ast_get(low->ast, idx);
  if (node->kind == AstNodeKind_Atom)   return is_known_type_atom(low, node->token.text);
  if (node->kind == AstNodeKind_Vector) return true; // `[ElementType Count]`; array literals can never
                                                       // self-type, so claiming every Vector costs nothing
  if (node->kind == AstNodeKind_List) {
    u16        lc;
    NodeIndex* lchildren = ast_seq_children(low->ast, idx, &lc);
    AstNode*   lhead     = (lc > 0) ? ast_get(low->ast, lchildren[0]) : NULL;
    return lhead && lhead->kind == AstNodeKind_Atom
        && (str8_match_lit("member-type", lhead->token.text, 0) || str8_match_lit("fn", lhead->token.text, 0));
  }
  return false;
}

// lower_type_node resolves any node in type position and reports its own
// errors, so call sites need no kind check of their own. The helpers below
// are all mutually recursive with it -- each resolves one compound type
// shape, whose components are themselves type nodes.
static TypeRef lower_member_type(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count);
static TypeRef lower_anon_struct_type(Lowerer* low, NodeIndex idx, NodeIndex map_idx, b32 is_union);
static TypeRef lower_fn_type(Lowerer* low, NodeIndex type_idx, NodeIndex* lchildren, u16 lc);
static TypeRef lower_hashtable_type(Lowerer* low, Token token, TypeRef key_ty, TypeRef* value_ty, b32 is_set);

static TypeRef
lower_type_node(Lowerer* low, NodeIndex type_idx) {
  AstNode* node = ast_get(low->ast, type_idx);
  if (node->kind == AstNodeKind_Atom) {
    TypeRef result;
    // `T^` handles are single-level and compose with neither aliases nor
    // pointers, so they resolve before any of that machinery runs.
    if (node->token.text.size > 0 && node->token.text.str[node->token.text.size - 1] == '^') {
      String8 handle_base = str8_chop(node->token.text, 1);
      if (!is_known_handle_pool_type(low, handle_base)) {
        lower_error(low, node->token, "`%.*s^` -- no `(handle %.*s)` declared",
                    str8_varg(handle_base), str8_varg(handle_base));
        return type_ref_unresolved();
      }
      TypeRef t = {0};
      t.kind    = TypeKind_Handle;
      t.name    = handle_base;
      result    = t;
    } else {
      // Peeling trailing '*'s before the alias lookup, then re-wrapping, is
      // what makes aliases compose with pointer syntax: `newi32*` becomes a
      // pointer to the alias's underlying type.
      String8 base       = node->token.text;
      u32     star_count = 0;
      while (base.size > 0 && base.str[base.size - 1] == '*') {
        base = str8_chop(base, 1);
        star_count += 1;
      }
      TypeAlias* alias = find_alias(low, base);
      if (alias) {
        TypeRef aliased    = alias->type;
        aliased.alias_name = alias->name; // codegen prints "newi32", not "i32". Set on the innermost
                                           // type, before the pointer wrapping below, so
                                           // c_type_from_typeref's recursive Pointer case still
                                           // reaches it for `newi32*`.
        foreach_index(i, star_count) {
          TypeRef* boxed = push_one(ctx_perm(), TypeRef);
          *boxed         = aliased;
          TypeRef ptr    = {0};
          ptr.kind       = TypeKind_Pointer;
          ptr.pointee    = boxed;
          aliased        = ptr;
        }
        result = aliased;
      } else {
        result = type_ref_from_atom(ctx_perm(), node->token.text);
      }
    }
    // Every atom in type position gets an annotation, primitives included,
    // for LSP goto-definition and hover. See TypeAnnotation in 3b.h.
    TypeAnnotation ann = {0};
    ann.token = node->token;
    ann.type  = result;
    dyn_push(low->tast->arena, low->tast->type_annotations, ann);
    return result;
  }
  if (node->kind == AstNodeKind_Vector) {
    u16        flat_count;
    NodeIndex* flat = ast_seq_children(low->ast, type_idx, &flat_count);
    if (flat_count == 1) {
      // `[ElementType]` -- a fixed array's bracket without the count, i.e.
      // shorthand for `(Vector ElementType)`.
      TypeRef  elem  = lower_type_node(low, flat[0]);
      TypeRef* boxed = push_one(ctx_perm(), TypeRef);
      *boxed         = elem;
      TypeRef t = {0};
      t.kind    = TypeKind_Vector;
      t.pointee = boxed;
      return t;
    }
    if (flat_count != 2) {
      lower_error(low, node->token, "array type must be `[ElementType Count]`, or `[ElementType]` for a growable Vector");
      return type_ref_unresolved();
    }
    AstNode*        count_node = ast_get(low->ast, flat[1]);
    NumericAtomInfo count_num  = count_node->kind == AstNodeKind_Atom
                               ? atom_classify_numeric(count_node->token.text)
                               : (NumericAtomInfo){0};
    if (!count_num.is_numeric || count_num.is_float) {
      lower_error(low, count_node->token, "array count must be a non-negative integer literal");
      return type_ref_unresolved();
    }
    i64 count_val = 0;
    if (!atom_parse_i64(count_num.body, count_num.is_hex, &count_val)) {
      lower_error(low, count_node->token, "array count `%.*s` doesn't fit in 64 bits",
                   str8_varg(count_node->token.text));
      return type_ref_unresolved();
    }
    if (count_val <= 0) {
      lower_error(low, count_node->token, "array count must be a positive integer");
      return type_ref_unresolved();
    }
    TypeRef  elem  = lower_type_node(low, flat[0]); // recurses -- handles nested arrays, e.g. `[[i32 3] 4]`
    TypeRef* boxed = push_one(ctx_perm(), TypeRef);
    *boxed         = elem;
    TypeRef t = {0};
    t.kind    = TypeKind_Array;
    t.pointee = boxed;
    t.count   = (u64)count_val;
    return t;
  }
  if (node->kind == AstNodeKind_Map) {
    // Three shorthands, told apart by arity and whether the first slot is a
    // `:field` key:
    //   `{T}`               -- one bare type              -> `(Set T)`
    //   `{K V}`             -- two bare types             -> `(Map K V)`
    //   `{:field type ...}` -- even count, `:field` keys  -> `(struct {...})`
    u16        flat_count;
    NodeIndex* flat = ast_seq_children(low->ast, type_idx, &flat_count);
    if (flat_count == 0) {
      lower_error(low, node->token,
        "empty `{}` isn't a valid type -- `{T}` for a Set, `{K V}` for a Map, or `{:field type ...}` for an anonymous struct");
      return type_ref_unresolved();
    }
    AstNode* first_slot = ast_get(low->ast, flat[0]);
    b32 first_is_field_key = first_slot->kind == AstNodeKind_Atom && first_slot->token.text.size > 0
                           && first_slot->token.text.str[0] == ':';
    if (!first_is_field_key && flat_count == 1) {
      TypeRef elem_ty = lower_type_node(low, flat[0]);
      return lower_hashtable_type(low, node->token, elem_ty, NULL, true);
    }
    if (!first_is_field_key && flat_count == 2) {
      TypeRef key_ty   = lower_type_node(low, flat[0]);
      TypeRef value_ty = lower_type_node(low, flat[1]);
      return lower_hashtable_type(low, node->token, key_ty, &value_ty, false);
    }
    if (first_is_field_key && flat_count % 2 == 0) {
      return lower_anon_struct_type(low, type_idx, type_idx, false);
    }
    lower_error(low, node->token,
      "`{}` in a type position must be `{T}` for a Set, `{K V}` for a Map, or `{:field type ...}` for an anonymous struct");
    return type_ref_unresolved();
  }
  if (node->kind == AstNodeKind_List) {
    u16        lc;
    NodeIndex* lchildren = ast_seq_children(low->ast, type_idx, &lc);
    AstNode*   lhead     = (lc > 0) ? ast_get(low->ast, lchildren[0]) : NULL;
    if (lhead && lhead->kind == AstNodeKind_Atom && str8_match_lit("member-type", lhead->token.text, 0)) {
      return lower_member_type(low, type_idx, lchildren, lc);
    }
    if (lhead && lhead->kind == AstNodeKind_Atom && str8_match_lit("fn", lhead->token.text, 0)) {
      return lower_fn_type(low, type_idx, lchildren, lc);
    }
    // `(Vector ElementType)` -- a growable array, and one of the closed set
    // of parametric type-formers (see TypeKind_Vector in 3b.h). Capitalized
    // like a named type, unlike the lowercase keyword forms around it.
    if (lhead && lhead->kind == AstNodeKind_Atom && str8_match_lit("Vector", lhead->token.text, 0)) {
      if (lc != 2) {
        lower_error(low, node->token, "`Vector` takes exactly one type argument, e.g. `(Vector i32)`");
        return type_ref_unresolved();
      }
      TypeRef  elem  = lower_type_node(low, lchildren[1]);
      TypeRef* boxed = push_one(ctx_perm(), TypeRef);
      *boxed         = elem;
      TypeRef t = {0};
      t.kind    = TypeKind_Vector;
      t.pointee = boxed;
      return t;
    }
    // `(Map K V)` / `(Set T)` -- monomorphized hash tables, same closed-set
    // status as Vector above. See TypeKind_Map/TypeKind_Set in 3b.h.
    if (lhead && lhead->kind == AstNodeKind_Atom && str8_match_lit("Map", lhead->token.text, 0)) {
      if (lc != 3) {
        lower_error(low, node->token, "`Map` takes exactly two type arguments, e.g. `(Map string i32)`");
        return type_ref_unresolved();
      }
      TypeRef key_ty   = lower_type_node(low, lchildren[1]);
      TypeRef value_ty = lower_type_node(low, lchildren[2]);
      return lower_hashtable_type(low, node->token, key_ty, &value_ty, false);
    }
    if (lhead && lhead->kind == AstNodeKind_Atom && str8_match_lit("Set", lhead->token.text, 0)) {
      if (lc != 2) {
        lower_error(low, node->token, "`Set` takes exactly one type argument, e.g. `(Set i32)`");
        return type_ref_unresolved();
      }
      TypeRef key_ty = lower_type_node(low, lchildren[1]);
      return lower_hashtable_type(low, node->token, key_ty, NULL, true);
    }
    b32 anon_struct_kw = lhead && lhead->kind == AstNodeKind_Atom && str8_match_lit("struct", lhead->token.text, 0);
    b32 anon_union_kw  = lhead && lhead->kind == AstNodeKind_Atom && str8_match_lit("union", lhead->token.text, 0);
    if ((anon_struct_kw || anon_union_kw) && lc == 2
        && (ast_get(low->ast, lchildren[1])->kind == AstNodeKind_Map
            || ast_get(low->ast, lchildren[1])->kind == AstNodeKind_Vector)) {
      return lower_anon_struct_type(low, type_idx, lchildren[1], anon_union_kw);
    }
    if (lhead && lhead->kind == AstNodeKind_Atom && str8_match_lit("const", lhead->token.text, 0)) {
      if (lc != 2) {
        lower_error(low, node->token, "`const` takes exactly one type, e.g. `(const u8)` or `(const u8*)` -- keep the `*` as part of the atom, same as any other pointer type");
        return type_ref_unresolved();
      }
      TypeRef t = lower_type_node(low, lchildren[1]);
      // const binds to the base type, as C reads `const u8 *p`: a mutable
      // pointer to a const u8. Marking the innermost pointee rather than `t`
      // is what makes `(const u8*)` come out that way once c_type_from_typeref
      // recurses down to it. C's rarer `u8* const` has no spelling here.
      TypeRef* base = &t;
      while (base->kind == TypeKind_Pointer && base->pointee) base = base->pointee;
      base->is_const = true;
      return t;
    }
  }
  lower_error(low, node->token,
    "expected a type: an atom, `[ElementType Count]` for an array, `(Vector ElementType)`/`[ElementType]` "
    "for a growable Vector, `(Map KeyType ValueType)`/`{KeyType ValueType}`, `(Set ElementType)`/`{ElementType}`, "
    "`(member-type StructName field)`, "
    "`(struct {:field type ...})`/`(struct [field type ...])`/bare `{:field type ...}` (or `union`) for an "
    "anonymous struct/union, `(const T)`, or `(fn [name type ...] ReturnType)` for a function pointer");
  return type_ref_unresolved();
}

// `(fn [name type ...] ReturnType)` in type position: a function POINTER
// type, unlike `fn`'s top-level meaning of a named declaration with a body
// (lower_fn). The parameter vector uses the same flat name/type pairs a real
// declaration does, so `(fn [str string] void)` reads like the signature
// portion of `(fn some-name [str string] void ...)`.
//
// Param names are required but never stored: a function type's identity is
// its param types and return type only (type_ref_equal).
static TypeRef
lower_fn_type(Lowerer* low, NodeIndex type_idx, NodeIndex* lchildren, u16 lc) {
  AstNode* node = ast_get(low->ast, type_idx);
  if (lc != 3) {
    lower_error(low, node->token, "function type must be `(fn [name type ...] ReturnType)`");
    return type_ref_unresolved();
  }
  AstNode* params_node = ast_get(low->ast, lchildren[1]);
  if (params_node->kind != AstNodeKind_Vector) {
    lower_error(low, params_node->token, "function type parameters must be a vector, e.g. `(fn [a i32] void)`");
    return type_ref_unresolved();
  }
  u16        flat_count;
  NodeIndex* flat = ast_seq_children(low->ast, lchildren[1], &flat_count);
  if (flat_count % 2 != 0) {
    lower_error(low, params_node->token,
      "function type parameter vector must contain name/type pairs (even number of entries)");
    return type_ref_unresolved();
  }
  ArenaTemp scratch         = arena_temp_begin(ctx_scratch());
  TypeRef*  scratch_params  = NULL;
  for (u32 i = 0; i + 1 < flat_count; i += 2) {
    AstNode* pname = ast_get(low->ast, flat[i]);
    if (pname->kind != AstNodeKind_Atom) {
      lower_error(low, params_node->token, "function type parameter name must be an atom");
      continue;
    }
    TypeRef ptype = lower_type_node(low, flat[i + 1]);
    dyn_push(scratch.arena, scratch_params, ptype);
  }
  u32     param_count = (u32)dyn_count(scratch_params);
  TypeRef* params      = NULL;
  if (param_count > 0) {
    params = push_array(ctx_perm(), TypeRef, param_count);
    MemoryCopyTyped(params, scratch_params, param_count);
  }
  arena_temp_end(&scratch);

  TypeRef return_type = lower_type_node(low, lchildren[2]);
  if (return_type.kind == TypeKind_Array) {
    lower_error(low, node->token, "function type cannot return an array type directly (C doesn't support that) -- return a pointer instead");
    return type_ref_unresolved();
  }
  if (return_type.kind == TypeKind_Fn) {
    lower_error(low, node->token, "function type cannot return another function type directly -- return a pointer to one instead");
    return type_ref_unresolved();
  }
  TypeRef* boxed_return = push_one(ctx_perm(), TypeRef);
  *boxed_return          = return_type;

  TypeRef t = {0};
  t.kind           = TypeKind_Fn;
  t.fn_params      = params;
  t.fn_param_count = param_count;
  t.fn_return      = boxed_return;
  return t;
}

// An anonymous `(struct {:x i32 :y i32})` or `(union {...})` used inline in
// type position -- a field's type, a parameter or return type, an alias target
// -- rather than declared at top level, as in
// `(struct Outer [pos (struct {:x i32 :y i32}) other i32])`. The
// `[x i32 y i32]` vector shape of a named declaration is accepted equivalently.
//
// There is no anonymous-struct TypedNodeKind. This synthesizes an ordinary
// StructDecl named `Anon0`, `Anon1` and so on, exactly as if it had been
// written at top level, and returns a Named TypeRef pointing at it. Each
// field's type goes through lower_type_node, so nesting works for free.
//
// The synthesized declaration cannot be appended to `forms` here, since this
// runs arbitrarily deep inside whatever top-level form is being lowered. It
// goes on low->pending_toplevel, which lower_program drains right afterward,
// splicing each entry in just before the form that triggered it. That order
// matters: codegen emits structs in source order with no topological sort, so
// a struct embedded by value must be declared ahead of its embedder.
static TypeRef
lower_anon_struct_type(Lowerer* low, NodeIndex idx, NodeIndex fields_idx, b32 is_union) {
  AstNode*   node     = ast_get(low->ast, idx);
  const char* kw      = is_union ? "union" : "struct";
  b32        is_map   = ast_get(low->ast, fields_idx)->kind == AstNodeKind_Map;
  u16        flat_count;
  NodeIndex* flat = ast_seq_children(low->ast, fields_idx, &flat_count);
  if (flat_count == 0 || flat_count % 2 != 0) {
    lower_error(low, node->token, "anonymous `%s` fields must be name/type pairs (even number of entries)", kw);
    return type_ref_unresolved();
  }
  ArenaTemp scratch = arena_temp_begin(ctx_scratch());
  Param*    scratch_fields = NULL;
  for (u32 i = 0; i + 1 < flat_count; i += 2) {
    AstNode* fname = ast_get(low->ast, flat[i]);
    String8  name;
    if (is_map) {
      if (fname->kind != AstNodeKind_Atom || fname->token.text.size == 0 || fname->token.text.str[0] != ':') {
        lower_error(low, node->token, "anonymous `%s` field names must be `:field`, e.g. `{:x i32 :y i32}`", kw);
        continue;
      }
      name = str8_skip(fname->token.text, 1); // drop the leading ':'
    } else {
      if (fname->kind != AstNodeKind_Atom) {
        lower_error(low, node->token, "anonymous `%s` field name must be an atom", kw);
        continue;
      }
      name = fname->token.text;
    }
    Param field = {0};
    field.name       = name;
    field.name_token = fname->token;
    field.is_anon = str8_match_lit("_", field.name, 0); // C11 anonymous member -- see Param.is_anon
    field.type    = lower_type_node(low, flat[i + 1]); // may push a nested struct's own fields, which
                                                        // must not interleave with this one's --
                                                        // hence the scratch buffer
    dyn_push(scratch.arena, scratch_fields, field);
  }
  u32 field_first = (u32)dyn_count(low->tast->params); // only valid once every type is resolved
  foreach_index(i, dyn_count(scratch_fields)) {
    dyn_push(low->tast->arena, low->tast->params, scratch_fields[i]);
  }
  u16 real_field_count = (u16)dyn_count(scratch_fields);
  arena_temp_end(&scratch);

  String8 synthesized_name = str8f(ctx_perm(), "Anon%u", low->anon_type_counter);
  low->anon_type_counter += 1;
  // Registering the name is what makes `(Anon0 1 2)` lower as a struct
  // literal rather than a call. The NODE_NIL form keeps struct_decl_forms the
  // same length as struct_names, which lower_member_type indexes in lockstep;
  // it guards against the nil rather than dereferencing it.
  lower_register_struct_name(low, synthesized_name, NODE_NIL);

  TypedNode n = {0};
  n.kind                    = TypedNodeKind_StructDecl;
  n.token                   = node->token;
  n.struct_decl.name        = synthesized_name;
  n.struct_decl.field_first = field_first;
  n.struct_decl.field_count = real_field_count;
  n.struct_decl.is_union    = is_union;
  n.struct_decl.is_synthesized = true;
  TypedIndex synthesized = typed_push(low->tast, n);
  dyn_push(low->tast->arena, low->pending_toplevel, synthesized);

  TypeRef t = {0};
  t.kind    = TypeKind_Named;
  t.name    = synthesized_name;
  return t;
}

// Boxed value stored in Lowerer.anon_struct_by_shape; read back only through
// lower_anon_param_struct below.
typedef struct AnonStructIntern {
  String8 name;
  u32     field_first;
  u16     field_count;
} AnonStructIntern;

// Canonical structural signature for a field list, used as the interning key:
// two `{:field type ...}` param shapes collapse to one synthesized struct when
// their field names, types, and order all match. Order counts, since it
// determines C struct layout, so the fields are not sorted.
//
// type_ref_display spells every TypeRef deterministically and
// self-delimitingly, so joining `name:type;` per field is unambiguous.
static String8
anon_param_field_signature(Param* fields, u16 count) {
  String8 sig = str8_lit("");
  foreach_index(i, count) {
    String8 ty = type_ref_display(ctx_scratch(), fields[i].type);
    sig = str8f(ctx_perm(), "%.*s%.*s:%.*s;", str8_varg(sig), str8_varg(fields[i].name), str8_varg(ty));
  }
  return sig;
}

// `{:field type ...}` used directly as a `fn` parameter. Synthesizes a real
// named struct type -- or reuses one, if an identically-shaped param appeared
// anywhere earlier in this compilation -- and hands back its field range so
// lower_fn can auto-destructure the fields into locals.
//
// Not lower_anon_struct_type: that one's synthesized names are never interned,
// and param shapes must be.
static TypeRef
lower_anon_param_struct(Lowerer* low, NodeIndex fields_idx, u32* out_field_first, u16* out_field_count) {
  AstNode*   node = ast_get(low->ast, fields_idx);
  u16        flat_count;
  NodeIndex* flat = ast_seq_children(low->ast, fields_idx, &flat_count);
  if (flat_count == 0 || flat_count % 2 != 0) {
    lower_error(low, node->token, "anonymous struct parameter fields must be `:field type` pairs (even number of entries), e.g. `{:name string :health i32}`");
    *out_field_first = 0;
    *out_field_count = 0;
    return type_ref_unresolved();
  }
  ArenaTemp scratch        = arena_temp_begin(ctx_scratch());
  Param*    scratch_fields = NULL;
  for (u32 i = 0; i + 1 < flat_count; i += 2) {
    AstNode* fname = ast_get(low->ast, flat[i]);
    if (fname->kind != AstNodeKind_Atom || fname->token.text.size == 0 || fname->token.text.str[0] != ':') {
      lower_error(low, node->token, "anonymous struct parameter field names must be `:field`, e.g. `{:name string :health i32}`");
      continue;
    }
    Param field      = {0};
    field.name       = str8_skip(fname->token.text, 1); // drop the leading ':'
    field.name_token = fname->token;
    field.type  = lower_type_node(low, flat[i + 1]); // may synthesize nested anonymous struct
                                                      // types, which must not interleave with
                                                      // this field list -- hence the scratch
    dyn_push(scratch.arena, scratch_fields, field);
  }
  u16 real_field_count = (u16)dyn_count(scratch_fields);

  lower_ensure_name_tables(low);
  String8 sig = anon_param_field_signature(scratch_fields, real_field_count);
  AnonStructIntern* existing = (AnonStructIntern*)hashtable_lookup(&low->anon_struct_by_shape, sig);
  if (existing != NULL) {
    arena_temp_end(&scratch); // identical shape already synthesized -- reuse it, discard this parse
    *out_field_first = existing->field_first;
    *out_field_count = existing->field_count;
    TypeRef t = {0};
    t.kind    = TypeKind_Named;
    t.name    = existing->name;
    return t;
  }

  u32 field_first = (u32)dyn_count(low->tast->params); // only valid once every type is resolved
  foreach_index(i, real_field_count) {
    dyn_push(low->tast->arena, low->tast->params, scratch_fields[i]);
  }
  arena_temp_end(&scratch);

  String8 synthesized_name = str8f(ctx_perm(), "AnonParam%u", low->anon_type_counter);
  low->anon_type_counter += 1;
  lower_register_struct_name(low, synthesized_name, NODE_NIL);

  TypedNode n = {0};
  n.kind                    = TypedNodeKind_StructDecl;
  n.token                   = node->token;
  n.struct_decl.name        = synthesized_name;
  n.struct_decl.field_first = field_first;
  n.struct_decl.field_count = real_field_count;
  n.struct_decl.is_union    = false;
  n.struct_decl.is_synthesized = true;
  TypedIndex synthesized = typed_push(low->tast, n);
  dyn_push(low->tast->arena, low->pending_toplevel, synthesized);

  AnonStructIntern* boxed = push_one(ctx_perm(), AnonStructIntern);
  boxed->name             = synthesized_name;
  boxed->field_first      = field_first;
  boxed->field_count      = real_field_count;
  hashtable_insert(ctx_perm(), &low->anon_struct_by_shape, sig, boxed, false);

  *out_field_first = field_first;
  *out_field_count = real_field_count;
  TypeRef t = {0};
  t.kind    = TypeKind_Named;
  t.name    = synthesized_name;
  return t;
}

// Synthesizes a struct with fields `_0`, `_1`, ... from already-resolved
// TypeRefs, and registers its name in multi_return_struct_set so lower_fn can
// tell multi-return sugar apart from a plain named struct return (lower_return
// is the other half of that).
//
// Interning goes through the same anon_struct_by_shape table
// lower_anon_param_struct uses, so a shape minted here and one minted there
// are the same struct. That is why the builtins below register their result
// types too, even though a `string-to-i32` call is never a fn's return type:
// a user fn declared with the same `(bool T)` shape must share this struct
// rather than mint a second identical one.
static TypeRef
lower_positional_struct_from_types(Lowerer* low, Token token, TypeRef* types, u16 count) {
  ArenaTemp scratch        = arena_temp_begin(ctx_scratch());
  Param*    scratch_fields = NULL;
  for (u32 i = 0; i < count; i += 1) {
    Param field = {0};
    field.name  = str8f(ctx_perm(), "_%u", i);
    field.type  = types[i];
    dyn_push(scratch.arena, scratch_fields, field);
  }
  u16 real_field_count = (u16)dyn_count(scratch_fields);

  lower_ensure_name_tables(low);
  String8 sig = anon_param_field_signature(scratch_fields, real_field_count);
  AnonStructIntern* existing = (AnonStructIntern*)hashtable_lookup(&low->anon_struct_by_shape, sig);
  if (existing != NULL) {
    arena_temp_end(&scratch); // identical shape already synthesized -- reuse it, discard this parse
    hashtable_insert(ctx_perm(), &low->multi_return_struct_set, existing->name, (void*)1, false);
    TypeRef t = {0};
    t.kind    = TypeKind_Named;
    t.name    = existing->name;
    return t;
  }

  u32 field_first = (u32)dyn_count(low->tast->params);
  foreach_index(i, real_field_count) {
    dyn_push(low->tast->arena, low->tast->params, scratch_fields[i]);
  }
  arena_temp_end(&scratch);

  String8 synthesized_name = str8f(ctx_perm(), "AnonReturn%u", low->anon_type_counter);
  low->anon_type_counter += 1;
  lower_register_struct_name(low, synthesized_name, NODE_NIL);

  TypedNode n = {0};
  n.kind                    = TypedNodeKind_StructDecl;
  n.token                   = token;
  n.struct_decl.name        = synthesized_name;
  n.struct_decl.field_first = field_first;
  n.struct_decl.field_count = real_field_count;
  n.struct_decl.is_union    = false;
  n.struct_decl.is_synthesized = true;
  TypedIndex synthesized = typed_push(low->tast, n);
  dyn_push(low->tast->arena, low->pending_toplevel, synthesized);

  AnonStructIntern* boxed = push_one(ctx_perm(), AnonStructIntern);
  boxed->name             = synthesized_name;
  boxed->field_first      = field_first;
  boxed->field_count      = real_field_count;
  hashtable_insert(ctx_perm(), &low->anon_struct_by_shape, sig, boxed, false);
  hashtable_insert(ctx_perm(), &low->multi_return_struct_set, synthesized_name, (void*)1, false);

  TypeRef t = {0};
  t.kind    = TypeKind_Named;
  t.name    = synthesized_name;
  return t;
}

// `(T0 T1 ...)` in a `fn` return-type position -- Odin-style multiple return
// values. lower_return_type_node, the only caller, explains why this is
// confined to that position.
static TypeRef
lower_multi_return_type(Lowerer* low, NodeIndex idx, NodeIndex* lchildren, u16 lc) {
  AstNode* node = ast_get(low->ast, idx);

  ArenaTemp scratch = arena_temp_begin(ctx_scratch());
  TypeRef*  types   = push_array(scratch.arena, TypeRef, lc);
  for (u32 i = 0; i < lc; i += 1) {
    types[i] = lower_type_node(low, lchildren[i]);
  }
  TypeRef result = lower_positional_struct_from_types(low, node->token, types, lc);
  arena_temp_end(&scratch);
  return result;
}

// `(string-to-i32 s)` and its i64/u32/u64/f32/f64 siblings, returning
// `(bool T)`; TypedNodeKind_ParseNumber documents the failure contract.
//
// The result type is synthesized here rather than left to the checker because
// only lowering can splice a new StructDecl into the program: pending_toplevel
// is fully drained before checking starts.
static TypedIndex
lower_parse_number(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count, TypeKind target_kind, const char* builtin_name) {
  AstNode* node = ast_get(low->ast, idx);
  if (count != 2) {
    lower_error(low, node->token, "`%s` takes exactly one argument, a string to parse", builtin_name);
    return TYPED_NIL;
  }
  TypedIndex arg_idx = lower_expr(low, children[1]);

  TypeRef bool_ty   = {0};
  bool_ty.kind      = TypeKind_Bool;
  TypeRef target_ty = {0};
  target_ty.kind    = target_kind;
  TypeRef types[2]  = { bool_ty, target_ty };
  TypeRef result_ty = lower_positional_struct_from_types(low, node->token, types, 2);

  TypedNode n = {0};
  n.kind                             = TypedNodeKind_ParseNumber;
  n.token                            = node->token;
  n.parse_number.arg                 = arg_idx;
  n.parse_number.target_kind         = target_kind;
  n.parse_number.result_struct_name  = result_ty.name;
  return typed_push(low->tast, n);
}

// `(vector-index-of v x)` -- linear search over a Vector, returning
// `(bool u64)`. Named by the `<container>-<verb>` convention that
// `map-contains?`/`set-contains?` follow, since unlike `len`/`nth` it applies
// to only one container type.
static TypedIndex
lower_index_of(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count) {
  AstNode* node = ast_get(low->ast, idx);
  if (count != 3) {
    lower_error(low, node->token, "`vector-index-of` requires exactly a Vector and a value to search for");
    return TYPED_NIL;
  }
  TypedIndex vec_idx    = lower_expr(low, children[1]);
  TypedIndex needle_idx = lower_expr(low, children[2]);

  TypeRef bool_ty = {0};
  bool_ty.kind    = TypeKind_Bool;
  TypeRef u64_ty  = {0};
  u64_ty.kind     = TypeKind_U64;
  TypeRef types[2]  = { bool_ty, u64_ty };
  TypeRef result_ty = lower_positional_struct_from_types(low, node->token, types, 2);

  TypedNode n = {0};
  n.kind                        = TypedNodeKind_IndexOf;
  n.token                       = node->token;
  n.index_of.vec                = vec_idx;
  n.index_of.needle             = needle_idx;
  n.index_of.result_struct_name  = result_ty.name;
  return typed_push(low->tast, n);
}

// `(sqrt-checked x)`, `asin-checked`, `acos-checked`, `(pow-checked base exp)`
// -- domain-checked libm, returning `(bool T)`. Unlike lower_parse_number,
// whose target type is fixed by the builtin's name, `T` here is f32 or f64
// depending on the argument, which lowering cannot know. Both shapes are
// interned up front; the checker picks one once it knows the argument type.
static TypedIndex
lower_checked_math(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count, String8 libm_name, b32 two_arg) {
  AstNode* node    = ast_get(low->ast, idx);
  u16      arg_count = two_arg ? 2 : 1;
  if (count != arg_count + 1) {
    lower_error(low, node->token, "`%.*s-checked` takes exactly %u argument(s), got %u",
                str8_varg(libm_name), (u32)arg_count, (u32)(count - 1));
    return TYPED_NIL;
  }
  TypedIndex arg_idx  = lower_expr(low, children[1]);
  TypedIndex arg2_idx = two_arg ? lower_expr(low, children[2]) : TYPED_NIL;

  TypeRef bool_ty = {0}; bool_ty.kind = TypeKind_Bool;
  TypeRef f32_ty  = {0}; f32_ty.kind  = TypeKind_F32;
  TypeRef f64_ty  = {0}; f64_ty.kind  = TypeKind_F64;
  TypeRef f32_types[2] = { bool_ty, f32_ty };
  TypeRef f64_types[2] = { bool_ty, f64_ty };
  TypeRef f32_result = lower_positional_struct_from_types(low, node->token, f32_types, 2);
  TypeRef f64_result = lower_positional_struct_from_types(low, node->token, f64_types, 2);

  TypedNode n = {0};
  n.kind                          = TypedNodeKind_CheckedMath;
  n.token                         = node->token;
  n.checked_math.libm_name        = libm_name;
  n.checked_math.arg              = arg_idx;
  n.checked_math.arg2             = arg2_idx;
  n.checked_math.f32_struct_name  = f32_result.name;
  n.checked_math.f64_struct_name  = f64_result.name;
  return typed_push(low->tast, n);
}

// Type entry point for a real `fn`'s return slot, used only by
// lower_fn/lower_extern_fn, and the only place `(T0 T1 ...)` reads as
// multi-return sugar.
//
// Keeping this out of lower_type_node matters: everywhere else a stray
// parenthesized list like `(Vector2 1.0 2.0)` is a value written in a type
// slot by mistake and must keep producing "expected a type", not silently
// become a 2-field tuple. Function-pointer return types are excluded for the
// same reason `return`'s multi-value sugar needs a real body's
// current_fn_multi_return_name -- there is no body to build a literal against.
static TypeRef
lower_return_type_node(Lowerer* low, NodeIndex type_idx) {
  AstNode* node = ast_get(low->ast, type_idx);
  if (node->kind == AstNodeKind_List) {
    u16        lc;
    NodeIndex* lchildren = ast_seq_children(low->ast, type_idx, &lc);
    AstNode*   lhead     = (lc > 0) ? ast_get(low->ast, lchildren[0]) : NULL;
    b32 lhead_is_reserved_type_kw = lhead && lhead->kind == AstNodeKind_Atom
      && (str8_match_lit("member-type", lhead->token.text, 0)
       || str8_match_lit("fn", lhead->token.text, 0)
       || str8_match_lit("Vector", lhead->token.text, 0)
       || str8_match_lit("Map", lhead->token.text, 0)
       || str8_match_lit("Set", lhead->token.text, 0)
       || str8_match_lit("struct", lhead->token.text, 0)
       || str8_match_lit("union", lhead->token.text, 0)
       || str8_match_lit("const", lhead->token.text, 0));
    if (!lhead_is_reserved_type_kw && lc >= 2) {
      return lower_multi_return_type(low, type_idx, lchildren, lc);
    }
  }
  return lower_type_node(low, type_idx);
}

// Shared by `(Map K V)` and `(Set T)`; `value_ty` is NULL for a Set. Interning
// by structural signature gives every identical instantiation in a package one
// monomorphized hash table rather than one per use site.
//
// Same shape as lower_anon_param_struct's interning, minus the boxed values:
// the synthesized name is recomputable from the type refs alone
// (hashtable_mangled_name), so the table need only record membership.
static TypeRef
lower_hashtable_type(Lowerer* low, Token token, TypeRef key_ty, TypeRef* value_ty, b32 is_set) {
  // The runtime can hash and compare only integers, bool/char, and `string`;
  // see runtime/bbb_hashtable.h. Everything else is rejected here rather than
  // silently mishandled.
  b32 key_ok = key_ty.kind == TypeKind_I8   || key_ty.kind == TypeKind_I16  || key_ty.kind == TypeKind_I32
            || key_ty.kind == TypeKind_I64  || key_ty.kind == TypeKind_U8   || key_ty.kind == TypeKind_U16
            || key_ty.kind == TypeKind_U32  || key_ty.kind == TypeKind_U64
            || key_ty.kind == TypeKind_Bool || key_ty.kind == TypeKind_Char || key_ty.kind == TypeKind_String;
  if (!key_ok) {
    lower_error(low, token, "`%s` keys must be a numeric type, `bool`, `char`, or `string` -- got %.*s (not yet supported)",
                is_set ? "Set" : "Map", str8_varg(type_ref_display(ctx_scratch(), key_ty)));
    return type_ref_unresolved();
  }

  lower_ensure_name_tables(low);
  String8 sig = is_set
    ? str8f(ctx_perm(), "Set:%.*s", str8_varg(type_ref_display(ctx_scratch(), key_ty)))
    : str8f(ctx_perm(), "Map:%.*s,%.*s", str8_varg(type_ref_display(ctx_scratch(), key_ty)), str8_varg(type_ref_display(ctx_scratch(), *value_ty)));

  if (hashtable_lookup(&low->hashtable_instances_emitted, sig) == NULL) {
    String8 mangled_name = hashtable_mangled_name(ctx_perm(), key_ty, is_set ? NULL : value_ty);
    TypedNode n = {0};
    n.kind                             = TypedNodeKind_HashTableInstanceDecl;
    n.token                            = token;
    n.hashtable_instance.mangled_name  = mangled_name;
    n.hashtable_instance.key_type      = key_ty;
    n.hashtable_instance.value_type    = is_set ? type_ref_unresolved() : *value_ty;
    n.hashtable_instance.is_set        = is_set;
    TypedIndex synthesized = typed_push(low->tast, n);
    dyn_push(low->tast->arena, low->pending_toplevel, synthesized);
    hashtable_insert(ctx_perm(), &low->hashtable_instances_emitted, sig, (void*)1, true);
  }

  TypeRef t = {0};
  if (is_set) {
    t.kind    = TypeKind_Set;
    TypeRef* boxed = push_one(ctx_perm(), TypeRef);
    *boxed         = key_ty;
    t.pointee      = boxed;
  } else {
    t.kind = TypeKind_Map;
    TypeRef* boxed_v = push_one(ctx_perm(), TypeRef);
    *boxed_v         = *value_ty;
    t.pointee        = boxed_v;
    TypeRef* boxed_k = push_one(ctx_perm(), TypeRef);
    *boxed_k         = key_ty;
    t.map_key        = boxed_k;
  }
  return t;
}

// `(member-type StructName field)` in type position, resolving to that field's
// type. It re-lowers the field's type node from the raw AST (via
// struct_decl_forms, gathered by lower_program's pre-pass) rather than reading
// lowered TypedAst content, so the referenced struct may be declared later in
// the file -- the same forward reference ordinary struct names already get.
static TypeRef
lower_member_type(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count) {
  AstNode* node = ast_get(low->ast, idx);
  if (count != 3) {
    lower_error(low, node->token, "`member-type` requires exactly a struct name and a field name");
    return type_ref_unresolved();
  }
  AstNode* struct_name_node = ast_get(low->ast, children[1]);
  AstNode* field_name_node  = ast_get(low->ast, children[2]);
  if (struct_name_node->kind != AstNodeKind_Atom || field_name_node->kind != AstNodeKind_Atom) {
    lower_error(low, node->token, "`member-type`'s struct name and field name must both be atoms");
    return type_ref_unresolved();
  }
  String8    struct_name = struct_name_node->token.text;
  String8    field_name  = field_name_node->token.text;
  lower_ensure_name_tables(low);
  NodeIndex* form_box = (NodeIndex*)hashtable_lookup(&low->struct_form_by_name, struct_name);
  if (!form_box) {
    lower_error(low, node->token, "`member-type`: unknown struct type `%.*s`", str8_varg(struct_name));
    return type_ref_unresolved();
  }
  NodeIndex struct_form = *form_box;
  if (struct_form == NODE_NIL) {
    lower_error(low, node->token,
      "`member-type` doesn't support `%.*s` -- it's an anonymous struct/union (from `(struct {:field type "
      "...})`/`(union {...})`), which has no name in source to look it back up by", str8_varg(struct_name));
    return type_ref_unresolved();
  }
  NodeIndex fields_idx = lower_struct_form_fields(low, struct_form);
  if (fields_idx == NODE_NIL) {
    lower_error(low, node->token, "`member-type`: `%.*s`'s own `struct` declaration is malformed",
                str8_varg(struct_name));
    return type_ref_unresolved();
  }
  u16        flat_count;
  NodeIndex* flat = ast_seq_children(low->ast, fields_idx, &flat_count);
  for (u32 i = 0; i + 1 < flat_count; i += 2) {
    AstNode* fname = ast_get(low->ast, flat[i]);
    if (fname->kind == AstNodeKind_Atom && str8_match(fname->token.text, field_name, 0)) {
      return lower_type_node(low, flat[i + 1]);
    }
  }
  lower_error(low, node->token, "`member-type`: struct `%.*s` has no field `%.*s`",
              str8_varg(struct_name), str8_varg(field_name));
  return type_ref_unresolved();
}

// `/` is not a lexer delimiter, so `WindowFlags/Borderless` arrives as a
// single atom. Splitting at the last '/' rather than the first keeps the leaf
// as the rightmost segment however deep a path grows.
static b32
str8_split_at_last_slash(String8 s, String8* out_prefix, String8* out_suffix) {
  for (i64 i = (i64)s.size - 1; i >= 0; i -= 1) {
    if (s.str[i] == '/') {
      *out_prefix = str8_prefix(s, (u64)i);
      *out_suffix = str8_skip(s, (u64)i + 1);
      return true;
    }
  }
  return false;
}

void
lower_error(Lowerer* low, Token tok, const char* fmt, ...) {
  low->had_error = true;
  va_list args;
  va_start(args, fmt);
  diag_errorv(tok, fmt, args);
  va_end(args);
}

TypedIndex
lower_atom(Lowerer* low, NodeIndex idx) {
  AstNode* node = ast_get(low->ast, idx);
  String8  text = node->token.text;
  TypedNode n   = {0};
  n.token       = node->token;

  // Scoped constant access, e.g. `WindowFlags/Borderless`; must precede the
  // Identifier fallback. A prefix that names no known enum falls through to an
  // Identifier holding the whole slashed text, so a typo'd path surfaces as
  // the checker's ordinary "undefined identifier" rather than its own error.
  String8 slash_prefix, slash_suffix;
  if (str8_split_at_last_slash(text, &slash_prefix, &slash_suffix)
      && is_known_enum_name(low, slash_prefix)) {
    n.kind                     = TypedNodeKind_EnumAccess;
    n.enum_access.enum_name    = slash_prefix;
    n.enum_access.variant_name = slash_suffix;
    return typed_push(low->tast, n);
  }

  // `nil`, `true` and `false` are reserved literal keywords, not identifiers.
  if (str8_match_lit("nil", text, 0)) {
    n.kind = TypedNodeKind_NilLiteral;
    return typed_push(low->tast, n);
  }

  if (str8_match_lit("true", text, 0) || str8_match_lit("false", text, 0)) {
    n.kind           = TypedNodeKind_BoolLiteral;
    n.bool_lit.value = str8_match_lit("true", text, 0);
    return typed_push(low->tast, n);
  }

  NumericAtomInfo num = atom_classify_numeric(text);
  if (num.is_numeric) {
    const char* body_cstr = cstr_from_str8_temp(num.body); // NOTE: replace with str8_copy
    if (num.is_float) {
      n.kind                     = TypedNodeKind_FloatLiteral;
      n.float_lit.value          = strtod(body_cstr, NULL);
      n.float_lit.explicit_type  = num.explicit_type;
    } else {
      n.kind                    = TypedNodeKind_IntLiteral;
      b32 is_unsigned            = num.explicit_type == TypeKind_U8  || num.explicit_type == TypeKind_U16
                                 || num.explicit_type == TypeKind_U32 || num.explicit_type == TypeKind_U64;
      // A literal too big for 64 bits is caught HERE, not by checker.c's own
      // range check -- that one compares against the target type using the
      // value stored below, which a clamp would have already made a lie
      // ("literal 9223372036854775807 doesn't fit in i32" for a source that
      // says no such thing).
      //
      // Hex reads as a BIT PATTERN whatever the suffix says, so it goes through
      // the unsigned parse too: `0x8000000000000000` names the top bit, and a
      // signed read would reject it as not fitting in 64 bits even though every
      // 64-bit pattern is a valid u64 -- and, once the literal takes a signed
      // type, a valid i64. This is what checker.c's int_literal_fits already
      // assumed by deriving unsigned_bits from is_hex. Decimal keeps the signed
      // read, so `-9223372036854775808` still works and an authored
      // `99999999999999999999` is still an error rather than a wrapped value.
      b32 in_range = true;
      if (is_unsigned || num.is_hex) {
        u64 uv   = 0;
        in_range = atom_parse_u64(num.body, num.is_hex, &uv);
        n.int_lit.value = (i64)uv; // two's complement: `-0xFF` wraps back to -255
      } else {
        in_range = atom_parse_i64(num.body, num.is_hex, &n.int_lit.value);
      }
      if (!in_range) {
        lower_error(low, node->token, "integer literal `%.*s` doesn't fit in 64 bits",
                     str8_varg(text));
      }
      n.int_lit.explicit_type    = num.explicit_type;
      // checker.c's range check needs this; see 3b.h. An authored `-` keeps a
      // negative out even under an unsigned suffix, so `-5u64` still fails.
      b32 authored_minus         = num.body.size > 0 && num.body.str[0] == '-';
      n.int_lit.unsigned_bits    = num.is_hex || (is_unsigned && !authored_minus);
    }
  } else {
    n.kind       = TypedNodeKind_Identifier;
    n.ident.name = text;
  }
  return typed_push(low->tast, n);
}

TypedIndex
lower_string(Lowerer* low, NodeIndex idx) {
  AstNode*  node = ast_get(low->ast, idx);
  TypedNode n    = {0};
  n.kind         = TypedNodeKind_StringLiteral;
  n.token        = node->token;
  n.string_lit.value = node->token.text;
  return typed_push(low->tast, n);
}

// One `{:field type ...}` parameter seen in a `fn`'s param vector. lower_fn's
// param loop collects these in scratch, then wraps the lowered body in LetExprs
// binding each field as a local.
typedef struct AnonParamRef {
  String8 param_name; // the synthesized `__anon_paramN` the real C parameter is named
  u32     field_first; // into low->tast->params -- the interned struct's fields
  u16     field_count;
} AnonParamRef;

TypedIndex
lower_fn(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count) {
  AstNode* node = ast_get(low->ast, idx);
  if (count < 5) {
    lower_error(low, node->token,
      "`fn` requires a name, a `[]` parameter vector, an explicit return type, "
      "and at least one body expression");
    return TYPED_NIL;
  }
  AstNode* name_node = ast_get(low->ast, children[1]);
  if (name_node->kind != AstNodeKind_Atom) {
    lower_error(low, name_node->token, "`fn` name must be an atom");
    return TYPED_NIL;
  }
  String8 fn_name = name_node->token.text;

  AstNode* params_node = ast_get(low->ast, children[2]);
  if (params_node->kind != AstNodeKind_Vector) {
    lower_error(low, params_node->token, "`fn` parameters must be a vector, e.g. `[a i32 b i32]`");
    return TYPED_NIL;
  }
  u16        flat_count;
  NodeIndex* flat = ast_seq_children(low->ast, children[2], &flat_count);

  // A param slot's stride varies: an ordinary `name type` pair takes two flat
  // entries, while a `{:field type ...}` anonymous struct shape is a single
  // Map node and takes one. So there is no valid up-front "must be even"
  // check, as in lower_let's bindings loop.
  //
  // Params are buffered in scratch and flushed at the end, the same
  // discipline the body collection and lower_struct_decl's fields use.
  // Resolving a param's type can recurse into lower_anon_struct_type or
  // lower_anon_param_struct, which push into the SAME tast->params array -- a
  // struct's fields and a function's params are different index ranges in one
  // array. Pushing directly would let such a nested push land between two of
  // this function's own params, inside its first..first+count range, and
  // corrupt param_count.
  //
  // scratch_params and anon_params must share one ArenaTemp, not two.
  // anon_params has to survive down to the body-wrapping code, well past where
  // scratch_params is flushed. Two ArenaTemps opened back-to-back on the same
  // scratch arena with nothing allocated between them mark the SAME position,
  // so ending the shorter-lived one would pop back to it and poison everything
  // allocated after -- anon_params included, since its entries are pushed
  // later in these same iterations.
  ArenaTemp anon_temp       = arena_temp_begin(ctx_scratch());
  AnonParamRef* anon_params = NULL; // scratch -- map-shaped params, auto-destructured into the body below
  Param*    scratch_params  = NULL;
  u32 i = 0;
  while (i < flat_count) {
    AstNode* pname = ast_get(low->ast, flat[i]);
    if (pname->kind == AstNodeKind_Map) {
      // `{:field type ...}` directly as a parameter -- synthesizes/interns
      // an anonymous struct type for it, gives the param itself a
      // compiler-private name, and records the field range so the body
      // can be auto-wrapped with `name`/`health`-style locals below.
      String8 synth_param_name = str8f(ctx_perm(), "__anon_param%u", low->anon_type_counter);
      low->anon_type_counter += 1;
      u32 field_first; u16 field_count;
      TypeRef t   = lower_anon_param_struct(low, flat[i], &field_first, &field_count);
      Param param = {0};
      param.name  = synth_param_name;
      param.type  = t;
      dyn_push(anon_temp.arena, scratch_params, param);
      AnonParamRef ap = {0};
      ap.param_name   = synth_param_name;
      ap.field_first  = field_first;
      ap.field_count  = field_count;
      dyn_push(anon_temp.arena, anon_params, ap);
      i += 1;
    } else if (pname->kind == AstNodeKind_Atom) {
      if (i + 1 >= flat_count) {
        lower_error(low, pname->token, "parameter `%.*s` is missing a type", str8_varg(pname->token.text));
        break;
      }
      AstNode* ptype_node = ast_get(low->ast, flat[i + 1]);
      if (ptype_node->kind == AstNodeKind_Atom && str8_match_lit("...", ptype_node->token.text, 0)) {
        // Variadic (`name ...`) only means anything on a bodyless `extern`
        // signature; see lower_extern_fn. A real body has no way to read the
        // trailing arguments -- no va_list/va_arg is exposed to user code --
        // so say so here rather than fall through to lower_type_node's
        // generic "expected a type".
        lower_error(low, ptype_node->token,
          "`...` (variadic) is only allowed in an `extern` fn signature, not a real `fn` body");
        i += 2;
        continue;
      }
      Param param      = {0};
      param.name       = pname->token.text;
      param.name_token = pname->token;
      param.type  = lower_type_node(low, flat[i + 1]);
      dyn_push(anon_temp.arena, scratch_params, param);
      i += 2;
    } else {
      lower_error(low, pname->token, "parameter must be a name, or a `{:field type ...}` anonymous struct shape");
      i += 1;
    }
  }
  u32 param_first = (u32)dyn_count(low->tast->params);
  u16 real_param_count = (u16)dyn_count(scratch_params);
  foreach_index(k, real_param_count) {
    dyn_push(low->tast->arena, low->tast->params, scratch_params[k]);
  }
  // scratch_params is finished with here, but stays in anon_temp rather than
  // getting its own temp -- see anon_temp above for why that is required.

  // The return type is mandatory and sits immediately after the parameter
  // vector, so no heuristic is needed to find it.
  TypeRef return_type = lower_return_type_node(low, children[3]);
  if (return_type.kind == TypeKind_Array) {
    lower_error(low, node->token,
      "`fn` cannot return an array type directly (C doesn't support that) -- return a pointer instead");
    arena_temp_end(&anon_temp);
    return TYPED_NIL;
  }
  u32     body_start  = 4;

  // Lets a nested `(return a b ...)` anywhere in this body build the struct
  // literal a multi-value return type needs; see lower_return. Saved and
  // restored rather than merely set, since lower_list's dispatch permits a
  // `fn` in expression position.
  String8 saved_multi_return_name   = low->current_fn_multi_return_name;
  low->current_fn_multi_return_name = (String8){0};
  if (return_type.kind == TypeKind_Named
      && hashtable_lookup(&low->multi_return_struct_set, return_type.name) != NULL) {
    low->current_fn_multi_return_name = return_type.name;
  }

  // Children must be fully lowered into scratch before any of them reach
  // low->tast->extra. lower_expr recurses into nested Blocks and Calls that
  // append their own children to that same array, so appending one at a time
  // would let those entries land inside this container's
  // first..first+count range and break its contiguity.
  ArenaTemp   body_temp  = arena_temp_begin(ctx_scratch());
  TypedIndex* stmts      = NULL;
  for (u32 i = body_start; i < count; i += 1) {
    TypedIndex stmt = lower_expr(low, children[i]);
    dyn_push(body_temp.arena, stmts, stmt);
  }
  u32 body_first = (u32)dyn_count(low->tast->extra);
  u16 body_count = (u16)dyn_count(stmts);
  foreach_index(i, body_count) {
    dyn_push(low->tast->arena, low->tast->extra, stmts[i]);
  }
  arena_temp_end(&body_temp);
  low->current_fn_multi_return_name = saved_multi_return_name;

  TypedNode block      = {0};
  block.kind           = TypedNodeKind_Block;
  block.token          = node->token;
  block.block.stmt_first = body_first;
  block.block.stmt_count = body_count;
  TypedIndex real_body_idx = typed_push(low->tast, block);

  // Any `{:field type ...}` params get auto-bound as plain locals by wrapping
  // the body in a synthesized `let`, using the same LetExpr-inside-a-one-
  // statement-Block shape lower_some_thread_step builds. fn.func.body must
  // remain a literal Block regardless (cg_function_body_stmts asserts it), so
  // with no anon params the plain block above is used unchanged.
  TypedIndex final_body_idx = real_body_idx;
  u32        anon_param_count = (u32)dyn_count(anon_params);
  if (anon_param_count > 0) {
    ArenaTemp bind_temp        = arena_temp_begin(ctx_scratch());
    Binding*  scratch_bindings = NULL;
    foreach_index(pi, anon_param_count) {
      AnonParamRef* ap = &anon_params[pi];
      TypedNode ident_n  = {0};
      ident_n.kind       = TypedNodeKind_Identifier;
      ident_n.token      = node->token;
      ident_n.ident.name = ap->param_name;
      TypedIndex ident_idx = typed_push(low->tast, ident_n);
      foreach_index(fi, ap->field_count) {
        Param* field = &low->tast->params[ap->field_first + fi];
        // No field-name token: this access is synthesized from the parameter's declared shape,
        // not written anywhere in the body it is spliced into.
        TypedIndex fa = make_field_access(low, node->token, ident_idx, field->name, (Token){0}, false);
        lower_destructure_bind(low, field->name, fa, &scratch_bindings, bind_temp.arena);
      }
    }
    u32 binding_first = (u32)dyn_count(low->tast->bindings);
    u16 binding_count = (u16)dyn_count(scratch_bindings);
    foreach_index(i, binding_count) {
      dyn_push(low->tast->arena, low->tast->bindings, scratch_bindings[i]);
    }
    arena_temp_end(&bind_temp);

    TypedNode let_n              = {0};
    let_n.kind                   = TypedNodeKind_LetExpr;
    let_n.token                  = node->token;
    let_n.let_expr.binding_first = binding_first;
    let_n.let_expr.binding_count = binding_count;
    let_n.let_expr.body          = real_body_idx;
    TypedIndex let_idx           = typed_push(low->tast, let_n);

    TypedNode outer_block = {0};
    outer_block.kind      = TypedNodeKind_Block;
    outer_block.token     = node->token;
    u32 outer_first       = (u32)dyn_count(low->tast->extra);
    dyn_push(low->tast->arena, low->tast->extra, let_idx);
    outer_block.block.stmt_first = outer_first;
    outer_block.block.stmt_count = 1;
    final_body_idx = typed_push(low->tast, outer_block);
  }
  arena_temp_end(&anon_temp);

  TypedNode fn         = {0};
  fn.kind              = TypedNodeKind_FunctionDecl;
  fn.token             = node->token;
  fn.func.name         = fn_name;
  fn.func.param_first  = param_first;
  fn.func.param_count  = real_param_count;
  fn.func.return_type  = return_type;
  fn.func.body         = final_body_idx;
  return typed_push(low->tast, fn);
}

// `(fn Name [params] ReturnType)` -- an extern signature, declaring something
// that already exists in C. It reuses TypedNodeKind_FunctionDecl with
// func.body == TYPED_NIL as the marker, so the checker still registers it and
// validates calls against it, but checks no body and codegen emits no
// definition.
//
// A trailing `name ...` pair marks the signature variadic; see the loop below.
static TypedIndex
lower_extern_fn(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count) {
  AstNode* node = ast_get(low->ast, idx);
  if (count != 4) {
    lower_error(low, node->token,
      "extern `fn` requires exactly a name, a `[]` parameter vector, and a return type (no body)");
    return TYPED_NIL;
  }
  AstNode* name_node = ast_get(low->ast, children[1]);
  if (name_node->kind != AstNodeKind_Atom) {
    lower_error(low, name_node->token, "extern `fn` name must be an atom");
    return TYPED_NIL;
  }
  String8 fn_name = name_node->token.text;

  AstNode* params_node = ast_get(low->ast, children[2]);
  if (params_node->kind != AstNodeKind_Vector) {
    lower_error(low, params_node->token, "extern `fn` parameters must be a vector, e.g. `[a i32 b i32]`");
    return TYPED_NIL;
  }
  u16        flat_count;
  NodeIndex* flat = ast_seq_children(low->ast, children[2], &flat_count);
  if (flat_count % 2 != 0) {
    lower_error(low, params_node->token,
      "parameter vector must contain name/type pairs (even number of entries)");
    return TYPED_NIL;
  }
  u32 param_first = (u32)dyn_count(low->tast->params);
  b32 is_variadic = false;
  for (u32 i = 0; i + 1 < flat_count; i += 2) {
    AstNode* pname = ast_get(low->ast, flat[i]);
    if (pname->kind != AstNodeKind_Atom) {
      lower_error(low, params_node->token, "parameter name must be an atom");
      continue;
    }
    // `name ...` -- the variadic tail, spelled as an ordinary name/type pair
    // so the vector keeps its even-count shape. The name is documentation
    // only, since an extern has no body to bind it in. C requires `...` to
    // follow at least one named parameter, hence both guards.
    AstNode* ptype_node = ast_get(low->ast, flat[i + 1]);
    if (ptype_node->kind == AstNodeKind_Atom && str8_match_lit("...", ptype_node->token.text, 0)) {
      if (i + 2 != flat_count) {
        lower_error(low, ptype_node->token, "`...` (variadic) must be the last parameter");
        continue;
      }
      if (i == 0) {
        lower_error(low, ptype_node->token, "`...` (variadic) requires at least one fixed parameter before it");
        continue;
      }
      is_variadic = true;
      continue;
    }
    Param param      = {0};
    param.name       = pname->token.text;
    param.name_token = pname->token;
    param.type  = lower_type_node(low, flat[i + 1]);
    dyn_push(low->tast->arena, low->tast->params, param);
  }
  u16 real_param_count = (u16)(dyn_count(low->tast->params) - param_first);

  TypeRef return_type = lower_return_type_node(low, children[3]);
  if (return_type.kind == TypeKind_Array) {
    lower_error(low, node->token, "extern `fn` cannot return an array type directly");
    return TYPED_NIL;
  }

  TypedNode fn        = {0};
  fn.kind             = TypedNodeKind_FunctionDecl;
  fn.token            = node->token;
  fn.func.name        = fn_name;
  fn.func.param_first = param_first;
  fn.func.param_count = real_param_count;
  fn.func.return_type = return_type;
  fn.func.body        = TYPED_NIL; // the "extern, no body" marker
  fn.func.is_variadic = is_variadic;
  return typed_push(low->tast, fn);
}

// `(val name type value)` -- module-level, explicitly typed, immutable.
// `(var name type value)` -- module-level, explicitly typed, mutable.
// Structurally identical; only the resulting TypedNodeKind differs.
//
// The value may be omitted only for an array, Vector, Map, or Set, which are
// then zero-initialized (see the TYPED_NIL handling in checker and codegen).
// Zero is each one's correct empty state: a Vector is a NULL dyn-grown pointer
// with `bbb_dyn_count(NULL) == 0`, and a Map/Set slot array allocates lazily on
// the first `map-set`/`set-add`. Every other type requires an explicit value.
TypedIndex
lower_val_or_var(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count, b32 is_var) {
  AstNode*    node      = ast_get(low->ast, idx);
  const char* form_name = is_var ? "var" : "val";
  if (count != 3 && count != 4) {
    lower_error(low, node->token,
      "`%s` requires a name, an explicit type, and (unless the type is an array) a value", form_name);
    return TYPED_NIL;
  }
  AstNode* name_node = ast_get(low->ast, children[1]);
  if (name_node->kind != AstNodeKind_Atom) {
    lower_error(low, node->token, "`%s` name must be an atom", form_name);
    return TYPED_NIL;
  }
  TypeRef type = lower_type_node(low, children[2]);

  TypedIndex init;
  if (count == 4) {
    init = lower_expr(low, children[3]);
  } else if (type.kind == TypeKind_Array || type.kind == TypeKind_Vector
          || type.kind == TypeKind_Map   || type.kind == TypeKind_Set) {
    init = TYPED_NIL; // omitted -- zero-initialized at codegen time
  } else {
    lower_error(low, node->token,
      "`%s` requires a value unless the type is an array, Vector, Map, or Set (all default to zero-initialized)", form_name);
    return TYPED_NIL;
  }

  TypedNode n = {0};
  n.token     = node->token;
  if (is_var) {
    n.kind          = TypedNodeKind_VarDecl;
    n.var_decl.name = name_node->token.text;
    n.var_decl.type = type;
    n.var_decl.init = init;
  } else {
    n.kind            = TypedNodeKind_ConstDecl;
    n.const_decl.name = name_node->token.text;
    n.const_decl.type = type;
    n.const_decl.init = init;
  }
  return typed_push(low->tast, n);
}

// Reads a destructuring pattern's source expression exactly once. A bare
// identifier can be re-referenced freely, since reading a variable twice costs
// nothing. Anything else -- a call, another destructure's field access --
// evaluates into a hidden temp Binding first, the same trick `some->` uses, so
// every field pulled from it reads that one evaluation.
TypedIndex
lower_destructure_source(Lowerer* low, NodeIndex source_ast_idx, Binding** scratch, Arena* scratch_arena, Token tok) {
  AstNode*   src_node = ast_get(low->ast, source_ast_idx);
  TypedIndex lowered  = lower_expr(low, source_ast_idx);
  if (src_node->kind == AstNodeKind_Atom) {
    return lowered;
  }
  String8 tmp_name = str8_lit("__destructure_src");
  Binding b        = {0};
  b.name           = tmp_name;
  b.type           = type_ref_unresolved(); // the checker infers it from `lowered`
  b.init           = lowered;
  dyn_push(scratch_arena, *scratch, b);

  TypedNode ref = {0};
  ref.kind       = TypedNodeKind_Identifier;
  ref.token      = tok;
  ref.ident.name = tmp_name;
  return typed_push(low->tast, ref);
}

// Pushes one destructured local into the scratch binding list. The type is
// always Unresolved: a destructured field has no source-level type to write,
// so the checker's LetExpr case infers it from `init`.
void
lower_destructure_bind(Lowerer* low, String8 local_name, TypedIndex init, Binding** scratch, Arena* scratch_arena) {
  (void)low;
  Binding b = {0};
  b.name    = local_name;
  b.type    = type_ref_unresolved();
  b.init    = init;
  dyn_push(scratch_arena, *scratch, b);
}

// Resolves one `:field target` pair. The target decides what becomes of the
// field's value: bind it under a new local name, bind it by reference, or
// recurse into a nested pattern.
void
lower_destructure_target(Lowerer* low, Token tok, String8 field_name, TypedIndex source,
                          NodeIndex target_idx, Binding** scratch, Arena* scratch_arena) {
  AstNode* t = ast_get(low->ast, target_idx);
  // `tok` is the `:field` atom itself here, so it doubles as the field name's own token --
  // hovering the pattern entry reports the field it names.
  if (t->kind == AstNodeKind_Atom) {
    TypedIndex fa = make_field_access(low, tok, source, field_name, tok, false);
    lower_destructure_bind(low, t->token.text, fa, scratch, scratch_arena);
  } else if (t->kind == AstNodeKind_Map) {
    TypedIndex nested_source = make_field_access(low, tok, source, field_name, tok, false);
    lower_destructure_map(low, target_idx, nested_source, scratch, scratch_arena);
  } else if (t->kind == AstNodeKind_Vector) {
    TypedIndex nested_source = make_field_access(low, tok, source, field_name, tok, false);
    lower_destructure_vector(low, target_idx, nested_source, scratch, scratch_arena);
  } else if (t->kind == AstNodeKind_List) {
    u16        lc;
    NodeIndex* lchildren = ast_seq_children(low->ast, target_idx, &lc);
    AstNode*   head       = (lc > 0) ? ast_get(low->ast, lchildren[0]) : NULL;
    AstNode*   local_atom = (lc == 2) ? ast_get(low->ast, lchildren[1]) : NULL;
    if (!head || !str8_match_lit("addr", head->token.text, 0) || lc != 2 ||
        !local_atom || local_atom->kind != AstNodeKind_Atom) {
      lower_error(low, t->token, "a destructuring target must be a local name, `(addr local)`, or a nested `{}`/`[]` pattern");
      return;
    }
    TypedIndex fa   = make_field_access(low, tok, source, field_name, tok, false);
    TypedNode  addr = {0};
    addr.kind       = TypedNodeKind_UnaryAddr;
    addr.token      = tok;
    addr.unary.expr = fa;
    lower_destructure_bind(low, local_atom->token.text, typed_push(low->tast, addr), scratch, scratch_arena);
  } else {
    lower_error(low, t->token, "a destructuring target must be a local name, `(addr local)`, or a nested `{}`/`[]` pattern");
  }
}

// Named destructuring: `{field1 field2 :field3 target3 (addr field4)} source`.
// A bare atom binds a field under its own name, `(addr atom)` binds it by
// reference, and a `:field` prefix hands the value to lower_destructure_target.
//
// Field names match by text and are resolved by the checker, as `get`'s are,
// so unlike positional destructuring this needs no type info at lowering time.
void
lower_destructure_map(Lowerer* low, NodeIndex map_idx, TypedIndex source, Binding** scratch, Arena* scratch_arena) {
  u16        flat_count;
  NodeIndex* flat = ast_seq_children(low->ast, map_idx, &flat_count);
  u32        i    = 0;
  while (i < flat_count) {
    AstNode* entry = ast_get(low->ast, flat[i]);
    if (entry->kind == AstNodeKind_Atom && entry->token.text.size > 0 && entry->token.text.str[0] == ':') {
      String8 field_name = str8_skip(entry->token.text, 1);
      if (i + 1 >= flat_count) {
        lower_error(low, entry->token, "`:%.*s` in a destructuring pattern needs a target after it", str8_varg(field_name));
        return;
      }
      lower_destructure_target(low, entry->token, field_name, source, flat[i + 1], scratch, scratch_arena);
      i += 2;
    } else if (entry->kind == AstNodeKind_Atom) {
      TypedIndex fa = make_field_access(low, entry->token, source, entry->token.text, entry->token, false);
      lower_destructure_bind(low, entry->token.text, fa, scratch, scratch_arena);
      i += 1;
    } else if (entry->kind == AstNodeKind_List) {
      u16        lc;
      NodeIndex* lchildren  = ast_seq_children(low->ast, flat[i], &lc);
      AstNode*   head       = (lc > 0) ? ast_get(low->ast, lchildren[0]) : NULL;
      AstNode*   field_atom = (lc == 2) ? ast_get(low->ast, lchildren[1]) : NULL;
      if (!head || !str8_match_lit("addr", head->token.text, 0) || lc != 2 ||
          !field_atom || field_atom->kind != AstNodeKind_Atom) {
        lower_error(low, entry->token, "destructuring pattern entries must be a field name, `:field target`, or `(addr field)`");
        return;
      }
      TypedIndex fa   = make_field_access(low, entry->token, source, field_atom->token.text,
                                          field_atom->token, false);
      TypedNode  addr = {0};
      addr.kind       = TypedNodeKind_UnaryAddr;
      addr.token      = entry->token;
      addr.unary.expr = fa;
      lower_destructure_bind(low, field_atom->token.text, typed_push(low->tast, addr), scratch, scratch_arena);
      i += 1;
    } else {
      lower_error(low, entry->token, "destructuring pattern entries must be a field name, `:field target`, or `(& field)`");
      return;
    }
  }
}

// Positional destructuring: `[a b c] source`. Lowering has no type info, so a
// slot cannot yet be resolved to a field or index; each becomes a
// TypedNodeKind_PositionalAccess placeholder that the checker rewrites in
// place into a FieldAccess (struct, by declared field order) or IndexAccess
// (array/pointer, by literal index) once `source`'s type is known.
//
// Binding fewer slots than there are fields is allowed.
void
lower_destructure_vector(Lowerer* low, NodeIndex vec_idx, TypedIndex source, Binding** scratch, Arena* scratch_arena) {
  AstNode*   vec_node = ast_get(low->ast, vec_idx);
  u16        flat_count;
  NodeIndex* flat = ast_seq_children(low->ast, vec_idx, &flat_count);
  foreach_index(slot, flat_count) {
    AstNode* entry      = ast_get(low->ast, flat[slot]);
    b32      by_ref     = false;
    AstNode* name_atom  = entry;
    NodeIndex nested_idx = NODE_NIL;
    if (entry->kind == AstNodeKind_List) {
      u16        lc;
      NodeIndex* lchildren = ast_seq_children(low->ast, flat[slot], &lc);
      AstNode*   head      = (lc > 0) ? ast_get(low->ast, lchildren[0]) : NULL;
      if (head && str8_match_lit("addr", head->token.text, 0) && lc == 2 &&
          ast_get(low->ast, lchildren[1])->kind == AstNodeKind_Atom) {
        by_ref    = true;
        name_atom = ast_get(low->ast, lchildren[1]);
      } else {
        lower_error(low, entry->token, "a positional destructuring slot must be a local name or `(addr local)`");
        continue;
      }
    } else if (entry->kind == AstNodeKind_Map || entry->kind == AstNodeKind_Vector) {
      nested_idx = flat[slot];
      name_atom  = NULL;
    } else if (entry->kind != AstNodeKind_Atom) {
      lower_error(low, entry->token, "a positional destructuring slot must be a local name, `(addr local)`, or a nested pattern");
      continue;
    }

    TypedNode pa               = {0};
    pa.kind                    = TypedNodeKind_PositionalAccess;
    pa.token                   = vec_node->token;
    pa.positional_access.base  = source;
    pa.positional_access.slot  = (u32)slot;
    TypedIndex pa_idx = typed_push(low->tast, pa);

    if (nested_idx != NODE_NIL) {
      AstNode* nested_node = ast_get(low->ast, nested_idx);
      if (nested_node->kind == AstNodeKind_Map) lower_destructure_map(low, nested_idx, pa_idx, scratch, scratch_arena);
      else                                       lower_destructure_vector(low, nested_idx, pa_idx, scratch, scratch_arena);
      continue;
    }

    TypedIndex init = pa_idx;
    if (by_ref) {
      TypedNode addr  = {0};
      addr.kind       = TypedNodeKind_UnaryAddr;
      addr.token      = entry->token;
      addr.unary.expr = pa_idx;
      init            = typed_push(low->tast, addr);
    }
    lower_destructure_bind(low, name_atom->token.text, init, scratch, scratch_arena);
  }
}

// `(let [name type init ...] body...)` -- a scoped expression, not a bare
// declaration: it binds for its own body only and evaluates to the body's
// last expression, as Scheme and Clojure `let` do.
//
// A binding entry is a `name type init` triple, or the 2-slot `name init`
// form whenever let_slot_looks_like_type can rule the middle slot out as a
// type, leaving the checker to infer from the initializer. `let` is the only
// form allowed to omit its type; checker.c's LetExpr case explains why.
//
// An entry starting with `{}` or `[]` is a destructuring pattern instead --
// `pattern source`, no type slot, since a destructured local's type is never
// written by hand. See lower_destructure_map and lower_destructure_vector.
TypedIndex
lower_let(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count) {
  AstNode* node = ast_get(low->ast, idx);
  if (count < 3) {
    lower_error(low, node->token, "`let` requires a `[]` binding vector and at least one body expression");
    return TYPED_NIL;
  }
  AstNode* bindings_node = ast_get(low->ast, children[1]);
  if (bindings_node->kind != AstNodeKind_Vector) {
    lower_error(low, bindings_node->token, "`let` bindings must be a vector, e.g. `[a i32 0 b i32 1]`");
    return TYPED_NIL;
  }
  u16        flat_count;
  NodeIndex* flat = ast_seq_children(low->ast, children[1], &flat_count);
  if (flat_count == 0) {
    lower_error(low, bindings_node->token, "`let` bindings must not be empty");
    return TYPED_NIL;
  }

  // Scratch-then-flush, as everywhere else: a binding's init expression can
  // recurse into a nested `let`, whose own bindings would otherwise land in
  // low->tast->bindings between this one's.
  ArenaTemp temp             = arena_temp_begin(ctx_scratch());
  Binding*  scratch_bindings = NULL;
  u32       i                = 0;
  while (i < flat_count) {
    AstNode* name_node = ast_get(low->ast, flat[i]);
    if (name_node->kind == AstNodeKind_Atom) {
      if (i + 1 >= flat_count) {
        lower_error(low, name_node->token, "`let` binding `%.*s` is missing an initializer",
                    str8_varg(name_node->token.text));
        break;
      }
      Binding b = {0};
      b.name    = name_node->token.text;
      if (let_slot_looks_like_type(low, flat[i + 1])) {
        if (i + 2 >= flat_count) {
          lower_error(low, name_node->token, "`let` binding `%.*s` is missing an initializer",
                      str8_varg(name_node->token.text));
          break;
        }
        b.type = lower_type_node(low, flat[i + 1]);
        b.init = lower_expr(low, flat[i + 2]);
        dyn_push(temp.arena, scratch_bindings, b);
        i += 3;
      } else {
        // No type slot: `flat[i+1]` is the initializer, and checker.c's
        // LetExpr case infers the type from it.
        b.type = type_ref_unresolved();
        b.init = lower_expr(low, flat[i + 1]);
        dyn_push(temp.arena, scratch_bindings, b);
        i += 2;
      }
    } else if (name_node->kind == AstNodeKind_Map || name_node->kind == AstNodeKind_Vector) {
      if (i + 1 >= flat_count) {
        lower_error(low, name_node->token, "destructuring pattern is missing a source expression");
        break;
      }
      TypedIndex source = lower_destructure_source(low, flat[i + 1], &scratch_bindings, temp.arena, name_node->token);
      if (name_node->kind == AstNodeKind_Map) lower_destructure_map(low, flat[i], source, &scratch_bindings, temp.arena);
      else                                     lower_destructure_vector(low, flat[i], source, &scratch_bindings, temp.arena);
      i += 2;
    } else {
      lower_error(low, name_node->token,
        "`let` binding must start with a name, a `{}` (named destructure), or a `[]` (positional destructure)");
      break;
    }
  }
  u32 binding_first = (u32)dyn_count(low->tast->bindings);
  u16 binding_count = (u16)dyn_count(scratch_bindings);
  foreach_index(i, binding_count) {
    dyn_push(low->tast->arena, low->tast->bindings, scratch_bindings[i]);
  }
  arena_temp_end(&temp);

  // Body: one or more expressions, the last of which is the let's value.
  ArenaTemp   body_temp = arena_temp_begin(ctx_scratch());
  TypedIndex* stmts     = NULL;
  for (u32 i = 2; i < count; i += 1) {
    TypedIndex stmt = lower_expr(low, children[i]);
    dyn_push(body_temp.arena, stmts, stmt);
  }
  u32 body_first = (u32)dyn_count(low->tast->extra);
  u16 body_count = (u16)dyn_count(stmts);
  foreach_index(i, body_count) {
    dyn_push(low->tast->arena, low->tast->extra, stmts[i]);
  }
  arena_temp_end(&body_temp);

  TypedNode block         = {0};
  block.kind              = TypedNodeKind_Block;
  block.token             = node->token;
  block.block.stmt_first  = body_first;
  block.block.stmt_count  = body_count;
  TypedIndex body_idx     = typed_push(low->tast, block);

  TypedNode n              = {0};
  n.kind                   = TypedNodeKind_LetExpr;
  n.token                  = node->token;
  n.let_expr.binding_first = binding_first;
  n.let_expr.binding_count = binding_count;
  n.let_expr.body          = body_idx;
  return typed_push(low->tast, n);
}

TypedIndex
lower_if(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count) {
  AstNode* node = ast_get(low->ast, idx);
  if (count != 3 && count != 4) {
    lower_error(low, node->token,
      "`if` requires a condition and a then-branch, with an optional else-branch");
    return TYPED_NIL;
  }
  TypedIndex cond    = lower_expr(low, children[1]);
  TypedIndex then_b  = lower_expr(low, children[2]);
  TypedIndex else_b  = (count == 4) ? lower_expr(low, children[3]) : TYPED_NIL;
  TypedNode  n       = {0};
  n.kind             = TypedNodeKind_IfExpr;
  n.token            = node->token;
  n.if_expr.cond         = cond;
  n.if_expr.then_branch  = then_b;
  n.if_expr.else_branch  = else_b;
  return typed_push(low->tast, n);
}

// Collects children[body_start..count) into a Block, scratch-then-flush like
// every other multi-statement body here. Shared by `while`, `for` and `when`,
// none of which require a non-empty body.
static TypedIndex
lower_block_from_children(Lowerer* low, Token token, NodeIndex* children, u32 body_start, u16 count) {
  ArenaTemp   body_temp = arena_temp_begin(ctx_scratch());
  TypedIndex* stmts     = NULL;
  for (u32 i = body_start; i < count; i += 1) {
    TypedIndex stmt = lower_expr(low, children[i]);
    dyn_push(body_temp.arena, stmts, stmt);
  }
  u32 body_first = (u32)dyn_count(low->tast->extra);
  u16 body_count = (u16)dyn_count(stmts);
  foreach_index(i, body_count) {
    dyn_push(low->tast->arena, low->tast->extra, stmts[i]);
  }
  arena_temp_end(&body_temp);

  TypedNode block        = {0};
  block.kind             = TypedNodeKind_Block;
  block.token            = token;
  block.block.stmt_first = body_first;
  block.block.stmt_count = body_count;
  return typed_push(low->tast, block);
}

// `(scratch [t] body...)` -- opens a temp-arena scope over the thread-local
// scratch arena and closes it at the end of the block.
// TypedNodeKind_ScratchExpr explains why this is a strict block form rather
// than a binding with an independent lifetime.
//
// The `[t]` vector holds one name and nothing else: the type is always
// `arena` and the value is always the scratch arena.
TypedIndex
lower_scratch(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count) {
  AstNode* node = ast_get(low->ast, idx);
  if (count < 3) {
    lower_error(low, node->token, "`scratch` requires a `[name]` vector and at least one body expression");
    return TYPED_NIL;
  }
  AstNode* name_vec = ast_get(low->ast, children[1]);
  if (name_vec->kind != AstNodeKind_Vector) {
    lower_error(low, name_vec->token, "`scratch`'s first argument must be a `[name]` vector, e.g. `[t]`");
    return TYPED_NIL;
  }
  u16        name_flat_count;
  NodeIndex* name_flat = ast_seq_children(low->ast, children[1], &name_flat_count);
  if (name_flat_count != 1) {
    lower_error(low, name_vec->token, "`scratch`'s `[]` must name exactly one arena, e.g. `[t]`");
    return TYPED_NIL;
  }
  AstNode* name_node = ast_get(low->ast, name_flat[0]);
  if (name_node->kind != AstNodeKind_Atom) {
    lower_error(low, name_node->token, "`scratch`'s bound name must be a plain identifier");
    return TYPED_NIL;
  }

  TypedIndex body_idx = lower_block_from_children(low, node->token, children, 2, count);

  TypedNode n              = {0};
  n.kind                   = TypedNodeKind_ScratchExpr;
  n.token                  = node->token;
  n.scratch_expr.var_name  = name_node->token.text;
  n.scratch_expr.body      = body_idx;
  return typed_push(low->tast, n);
}

// `(parallel [name init ...] body...)` -- forks the fixed lane pool, runs the
// body once per lane, joins synchronously. See TypedNodeKind_ParallelExpr.
//
// Captures are `name init` pairs, simpler than `let`'s bindings: no type slot
// and no destructuring, since a capture hands one value to every lane.
TypedIndex
lower_parallel(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count) {
  AstNode* node = ast_get(low->ast, idx);
  if (count < 2) {
    lower_error(low, node->token, "`parallel` requires a `[]` capture vector and zero or more body expressions");
    return TYPED_NIL;
  }
  AstNode* captures_node = ast_get(low->ast, children[1]);
  if (captures_node->kind != AstNodeKind_Vector) {
    lower_error(low, captures_node->token, "`parallel`'s captures must be a vector, e.g. `[items items count count]`");
    return TYPED_NIL;
  }
  u16        flat_count;
  NodeIndex* flat = ast_seq_children(low->ast, children[1], &flat_count);
  if (flat_count % 2 != 0) {
    lower_error(low, captures_node->token, "`parallel` captures must be `name init` pairs");
    return TYPED_NIL;
  }

  // Scratch-then-flush, for the reason lower_let's bindings loop gives.
  ArenaTemp temp             = arena_temp_begin(ctx_scratch());
  Binding*  scratch_bindings = NULL;
  for (u32 i = 0; i < flat_count; i += 2) {
    AstNode* name_node = ast_get(low->ast, flat[i]);
    if (name_node->kind != AstNodeKind_Atom) {
      lower_error(low, name_node->token, "`parallel` capture name must be a plain identifier");
      break;
    }
    Binding b = {0};
    b.name    = name_node->token.text;
    b.type    = type_ref_unresolved(); // captures are always inferred
    b.init    = lower_expr(low, flat[i + 1]);
    dyn_push(temp.arena, scratch_bindings, b);
  }
  u32 capture_first = (u32)dyn_count(low->tast->bindings);
  u16 capture_count = (u16)dyn_count(scratch_bindings);
  foreach_index(i, capture_count) {
    dyn_push(low->tast->arena, low->tast->bindings, scratch_bindings[i]);
  }
  arena_temp_end(&temp);

  TypedIndex body_idx = lower_block_from_children(low, node->token, children, 2, count);

  TypedNode n                   = {0};
  n.kind                        = TypedNodeKind_ParallelExpr;
  n.token                       = node->token;
  n.parallel_expr.capture_first = capture_first;
  n.parallel_expr.capture_count = capture_count;
  n.parallel_expr.body          = body_idx;
  return typed_push(low->tast, n);
}

// `(when pred body...)` -- sugar for `(if pred (do body...))`, desugared here
// into an ordinary IfExpr with else_branch = TYPED_NIL, which is what `if`
// itself produces when its else is omitted.
TypedIndex
lower_when(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count) {
  AstNode* node = ast_get(low->ast, idx);
  if (count < 2) {
    lower_error(low, node->token, "`when` requires a condition and zero or more body expressions");
    return TYPED_NIL;
  }
  TypedIndex cond_idx = lower_expr(low, children[1]);
  TypedIndex then_idx = lower_block_from_children(low, node->token, children, 2, count);
  TypedNode  n         = {0};
  n.kind                = TypedNodeKind_IfExpr;
  n.token                = node->token;
  n.if_expr.cond         = cond_idx;
  n.if_expr.then_branch  = then_idx;
  n.if_expr.else_branch  = TYPED_NIL;
  return typed_push(low->tast, n);
}

// The `(= __match_val pat)` comparison a single-value pattern lowers to.
// Factored out because lower_match_cond builds this same shape once per value
// in a group, ORed together.
static TypedIndex
lower_match_eq(Lowerer* low, String8 tmp_name, Token pat_token, NodeIndex pat_ast_idx) {
  TypedNode tmp_ref = {0};
  tmp_ref.kind       = TypedNodeKind_Identifier;
  tmp_ref.token      = pat_token;
  tmp_ref.ident.name = tmp_name;
  TypedIndex lhs = typed_push(low->tast, tmp_ref);
  TypedIndex rhs = lower_expr(low, pat_ast_idx);

  TypedNode eq = {0};
  eq.kind       = TypedNodeKind_BinaryEq;
  eq.token      = pat_token;
  eq.binary.lhs = lhs;
  eq.binary.rhs = rhs;
  return typed_push(low->tast, eq);
}

// A `match` pattern is one value (`sdl/SDLK_LEFT`) or a group of them
// (`[sdl/SDLK_LEFT sdl/SDLK_A]`). A group matches if the scrutinee equals any
// member, left-folded into `(or (= mv a) (= mv b) ...)` the way a variadic
// `(or a b c)` folds in lower_binary_op.
static TypedIndex
lower_match_cond(Lowerer* low, String8 tmp_name, AstNode* pat_node, NodeIndex pat_ast_idx) {
  if (pat_node->kind != AstNodeKind_Vector) {
    return lower_match_eq(low, tmp_name, pat_node->token, pat_ast_idx);
  }
  u16        group_count;
  NodeIndex* group = ast_seq_children(low->ast, pat_ast_idx, &group_count);
  if (group_count == 0) {
    lower_error(low, pat_node->token, "a grouped `match` pattern `[...]` needs at least one value");
    return TYPED_NIL;
  }
  TypedIndex cond = lower_match_eq(low, tmp_name, pat_node->token, group[0]);
  for (u16 gi = 1; gi < group_count; gi += 1) {
    TypedIndex rhs_eq = lower_match_eq(low, tmp_name, pat_node->token, group[gi]);
    TypedNode  or_n    = {0};
    or_n.kind          = TypedNodeKind_LogicalOr;
    or_n.token         = pat_node->token;
    or_n.binary.lhs    = cond;
    or_n.binary.rhs    = rhs_eq;
    cond               = typed_push(low->tast, or_n);
  }
  return cond;
}

// Wraps `value` as hand-written `(do value (cast void 0))` would: a 2-statement
// Block typed Void whatever `value` evaluates to, still evaluating `value` and
// its side effects once. lower_match uses it to force every arm void.
static TypedIndex
lower_match_void_wrap(Lowerer* low, Token tok, TypedIndex value) {
  TypedNode zero_lit     = {0};
  zero_lit.kind          = TypedNodeKind_IntLiteral;
  zero_lit.token         = tok;
  zero_lit.int_lit.value = 0;
  TypedIndex zero_idx    = typed_push(low->tast, zero_lit);

  TypedNode void_ty      = {0};
  void_ty.kind           = TypedNodeKind_Identifier;
  void_ty.token          = tok;
  void_ty.ident.name     = str8_lit("void");
  TypedIndex void_ty_idx = typed_push(low->tast, void_ty);

  TypedNode cast_n    = {0};
  cast_n.kind         = TypedNodeKind_BinaryCast;
  cast_n.token        = tok;
  cast_n.binary.lhs   = void_ty_idx;
  cast_n.binary.rhs   = zero_idx;
  TypedIndex cast_idx = typed_push(low->tast, cast_n);

  TypedNode block        = {0};
  block.kind             = TypedNodeKind_Block;
  block.token            = tok;
  block.block.stmt_first = (u32)dyn_count(low->tast->extra);
  block.block.stmt_count = 2;
  dyn_push(low->tast->arena, low->tast->extra, value);
  dyn_push(low->tast->arena, low->tast->extra, cast_idx);
  return typed_push(low->tast, block);
}

// `(match scrutinee [pat1 val1 pat2 val2 ... else default])`, desugared
// entirely here like `when`: no new TypedNodeKind, no new checker or codegen
// logic. It expands to
//
//   (let [__match_val scrutinee]
//     (if (= __match_val pat1) val1
//       (if (= __match_val pat2) val2
//         ...
//         default)))    ; TYPED_NIL when there is no `else`
//
// The scrutinee is bound once into a temp, so `match` does not require it to
// be cheap or idempotent to re-evaluate.
//
// Patterns compare with plain `=`, so `match` inherits exactly what `=`
// supports -- numeric, bool, enum and string equality -- and its exact
// semantics: no epsilon for floats, no case folding for strings. Either can
// still be written by hand with `string-match` or an explicit comparison.
//
// `else` is reserved rather than compared. If present it must be the last
// clause and supplies the final else-branch; without it that branch is
// TYPED_NIL, a no-op as a statement, and the checker's IfExpr branch-type
// rules already reject a non-exhaustive `match` used as a value.
TypedIndex
lower_match(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count) {
  AstNode* node = ast_get(low->ast, idx);
  if (count != 3) {
    lower_error(low, node->token, "`match` requires a scrutinee and a `[]` clause vector");
    return TYPED_NIL;
  }
  AstNode* clauses_node = ast_get(low->ast, children[2]);
  if (clauses_node->kind != AstNodeKind_Vector) {
    lower_error(low, clauses_node->token, "`match` clauses must be a vector, e.g. `[pat1 val1 pat2 val2]`");
    return TYPED_NIL;
  }
  u16        flat_count;
  NodeIndex* flat = ast_seq_children(low->ast, children[2], &flat_count);
  if (flat_count == 0 || flat_count % 2 != 0) {
    lower_error(low, clauses_node->token, "`match` clauses must be pattern/value pairs");
    return TYPED_NIL;
  }

  // Scrutinee bound once into an Unresolved-typed binding; the checker's
  // LetExpr case infers it from the already-checked init.
  String8 tmp_name = str8_lit("__match_val");
  Binding b = {0};
  b.name    = tmp_name;
  b.type    = type_ref_unresolved();
  b.init    = lower_expr(low, children[1]);
  u32 binding_first = (u32)dyn_count(low->tast->bindings);
  dyn_push(low->tast->arena, low->tast->bindings, b);

  TypedIndex else_branch = TYPED_NIL;
  b32        has_else    = false;
  u16        last_pair   = flat_count - 2;

  // Back-to-front, so each else_branch is already built by the time the
  // preceding pair needs to nest it.
  for (i32 pi = (i32)last_pair; pi >= 0; pi -= 2) {
    AstNode* pat_node = ast_get(low->ast, flat[pi]);
    b32 pat_is_else = pat_node->kind == AstNodeKind_Atom && str8_match_lit("else", pat_node->token.text, 0);

    if (pat_is_else) {
      if ((u32)pi != last_pair) {
        lower_error(low, pat_node->token, "`else` must be the last `match` clause");
        return TYPED_NIL;
      }
      else_branch = lower_expr(low, flat[pi + 1]);
      has_else    = true;
      continue;
    }

    TypedIndex cond  = lower_match_cond(low, tmp_name, pat_node, flat[pi]);
    TypedIndex value = lower_expr(low, flat[pi + 1]);
    // With no `else`, this `match` can never be used as a value, so every
    // arm is void-discarded. Without this, only the last clause would go
    // void; earlier ones would keep their real type and trip the
    // if-branch-mismatch check as soon as it wasn't void. `set` evaluates to
    // the assigned value, so a `match` of `set` arms -- the common statement
    // dispatcher -- would otherwise need this wrapping at every call site.
    if (!has_else) value = lower_match_void_wrap(low, pat_node->token, value);

    TypedNode ifn            = {0};
    ifn.kind                 = TypedNodeKind_IfExpr;
    ifn.token                = pat_node->token;
    ifn.if_expr.cond         = cond;
    ifn.if_expr.then_branch  = value;
    ifn.if_expr.else_branch  = else_branch;
    else_branch = typed_push(low->tast, ifn);
  }

  TypedNode block         = {0};
  block.kind              = TypedNodeKind_Block;
  block.token             = node->token;
  block.block.stmt_first  = (u32)dyn_count(low->tast->extra);
  block.block.stmt_count  = 1;
  dyn_push(low->tast->arena, low->tast->extra, else_branch);
  TypedIndex body_idx = typed_push(low->tast, block);

  TypedNode let_n               = {0};
  let_n.kind                    = TypedNodeKind_LetExpr;
  let_n.token                   = node->token;
  let_n.let_expr.binding_first  = binding_first;
  let_n.let_expr.binding_count  = 1;
  let_n.let_expr.body           = body_idx;
  return typed_push(low->tast, let_n);
}

// `(while cond body...)` -- always void. The body may run zero or many times,
// so there is no well-defined last value, unlike a Block or LetExpr that runs
// its statements exactly once.
TypedIndex
lower_while(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count) {
  AstNode* node = ast_get(low->ast, idx);
  if (count < 2) {
    lower_error(low, node->token, "`while` requires a condition and zero or more body expressions");
    return TYPED_NIL;
  }
  TypedIndex cond_idx      = lower_expr(low, children[1]);
  TypedIndex body_block_idx = lower_block_from_children(low, node->token, children, 2, count);

  TypedNode n         = {0};
  n.kind              = TypedNodeKind_WhileExpr;
  n.token             = node->token;
  n.while_expr.cond   = cond_idx;
  n.while_expr.body   = body_block_idx;
  return typed_push(low->tast, n);
}

static TypedIndex lower_foreach_clause(Lowerer* low, AstNode* node, NodeIndex* children, u16 count, NodeIndex* clause_flat);

// `(for [name begin end] body...)` and `(for [name begin end step] body...)`
// -- range iteration, both shapes producing one typed node.
//
// An omitted step stays TYPED_NIL rather than becoming a literal `1` here: it
// must match `begin`'s type (u32 range, u32 step; f32 range, f32 step) and
// that type is unknown until checking. The checker's ForRangeExpr case
// synthesizes the literal. Contrast lower_incdec, which can build its `1`
// immediately because it needs no type to do so.
//
// A 2-element clause is collection iteration instead and builds a
// TypedNodeKind_ForEachExpr; see lower_foreach_clause.
TypedIndex
lower_for(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count) {
  AstNode* node = ast_get(low->ast, idx);
  if (count < 2) {
    lower_error(low, node->token, "`for` requires a `[]` clause vector and zero or more body expressions");
    return TYPED_NIL;
  }
  AstNode* clause_node = ast_get(low->ast, children[1]);
  if (clause_node->kind != AstNodeKind_Vector) {
    lower_error(low, clause_node->token, "`for` clause must be a vector, e.g. `[i 0 10]`");
    return TYPED_NIL;
  }
  u16        flat_count;
  NodeIndex* flat = ast_seq_children(low->ast, children[1], &flat_count);

  if (flat_count == 2) {
    return lower_foreach_clause(low, node, children, count, flat);
  }
  if (flat_count != 3 && flat_count != 4) {
    lower_error(low, clause_node->token,
      "`for` clause must be `[name begin end]` or `[name begin end step]` "
      "(or `[item coll]` / `[[i item] coll]` to iterate a collection)");
    return TYPED_NIL;
  }

  AstNode* name_node = ast_get(low->ast, flat[0]);
  if (name_node->kind != AstNodeKind_Atom) {
    lower_error(low, name_node->token, "`for` loop variable name must be an atom");
    return TYPED_NIL;
  }

  TypedIndex begin_idx = lower_expr(low, flat[1]);
  TypedIndex end_idx   = lower_expr(low, flat[2]);
  // TYPED_NIL means "no explicit step"; the checker fills it in.
  TypedIndex step_idx  = flat_count == 4 ? lower_expr(low, flat[3]) : TYPED_NIL;

  TypedIndex body_block_idx = lower_block_from_children(low, node->token, children, 2, count);

  TypedNode n            = {0};
  n.kind                 = TypedNodeKind_ForRangeExpr;
  n.token                = node->token;
  n.for_range.var_name   = name_node->token.text;
  n.for_range.begin      = begin_idx;
  n.for_range.end        = end_idx;
  n.for_range.step       = step_idx;
  n.for_range.body       = body_block_idx;
  return typed_push(low->tast, n);
}

// `(for [item coll] body...)` and `(for [[i item] coll] body...)` -- the
// 2-element clause shape, split out of lower_for to keep it readable.
// `clause_flat[0]` is a bare atom or a `[i item]` vector; `clause_flat[1]` is
// the collection either way. The element and index types are unknown here, so
// the checker's ForEachExpr case resolves them from the collection's type.
static TypedIndex
lower_foreach_clause(Lowerer* low, AstNode* node, NodeIndex* children, u16 count, NodeIndex* clause_flat) {
  AstNode* first_node = ast_get(low->ast, clause_flat[0]);

  TypedNode n = {0};
  n.kind      = TypedNodeKind_ForEachExpr;
  n.token     = node->token;

  if (first_node->kind == AstNodeKind_Atom) {
    n.for_each.has_index = false;
    n.for_each.elem_name = first_node->token.text;
  } else if (first_node->kind == AstNodeKind_Vector) {
    u16        idx_flat_count;
    NodeIndex* idx_flat = ast_seq_children(low->ast, clause_flat[0], &idx_flat_count);
    if (idx_flat_count != 2) {
      lower_error(low, first_node->token, "`for`'s `[index item]` binding must name exactly two things, e.g. `[i x]`");
      return TYPED_NIL;
    }
    AstNode* index_atom = ast_get(low->ast, idx_flat[0]);
    AstNode* elem_atom  = ast_get(low->ast, idx_flat[1]);
    if (index_atom->kind != AstNodeKind_Atom || elem_atom->kind != AstNodeKind_Atom) {
      lower_error(low, first_node->token, "`for`'s `[index item]` binding names must be plain identifiers");
      return TYPED_NIL;
    }
    n.for_each.has_index  = true;
    n.for_each.index_name = index_atom->token.text;
    n.for_each.elem_name  = elem_atom->token.text;
  } else {
    lower_error(low, first_node->token, "`for` over a collection must be `[item coll]` or `[[index item] coll]`");
    return TYPED_NIL;
  }

  n.for_each.collection = lower_expr(low, clause_flat[1]);
  n.for_each.body       = lower_block_from_children(low, node->token, children, 2, count);
  return typed_push(low->tast, n);
}

// `(parallel-for [name count] body...)` -- valid only inside a `parallel`
// body, which the checker enforces. There is no begin or step: lane_range
// always partitions `[0, work_count)`, so the clause is exactly
// `[name count]`. See TypedNodeKind_ParallelForExpr.
TypedIndex
lower_parallel_for(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count) {
  AstNode* node = ast_get(low->ast, idx);
  if (count < 2) {
    lower_error(low, node->token, "`parallel-for` requires a `[]` clause vector and zero or more body expressions");
    return TYPED_NIL;
  }
  AstNode* clause_node = ast_get(low->ast, children[1]);
  if (clause_node->kind != AstNodeKind_Vector) {
    lower_error(low, clause_node->token, "`parallel-for` clause must be a vector, e.g. `[i n]`");
    return TYPED_NIL;
  }
  u16        flat_count;
  NodeIndex* flat = ast_seq_children(low->ast, children[1], &flat_count);
  if (flat_count != 2) {
    lower_error(low, clause_node->token, "`parallel-for` clause must be exactly `[name count]`");
    return TYPED_NIL;
  }

  AstNode* name_node = ast_get(low->ast, flat[0]);
  if (name_node->kind != AstNodeKind_Atom) {
    lower_error(low, name_node->token, "`parallel-for` loop variable name must be an atom");
    return TYPED_NIL;
  }

  TypedIndex count_idx      = lower_expr(low, flat[1]);
  TypedIndex body_block_idx = lower_block_from_children(low, node->token, children, 2, count);

  TypedNode n              = {0};
  n.kind                   = TypedNodeKind_ParallelForExpr;
  n.token                  = node->token;
  n.parallel_for.var_name  = name_node->token.text;
  n.parallel_for.count     = count_idx;
  n.parallel_for.body      = body_block_idx;
  return typed_push(low->tast, n);
}

// `(return v0 v1 ...)`, `(return value)`, or bare `(return)` -- an early
// return from the enclosing fn at any nesting depth. It reuses the `unary`
// union member; the bare form leaves it TYPED_NIL, which the checker later
// requires to mean a void-returning fn. See TypedNodeKind_ReturnExpr.
//
// Two or more values is multi-return sugar: it builds a `_0`/`_1`/... struct
// literal of the enclosing fn's multi-return type (threaded in by lower_fn as
// current_fn_multi_return_name) and returns that one literal, so the checker
// and codegen never see more than a single value here.
TypedIndex
lower_return(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count) {
  AstNode* node = ast_get(low->ast, idx);
  u16 value_count = count - 1; // `count` includes the `return` head
  TypedIndex value_idx = TYPED_NIL;
  if (value_count == 1) {
    value_idx = lower_expr(low, children[1]);
  } else if (value_count > 1) {
    if (low->current_fn_multi_return_name.size == 0) {
      lower_error(low, node->token,
        "`return` with %u values requires the enclosing `fn`'s declared return type to be a "
        "multi-value type, e.g. `(bool T)`", (u32)value_count);
      return TYPED_NIL;
    }
    ArenaTemp  temp           = arena_temp_begin(ctx_scratch());
    FieldInit* scratch_fields = NULL;
    for (u32 i = 0; i < value_count; i += 1) {
      FieldInit fi = {0};
      fi.name  = str8f(ctx_perm(), "_%u", i);
      fi.value = lower_expr(low, children[1 + i]);
      dyn_push(temp.arena, scratch_fields, fi);
    }
    u32 field_first = (u32)dyn_count(low->tast->field_inits);
    u16 field_count = (u16)dyn_count(scratch_fields);
    foreach_index(i, field_count) {
      dyn_push(low->tast->arena, low->tast->field_inits, scratch_fields[i]);
    }
    arena_temp_end(&temp);

    TypedNode lit                    = {0};
    lit.kind                         = TypedNodeKind_StructLiteral;
    lit.token                        = node->token;
    lit.struct_lit.type_name         = low->current_fn_multi_return_name;
    lit.struct_lit.type_name_token   = node->token;
    lit.struct_lit.field_first       = field_first;
    lit.struct_lit.field_count       = field_count;
    value_idx = typed_push(low->tast, lit);
  }
  TypedNode  n         = {0};
  n.kind               = TypedNodeKind_ReturnExpr;
  n.token              = node->token;
  n.unary.expr         = value_idx;
  return typed_push(low->tast, n);
}

// `(break)` and `(continue)` -- the same empty shape, so one function builds
// both. Neither takes an operand or a loop label; "which loop" is always the
// innermost one, which the checker resolves by depth rather than by name.
// Whether a loop encloses this at all is likewise the checker's job: lowering
// has no notion of nesting.
TypedIndex
lower_loop_jump(Lowerer* low, NodeIndex idx, u16 count, TypedNodeKind kind, const char* name) {
  AstNode* node = ast_get(low->ast, idx);
  if (count != 1) {
    lower_error(low, node->token, "`%s` takes no arguments", name);
    return TYPED_NIL;
  }
  TypedNode n = {0};
  n.kind      = kind;
  n.token     = node->token;
  return typed_push(low->tast, n);
}

TypedIndex
lower_do(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count) {
  AstNode* node = ast_get(low->ast, idx);

  ArenaTemp   temp  = arena_temp_begin(ctx_scratch());
  TypedIndex* stmts = NULL;
  for (u32 i = 1; i < count; i += 1) {
    TypedIndex stmt = lower_expr(low, children[i]);
    dyn_push(temp.arena, stmts, stmt);
  }
  u32 first = (u32)dyn_count(low->tast->extra);
  u16 stmt_count = (u16)dyn_count(stmts);
  foreach_index(i, stmt_count) {
    dyn_push(low->tast->arena, low->tast->extra, stmts[i]);
  }
  arena_temp_end(&temp);

  TypedNode n           = {0};
  n.kind                = TypedNodeKind_Block;
  n.token               = node->token;
  n.block.stmt_first    = first;
  n.block.stmt_count    = stmt_count;
  return typed_push(low->tast, n);
}

// `(void expr...)` -- `do` with the whole form's type forced to Void, wrapped
// the way lower_match wraps an else-less `match`.
//
// It exists so an `if`/`match` arm that is just a `set` or `+=` -- which
// evaluate to the assigned value, not void -- can be discarded without
// hand-writing a `(cast void 0)` tail wherever two branches must agree in
// type. `do` still produces a real value; this is its discarding sibling.
TypedIndex
lower_void_do(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count) {
  AstNode*   node = ast_get(low->ast, idx);
  TypedIndex body = lower_do(low, idx, children, count);
  return lower_match_void_wrap(low, node->token, body);
}

// Shared by every binary-shaped operator. A `variadic` one (+, -, *, /, the
// bitwise ops, and/or) takes two or more operands and left-folds into nested
// binary nodes, so `(+ a b c)` lowers as `(+ (+ a b) c)` and needs nothing new
// from the checker or codegen. Comparisons and shifts require exactly two,
// since chaining them has no unambiguous meaning -- `(< a b c)` would compare
// a bool against `c`.
TypedIndex
lower_binary_op(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count, TypedNodeKind kind, const char* op_name, b32 variadic) {
  AstNode* node = ast_get(low->ast, idx);
  if (variadic ? count < 3 : count != 3) {
    lower_error(low, node->token, variadic
      ? "`%s` requires at least two operands"
      : "`%s` requires exactly two operands", op_name);
    return TYPED_NIL;
  }
  TypedIndex acc = lower_expr(low, children[1]);
  for (u32 i = 2; i < count; i += 1) {
    TypedIndex rhs = lower_expr(low, children[i]);
    TypedNode  n   = {0};
    n.kind         = kind;
    n.token        = node->token;
    n.binary.lhs   = acc;
    n.binary.rhs   = rhs;
    acc            = typed_push(low->tast, n);
  }
  return acc;
}

// A bare `{:field value ...}` map literal used as a call argument, with no
// struct type name in front of it (contrast lower_struct_construct's
// `is_named` branch). Which struct it becomes depends on the callee's declared
// parameter type at this position, which is unresolved until checking, so
// `type_name` is left empty as a "needs inference" sentinel.
//
// The checker's Call case fills it in before argument checking, or leaves it
// empty and lets StructLiteral report its usual "unknown struct type".
static TypedIndex
lower_bare_map_literal(Lowerer* low, NodeIndex map_idx) {
  AstNode*   node = ast_get(low->ast, map_idx);
  u16        flat_count;
  NodeIndex* flat = ast_seq_children(low->ast, map_idx, &flat_count);
  if (flat_count % 2 != 0) {
    lower_error(low, node->token, "struct field map must contain `:key value` pairs (even number of entries)");
    return TYPED_NIL;
  }
  ArenaTemp  temp           = arena_temp_begin(ctx_scratch());
  FieldInit* scratch_fields = NULL;
  for (u32 i = 0; i + 1 < flat_count; i += 2) {
    AstNode* key_node = ast_get(low->ast, flat[i]);
    if (key_node->kind != AstNodeKind_Atom || key_node->token.text.size == 0 || key_node->token.text.str[0] != ':') {
      lower_error(low, key_node->token, "struct field keys must be `:keyword` atoms");
      continue;
    }
    String8    field_name = str8_skip(key_node->token.text, 1); // drop leading ':'
    TypedIndex value       = lower_expr(low, flat[i + 1]);
    FieldInit  fi          = {0};
    fi.name  = field_name;
    fi.value = value;
    dyn_push(temp.arena, scratch_fields, fi);
  }
  u32 field_first = (u32)dyn_count(low->tast->field_inits);
  u16 field_count = (u16)dyn_count(scratch_fields);
  foreach_index(i, field_count) {
    dyn_push(low->tast->arena, low->tast->field_inits, scratch_fields[i]);
  }
  arena_temp_end(&temp);

  TypedNode n = {0};
  n.kind                       = TypedNodeKind_StructLiteral;
  n.token                      = node->token;
  n.struct_lit.type_name       = str8_lit(""); // the "needs inference" sentinel
  n.struct_lit.type_name_token = node->token;
  n.struct_lit.field_first     = field_first;
  n.struct_lit.field_count     = field_count;
  return typed_push(low->tast, n);
}

TypedIndex
lower_call(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count) {
  AstNode* node   = ast_get(low->ast, idx);
  AstNode* head   = ast_get(low->ast, children[0]); // the caller has checked this is an Atom
  String8  callee = head->token.text;

  ArenaTemp   temp = arena_temp_begin(ctx_scratch());
  TypedIndex* args = NULL;
  for (u32 i = 1; i < count; i += 1) {
    AstNode*   arg_node = ast_get(low->ast, children[i]);
    TypedIndex arg       = (arg_node->kind == AstNodeKind_Map)
                         ? lower_bare_map_literal(low, children[i])
                         : lower_expr(low, children[i]);
    dyn_push(temp.arena, args, arg);
  }
  u32 first = (u32)dyn_count(low->tast->extra);
  u16 arg_count = (u16)dyn_count(args);
  foreach_index(i, arg_count) {
    dyn_push(low->tast->arena, low->tast->extra, args[i]);
  }
  arena_temp_end(&temp);

  TypedNode n         = {0};
  n.kind              = TypedNodeKind_Call;
  n.token             = node->token;
  n.call.callee       = callee;
  n.call.callee_token = head->token;
  n.call.arg_first    = first;
  n.call.arg_count    = arg_count;
  return typed_push(low->tast, n);
}

// `(struct Creature [name string health u16])`. Fields use the same flat
// `[name type ...]` shape as fn params and are stored as ordinary Param
// entries in the same array. `(union ...)` shares this function outright,
// differing only by is_union, which nothing before codegen reads.
//
// `(struct data "cgltf_data" [...])` additionally pins the C type this struct
// mirrors. Only `3b translate` writes it; see cg_ffi_c_type for what reads it.
TypedIndex
lower_struct_decl(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count, b32 is_union) {
  AstNode* node = ast_get(low->ast, idx);
  const char* kw = is_union ? "union" : "struct";
  if (count != 3 && count != 4) {
    lower_error(low, node->token, "`%s` requires a name, optionally a C spelling string, and a `[]` field vector", kw);
    return TYPED_NIL;
  }
  AstNode* name_node = ast_get(low->ast, children[1]);
  if (name_node->kind != AstNodeKind_Atom) {
    lower_error(low, name_node->token, "`%s` name must be an atom", kw);
    return TYPED_NIL;
  }
  String8 c_name = {0};
  if (count == 4) {
    AstNode* c_name_node = ast_get(low->ast, children[2]);
    if (c_name_node->kind != AstNodeKind_String) {
      lower_error(low, c_name_node->token, "`%s` C spelling must be a string literal", kw);
      return TYPED_NIL;
    }
    c_name = c_name_node->token.text;
    if (c_name.size == 0) {
      lower_error(low, c_name_node->token, "`%s` C spelling must not be empty", kw);
      return TYPED_NIL;
    }
  }
  NodeIndex fields_idx  = children[count - 1];
  AstNode*  fields_node = ast_get(low->ast, fields_idx);
  if (fields_node->kind != AstNodeKind_Vector) {
    lower_error(low, fields_node->token, "`%s` fields must be a vector, e.g. `[name string health u16]`", kw);
    return TYPED_NIL;
  }
  u16        flat_count;
  NodeIndex* flat = ast_seq_children(low->ast, fields_idx, &flat_count);
  if (flat_count == 0 || flat_count % 2 != 0) {
    lower_error(low, fields_node->token, "`%s` fields must be name/type pairs (even number of entries)", kw);
    return TYPED_NIL;
  }
  ArenaTemp scratch = arena_temp_begin(ctx_scratch());
  Param*    scratch_fields = NULL;
  for (u32 i = 0; i + 1 < flat_count; i += 2) {
    AstNode* fname = ast_get(low->ast, flat[i]);
    if (fname->kind != AstNodeKind_Atom) {
      lower_error(low, fields_node->token, "field name must be an atom");
      continue;
    }
    Param field = {0};
    field.name       = fname->token.text;
    field.name_token = fname->token;
    field.is_anon = str8_match_lit("_", field.name, 0); // C11 anonymous member -- see Param.is_anon
    field.type    = lower_type_node(low, flat[i + 1]); // a nested anonymous struct/union pushes its
                                                        // own fields into low->tast->params, so this
                                                        // struct's go to scratch until that's done
    dyn_push(scratch.arena, scratch_fields, field);
  }
  u32 field_first = (u32)dyn_count(low->tast->params); // taken after every nested push above, so the
                                                          // range stays contiguous
  foreach_index(i, dyn_count(scratch_fields)) {
    dyn_push(low->tast->arena, low->tast->params, scratch_fields[i]);
  }
  u16 real_field_count = (u16)dyn_count(scratch_fields);
  arena_temp_end(&scratch);

  TypedNode n = {0};
  n.kind                    = TypedNodeKind_StructDecl;
  n.token                   = node->token;
  n.struct_decl.name        = name_node->token.text;
  n.struct_decl.field_first = field_first;
  n.struct_decl.field_count = real_field_count;
  n.struct_decl.is_union    = is_union;
  n.struct_decl.c_name      = c_name;
  return typed_push(low->tast, n);
}

// `(alias NewName ExistingType)` -- produces a typed node so codegen can emit
// a C typedef (cg_alias_decl), even though lower_program's alias-gathering
// pass already resolved the underlying type. Re-resolving here is redundant
// but harmless, since lower_type_node has no side effects.
TypedIndex
lower_alias_decl(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count) {
  AstNode* node = ast_get(low->ast, idx);
  if (count != 3 && count != 4) {
    lower_error(low, node->token, "`alias` requires a new name, an existing type, and optionally a C spelling string");
    return TYPED_NIL;
  }
  AstNode* name_node = ast_get(low->ast, children[1]);
  if (name_node->kind != AstNodeKind_Atom) {
    lower_error(low, name_node->token, "`alias` name must be an atom");
    return TYPED_NIL;
  }
  TypeRef underlying = lower_type_node(low, children[2]);

  // The optional third operand pins the C spelling of the emitted typedef. Only
  // translated bindings write it: see cg_alias_decl for what it is for.
  String8 c_name = {0};
  if (count == 4) {
    AstNode* c_name_node = ast_get(low->ast, children[3]);
    if (c_name_node->kind != AstNodeKind_String) {
      lower_error(low, c_name_node->token, "`alias` C spelling must be a string literal");
      return TYPED_NIL;
    }
    c_name = c_name_node->token.text;
    if (c_name.size == 0) {
      lower_error(low, c_name_node->token, "`alias` C spelling must not be empty");
      return TYPED_NIL;
    }
  }

  TypedNode n = {0};
  n.kind              = TypedNodeKind_AliasDecl;
  n.token             = node->token;
  n.alias_decl.name   = name_node->token.text;
  n.alias_decl.type   = underlying;
  n.alias_decl.c_name = c_name;
  return typed_push(low->tast, n);
}

// `(handle Name)` -- records which struct a handle pool is for. The name is
// already registered by the time this runs; lower_program's pre-pass calls
// lower_register_handle_pool_type. codegen.c's cg_handle_pool_decl does the
// real work, emitting base.h's DEFINE_HANDLE_POOL and the pool storage.
TypedIndex
lower_handle_pool_decl(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count) {
  AstNode* node = ast_get(low->ast, idx);
  if (count != 2) {
    lower_error(low, node->token, "`handle` requires exactly one type name, e.g. `(handle Mesh)`");
    return TYPED_NIL;
  }
  AstNode* name_node = ast_get(low->ast, children[1]);
  if (name_node->kind != AstNodeKind_Atom) {
    lower_error(low, name_node->token, "`handle` name must be an atom");
    return TYPED_NIL;
  }
  TypedNode n = {0};
  n.kind                        = TypedNodeKind_HandlePoolDecl;
  n.token                       = node->token;
  n.handle_pool_decl.type_name  = name_node->token.text;
  return typed_push(low->tast, n);
}

// `(handle-pool-init Name capacity arena)` -- one-time pool storage init.
// `Name` is the bare struct type (`Mesh`, not `Mesh^`), as in `handle-alloc`:
// you name what is pooled, not the handle type. The checker validates that
// `Name` has a `(handle Name)`, as it does for every type-query form.
TypedIndex
lower_handle_pool_init(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count) {
  AstNode* node = ast_get(low->ast, idx);
  if (count != 4) {
    lower_error(low, node->token,
      "`handle-pool-init` requires a type, a capacity, and an arena, e.g. `(handle-pool-init Mesh 256 a)`");
    return TYPED_NIL;
  }
  TypedNode n = {0};
  n.kind                       = TypedNodeKind_HandlePoolInit;
  n.token                      = node->token;
  n.handle_pool_init.type      = lower_type_node(low, children[1]);
  n.handle_pool_init.capacity  = lower_expr(low, children[2]);
  n.handle_pool_init.arena     = lower_expr(low, children[3]);
  return typed_push(low->tast, n);
}

// `(enum Name [variant ...])` and `(flags Name [variant ...])`. Variants are a
// flat sequence of bare atoms, each optionally followed by an explicit value
// (`NotFound 404`), so unlike struct fields or fn params the entries are not
// fixed-arity pairs. Only `is_flags` differs between the two forms, and only
// codegen -- which assigns the final values -- reads it.
//
// `(enum PixelFormat "enum AVPixelFormat" [...])` additionally pins the C type
// this enum mirrors, exactly as lower_struct_decl's does; see cg_ffi_c_type.
TypedIndex
lower_enum_decl(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count, b32 is_flags) {
  AstNode*    node      = ast_get(low->ast, idx);
  const char* form_name = is_flags ? "flags" : "enum";
  if (count != 3 && count != 4) {
    lower_error(low, node->token, "`%s` requires a name, optionally a C spelling string, and a `[]` variant vector", form_name);
    return TYPED_NIL;
  }
  AstNode* name_node = ast_get(low->ast, children[1]);
  if (name_node->kind != AstNodeKind_Atom) {
    lower_error(low, name_node->token, "`%s` name must be an atom", form_name);
    return TYPED_NIL;
  }
  String8 c_name = {0};
  if (count == 4) {
    AstNode* c_name_node = ast_get(low->ast, children[2]);
    if (c_name_node->kind != AstNodeKind_String) {
      lower_error(low, c_name_node->token, "`%s` C spelling must be a string literal", form_name);
      return TYPED_NIL;
    }
    c_name = c_name_node->token.text;
    if (c_name.size == 0) {
      lower_error(low, c_name_node->token, "`%s` C spelling must not be empty", form_name);
      return TYPED_NIL;
    }
  }
  NodeIndex variants_idx  = children[count - 1];
  AstNode*  variants_node = ast_get(low->ast, variants_idx);
  if (variants_node->kind != AstNodeKind_Vector) {
    lower_error(low, variants_node->token, "`%s` variants must be a vector, e.g. `[A B C]`", form_name);
    return TYPED_NIL;
  }
  u16        flat_count;
  NodeIndex* flat = ast_seq_children(low->ast, variants_idx, &flat_count);
  if (flat_count == 0) {
    lower_error(low, variants_node->token, "`%s` must declare at least one variant", form_name);
    return TYPED_NIL;
  }

  u32 variant_first = (u32)dyn_count(low->tast->enum_variants);
  u32 i             = 0;
  while (i < flat_count) {
    AstNode* vname_node = ast_get(low->ast, flat[i]);
    if (vname_node->kind != AstNodeKind_Atom || atom_looks_numeric(vname_node->token.text)) {
      lower_error(low, vname_node->token, "%s variant name must be a non-numeric atom", form_name);
      i += 1;
      continue;
    }
    EnumVariant v = {0};
    v.name        = vname_node->token.text;
    i += 1;
    if (i < flat_count) {
      AstNode* maybe_value = ast_get(low->ast, flat[i]);
      NumericAtomInfo vnum = maybe_value->kind == AstNodeKind_Atom
                           ? atom_classify_numeric(maybe_value->token.text)
                           : (NumericAtomInfo){0};
      if (vnum.is_numeric) {
        v.has_explicit_value = true;
        if (vnum.is_float) {
          lower_error(low, maybe_value->token, "%s variant value `%.*s` must be an integer",
                       form_name, str8_varg(maybe_value->token.text));
        } else if (!atom_parse_i64(vnum.body, vnum.is_hex, &v.value)) {
          lower_error(low, maybe_value->token, "%s variant value `%.*s` doesn't fit in 64 bits",
                       form_name, str8_varg(maybe_value->token.text));
        }
        i += 1;
      }
    }
    dyn_push(low->tast->arena, low->tast->enum_variants, v);
  }
  u16 variant_count = (u16)(dyn_count(low->tast->enum_variants) - variant_first);

  TypedNode n = {0};
  n.kind                    = TypedNodeKind_EnumDecl;
  n.token                   = node->token;
  n.enum_decl.name          = name_node->token.text;
  n.enum_decl.is_flags      = is_flags;
  n.enum_decl.variant_first = variant_first;
  n.enum_decl.variant_count = variant_count;
  n.enum_decl.c_name        = c_name;
  return typed_push(low->tast, n);
}

// Struct construction in two shapes:
//
//   (Creature {:name "Orc" :health 50})   named, by field
//   (Creature "Orc" 50)                   positional, in declared order
//
// A single `{}` argument means named; anything else means positional. No
// keyword distinguishes them, matching how `push`'s second argument and this
// file's head-symbol dispatch resolve shape ambiguity elsewhere.
//
// Positional arity is strict, unlike positional destructuring's under-binding:
// a constructor that silently zero-fills forgotten fields is a bug magnet.
TypedIndex
lower_struct_construct(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count, String8 struct_name) {
  AstNode* node = ast_get(low->ast, idx);

  // Positional construction needs the declared field list, to give each value
  // a field name by declared order. It comes from the raw `(struct Name [...])`
  // form, the same lookup `member-type` uses, so declaration order is free.
  //
  // Named construction never needs it, taking its names from the `{}` map for
  // the checker to resolve, so it works even for a synthesized anonymous
  // struct with no raw form (the NODE_NIL placeholder in
  // lower_anon_struct_type). Constructing one of those positionally is a real
  // limitation: there is no declared source order to map positions onto.
  lower_ensure_name_tables(low);
  NodeIndex* form_box = (NodeIndex*)hashtable_lookup(&low->struct_form_by_name, struct_name);
  if (!form_box) {
    lower_error(low, node->token, "unknown struct type `%.*s`", str8_varg(struct_name));
    return TYPED_NIL;
  }
  NodeIndex struct_form = *form_box;

  b32 is_named = (count == 2) && (ast_get(low->ast, children[1])->kind == AstNodeKind_Map);

  u16        decl_flat_count = 0;
  NodeIndex* decl_flat       = NULL;
  u16        declared_field_count = 0;
  if (!is_named) {
    if (struct_form == NODE_NIL) {
      lower_error(low, node->token,
        "`%.*s` is an anonymous struct/union (from `(struct {:field type ...})`/`(union {...})`), which "
        "has no declared field ORDER in source to construct positionally -- use named construction "
        "instead, e.g. `(%.*s {:field value ...})`", str8_varg(struct_name), str8_varg(struct_name));
      return TYPED_NIL;
    }
    NodeIndex decl_fields = lower_struct_form_fields(low, struct_form);
    if (decl_fields == NODE_NIL) {
      lower_error(low, node->token, "`%.*s`'s own `struct` declaration is malformed", str8_varg(struct_name));
      return TYPED_NIL;
    }
    decl_flat = ast_seq_children(low->ast, decl_fields, &decl_flat_count);
    declared_field_count = (u16)(decl_flat_count / 2); // `[f1 t1 f2 t2 ...]`
  }

  // Scratch-then-flush: a field's value can be a nested struct construction,
  // which appends to low->tast->field_inits itself.
  ArenaTemp  temp            = arena_temp_begin(ctx_scratch());
  FieldInit* scratch_fields  = NULL;

  if (is_named) {
    u16        flat_count;
    NodeIndex* flat = ast_seq_children(low->ast, children[1], &flat_count);
    if (flat_count % 2 != 0) {
      lower_error(low, node->token, "struct field map must contain `:key value` pairs (even number of entries)");
      arena_temp_end(&temp);
      return TYPED_NIL;
    }
    for (u32 i = 0; i + 1 < flat_count; i += 2) {
      AstNode* key_node = ast_get(low->ast, flat[i]);
      if (key_node->kind != AstNodeKind_Atom || key_node->token.text.size == 0 || key_node->token.text.str[0] != ':') {
        lower_error(low, key_node->token, "struct field keys must be `:keyword` atoms");
        continue;
      }
      String8    field_name = str8_skip(key_node->token.text, 1); // drop leading ':'
      TypedIndex value       = lower_expr(low, flat[i + 1]);
      FieldInit  fi          = {0};
      fi.name  = field_name;
      fi.value = value;
      dyn_push(temp.arena, scratch_fields, fi);
    }
  } else {
    u16 given = (u16)(count - 1);
    if (given != declared_field_count) {
      lower_error(low, node->token,
        "`%.*s` positional construction supplies %u value(s), struct declares %u field(s)",
        str8_varg(struct_name), (u32)given, (u32)declared_field_count);
      arena_temp_end(&temp);
      return TYPED_NIL;
    }
    for (u32 i = 0; i < given; i += 1) {
      AstNode*   fname_node = ast_get(low->ast, decl_flat[i * 2]);
      TypedIndex value      = lower_expr(low, children[1 + i]);
      FieldInit  fi         = {0};
      fi.name  = fname_node->token.text;
      fi.value = value;
      dyn_push(temp.arena, scratch_fields, fi);
    }
  }

  u32 field_first = (u32)dyn_count(low->tast->field_inits);
  u16 field_count = (u16)dyn_count(scratch_fields);
  foreach_index(i, field_count) {
    dyn_push(low->tast->arena, low->tast->field_inits, scratch_fields[i]);
  }
  arena_temp_end(&temp);

  TypedNode n = {0};
  n.kind                           = TypedNodeKind_StructLiteral;
  n.token                          = node->token;
  n.struct_lit.type_name           = struct_name;
  n.struct_lit.type_name_token     = ast_get(low->ast, children[0])->token;
  n.struct_lit.field_first         = field_first;
  n.struct_lit.field_count         = field_count;
  return typed_push(low->tast, n);
}

// `(push arena Type)`, `(push arena Type Count)`, `(push arena value)`. The
// two Type forms unify into one typed kind, the countless one synthesizing a
// literal `1`. The copy form is a different operation -- arena_push plus a
// memory copy -- so it gets its own kind.
//
// The shapes are told apart by whether the second argument is a bare atom
// naming a known type (is_known_type_atom). If so, a third argument is the
// count; otherwise the second is a value to copy and no count is accepted.
TypedIndex
lower_push(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count, b32 zeroed) {
  AstNode*    node      = ast_get(low->ast, idx);
  const char* form_name = zeroed ? "push0/push-zero" : "push";
  if (count < 3 || count > 4) {
    lower_error(low, node->token,
      "`%s` requires an arena and either a type (with an optional count) or a value to copy", form_name);
    return TYPED_NIL;
  }
  TypedIndex arena_idx = lower_expr(low, children[1]);

  AstNode* second_node = ast_get(low->ast, children[2]);
  b32 second_is_type = (second_node->kind == AstNodeKind_Atom) && is_known_type_atom(low, second_node->token.text);

  if (second_is_type) {
    TypeRef    elem_type = type_ref_from_atom(ctx_perm(), second_node->token.text);
    TypedIndex count_idx;
    if (count == 4) {
      count_idx = lower_expr(low, children[3]);
    } else {
      TypedNode one_lit     = {0};
      one_lit.kind          = TypedNodeKind_IntLiteral;
      one_lit.token         = node->token;
      one_lit.int_lit.value = 1;
      count_idx             = typed_push(low->tast, one_lit);
    }
    TypedNode n            = {0};
    n.kind                 = TypedNodeKind_PushAlloc;
    n.token                = node->token;
    n.push_alloc.elem_type = elem_type;
    n.push_alloc.arena     = arena_idx;
    n.push_alloc.count     = count_idx;
    n.push_alloc.zeroed    = zeroed;
    return typed_push(low->tast, n);
  }

  // Not a known type, so this is the copy shape, which takes no count.
  if (count != 3) {
    lower_error(low, node->token, "`%s` with a value to copy takes no count argument", form_name);
    return TYPED_NIL;
  }
  if (zeroed) {
    lower_error(low, node->token,
      "`%s` doesn't make sense with a value to copy -- the copy always overwrites the zeroed memory", form_name);
    return TYPED_NIL;
  }
  TypedIndex value_idx = lower_expr(low, children[2]);
  TypedNode  n          = {0};
  n.kind                = TypedNodeKind_PushCopy;
  n.token                = node->token;
  n.push_copy.arena      = arena_idx;
  n.push_copy.value      = value_idx;
  return typed_push(low->tast, n);
}

// `(alloc Type)` and `(alloc Type Count)` -- malloc-backed allocation, for
// something needing a lifetime independent of any arena. Like `push`'s type
// form, a missing count becomes a literal `1`. `free` is not handled here: it
// takes a plain pointer and no type, so it is an ordinary Call-shaped builtin.
TypedIndex
lower_alloc(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count) {
  AstNode* node = ast_get(low->ast, idx);
  if (count < 2 || count > 3) {
    lower_error(low, node->token, "`alloc` requires a type and an optional count");
    return TYPED_NIL;
  }
  AstNode* type_node = ast_get(low->ast, children[1]);
  if (type_node->kind != AstNodeKind_Atom || !is_known_type_atom(low, type_node->token.text)) {
    lower_error(low, node->token, "`alloc`'s first argument must name a known type");
    return TYPED_NIL;
  }
  TypeRef    elem_type = type_ref_from_atom(ctx_perm(), type_node->token.text);
  TypedIndex count_idx;
  if (count == 3) {
    count_idx = lower_expr(low, children[2]);
  } else {
    TypedNode one_lit     = {0};
    one_lit.kind          = TypedNodeKind_IntLiteral;
    one_lit.token         = node->token;
    one_lit.int_lit.value = 1;
    count_idx             = typed_push(low->tast, one_lit);
  }
  TypedNode n            = {0};
  n.kind                 = TypedNodeKind_AllocExpr;
  n.token                = node->token;
  n.alloc_expr.elem_type = elem_type;
  n.alloc_expr.count     = count_idx;
  return typed_push(low->tast, n);
}

// `(dyn-push arena arr value)`. `arr` must be a bare identifier or a
// field-access chain (`.`/`get`/`get-in`), not an arbitrary expression as
// `push`'s arena argument may be: bbb_dyn_push's macro expands it many times
// and reseats it on growth, so it has to be a repeatable, side-effect-free C
// lvalue.
//
// The field-access shape reuses lower_set_target's detection and lowering
// wholesale, for the reason SetTargetKind_Field gives: `.` already bakes in
// auto-deref per hop, and C already accepts `base.field` as an assignment
// target, so there is no separate lvalue form to build.
TypedIndex
lower_dyn_push(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count) {
  AstNode* node = ast_get(low->ast, idx);
  if (count != 4) {
    lower_error(low, node->token, "`dyn-push` requires exactly an arena, an array-variable name, and a value");
    return TYPED_NIL;
  }
  TypedIndex arena_idx = lower_expr(low, children[1]);

  AstNode* arr_node = ast_get(low->ast, children[2]);
  TypedNode n = {0};
  n.kind      = TypedNodeKind_DynPush;
  n.token     = node->token;
  n.dyn_push.arena = arena_idx;

  if (arr_node->kind == AstNodeKind_Atom) {
    n.dyn_push.arr_name = arr_node->token.text;
  } else if (arr_node->kind == AstNodeKind_List) {
    u16        ac;
    NodeIndex* achildren = ast_seq_children(low->ast, children[2], &ac);
    AstNode*   ahead     = (ac > 0) ? ast_get(low->ast, achildren[0]) : NULL;
    b32        is_dot    = ahead && ahead->kind == AstNodeKind_Atom && str8_match_lit(".", ahead->token.text, 0);
    b32        is_get    = ahead && ahead->kind == AstNodeKind_Atom && str8_match_lit("get", ahead->token.text, 0);
    b32        is_get_in = ahead && ahead->kind == AstNodeKind_Atom && str8_match_lit("get-in", ahead->token.text, 0);
    if (is_dot || is_get || is_get_in) {
      n.dyn_push.is_field_target = true;
      if (is_dot)         n.dyn_push.target_expr = lower_dot(low, children[2], achildren, ac);
      else if (is_get)    n.dyn_push.target_expr = lower_get(low, children[2], achildren, ac);
      else                n.dyn_push.target_expr = lower_get_in(low, children[2], achildren, ac);
      if (n.dyn_push.target_expr == TYPED_NIL) return TYPED_NIL; // error already reported
    } else {
      lower_error(low, arr_node->token,
                  "`dyn-push`'s array argument must be a plain identifier or a field access (`.`/`get`/`get-in`)");
      return TYPED_NIL;
    }
  } else {
    lower_error(low, arr_node->token,
                "`dyn-push`'s array argument must be a plain identifier or a field access (`.`/`get`/`get-in`)");
    return TYPED_NIL;
  }

  n.dyn_push.value = lower_expr(low, children[3]);
  return typed_push(low->tast, n);
}

// `(commit dst-arena src)` -- both plain expressions, unlike `dyn-push`'s
// `arr`. `commit` only reads `src`, through its hidden dyn-array count, and
// never reseats it, so it needs no lvalue restriction.
TypedIndex
lower_commit(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count) {
  AstNode* node = ast_get(low->ast, idx);
  if (count != 3) {
    lower_error(low, node->token, "`commit` requires exactly a destination arena and a dyn-push-grown array");
    return TYPED_NIL;
  }
  TypedIndex dst_idx = lower_expr(low, children[1]);
  TypedIndex src_idx = lower_expr(low, children[2]);

  TypedNode n              = {0};
  n.kind                   = TypedNodeKind_CommitExpr;
  n.token                  = node->token;
  n.commit_expr.dst_arena  = dst_idx;
  n.commit_expr.src        = src_idx;
  return typed_push(low->tast, n);
}

// Resolves the base and index of lower_set_target's `(nth base index)` shape.
// A `[i1 i2 ...]` chain works as it does for reads in lower_nth: every index
// but the last becomes an ordinary read via make_index_access, and only the
// last is the write target. `(set (nth grid [1 2]) v)` reads `grid[1]` and
// writes through the final `[2]`.
static void
lower_nth_target(Lowerer* low, Token token, NodeIndex base_ast_idx, NodeIndex index_ast_idx,
                  TypedIndex* out_base, TypedIndex* out_index) {
  TypedIndex base_idx   = lower_expr(low, base_ast_idx);
  AstNode*   index_node = ast_get(low->ast, index_ast_idx);
  if (index_node->kind != AstNodeKind_Vector) {
    *out_base  = base_idx;
    *out_index = lower_expr(low, index_ast_idx);
    return;
  }
  u16        flat_count;
  NodeIndex* flat = ast_seq_children(low->ast, index_ast_idx, &flat_count);
  if (flat_count == 0) {
    lower_error(low, index_node->token, "`nth`'s `[]` index list must have at least one index");
    *out_base  = TYPED_NIL;
    *out_index = TYPED_NIL;
    return;
  }
  TypedIndex cur = base_idx;
  foreach_index(i, flat_count) {
    TypedIndex index_i = lower_expr(low, flat[i]);
    if (i + 1 == flat_count) {
      *out_base  = cur;
      *out_index = index_i;
    } else {
      cur = make_index_access(low, token, cur, index_i);
    }
  }
}

// Shared by `set`, `++`/`--` and the compound assignments: parses a target and
// builds the SetExpr assigning `value` to it. A target is a plain identifier,
// `(deref expr)` to write through a pointer, `(nth base index)` to write
// through an index, or a `.`/`get`/`get-in` field access.
TypedIndex
lower_set_target(Lowerer* low, Token token, NodeIndex target_ast_idx, TypedIndex value) {
  AstNode*  target_node = ast_get(low->ast, target_ast_idx);
  TypedNode n           = {0};
  n.kind                = TypedNodeKind_SetExpr;
  n.token               = token;
  const char* shape_error = "target must be a plain identifier, `(deref expr)`, `(nth base index)`, "
                            "or a field access (`.`/`get`/`get-in`)";
  if (target_node->kind == AstNodeKind_Atom) {
    n.set_expr.target_kind = SetTargetKind_Identifier;
    n.set_expr.target_name = target_node->token.text;
  } else if (target_node->kind == AstNodeKind_List) {
    u16        tc;
    NodeIndex* tchildren = ast_seq_children(low->ast, target_ast_idx, &tc);
    AstNode*   thead     = (tc > 0) ? ast_get(low->ast, tchildren[0]) : NULL;
    b32        is_deref  = thead && thead->kind == AstNodeKind_Atom && str8_match_lit("deref", thead->token.text, 0);
    b32        is_nth    = thead && thead->kind == AstNodeKind_Atom && str8_match_lit("nth", thead->token.text, 0);
    b32        is_dot    = thead && thead->kind == AstNodeKind_Atom && str8_match_lit(".", thead->token.text, 0);
    b32        is_get    = thead && thead->kind == AstNodeKind_Atom && str8_match_lit("get", thead->token.text, 0);
    b32        is_get_in = thead && thead->kind == AstNodeKind_Atom && str8_match_lit("get-in", thead->token.text, 0);
    if (is_deref && tc == 2) {
      n.set_expr.target_kind = SetTargetKind_Deref;
      n.set_expr.target_expr = lower_expr(low, tchildren[1]); // the pointer itself, not a deref of it
    } else if (is_nth && tc == 3) {
      n.set_expr.target_kind = SetTargetKind_Index;
      lower_nth_target(low, token, tchildren[1], tchildren[2], &n.set_expr.index_base, &n.set_expr.index_index);
    } else if (is_dot || is_get || is_get_in) {
      // The FieldAccess chain these produce, auto-deref per hop included, is
      // checked and codegenned exactly like a read. C already accepts
      // `base.field` as an assignment target, so no lvalue form is needed.
      n.set_expr.target_kind = SetTargetKind_Field;
      if (is_dot)         n.set_expr.target_expr = lower_dot(low, target_ast_idx, tchildren, tc);
      else if (is_get)    n.set_expr.target_expr = lower_get(low, target_ast_idx, tchildren, tc);
      else                n.set_expr.target_expr = lower_get_in(low, target_ast_idx, tchildren, tc);
      if (n.set_expr.target_expr == TYPED_NIL) return TYPED_NIL; // error already reported
    } else {
      lower_error(low, target_node->token, "%s", shape_error);
      return TYPED_NIL;
    }
  } else {
    lower_error(low, target_node->token, "%s", shape_error);
    return TYPED_NIL;
  }
  n.set_expr.value = value;
  return typed_push(low->tast, n);
}

// `(set target value)` -- expands to `target = value`. lower_set_target lists
// the accepted target shapes.
TypedIndex
lower_set(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count) {
  AstNode* node = ast_get(low->ast, idx);
  if (count != 3) {
    lower_error(low, node->token, "`set` requires exactly a target and a value");
    return TYPED_NIL;
  }
  TypedIndex value = lower_expr(low, children[2]);
  return lower_set_target(low, node->token, children[1], value);
}

// Shared core for `++`/`--` and the compound assignments: builds
// `(set target (BINOP target rhs_value))`. Reading `target` a second time for
// the read side is safe, since no accepted target shape has a side effect of
// its own. The write side goes through lower_set_target, so every target shape
// `set` accepts works here too.
TypedIndex
lower_compound_assign(Lowerer* low, Token token, NodeIndex target_ast_idx, TypedIndex rhs_value, TypedNodeKind binop_kind) {
  TypedIndex read_expr = lower_expr(low, target_ast_idx);

  TypedNode binop_node   = {0};
  binop_node.kind        = binop_kind;
  binop_node.token       = token;
  binop_node.binary.lhs  = read_expr;
  binop_node.binary.rhs  = rhs_value;
  TypedIndex binop_idx   = typed_push(low->tast, binop_node);

  return lower_set_target(low, token, target_ast_idx, binop_idx);
}

// `(++ target)` -- expands to `(set target (+ target 1))`.
// `(-- target)` -- expands to `(set target (- target 1))`.
TypedIndex
lower_incdec(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count, TypedNodeKind binop_kind, const char* op_name) {
  AstNode* node = ast_get(low->ast, idx);
  if (count != 2) {
    lower_error(low, node->token, "`%s` requires exactly one target", op_name);
    return TYPED_NIL;
  }
  // infer_from_peer rather than an explicit_type suffix: the user wrote no
  // source text to hang a suffix on, and lowering does not yet know the
  // target's type. See int_lit.infer_from_peer.
  TypedNode one_lit                = {0};
  one_lit.kind                     = TypedNodeKind_IntLiteral;
  one_lit.token                    = node->token;
  one_lit.int_lit.value            = 1;
  one_lit.int_lit.infer_from_peer  = true;
  TypedIndex one_idx    = typed_push(low->tast, one_lit);
  return lower_compound_assign(low, node->token, children[1], one_idx, binop_kind);
}

// `(+= target value)` -- expands to `(set target (+ target value))`, and
// likewise for `-=`/`*=`//=`.
TypedIndex
lower_compound_assign_op(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count, TypedNodeKind binop_kind, const char* op_name) {
  AstNode* node = ast_get(low->ast, idx);
  if (count != 3) {
    lower_error(low, node->token, "`%s` requires exactly a target and a value", op_name);
    return TYPED_NIL;
  }
  TypedIndex rhs_value = lower_expr(low, children[2]);
  return lower_compound_assign(low, node->token, children[1], rhs_value, binop_kind);
}

// `(deref ptr)` -- expands to `*ptr`. Fully type-checked: unwraps one
// pointer level, errors on non-pointer operands (see check_expr).
TypedIndex
lower_unary_deref(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count) {
  AstNode* node = ast_get(low->ast, idx);
  if (count != 2) {
    lower_error(low, node->token,
      "`deref` currently requires exactly one operand");
    return TYPED_NIL;
  }
  TypedIndex expr = lower_expr(low, children[1]);
  TypedNode  n    = {0};
  n.kind          = TypedNodeKind_UnaryDeref;
  n.token         = node->token;
  n.unary.expr    = expr;
  return typed_push(low->tast, n);
}

// `(addr x)` -- expands to `&x`, and the only way to produce a pointer value
// from within the language. `&` is a separate sugar for the field-chain case;
// see lower_addr_field.
TypedIndex
lower_unary_addr(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count) {
  AstNode* node = ast_get(low->ast, idx);
  if (count != 2) {
    lower_error(low, node->token,
      "`addr` currently requires exactly one operand");
    return TYPED_NIL;
  }
  TypedIndex expr = lower_expr(low, children[1]);
  TypedNode  n    = {0};
  n.kind          = TypedNodeKind_UnaryAddr;
  n.token         = node->token;
  n.unary.expr    = expr;
  return typed_push(low->tast, n);
}

// `(not x)` -- expands to `!x`. Any type accepted, same C-style truthiness
// as `if`/`and`/`or`; always produces `bool`.
TypedIndex
lower_not(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count) {
  AstNode* node = ast_get(low->ast, idx);
  if (count != 2) {
    lower_error(low, node->token, "`not` currently requires exactly one operand");
    return TYPED_NIL;
  }
  TypedIndex expr = lower_expr(low, children[1]);
  TypedNode  n    = {0};
  n.kind          = TypedNodeKind_LogicalNot;
  n.token         = node->token;
  n.unary.expr    = expr;
  return typed_push(low->tast, n);
}

// `(bit-not x)` -- expands to `~x`. Unlike `not`, this preserves x's own
// type rather than collapsing to `bool` -- see TypedNodeKind_UnaryBitNot.
TypedIndex
lower_bit_not(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count) {
  AstNode* node = ast_get(low->ast, idx);
  if (count != 2) {
    lower_error(low, node->token, "`bit-not` currently requires exactly one operand");
    return TYPED_NIL;
  }
  TypedIndex expr = lower_expr(low, children[1]);
  TypedNode  n    = {0};
  n.kind          = TypedNodeKind_UnaryBitNot;
  n.token         = node->token;
  n.unary.expr    = expr;
  return typed_push(low->tast, n);
}

// `(- x)` -- negation, expanding to `-x`. lower_dispatch routes two or more
// operands to lower_binary_op instead, so `(- a b)` is still subtraction.
TypedIndex
lower_unary_neg(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count) {
  AstNode* node = ast_get(low->ast, idx);
  if (count != 2) {
    lower_error(low, node->token, "`-` requires exactly one operand here");
    return TYPED_NIL;
  }
  TypedIndex expr = lower_expr(low, children[1]);
  TypedNode  n    = {0};
  n.kind          = TypedNodeKind_UnaryNeg;
  n.token         = node->token;
  n.unary.expr    = expr;
  return typed_push(low->tast, n);
}

// `(+ x)` -- identity, a no-op. As with `-`, lower_dispatch routes two or more
// operands to lower_binary_op.
TypedIndex
lower_unary_pos(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count) {
  AstNode* node = ast_get(low->ast, idx);
  if (count != 2) {
    lower_error(low, node->token, "`+` requires exactly one operand here");
    return TYPED_NIL;
  }
  TypedIndex expr = lower_expr(low, children[1]);
  TypedNode  n    = {0};
  n.kind          = TypedNodeKind_UnaryPos;
  n.token         = node->token;
  n.unary.expr    = expr;
  return typed_push(low->tast, n);
}

// `(cstring s)` -- see TypedNodeKind_CstrExpr's comment for why this
// exists: the only way to turn a 3b `string` literal/value into something
// an extern C function's `i8*`/`(const i8*)` param will actually accept.
TypedIndex
lower_cstring(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count) {
  AstNode* node = ast_get(low->ast, idx);
  if (count != 2) {
    lower_error(low, node->token, "`cstring` requires exactly one operand");
    return TYPED_NIL;
  }
  TypedIndex expr = lower_expr(low, children[1]);
  TypedNode  n    = {0};
  n.kind          = TypedNodeKind_CstrExpr;
  n.token         = node->token;
  n.unary.expr    = expr;
  return typed_push(low->tast, n);
}

// `(string-len s)` -- see TypedNodeKind_StringLenExpr's comment for why
// this exists.
TypedIndex
lower_string_len(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count) {
  AstNode* node = ast_get(low->ast, idx);
  if (count != 2) {
    lower_error(low, node->token, "`string-len` requires exactly one operand");
    return TYPED_NIL;
  }
  TypedIndex expr = lower_expr(low, children[1]);
  TypedNode  n    = {0};
  n.kind          = TypedNodeKind_StringLenExpr;
  n.token         = node->token;
  n.unary.expr    = expr;
  return typed_push(low->tast, n);
}

// `(cast Type value)` -- expands to `(Type)value`. The first operand names a
// type, unlike every other binary form, but lowering still runs lower_expr on
// it: type names never look numeric, so it comes back as a plain Identifier.
// The checker is what treats that lhs as a type rather than a scope lookup.
TypedIndex
lower_binary_cast(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count) {
  AstNode* node = ast_get(low->ast, idx);
  if (count != 3) {
    lower_error(low, node->token,
      "`cast` currently requires exactly two operands");
    return TYPED_NIL;
  }
  TypedIndex lhs = lower_expr(low, children[1]);
  TypedIndex rhs = lower_expr(low, children[2]);
  TypedNode  n   = {0};
  n.kind         = TypedNodeKind_BinaryCast;
  n.token        = node->token;
  n.binary.lhs   = lhs;
  n.binary.rhs   = rhs;
  return typed_push(low->tast, n);
}

// `(reinterpret Type value)` -- the same shape as `cast`, under a different
// TypedNodeKind so the backends give it bit-pattern-copy semantics rather than
// `cast`'s numeric conversion.
TypedIndex
lower_binary_reinterpret(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count) {
  AstNode* node = ast_get(low->ast, idx);
  if (count != 3) {
    lower_error(low, node->token,
      "`reinterpret` currently requires exactly two operands");
    return TYPED_NIL;
  }
  TypedIndex lhs = lower_expr(low, children[1]);
  TypedIndex rhs = lower_expr(low, children[2]);
  TypedNode  n   = {0};
  n.kind         = TypedNodeKind_BinaryReinterpret;
  n.token        = node->token;
  n.binary.lhs   = lhs;
  n.binary.rhs   = rhs;
  return typed_push(low->tast, n);
}

TypedIndex
make_field_access(Lowerer* low, Token token, TypedIndex base, String8 field, Token field_token,
                  b32 auto_deref) {
  TypedNode n = {0};
  n.kind                     = TypedNodeKind_FieldAccess;
  n.token                    = token;
  n.field_access.base        = base;
  n.field_access.field       = field;
  n.field_access.field_token = field_token;
  n.field_access.auto_deref  = auto_deref;
  return typed_push(low->tast, n);
}

// `(get creature health)` -- expands to `creature.health`. The field name is a
// bare atom, as in a struct's declared field list, not a `:keyword`; keywords
// are reserved for literal keys. `get`'s second operand is always a fixed
// member name, never a value, so it cannot be confused with an identifier
// reference.
TypedIndex
lower_get(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count) {
  AstNode* node = ast_get(low->ast, idx);
  if (count != 3) {
    lower_error(low, node->token, "`get` requires exactly a base expression and a field name");
    return TYPED_NIL;
  }
  AstNode* field_node = ast_get(low->ast, children[2]);
  if (field_node->kind != AstNodeKind_Atom) {
    lower_error(low, field_node->token, "`get` field name must be an atom");
    return TYPED_NIL;
  }
  TypedIndex base = lower_expr(low, children[1]);
  return make_field_access(low, node->token, base, field_node->token.text, field_node->token, false);
}

// `(get-in john [parent sister age])` -- sugar over `get`, expanding here into
// nested field accesses. Multi-level chaining therefore needs no handling
// elsewhere: the checker and codegen recurse through it as through any other
// nesting.
TypedIndex
lower_get_in(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count) {
  AstNode* node = ast_get(low->ast, idx);
  if (count != 3) {
    lower_error(low, node->token, "`get-in` requires exactly a base expression and a `[]` vector of field names");
    return TYPED_NIL;
  }
  AstNode* path_node = ast_get(low->ast, children[2]);
  if (path_node->kind != AstNodeKind_Vector) {
    lower_error(low, path_node->token, "`get-in` path must be a vector, e.g. `[parent sister age]`");
    return TYPED_NIL;
  }
  u16        field_count;
  NodeIndex* fields = ast_seq_children(low->ast, children[2], &field_count);
  if (field_count == 0) {
    lower_error(low, path_node->token, "`get-in` path must have at least one field");
    return TYPED_NIL;
  }
  TypedIndex cur = lower_expr(low, children[1]);
  foreach_index(i, field_count) {
    AstNode* field_node = ast_get(low->ast, fields[i]);
    if (field_node->kind != AstNodeKind_Atom) {
      lower_error(low, field_node->token, "`get-in` field name must be an atom");
      return TYPED_NIL;
    }
    cur = make_field_access(low, node->token, cur, field_node->token.text, field_node->token, false);
  }
  return cur;
}

// Shared core for `.` and `&`: builds one auto-deref-enabled
// TypedNodeKind_DotHop per entry in `children[field_start..count)`. These
// forms are variadic, `(. base f1 f2 ...)`, rather than taking `get-in`'s `[]`
// vector.
//
// A hop's meaning depends on the type it lands on -- a struct wants a field
// name, a Map wants a key expression -- which lowering cannot know. Each hop
// therefore carries both candidate interpretations, and the checker picks one
// once the base's type is known. See TypedNodeKind_DotHop.
TypedIndex
lower_field_chain_core(Lowerer* low, Token token, TypedIndex base, NodeIndex* children, u32 field_start, u16 count) {
  TypedIndex cur = base;
  for (u32 i = field_start; i < count; i += 1) {
    AstNode* hop_node = ast_get(low->ast, children[i]);
    String8 field_name_candidate = hop_node->kind == AstNodeKind_Atom ? hop_node->token.text : (String8){0};
    TypedIndex key_expr = lower_expr(low, children[i]); // every hop shape a Map key can take. An
      // identifier-shaped atom becomes an Identifier that is only check_expr'd -- and so only has to
      // resolve -- if the checker picks the Map interpretation.
    TypedNode n          = {0};
    n.kind               = TypedNodeKind_DotHop;
    n.token              = token;
    n.dot_hop.base       = cur;
    n.dot_hop.auto_deref = true;
    n.dot_hop.field_name  = field_name_candidate;
    n.dot_hop.field_token = hop_node->token;
    n.dot_hop.key_expr    = key_expr;
    cur = typed_push(low->tast, n);
  }
  return cur;
}

// `(. base field1 field2 ...)` -- `get`/`get-in` plus two checker-decided
// behaviours per hop: a deref is inserted where the base resolves to a
// pointer, and a hop may resolve to a Map key lookup instead of a struct
// field. Neither happens unless that hop needs it.
//
//   (. vec2 x)          on Vector2*          == (get (deref vec2) x)
//   (. vec2 x)          on Vector2           == (get vec2 x)
//   (. registry "two")  on {string i32}      == (map-get registry "two")
TypedIndex
lower_dot(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count) {
  AstNode* node = ast_get(low->ast, idx);
  if (count < 3) {
    lower_error(low, node->token, "`.` requires a base expression and at least one field name");
    return TYPED_NIL;
  }
  TypedIndex base = lower_expr(low, children[1]);
  return lower_field_chain_core(low, node->token, base, children, 2, count);
}

// `(& base field1 field2 ...)` -- the chain `.` walks, yielding the address of
// the final result: `(& vec2 x)` == `(addr (. vec2 x))`. Address-of with no
// field chain is `(addr x)` instead.
TypedIndex
lower_addr_field(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count) {
  AstNode* node = ast_get(low->ast, idx);
  if (count < 3) {
    lower_error(low, node->token, "`&` requires a base expression and at least one field name");
    return TYPED_NIL;
  }
  TypedIndex base  = lower_expr(low, children[1]);
  TypedIndex chain = lower_field_chain_core(low, node->token, base, children, 2, count);
  if (chain == TYPED_NIL) return TYPED_NIL;
  TypedNode n  = {0};
  n.kind       = TypedNodeKind_UnaryAddr;
  n.token      = node->token;
  n.unary.expr = chain;
  return typed_push(low->tast, n);
}

// Shared by `sizeof`, `alignof` and `type-name`, which differ only in the
// TypedNodeKind -- and so the C construct -- codegen emits.
TypedIndex
lower_type_query(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count,
                  TypedNodeKind kind, const char* form_name) {
  AstNode* node = ast_get(low->ast, idx);
  // `sizeof` and `alignof` take an optional second type that overrides their
  // u64 result -- `(sizeof f32 u32)` -- sparing callers a wrapping `cast`.
  b32 allows_result_type = kind == TypedNodeKind_SizeofExpr || kind == TypedNodeKind_AlignofExpr;
  if (count != 2 && !(allows_result_type && count == 3)) {
    lower_error(low, node->token,
                allows_result_type ? "`%s` requires a type argument and an optional result-type argument"
                                    : "`%s` requires exactly one type argument",
                form_name);
    return TYPED_NIL;
  }
  TypedNode n = {0};
  n.kind            = kind;
  n.token           = node->token;
  n.type_query.type = lower_type_node(low, children[1]);
  if (allows_result_type && count == 3) n.type_query.result_type = lower_type_node(low, children[2]);
  return typed_push(low->tast, n);
}

// `(member-offset StructName field)` -- `field` is an unevaluated name, as it
// is for `.`/`get` and struct-literal keys. Lowering records both pieces
// verbatim; the checker validates the field against the struct.
TypedIndex
lower_member_offset(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count) {
  AstNode* node = ast_get(low->ast, idx);
  if (count != 3) {
    lower_error(low, node->token, "`member-offset` requires exactly a struct type and a field name");
    return TYPED_NIL;
  }
  TypeRef  type       = lower_type_node(low, children[1]);
  AstNode* field_node = ast_get(low->ast, children[2]);
  if (field_node->kind != AstNodeKind_Atom) {
    lower_error(low, field_node->token, "`member-offset`'s field argument must be an atom");
    return TYPED_NIL;
  }
  TypedNode n = {0};
  n.kind                = TypedNodeKind_MemberOffsetExpr;
  n.token                = node->token;
  n.member_offset.type   = type;
  n.member_offset.field  = field_node->token.text;
  return typed_push(low->tast, n);
}

////////////////////////////////
//~ Threading macros: ->, ->>, some->
//
// All three are sugar with no runtime representation. `->` and `->>` expand at
// the AST level, splicing the accumulator into each step's call form and
// lowering the result as if it had been written by hand.
//
// `some->` also needs once-only evaluation with a short-circuit nil check
// between steps, which AST splicing cannot express without re-evaluating each
// step, so it builds let/if/= TypedNodes directly. See
// lower_some_thread_step.

// Splices `acc` into `step`'s call form: a bare atom `f` becomes `(f acc)`; a
// list `(f a b)` becomes `(f acc a b)` or `(f a b acc)` by `insert_first`.
// `acc` is a NodeIndex from a previous step, or a synthesized atom standing in
// for one (lower_some_thread_step). Splicing is purely at the AST level --
// nothing is evaluated or lowered here.
static NodeIndex
thread_step(Lowerer* low, NodeIndex acc, NodeIndex step, b32 insert_first) {
  AstNode* step_node = ast_get(low->ast, step);
  if (step_node->kind == AstNodeKind_Atom) {
    NodeIndex pair[2] = { step, acc };
    return ast_push_seq(low->ast, AstNodeKind_List, step_node->token, pair, 2);
  }
  if (step_node->kind == AstNodeKind_List) {
    u16        sc;
    NodeIndex* schildren = ast_seq_children(low->ast, step, &sc);
    if (sc == 0) {
      lower_error(low, step_node->token, "threading step `()` has no function to call");
      return NODE_NIL;
    }
    ArenaTemp  temp = arena_temp_begin(ctx_scratch());
    NodeIndex* out  = NULL;
    dyn_push(temp.arena, out, schildren[0]); // the function/op head, untouched
    if (insert_first) dyn_push(temp.arena, out, acc);
    for (u32 i = 1; i < sc; i += 1) dyn_push(temp.arena, out, schildren[i]);
    if (!insert_first) dyn_push(temp.arena, out, acc);
    NodeIndex result = ast_push_seq(low->ast, AstNodeKind_List, step_node->token, out, (u16)dyn_count(out));
    arena_temp_end(&temp);
    return result;
  }
  lower_error(low, step_node->token, "threading step must be a function name or a `(f ...)` call form");
  return NODE_NIL;
}

// `(-> x f1 f2 ...)` / `(->> x f1 f2 ...)` -- thread `x` through each step
// as the first (`->`) or last (`->>`) argument. `(-> x)` with no steps is
// just `x`.
TypedIndex
lower_thread(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count, b32 insert_first) {
  AstNode* node = ast_get(low->ast, idx);
  if (count < 2) {
    lower_error(low, node->token, "`%s` requires an initial value", insert_first ? "->" : "->>");
    return TYPED_NIL;
  }
  NodeIndex acc = children[1];
  for (u32 i = 2; i < count; i += 1) {
    acc = thread_step(low, acc, children[i], insert_first);
    if (acc == NODE_NIL) return TYPED_NIL;
  }
  return lower_expr(low, acc);
}

// One layer of `some->`'s expansion:
//   (let [tmp x] (if (= tmp nil) nil <continuation>))
// where the continuation is the next layer, with steps[step_idx] spliced onto
// `tmp` and recursed, or on the last step a plain reference to `tmp`, the
// chain's final value.
//
// The binding's type is left Unresolved on purpose. There is no source-level
// type to write, so this bypasses lower_let and builds the Binding, LetExpr
// and IfExpr nodes directly; the checker's LetExpr case then infers the type
// from the checked initializer rather than validating against it. Only this
// path produces an Unresolved binding -- real `let` syntax always yields a
// resolved type or a lowering error.
//
// The temp reuses one name at every nesting level. That is safe because each
// `let` is its own C block and a step's init is lowered before its binding
// exists, so it resolves to the outer, not-yet-shadowed `tmp`, exactly as
// ordinary nested `let` shadowing does.
static TypedIndex
lower_some_thread_step(Lowerer* low, Token tok, TypedIndex input, NodeIndex* steps, u16 step_idx, u16 step_count) {
  // One name at every nesting level: codegen's scope tracking
  // (cg_scope_reserve/cg_scope_register) disambiguates same-name shadowing.
  String8 tmp_name = str8_lit("__some_thread_val");

  Binding b = {0};
  b.name    = tmp_name;
  b.type    = type_ref_unresolved(); // the checker infers it from `input`
  b.init    = input;
  u32 binding_first = (u32)dyn_count(low->tast->bindings);
  dyn_push(low->tast->arena, low->tast->bindings, b);

  TypedNode tmp_ref_cond = {0};
  tmp_ref_cond.kind       = TypedNodeKind_Identifier;
  tmp_ref_cond.token      = tok;
  tmp_ref_cond.ident.name = tmp_name;
  TypedIndex tmp_cond_idx = typed_push(low->tast, tmp_ref_cond);

  TypedNode nil_node = {0};
  nil_node.kind  = TypedNodeKind_NilLiteral;
  nil_node.token = tok;
  TypedIndex nil_idx = typed_push(low->tast, nil_node);

  TypedNode eq_node  = {0};
  eq_node.kind        = TypedNodeKind_BinaryEq;
  eq_node.token        = tok;
  eq_node.binary.lhs   = tmp_cond_idx;
  eq_node.binary.rhs   = nil_idx;
  TypedIndex cond_idx = typed_push(low->tast, eq_node);

  TypedIndex else_branch;
  if (step_idx == step_count) {
    TypedNode tmp_ref_final = {0};
    tmp_ref_final.kind       = TypedNodeKind_Identifier;
    tmp_ref_final.token      = tok;
    tmp_ref_final.ident.name = tmp_name;
    else_branch = typed_push(low->tast, tmp_ref_final);
  } else {
    Token     tmp_tok  = {0};
    tmp_tok.kind        = TokenKind_Atom;
    tmp_tok.text        = tmp_name;
    tmp_tok.line        = tok.line;
    tmp_tok.col         = tok.col;
    NodeIndex tmp_atom = ast_push_atom(low->ast, tmp_tok);
    NodeIndex spliced  = thread_step(low, tmp_atom, steps[step_idx], /*insert_first=*/true);
    if (spliced == NODE_NIL) return TYPED_NIL;
    TypedIndex next_input = lower_expr(low, spliced);
    else_branch = lower_some_thread_step(low, tok, next_input, steps, (u16)(step_idx + 1), step_count);
    if (else_branch == TYPED_NIL) return TYPED_NIL;
  }

  TypedNode if_node = {0};
  if_node.kind                = TypedNodeKind_IfExpr;
  if_node.token                = tok;
  if_node.if_expr.cond         = cond_idx;
  if_node.if_expr.then_branch  = nil_idx; // a constant, so sharing one node is safe
  if_node.if_expr.else_branch  = else_branch;
  TypedIndex if_idx = typed_push(low->tast, if_node);

  TypedNode block = {0};
  block.kind  = TypedNodeKind_Block;
  block.token = tok;
  u32 body_first = (u32)dyn_count(low->tast->extra);
  dyn_push(low->tast->arena, low->tast->extra, if_idx);
  block.block.stmt_first = body_first;
  block.block.stmt_count = 1;
  TypedIndex body_idx = typed_push(low->tast, block);

  TypedNode let_node = {0};
  let_node.kind                  = TypedNodeKind_LetExpr;
  let_node.token                  = tok;
  let_node.let_expr.binding_first = binding_first;
  let_node.let_expr.binding_count = 1;
  let_node.let_expr.body          = body_idx;
  return typed_push(low->tast, let_node);
}

// `(some-> x f1 f2 ...)` -- `->` that short-circuits to `nil` as soon as `x`
// or a step's result is `nil`. Only chains of pointer-returning steps type
// check, following the language's rule that `nil` is valid only for pointers
// (NilLiteral); a non-pointer step surfaces as an ordinary type error from the
// desugared `(= tmp nil)`.
TypedIndex
lower_some_thread(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count) {
  AstNode* node = ast_get(low->ast, idx);
  if (count < 2) {
    lower_error(low, node->token, "`some->` requires an initial value");
    return TYPED_NIL;
  }
  if (count == 2) return lower_expr(low, children[1]); // no steps -- just `x`
  TypedIndex initial = lower_expr(low, children[1]);
  return lower_some_thread_step(low, node->token, initial, children + 2, 0, (u16)(count - 2));
}

// `[e1 e2 ... eN]` in expression position -- an array literal. The element
// type is not determined here; check_array_literal resolves it against
// whatever the surrounding context expects, such as a `let`/`val`/`var`
// annotation or a struct-literal field type.
TypedIndex
lower_array_literal(Lowerer* low, NodeIndex idx) {
  AstNode*   node = ast_get(low->ast, idx);
  u16        flat_count;
  NodeIndex* flat = ast_seq_children(low->ast, idx, &flat_count);

  ArenaTemp   temp     = arena_temp_begin(ctx_scratch());
  TypedIndex* elements = NULL;
  foreach_index(i, flat_count) {
    TypedIndex elem = lower_expr(low, flat[i]);
    dyn_push(temp.arena, elements, elem);
  }
  u32 first      = (u32)dyn_count(low->tast->array_elements);
  u16 elem_count = (u16)dyn_count(elements);
  foreach_index(i, elem_count) {
    dyn_push(low->tast->arena, low->tast->array_elements, elements[i]);
  }
  arena_temp_end(&temp);

  TypedNode n = {0};
  n.kind                    = TypedNodeKind_ArrayLiteral;
  n.token                   = node->token;
  n.array_lit.element_first = first;
  n.array_lit.element_count = elem_count;
  return typed_push(low->tast, n);
}

TypedIndex
make_index_access(Lowerer* low, Token token, TypedIndex base, TypedIndex index) {
  TypedNode n = {0};
  n.kind                = TypedNodeKind_IndexAccess;
  n.token                = token;
  n.index_access.base    = base;
  n.index_access.index   = index;
  return typed_push(low->tast, n);
}

// `(nth base index)` -- expands to `base[index]`. `index` may be a
// `[i1 i2 ...]` vector for nested arrays: `(nth grid [1 2])` expands here to
// `(nth (nth grid 1) 2)`, as `get-in` does over `get`.
//
// Nothing checks a dimension count. Each level is an ordinary IndexAccess
// under the rules a single-index `nth` already has, so under-indexing yields a
// smaller array as C's own `grid[1]` does, and over-indexing fails once the
// base is no longer an array or pointer. C's `[]` is left-associative, so the
// nested nodes compose into `grid[1][2]` without help from codegen.
TypedIndex
lower_nth(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count) {
  AstNode* node = ast_get(low->ast, idx);
  if (count != 3) {
    lower_error(low, node->token, "`nth` requires exactly a base and an index");
    return TYPED_NIL;
  }
  TypedIndex base_idx = lower_expr(low, children[1]);

  AstNode* index_node = ast_get(low->ast, children[2]);
  if (index_node->kind != AstNodeKind_Vector) {
    TypedIndex single_index = lower_expr(low, children[2]);
    return make_index_access(low, node->token, base_idx, single_index);
  }

  u16        flat_count;
  NodeIndex* flat = ast_seq_children(low->ast, children[2], &flat_count);
  if (flat_count == 0) {
    lower_error(low, index_node->token, "`nth`'s `[]` index list must have at least one index");
    return TYPED_NIL;
  }
  TypedIndex cur = base_idx;
  foreach_index(i, flat_count) {
    TypedIndex index_i = lower_expr(low, flat[i]);
    cur = make_index_access(low, node->token, cur, index_i);
  }
  return cur;
}

TypedIndex
lower_list(Lowerer* low, NodeIndex idx) {
  AstNode*   node = ast_get(low->ast, idx);
  u16        count;
  NodeIndex* children = ast_seq_children(low->ast, idx, &count);
  if (count == 0) {
    lower_error(low, node->token, "empty list `()` is not a valid expression");
    return TYPED_NIL;
  }
  AstNode* head = ast_get(low->ast, children[0]);
  if (head->kind != AstNodeKind_Atom) {
    lower_error(low, node->token, "list head must be an atom naming a keyword/operator/function");
    return TYPED_NIL;
  }
  String8 op = head->token.text;
  if (str8_match_lit("fn", op, 0))     return lower_fn(low, idx, children, count);
  if (str8_match_lit("struct", op, 0)) return lower_struct_decl(low, idx, children, count, false);
  if (str8_match_lit("union", op, 0))  return lower_struct_decl(low, idx, children, count, true);
  if (str8_match_lit("alias", op, 0))  return lower_alias_decl(low, idx, children, count);
  if (str8_match_lit("handle", op, 0)) return lower_handle_pool_decl(low, idx, children, count);
  if (str8_match_lit("handle-pool-init", op, 0))   return lower_handle_pool_init(low, idx, children, count);
  if (str8_match_lit("handle-alloc", op, 0))
    return lower_type_query(low, idx, children, count, TypedNodeKind_HandleAlloc, "handle-alloc");
  if (str8_match_lit("push", op, 0))       return lower_push(low, idx, children, count, false);
  if (str8_match_lit("push0", op, 0))      return lower_push(low, idx, children, count, true);
  if (str8_match_lit("push-zero", op, 0))  return lower_push(low, idx, children, count, true);
  if (str8_match_lit("alloc", op, 0))      return lower_alloc(low, idx, children, count);
  if (str8_match_lit("dyn-push", op, 0))   return lower_dyn_push(low, idx, children, count);
  if (str8_match_lit("vector-push", op, 0)) return lower_dyn_push(low, idx, children, count); // a Vector is a
    // dyn-push-grown pointer (TypeKind_Vector), so this is the same operation under a more
    // discoverable name; the checker's DynPush case accepts a Vector or Pointer target.
  if (str8_match_lit("commit", op, 0))     return lower_commit(low, idx, children, count);
  if (str8_match_lit("enum", op, 0))   return lower_enum_decl(low, idx, children, count, false);
  if (str8_match_lit("flags", op, 0))  return lower_enum_decl(low, idx, children, count, true);
  if (str8_match_lit("val", op, 0))    return lower_val_or_var(low, idx, children, count, false);
  if (str8_match_lit("var", op, 0))    return lower_val_or_var(low, idx, children, count, true);
  if (str8_match_lit("let", op, 0))    return lower_let(low, idx, children, count);
  if (str8_match_lit("scratch", op, 0)) return lower_scratch(low, idx, children, count);
  if (str8_match_lit("parallel", op, 0)) return lower_parallel(low, idx, children, count);
  if (str8_match_lit("parallel-for", op, 0)) return lower_parallel_for(low, idx, children, count);
  if (str8_match_lit("if", op, 0))     return lower_if(low, idx, children, count);
  if (str8_match_lit("when", op, 0))   return lower_when(low, idx, children, count);
  if (str8_match_lit("match", op, 0))  return lower_match(low, idx, children, count);
  if (str8_match_lit("while", op, 0))  return lower_while(low, idx, children, count);
  if (str8_match_lit("for", op, 0))    return lower_for(low, idx, children, count);
  if (str8_match_lit("return", op, 0)) return lower_return(low, idx, children, count);
  if (str8_match_lit("break", op, 0))    return lower_loop_jump(low, idx, count, TypedNodeKind_BreakExpr, "break");
  if (str8_match_lit("continue", op, 0)) return lower_loop_jump(low, idx, count, TypedNodeKind_ContinueExpr, "continue");
  if (str8_match_lit("do", op, 0))     return lower_do(low, idx, children, count);
  if (str8_match_lit("void", op, 0))   return lower_void_do(low, idx, children, count);
  if (str8_match_lit("set", op, 0))    return lower_set(low, idx, children, count);
  if (str8_match_lit("deref", op, 0))  return lower_unary_deref(low, idx, children, count);
  if (str8_match_lit("addr", op, 0))   return lower_unary_addr(low, idx, children, count);
  if (str8_match_lit("not", op, 0))    return lower_not(low, idx, children, count);
  if (str8_match_lit("cstring", op, 0)) return lower_cstring(low, idx, children, count);
  if (str8_match_lit("string-len", op, 0)) return lower_string_len(low, idx, children, count);
  if (str8_match_lit("string-to-i32", op, 0)) return lower_parse_number(low, idx, children, count, TypeKind_I32, "string-to-i32");
  if (str8_match_lit("string-to-i64", op, 0)) return lower_parse_number(low, idx, children, count, TypeKind_I64, "string-to-i64");
  if (str8_match_lit("string-to-u32", op, 0)) return lower_parse_number(low, idx, children, count, TypeKind_U32, "string-to-u32");
  if (str8_match_lit("string-to-u64", op, 0)) return lower_parse_number(low, idx, children, count, TypeKind_U64, "string-to-u64");
  if (str8_match_lit("string-to-f32", op, 0)) return lower_parse_number(low, idx, children, count, TypeKind_F32, "string-to-f32");
  if (str8_match_lit("string-to-f64", op, 0)) return lower_parse_number(low, idx, children, count, TypeKind_F64, "string-to-f64");
  if (str8_match_lit("sqrt-checked", op, 0)) return lower_checked_math(low, idx, children, count, str8_lit("sqrt"), false);
  if (str8_match_lit("asin-checked", op, 0)) return lower_checked_math(low, idx, children, count, str8_lit("asin"), false);
  if (str8_match_lit("acos-checked", op, 0)) return lower_checked_math(low, idx, children, count, str8_lit("acos"), false);
  if (str8_match_lit("pow-checked", op, 0))  return lower_checked_math(low, idx, children, count, str8_lit("pow"), true);
  if (str8_match_lit("cast", op, 0))   return lower_binary_cast(low, idx, children, count);
  if (str8_match_lit("reinterpret", op, 0)) return lower_binary_reinterpret(low, idx, children, count);
  if (str8_match_lit("get", op, 0))    return lower_get(low, idx, children, count);
  if (str8_match_lit("get-in", op, 0)) return lower_get_in(low, idx, children, count);
  if (str8_match_lit("nth", op, 0))    return lower_nth(low, idx, children, count);
  if (str8_match_lit("vector-index-of", op, 0)) return lower_index_of(low, idx, children, count);
  if (str8_match_lit(".", op, 0))      return lower_dot(low, idx, children, count);
  if (str8_match_lit("&", op, 0))      return lower_addr_field(low, idx, children, count);
  if (str8_match_lit("+", op, 0))      return count == 2
                                             ? lower_unary_pos(low, idx, children, count)
                                             : lower_binary_op(low, idx, children, count, TypedNodeKind_BinaryAdd, "+", true);
  if (str8_match_lit("-", op, 0))      return count == 2
                                             ? lower_unary_neg(low, idx, children, count)
                                             : lower_binary_op(low, idx, children, count, TypedNodeKind_BinarySub, "-", true);
  if (str8_match_lit("*", op, 0))      return lower_binary_op(low, idx, children, count, TypedNodeKind_BinaryMul, "*", true);
  if (str8_match_lit("/", op, 0))       return lower_binary_op(low, idx, children, count, TypedNodeKind_BinaryDiv, "/", true);
  if (str8_match_lit("%", op, 0))       return lower_binary_op(low, idx, children, count, TypedNodeKind_BinaryMod, "%", true);
  if (str8_match_lit("bit-or", op, 0))  return lower_binary_op(low, idx, children, count, TypedNodeKind_BinaryBitOr, "bit-or", true);
  if (str8_match_lit("bit-and", op, 0)) return lower_binary_op(low, idx, children, count, TypedNodeKind_BinaryBitAnd, "bit-and", true);
  if (str8_match_lit("bit-xor", op, 0)) return lower_binary_op(low, idx, children, count, TypedNodeKind_BinaryBitXor, "bit-xor", true);
  if (str8_match_lit("bit-shl", op, 0)) return lower_binary_op(low, idx, children, count, TypedNodeKind_BinaryShl, "bit-shl", false);
  if (str8_match_lit("bit-shr", op, 0)) return lower_binary_op(low, idx, children, count, TypedNodeKind_BinaryShr, "bit-shr", false);
  if (str8_match_lit("bit-not", op, 0)) return lower_bit_not(low, idx, children, count);
  if (str8_match_lit("=", op, 0))       return lower_binary_op(low, idx, children, count, TypedNodeKind_BinaryEq, "=", false);
  if (str8_match_lit("!=", op, 0))      return lower_binary_op(low, idx, children, count, TypedNodeKind_BinaryNeq, "!=", false);
  if (str8_match_lit("<", op, 0))       return lower_binary_op(low, idx, children, count, TypedNodeKind_BinaryLt, "<", false);
  if (str8_match_lit("<=", op, 0))      return lower_binary_op(low, idx, children, count, TypedNodeKind_BinaryLe, "<=", false);
  if (str8_match_lit(">", op, 0))       return lower_binary_op(low, idx, children, count, TypedNodeKind_BinaryGt, ">", false);
  if (str8_match_lit(">=", op, 0))      return lower_binary_op(low, idx, children, count, TypedNodeKind_BinaryGe, ">=", false);
  if (str8_match_lit("and", op, 0))     return lower_binary_op(low, idx, children, count, TypedNodeKind_LogicalAnd, "and", true);
  if (str8_match_lit("or", op, 0))      return lower_binary_op(low, idx, children, count, TypedNodeKind_LogicalOr, "or", true);
  if (str8_match_lit("++", op, 0))     return lower_incdec(low, idx, children, count, TypedNodeKind_BinaryAdd, "++");
  if (str8_match_lit("--", op, 0))     return lower_incdec(low, idx, children, count, TypedNodeKind_BinarySub, "--");
  if (str8_match_lit("+=", op, 0))     return lower_compound_assign_op(low, idx, children, count, TypedNodeKind_BinaryAdd, "+=");
  if (str8_match_lit("-=", op, 0))     return lower_compound_assign_op(low, idx, children, count, TypedNodeKind_BinarySub, "-=");
  if (str8_match_lit("*=", op, 0))     return lower_compound_assign_op(low, idx, children, count, TypedNodeKind_BinaryMul, "*=");
  if (str8_match_lit("/=", op, 0))     return lower_compound_assign_op(low, idx, children, count, TypedNodeKind_BinaryDiv, "/=");
  if (str8_match_lit("%=", op, 0))     return lower_compound_assign_op(low, idx, children, count, TypedNodeKind_BinaryMod, "%=");
  if (str8_match_lit("sizeof", op, 0))
    return lower_type_query(low, idx, children, count, TypedNodeKind_SizeofExpr, "sizeof");
  if (str8_match_lit("alignof", op, 0))
    return lower_type_query(low, idx, children, count, TypedNodeKind_AlignofExpr, "alignof");
  if (str8_match_lit("type-name", op, 0))
    return lower_type_query(low, idx, children, count, TypedNodeKind_TypeNameExpr, "type-name");
  if (str8_match_lit("zero", op, 0))
    return lower_type_query(low, idx, children, count, TypedNodeKind_ZeroExpr, "zero");
  if (str8_match_lit("member-offset", op, 0)) return lower_member_offset(low, idx, children, count);
  if (str8_match_lit("->", op, 0))    return lower_thread(low, idx, children, count, true);
  if (str8_match_lit("->>", op, 0))   return lower_thread(low, idx, children, count, false);
  if (str8_match_lit("some->", op, 0)) return lower_some_thread(low, idx, children, count);
  if (is_known_struct_name(low, op))   return lower_struct_construct(low, idx, children, count, op);
  return lower_call(low, idx, children, count); // fallback: an ordinary function call
}

TypedIndex
lower_expr(Lowerer* low, NodeIndex idx) {
  AstNode* node = ast_get(low->ast, idx);
  switch (node->kind) {
    case AstNodeKind_Atom:   return lower_atom(low, idx);
    case AstNodeKind_String: return lower_string(low, idx);
    case AstNodeKind_List:   return lower_list(low, idx);
    case AstNodeKind_Vector: return lower_array_literal(low, idx);
    default: {
      lower_error(low, node->token, "unexpected node kind in expression position");
      return TYPED_NIL;
    }
  }
}

// One real top-level declaration, after flatten_toplevel_forms unwraps any
// enclosing block wrappers. Every pass over the top level -- name gathering,
// alias gathering, the main lowering loop -- walks this flattened list rather
// than the raw AST children, so a wrapped declaration is as visible to all of
// them as an unwrapped one.
typedef struct FlatToplevelForm {
  NodeIndex form;
  b32       is_private;
  b32       is_extern;    // `form` is a bodyless fn signature, lowered via lower_extern_fn
                           // rather than the ordinary lower_expr dispatch
  b32       is_packed;    // `(packed ...)` -- `form` must lower to a StructDecl; see lower_program
  u32       align_bytes;  // `(align N ...)` -- 0 means "no explicit alignment requested"
  b32       is_lane_fn;   // `(lane-fn ...)` -- `form` must lower to a FunctionDecl; see lower_program
} FlatToplevelForm;

// Unwraps the block-wrapper forms, tagging what they contain.
//
// `(private decl ...)` recurses, so `(private (extern ...))` composes, and
// marks everything it unwraps private. `(extern decl ...)` takes bodyless
// `(fn ...)` signatures and is not recursed into, its contents never being
// ordinary top-level forms. Both may appear anywhere and as often as wanted,
// which falls out of simply walking every top-level form and unwrapping what
// is found.
//
// `(packed decl ...)` and `(align N decl ...)` have the same shape and tag
// every struct they unwrap, so `(private (packed (struct ...)))` composes in
// either nesting order. Unlike `private`, these only make sense on a struct --
// lower_program enforces that once it knows what each form lowered to, since
// this function sees only raw AST.
//
// `(lane-fn decl ...)` likewise marks each function it unwraps as callable
// only from inside a `parallel` block or another lane-fn. See checker.c's
// FunctionDecl case. It is restricted to functions the same way packed and
// align are restricted to structs.
static void
flatten_toplevel_forms(Lowerer* low, NodeIndex* children, u16 count,
                       FlatToplevelForm** out, Arena* out_arena, b32 in_private,
                       b32 in_packed, u32 in_align_bytes, b32 in_lane_fn) {
  // dyn_push substitutes its array argument into `arr[_c] = val`, so passing
  // `*out` would expand to `*out[_c]` -- parsed as `*(out[_c])`, not the
  // `(*out)[_c]` needed. Keeping a local means dyn_push only ever sees a plain
  // identifier, and `*out` is synced from it at the end.
  FlatToplevelForm* arr = *out;
  foreach_index(i, count) {
    AstNode* stmt = ast_get(low->ast, children[i]);
    if (stmt->kind == AstNodeKind_List) {
      u16        sc;
      NodeIndex* schildren = ast_seq_children(low->ast, children[i], &sc);
      AstNode*   shead     = (sc > 0) ? ast_get(low->ast, schildren[0]) : NULL;
      b32        is_priv_kw = shead && shead->kind == AstNodeKind_Atom
                              && str8_match_lit("private", shead->token.text, 0);
      b32        is_extern_kw = shead && shead->kind == AstNodeKind_Atom
                                && str8_match_lit("extern", shead->token.text, 0);
      b32        is_packed_kw = shead && shead->kind == AstNodeKind_Atom
                                && str8_match_lit("packed", shead->token.text, 0);
      b32        is_align_kw = shead && shead->kind == AstNodeKind_Atom
                               && str8_match_lit("align", shead->token.text, 0);
      b32        is_lane_fn_kw = shead && shead->kind == AstNodeKind_Atom
                                 && str8_match_lit("lane-fn", shead->token.text, 0);
      if (is_priv_kw) {
        flatten_toplevel_forms(low, schildren + 1, (u16)(sc - 1), &arr, out_arena, true,
                                in_packed, in_align_bytes, in_lane_fn);
        continue;
      }
      if (is_packed_kw) {
        flatten_toplevel_forms(low, schildren + 1, (u16)(sc - 1), &arr, out_arena, in_private,
                                true, in_align_bytes, in_lane_fn);
        continue;
      }
      if (is_align_kw) {
        if (sc < 2) {
          lower_error(low, stmt->token, "`align` requires an alignment (in bytes) before its declarations");
          continue;
        }
        AstNode*        align_node = ast_get(low->ast, schildren[1]);
        NumericAtomInfo align_num  = align_node->kind == AstNodeKind_Atom
                                   ? atom_classify_numeric(align_node->token.text)
                                   : (NumericAtomInfo){0};
        if (!align_num.is_numeric || align_num.is_float) {
          lower_error(low, align_node->token, "`align`'s first argument must be an integer literal, e.g. `(align 16 ...)`");
          continue;
        }
        i64 align_val = 0;
        if (!atom_parse_i64(align_num.body, align_num.is_hex, &align_val)) {
          lower_error(low, align_node->token, "`align` of `%.*s` bytes doesn't fit in 64 bits",
                       str8_varg(align_node->token.text));
          continue;
        }
        if (align_val <= 0) {
          lower_error(low, align_node->token, "`align` must be a positive number of bytes");
          continue;
        }
        flatten_toplevel_forms(low, schildren + 2, (u16)(sc - 2), &arr, out_arena, in_private,
                                in_packed, (u32)align_val, in_lane_fn);
        continue;
      }
      if (is_lane_fn_kw) {
        flatten_toplevel_forms(low, schildren + 1, (u16)(sc - 1), &arr, out_arena, in_private,
                                in_packed, in_align_bytes, true);
        continue;
      }
      if (is_extern_kw) {
        for (u32 j = 1; j < sc; j += 1) {
          FlatToplevelForm f = {0};
          f.form       = schildren[j];
          f.is_private = in_private;
          f.is_extern  = true;
          dyn_push(out_arena, arr, f);
        }
        continue;
      }
    }
    FlatToplevelForm f = {0};
    f.form        = children[i];
    f.is_private   = in_private;
    f.is_packed    = in_packed;
    f.align_bytes  = in_align_bytes;
    f.is_lane_fn   = in_lane_fn;
    dyn_push(out_arena, arr, f);
  }
  *out = arr;
}

// Lowers the implicit top-level list produced by parse_program() into a
// top-level Block of typed top-level forms.
TypedIndex
lower_program(Lowerer* low, NodeIndex root) {
  AstNode*   node = ast_get(low->ast, root);
  u16        count;
  NodeIndex* children = ast_seq_children(low->ast, root, &count);

  ArenaTemp          flat_temp = arena_temp_begin(ctx_scratch());
  FlatToplevelForm*  flat      = NULL;
  flatten_toplevel_forms(low, children, count, &flat, flat_temp.arena, false, false, 0, false);
  u64 flat_count = dyn_count(flat);

  // Pre-pass: gather every struct, union, enum, flags and handle name, so the
  // lowering pass below recognizes `(Name {...})` constructions and
  // `Name/Variant` accesses whether the declaration comes earlier or later in
  // the file. The checker gathers `fn` signatures up front for the same reason.
  foreach_index(i, flat_count) {
    if (flat[i].is_extern) continue; // always a bodyless `fn`, never a struct or enum
    AstNode* stmt = ast_get(low->ast, flat[i].form);
    if (stmt->kind != AstNodeKind_List) continue;
    u16        sc;
    NodeIndex* schildren = ast_seq_children(low->ast, flat[i].form, &sc);
    if (sc < 2) continue;
    AstNode* head = ast_get(low->ast, schildren[0]);
    if (head->kind != AstNodeKind_Atom) continue;
    AstNode* name_node = ast_get(low->ast, schildren[1]);
    if (name_node->kind != AstNodeKind_Atom) continue;
    if (str8_match_lit("struct", head->token.text, 0) || str8_match_lit("union", head->token.text, 0)) {
      lower_register_struct_name(low, name_node->token.text, flat[i].form);
    } else if (str8_match_lit("enum", head->token.text, 0) || str8_match_lit("flags", head->token.text, 0)) {
      lower_register_enum_name(low, name_node->token.text);
    } else if (str8_match_lit("handle", head->token.text, 0)) {
      lower_register_handle_pool_type(low, name_node->token.text);
    }
  }

  // Second pre-pass, which must follow the one above: resolve every
  // `(alias NewName ExistingType)`, since an alias may reference a struct or
  // enum name. Aliases themselves are gathered in source order, so an alias
  // can reference an earlier alias but not a later one.
  foreach_index(i, flat_count) {
    if (flat[i].is_extern) continue;
    AstNode* stmt = ast_get(low->ast, flat[i].form);
    if (stmt->kind != AstNodeKind_List) continue;
    u16        sc;
    NodeIndex* schildren = ast_seq_children(low->ast, flat[i].form, &sc);
    if (sc != 3 && sc != 4) continue; // 4 is the pinned-C-spelling form, which types identically
    AstNode* head = ast_get(low->ast, schildren[0]);
    if (head->kind != AstNodeKind_Atom || !str8_match_lit("alias", head->token.text, 0)) continue;
    AstNode* name_node = ast_get(low->ast, schildren[1]);
    if (name_node->kind != AstNodeKind_Atom) continue;
    TypeAlias alias = {0};
    alias.name      = name_node->token.text;
    alias.type      = lower_type_node(low, schildren[2]);
    lower_register_alias(low, alias);
  }

  ArenaTemp   temp  = arena_temp_begin(ctx_scratch());
  TypedIndex* forms = NULL;
  foreach_index(i, flat_count) {
    TypedIndex t;
    u64 pending_before = dyn_count(low->pending_toplevel);
    if (flat[i].is_extern) {
      AstNode*   inner_node = ast_get(low->ast, flat[i].form);
      if (inner_node->kind != AstNodeKind_List) {
        lower_error(low, inner_node->token, "`extern` block entries must be `(fn ...)` signatures");
        continue;
      }
      u16        ic;
      NodeIndex* ichildren = ast_seq_children(low->ast, flat[i].form, &ic);
      AstNode*   ihead     = (ic > 0) ? ast_get(low->ast, ichildren[0]) : NULL;
      if (!ihead || ihead->kind != AstNodeKind_Atom || !str8_match_lit("fn", ihead->token.text, 0)) {
        lower_error(low, inner_node->token, "`extern` block entries must be `(fn ...)` signatures");
        continue;
      }
      t = lower_extern_fn(low, flat[i].form, ichildren, ic);
    } else {
      t = lower_expr(low, flat[i].form);
    }
    // Anything synthesized at any nesting depth while lowering this form --
    // see lower_anon_struct_type -- is spliced in immediately before it, so
    // codegen's source-order emission sees an embedded type declared first.
    for (u64 pi = pending_before; pi < dyn_count(low->pending_toplevel); pi += 1) {
      dyn_push(temp.arena, forms, low->pending_toplevel[pi]);
    }
    if (t != TYPED_NIL && flat[i].is_private) {
      low->tast->nodes[t].is_private = true;
    }
    if (t != TYPED_NIL && (flat[i].is_packed || flat[i].align_bytes > 0)) {
      if (low->tast->nodes[t].kind != TypedNodeKind_StructDecl) {
        lower_error(low, low->tast->nodes[t].token, "`packed`/`align` can only wrap a `struct` declaration");
      } else {
        low->tast->nodes[t].struct_decl.is_packed   = flat[i].is_packed;
        low->tast->nodes[t].struct_decl.align_bytes = flat[i].align_bytes;
      }
    }
    if (t != TYPED_NIL && flat[i].is_lane_fn) {
      if (low->tast->nodes[t].kind != TypedNodeKind_FunctionDecl) {
        lower_error(low, low->tast->nodes[t].token, "`lane-fn` can only wrap a `fn` declaration");
      } else {
        low->tast->nodes[t].func.is_lane_fn = true;
      }
    }
    dyn_push(temp.arena, forms, t);
  }
  u32 first = (u32)dyn_count(low->tast->extra);
  u16 form_count = (u16)dyn_count(forms);
  foreach_index(i, form_count) {
    dyn_push(low->tast->arena, low->tast->extra, forms[i]);
  }
  arena_temp_end(&temp);
  arena_temp_end(&flat_temp);

  TypedNode n        = {0};
  n.kind             = TypedNodeKind_Block;
  n.token            = node->token;
  n.block.stmt_first = first;
  n.block.stmt_count = form_count;
  return typed_push(low->tast, n);
}

////////////////////////////////
//~ Typed AST debug printer

// The TypedAst counterpart of parser.c's ast_print: the two together dump what
// each of the first two stages produced, which is how a lowering bug is
// normally found -- the node kind, the resolved type, and the token position
// per node, indented by depth.
//
// It lived in test/demo.c until that file was deleted, which made it reachable
// only from `3b test`. Nothing about printing a TypedAst is test-specific, and
// the pass that BUILDS the structure is the honest owner of the code that
// renders it.

void
typed_ast_print(TypedAst* tast, TypedIndex idx, u32 depth) {
  if (idx == TYPED_NIL) {
    foreach_index(i, depth) { printf("  "); }
    printf("<nil>\n");
    return;
  }
  TypedNode* n = &tast->nodes[idx];
  foreach_index(i, depth) { printf("  "); }
  switch (n->kind) {
    case TypedNodeKind_FunctionDecl: {
      printf("%s%sFunctionDecl `%.*s` -> %.*s  (%u:%u)\n",
             n->is_private ? "(private) " : "", n->func.body == TYPED_NIL ? "(extern) " : "",
             str8_varg(n->func.name), str8_varg(type_ref_display(ctx_scratch(), n->func.return_type)),
             n->token.line, n->token.col);
      foreach_index(i, n->func.param_count) {
        Param* p = &tast->params[n->func.param_first + i];
        foreach_index(j, depth + 1) { printf("  "); }
        printf("param `%.*s`: %.*s\n", str8_varg(p->name), str8_varg(type_ref_display(ctx_scratch(), p->type)));
      }
      if (n->func.body != TYPED_NIL) typed_ast_print(tast, n->func.body, depth + 1);
    } break;
    case TypedNodeKind_ConstDecl: {
      printf("%sConstDecl `%.*s`: %.*s  (%u:%u)\n", n->is_private ? "(private) " : "",
             str8_varg(n->const_decl.name), str8_varg(type_ref_display(ctx_scratch(), n->const_decl.type)),
             n->token.line, n->token.col);
      if (n->const_decl.init != TYPED_NIL) typed_ast_print(tast, n->const_decl.init, depth + 1);
    } break;
    case TypedNodeKind_VarDecl: {
      printf("%sVarDecl `%.*s`: %.*s  (%u:%u)\n", n->is_private ? "(private) " : "",
             str8_varg(n->var_decl.name), str8_varg(type_ref_display(ctx_scratch(), n->var_decl.type)),
             n->token.line, n->token.col);
      if (n->var_decl.init != TYPED_NIL) typed_ast_print(tast, n->var_decl.init, depth + 1);
    } break;
    case TypedNodeKind_LetExpr: {
      printf("LetExpr [%u bindings]  (%u:%u)\n", (u32)n->let_expr.binding_count, n->token.line, n->token.col);
      foreach_index(i, n->let_expr.binding_count) {
        Binding* b = &tast->bindings[n->let_expr.binding_first + i];
        foreach_index(j, depth + 1) { printf("  "); }
        printf("binding `%.*s`: %.*s =\n", str8_varg(b->name), str8_varg(type_ref_display(ctx_scratch(), b->type)));
        typed_ast_print(tast, b->init, depth + 2);
      }
      typed_ast_print(tast, n->let_expr.body, depth + 1);
    } break;
    case TypedNodeKind_StructDecl: {
      printf("%sStructDecl `%.*s`  (%u:%u)\n", n->is_private ? "(private) " : "",
             str8_varg(n->struct_decl.name), n->token.line, n->token.col);
      foreach_index(i, n->struct_decl.field_count) {
        Param* f = &tast->params[n->struct_decl.field_first + i];
        foreach_index(j, depth + 1) { printf("  "); }
        printf("field `%.*s`: %.*s\n", str8_varg(f->name), str8_varg(type_ref_display(ctx_scratch(), f->type)));
      }
    } break;
    case TypedNodeKind_AliasDecl: {
      printf("%sAliasDecl `%.*s` = %.*s  (%u:%u)\n", n->is_private ? "(private) " : "",
             str8_varg(n->alias_decl.name),
             str8_varg(type_ref_display(ctx_scratch(), n->alias_decl.type)), n->token.line, n->token.col);
    } break;
    case TypedNodeKind_StructLiteral: {
      printf("StructLiteral `%.*s`  (%u:%u)\n", str8_varg(n->struct_lit.type_name), n->token.line, n->token.col);
      foreach_index(i, n->struct_lit.field_count) {
        FieldInit* fi = &tast->field_inits[n->struct_lit.field_first + i];
        foreach_index(j, depth + 1) { printf("  "); }
        printf(":%.*s =\n", str8_varg(fi->name));
        typed_ast_print(tast, fi->value, depth + 2);
      }
    } break;
    case TypedNodeKind_PushAlloc: {
      printf("PushAlloc%s : %.*s  (%u:%u)\n", n->push_alloc.zeroed ? " (zeroed)" : "",
             str8_varg(type_ref_display(ctx_scratch(), n->push_alloc.elem_type)), n->token.line, n->token.col);
      foreach_index(j, depth + 1) { printf("  "); }
      printf("arena =\n");
      typed_ast_print(tast, n->push_alloc.arena, depth + 2);
      foreach_index(j, depth + 1) { printf("  "); }
      printf("count =\n");
      typed_ast_print(tast, n->push_alloc.count, depth + 2);
    } break;
    case TypedNodeKind_PushCopy: {
      printf("PushCopy  (%u:%u)\n", n->token.line, n->token.col);
      foreach_index(j, depth + 1) { printf("  "); }
      printf("arena =\n");
      typed_ast_print(tast, n->push_copy.arena, depth + 2);
      foreach_index(j, depth + 1) { printf("  "); }
      printf("value =\n");
      typed_ast_print(tast, n->push_copy.value, depth + 2);
    } break;
    case TypedNodeKind_AllocExpr: {
      printf("AllocExpr : %.*s  (%u:%u)\n",
             str8_varg(type_ref_display(ctx_scratch(), n->alloc_expr.elem_type)), n->token.line, n->token.col);
      foreach_index(j, depth + 1) { printf("  "); }
      printf("count =\n");
      typed_ast_print(tast, n->alloc_expr.count, depth + 2);
    } break;
    case TypedNodeKind_EnumDecl: {
      printf("%s%s `%.*s`  (%u:%u)\n", n->is_private ? "(private) " : "",
             n->enum_decl.is_flags ? "FlagsDecl" : "EnumDecl",
             str8_varg(n->enum_decl.name), n->token.line, n->token.col);
      foreach_index(i, n->enum_decl.variant_count) {
        EnumVariant* v = &tast->enum_variants[n->enum_decl.variant_first + i];
        foreach_index(j, depth + 1) { printf("  "); }
        if (v->has_explicit_value) {
          printf("variant `%.*s` = %lld\n", str8_varg(v->name), (long long)v->value);
        } else {
          printf("variant `%.*s` (auto)\n", str8_varg(v->name));
        }
      }
    } break;
    case TypedNodeKind_EnumAccess: {
      printf("EnumAccess `%.*s/%.*s`  (%u:%u)\n", str8_varg(n->enum_access.enum_name),
             str8_varg(n->enum_access.variant_name), n->token.line, n->token.col);
    } break;
    case TypedNodeKind_SetExpr: {
      if (n->set_expr.target_kind == SetTargetKind_Deref) {
        printf("SetExpr (deref target) =  (%u:%u)\n", n->token.line, n->token.col);
        typed_ast_print(tast, n->set_expr.target_expr, depth + 1);
      } else if (n->set_expr.target_kind == SetTargetKind_Index) {
        printf("SetExpr (nth target) =  (%u:%u)\n", n->token.line, n->token.col);
        typed_ast_print(tast, n->set_expr.index_base, depth + 1);
        typed_ast_print(tast, n->set_expr.index_index, depth + 1);
      } else {
        printf("SetExpr `%.*s` =  (%u:%u)\n", str8_varg(n->set_expr.target_name), n->token.line, n->token.col);
      }
      typed_ast_print(tast, n->set_expr.value, depth + 1);
    } break;
    case TypedNodeKind_ArrayLiteral: {
      printf("ArrayLiteral [%u elements]  (%u:%u)\n", (u32)n->array_lit.element_count, n->token.line, n->token.col);
      foreach_index(i, n->array_lit.element_count) {
        typed_ast_print(tast, tast->array_elements[n->array_lit.element_first + i], depth + 1);
      }
    } break;
    case TypedNodeKind_IndexAccess: {
      printf("IndexAccess  (%u:%u)\n", n->token.line, n->token.col);
      typed_ast_print(tast, n->index_access.base, depth + 1);
      typed_ast_print(tast, n->index_access.index, depth + 1);
    } break;
    case TypedNodeKind_PositionalAccess: {
      printf("PositionalAccess [slot %u]  (%u:%u)\n", n->positional_access.slot, n->token.line, n->token.col);
      typed_ast_print(tast, n->positional_access.base, depth + 1);
    } break;
    case TypedNodeKind_DotHop: {
      printf("DotHop%s .%.*s  (%u:%u)\n", n->dot_hop.auto_deref ? "(auto-deref)" : "",
             str8_varg(n->dot_hop.field_name), n->token.line, n->token.col);
      typed_ast_print(tast, n->dot_hop.base, depth + 1);
      foreach_index(j, depth + 1) { printf("  "); }
      printf("key_expr =\n");
      typed_ast_print(tast, n->dot_hop.key_expr, depth + 2);
    } break;
    case TypedNodeKind_ParseNumber: {
      TypeRef target_ty = {0};
      target_ty.kind    = n->parse_number.target_kind;
      printf("ParseNumber -> %.*s  (%u:%u)\n", str8_varg(type_ref_display(ctx_scratch(), target_ty)), n->token.line, n->token.col);
      typed_ast_print(tast, n->parse_number.arg, depth + 1);
    } break;
    case TypedNodeKind_CheckedMath: {
      printf("CheckedMath `%.*s-checked`  (%u:%u)\n", str8_varg(n->checked_math.libm_name), n->token.line, n->token.col);
      typed_ast_print(tast, n->checked_math.arg, depth + 1);
      if (n->checked_math.arg2 != TYPED_NIL) typed_ast_print(tast, n->checked_math.arg2, depth + 1);
    } break;
    case TypedNodeKind_UnaryDeref: {
      printf("UnaryDeref  (%u:%u)\n", n->token.line, n->token.col);
      typed_ast_print(tast, n->unary.expr, depth + 1);
    } break;
    case TypedNodeKind_UnaryAddr: {
      printf("UnaryAddr  (%u:%u)\n", n->token.line, n->token.col);
      typed_ast_print(tast, n->unary.expr, depth + 1);
    } break;
    case TypedNodeKind_LogicalNot: {
      printf("LogicalNot  (%u:%u)\n", n->token.line, n->token.col);
      typed_ast_print(tast, n->unary.expr, depth + 1);
    } break;
    case TypedNodeKind_ReturnExpr: {
      printf("ReturnExpr  (%u:%u)\n", n->token.line, n->token.col);
      typed_ast_print(tast, n->unary.expr, depth + 1);
    } break;
    case TypedNodeKind_BinaryCast: {
      printf("BinaryCast  (%u:%u)\n", n->token.line, n->token.col);
      typed_ast_print(tast, n->binary.lhs, depth + 1);
      typed_ast_print(tast, n->binary.rhs, depth + 1);
    } break;
    case TypedNodeKind_FieldAccess: {
      printf("FieldAccess%s .%.*s  (%u:%u)\n", n->field_access.auto_deref ? "(auto-deref)" : "",
             str8_varg(n->field_access.field), n->token.line, n->token.col);
      typed_ast_print(tast, n->field_access.base, depth + 1);
    } break;
    case TypedNodeKind_IfExpr: {
      printf("IfExpr  (%u:%u)\n", n->token.line, n->token.col);
      typed_ast_print(tast, n->if_expr.cond, depth + 1);
      typed_ast_print(tast, n->if_expr.then_branch, depth + 1);
      if (n->if_expr.else_branch != TYPED_NIL) {
        typed_ast_print(tast, n->if_expr.else_branch, depth + 1);
      }
    } break;
    case TypedNodeKind_WhileExpr: {
      printf("WhileExpr  (%u:%u)\n", n->token.line, n->token.col);
      typed_ast_print(tast, n->while_expr.cond, depth + 1);
      typed_ast_print(tast, n->while_expr.body, depth + 1);
    } break;
    case TypedNodeKind_ForRangeExpr: {
      printf("ForRangeExpr `%.*s`  (%u:%u)\n", str8_varg(n->for_range.var_name), n->token.line, n->token.col);
      foreach_index(j, depth + 1) { printf("  "); }
      printf("begin =\n");
      typed_ast_print(tast, n->for_range.begin, depth + 2);
      foreach_index(j, depth + 1) { printf("  "); }
      printf("end =\n");
      typed_ast_print(tast, n->for_range.end, depth + 2);
      foreach_index(j, depth + 1) { printf("  "); }
      printf("step =\n");
      typed_ast_print(tast, n->for_range.step, depth + 2);
      typed_ast_print(tast, n->for_range.body, depth + 1);
    } break;
    case TypedNodeKind_BinaryAdd:
    case TypedNodeKind_BinarySub:
    case TypedNodeKind_BinaryMul:
    case TypedNodeKind_BinaryDiv:
    case TypedNodeKind_BinaryBitOr:
    case TypedNodeKind_BinaryBitAnd:
    case TypedNodeKind_BinaryEq:
    case TypedNodeKind_BinaryNeq:
    case TypedNodeKind_BinaryLt:
    case TypedNodeKind_BinaryLe:
    case TypedNodeKind_BinaryGt:
    case TypedNodeKind_BinaryGe:
    case TypedNodeKind_LogicalAnd:
    case TypedNodeKind_LogicalOr: {
      printf("BinaryOp `%s`  (%u:%u)\n", binary_op_symbol(n->kind), n->token.line, n->token.col);
      typed_ast_print(tast, n->binary.lhs, depth + 1);
      typed_ast_print(tast, n->binary.rhs, depth + 1);
    } break;
    case TypedNodeKind_Call: {
      printf("Call `%.*s`  (%u:%u)\n", str8_varg(n->call.callee), n->token.line, n->token.col);
      foreach_index(i, n->call.arg_count) {
        typed_ast_print(tast, tast->extra[n->call.arg_first + i], depth + 1);
      }
    } break;
    case TypedNodeKind_Identifier: {
      printf("Identifier `%.*s`  (%u:%u)\n", str8_varg(n->ident.name), n->token.line, n->token.col);
    } break;
    case TypedNodeKind_IntLiteral: {
      printf("IntLiteral %lld : i32  (%u:%u)\n", (long long)n->int_lit.value, n->token.line, n->token.col);
    } break;
    case TypedNodeKind_FloatLiteral: {
      printf("FloatLiteral %g : f32  (%u:%u)\n", (double)n->float_lit.value, n->token.line, n->token.col);
    } break;
    case TypedNodeKind_StringLiteral: {
      printf("StringLiteral \"%.*s\"  (%u:%u)\n", str8_varg(n->string_lit.value), n->token.line, n->token.col);
    } break;
    case TypedNodeKind_NilLiteral: {
      printf("NilLiteral  (%u:%u)\n", n->token.line, n->token.col);
    } break;
    case TypedNodeKind_BoolLiteral: {
      printf("BoolLiteral %s : bool  (%u:%u)\n", n->bool_lit.value ? "true" : "false", n->token.line, n->token.col);
    } break;
    case TypedNodeKind_Block: {
      printf("Block [%u stmts]  (%u:%u)\n", (u32)n->block.stmt_count, n->token.line, n->token.col);
      foreach_index(i, n->block.stmt_count) {
        typed_ast_print(tast, tast->extra[n->block.stmt_first + i], depth + 1);
      }
    } break;
    default: {
      printf("Nil\n");
    } break;
  }
}
