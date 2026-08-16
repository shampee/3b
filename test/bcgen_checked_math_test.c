// bcgen_checked_math_test.c -- validates `sqrt-checked`/`asin-checked`/
// `acos-checked`/`pow-checked` (TypedNodeKind_CheckedMath) support added
// to bcgen.c/bcvm.c: real libm calls (BcOp_Sqrt/Asin/Acos/Pow, always
// computed in f64 -- see those opcodes' own bytecode.h comment on the
// narrower-than-codegen.c f32-precision tradeoff) plus BcOp_F64IsFinite
// building the synthesized `(bool T)` result struct. Same rig as the
// other bcgen_*_test.c files.
//
// Exercises:
//  - `sqrt-checked` on a valid f64 input (ok=true, correct value) and an
//    invalid one (negative -> NaN -> ok=false).
//  - `pow-checked` (the one two-argument case) on a valid input.
//  - `asin-checked`/`acos-checked` valid AND domain-error (out-of-[-1,1])
//    inputs.
//  - The SAME checks repeated for an f32 argument/result -- proves the
//    widen-compute-narrow path (not just the f64-native path) works.
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
  "(package bcgen_checked_math_test)\n"
  "\n"
  "(fn sqrt-ok-f64 [] bool (. (sqrt-checked 16.0f64) _0))\n"
  "(fn sqrt-val-f64 [] f64 (. (sqrt-checked 16.0f64) _1))\n"
  "(fn sqrt-domain-f64 [] bool (. (sqrt-checked -1.0f64) _0))\n"
  "\n"
  "(fn pow-ok-f64 [] bool (. (pow-checked 2.0f64 10.0f64) _0))\n"
  "(fn pow-val-f64 [] f64 (. (pow-checked 2.0f64 10.0f64) _1))\n"
  "\n"
  "(fn asin-ok-f64 [] bool (. (asin-checked 1.0f64) _0))\n"
  "(fn asin-domain-f64 [] bool (. (asin-checked 2.0f64) _0))\n"
  "(fn acos-ok-f64 [] bool (. (acos-checked 1.0f64) _0))\n"
  "(fn acos-domain-f64 [] bool (. (acos-checked 2.0f64) _0))\n"
  "\n"
  "(fn sqrt-ok-f32 [] bool (. (sqrt-checked 16.0f32) _0))\n"
  "(fn sqrt-val-f32 [] f32 (. (sqrt-checked 16.0f32) _1))\n"
  "(fn sqrt-domain-f32 [] bool (. (sqrt-checked -1.0f32) _0))\n"
  "\n"
  "(fn main [] i32 0)\n";

static Checker
check_fixture(TypedAst* tast, TypedIndex* out_root) {
  Ast ast;
  ast_init(&ast, ctx_perm());

  String8 src     = str8_cstring((char*)(g_fixture_source));
  u32     file_id = source_file_register(str8_lit("bcgen_checked_math_test_fixture.3b"), src);

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

  u32 sqrt_ok_f64_fn      = bc_program_find_fn(&prog, str8_lit("sqrt-ok-f64"));
  u32 sqrt_val_f64_fn      = bc_program_find_fn(&prog, str8_lit("sqrt-val-f64"));
  u32 sqrt_domain_f64_fn  = bc_program_find_fn(&prog, str8_lit("sqrt-domain-f64"));
  u32 pow_ok_f64_fn        = bc_program_find_fn(&prog, str8_lit("pow-ok-f64"));
  u32 pow_val_f64_fn        = bc_program_find_fn(&prog, str8_lit("pow-val-f64"));
  u32 asin_ok_f64_fn        = bc_program_find_fn(&prog, str8_lit("asin-ok-f64"));
  u32 asin_domain_f64_fn  = bc_program_find_fn(&prog, str8_lit("asin-domain-f64"));
  u32 acos_ok_f64_fn        = bc_program_find_fn(&prog, str8_lit("acos-ok-f64"));
  u32 acos_domain_f64_fn  = bc_program_find_fn(&prog, str8_lit("acos-domain-f64"));
  u32 sqrt_ok_f32_fn        = bc_program_find_fn(&prog, str8_lit("sqrt-ok-f32"));
  u32 sqrt_val_f32_fn        = bc_program_find_fn(&prog, str8_lit("sqrt-val-f32"));
  u32 sqrt_domain_f32_fn  = bc_program_find_fn(&prog, str8_lit("sqrt-domain-f32"));

  Arena* heap = ctx_perm();

  expect_eq_i64("sqrt-ok-f64()",     bc_run_in_program(&prog, sqrt_ok_f64_fn,     NULL, 0, heap, &host_imports).value, 1);
  { f64 v; i64 bits = bc_run_in_program(&prog, sqrt_val_f64_fn, NULL, 0, heap, &host_imports).value;
    memcpy(&v, &bits, sizeof(v));
    if (v != 4.0) { fprintf(stderr, "FAIL sqrt-val-f64(): got %g, want 4\n", v); g_failures += 1; }
  }
  expect_eq_i64("sqrt-domain-f64()", bc_run_in_program(&prog, sqrt_domain_f64_fn, NULL, 0, heap, &host_imports).value, 0);

  expect_eq_i64("pow-ok-f64()", bc_run_in_program(&prog, pow_ok_f64_fn, NULL, 0, heap, &host_imports).value, 1);
  { f64 v; i64 bits = bc_run_in_program(&prog, pow_val_f64_fn, NULL, 0, heap, &host_imports).value;
    memcpy(&v, &bits, sizeof(v));
    if (v != 1024.0) { fprintf(stderr, "FAIL pow-val-f64(): got %g, want 1024\n", v); g_failures += 1; }
  }

  expect_eq_i64("asin-ok-f64()",     bc_run_in_program(&prog, asin_ok_f64_fn,     NULL, 0, heap, &host_imports).value, 1);
  expect_eq_i64("asin-domain-f64()", bc_run_in_program(&prog, asin_domain_f64_fn, NULL, 0, heap, &host_imports).value, 0);
  expect_eq_i64("acos-ok-f64()",     bc_run_in_program(&prog, acos_ok_f64_fn,     NULL, 0, heap, &host_imports).value, 1);
  expect_eq_i64("acos-domain-f64()", bc_run_in_program(&prog, acos_domain_f64_fn, NULL, 0, heap, &host_imports).value, 0);

  expect_eq_i64("sqrt-ok-f32()",     bc_run_in_program(&prog, sqrt_ok_f32_fn,     NULL, 0, heap, &host_imports).value, 1);
  { f32 v; u32 bits = (u32)bc_run_in_program(&prog, sqrt_val_f32_fn, NULL, 0, heap, &host_imports).value;
    memcpy(&v, &bits, sizeof(v));
    if (v != 4.0f) { fprintf(stderr, "FAIL sqrt-val-f32(): got %g, want 4\n", (double)v); g_failures += 1; }
  }
  expect_eq_i64("sqrt-domain-f32()", bc_run_in_program(&prog, sqrt_domain_f32_fn, NULL, 0, heap, &host_imports).value, 0);

  arena_temp_end(&fn_temp);

  if (g_failures == 0) printf("bcgen_checked_math_test: all checks passed\n");
  else                 printf("bcgen_checked_math_test: %d check(s) FAILED\n", g_failures);

  ctx_free();
  return g_failures == 0 ? 0 : 1;
}
