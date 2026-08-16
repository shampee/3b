// unknown_type_test.c -- validates check_type_annotations (checker.c): a type
// annotation naming a type the package never declares must be rejected, at the
// annotation, instead of being handed to codegen as if it were real.
//
// This is a regression test for a gap, not a hypothetical. `(struct Holder [p
// NoSuchType*])` used to type-check clean and come back as gcc's "unknown type
// name 'NoSuchType'" against output/*.h -- a diagnostic pointing at generated
// C, several steps downstream of the .3b line that caused it. The
// function-pointer case was worse than that: `i32 (*cb)(Missing);` is a legal
// C declaration, an old-style identifier list, so gcc accepted it silently and
// the parameter type was simply lost.
//
// The `expect_accepted` half is the half that constrains the design. The check
// has to resolve everything legitimate before it rejects anything: primitives,
// declarations later in the file, aliases, handles, pointers, collections,
// function-pointer types, and `(const T*)`. Rejecting one of those would be a
// worse regression than the hole it closed, and every one of them appears in
// the translated C bindings under examples/.
//
// Same rig as struct_cycle_test.c -- hand-rolled parse -> lower -> check
// against an in-memory fixture, one Checker per case, no imports, no files on
// disk. Qualified `pkg/Type` names are therefore out of reach here; they
// resolve through the same table, populated by splice_public_decl, and are
// covered by building examples/game3d and friends.
#include "3b.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;

// Runs one fixture through parse -> lower -> check, returning the diagnostics
// check_program produced. A parse or lower failure is fatal: every fixture
// here is meant to reach the CHECKER, so stopping earlier means the fixture is
// wrong, which would otherwise masquerade as the checker rejecting a name.
static Diagnostic*
check_source(const char* name, const char* source, u64* out_count) {
  Ast ast;
  ast_init(&ast, ctx_perm());

  String8 src     = str8_cstring((char*)source);
  u32     file_id = source_file_register(str8_cstring((char*)name), src);

  Parser p;
  parser_init(&p, src, &ast, file_id);
  NodeIndex root = parse_program(&p);
  if (p.had_error) { fprintf(stderr, "FATAL: fixture `%s` failed to parse\n", name); exit(1); }

  // Drop the leading `(package ...)` form, as struct_cycle_test.c does --
  // there are no imports here, so that is all compile_package's own stripping
  // amounts to.
  u16        form_count;
  NodeIndex* forms = ast_seq_children(&ast, root, &form_count);
  Token synth_open = {0};
  synth_open.line  = 1;
  synth_open.col   = 1;
  NodeIndex combined_root = ast_push_seq(&ast, AstNodeKind_List, synth_open, forms + 1, (u16)(form_count - 1));

  TypedAst tast;
  typed_ast_init(&tast, ctx_perm());
  Lowerer low = {0};
  low.ast  = &ast;
  low.tast = &tast;
  TypedIndex own_root = lower_program(&low, combined_root);
  if (low.had_error) { fprintf(stderr, "FATAL: fixture `%s` failed to lower\n", name); exit(1); }

  diag_capture_begin(/*also_print=*/false);
  check_program(&tast, own_root, /*is_root_package=*/true, /*scope_query=*/NULL);
  return diag_capture_end(out_count);
}

static b32
any_diag_contains(Diagnostic* diags, u64 count, const char* needle) {
  foreach_index(i, count) {
    if (strstr((const char*)diags[i].message.str, needle) != NULL) return true;
  }
  return false;
}

// A fixture that must be rejected, with a diagnostic naming the offending
// type. Matching on the name and not just on "some error happened" is what
// separates this check from the unrelated errors a broken fixture would
// produce -- `cast Missing` is rejected by the cast-legality check too, but
// for a different reason and with a different message.
static void
expect_unknown_type(const char* what, const char* type_name, const char* source) {
  u64         count = 0;
  Diagnostic* diags = check_source(what, source, &count);
  char        needle[128];
  snprintf(needle, sizeof(needle), "unknown type `%s`", type_name);
  if (!any_diag_contains(diags, count, needle)) {
    fprintf(stderr, "FAIL %s: no `%s` diagnostic; got %llu:\n", what, needle, (unsigned long long)count);
    foreach_index(i, count) fprintf(stderr, "    %.*s\n", str8_varg(diags[i].message));
    g_failures += 1;
  }
}

// A fixture that must be accepted, with no diagnostics at all.
static void
expect_accepted(const char* what, const char* source) {
  u64         count = 0;
  Diagnostic* diags = check_source(what, source, &count);
  if (count != 0) {
    fprintf(stderr, "FAIL %s: expected no diagnostics, got %llu:\n", what, (unsigned long long)count);
    foreach_index(i, count) fprintf(stderr, "    %.*s\n", str8_varg(diags[i].message));
    g_failures += 1;
  }
}

int
main(void) {
  Context ctx;
  ctx_init(&ctx, MB(64));

  // ~~ Rejected: every position an undeclared name can be written in.

  expect_unknown_type("struct field, by value", "AlsoMissing",
    "(package t)\n"
    "(struct Holder [q AlsoMissing])\n"
    "(fn main [] i32 0)\n");

  // The original report: a pointer field. The name is one level down from the
  // annotation's own TypeRef, which is what the walk exists for.
  expect_unknown_type("struct field, by pointer", "NoSuchType",
    "(package t)\n"
    "(struct Holder [p NoSuchType*])\n"
    "(fn main [] i32 0)\n");

  expect_unknown_type("function parameter", "Missing",
    "(package t)\n"
    "(fn f [x Missing] i32 0)\n"
    "(fn main [] i32 0)\n");

  expect_unknown_type("function return type", "Missing",
    "(package t)\n"
    "(fn f [] Missing* nil)\n"
    "(fn main [] i32 0)\n");

  expect_unknown_type("global annotation", "Missing",
    "(package t)\n"
    "(var g Missing* nil)\n"
    "(fn main [] i32 0)\n");

  expect_unknown_type("local annotation", "Missing",
    "(package t)\n"
    "(fn main [] i32 (do (var y Missing* nil) 0))\n");

  expect_unknown_type("Vector element", "Missing",
    "(package t)\n"
    "(struct H [v [Missing]])\n"
    "(fn main [] i32 0)\n");

  expect_unknown_type("fixed-size array element", "Missing",
    "(package t)\n"
    "(struct H [a [Missing 4]])\n"
    "(fn main [] i32 0)\n");

  expect_unknown_type("Map value", "Missing",
    "(package t)\n"
    "(struct H [m {string Missing}])\n"
    "(fn main [] i32 0)\n");

  // The case gcc could not have caught for us: an old-style C identifier list.
  expect_unknown_type("function-pointer parameter", "Missing",
    "(package t)\n"
    "(struct H [cb (fn [a Missing] i32)])\n"
    "(fn main [] i32 0)\n");

  expect_unknown_type("function-pointer return type", "Missing",
    "(package t)\n"
    "(struct H [cb (fn [a i32] Missing*)])\n"
    "(fn main [] i32 0)\n");

  expect_unknown_type("const pointer parameter", "Missing",
    "(package t)\n"
    "(fn f [p (const Missing*)] i32 0)\n"
    "(fn main [] i32 0)\n");

  // A cast target is lowered as an expression, not through lower_type_node, so
  // it carries no TypeAnnotation and needs its own check. Pointer-to-pointer
  // is legal whatever it points at, so the cast-legality check cannot catch
  // this one on its own.
  expect_unknown_type("cast target", "Missing",
    "(package t)\n"
    "(fn main [] i32 (do (cast Missing* nil) 0))\n");

  expect_unknown_type("reinterpret target", "Missing",
    "(package t)\n"
    "(fn main [] i32 (do (reinterpret Missing* nil) 0))\n");

  // A declared name of the wrong KIND is still not a type. `fns` and types
  // live in separate tables, so nothing would have resolved this either.
  expect_unknown_type("name declared as a function, not a type", "Foo",
    "(package t)\n"
    "(fn Foo [] i32 0)\n"
    "(struct H [x Foo])\n"
    "(fn main [] i32 0)\n");

  // ~~ Accepted: everything legitimate, in the same positions.

  expect_accepted("primitives, including the FFI and resource ones",
    "(package t)\n"
    "(struct H [a i32 b u64 c f32 d bool e char f string g any h arena i stream])\n"
    "(fn main [] i32 0)\n");

  // Top-level declarations are forward-visible, so a field may name a struct
  // its own declaration precedes -- the check runs after the name tables are
  // built precisely so declaration order does not matter.
  expect_accepted("struct declared after its use",
    "(package t)\n"
    "(struct H [x Later])\n"
    "(struct Later [n i32])\n"
    "(fn main [] i32 0)\n");

  expect_accepted("enum declared after its use",
    "(package t)\n"
    "(struct H [k Kind])\n"
    "(enum Kind [A 0 B 1])\n"
    "(fn main [] i32 0)\n");

  // An alias is substituted away during lowering, so `Id` never reaches the
  // checker as a name at all -- but an alias to a STRUCT does, as that
  // struct's name.
  expect_accepted("aliases, to a primitive and to a struct",
    "(package t)\n"
    "(alias Id u32)\n"
    "(alias PointRef Point*)\n"
    "(struct Point [x f32 y f32])\n"
    "(struct H [i Id p PointRef])\n"
    "(fn main [] i32 0)\n");

  expect_accepted("pointers and collections of declared types",
    "(package t)\n"
    "(struct Point [x f32 y f32])\n"
    "(enum Kind [A 0 B 1])\n"
    "(struct H [p Point* ps [Point] grid [Point 4] by-name {string Point} kinds [Kind]])\n"
    "(fn main [] i32 0)\n");

  expect_accepted("function-pointer types over declared types",
    "(package t)\n"
    "(struct Point [x f32 y f32])\n"
    "(struct H [cb (fn [p Point* k i32] Point*)])\n"
    "(fn main [] i32 0)\n");

  // `T^` is validated against the handle-pool table during lowering and is not
  // a Named type by the time the checker sees it. It must not be re-judged
  // against the struct table under a name that never appears there.
  expect_accepted("handle types",
    "(package t)\n"
    "(struct Mesh [n i32])\n"
    "(handle Mesh)\n"
    "(struct H [m Mesh^])\n"
    "(fn main [] i32 0)\n");

  // The shape every translated C binding is full of: a `(const T*)` parameter
  // over a mirrored struct.
  expect_accepted("const pointer parameters and struct returns",
    "(package t)\n"
    "(struct Point [x f32 y f32])\n"
    "(fn f [p (const Point*)] Point* p)\n"
    "(fn main [] i32 0)\n");

  expect_accepted("casts and reinterprets to declared types",
    "(package t)\n"
    "(struct Point [x f32 y f32])\n"
    "(enum Kind [A 0 B 1])\n"
    "(fn main [] i32\n"
    "  (do (cast Point* nil)\n"
    "      (cast Kind 0)\n"
    "      (reinterpret Point* nil)\n"
    "      0))\n");

  ctx_free();
  if (g_failures > 0) {
    fprintf(stderr, "unknown_type_test: %d failure(s)\n", g_failures);
    return 1;
  }
  printf("unknown_type_test: all checks passed\n");
  return 0;
}
