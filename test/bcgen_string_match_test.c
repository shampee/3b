// bcgen_string_match_test.c -- validates the `(string-match a b flags)`
// builtin added to bcgen.c/bcvm.c (BcOp_StringMatch), a real gap found by
// running examples/{anon-struct-params,arenas,structs}/main.3b through the
// bytecode VM -- previously "no compiled function or registered host
// import by that name" for any script/example calling it. Unlike
// BcOp_StrCmp (reimplemented inline to avoid pulling in a generated-
// program-only runtime file), this opcode calls base.h's OWN str8_match
// directly, since that's compiler-side infrastructure already linked into
// the interpreter binary -- this test is also, incidentally, the first
// real end-to-end proof that call actually reaches the right function
// with the right bits.
//
// Exercises:
//  - An exact match (flags 0) and a genuine mismatch, both with string
//    LITERAL operands.
//  - Case-INSENSITIVE match (StringMatchFlag_CaseInsensitive) succeeding
//    where a flags-0 comparison of the same two strings would fail --
//    proves the flags argument actually reaches str8_match, not just
//    "some string comparison" ignoring it.
//  - `flags` as a RUNTIME parameter (not a compile-time-constant literal)
//    -- proves BcOp_StringMatch's 3-register contiguous-block argument
//    convention (str_a_addr, str_b_addr, flags) works for an arbitrary
//    expression in the flags slot, not just literal 0/1.
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
  "(package bcgen_string_match_test)\n"
  "\n"
  "(fn exact-match [] bool (string-match \"hello\" \"hello\" 0))\n"
  "(fn exact-mismatch [] bool (string-match \"hello\" \"world\" 0))\n"
  "(fn case-insensitive-match [] bool (string-match \"Hello\" \"hello\" 1))\n"
  "(fn case-sensitive-mismatch [] bool (string-match \"Hello\" \"hello\" 0))\n"
  "(fn with-runtime-flags [flags i32] bool (string-match \"Hello\" \"hello\" flags))\n"
  "\n"
  "(fn main [] i32 0)\n";

static Checker
check_fixture(TypedAst* tast, TypedIndex* out_root) {
  Ast ast;
  ast_init(&ast, ctx_perm());

  String8 src     = str8_cstring((char*)(g_fixture_source));
  u32     file_id = source_file_register(str8_lit("bcgen_string_match_test_fixture.3b"), src);

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

  u32 exact_match_fn        = bc_program_find_fn(&prog, str8_lit("exact-match"));
  u32 exact_mismatch_fn      = bc_program_find_fn(&prog, str8_lit("exact-mismatch"));
  u32 ci_match_fn             = bc_program_find_fn(&prog, str8_lit("case-insensitive-match"));
  u32 cs_mismatch_fn          = bc_program_find_fn(&prog, str8_lit("case-sensitive-mismatch"));
  u32 runtime_flags_fn        = bc_program_find_fn(&prog, str8_lit("with-runtime-flags"));

  Arena* heap = ctx_perm();

  expect_eq_i64("exact-match()",              bc_run_in_program(&prog, exact_match_fn,   NULL, 0, heap, &host_imports).value, 1);
  expect_eq_i64("exact-mismatch()",           bc_run_in_program(&prog, exact_mismatch_fn, NULL, 0, heap, &host_imports).value, 0);
  expect_eq_i64("case-insensitive-match()",   bc_run_in_program(&prog, ci_match_fn,       NULL, 0, heap, &host_imports).value, 1);
  expect_eq_i64("case-sensitive-mismatch()",  bc_run_in_program(&prog, cs_mismatch_fn,    NULL, 0, heap, &host_imports).value, 0);

  { i64 args[1] = {0};
    expect_eq_i64("with-runtime-flags(0)", bc_run_in_program(&prog, runtime_flags_fn, args, 1, heap, &host_imports).value, 0); }
  { i64 args[1] = {1};
    expect_eq_i64("with-runtime-flags(1)", bc_run_in_program(&prog, runtime_flags_fn, args, 1, heap, &host_imports).value, 1); }

  arena_temp_end(&fn_temp);

  if (g_failures == 0) printf("bcgen_string_match_test: all checks passed\n");
  else                 printf("bcgen_string_match_test: %d check(s) FAILED\n", g_failures);

  ctx_free();
  return g_failures == 0 ? 0 : 1;
}
