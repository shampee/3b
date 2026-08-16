// bcgen_scope_test.c -- validates a real scoping/shadowing bug fix in
// bcgen.c: bc_local_try_lookup does a BACKWARD scan over fc->locals (most-
// recently-bound name wins), but nothing used to POP a name back off that
// array when the BLOCK that bound it closed -- so a name shadowed inside
// a nested `do`/`let`/`for`/`scratch` stayed "the most recent" binding for
// that name FOREVER after, even once execution left that block, silently
// resolving a later OUTER reference to the SAME name to the wrong (out-of-
// scope, stale) register instead of the outer variable's own. Found by
// running examples/control-flow/main.3b (its `scoped-shadow` function)
// through the bytecode VM via `3b run` -- previously unreachable at
// runtime because an UNRELATED compile error elsewhere in that same file
// aborted compilation first (see the `abs`/`min`/`max`/`clamp` family
// added alongside this fix); fixing that unblocked this test and this bug
// then reproduced immediately.
//
// Fixed by truncating fc->locals back to a mark taken before ANY binding
// a scope introduces, at every place bcgen.c binds a name OUTSIDE a plain
// TypedNodeKind_Block: bc_compile_block itself (covers `do`/`if`-branch-
// as-Block/while/for bodies uniformly, one shared choke point), PLUS
// TypedNodeKind_LetExpr/ForRangeExpr/ForEachExpr/ScratchExpr individually
// (each binds a name BEFORE calling bc_compile_block, so relying on
// bc_compile_block's OWN truncation alone doesn't cover THEIR OWN
// binding -- confirmed necessary via the `let`-as-a-bare-if-branch case
// below, which never touches bc_compile_block at the LetExpr's own level
// at all).
//
// Exercises, each proving the OUTER variable is correctly restored after
// the shadowing scope closes:
//  - `do` nested inside an `if` branch (the ORIGINAL reported case).
//  - `let` as an ordinary block statement.
//  - `let` as a BARE `if`-branch expression (not wrapped in its own
//    Block) -- the case bc_compile_block's own fix alone would NOT catch.
//  - `for` (range form)'s own loop variable.
//  - `for` (collection form)'s own element variable, PLUS proves the loop
//    still iterates/computes correctly (not just that shadowing itself is
//    fixed).
//  - `scratch`'s own arena-handle variable.
//  - TWO SIBLING blocks shadowing the SAME name, proving the second
//    doesn't confusingly resolve to the first's now-closed binding.
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
  "(package bcgen_scope_test)\n"
  "\n"
  "(fn shadow-in-do [] i32\n"
  "  (var total i32 111)\n"
  "  (if (> total 0)\n"
  "    (do (var total i32 99) total)\n"
  "    0)\n"
  "  total)\n"
  "\n"
  "(fn shadow-in-let-block [] i32\n"
  "  (var x i32 111)\n"
  "  (let [x 99] x)\n"
  "  x)\n"
  "\n"
  "(fn shadow-in-let-if-branch [] i32\n"
  "  (var x i32 111)\n"
  "  (if (> x 0) (let [x 99] x) 0)\n"
  "  x)\n"
  "\n"
  "(fn shadow-in-for-range [] i32\n"
  "  (var i i32 111)\n"
  "  (for [i 0 5] 0)\n"
  "  i)\n"
  "\n"
  "(fn shadow-in-for-each [] i32\n"
  "  (var x i32 111)\n"
  "  (var arr [i32 3] [1 2 3])\n"
  "  (var sum i32 0)\n"
  "  (for [x arr] (set sum (+ sum x)))\n"
  "  (+ (* x 1000) sum))\n"
  "\n"
  "(fn shadow-in-scratch [] i32\n"
  "  (var t i32 111)\n"
  "  (scratch [t] 0)\n"
  "  t)\n"
  "\n"
  "(fn sibling-shadows [] i32\n"
  "  (var x i32 111)\n"
  "  (do (var x i32 1) x)\n"
  "  (do (var x i32 2) x)\n"
  "  x)\n"
  "\n"
  "(fn main [] i32 0)\n";

static Checker
check_fixture(TypedAst* tast, TypedIndex* out_root) {
  Ast ast;
  ast_init(&ast, ctx_perm());

  String8 src     = str8_cstring((char*)(g_fixture_source));
  u32     file_id = source_file_register(str8_lit("bcgen_scope_test_fixture.3b"), src);

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
  Checker ck = check_program(tast, own_root, /*is_root_package=*/true, /*scope_query=*/NULL);
  if (ck.had_error) { fprintf(stderr, "FATAL: fixture failed to type-check\n"); exit(1); }
  return ck;
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

  u32 shadow_do_fn           = bc_program_find_fn(&prog, str8_lit("shadow-in-do"));
  u32 shadow_let_block_fn     = bc_program_find_fn(&prog, str8_lit("shadow-in-let-block"));
  u32 shadow_let_if_fn        = bc_program_find_fn(&prog, str8_lit("shadow-in-let-if-branch"));
  u32 shadow_for_range_fn     = bc_program_find_fn(&prog, str8_lit("shadow-in-for-range"));
  u32 shadow_for_each_fn      = bc_program_find_fn(&prog, str8_lit("shadow-in-for-each"));
  u32 shadow_scratch_fn       = bc_program_find_fn(&prog, str8_lit("shadow-in-scratch"));
  u32 sibling_shadows_fn      = bc_program_find_fn(&prog, str8_lit("sibling-shadows"));

  Arena* heap = ctx_perm();

  expect_eq_i64("shadow-in-do()",             bc_run_in_program(&prog, shadow_do_fn,         NULL, 0, heap, &host_imports).value, 111);
  expect_eq_i64("shadow-in-let-block()",       bc_run_in_program(&prog, shadow_let_block_fn,   NULL, 0, heap, &host_imports).value, 111);
  expect_eq_i64("shadow-in-let-if-branch()",   bc_run_in_program(&prog, shadow_let_if_fn,      NULL, 0, heap, &host_imports).value, 111);
  expect_eq_i64("shadow-in-for-range()",       bc_run_in_program(&prog, shadow_for_range_fn,   NULL, 0, heap, &host_imports).value, 111);
  expect_eq_i64("shadow-in-for-each()",        bc_run_in_program(&prog, shadow_for_each_fn,    NULL, 0, heap, &host_imports).value, 111006); // outer x(111)*1000 + sum(1+2+3=6)
  expect_eq_i64("shadow-in-scratch()",         bc_run_in_program(&prog, shadow_scratch_fn,     NULL, 0, heap, &host_imports).value, 111);
  expect_eq_i64("sibling-shadows()",           bc_run_in_program(&prog, sibling_shadows_fn,    NULL, 0, heap, &host_imports).value, 111);

  arena_temp_end(&fn_temp);

  if (g_failures == 0) printf("bcgen_scope_test: all checks passed\n");
  else                 printf("bcgen_scope_test: %d check(s) FAILED\n", g_failures);

  ctx_free();
  return g_failures == 0 ? 0 : 1;
}
