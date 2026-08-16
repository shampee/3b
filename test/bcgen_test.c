// bcgen_test.c -- validates the bcgen.c/bcvm.c slice end to end: compile a
// fixture function's TypedAst into a BcChunk, run it through the
// interpreter, check the result against hand-computed expected values.
// Same rig as test/layout_test.c (hand-rolls parse -> lower -> check
// directly against an in-memory fixture, single file, no imports -- see
// that file's own note on why compile_package/compile_all_packages aren't
// used: they don't expose the live Checker past their own return).
#include "3b.h"
#include "bcgen.h"
#include "bcvm.h"
#include <stdio.h>
#include <stdlib.h>

static int g_failures = 0;

static void
expect_eq_i64(const char* what, i64 got, i64 want) {
  if (got != want) {
    fprintf(stderr, "FAIL %s: got %lld, want %lld\n", what, (long long)got, (long long)want);
    g_failures += 1;
  }
}

// ~~ Fixture: arithmetic, comparisons, `if` used as a value, `when` +
// early `return` (mid-block, not just tail position), and `let`-bound
// locals feeding each other.
static const char* g_fixture_source =
  "(package bcgen_test)\n"
  "(fn add [a i32 b i32] i32 (+ a b))\n"
  "(fn max2 [a i32 b i32] i32 (if (> a b) a b))\n"
  "(fn abs-diff [a i32 b i32] i32\n"
  "  (when (< a b) (return (- b a)))\n"
  "  (- a b))\n"
  "(fn compute [x i32] i32\n"
  "  (let [y (* x 2)\n"
  "        z (+ y 1)]\n"
  "    (+ y z)))\n"
  "(fn main [] i32 0)\n";

static Checker
check_fixture(TypedAst* tast, TypedIndex* out_root) {
  Ast ast;
  ast_init(&ast, ctx_perm());

  String8 src     = str8_cstring((char*)(g_fixture_source));
  u32     file_id = source_file_register(str8_lit("bcgen_test_fixture.3b"), src);

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
  layout_cache_init(&layout_cache, ctx_perm()); // unused by these fixtures (no structs), but
                                                    // required by bc_compile_program's own plumbing
  BcHostImportTable host_imports = {0}; // no host calls in this fixture

  BcProgram prog = bc_compile_program(&ck, &tast, root, fn_temp.arena, ctx_perm(), &layout_cache, &host_imports, NULL);
  u32 add_fn      = bc_program_find_fn(&prog, str8_lit("add"));
  u32 max2_fn     = bc_program_find_fn(&prog, str8_lit("max2"));
  u32 absdiff_fn  = bc_program_find_fn(&prog, str8_lit("abs-diff"));
  u32 compute_fn  = bc_program_find_fn(&prog, str8_lit("compute"));

  Arena* heap = ctx_perm(); // unused by these fixtures (no structs), but required by bc_run_in_program's signature

  { i64 args[2] = {3, 4};   expect_eq_i64("add(3,4)",   bc_run_in_program(&prog, add_fn, args, 2, heap, &host_imports).value, 7); }
  { i64 args[2] = {-2, 5};  expect_eq_i64("add(-2,5)",  bc_run_in_program(&prog, add_fn, args, 2, heap, &host_imports).value, 3); }

  { i64 args[2] = {3, 7};   expect_eq_i64("max2(3,7)",  bc_run_in_program(&prog, max2_fn, args, 2, heap, &host_imports).value, 7); }
  { i64 args[2] = {9, 2};   expect_eq_i64("max2(9,2)",  bc_run_in_program(&prog, max2_fn, args, 2, heap, &host_imports).value, 9); }
  { i64 args[2] = {5, 5};   expect_eq_i64("max2(5,5)",  bc_run_in_program(&prog, max2_fn, args, 2, heap, &host_imports).value, 5); }

  { i64 args[2] = {10, 3};  expect_eq_i64("abs-diff(10,3)", bc_run_in_program(&prog, absdiff_fn, args, 2, heap, &host_imports).value, 7); }
  { i64 args[2] = {3, 10};  expect_eq_i64("abs-diff(3,10)", bc_run_in_program(&prog, absdiff_fn, args, 2, heap, &host_imports).value, 7); } // exercises the early-return path
  { i64 args[2] = {5, 5};   expect_eq_i64("abs-diff(5,5)",  bc_run_in_program(&prog, absdiff_fn, args, 2, heap, &host_imports).value, 0); }

  { i64 args[1] = {5};      expect_eq_i64("compute(5)",  bc_run_in_program(&prog, compute_fn, args, 1, heap, &host_imports).value, 21); } // y=10 z=11 -> 21
  { i64 args[1] = {0};      expect_eq_i64("compute(0)",  bc_run_in_program(&prog, compute_fn, args, 1, heap, &host_imports).value, 1); }  // y=0  z=1  -> 1

  arena_temp_end(&fn_temp);

  if (g_failures == 0) printf("bcgen_test: all checks passed\n");
  else                 printf("bcgen_test: %d check(s) FAILED\n", g_failures);

  ctx_free();
  return g_failures == 0 ? 0 : 1;
}
