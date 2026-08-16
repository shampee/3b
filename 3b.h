#ifndef BBB_H
#define BBB_H
#include "base/base.h"

////////////////////////////////
//~ Version

// Reported by `3b --version`. The only version number in the tree, and the
// one thing a bug report can name a build by -- the on-disk bytecode cache
// carries its OWN independent format version (BC_CACHE_VERSION in bcio.c), not
// tied to this one, so a compiler release that doesn't change the bytecode
// format doesn't invalidate every cached `.3bc`.
#define BBB_VERSION "0.1.0"

////////////////////////////////
//~ Tokens

typedef enum TokenKind {
  TokenKind_EOF = 0,
  TokenKind_LParen,
  TokenKind_RParen,
  TokenKind_LBracket, // '[' -- a Vector: bindings, param lists
  TokenKind_RBracket,
  TokenKind_LBrace,   // '{' -- a Map: key/value literals
  TokenKind_RBrace,
  TokenKind_Atom,
  TokenKind_String,
  TokenKind_Error,
} TokenKind;

typedef struct Token {
  TokenKind kind;
  String8   text; // Atom: raw text. String: decoded contents, no quotes. Brackets/EOF: unused.
  u32       line;
  u32       col;
  u32       file_id; // indexes the source-file registry below; SOURCE_FILE_UNKNOWN for
                     // synthetic tokens never lexed from a real file
} Token;

////////////////////////////////
//~ Diagnostics
//
// A source-file registry plus "path:line:col" error printing that quotes and
// underlines the offending line. Every Token carries the file_id it came
// from, so any error function holding a Token -- type_error, lower_error,
// the parser's own -- gets this by routing through diag_error.

typedef struct SourceFile {
  String8 path;
  String8 text; // full contents, arena-allocated and kept alive for the whole
                // process so a diagnostic can quote any line at any later point
} SourceFile;

#define SOURCE_FILE_UNKNOWN 0 // registered at id 0, so a Token without a real
                              // file_id (e.g. one built by hand in a test)
                              // still prints something sane

u32         source_file_register(String8 path, String8 text);
SourceFile* source_file_get(u32 file_id);
// Clears the registry so a long-lived host (lib3b.h) can call into the
// compiler repeatedly without file_ids outliving the arena they were
// registered in.
void        source_registry_reset(void);

void diag_errorv(Token tok, const char* fmt, va_list args);
void diag_error(Token tok, const char* fmt, ...);

// One captured diagnostic: what diag_errorv prints, as data rather than
// stderr text. See diag_capture_begin/diag_capture_end.
typedef struct Diagnostic {
  String8 message;   // fully formatted, no ANSI color codes
  String8 file_path;
  u32     line;
  u32     col;
} Diagnostic;

// Collects every diag_error/diag_errorv call into a dyn array instead of
// printing it, or alongside printing when also_print is true. Resets any
// previously captured list. Call sites need no changes; capture is additive.
void        diag_capture_begin(b32 also_print);
// Stops capturing and returns the collected list, allocated on ctx_perm() and
// valid until the caller's ctx_free(). Writes the length to *out_count.
Diagnostic* diag_capture_end(u64* out_count);

////////////////////////////////
//~ Package kind
//
// Shared by compiler.h (PackageBuild.kind, which gates whether a root
// package's `fn main` becomes the C entry point) and build.h
// (BuildConfig.kind, parsed from build.cfg.3b). It lives here so neither
// header has to depend on the other for one enum.
typedef enum PackageKind {
  PackageKind_Binary = 0, // default; a zeroed BuildConfig or absent manifest means Binary
  PackageKind_Library,
} PackageKind;

////////////////////////////////
//~ Source overlays
//
// Substitutes in-memory content for the named files; everything else in the
// package still reads from disk. compile_package consults this list, matched
// by filename, before its own file_load_str8 call. This exists for an LSP
// (lib3b.h) wanting diagnostics against unsaved edits without writing to disk.
typedef struct SourceOverlay {
  String8 path;    // absolute path of the file being overridden
  String8 content; // in-memory buffer text to use instead of the file's
} SourceOverlay;

////////////////////////////////
//~ Lexer

typedef struct Lexer {
  String8 src;
  u64     pos;
  u32     line;
  u32     col;
  u32     file_id;
} Lexer;

void  lexer_init(Lexer* lex, String8 src, u32 file_id);
u8    lexer_peek(Lexer* lex);
u8    lexer_advance(Lexer* lex);
void  lexer_skip_ignorable(Lexer* lex);
Token lexer_read_string(Lexer* lex, Arena* dst_arena, u32 line, u32 col);
Token lexer_next(Lexer* lex, Arena* dst_arena);

////////////////////////////////
//~ AST
//
// The whole grammar:
//   expr := atom | string | list
//   list := '(' expr* ')'
//
// The parser is context-free: it knows nothing about `fn`, `if`, or `let`,
// and builds only a generic tree of atoms, strings, and lists. Special forms
// are recognized later, during lowering.
//
// Representation:
//   - AstNode lives in one contiguous arena-backed dyn array. Nodes are
//     referenced by index (NodeIndex), never by pointer.
//   - Index 0 is a reserved Nil sentinel, mirroring base.h's HandlePool
//     convention of reserving slot 0 as null.
//   - A List's children are not sibling AstNodes -- recursive parsing leaves
//     true siblings non-contiguous as soon as one child is itself a list.
//     Instead a List stores first_child/child_count into `Ast.extra`, a
//     second flat array of child NodeIndex values. AstNode stays fixed-size
//     and the child index list stays contiguous. Zig's `extra_data` array
//     uses the same technique.
//
// Numbers are ordinary atoms at this level; there is no numeric token kind.
// Classifying one as i32 or f32 is a semantic question, handled by atom.c
// rather than the parser. Comments are `;` to end of line, per Lisp.

typedef u32 NodeIndex;
#define NODE_NIL ((NodeIndex)0)

typedef enum AstNodeKind {
  AstNodeKind_Nil = 0, // reserved sentinel occupying index 0
  AstNodeKind_Atom,
  AstNodeKind_String,
  AstNodeKind_List,
  AstNodeKind_Vector, // [...] -- bindings, param lists
  AstNodeKind_Map,    // {...} -- key/value literals
} AstNodeKind;

// first_child/child_count index into Ast.extra -- see the section comment.
typedef struct AstNode {
  AstNodeKind kind;
  u32         first_child;
  u16         child_count;
  Token       token;
} AstNode;

typedef struct Ast {
  Arena*    arena;
  AstNode*  nodes; // arena-backed dynamic array (dyn_push); index 0 = Nil
  u32*      extra; // arena-backed dynamic array of child NodeIndex values
} Ast;

void      ast_init(Ast* ast, Arena* arena);
NodeIndex ast_push_node(Ast* ast, AstNode node);
NodeIndex ast_push_atom(Ast* ast, Token tok);
NodeIndex ast_push_string(Ast* ast, Token tok);
NodeIndex ast_push_seq(Ast* ast, AstNodeKind kind, Token open_tok, NodeIndex* children, u16 count);

////////////////////////////////
//~ Parser
//
// Knows only `expr := atom | string | list`. It has no idea `fn`, `if`, or
// `let` exist.

typedef struct Parser {
  Lexer  lex;
  Token  cur;
  Ast*   ast;
  Arena* arena;
  b32    had_error;
} Parser;

void      parser_init(Parser* p, String8 src, Ast* ast, u32 file_id);
void      parser_advance(Parser* p);
NodeIndex parse_bracketed(Parser* p, TokenKind close_kind, const char* close_str, AstNodeKind node_kind);
NodeIndex parse_expr(Parser* p);
NodeIndex parse_program(Parser* p);

const char* ast_node_kind_name(AstNodeKind kind);
void        ast_print(Ast* ast, NodeIndex idx, u32 depth);

////////////////////////////////
//~ Atom

b32 atom_looks_numeric(String8 atom);
b32 atom_is_float_literal(String8 atom);
b32 atom_is_hex_literal(String8 atom);

// Null-terminates a String8 into scratch memory so it can be passed to libc
// functions like strtof/strtoll. Safe when the source is a non-terminated
// slice into a larger buffer, as an atom lexed from a file always is.
static inline const char*
cstr_from_str8_temp(String8 s) {
  char* buf = push_array(ctx_scratch(), char, s.size + 1);
  MemoryCopy(buf, s.str, s.size);
  buf[s.size] = 0;
  return buf;
}

////////////////////////////////
//~ In-memory FILE* streams
//
// A portable stand-in for open_memstream, which is a glibc/BSD extension
// unavailable on Windows. Backed by tmpfile(), which is plain ANSI C.
// Codegen buffers whole function bodies and parallel-block preludes this way
// when it needs to hoist (cg_function, cg_parallel_phase_fn); none of that is
// a hot path, so a real temp file costs nothing that matters.

static inline FILE*
mem_stream_open(void) {
  FILE* f = tmpfile();
  if (!f) { fprintf(stderr, "mem_stream_open(): tmpfile() failed\n"); exit(1); }
  return f;
}

// Reads everything written to `f` into a fresh heap buffer, then closes `f`.
static inline void
mem_stream_close(FILE* f, char** out_buf, u64* out_size) {
  fflush(f);
  long size = ftell(f);
  if (size < 0) size = 0;
  rewind(f);
  char* buf = (char*)malloc((u64)size + 1);
  u64   n   = fread(buf, 1, (u64)size, f);
  buf[n] = 0;
  fclose(f);
  *out_buf  = buf;
  *out_size = n;
}

////////////////////////////////
//~ Types
//
// Lowering records whatever annotation the source gave; checker.c resolves
// the rest. TypeKind_Unresolved means "not annotated here", not "unknown
// forever".

typedef enum TypeKind {
  TypeKind_Unresolved = 0, // no annotation given; the checker fills it in
  TypeKind_I8,
  TypeKind_I16,
  TypeKind_I32,
  TypeKind_I64,
  TypeKind_U8,
  TypeKind_U16,
  TypeKind_U32,
  TypeKind_U64,
  TypeKind_F32,
  TypeKind_F64,
  TypeKind_Bool,
  TypeKind_Char,
  TypeKind_String,
  TypeKind_Void,
  TypeKind_Any,       // `any` -- an untyped pointer, C's `void*`
  TypeKind_Arena,     // `arena` -- base.h's `Arena`. A primitive rather than a Named struct
                      // because it is two pointers copied by value everywhere, a handle whose
                      // mutable state lives behind `.backend`. `arena*` never appears at the
                      // language level.
  TypeKind_ArenaMark, // `ArenaMark` -- a saved arena cursor, produced by `mark` and consumed by
                      // `pop`. Capitalized rather than named like the `arena` primitive it
                      // accompanies, since `mark` is too common a word to reserve as a keyword.
                      // Still resolved here rather than through the struct table; there is no
                      // user-facing declaration for it.
  TypeKind_Stream,    // `stream` -- an open byte stream (a file or one of the three standard
                      // streams), runtime/bbb_file.h's `bbb_Stream`. A primitive for the same
                      // reason `arena` is: a by-value handle whose state lives behind the
                      // pointer. Opaque -- no cast, no field access, no printing OF a stream,
                      // though `print`/`println` can print TO one. Its operations live in the
                      // `os` module. Being a primitive rather than a struct declared in `os` is
                      // also what lets it cross a package boundary, since struct types don't
                      // splice between packages.
  TypeKind_Pointer,   // `T*` -- wraps a TypeRef via .pointee, to arbitrary depth
  TypeKind_Handle,    // `T^` -- a generational index into T's handle pool, base.h's `T##Handle`
                      // `{index, generation}` pair. T's name lives in .name, not .pointee:
                      // handles are single-level (no `T^^`) and always name a struct with a
                      // `(handle T)` declaration. See checker.c's handle_pool_types.
  TypeKind_Array,     // `[ElementType Count]` -- fixed size, element type in .pointee plus
                      // .count. Written bracketed rather than as an atom suffix like `i32*`,
                      // because `[` and `]` are lexer delimiters and so can't be part of one
                      // atom; this reuses the Vector AST node instead.
  TypeKind_Vector,    // `(Vector ElementType)` -- a growable array, element type in .pointee.
                      // Compiles to a bare `ElementType*` grown by dyn-push, with no runtime
                      // representation of its own: Vector is a compile-time safety wrapper over
                      // the pointer, closing the hole where `dyn-count` and `commit` otherwise
                      // have to trust that a pointer really was dyn-push-grown.
  TypeKind_Map,       // `(Map KeyType ValueType)` -- a monomorphized open-addressing hash table.
                      // ValueType is in .pointee, KeyType in .map_key; Map is the only kind
                      // needing two type arguments. Compiles to a synthesized C struct plus
                      // set/get/remove/contains functions, interned per (KeyType, ValueType) so
                      // identical instantiations in one package share a definition -- see
                      // lower_hashtable_type. KeyType is restricted to numeric primitives and
                      // `string`; hashing anything else is a lowering error. ValueType is
                      // unrestricted.
  TypeKind_Set,       // `(Set ElementType)` -- a Map with no value slot. ElementType is in
                      // .pointee, under the same key-type restriction as Map.
  TypeKind_Named,     // a user-defined type referenced by name
  TypeKind_Fn,        // `(fn [name type ...] ReturnType)` in type position -- a function
                      // pointer. Reuses the parameter-vector shape of a real `fn` declaration;
                      // names are accepted for readability and discarded, since only the types
                      // are part of a function type's identity. A bare top-level `fn` name used
                      // as a value also resolves to this kind, which needs nothing special from
                      // codegen -- a C function name already decays to a pointer.
} TypeKind;

// Recursive through a pointer rather than a nested value, so TypeRef stays
// small enough to pass by value everywhere it appears (Param.type,
// Binding.type, return types). Boxed members are allocated on ctx_perm(),
// the same lifetime as the rest of the typed program.
//
// A Pointer with a NULL pointee is `nil`'s type: a wildcard compatible with
// any other pointer type. Making it a NULL pointee rather than a TypeKind of
// its own means every existing comparison -- return types, initializers,
// parameter matching, set targets, struct fields, `=`/`!=` -- accepts nil
// through type_ref_equal with no additional plumbing.
typedef struct TypeRef TypeRef;
struct TypeRef {
  TypeKind kind;
  String8  name;      // Named and Handle only
  TypeRef* pointee;   // Pointer, Array, Vector, Set: the pointee or element type.
                      // Map: the VALUE type. NULL under Pointer means `nil`.
  TypeRef* map_key;   // Map only: the key type
  u64      count;     // Array only
  String8  alias_name; // when non-empty, codegen spells the type this way instead of by its
                      // computed name, so `(alias newi32 i32)` reads as `newi32` at its use
                      // sites in the generated C. Never consulted for type compatibility;
                      // type_ref_equal ignores it.
  b32      is_const;  // from `(const T)` in type position. Like alias_name, purely a codegen
                      // concern -- it emits a C `const` and nothing checks it. Enough to match
                      // a C API's declared signature when translating headers, not real
                      // const-correctness.
  TypeRef* fn_params; // Fn only: array of each parameter's type, length fn_param_count, NULL
                      // when there are none. A function type's identity is its parameter types
                      // alone, so these are bare TypeRefs rather than Params.
  u32      fn_param_count;
  TypeRef* fn_return; // Fn only: boxed return type, never NULL when well-formed
};

TypeRef type_ref_from_atom(Arena* arena, String8 name);
String8 type_ref_display(Arena* arena, TypeRef t);
b32     is_primitive_type_name(String8 name); // i8..f64, bool, char, string, void, any, arena, ArenaMark
b32     type_ref_equal(TypeRef a, TypeRef b); // defined in checker.c

// Deterministic C-identifier-safe name for one Map or Set instantiation --
// "Map_string_i32", "Set_i32". A NULL value_type means Set. Shared by lower.c,
// which interns the synthesized declaration under this name, and codegen.c's
// c_type_from_typeref, which must reproduce it exactly from the TypeRef alone.
String8 hashtable_mangled_name(Arena* arena, TypeRef key_type, TypeRef* value_type);

// Full classification of a numeric atom, computed once and shared by literal
// lowering and the contexts that accept only an integer literal and ignore
// its suffix: array counts, `align`, enum variant values.
typedef struct NumericAtomInfo {
  b32      is_numeric;
  b32      is_float;
  b32      is_hex;
  TypeKind explicit_type; // Unresolved when the atom carries no i8/u32/f64 suffix
  String8  body;          // the atom with any type suffix stripped, ready for strtoll/strtod
} NumericAtomInfo;

NumericAtomInfo atom_classify_numeric(String8 atom);

// Read a classified integer `body` (see NumericAtomInfo.body) into a value.
// False means the literal is out of the type's range, or has a malformed
// tail -- callers report that rather than using a clamped result. See
// atom.c's own note on why every integer atom goes through these.
b32 atom_parse_i64(String8 body, b32 is_hex, i64* out);
b32 atom_parse_u64(String8 body, b32 is_hex, u64* out);

////////////////////////////////
//~ Typed AST
//
// Same contiguous-arena, index-referenced design as the generic AST above.
// TypedNode lives in one dyn array with index 0 reserved as Nil, and
// variable-length lists -- a Block's statements, a Call's arguments -- are
// contiguous runs of TypedIndex in `TypedAst.extra`, exactly like Ast.extra.
// Parameters, bindings, field initializers, and enum variants each get their
// own side array for the same reason.
//
// The tree carries no scoping or symbol resolution of its own; checker.c
// supplies those in a separate pass over it.

typedef u32 TypedIndex;
#define TYPED_NIL ((TypedIndex)0)

typedef struct Param {
  String8 name;
  Token   name_token; // where the name itself was written, for LSP hover and goto-definition on a
                      // field or parameter reference. SOURCE_FILE_UNKNOWN for a compiler-
                      // synthesized entry (a multi-return shape's `_0`, an anonymous parameter
                      // struct's private name), which has no source position to point at.
  TypeRef type;
  b32     is_anon; // struct and union fields only: `_` in source, as in
                   // `(union Vec2 [_ (struct [x f32 y f32]) v [f32 2]])`. `name` still
                   // holds the literal `_`, but this flag is the marker. The field's type
                   // must resolve to a struct or union, and its fields are reachable
                   // directly on the outer value with no extra path segment -- C11
                   // anonymous member semantics. See find_field_recursive (checker.c) and
                   // cg_emit_anon_member_body (codegen.c).
  b32     is_read; // function parameters only: filled in by the checker, see ScopeEntry.was_read.
                   // An unused C parameter draws no -Wall warning, so the one consumer is
                   // cg_function_main, which turns main's two into real locals.
} Param;

// A `let` binding, written `[name init ...]` or `[name type init ...]`. `let`
// is the only binding form with type inference -- see checker.c's LetExpr case
// for why. `type` is Unresolved for an unannotated binding, a destructured
// local, or a compiler-synthesized one, and the checker fills it in from the
// checked initializer in all three cases. `val`, `var`, and parameters never
// come through here; their types are always explicit.
typedef struct Binding {
  String8    name;
  TypeRef    type;
  TypedIndex init;
  b32        is_read; // filled in by the checker; see ScopeEntry.was_read
} Binding;

// One `:field value` entry in a struct literal, as in
// `(Creature {:name "Orc" :health 50})`. Order need not match the struct's
// declared field order: codegen emits C99 designated initializers.
typedef struct FieldInit {
  String8    name; // field name, with the leading ':' already stripped
  TypedIndex value;
} FieldInit;

// One variant of an `enum` or `flags` declaration -- `Directional`, or
// `NotFound 404`. Without an explicit value, codegen assigns one: sequential
// for `enum`, the next bit position for `flags`. With one, `value` is used as
// given and auto-assignment resumes from there.
typedef struct EnumVariant {
  String8 name;
  b32     has_explicit_value;
  i64     value;
} EnumVariant;

// Which shape `(set TARGET value)`'s target takes. Field covers `.`, `get`,
// and `get-in` targets, reusing those forms' own auto-deref field-chain
// lowering (make_field_access) rather than making the caller write
// `(set (deref (& base field)) value)` by hand.
typedef enum SetTargetKind {
  SetTargetKind_Identifier,
  SetTargetKind_Deref,
  SetTargetKind_Index,
  SetTargetKind_Field,
} SetTargetKind;

typedef enum TypedNodeKind {
  TypedNodeKind_Nil = 0,
  TypedNodeKind_FunctionDecl,
  TypedNodeKind_ConstDecl,     // `val` -- module-level, explicitly typed, immutable
  TypedNodeKind_VarDecl,       // `var` -- module-level, explicitly typed, mutable
  TypedNodeKind_LetExpr,       // `let` -- local, mutable, type optional (see Binding)
  TypedNodeKind_ScratchExpr,   // `(scratch [t] body...)` -- opens a temp-arena scope over the
                               // thread-local scratch arena, binds `t` to it for the body, and
                               // rewinds unconditionally when the block ends. Exactly
                               // arena_temp_begin/arena_temp_end as a language construct. Not a
                               // general binding form: one name, never user-initialized, and the
                               // region cannot be ended early. The block's value is its body's
                               // last expression, captured before the rewind -- so returning a
                               // value computed inside is fine, while returning a pointer INTO
                               // `t` dangles, as unchecked as any other dangling pointer here.
                               //
                               // "Unconditionally" includes leaving through `return`, `break` or
                               // `continue` rather than off the end. That does not come for
                               // free: the rewind is emitted after the body, so a jump would
                               // sail straight past it and strand the arena at its high-water
                               // mark until something unrelated reset it. Both backends
                               // therefore track the scratch scopes open around the expression
                               // being emitted and unwind them, innermost first, ahead of the
                               // jump -- codegen.c's Codegen.scratch_depth plus
                               // cg_unwind_scratch_scopes, and bcgen.c's BcFnCtx.scratch_scopes
                               // plus bc_unwind_scratch_scopes.
                               //
                               // How FAR to unwind differs by jump. `return` leaves the function
                               // and unwinds every open scope. `break`/`continue` leave only the
                               // innermost loop, so they stop at the depth that loop started at
                               // (Codegen.loop_scratch_depth, BcLoopCtx.scratch_mark): a
                               // `scratch` WRAPPING the loop is still live afterwards, and
                               // rewinding it here would both free memory still in use and let
                               // its own end-of-block rewind run twice. A jump form added later
                               // has to pick its floor the same way.
                               // test/golden/control_flow.3b pins the emitted shape and
                               // examples/nested-scratch checks the arena really does come back.
  TypedNodeKind_HashTableInstanceDecl, // synthesized, never written in source: one distinct
                               // `(Map K V)` or `(Set T)` instantiation, interned by its type
                               // arguments so identical ones share a definition. Spliced into
                               // pending_toplevel the first time lower_type_node sees a new
                               // combination. Codegen emits a key-specific hash and equality
                               // pair plus one bbb_DEFINE_HASHMAP/HASHSET invocation, so this
                               // bypasses the ordinary struct forward-decl and topo-sort
                               // machinery; the splice-before-trigger ordering handles it.
  TypedNodeKind_HandlePoolDecl, // `(handle Name)` -- registers `Name^` as a handle type backed
                               // by a pool of `Name` structs. Emits base.h's
                               // DEFINE_HANDLE_POOL plus a pool-storage global. Storage is
                               // always private to the declaring package's .c regardless of
                               // this declaration's own visibility, since only functions
                               // compiled into that .c touch it.
  TypedNodeKind_HandlePoolInit, // `(handle-pool-init Name capacity arena)` -- one-time pool
                               // setup. Capacity is fixed here; base.h's DEFINE_HANDLE_POOL is
                               // arena-allocated once and never resized.
  TypedNodeKind_HandleAlloc,   // `(handle-alloc Name)` -- takes a slot from Name's pool and
                               // returns `Name^`. Shares the `type_query` union member with
                               // sizeof and zero, being likewise "one type argument, nothing
                               // else".
  TypedNodeKind_StructDecl,    // `(struct Name [field type ...])`
  TypedNodeKind_AliasDecl,     // `(alias NewName ExistingType)` -- name first, the reverse of
                               // C's typedef, matching the name-then-type order used for
                               // val/var/params/fields. Emits a real C typedef; see TypeRef's
                               // alias_name for how the name reaches its use sites.
  TypedNodeKind_StructLiteral, // `(Name {:field value ...})`
  TypedNodeKind_ArrayLiteral,  // `[e1 e2 ... eN]` in expression position. Unlike every other
                               // literal it is not self-typed -- the source carries no element
                               // type -- so it can only appear where the surrounding context
                               // supplies an expected type: a let/val/var annotation or a
                               // struct-literal field. Anywhere else is a checker error rather
                               // than a lowering one. See check_array_literal.
  TypedNodeKind_IndexAccess,   // `(nth base index)` -- `base[index]`, over an Array or a
                               // Pointer, both being equally valid to index in C
  TypedNodeKind_PositionalAccess, // `[a b] source` destructuring, one slot. A placeholder that
                               // never reaches codegen: lowering has no types yet, so it cannot
                               // know whether slot N means declared field N or index N. The
                               // checker resolves `base`'s type and rewrites this node in place
                               // -- struct to FieldAccess, array or pointer to IndexAccess --
                               // so codegen needs no case for this kind.
  TypedNodeKind_DotHop,        // one hop of `(. base f1 f2 ...)` or `(& base f1 f2 ...)`.
                               // Another placeholder that never reaches codegen, for the same
                               // reason as PositionalAccess: a hop is a compile-time struct
                               // field name if `base` is a struct, and a runtime Map key
                               // expression if it is a Map. Lowering records both readings --
                               // field_name, set only when the hop was a plain identifier, and
                               // key_expr, always a lowered expression -- and the checker
                               // rewrites the node once `base`'s type is known: to FieldAccess
                               // for a struct, or to a `map-get` Call for a Map. It does not
                               // re-run check_expr on the rewritten node, since `base` would be
                               // checked twice and the cost would compound along a multi-hop
                               // chain; it inlines just the validation it needs instead.
  TypedNodeKind_ParseNumber,   // `(string-to-i32 s)` and its i64/u32/u64/f32/f64 siblings,
                               // returning `(bool T)`. False for anything malformed -- empty
                               // input, a lone sign, trailing garbage, out-of-range magnitude
                               // -- never a silent zero. The result is an ordinary multi-return
                               // struct synthesized (or reused, through the shape-interning
                               // table) at lowering time, since only lowering can splice a new
                               // StructDecl into the program. `result_struct_name` records
                               // which one, as the checker never sees the interning table.
  TypedNodeKind_IndexOf,       // `(vector-index-of v x)` -- linear search returning
                               // `(bool u64)`. The second field is always u64 regardless of
                               // element type, so lowering can intern this one shape up front.
                               // The element type must be type_ref_is_comparable, validated
                               // once the checker resolves it -- the same gate `=` and
                               // `vector-contains?` use.
  TypedNodeKind_CheckedMath,   // `(sqrt-checked x)`, `(asin-checked x)`, `(acos-checked x)`,
                               // `(pow-checked base exp)` -- the domain-restricted libm
                               // functions, returning `(bool T)` where ok is
                               // `isfinite(result)`. That catches a negative sqrt, an
                               // out-of-range asin or acos, or a negative base with a
                               // fractional exponent, all of which the unchecked call would
                               // silently return NaN or Inf for. sin, cos, tan, cbrt, floor and
                               // friends are total over the finite floats and get no `-checked`
                               // form. T follows the argument, so lowering interns both the
                               // `(bool f32)` and `(bool f64)` shapes and the checker picks by
                               // the argument's resolved type.
  TypedNodeKind_PushAlloc,     // `(push arena Type)` / `(push arena Type Count)` -- one kind,
                               // with the countless form synthesizing a literal 1.
                               // `push0`/`push-zero` are the same kind with `zeroed` set.
  TypedNodeKind_PushCopy,      // `(push arena value)` -- arena-allocates one element sized to
                               // `value`'s checked type and copies it in. The third `push`
                               // shape, told apart from PushAlloc at lowering time by whether
                               // the second argument names a known type.
  TypedNodeKind_DynPush,       // `(dyn-push arena arr value)` -- base.h's amortized-growth
                               // array append, a different operation from `push`: `push` gives
                               // you a known size immediately, `dyn-push` grows by doubling and
                               // wastes the intermediate allocations until a later `commit`.
                               // `arr` must be a bare mutable local, not a general lvalue,
                               // because base.h's dyn_push macro reseats it on every growth.
                               // Always void -- see cg_dyn_push.
  TypedNodeKind_CommitExpr,    // `(commit dst-arena src)` -- copies a dyn-push-grown array into
                               // `dst-arena` at its exact size, dropping the growth slack.
                               // Does not use base.h's dyn_commit_from_temp, which calls
                               // arena_temp_end on the empty path and would double-end the
                               // region when `commit` runs inside a `scratch` block.
                               // cg_commit_expr does the copy from lower-level primitives so
                               // `commit` has no lifetime side effects of its own.
  TypedNodeKind_EnumDecl,      // `(enum Name [...])` / `(flags Name [...])` -- structurally
                               // identical, differing only in default value assignment
  TypedNodeKind_EnumAccess,    // `Name/Variant` -- a scoped constant reference. Its own kind
                               // rather than an Identifier, since a scoped constant and a
                               // scope-bound variable are different things and the distinction
                               // should not have to be re-derived from the name's text.
  TypedNodeKind_SetExpr,       // `(set target value)` -- `target = value`
  TypedNodeKind_UnaryDeref,    // `(deref ptr)` -- `*ptr`
  TypedNodeKind_UnaryAddr,     // `(addr x)` -- `&x`
  TypedNodeKind_LogicalNot,    // `(not x)` -- `!x`. Accepts any type, with C's truthiness, and
                               // always produces bool.
  TypedNodeKind_UnaryBitNot,   // `(bit-not x)` -- `~x`. Word-form like bit-or and bit-and.
                               // Unlike `not`, preserves x's type rather than producing bool.
  TypedNodeKind_UnaryNeg,      // `(- x)` -- `-(x)`, parenthesized so nesting cannot merge into
                               // C's `--` token. Numeric only, type-preserving. `-` with two or
                               // more operands stays BinarySub.
  TypedNodeKind_UnaryPos,      // `(+ x)` -- identity, emitted with no wrapping at all. Exists
                               // only so `+` mirrors `-`'s one-operand case instead of erroring.
  TypedNodeKind_CstrExpr,      // `(cstring s)` -- `(char*)(s).str`, the only bridge from a 3b
                               // string to a raw C string. A `string` is a `{ptr,len}` struct
                               // and can never satisfy an extern's `char*` on its own: casting
                               // a struct to a pointer is invalid C, and argument matching
                               // permits no implicit coercion. Always plain `char*`, never
                               // const-qualified, which is harmless since is_const is a codegen
                               // detail type_ref_equal ignores.
  TypedNodeKind_StringLenExpr, // `(string-len s)` -- `(s).size`, u64. `string` is a primitive
                               // rather than a Named type, so `.` and `get` do not apply to it;
                               // this is the one field read exposed. Needed because os_file_read
                               // returns `{0}` on failure and a caller has to be able to check.
  TypedNodeKind_BinaryCast,    // `(cast Type value)` -- `(Type)value`
  TypedNodeKind_BinaryReinterpret, // `(reinterpret Type value)` -- the same shape as cast, but
                               // copying the bit pattern instead of converting the value.
                               // `(reinterpret i32 f)` reads a float's bits as an int where
                               // `(cast i32 f)` truncates its value. The checker enforces an
                               // equal-byte-width rule that cast does not need.
  TypedNodeKind_FieldAccess,   // `(get base field)` -- `base.field`. `get-in` is sugar,
                               // expanding to a chain of these at lowering time, so chaining
                               // works through ordinary recursive checking.
  TypedNodeKind_IfExpr,
  TypedNodeKind_ReturnExpr,    // `(return value)`, or bare `(return)` in a void fn. Returns
                               // from the enclosing function regardless of nesting depth,
                               // compiling to a C `return` inside `({ ... })` -- which is
                               // well-defined GNU C, exiting the function rather than the
                               // statement expression -- so it stays usable in expression
                               // position, such as one branch of an `if`. Its checked type is
                               // always Unresolved, since control leaves the function and no
                               // value reaches the surrounding context; every comparison in the
                               // checker already skips an Unresolved side, so this composes
                               // with `if` branches for free.
  TypedNodeKind_BreakExpr,     // `(break)` -- leaves the innermost enclosing loop. Carries no
                               // payload and takes no label: only the innermost loop is
                               // targetable, which is what C's own `break` offers and enough for
                               // every loop in this tree. Typed Unresolved for the same reason
                               // ReturnExpr is -- control leaves, no value reaches the
                               // surrounding context -- so it composes with `if` branches for
                               // free, including `(+= n (if done (break) i))`, where codegen
                               // supplies the throwaway value C's ternary insists on (see
                               // cg_if_branch). The checker rejects it outside a loop
                               // (Checker.loop_depth), and also inside a `parallel` body, which
                               // becomes its own C function with no loop to jump to.
  TypedNodeKind_ContinueExpr,  // `(continue)` -- skips to the innermost enclosing loop's next
                               // iteration. Same shape and same rules as BreakExpr above; the
                               // only difference is where the two backends jump to. On the
                               // bytecode VM that distinction is real work: `continue` must land
                               // on the loop's step/increment, not on its condition, or a range
                               // `for` would spin forever.
  TypedNodeKind_WhileExpr,     // `(while cond body...)` -- always void. The body is checked
                               // once structurally, whatever the runtime iteration count.
  TypedNodeKind_ForRangeExpr,  // `(for [name begin end] body...)` and the four-element stepped
                               // form -- one kind, with the unstepped form synthesizing a
                               // literal 1 the way `++` and `--` do. That literal is i32, so a
                               // non-i32 range needs its step written out. Collection iteration
                               // is ForEachExpr below, told apart purely by clause shape.
  TypedNodeKind_ForEachExpr,   // `(for [item coll] body...)` / `(for [[i item] coll] body...)`
                               // -- iterates an Array, Vector or Set's elements, or a Map's
                               // pairs. Distinct from ForRangeExpr because the loop variables'
                               // types come from the collection, and codegen needs a
                               // structurally different C loop per collection kind: indexed for
                               // Array and Vector, slot-walking for Set and Map. `has_index`
                               // picks the surface form. Map iteration requires it, there being
                               // no single natural element for a pair -- enforced in the
                               // checker, since lowering cannot yet know `coll` is a Map.
  TypedNodeKind_ParallelExpr,  // `(parallel [name init ...] body...)` -- forks the process-wide
                               // lane pool, runs the body once per lane, and joins before
                               // continuing. Captures are explicit `name init` pairs evaluated
                               // once in the caller and copied by value into every lane, which
                               // sidesteps free-variable analysis entirely: the capture set is
                               // just the node's own bindings. The body must check as Void.
                               // Not nestable. lane-index, lane-count, lane-sync, lane-arena
                               // and parallel-for are valid only inside it -- see
                               // Checker.in_parallel_block.
  TypedNodeKind_ParallelForExpr, // `(parallel-for [name count] body...)` -- valid only inside a
                               // `parallel` body. Partitions `[0, count)` across lanes with
                               // lane_range and walks only this lane's slice. No begin or step,
                               // lane_range's contract always being `[0, work_count)`. The body
                               // must check as Void.
  TypedNodeKind_BinaryAdd,
  TypedNodeKind_BinarySub,
  TypedNodeKind_BinaryMul,
  TypedNodeKind_BinaryDiv,
  TypedNodeKind_BinaryMod,     // `(% a b)` -- integer only, unlike the four above, since C's `%`
                               // is undefined on floats and base.h's mod_f32/mod_f64 are not
                               // wired up here. Use an `extern fn` if float remainder is needed.
  TypedNodeKind_BinaryBitOr,   // `(bit-or a b)` -- `a | b`. Word-form so `|` stays free.
  TypedNodeKind_BinaryBitAnd,  // `(bit-and a b)` -- `a & b`. Word-form because `&` is already
                               // the address-of-field sugar.
  TypedNodeKind_BinaryBitXor,  // `(bit-xor a b)` -- `a ^ b`
  TypedNodeKind_BinaryShl,     // `(bit-shl a b)` -- `a << b`. The shift amount need not match
                               // `a`'s type, as in C, so this is checked apart from the three
                               // above.
  TypedNodeKind_BinaryShr,     // `(bit-shr a b)` -- `a >> b`, C's implementation-defined
                               // signed shift, arithmetic on every compiler this targets. No
                               // separate logical-shift form is exposed.
  TypedNodeKind_BinaryEq,      // `(= a b)` -- `a == b`. Unambiguous here, assignment being `set`.
  TypedNodeKind_BinaryNeq,     // `(!= a b)` -- `a != b`
  TypedNodeKind_BinaryLt,      // `(< a b)`  -- `a < b`
  TypedNodeKind_BinaryLe,      // `(<= a b)` -- `a <= b`
  TypedNodeKind_BinaryGt,      // `(> a b)`  -- `a > b`
  TypedNodeKind_BinaryGe,      // `(>= a b)` -- `a >= b`
  TypedNodeKind_LogicalAnd,    // `(and a b)` -- `a && b`, short-circuiting via C's own operator
  TypedNodeKind_LogicalOr,     // `(or a b)`  -- `a || b`, likewise
  TypedNodeKind_Call,
  TypedNodeKind_Identifier,
  TypedNodeKind_IntLiteral,
  TypedNodeKind_FloatLiteral,
  TypedNodeKind_StringLiteral,
  TypedNodeKind_NilLiteral,    // `nil` -- a reserved keyword, not usable as an identifier.
                               // Typed as a Pointer with a NULL pointee, the wildcard-pointer
                               // convention described on TypeRef.
  TypedNodeKind_BoolLiteral,   // `true` / `false` -- likewise reserved. Emits the C identifiers
                               // of the same name, valid through <stdbool.h>, which base.h
                               // already includes.
  TypedNodeKind_SizeofExpr,    // `(sizeof T)`  -- C's `sizeof(T)`, u64
  TypedNodeKind_AlignofExpr,   // `(alignof T)` -- C's `_Alignof(T)`, u64
  TypedNodeKind_TypeNameExpr,  // `(type-name T)` -- T's final mangled, package-prefixed C name
                               // as a string literal. Resolved at codegen time, the prefix not
                               // being known before then.
  TypedNodeKind_ZeroExpr,      // `(zero T)` -- an all-bytes-zero value of type T as an ordinary
                               // expression, usable as an initializer, a struct-literal field
                               // or a call argument, rather than only through the
                               // omit-the-initializer carve-out arrays get. Emits C99's
                               // `(T){0}`, which zeroes every unspecified member recursively,
                               // so codegen needs no per-TypeKind handling.
  TypedNodeKind_MemberOffsetExpr, // `(member-offset StructName field)` -- C's
                               // `offsetof(StructName, field)`, u64. `field` is an unevaluated
                               // name, validated against the struct's field list.
  TypedNodeKind_AllocExpr,     // `(alloc Type)` / `(alloc Type Count)` -- malloc-backed, for a
                               // value needing a lifetime independent of any arena. Same
                               // synthesized literal 1 as PushAlloc, minus the arena. Pairs
                               // with the `free` builtin, an ordinary Call.
  TypedNodeKind_Block,
} TypedNodeKind;

typedef struct TypedNode {
  TypedNodeKind kind;
  Token         token;     // for diagnostics only: line and column back to source
  b32           is_private; // top-level declarations only, set by a wrapping `(private ...)`.
                            // Outside the union so it applies uniformly rather than being
                            // repeated across six members. Affects C linkage only; real
                            // cross-package visibility enforcement comes later.
  b32           is_imported; // synthetic top-level nodes spliced in to stand for another
                            // package's public declaration. The name is already qualified
                            // "pkg/member" and fn bodies are always TYPED_NIL. The checker
                            // treats these as ordinary declarations, which is the point, but
                            // codegen must never emit one -- the real definition lives in that
                            // package's own generated .c, reached through its .h.
  union {
    struct {
      String8    name;
      u32        param_first;
      u16        param_count;
      TypeRef    return_type; // always explicit
      TypedIndex body;        // a Block. TYPED_NIL marks an `extern` signature: declared to
                              // exist in C, with no body to check or emit.
      b32        is_variadic; // a trailing `name ...` in the parameter vector, legal only on
                              // `extern` signatures. Arguments past param_count are unchecked,
                              // as with C varargs. Codegen needs nothing special: extern calls
                              // already emit whatever arg_count they have, and extern
                              // signatures never get a prototype -- the real C declaration is
                              // trusted to be visible.
      b32        is_lane_fn;  // `(lane-fn (fn ...))` -- callable only from inside a `parallel`
                              // block or another lane-fn. The checker enforces both ends: it
                              // forces in_parallel_block while checking this body, and rejects
                              // call sites reached without it. A checker-time marker only;
                              // codegen treats a lane-fn like any other function, since
                              // lane_idx and lane_range read thread-local state rather than
                              // parameters, so a plain C call works at any depth.
    } func;
    struct {
      String8    name;
      TypeRef    type; // always explicit for def/var
      TypedIndex init;
    } const_decl;
    struct {
      String8    name;
      TypeRef    type; // always explicit for def/var
      TypedIndex init;
    } var_decl;
    struct {
      u32        binding_first; // index into TypedAst.bindings
      u16        binding_count;
      TypedIndex body;          // a Block; last stmt is the let's value
    } let_expr;
    struct {
      String8    var_name; // always bound as type `arena`
      TypedIndex body;     // a Block; its last statement is the block's value, captured before
                           // the arena rewind runs
    } scratch_expr;
    struct {
      u32        capture_first; // index into TypedAst.bindings, the array LetExpr also uses
      u16        capture_count;
      TypedIndex body;          // a Block; must check as Void
    } parallel_expr;
    struct {
      String8 name;
      u32     field_first; // index into TypedAst.params; Param{name,type} suits fields too
      u16     field_count;
      b32     is_packed;   // `(packed (struct ...))` -- emits `__attribute__((packed))`
      u32     align_bytes; // `(align N (struct ...))` -- emits `aligned(N)`; 0 means unspecified
      b32     is_union;    // identical in shape, fields and checker treatment -- member access
                           // and construction do not care -- differing only in codegen emitting
                           // `union`, and so getting C's overlapping-offset semantics
      b32     is_synthesized; // lowering minted this struct and its `AnonN`/`AnonParamN`/
                              // `AnonReturnN` name -- an inline `(struct [...])`, a `{:field type}`
                              // parameter shape, a `(bool T)` multi-return. Nothing in the compiler
                              // treats it differently; it exists so an LSP popup can avoid
                              // reporting a name the user never wrote and cannot look up
      String8 c_name; // optional `(struct Name "c-spelling" [...])` second operand: the C type
                      // this struct mirrors. Unlike alias_decl.c_name it does NOT change how the
                      // struct is emitted -- the mirror is still a real, independent C struct --
                      // it only names the original for the FFI casts described at cg_ffi_c_type.
    } struct_decl;
    struct {
      String8 name;
      TypeRef type;
      String8 c_name; // optional `(alias Name Type "c-spelling")` third operand: the verbatim
                      // C type the emitted typedef names, instead of `type`'s own C spelling.
                      // Empty for the ordinary two-operand form. See cg_alias_decl.
    } alias_decl;
    struct {
      String8 type_name; // the backing struct type, e.g. "Mesh" in `(handle Mesh)`
    } handle_pool_decl;
    struct {
      String8 mangled_name; // "Map_string_i32", "Set_i32". Not yet package-prefixed;
                            // cg_symbol_name does that when it is emitted.
      TypeRef key_type;
      TypeRef value_type;   // zeroed when is_set
      b32     is_set;
    } hashtable_instance;
    struct {
      TypeRef    type;     // the pooled struct type, naming which pool this initializes
      TypedIndex capacity;
      TypedIndex arena;
    } handle_pool_init;
    struct {
      String8 type_name;
      Token   type_name_token; // the name atom's own token. The node's `token` is the whole
                               // form's opening paren, but an LSP hover or goto-definition
                               // query needs to match against where the name itself starts.
      u32     field_first; // index into TypedAst.field_inits
      u16     field_count;
    } struct_lit;
    struct {
      u32 element_first; // index into TypedAst.array_elements
      u16 element_count;
    } array_lit;
    struct {
      TypedIndex base;
      TypedIndex index;
    } index_access;
    struct {
      TypedIndex base;
      u32        slot; // 0-based. The checker maps it to a declared field or a literal index
                       // once `base`'s type is known.
    } positional_access;
    struct {
      TypedIndex base;
      b32        auto_deref; // as in field_access.auto_deref: true for `.` and `&`
      String8    field_name; // non-empty only when the hop was a plain identifier, and so a
                             // candidate field name. Empty for a literal or list hop, which
                             // can only be a Map key.
      Token      field_token; // the hop's own token, so the FieldAccess this becomes can carry
                              // where the field name was written -- see field_access.field_token
      TypedIndex key_expr;   // always set
    } dot_hop;
    struct {
      TypedIndex arg;                // the `string` expression to parse
      TypeKind   target_kind;        // I32, I64, U32, U64, F32 or F64
      String8    result_struct_name; // the interned `(bool T)` shape
    } parse_number;
    struct {
      TypedIndex vec;                // the Vector to search
      TypedIndex needle;             // the value to find, of `vec`'s comparable element type
      String8    result_struct_name; // the interned `(bool u64)` shape
    } index_of;
    struct {
      String8    libm_name;       // "sqrt", "asin", "acos" or "pow", the `-checked` stripped
      TypedIndex arg;             // the only argument, for all but pow
      TypedIndex arg2;            // TYPED_NIL unless this is `pow-checked`
      String8    f32_struct_name; // both `(bool T)` shapes are interned up front; the checker
      String8    f64_struct_name; // picks by the argument's resolved type
    } checked_math;
    struct {
      TypeRef    elem_type;
      TypedIndex arena;
      TypedIndex count;  // always present; a literal 1 is synthesized when source omits it
      b32        zeroed; // push_array_zero rather than push_array
    } push_alloc;
    struct {
      TypedIndex arena;
      TypedIndex value;
    } push_copy;
    struct {
      TypeRef    elem_type;
      TypedIndex count; // always present; a literal 1 is synthesized when source omits it
    } alloc_expr;
    struct {
      TypedIndex arena;
      String8    arr_name;        // unless is_field_target: a mutable local of Pointer type
      b32        is_field_target; // true for `(vector-push arena (. base field ...) value)`
      TypedIndex target_expr;     // when is_field_target: the FieldAccess chain. cg_expr on it
                                  // produces a repeatable C lvalue, which is what dyn_push's
                                  // macro needs in order to reseat the array on growth.
      TypedIndex value;
    } dyn_push;
    struct {
      TypedIndex dst_arena;
      TypedIndex src; // a dyn-push-grown pointer; the result mirrors its type
    } commit_expr;
    struct {
      String8 name;
      b32     is_flags;      // `enum`: sequential values and a `_Count` sentinel.
                             // `flags`: bit-position values and an `_All` mask.
      u32     variant_first; // index into TypedAst.enum_variants
      u16     variant_count;
      String8 c_name;        // optional `(enum Name "c-spelling" [...])` second operand, the same
                             // FFI-cast pin struct_decl.c_name is. Typically "enum AVPixelFormat".
    } enum_decl;
    struct {
      String8 enum_name;
      String8 variant_name;
    } enum_access;
    struct {
      SetTargetKind target_kind;
      String8       target_name;   // Identifier targets
      TypedIndex    target_expr;   // Deref targets: the pointer expression, not yet deref'd.
                                   // Field targets: the lowered FieldAccess chain. Both the
                                   // checker and codegen walk it exactly like a read, a field
                                   // access being a valid C lvalue on either side of `=`.
      TypedIndex    index_base;    // Index targets
      TypedIndex    index_index;   // Index targets
      TypedIndex    value;
    } set_expr;
    struct {
      TypedIndex expr;
    } unary; // UnaryDeref. Cast and Reinterpret use `binary`, lhs being the type name.
    struct {
      TypedIndex base;
      String8    field;
      Token      field_token; // the field name's own token. The node's `token` is the whole form's,
                              // shared by every hop of a `.` chain, so an LSP query for one hop has
                              // to match against this instead (see query_token_for, lib3b.c).
                              // SOURCE_FILE_UNKNOWN where lowering synthesized the access rather
                              // than reading it from source, as destructuring does.
      b32        auto_deref; // `.` and `&` insert one deref when base resolves to a pointer;
                             // `get` and `get-in` require it spelled out
    } field_access;
    struct {
      TypedIndex cond;
      TypedIndex then_branch;
      TypedIndex else_branch; // TYPED_NIL if omitted
    } if_expr;
    struct {
      TypedIndex cond;
      TypedIndex body; // a Block
    } while_expr;
    struct {
      String8    var_name;
      TypedIndex begin;
      TypedIndex end;
      TypedIndex step; // always present -- literal 1 synthesized if omitted in source
      TypedIndex body; // a Block
    } for_range;
    struct {
      b32        has_index;  // `[item coll]` binds elem_name only; `[[i item] coll]` binds both
      String8    index_name; // when has_index: a u64 position, or a Map's key
      String8    elem_name;  // the element, or a Map's value
      TypedIndex collection;
      TypedIndex body;       // a Block
      b32        elem_is_read;  // both filled in by the checker; see ScopeEntry.was_read
      b32        index_is_read;
    } for_each;
    struct {
      String8    var_name;
      TypedIndex count; // work_count for lane_range; always partitions [0, count)
      TypedIndex body;  // a Block; must check as Void (see TypedNodeKind_ParallelForExpr)
    } parallel_for;
    struct {
      TypedIndex lhs;
      TypedIndex rhs;
    } binary;
    struct {
      String8 callee;
      Token   callee_token; // the callee atom's own token; the node's `token` is the form's
                            // opening paren, which is not where an LSP query needs to match
      u32     arg_first;
      u16     arg_count;
    } call;
    struct {
      String8 name;
      Token   decl_token; // copied from the matched ScopeEntry, but only when this reference
                          // resolved through the checker's shadowing-aware Scope chain. Left
                          // zeroed when it resolved another way, such as a top-level fn used
                          // as a value; goto-definition then reports it as unresolvable rather
                          // than guessing.
    } ident;
    struct {
      i64      value;
      TypeKind explicit_type;   // Unresolved when the atom had no suffix; defaults to i32
      b32      infer_from_peer; // set only on the literal 1 that lower_incdec synthesizes.
                                // `++` cannot write a suffix the user never typed, and
                                // defaulting to i32 would break `++` on a u64 or f32. The
                                // checker patches explicit_type from the other operand once
                                // that type is known. Nothing else sets this.
      b32      unsigned_bits;   // the atom was read as an unsigned bit pattern, not a signed
                                // magnitude: a hex atom, or one carrying an unsigned suffix
                                // with no authored `-`. Either way `value` holds the pattern,
                                // so `0xFFFFFFFFFFFFFFFFu64` and `18446744073709551615u64`
                                // both land here as -1 rather than overflowing this signed
                                // field. int_literal_fits reads this to accept those as
                                // all-bits-set while still rejecting a real `-5u64`.
    } int_lit;
    struct {
      f64      value;         // kept at double precision whatever the suffix; codegen narrows
      TypeKind explicit_type; // Unresolved when the atom had no suffix; defaults to f32
    } float_lit;
    struct {
      String8 value;
    } string_lit;
    struct {
      b32 value;
    } bool_lit;
    struct {
      TypeRef type;
      TypeRef result_type; // Sizeof and Alignof only: `(sizeof T Type)` overrides the otherwise
                           // fixed u64 result. Unresolved when not given.
    } type_query; // Sizeof, Alignof, TypeName, Zero, HandleAlloc -- all "a type, nothing else"
    struct {
      TypeRef type;  // must resolve to a struct; check_expr enforces it
      String8 field; // unevaluated, validated against that struct's field list
    } member_offset;
    struct {
      u32 stmt_first;
      u16 stmt_count;
    } block;
  };
} TypedNode;

// One entry per source atom that lower_type_node resolves a TypeRef from,
// which covers every annotation site -- parameters, fields, let/val/var and
// return types, type arguments -- since all of them bottom out there. `token`
// is the atom verbatim, so `Mesh`, `Mesh**` and `Mesh^` are each a single
// token spanning the whole spelling; pointer and handle suffixes are part of
// the atom rather than separate punctuation. An LSP query therefore needs no
// unwrapping to find the atom under the cursor, only to resolve `type`.
typedef struct TypeAnnotation {
  Token   token;
  TypeRef type;
} TypeAnnotation;

typedef struct TypedAst {
  Arena*           arena;
  TypedNode*       nodes;         // dyn array; index 0 = Nil
  TypedIndex*      extra;         // dyn array of TypedIndex (Block stmts / Call args)
  Param*           params;        // dyn array of function parameters (and struct fields)
  Binding*         bindings;      // dyn array of `let` bindings
  FieldInit*       field_inits;   // dyn array of struct-literal field initializers
  EnumVariant*     enum_variants; // dyn array of enum/flags variants
  TypedIndex*      array_elements;   // dyn array of array-literal element expressions
  TypeAnnotation*  type_annotations; // dyn array; see TypeAnnotation above
} TypedAst;

void       typed_ast_init(TypedAst* tast, Arena* arena);
TypedIndex typed_push(TypedAst* tast, TypedNode node);

////////////////////////////////
//~ Lowering: generic AST -> typed AST
//
// Where `fn`, `let`, `if`, `do` and `+` first mean anything; the parser has
// no idea they exist.

// `(alias NewName ExistingType)`, gathered in its own pass after struct and
// enum names, so an alias can name a struct declared later in the file.
// Aliases themselves resolve in source order: one alias can reference an
// earlier alias but not a later one.
typedef struct TypeAlias {
  String8 name;
  TypeRef type;
} TypeAlias;

typedef struct Lowerer {
  Ast*       ast;
  TypedAst*  tast;
  String8*   struct_names;      // dyn array, gathered up front so that `(Creature {...})`
                                // construction sites are recognizable in the same pass that
                                // lowers them
  NodeIndex* struct_decl_forms; // dyn array parallel to struct_names: each name's own
                                // `(struct Name [...])` form, so `member-type` can re-read its
                                // field vector whatever the declaration order
  String8*   enum_names;        // dyn array, gathered the same way. `enum` and `flags` names sit
                                // together undifferentiated, since slash resolution only asks
                                // whether a name is a scoped constant access, not which kind.
  TypeAlias* aliases;           // dyn array, gathered in the later pass described above

  // Lookup tables mirroring the four dyn arrays above. Each is kept in sync by
  // the matching lower_register_* function, which is the only sanctioned way
  // to add a name.
  HashTable  struct_form_by_name;  // name -> boxed NodeIndex*, boxed because NODE_NIL is 0 and
                                   // would otherwise be indistinguishable from "not found"
  HashTable  enum_name_set;        // name -> (void*)1
  HashTable  alias_by_name;        // name -> boxed TypeAlias*
  HashTable  handle_pool_type_set; // name -> (void*)1: which structs have a `(handle Name)`, so
                                   // that `Name^` in type position resolves to a handle rather
                                   // than an unknown-type error

  HashTable  anon_struct_by_shape; // structural signature -> boxed AnonStructIntern (a lower.c
                                   // type). Lets two functions declaring the same
                                   // `{:field type ...}` anonymous parameter shape share one
                                   // synthesized struct rather than minting AnonParamN twice.
                                   // Anonymous struct types in FIELD position do not dedupe;
                                   // parameter shapes are meant to recur as a lightweight API
                                   // convention, which is what makes interning worthwhile here.
  HashTable  hashtable_instances_emitted; // "Map:<key>,<value>" / "Set:<key>" -> (void*)1.
                                   // Membership only: the mangled name is always recomputable
                                   // from the TypeRefs via hashtable_mangled_name, so this only
                                   // has to answer whether the instantiation has already been
                                   // spliced into pending_toplevel.
  HashTable  multi_return_struct_set; // name -> (void*)1: which synthesized structs came from a
                                   // `(T0 T1 ...)` multi-value return type. Lets lower_fn
                                   // recognize its own return type as multi-return sugar without
                                   // confusing it with a function returning a real struct.
  String8    current_fn_multi_return_name; // the enclosing fn's multi-return struct name, or
                                   // empty when its return type is not multi-return sugar. Set
                                   // and restored around body lowering in lower_fn, and read by
                                   // lower_return so `(return a b)` can build a `_0`/`_1` literal
                                   // of the right synthesized type.
  b32        name_tables_init;  // the tables above are initialized on first use, Lowerer having
                                // more than one construction site
  b32        had_error;
  TypedIndex* pending_toplevel; // dyn array of StructDecls synthesized from an anonymous
                                // `(struct {...})` used inline in type position. lower_program
                                // drains it right after the top-level form that triggered the
                                // synthesis and splices the contents in just before that form.
                                // Codegen emits structs in source order with no topological
                                // sort, so an anonymous struct embedded by value has to land
                                // ahead of its embedder, exactly as a named one would.
  u32        anon_type_counter; // monotonic; names synthesized types `Anon0`, `Anon1`, ...
} Lowerer;

b32 is_known_struct_name(Lowerer* low, String8 name);
b32 is_known_enum_name(Lowerer* low, String8 name);
// The only sanctioned way to add a name: each pushes to the dyn array and
// updates the matching lookup table, so the two can never drift apart.
void lower_register_struct_name(Lowerer* low, String8 name, NodeIndex form);
void lower_register_enum_name(Lowerer* low, String8 name);
void lower_register_alias(Lowerer* low, TypeAlias alias);
void lower_register_handle_pool_type(Lowerer* low, String8 name);
b32  is_known_handle_pool_type(Lowerer* low, String8 name);

AstNode*   ast_get(Ast* ast, NodeIndex idx);
NodeIndex* ast_seq_children(Ast* ast, NodeIndex idx, u16* out_count);

void       lower_error(Lowerer* low, Token tok, const char* fmt, ...);
TypedIndex lower_atom(Lowerer* low, NodeIndex idx);
TypedIndex lower_string(Lowerer* low, NodeIndex idx);
TypedIndex lower_fn(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count);
TypedIndex lower_val_or_var(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count,
                            b32 is_var);
TypedIndex lower_let(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count);
TypedIndex lower_destructure_source(Lowerer* low, NodeIndex source_ast_idx, Binding** scratch,
                                     Arena* scratch_arena, Token tok);
void       lower_destructure_bind(Lowerer* low, String8 local_name, TypedIndex init,
                                   Binding** scratch, Arena* scratch_arena);
void       lower_destructure_target(Lowerer* low, Token tok, String8 field_name, TypedIndex source,
                                     NodeIndex target_idx, Binding** scratch, Arena* scratch_arena);
void       lower_destructure_map(Lowerer* low, NodeIndex map_idx, TypedIndex source,
                                  Binding** scratch, Arena* scratch_arena);
void       lower_destructure_vector(Lowerer* low, NodeIndex vec_idx, TypedIndex source,
                                     Binding** scratch, Arena* scratch_arena);
TypedIndex lower_if(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count);
TypedIndex lower_when(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count);
TypedIndex lower_match(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count);
TypedIndex lower_while(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count);
TypedIndex lower_for(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count);
TypedIndex lower_return(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count);
TypedIndex lower_loop_jump(Lowerer* low, NodeIndex idx, u16 count, TypedNodeKind kind, const char* name);
TypedIndex lower_do(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count);
TypedIndex lower_void_do(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count);
TypedIndex lower_binary_op(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count,
                           TypedNodeKind kind, const char* op_name, b32 variadic);
TypedIndex lower_call(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count);
TypedIndex lower_struct_decl(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count, b32 is_union);
TypedIndex lower_alias_decl(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count);
TypedIndex lower_handle_pool_decl(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count);
TypedIndex lower_handle_pool_init(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count);
TypedIndex lower_push(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count, b32 zeroed);
TypedIndex lower_alloc(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count);
TypedIndex lower_enum_decl(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count, b32 is_flags);
TypedIndex lower_struct_construct(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count,
                                  String8 struct_name);
TypedIndex lower_array_literal(Lowerer* low, NodeIndex idx);
TypedIndex lower_nth(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count);
TypedIndex lower_set_target(Lowerer* low, Token token, NodeIndex target_ast_idx, TypedIndex value);
TypedIndex lower_set(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count);
TypedIndex lower_compound_assign(Lowerer* low, Token token, NodeIndex target_ast_idx,
                                 TypedIndex rhs_value, TypedNodeKind binop_kind);
TypedIndex lower_incdec(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count,
                        TypedNodeKind binop_kind, const char* op_name);
TypedIndex lower_compound_assign_op(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count,
                                    TypedNodeKind binop_kind, const char* op_name);
TypedIndex lower_unary_deref(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count);
TypedIndex lower_unary_addr(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count);
TypedIndex lower_not(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count);
TypedIndex lower_bit_not(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count);
TypedIndex lower_unary_neg(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count);
TypedIndex lower_unary_pos(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count);
TypedIndex lower_cstring(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count);
TypedIndex lower_string_len(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count);
TypedIndex lower_binary_cast(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count);
TypedIndex make_field_access(Lowerer* low, Token token, TypedIndex base, String8 field,
                             Token field_token, b32 auto_deref);
TypedIndex make_index_access(Lowerer* low, Token token, TypedIndex base, TypedIndex index);
TypedIndex lower_get(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count);
TypedIndex lower_get_in(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count);
TypedIndex lower_field_chain_core(Lowerer* low, Token token, TypedIndex base, NodeIndex* children,
                                  u32 field_start, u16 count);
TypedIndex lower_dot(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count);
TypedIndex lower_addr_field(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count);
TypedIndex lower_type_query(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count,
                            TypedNodeKind kind, const char* form_name);
TypedIndex lower_member_offset(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count);
TypedIndex lower_thread(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count, b32 insert_first);
TypedIndex lower_some_thread(Lowerer* low, NodeIndex idx, NodeIndex* children, u16 count);
TypedIndex lower_list(Lowerer* low, NodeIndex idx);
TypedIndex lower_expr(Lowerer* low, NodeIndex idx);
TypedIndex lower_program(Lowerer* low, NodeIndex root);

// Dumps a lowered subtree -- kind, resolved type and token position per node,
// indented by `depth`. The TypedAst counterpart of ast_print above.
void       typed_ast_print(TypedAst* tast, TypedIndex idx, u32 depth);

// The C operator a binary kind emits. Shared by codegen and the debug
// printer, the binary kinds being structurally identical otherwise.
const char* binary_op_symbol(TypedNodeKind kind);

// The source keyword the user actually typed, for error messages -- not
// always the emitted operator, since `bit-or` emits `|` and `=` emits `==`.
// Kept separate from binary_op_symbol so diagnostics and generated output
// cannot drift by editing one and forgetting the other.
const char* binary_op_display_name(TypedNodeKind kind);

////////////////////////////////
//~ Type checking
//
// A pass over the already-lowered typed AST, rather than the more usual
// arrangement of checking a still-generic representation before lowering it.
// Lowering here already produces typed nodes with full structural validation,
// so a separate pass afterward is simpler than threading type information
// through lowering itself.
//
// Scopes use real push/pop. Codegen's own name tracking can tolerate stale
// entries, since the generated C still catches any genuine scoping mistake,
// but the same laxness in the checker would be actively misleading: a missed
// error here means the user gets a confusing GCC message instead of a clear
// 3b one.

typedef struct FnEntry {
  String8    name;
  TypedIndex decl; // FunctionDecl typed node index
} FnEntry;

// name -> TypeRef, with lexical push/pop via scope_mark and scope_pop_to.
typedef struct ScopeEntry {
  String8 name;
  TypeRef type;
  b32     is_mutable; // false only for `val`; var, let and parameters are all mutable
  b32     was_read;   // set by scope_lookup_entry -- "some expression in scope named this".
                      // Deliberately coarse: a write target counts as a read, and it is set
                      // even on a probe that only asks whether the name is a local. The one
                      // consumer is codegen's `(void)name;` suppression for a binding the
                      // body never mentions, where over-reporting a use costs nothing and
                      // under-reporting would emit a bogus unused-variable warning.
  Token   decl_token; // the enclosing binding form's opening paren, not a per-name position:
                      // Param and Binding carry no token of their own, and threading one
                      // through lower.c's many construction sites was not worth the extra
                      // jump-target precision. check_expr copies this onto resolved references
                      // for goto-definition to read back.
} ScopeEntry;

typedef struct Scope {
  ScopeEntry* entries; // dyn array, arena = ctx_scratch()
  // Entries in [hidden_lo, hidden_hi) are skipped by scope_lookup_entry.
  // Equal (both 0 outside a `parallel` body) means nothing is hidden.
  //
  // A `parallel` body runs on another thread, where the enclosing function's
  // params and locals do not exist -- codegen emits it as a standalone
  // trampoline whose only inputs are the explicit captures. Globals DO exist
  // there (they are C file-scope), and lane code writes to them routinely, so
  // this hides a RANGE rather than everything below a mark: globals sit below
  // Checker.fn_scope_base, the hidden params/locals between it and the
  // captures, the captures and the body's own bindings above.
  u64         hidden_lo;
  u64         hidden_hi;
} Scope;

// One position an embedder wants lexical scope snapshotted at, for
// completion. Matched by basename alone, a single compile only ever having one
// file open under a given basename. See check_expr's hook.
typedef struct ScopeQuery {
  String8 file_basename;
  u32     line;
  u32     col; // one past the last character of the identifier being typed, which is
               // exactly where patch_unclosed_delimiters truncated the buffer, so this
               // matches at most one real token
} ScopeQuery;

typedef struct StructEntry {
  String8    name;
  TypedIndex decl; // StructDecl typed node index
} StructEntry;

typedef struct EnumEntry {
  String8    name;
  TypedIndex decl;     // EnumDecl typed node index
  b32        is_flags; // cached from decl for lookup convenience
} EnumEntry;

typedef struct Checker {
  TypedAst*    tast;
  FnEntry*     fns;     // dyn array of every top-level fn, gathered up front so calls resolve
                        // regardless of declaration order
  StructEntry* structs; // dyn array of every top-level struct, gathered the same way
  EnumEntry*   enums;   // dyn array of every top-level enum and flags, likewise

  // Lookup tables built alongside the three arrays above during check_program's
  // first pass, so resolution is O(1) rather than a scan over every top-level
  // declaration at each reference.
  HashTable    fns_by_name;     // name -> FnEntry*
  HashTable    structs_by_name; // name -> StructEntry*
  HashTable    enums_by_name;   // name -> EnumEntry*
  HashTable    handle_pool_types; // name -> (void*)1: which structs have a `(handle Name)`.
                                  // Validates handle type positions and confirms that the
                                  // handle-alloc/deref/free/valid? builtins target a real pool.

  TypeRef*     resolved_types;     // one entry per typed node, indexed by TypedIndex. Sized from
                                   // the node count at entry, but checking can add nodes -- a
                                   // PositionalAccess synthesizes an IntLiteral per destructured
                                   // slot -- so every write must go through
                                   // resolved_types_ensure_capacity rather than indexing here.
  u64          resolved_types_cap; // element count, tracked separately because this array has no
                                   // dyn_push header of its own

  TypeRef      current_fn_return_type; // meaningful while in_function_body. Not a stack: nested
                                       // fn declarations are unsupported, so only one body is
                                       // ever in flight.
  b32          in_function_body;  // lets `return` outside any function give a clear error
  u32          loop_depth;        // enclosing loop bodies currently being checked, gating `break`
                                  // and `continue`. A counter rather than a flag because it is
                                  // also SAVED AND ZEROED around a `parallel` body: that body
                                  // becomes its own C function natively (cg_parallel_expr's
                                  // trampoline), so a `break` inside it has no loop to jump to
                                  // even when the `parallel` itself sits in one.
  b32          in_loop_header;    // set while checking a loop's HEADER -- a `while` condition, a
                                  // range `for`'s begin/end/step, a `for`'s collection, a
                                  // `parallel-for`'s count -- where `break` and `continue` are
                                  // rejected outright. Cleared again by check_loop_body, so a
                                  // loop nested INSIDE a header still admits them in its own body.
  b32          in_parallel_block; // set around a `parallel` body. Gates lane-index, lane-count,
                                  // lane-sync, lane-arena and parallel-for the way
                                  // in_function_body gates `return`, and rejects a `parallel`
                                  // directly inside another.
  b32          has_parallel;      // true once any `parallel` block has been checked. Copied into
                                  // Codegen so a program that never forks lanes does not pay for
                                  // the thread pool's spin-up.
  u64          fn_scope_base;     // scope_mark taken just before the current function's params were
                                  // bound, so entries below it are globals and entries at or above
                                  // it are this call's own. Only ParallelExpr reads it, to hide
                                  // exactly the latter -- see Scope.hidden_lo.
  b32          had_error;

  const ScopeQuery* scope_query;   // NULL unless an embedder wants a scope snapshot; an ordinary
                                   // compile pays only one pointer check per check_expr
  b32          scope_query_done;   // set as soon as the hook matches, since a legitimately empty
                                   // scope leaves scope_query_result NULL and would otherwise be
                                   // indistinguishable from "not captured yet"
  ScopeEntry*  scope_query_result; // malloc'd rather than arena-allocated: it must outlive
                                   // ctx_free, as PackageBuild's copy of it does
  u64          scope_query_count;

  b32          is_root_package; // true for the package named on the command line, and for either
                                // single-file path, which is inherently its own root. Gates
                                // check_main_signature: a public `fn main` is the C entry point
                                // only in the root package. cg_symbol_name and cg_function have
                                // the matching codegen-side gates.
} Checker;

void        type_error(Checker* ck, Token tok, const char* fmt, ...);
TypeRef     type_ref_unresolved(void);
b32         type_ref_equal(TypeRef a, TypeRef b);
b32         type_kind_is_numeric(TypeKind k);
b32         type_kind_is_printable(TypeKind k);
b32         type_ref_is_printable(TypeRef t);
b32         type_ref_is_bitwise_ok(Checker* ck, TypeRef t);
b32         type_ref_is_comparable(Checker* ck, TypeRef t);
b32         type_ref_is_deep_comparable(TypedAst* tast, StructEntry* structs, u64 struct_count, TypeRef t);
u64         scope_bind_with_mutability(Scope* scope, String8 name, TypeRef type, b32 is_mutable, Token decl_token);
ScopeEntry* scope_lookup_entry(Scope* scope, String8 name);
TypeRef     scope_lookup(Scope* scope, String8 name);
TypeRef     check_block(Checker* ck, Scope* scope, TypedIndex block_idx);
TypeRef     check_array_literal(Checker* ck, Scope* scope, TypedIndex idx, TypeRef expected);
TypeRef     check_init_expr(Checker* ck, Scope* scope, TypedIndex init, TypeRef declared_type);
TypeRef     check_expr(Checker* ck, Scope* scope, TypedIndex idx);
Checker     check_program(TypedAst* tast, TypedIndex root, b32 is_root_package, const ScopeQuery* scope_query);
// Native codegen only: catches two top-level fns -- typically an `extern` and
// a public wrapper -- that mangle to the same C symbol.
void         check_mangled_name_collisions(Checker* ck, String8 pkg_name, b32 is_root_package);
StructEntry* struct_table_lookup(Checker* ck, String8 name);
EnumEntry*   enum_table_lookup(Checker* ck, String8 name);

////////////////////////////////
//~ C codegen
//
//  - cg_expr on any TypedNode produces exactly one valid C expression. Block
//    and LetExpr are the interesting cases: they compile to GNU statement
//    expressions, `({ stmt1; stmt2; last_expr; })`, whose value is the last
//    expression -- the same trick base.h's own `any`/`all`/`find` macros use.
//    So `if` branches, `do` bodies and `let` bodies nest and compose with no
//    special-casing at any depth.
//  - The one place needing a real C `return` is a function's top-level body,
//    which cg_function handles directly rather than going through cg_expr.
//  - `-` is not legal in a C identifier, so names are mangled: `add-two`
//    becomes `add_two`.
//  - `print` is not a real builtin; dispatch reads the argument's entry in
//    resolved_types rather than tracking scope itself.

typedef struct CgScopeEntry {
  String8 source_name; // the language-level name as written
  String8 c_name;      // the C identifier emitted for it: the plain mangled name unless it
                       // collided with something already in scope, in which case
                       // cg_scope_reserve disambiguated it
  b32 is_vector_ref_param; // set only for a Vector function PARAMETER, which is declared and
                           // passed as T** rather than T* so that a `vector-push` inside the
                           // function grows the caller's Vector instead of a local copy of the
                           // pointer. Every read of the name inside the body dereferences once
                           // to get back to the T*. False for every other binding, including a
                           // Vector that is a plain local.
} CgScopeEntry;

// One "this parameter name means this call-site argument" binding, live only
// while cg_emit_folded_call is emitting an inlined body. See const_inline_subs.
typedef struct CgConstInlineSub {
  String8    name;
  TypedIndex expr;
} CgConstInlineSub;

typedef struct Codegen {
  TypedAst*    tast;
  TypeRef*     resolved_types; // from Checker, indexed the same way by TypedIndex
  StructEntry* structs;        // from Checker. Used only by cg_emit_anon_member_body, to look
                               // up a field list when inline-expanding an anonymous member.
  HashTable    fns_by_name;    // from Checker. Lets cg_call read a callee's declared parameter
                               // types -- FnEntry.decl indexes the same tast -- to spot an
                               // array-to-pointer decay argument and emit the cast for it.
  FILE*        out;
  String8      package_name;  // unmangled. Needed for the header's include guard and the .c
                              // file's #include. Empty on the single-file path.
  TypedIndex   program_root;  // this package's top-level Block, imports already spliced in, so
                              // cg_symbol_name can ask whether a name is one of this package's
                              // own public declarations from anywhere in codegen. TYPED_NIL on
                              // the single-file path, where nothing is prefixed.
  String8*     imported_pkg_names; // dyn array of the packages this one directly imports. Used
                              // only to emit their `#include` lines.
  CgScopeEntry* scope;        // dyn array used as a stack, mirroring checker.c's Scope: every
                              // local binding pushes here and pops on the way out
  u32           next_disambig_id; // consumed only when cg_scope_reserve hits a collision, which
                                  // most programs never do
  b32           has_parallel; // copied from Checker. Gates whether cg_function hoists `parallel`
                              // trampolines and whether cg_function_main emits
                              // async_threads_init.
  FILE*         parallel_prelude_out; // non-NULL only while cg_function emits a body in a
                              // has_parallel package. cg_parallel_expr writes each occurrence's
                              // capture struct and trampoline here, to be flushed just ahead of
                              // the enclosing function's signature, and the fork/join snippet
                              // to cg->out as usual.
  b32           is_root_package; // the counterpart of Checker.is_root_package: gates whether a
                              // public `main` becomes the literal C entry point. See
                              // cg_symbol_name, cg_function and cg_function_prototype, the three
                              // places that special-case that name.
  HashTable     public_toplevel_names; // memoized by cg_is_own_public_toplevel_name on first
                              // use, program_root being fixed by then, so later calls are O(1)
                              // rather than a walk over every top-level statement
  b32           public_toplevel_names_built;
  HashTable     ffi_c_names; // 3b struct/enum name -> boxed String8, the C type that declaration
                              // mirrors. Populated from struct_decl.c_name/enum_decl.c_name, which
                              // only `3b translate` writes. Memoized like the set above; see
                              // cg_ffi_c_type.
  b32           ffi_c_names_built;
  b32           had_error;    // a construct the C backend cannot express was reported through
                              // diag_error. compiler.c fails the package on it rather than
                              // handing the emitted .c to the C compiler, whose own message
                              // would describe generated code instead of the 3b behind it.
  b32           in_static_init; // emitting a file-scope initializer, where C demands a constant
                              // expression. Read by cg_call's fallback, a function call not
                              // being one, and by cg_expr's StringLiteral case, since
                              // `bbb_str8_lit` expands to a call. Set by cg_toplevel around
                              // cg_init_value and read at arbitrary depth, a string or call
                              // being able to sit nested inside a struct or array literal.
  u32           scratch_depth; // how many `scratch` blocks enclose the expression being emitted,
                              // within the C function currently being written. cg_scratch_expr
                              // numbers its `_3b_scratch_temp_N` by this, so nested scopes do not
                              // shadow one another, and cg_unwind_scratch_scopes ends them before
                              // a jump leaves them. Saved and reset to 0 around a `parallel`
                              // body, whose statements land in a separate trampoline function
                              // where the enclosing names are not in scope -- the same reason
                              // cg_parallel_expr swaps `scope` to NULL.
  u32           loop_scratch_depth; // `scratch_depth` as it was when the innermost enclosing loop
                              // body began, and so the floor a `break`/`continue` unwinds to --
                              // see TypedNodeKind_ScratchExpr for why those two stop short of 0
                              // where `return` does not. Set at the two places a loop body is
                              // emitted, cg_loop_body_block and cg_foreach_body_stmts.

  // While cg_emit_folded_call emits an inlined body, each entry means "this
  // identifier is really this argument expression from the call site".
  // cg_expr's Identifier case consults it before cg_scope_lookup. Pushed and
  // popped around the body, and searched most-recent-first, so a foldable call
  // inside another shadows correctly. Substitution silently stops past 16,
  // which is far beyond the small pure helpers this exists for; overflowing it
  // would mean a constexpr call chain worth looking at directly.
  CgConstInlineSub const_inline_subs[16];
  u32              const_inline_sub_count;
} Codegen;

void cg_write_c_escaped(Codegen* cg, String8 s);
void cg_expr(Codegen* cg, TypedIndex idx);
b32  cg_needs_parens_before_dot(Codegen* cg, TypedIndex idx);
void cg_block_as_expr(Codegen* cg, TypedIndex block_idx);
void cg_let_expr(Codegen* cg, TypedIndex idx);
void cg_scratch_expr(Codegen* cg, TypedIndex idx);
void cg_while_expr(Codegen* cg, TypedIndex idx);
void cg_for_range_expr(Codegen* cg, TypedIndex idx);
void cg_foreach_expr(Codegen* cg, TypedIndex idx);
void cg_parallel_expr(Codegen* cg, TypedIndex idx);
void cg_parallel_for_expr(Codegen* cg, TypedIndex idx);
void cg_call(Codegen* cg, TypedIndex idx);
void cg_struct_literal(Codegen* cg, TypedIndex idx);
void cg_array_literal(Codegen* cg, TypedIndex idx);
void cg_push_alloc(Codegen* cg, TypedIndex idx);
void cg_alloc_expr(Codegen* cg, TypedIndex idx);
void cg_push_copy(Codegen* cg, TypedIndex idx);
void cg_dyn_push(Codegen* cg, TypedIndex idx);
void cg_commit_expr(Codegen* cg, TypedIndex idx);
void cg_expr(Codegen* cg, TypedIndex idx);
void cg_function(Codegen* cg, TypedIndex idx);
void cg_function_prototype(Codegen* cg, TypedIndex idx);
void cg_struct_decl(Codegen* cg, TypedIndex idx);
void cg_struct_forward_decl(Codegen* cg, TypedIndex idx);
void cg_alias_decl(Codegen* cg, TypedIndex idx);
void cg_handle_pool_typedef(Codegen* cg, TypedIndex idx);
void cg_handle_pool_decl(Codegen* cg, TypedIndex idx);
void cg_handle_pool_storage(Codegen* cg, TypedIndex idx);
void cg_write_runtime_header(FILE* out);
void cg_write_runtime_source(FILE* out);
void cg_enum_decl(Codegen* cg, TypedIndex idx);
void cg_toplevel(Codegen* cg, TypedIndex idx);
void cg_program(Codegen* cg, TypedIndex root);
void cg_program_parallel(Codegen* cg, TypedIndex root); // see codegen.c
void cg_program_header(Codegen* cg, TypedIndex root);
String8 c_mangle_name(Arena* arena, String8 name);
String8  cg_scope_reserve(Codegen* cg, String8 name);
void     cg_scope_register(Codegen* cg, String8 source_name, String8 c_name);
String8* cg_scope_lookup(Codegen* cg, String8 name);
b32      cg_scope_is_vector_ref_param(Codegen* cg, String8 name);
u64      cg_scope_mark(Codegen* cg);
void     cg_scope_pop_to(Codegen* cg, u64 mark);

////////////////////////////////
//~ Formatter

void fmt_program(FILE* out, Ast* ast, NodeIndex root, String8 src, u32 hang);

#endif
