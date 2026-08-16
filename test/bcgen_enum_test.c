// bcgen_enum_test.c -- validates `EnumAccess` (`Name/Variant`) support
// added to bcgen.c: bc_enum_variant_value replicates codegen.c's own
// cg_enum_decl auto-assignment algorithm at COMPILE time (a pure constant,
// no new opcodes). Same rig as the other bcgen_*_test.c files.
//
// Exercises:
//  - Plain `enum`: sequential auto-assignment starting at 0.
//  - `enum` with an EXPLICIT value partway through, auto-assignment for
//    what follows continuing FROM it (not restarting at 0 or continuing
//    from the previous auto value).
//  - `flags`: bit POSITION auto-assignment (1, 2, 4, ...), not sequential
//    integers.
//  - Two SEPARATE enum/flags types in the same program, proving variant
//    resolution is keyed by (enum name, variant name) together, not
//    variant name alone (both declare a variant named `A`, with DIFFERENT
//    values).
//  - An enum type used EVERYWHERE a Named type is otherwise assumed to be
//    a STRUCT (bc_field_is_embedded/bc_load_op_for_type/bc_store_op_for_type/
//    bc_compile_value_cmp/bc_store_value/call-argument-copy/struct-field-
//    fill all used to treat TypeKind_Named as unconditionally struct-
//    shaped -- an enum-typed `var`/`val` crashed bcgen.c outright, see the
//    bcgen-vector-nth-fix-style project memory for this fix): a `var`/`val`
//    with an EXPLICIT enum type annotation, an enum-typed STRUCT field
//    (both filled from a literal and read back), an enum-typed fixed-size
//    ARRAY (`nth` read AND `set`-write), an enum function PARAMETER, and
//    `=`/`!=` comparison on enum values both directly and through a
//    struct field (the struct-field case specifically exercises
//    bc_compile_value_cmp's own enum-vs-struct branch, not just the
//    top-level dispatch that routes a DIRECT enum comparison around it
//    entirely).
#include "3b.h"
#include "bcgen.h"
#include "bcvm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;

static void
expect_eq_i64(const char* what, i64 got, i64 want) {
  if (got != want) {
    fprintf(stderr, "FAIL %s: got %lld, want %lld\n", what, (long long)got, (long long)want);
    g_failures += 1;
  }
}

static const char* g_fixture_source =
  "(package bcgen_enum_test)\n"
  "\n"
  "(enum Color [Red Green Blue])\n"
  "(enum Explicit [A B 10 C])\n"
  "(flags Perm [Read Write Exec])\n"
  "(enum OtherA [X A Y])\n"
  "\n"
  "(fn color-red [] i32 (cast i32 Color/Red))\n"
  "(fn color-green [] i32 (cast i32 Color/Green))\n"
  "(fn color-blue [] i32 (cast i32 Color/Blue))\n"
  "\n"
  "(fn explicit-a [] i32 (cast i32 Explicit/A))\n"
  "(fn explicit-b [] i32 (cast i32 Explicit/B))\n"
  "(fn explicit-c [] i32 (cast i32 Explicit/C))\n"
  "\n"
  "(fn perm-read [] i32 (cast i32 Perm/Read))\n"
  "(fn perm-write [] i32 (cast i32 Perm/Write))\n"
  "(fn perm-exec [] i32 (cast i32 Perm/Exec))\n"
  "\n"
  "(fn other-a [] i32 (cast i32 OtherA/A))\n"
  "\n"
  "(struct Item [name string kind Color])\n"
  "\n"
  "(fn describe [c Color] i32 (cast i32 c))\n"
  "\n"
  "(fn var-val-roundtrip [] i32\n"
  "  (val v Color Color/Green)\n"
  "  (var w Color Color/Blue)\n"
  "  (+ (cast i32 v) (cast i32 w)))\n"
  "\n"
  "(fn struct-field-roundtrip [] i32\n"
  "  (val it Item (Item {:name \"widget\" :kind Color/Green}))\n"
  "  (cast i32 (. it kind)))\n"
  "\n"
  "(fn array-roundtrip [] i32\n"
  "  (var arr [Color 3] [Color/Red Color/Green Color/Blue])\n"
  "  (set (nth arr 0) Color/Blue)\n"
  "  (+ (* (cast i32 (nth arr 0)) 100) (cast i32 (nth arr 2))))\n"
  "\n"
  "(fn param-roundtrip [] i32 (describe Color/Blue))\n"
  "\n"
  "(fn eq-roundtrip [] bool (= Color/Green Color/Green))\n"
  "(fn neq-roundtrip [] bool (!= Color/Green Color/Blue))\n"
  "(fn struct-field-eq [] bool\n"
  "  (val it Item (Item {:name \"widget\" :kind Color/Green}))\n"
  "  (= (. it kind) Color/Green))\n"
  "\n"
  "(fn main [] i32 0)\n";

static Checker
check_fixture(TypedAst* tast, TypedIndex* out_root) {
  Ast ast;
  ast_init(&ast, ctx_perm());

  String8 src     = str8_cstring((char*)(g_fixture_source));
  u32     file_id = source_file_register(str8_lit("bcgen_enum_test_fixture.3b"), src);

  Parser p;
  parser_init(&p, src, &ast, file_id);
  NodeIndex root = parse_program(&p);
  if (p.had_error) { fprintf(stderr, "FATAL: fixture failed to parse\n"); exit(1); }

  u16        form_count;
  NodeIndex* forms = ast_seq_children(&ast, root, &form_count);
  Token synth_open = {0};
  synth_open.line  = 1;
  synth_open.col   = 1;
  NodeIndex combined_root = ast_push_seq(&ast, AstNodeKind_List, synth_open, forms + 1, (u16)(form_count - 1));

  typed_ast_init(tast, ctx_perm());
  Lowerer low = {0};
  low.ast  = &ast;
  low.tast = tast;
  TypedIndex own_root = lower_program(&low, combined_root);
  if (low.had_error) { fprintf(stderr, "FATAL: fixture failed to lower\n"); exit(1); }

  *out_root = own_root;
  return check_program(tast, own_root, /*is_root_package=*/true, /*scope_query=*/NULL);
}

int
main(void) {
  Context ctx;
  ctx_init(&ctx, MB(16));

  TypedAst   tast;
  TypedIndex root;
  Checker    ck = check_fixture(&tast, &root);
  xassert(tast.nodes[root].kind == TypedNodeKind_Block);

  ArenaTemp   fn_temp = arena_temp_begin(ctx_scratch());
  LayoutCache layout_cache;
  layout_cache_init(&layout_cache, ctx_perm());
  BcHostImportTable host_imports = {0}; // no host calls in this fixture

  BcProgram prog = bc_compile_program(&ck, &tast, root, fn_temp.arena, ctx_perm(), &layout_cache, &host_imports, NULL);
  xassert(prog.ok);

  u32 color_red_fn    = bc_program_find_fn(&prog, str8_lit("color-red"));
  u32 color_green_fn  = bc_program_find_fn(&prog, str8_lit("color-green"));
  u32 color_blue_fn   = bc_program_find_fn(&prog, str8_lit("color-blue"));
  u32 explicit_a_fn    = bc_program_find_fn(&prog, str8_lit("explicit-a"));
  u32 explicit_b_fn    = bc_program_find_fn(&prog, str8_lit("explicit-b"));
  u32 explicit_c_fn    = bc_program_find_fn(&prog, str8_lit("explicit-c"));
  u32 perm_read_fn     = bc_program_find_fn(&prog, str8_lit("perm-read"));
  u32 perm_write_fn    = bc_program_find_fn(&prog, str8_lit("perm-write"));
  u32 perm_exec_fn     = bc_program_find_fn(&prog, str8_lit("perm-exec"));
  u32 other_a_fn        = bc_program_find_fn(&prog, str8_lit("other-a"));
  u32 var_val_fn         = bc_program_find_fn(&prog, str8_lit("var-val-roundtrip"));
  u32 struct_field_fn    = bc_program_find_fn(&prog, str8_lit("struct-field-roundtrip"));
  u32 array_fn            = bc_program_find_fn(&prog, str8_lit("array-roundtrip"));
  u32 param_fn             = bc_program_find_fn(&prog, str8_lit("param-roundtrip"));
  u32 eq_fn                = bc_program_find_fn(&prog, str8_lit("eq-roundtrip"));
  u32 neq_fn               = bc_program_find_fn(&prog, str8_lit("neq-roundtrip"));
  u32 struct_field_eq_fn  = bc_program_find_fn(&prog, str8_lit("struct-field-eq"));

  Arena* heap = ctx_perm();

  // ~~ Plain `enum` -- sequential auto-assignment starting at 0.
  expect_eq_i64("Color/Red",   bc_run_in_program(&prog, color_red_fn,   NULL, 0, heap, &host_imports).value, 0);
  expect_eq_i64("Color/Green", bc_run_in_program(&prog, color_green_fn, NULL, 0, heap, &host_imports).value, 1);
  expect_eq_i64("Color/Blue",  bc_run_in_program(&prog, color_blue_fn,  NULL, 0, heap, &host_imports).value, 2);

  // ~~ Explicit value partway through -- auto-assignment for what
  // follows continues FROM it.
  expect_eq_i64("Explicit/A", bc_run_in_program(&prog, explicit_a_fn, NULL, 0, heap, &host_imports).value, 0);
  expect_eq_i64("Explicit/B", bc_run_in_program(&prog, explicit_b_fn, NULL, 0, heap, &host_imports).value, 10);
  expect_eq_i64("Explicit/C", bc_run_in_program(&prog, explicit_c_fn, NULL, 0, heap, &host_imports).value, 11);

  // ~~ `flags` -- bit POSITION auto-assignment, not sequential integers.
  expect_eq_i64("Perm/Read",  bc_run_in_program(&prog, perm_read_fn,  NULL, 0, heap, &host_imports).value, 1);
  expect_eq_i64("Perm/Write", bc_run_in_program(&prog, perm_write_fn, NULL, 0, heap, &host_imports).value, 2);
  expect_eq_i64("Perm/Exec",  bc_run_in_program(&prog, perm_exec_fn,  NULL, 0, heap, &host_imports).value, 4);

  // ~~ Two SEPARATE enum types sharing a variant NAME (`A`) resolve to
  // their OWN distinct values -- keyed by (enum, variant) together, not
  // variant name alone (`Explicit/A` == 0, `OtherA/A` == 1 -- different
  // declaration positions within each enum).
  expect_eq_i64("Explicit/A (re-check)", bc_run_in_program(&prog, explicit_a_fn, NULL, 0, heap, &host_imports).value, 0);
  expect_eq_i64("OtherA/A",               bc_run_in_program(&prog, other_a_fn,   NULL, 0, heap, &host_imports).value, 1);

  // ~~ An enum-typed `var`/`val` -- used to crash bcgen.c outright (see
  // this file's top-of-file note) ~~
  expect_eq_i64("var-val-roundtrip()", bc_run_in_program(&prog, var_val_fn, NULL, 0, heap, &host_imports).value, 3); // Green(1)+Blue(2)

  // ~~ enum-typed STRUCT field, filled from a literal and read back ~~
  expect_eq_i64("struct-field-roundtrip()", bc_run_in_program(&prog, struct_field_fn, NULL, 0, heap, &host_imports).value, 1); // Green

  // ~~ enum-typed ARRAY -- `nth` read AND `set`-write ~~
  expect_eq_i64("array-roundtrip()", bc_run_in_program(&prog, array_fn, NULL, 0, heap, &host_imports).value, 202); // Blue(2)*100 + Blue(2)

  // ~~ enum function PARAMETER (by-value copy machinery must not run) ~~
  expect_eq_i64("param-roundtrip()", bc_run_in_program(&prog, param_fn, NULL, 0, heap, &host_imports).value, 2); // Blue

  // ~~ `=`/`!=` on enum values, direct AND through a struct field (the
  // struct-field case alone exercises bc_compile_value_cmp's own
  // enum-vs-struct branch) ~~
  expect_eq_i64("eq-roundtrip()",         bc_run_in_program(&prog, eq_fn,             NULL, 0, heap, &host_imports).value, 1);
  expect_eq_i64("neq-roundtrip()",        bc_run_in_program(&prog, neq_fn,            NULL, 0, heap, &host_imports).value, 1);
  expect_eq_i64("struct-field-eq()",      bc_run_in_program(&prog, struct_field_eq_fn, NULL, 0, heap, &host_imports).value, 1);

  arena_temp_end(&fn_temp);

  if (g_failures == 0) printf("bcgen_enum_test: all checks passed\n");
  else                 printf("bcgen_enum_test: %d check(s) FAILED\n", g_failures);

  ctx_free();
  return g_failures == 0 ? 0 : 1;
}
