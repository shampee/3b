// bcgen_loop_test.c -- validates the loop-construct support (TypedNodeKind_
// WhileExpr/ForRangeExpr) added to bcgen.c on top of the earlier slices.
// Same rig as the other bcgen_*_test.c files.
//
// Exercises:
//  - `while` with a `var` accumulator AND a `var` loop counter, both
//    mutated via `set` INSIDE the loop body -- the specific "`set` inside a
//    real loop hasn't been exercised yet" gap the project memory flagged.
//  - Range `for` (unstepped, synthesized-1 step) with a `var` accumulator
//    mutated via `set` inside the body.
//  - Range `for` with an EXPLICIT step.
//  - Range `for` with an early `(return ...)` from inside the loop body
//    (via `when`), proving BcOp_Return's "halts the interpreter loop right
//    there" behavior composes correctly with a loop's own backward jump --
//    and the same function's normal (non-early) fallthrough path, when the
//    loop completes without ever hitting the early return.
//  - Range `for` over an f64 range (fractional begin/end/step), exercising
//    ForRangeExpr's f64 operand-type dispatch for `+=`/`<` (not just the
//    default integer path).
//  - `break` and `continue` (BreakExpr/ContinueExpr), whose jump targets are
//    backpatched by the enclosing loop rather than known when emitted: several
//    per body, at different nesting depths, and in nested loops where one must
//    not reach the other. A `continue` patched to the condition instead of the
//    step spins forever, so these cases fail as a HANG, not a wrong answer.
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

static void
expect_eq_f64(const char* what, i64 got_bits, f64 want) {
  f64 got; memcpy(&got, &got_bits, sizeof(got));
  if (got != want) {
    fprintf(stderr, "FAIL %s: got %g, want %g\n", what, got, want);
    g_failures += 1;
  }
}

static const char* g_fixture_source =
  "(package bcgen_loop_test)\n"
  "(fn sum-while [n i32] i32\n"
  "  (var total i32 0)\n"
  "  (var i i32 0)\n"
  "  (while (< i n)\n"
  "    (set total (+ total i))\n"
  "    (set i (+ i 1)))\n"
  "  total)\n"
  "\n"
  "(fn sum-for [n i32] i32\n"
  "  (var total i32 0)\n"
  "  (for [i 0 n]\n"
  "    (set total (+ total i)))\n"
  "  total)\n"
  "\n"
  "(fn sum-for-step [n i32] i32\n"
  "  (var total i32 0)\n"
  "  (for [i 0 n 2]\n"
  "    (set total (+ total i)))\n"
  "  total)\n"
  "\n"
  "(fn find-first-ge [n i32 threshold i32] i32\n"
  "  (for [i 0 n]\n"
  "    (when (>= i threshold) (return i)))\n"
  "  -1)\n"
  "\n"
  "(fn sum-float-range [] f64\n"
  "  (var total f64 0.0f64)\n"
  "  (for [x 0.0f64 5.0f64 1.0f64]\n"
  "    (set total (+ total x)))\n"
  "  total)\n"
  "\n"
  "(fn while-break [n i32] i32\n"
  "  (var i i32 0)\n"
  "  (while true\n"
  "    (when (>= i n) (break))\n"
  "    (set i (+ i 1)))\n"
  "  i)\n"
  "\n"
  "(fn for-continue [n i32] i32\n"
  "  (var total i32 0)\n"
  "  (for [i 0 n]\n"
  "    (when (= (% i 2) 1) (continue))\n"
  "    (set total (+ total i)))\n"
  "  total)\n"
  "\n"
  "(fn while-continue [n i32] i32\n"
  "  (var i i32 0)\n"
  "  (var total i32 0)\n"
  "  (while (< i n)\n"
  "    (set i (+ i 1))\n"
  "    (when (= (% i 3) 0) (continue))\n"
  "    (set total (+ total i)))\n"
  "  total)\n"
  "\n"
  "(fn many-exits [n i32] i32\n"
  "  (var total i32 0)\n"
  "  (for [i 0 n]\n"
  "    (when (= i 3) (break))\n"
  "    (when (= i 7) (break))\n"
  "    (let [doubled i32 (* i 2)]\n"
  "      (set total (+ total doubled))))\n"
  "  total)\n"
  "\n"
  "(fn nested-innermost-only [] i32\n"
  "  (var hits i32 0)\n"
  "  (for [i 0 3]\n"
  "    (for [j 0 10]\n"
  "      (when (>= j 2) (break))\n"
  "      (set hits (+ hits 1)))\n"
  "    (set hits (+ hits 1)))\n"
  "  hits)\n"
  "\n"
  "(fn main [] i32 0)\n";

static Checker
check_fixture(TypedAst* tast, TypedIndex* out_root) {
  Ast ast;
  ast_init(&ast, ctx_perm());

  String8 src     = str8_cstring((char*)(g_fixture_source));
  u32     file_id = source_file_register(str8_lit("bcgen_loop_test_fixture.3b"), src);

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
  u32 sum_while_fn       = bc_program_find_fn(&prog, str8_lit("sum-while"));
  u32 sum_for_fn         = bc_program_find_fn(&prog, str8_lit("sum-for"));
  u32 sum_for_step_fn    = bc_program_find_fn(&prog, str8_lit("sum-for-step"));
  u32 find_first_ge_fn   = bc_program_find_fn(&prog, str8_lit("find-first-ge"));
  u32 sum_float_range_fn = bc_program_find_fn(&prog, str8_lit("sum-float-range"));
  u32 while_break_fn     = bc_program_find_fn(&prog, str8_lit("while-break"));
  u32 for_continue_fn    = bc_program_find_fn(&prog, str8_lit("for-continue"));
  u32 while_continue_fn  = bc_program_find_fn(&prog, str8_lit("while-continue"));
  u32 many_exits_fn      = bc_program_find_fn(&prog, str8_lit("many-exits"));
  u32 nested_fn          = bc_program_find_fn(&prog, str8_lit("nested-innermost-only"));

  Arena* heap = ctx_perm();

  // ~~ `while` with a `var` accumulator AND `var` counter, both mutated via
  // `set` inside the loop body.
  { i64 args[1] = {5}; expect_eq_i64("sum-while(5)", bc_run_in_program(&prog, sum_while_fn, args, 1, heap, &host_imports).value, 10); }  // 0+1+2+3+4
  { i64 args[1] = {0}; expect_eq_i64("sum-while(0)", bc_run_in_program(&prog, sum_while_fn, args, 1, heap, &host_imports).value, 0); }   // never executes

  // ~~ Range `for`, unstepped (synthesized step 1), `var` accumulator
  // mutated via `set`.
  { i64 args[1] = {5}; expect_eq_i64("sum-for(5)", bc_run_in_program(&prog, sum_for_fn, args, 1, heap, &host_imports).value, 10); }
  { i64 args[1] = {1}; expect_eq_i64("sum-for(1)", bc_run_in_program(&prog, sum_for_fn, args, 1, heap, &host_imports).value, 0); }

  // ~~ Range `for` with an explicit step.
  { i64 args[1] = {10}; expect_eq_i64("sum-for-step(10)", bc_run_in_program(&prog, sum_for_step_fn, args, 1, heap, &host_imports).value, 20); } // 0+2+4+6+8

  // ~~ Early `return` from inside a loop body (via `when`), AND the normal
  // fallthrough path when the loop completes without ever early-returning.
  { i64 args[2] = {10, 4};  expect_eq_i64("find-first-ge(10,4) early-returns",  bc_run_in_program(&prog, find_first_ge_fn, args, 2, heap, &host_imports).value, 4); }
  { i64 args[2] = {10, 20}; expect_eq_i64("find-first-ge(10,20) falls through", bc_run_in_program(&prog, find_first_ge_fn, args, 2, heap, &host_imports).value, -1); }

  // ~~ Range `for` over an f64 range -- exercises the f64 operand-type
  // dispatch for `+=`/`<` inside ForRangeExpr, not just the integer path.
  { BcResult r = bc_run_in_program(&prog, sum_float_range_fn, NULL, 0, heap, &host_imports);
    expect_eq_f64("sum-float-range() == 0+1+2+3+4", r.value, 10.0);
  }

  // ~~ `break` and `continue`. Every check here is a timeout risk rather than
  // a wrong-answer risk if the fixups land on the wrong instruction: a
  // `continue` patched to the loop's CONDITION instead of its step re-tests an
  // unchanged counter and spins forever, so a hang in this file is the
  // signature of that bug specifically.
  { i64 args[1] = {4}; expect_eq_i64("while-break(4)", bc_run_in_program(&prog, while_break_fn, args, 1, heap, &host_imports).value, 4); }
  { i64 args[1] = {0}; expect_eq_i64("while-break(0) breaks immediately", bc_run_in_program(&prog, while_break_fn, args, 1, heap, &host_imports).value, 0); }

  { i64 args[1] = {8}; expect_eq_i64("for-continue(8) skips odds", bc_run_in_program(&prog, for_continue_fn, args, 1, heap, &host_imports).value, 12); }   // 0+2+4+6
  { i64 args[1] = {6}; expect_eq_i64("while-continue(6) skips multiples of 3", bc_run_in_program(&prog, while_continue_fn, args, 1, heap, &host_imports).value, 12); } // 1+2+4+5

  // Two `break`s in one body, plus a `continue`-free body whose live path runs
  // inside a `let`: one fixup SLOT per loop would drop the first `break`, and a
  // fixup list that isn't popped per-loop would let these reach an outer one.
  { i64 args[1] = {10}; expect_eq_i64("many-exits(10) takes the first break", bc_run_in_program(&prog, many_exits_fn, args, 1, heap, &host_imports).value, 6); } // 0+2+4
  { i64 args[1] = {2};  expect_eq_i64("many-exits(2) reaches neither break",  bc_run_in_program(&prog, many_exits_fn, args, 1, heap, &host_imports).value, 2); } // 0+2

  // A `break` in a nested loop must end the INNER loop only -- if the fixup
  // escaped to the outer one this returns 3 (one outer iteration) not 9.
  expect_eq_i64("nested-innermost-only()", bc_run_in_program(&prog, nested_fn, NULL, 0, heap, &host_imports).value, 9);

  arena_temp_end(&fn_temp);

  if (g_failures == 0) printf("bcgen_loop_test: all checks passed\n");
  else                 printf("bcgen_loop_test: %d check(s) FAILED\n", g_failures);

  ctx_free();
  return g_failures == 0 ? 0 : 1;
}
