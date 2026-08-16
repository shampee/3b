// bcgen_parse_number_test.c -- validates `string-to-i32`/`string-to-i64`/
// `string-to-u32`/`string-to-u64`/`string-to-f32`/`string-to-f64`
// (TypedNodeKind_ParseNumber) support added to bcgen.c/bcvm.c: the new
// BcOp_ParseNumberValue/BcOp_ParseNumberOk opcode pair (dispatching on
// the target TypeKind carried in the `c` operand) plus the shared
// bc_compile_bool_t_result helper building the synthesized `(bool T)`
// result struct. Same rig as the other bcgen_*_test.c files.
//
// Exercises:
//  - Each of the 6 numeric kinds on a valid input (ok=true, correct
//    value).
//  - A malformed input (ok=false) for a representative few kinds.
//  - An i32/u32 input that parses as a valid i64/u64 but overflows the
//    narrower 32-bit range (ok=false) -- proves the range check isn't
//    just "did strtoll succeed".
//  - The VALUE slot of every failing parse, which must be a defined 0.
//    ok=false means "don't read this", but the native backend's runtime
//    zero-initializes its result struct, so the VM printing a truncated
//    magnitude there made the same program produce different output on
//    the two backends. The i32 overflow case is the one that regressed:
//    the value opcode parsed as i64 and let the register truncate while
//    only the ok opcode applied the 32-bit range check.
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

static void
expect_eq_f32(const char* what, i64 got_bits, f32 want) {
  f32 got; u32 bits32 = (u32)got_bits; memcpy(&got, &bits32, sizeof(got));
  if (got != want) {
    fprintf(stderr, "FAIL %s: got %g, want %g\n", what, (double)got, (double)want);
    g_failures += 1;
  }
}

static const char* g_fixture_source =
  "(package bcgen_parse_number_test)\n"
  "\n"
  "(fn i32-ok []    bool (. (string-to-i32 \"42\") _0))\n"
  "(fn i32-val []    i32 (. (string-to-i32 \"42\") _1))\n"
  "(fn i32-neg-ok [] bool (. (string-to-i32 \"-7\") _0))\n"
  "(fn i32-neg-val [] i32 (. (string-to-i32 \"-7\") _1))\n"
  "(fn i32-bad-ok [] bool (. (string-to-i32 \"abc\") _0))\n"
  "(fn i32-bad-val [] i32 (. (string-to-i32 \"abc\") _1))\n"
  "(fn i32-overflow-ok [] bool (. (string-to-i32 \"99999999999\") _0))\n"
  ";; parses cleanly as an i64, so only the narrow range check rejects it --\n"
  ";; the value must be 0, NOT the low 32 bits of the wide parse.\n"
  "(fn i32-overflow-val [] i32 (. (string-to-i32 \"99999999999\") _1))\n"
  "(fn i32-neg-overflow-val [] i32 (. (string-to-i32 \"-99999999999\") _1))\n"
  // The exact string from the bug report: parses fine as i64, and its low
  // 32 bits are 1316134911 -- the truncated garbage the VM used to yield.
  "(fn i32-report-val [] i32 (. (string-to-i32 \"9999999999999\") _1))\n"
  "\n"
  "(fn i64-ok []  bool (. (string-to-i64 \"9000000000\") _0))\n"
  "(fn i64-val []  i64 (. (string-to-i64 \"9000000000\") _1))\n"
  // 20 digits: wraps a u64 accumulator back UNDER I64_MAX, so a parser that
  // only range-checks the final magnitude reports ok=true for a number the
  // string never named. Boundary cases below pin the guard down to the exact
  // limit, so tightening it cannot start over-rejecting.
  "(fn i64-overflow-ok [] bool (. (string-to-i64 \"99999999999999999999\") _0))\n"
  "(fn i64-overflow-val [] i64 (. (string-to-i64 \"99999999999999999999\") _1))\n"
  "(fn i64-max-ok []  bool (. (string-to-i64 \"9223372036854775807\") _0))\n"
  "(fn i64-max-val []  i64 (. (string-to-i64 \"9223372036854775807\") _1))\n"
  "(fn i64-min-ok []  bool (. (string-to-i64 \"-9223372036854775808\") _0))\n"
  "(fn i64-min-val []  i64 (. (string-to-i64 \"-9223372036854775808\") _1))\n"
  "(fn i64-past-max-ok [] bool (. (string-to-i64 \"9223372036854775808\") _0))\n"
  "(fn i64-past-min-ok [] bool (. (string-to-i64 \"-9223372036854775809\") _0))\n"
  "\n"
  "(fn u32-ok []  bool (. (string-to-u32 \"42\") _0))\n"
  "(fn u32-val []  u32 (. (string-to-u32 \"42\") _1))\n"
  "(fn u32-neg-ok [] bool (. (string-to-u32 \"-1\") _0))\n"
  "(fn u32-neg-val [] u32 (. (string-to-u32 \"-1\") _1))\n"
  "(fn u32-overflow-ok [] bool (. (string-to-u32 \"5000000000\") _0))\n"
  "(fn u32-overflow-val [] u32 (. (string-to-u32 \"5000000000\") _1))\n"
  "\n"
  "(fn u64-ok []  bool (. (string-to-u64 \"18000000000000000000\") _0))\n"
  "(fn u64-val []  u64 (. (string-to-u64 \"18000000000000000000\") _1))\n"
  "(fn u64-overflow-val [] u64 (. (string-to-u64 \"99999999999999999999999\") _1))\n"
  "\n"
  "(fn f32-ok []  bool (. (string-to-f32 \"3.5\") _0))\n"
  "(fn f32-val []  f32 (. (string-to-f32 \"3.5\") _1))\n"
  "(fn f32-bad-val [] f32 (. (string-to-f32 \"not-a-number\") _1))\n"
  "\n"
  "(fn f64-ok []  bool (. (string-to-f64 \"2.5\") _0))\n"
  "(fn f64-val []  f64 (. (string-to-f64 \"2.5\") _1))\n"
  "(fn f64-bad-ok [] bool (. (string-to-f64 \"not-a-number\") _0))\n"
  "(fn f64-bad-val [] f64 (. (string-to-f64 \"not-a-number\") _1))\n"
  "(fn f64-empty-val [] f64 (. (string-to-f64 \"\") _1))\n"
  "(fn u64-neg-val [] u64 (. (string-to-u64 \"-5\") _1))\n"
  "\n"
  "(fn main [] i32 0)\n";

static Checker
check_fixture(TypedAst* tast, TypedIndex* out_root) {
  Ast ast;
  ast_init(&ast, ctx_perm());

  String8 src     = str8_cstring((char*)(g_fixture_source));
  u32     file_id = source_file_register(str8_lit("bcgen_parse_number_test_fixture.3b"), src);

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

  Arena* heap = ctx_perm();

#define RUN(name) bc_run_in_program(&prog, bc_program_find_fn(&prog, str8_lit(name)), NULL, 0, heap, &host_imports).value

  expect_eq_i64("i32-ok()",  RUN("i32-ok"), 1);
  expect_eq_i64("i32-val()", RUN("i32-val"), 42);
  expect_eq_i64("i32-neg-ok()",  RUN("i32-neg-ok"), 1);
  expect_eq_i64("i32-neg-val()", RUN("i32-neg-val"), -7);
  expect_eq_i64("i32-bad-ok()", RUN("i32-bad-ok"), 0);
  expect_eq_i64("i32-overflow-ok()", RUN("i32-overflow-ok"), 0);

  // The value alongside every one of those false flags: 0, never a
  // truncation of a parse that succeeded at a wider type.
  expect_eq_i64("i32-bad-val()", RUN("i32-bad-val"), 0);
  expect_eq_i64("i32-overflow-val()", RUN("i32-overflow-val"), 0);
  expect_eq_i64("i32-neg-overflow-val()", RUN("i32-neg-overflow-val"), 0);
  expect_eq_i64("i32-neg-overflow-val()", RUN("i32-neg-overflow-val"), 0);
  expect_eq_i64("i64-overflow-val()", RUN("i64-overflow-val"), 0);
  expect_eq_i64("u64-neg-val()", RUN("u64-neg-val"), 0);
  expect_eq_f32("f32-bad-val()", RUN("f32-bad-val"), 0.0f);
  expect_eq_f64("f64-bad-val()", RUN("f64-bad-val"), 0.0);

  expect_eq_i64("i64-ok()",  RUN("i64-ok"), 1);
  expect_eq_i64("i64-val()", RUN("i64-val"), 9000000000ll);
  expect_eq_i64("i64-overflow-ok()", RUN("i64-overflow-ok"), 0);
  expect_eq_i64("i64-max-ok()",  RUN("i64-max-ok"), 1);
  expect_eq_i64("i64-max-val()", RUN("i64-max-val"), 9223372036854775807ll);
  expect_eq_i64("i64-min-ok()",  RUN("i64-min-ok"), 1);
  expect_eq_i64("i64-min-val()", RUN("i64-min-val"), (-9223372036854775807ll - 1));
  expect_eq_i64("i64-past-max-ok()", RUN("i64-past-max-ok"), 0);
  expect_eq_i64("i64-past-min-ok()", RUN("i64-past-min-ok"), 0);

  expect_eq_i64("u32-ok()",  RUN("u32-ok"), 1);
  expect_eq_i64("u32-val()", RUN("u32-val"), 42);
  expect_eq_i64("u32-neg-ok()", RUN("u32-neg-ok"), 0);
  expect_eq_i64("u32-overflow-ok()", RUN("u32-overflow-ok"), 0);
  expect_eq_i64("u32-overflow-val()", RUN("u32-overflow-val"), 0);

  expect_eq_i64("u64-ok()",  RUN("u64-ok"), 1);
  { u64 v = (u64)RUN("u64-val");
    if (v != 18000000000000000000ull) { fprintf(stderr, "FAIL u64-val(): got %llu\n", (unsigned long long)v); g_failures += 1; }
  }

  expect_eq_i64("f32-ok()", RUN("f32-ok"), 1);
  expect_eq_f32("f32-val()", RUN("f32-val"), 3.5f);

  expect_eq_i64("f64-ok()", RUN("f64-ok"), 1);
  expect_eq_f64("f64-val()", RUN("f64-val"), 2.5);
  expect_eq_i64("f64-bad-ok()", RUN("f64-bad-ok"), 0);

  // Every failing parse hands back a defined 0 in the value slot, matching
  // what the native backend's zero-initialized result struct produces. See
  // bc_parse_number in bcvm.c -- one predicate feeds both opcodes precisely
  // so the value and the ok flag cannot disagree about what failed.
  expect_eq_i64("i32-bad-val()", RUN("i32-bad-val"), 0);
  expect_eq_i64("i32-overflow-val()", RUN("i32-overflow-val"), 0);
  expect_eq_i64("i32-report-val()", RUN("i32-report-val"), 0);
  expect_eq_i64("i64-overflow-val()", RUN("i64-overflow-val"), 0);
  expect_eq_i64("u32-neg-val()", RUN("u32-neg-val"), 0);
  expect_eq_i64("u32-overflow-val()", RUN("u32-overflow-val"), 0);
  expect_eq_i64("u64-overflow-val()", RUN("u64-overflow-val"), 0);
  expect_eq_i64("u64-neg-val()", RUN("u64-neg-val"), 0);
  expect_eq_f32("f32-bad-val()", RUN("f32-bad-val"), 0.0f);
  expect_eq_f64("f64-bad-val()", RUN("f64-bad-val"), 0.0);
  expect_eq_f64("f64-empty-val()", RUN("f64-empty-val"), 0.0);

#undef RUN

  arena_temp_end(&fn_temp);

  if (g_failures == 0) printf("bcgen_parse_number_test: all checks passed\n");
  else                 printf("bcgen_parse_number_test: %d check(s) FAILED\n", g_failures);

  ctx_free();
  return g_failures == 0 ? 0 : 1;
}
