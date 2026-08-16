// bcgen_call_test.c -- validates calls between compiled functions
// (bcgen.c's TypedNodeKind_Call handling + BcOp_Call, bcvm.c's explicit
// BcFrame stack -- see that file's own top-of-file note on why it's no
// longer C recursion). Same rig as the other bcgen_*_test.c files.
//
// Exercises: a simple non-recursive two-function call chain (square /
// sum-of-squares), real recursion (factorial/fib calling themselves) --
// proving BcFnTable's two-pass name gathering resolves a self-call
// correctly (the function's own name is registered before its body is
// even compiled) -- and RECURSION DEEP ENOUGH to actually exercise the
// explicit frame stack's push/pop bookkeeping (reg_base/dst_reg/pc
// save-restore across hundreds of live frames at once), not just a
// handful of levels a bug in that bookkeeping might not surface at.
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

static const char* g_fixture_source =
  "(package bcgen_call_test)\n"
  "(fn square [x i32] i32 (* x x))\n"
  "(fn sum-of-squares [a i32 b i32] i32 (+ (square a) (square b)))\n"
  "(fn factorial [n i32] i32\n"
  "  (if (<= n 1) 1 (* n (factorial (- n 1)))))\n"
  "(fn fib [n i32] i32\n"
  "  (if (<= n 1) n (+ (fib (- n 1)) (fib (- n 2)))))\n"
  "(fn sum-to [n i32] i32\n"
  "  (if (<= n 0) 0 (+ n (sum-to (- n 1)))))\n"
  "(fn main [] i32 0)\n";

static Checker
check_fixture(TypedAst* tast, TypedIndex* out_root) {
  Ast ast;
  ast_init(&ast, ctx_perm());

  String8 src     = str8_cstring((char*)(g_fixture_source));
  u32     file_id = source_file_register(str8_lit("bcgen_call_test_fixture.3b"), src);

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
  u32 sum_of_squares_fn = bc_program_find_fn(&prog, str8_lit("sum-of-squares"));
  u32 factorial_fn      = bc_program_find_fn(&prog, str8_lit("factorial"));
  u32 fib_fn            = bc_program_find_fn(&prog, str8_lit("fib"));
  u32 sum_to_fn         = bc_program_find_fn(&prog, str8_lit("sum-to"));

  Arena* heap = ctx_perm(); // unused by these fixtures (no structs), but required by bc_run_in_program's signature

  { i64 args[2] = {3, 4};  expect_eq_i64("sum-of-squares(3,4)", bc_run_in_program(&prog, sum_of_squares_fn, args, 2, heap, &host_imports).value, 25); } // 9+16
  { i64 args[2] = {0, 5};  expect_eq_i64("sum-of-squares(0,5)", bc_run_in_program(&prog, sum_of_squares_fn, args, 2, heap, &host_imports).value, 25); } // 0+25

  { i64 args[1] = {0};  expect_eq_i64("factorial(0)", bc_run_in_program(&prog, factorial_fn, args, 1, heap, &host_imports).value, 1); }
  { i64 args[1] = {1};  expect_eq_i64("factorial(1)", bc_run_in_program(&prog, factorial_fn, args, 1, heap, &host_imports).value, 1); }
  { i64 args[1] = {5};  expect_eq_i64("factorial(5)", bc_run_in_program(&prog, factorial_fn, args, 1, heap, &host_imports).value, 120); }
  { i64 args[1] = {10}; expect_eq_i64("factorial(10)", bc_run_in_program(&prog, factorial_fn, args, 1, heap, &host_imports).value, 3628800); }

  // Doubly-recursive (two self-calls per level) -- exercises
  // bc_run_in_program's recursion nesting more than factorial's single
  // self-call chain does.
  { i64 args[1] = {0};  expect_eq_i64("fib(0)", bc_run_in_program(&prog, fib_fn, args, 1, heap, &host_imports).value, 0); }
  { i64 args[1] = {1};  expect_eq_i64("fib(1)", bc_run_in_program(&prog, fib_fn, args, 1, heap, &host_imports).value, 1); }
  { i64 args[1] = {10}; expect_eq_i64("fib(10)", bc_run_in_program(&prog, fib_fn, args, 1, heap, &host_imports).value, 55); }

  // Deep recursion -- 900 live frames at the deepest point, well past
  // what a handful of factorial/fib calls above would ever exercise, to
  // actually stress the explicit BcFrame stack's own bookkeeping (still
  // comfortably under BC_MAX_CALL_DEPTH's 1024-frame ceiling).
  { i64 args[1] = {900};
    i64 expected = (i64)900 * 901 / 2; // Gauss sum 1..900
    expect_eq_i64("sum-to(900)", bc_run_in_program(&prog, sum_to_fn, args, 1, heap, &host_imports).value, expected);
  }
  { i64 args[1] = {0};   expect_eq_i64("sum-to(0)",  bc_run_in_program(&prog, sum_to_fn, args, 1, heap, &host_imports).value, 0); }
  { i64 args[1] = {1};   expect_eq_i64("sum-to(1)",  bc_run_in_program(&prog, sum_to_fn, args, 1, heap, &host_imports).value, 1); }

  arena_temp_end(&fn_temp);

  if (g_failures == 0) printf("bcgen_call_test: all checks passed\n");
  else                 printf("bcgen_call_test: %d check(s) FAILED\n", g_failures);

  ctx_free();
  return g_failures == 0 ? 0 : 1;
}
