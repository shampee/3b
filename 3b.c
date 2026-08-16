#include "3b.h"

// Returns TypeKind_Named as a sentinel meaning "not a recognized primitive".
static TypeKind
primitive_type_kind_from_name(String8 name) {
  if (str8_match_lit("i8", name, 0))     return TypeKind_I8;
  if (str8_match_lit("i16", name, 0))    return TypeKind_I16;
  if (str8_match_lit("i32", name, 0))    return TypeKind_I32;
  if (str8_match_lit("i64", name, 0))    return TypeKind_I64;
  if (str8_match_lit("u8", name, 0))     return TypeKind_U8;
  if (str8_match_lit("u16", name, 0))    return TypeKind_U16;
  if (str8_match_lit("u32", name, 0))    return TypeKind_U32;
  if (str8_match_lit("u64", name, 0))    return TypeKind_U64;
  if (str8_match_lit("f32", name, 0))    return TypeKind_F32;
  if (str8_match_lit("f64", name, 0))    return TypeKind_F64;
  if (str8_match_lit("bool", name, 0))   return TypeKind_Bool;
  if (str8_match_lit("char", name, 0))   return TypeKind_Char;
  if (str8_match_lit("string", name, 0)) return TypeKind_String;
  if (str8_match_lit("void", name, 0))   return TypeKind_Void;
  if (str8_match_lit("any", name, 0))    return TypeKind_Any;
  if (str8_match_lit("arena", name, 0))     return TypeKind_Arena;
  if (str8_match_lit("ArenaMark", name, 0)) return TypeKind_ArenaMark;
  if (str8_match_lit("stream", name, 0))    return TypeKind_Stream;
  return TypeKind_Named;
}

b32
is_primitive_type_name(String8 name) {
  return primitive_type_kind_from_name(name) != TypeKind_Named;
}

// `*` is not a lexer delimiter, so `i32*` (or `Creature**`) lexes as a single
// atom; this peels trailing `*` characters off one at a time and wraps
// recursively. Each pointer level boxes its pointee into `arena`, so callers
// pass ctx_perm() here: types outlive the pass that created them.
TypeRef
type_ref_from_atom(Arena* arena, String8 name) {
  // `^` is checked first and only once: handles are single-level, no `T^^` or
  // `T^*`, unlike pointers' arbitrary depth. The backing struct name stays a
  // plain String8 in .name rather than being boxed in .pointee -- there is
  // nothing to recurse into. Whether `name` really names a handle-pooled struct
  // is validated later, by lower_type_node; this helper has no Lowerer or
  // Checker to consult and only builds the TypeRef shape.
  if (name.size > 0 && name.str[name.size - 1] == '^') {
    TypeRef t = {0};
    t.kind    = TypeKind_Handle;
    t.name    = str8_chop(name, 1);
    return t;
  }
  if (name.size > 0 && name.str[name.size - 1] == '*') {
    String8 inner_name = str8_chop(name, 1); // drop the trailing '*'
    TypeRef inner       = type_ref_from_atom(arena, inner_name);
    TypeRef* boxed       = push_one(arena, TypeRef);
    *boxed               = inner;
    TypeRef t = {0};
    t.kind    = TypeKind_Pointer;
    t.pointee = boxed;
    return t;
  }
  TypeRef  t = {0};
  TypeKind k = primitive_type_kind_from_name(name);
  if (k == TypeKind_Named) {
    t.kind = TypeKind_Named;
    t.name = name;
  } else {
    t.kind = k;
  }
  return t;
}
String8
type_ref_display(Arena* arena, TypeRef t) {
  // TypeRef.is_const carries real meaning (type_ref_assignable's
  // const-compatibility check in checker.c), so an error message rejecting a
  // `const char*` where a plain `char*` was expected has to show the
  // difference, or it reads as "expected char*, got char*". The recursive calls
  // below re-enter this same function, so a pointee's own is_const is picked up
  // automatically -- the same wrap-then-recurse shape codegen.c's
  // c_type_from_typeref relies on.
  if (t.is_const) {
    TypeRef unqualified = t;
    unqualified.is_const = false;
    return str8f(arena, "const %.*s", str8_varg(type_ref_display(arena, unqualified)));
  }
  switch (t.kind) {
    case TypeKind_Unresolved: return str8_lit("<inferred>");
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
    case TypeKind_Bool:       return str8_lit("bool");
    case TypeKind_Char:       return str8_lit("char");
    case TypeKind_String:     return str8_lit("string");
    case TypeKind_Void:       return str8_lit("void");
    case TypeKind_Any:        return str8_lit("any");
    case TypeKind_Arena:      return str8_lit("arena");
    case TypeKind_ArenaMark:  return str8_lit("ArenaMark");
    case TypeKind_Stream:     return str8_lit("stream");
    case TypeKind_Pointer: {
      if (t.pointee == NULL) return str8_lit("nil"); // the untyped/wildcard nil pointer
      String8 inner = type_ref_display(arena, *t.pointee);
      return str8f(arena, "%.*s*", str8_varg(inner));
    }
    case TypeKind_Handle:     return str8f(arena, "%.*s^", str8_varg(t.name));
    // Array, Vector, Map, Set and Fn all mirror source syntax exactly.
    case TypeKind_Array: {
      String8 inner = type_ref_display(arena, *t.pointee);
      return str8f(arena, "[%.*s %llu]", str8_varg(inner), (unsigned long long)t.count);
    }
    case TypeKind_Vector: {
      String8 inner = type_ref_display(arena, *t.pointee);
      return str8f(arena, "(Vector %.*s)", str8_varg(inner));
    }
    case TypeKind_Map: {
      String8 key = type_ref_display(arena, *t.map_key);
      String8 val = type_ref_display(arena, *t.pointee);
      return str8f(arena, "(Map %.*s %.*s)", str8_varg(key), str8_varg(val));
    }
    case TypeKind_Set: {
      String8 inner = type_ref_display(arena, *t.pointee);
      return str8f(arena, "(Set %.*s)", str8_varg(inner));
    }
    case TypeKind_Named:      return t.name;
    case TypeKind_Fn: {
      String8 ret    = type_ref_display(arena, *t.fn_return);
      String8 params = str8_lit("");
      foreach_index(i, t.fn_param_count) {
        String8 p = type_ref_display(arena, t.fn_params[i]);
        params    = (i == 0) ? p : str8f(arena, "%.*s %.*s", str8_varg(params), str8_varg(p));
      }
      return str8f(arena, "(fn [%.*s] %.*s)", str8_varg(params), str8_varg(ret));
    }
  }
  return str8_lit("?");
}

// See the declaration in 3b.h. type_ref_display already gives a deterministic,
// self-delimiting spelling for any TypeRef; this sanitizes it byte for byte
// down to alnum-or-underscore, since that spelling can contain `(`, `)`, `*`,
// `[`, `]` and spaces, none of them valid in a C identifier. Not injective in
// the pathological case where two different displayed strings fold to the same
// text -- the same caveat anon_param_field_signature carries.
String8
hashtable_mangled_name(Arena* arena, TypeRef key_type, TypeRef* value_type) {
  String8 key_disp = type_ref_display(ctx_scratch(), key_type);
  String8 raw       = value_type
    ? str8f(ctx_scratch(), "Map_%.*s_%.*s", str8_varg(key_disp), str8_varg(type_ref_display(ctx_scratch(), *value_type)))
    : str8f(ctx_scratch(), "Set_%.*s", str8_varg(key_disp));
  u8* buf = push_array(arena, u8, raw.size == 0 ? 1 : raw.size);
  foreach_index(i, raw.size) {
    u8 c = raw.str[i];
    b32 alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    buf[i] = alnum ? c : '_';
  }
  return str8(buf, raw.size);
}

// Shared by checker.c, validating `=`/`!=`/`<`/`<=`/`>`/`>=` on a struct or
// string operand, and codegen.c, deciding whether a struct needs synthesized
// `_eq`/`_cmp` helpers (see cg_emit_struct_comparators). Takes a plain
// `StructEntry*` dyn array rather than a `Checker*`/`Codegen*`, since both
// carry one in this shape and codegen has no Checker to call into.
//
// One set of rules serves equality and ordering alike: whatever composes into a
// bare C `==` also composes into `<`/`<=` field by field. Numerics, bool, char,
// pointers and any other TypeKind_Named -- assumed to be an enum, since a
// struct name would have been found in `structs` -- are all scalar compares.
// `string` goes through bbb_str8_match/bbb_str8_compare; a fixed-size array is
// comparable if its element type recursively is; a struct is comparable if it
// is not a `union` (ambiguous which member is active), has no C11 anonymous
// member (no flat field list to walk), and every one of its own fields
// recursively passes. Vector, Map, Set, Handle and Fn match no branch here and
// fall through to the final `return false`, making any struct holding one
// incomparable.
//
// `depth` exists only to terminate on a struct that contains itself by value
// (`(struct A [x A])`, or any longer cycle). That is not a legal type -- the
// checker rejects it, see check_struct_cycles -- but this runs on the same
// typed AST the checker was handed, and both backends walk every struct
// unconditionally, so it cannot assume the check already ran and passed.
// A cycle-free nesting chain can visit each struct at most once, so any chain
// longer than `struct_count` has revisited one; bailing out as "not
// comparable" there is conservative and costs nothing on well-formed input.
static b32
type_ref_is_deep_comparable_rec(TypedAst* tast, StructEntry* structs, u64 struct_count, TypeRef t, u64 depth) {
  if (type_kind_is_numeric(t.kind)) return true;
  if (t.kind == TypeKind_Bool || t.kind == TypeKind_Char || t.kind == TypeKind_Pointer) return true;
  if (t.kind == TypeKind_String) return true;
  if (t.kind == TypeKind_Array) return type_ref_is_deep_comparable_rec(tast, structs, struct_count, *t.pointee, depth);
  if (t.kind != TypeKind_Named) return false;
  TypedIndex decl_idx = TYPED_NIL;
  foreach_index(i, struct_count) {
    if (str8_match(structs[i].name, t.name, 0)) { decl_idx = structs[i].decl; break; }
  }
  if (decl_idx == TYPED_NIL) return true; // not a registered struct -- must be an enum, plain `==`-comparable
  if (depth > struct_count) return false; // cyclic by-value nesting -- see the note above
  TypedNode* decl = &tast->nodes[decl_idx];
  if (decl->struct_decl.is_union) return false;
  foreach_index(i, decl->struct_decl.field_count) {
    Param* f = &tast->params[decl->struct_decl.field_first + i];
    if (f->is_anon) return false;
    if (!type_ref_is_deep_comparable_rec(tast, structs, struct_count, f->type, depth + 1)) return false;
  }
  return true;
}

b32
type_ref_is_deep_comparable(TypedAst* tast, StructEntry* structs, u64 struct_count, TypeRef t) {
  return type_ref_is_deep_comparable_rec(tast, structs, struct_count, t, 0);
}

void
typed_ast_init(TypedAst* tast, Arena* arena) {
  tast->arena          = arena;
  tast->nodes          = NULL;
  tast->extra          = NULL;
  tast->params         = NULL;
  tast->bindings       = NULL;
  tast->field_inits    = NULL;
  tast->enum_variants  = NULL;
  tast->array_elements = NULL;
  tast->type_annotations = NULL;
  TypedNode nil = {0};
  nil.kind      = TypedNodeKind_Nil;
  dyn_push(tast->arena, tast->nodes, nil); // reserve index 0
}

TypedIndex
typed_push(TypedAst* tast, TypedNode node) {
  dyn_push(tast->arena, tast->nodes, node);
  return (TypedIndex)(dyn_count(tast->nodes) - 1);
}

const char*
binary_op_symbol(TypedNodeKind kind) {
  switch (kind) {
    case TypedNodeKind_BinaryAdd:    return "+";
    case TypedNodeKind_BinarySub:    return "-";
    case TypedNodeKind_BinaryMul:    return "*";
    case TypedNodeKind_BinaryDiv:    return "/";
    case TypedNodeKind_BinaryMod:    return "%";
    case TypedNodeKind_BinaryBitOr:  return "|";
    case TypedNodeKind_BinaryBitAnd: return "&";
    case TypedNodeKind_BinaryBitXor: return "^";
    case TypedNodeKind_BinaryShl:    return "<<";
    case TypedNodeKind_BinaryShr:    return ">>";
    case TypedNodeKind_BinaryEq:     return "==";
    case TypedNodeKind_BinaryNeq:    return "!=";
    case TypedNodeKind_BinaryLt:     return "<";
    case TypedNodeKind_BinaryLe:     return "<=";
    case TypedNodeKind_BinaryGt:     return ">";
    case TypedNodeKind_BinaryGe:     return ">=";
    case TypedNodeKind_LogicalAnd:   return "&&";
    case TypedNodeKind_LogicalOr:    return "||";
    default:                         return "?";
  }
}

const char*
binary_op_display_name(TypedNodeKind kind) {
  switch (kind) {
    case TypedNodeKind_BinaryBitOr:  return "bit-or";
    case TypedNodeKind_BinaryBitAnd: return "bit-and";
    case TypedNodeKind_BinaryBitXor: return "bit-xor";
    case TypedNodeKind_BinaryShl:    return "bit-shl";
    case TypedNodeKind_BinaryShr:    return "bit-shr";
    case TypedNodeKind_BinaryEq:     return "=";
    case TypedNodeKind_LogicalAnd:   return "and";
    case TypedNodeKind_LogicalOr:    return "or";
    default:                         return binary_op_symbol(kind); // everyone else: source keyword == C symbol
  }
}
