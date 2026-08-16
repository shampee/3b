// bcgen_ops_test.c -- validates the operator/compile-time-constant slice
// added to bcgen.c on top of the earlier slices (real short-circuit `and`/
// `or`, `not`, unary `-`/`+`, `string-len`, `cstring`, `sizeof`/`alignof`/
// `member-offset`, `zero`, `nil`). Same rig as the other bcgen_*_test.c
// files.
//
// Exercises:
//  - `and`/`or` truth tables, AND real short-circuit: the rhs of each is a
//    div-by-zero that would SIGFPE-crash this whole test process if it were
//    (wrongly) evaluated anyway when the result is already determined.
//  - `not` on both a truthy and falsy bool.
//  - Unary `-` on i32 and f64 (bc_f64_op_for_kind dispatch), unary `+` as a
//    true no-op.
//  - `string-len`/`cstring` -- the latter via pointer EQUALITY (same string
//    variable's `(cstring s)` read twice must be the SAME address; two
//    DIFFERENT string literals' must differ), proving the offset-0 load is
//    real and consistent without needing `char`/`cast` support.
//  - `sizeof`/`alignof`/`member-offset` against a real 2-field struct,
//    cross-checked against layout.c's own already-tested numbers.
//  - `zero` on both a scalar (i32) and an embedded struct (field read back).
//  - `nil` -- a genuinely NULL pointer argument passed in from the C
//    harness, round-tripped through `(= p nil)`.
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

// An f32 result lives in the LOW 4 bytes of its i64 register only --
// everything above is don't-care -- so it must be narrowed before
// reinterpreting, unlike expect_eq_f64 above (a genuine 8-byte value).
static void
expect_eq_f32(const char* what, i64 got_bits, f32 want) {
  f32 got; u32 bits32 = (u32)got_bits; memcpy(&got, &bits32, sizeof(got));
  if (got != want) {
    fprintf(stderr, "FAIL %s: got %g, want %g\n", what, (double)got, (double)want);
    g_failures += 1;
  }
}

static const char* g_fixture_source =
  "(package bcgen_ops_test)\n"
  "(struct Vec2 [x f32 y f32])\n"
  "\n"
  "(fn and-b [a bool b bool] bool (and a b))\n"
  "(fn or-b [a bool b bool] bool (or a b))\n"
  "(fn not-b [a bool] bool (not a))\n"
  "\n"
  "(fn and-short-circuits [] bool (and false (= (/ 1 0) 0)))\n"
  "(fn or-short-circuits [] bool (or true (= (/ 1 0) 0)))\n"
  "\n"
  "(fn neg-i [x i32] i32 (- x))\n"
  "(fn neg-f64 [x f64] f64 (- x))\n"
  "(fn pos-i [x i32] i32 (+ x))\n"
  "\n"
  "(fn str-len [s string] u64 (string-len s))\n"
  "(fn cstr-eq-self [s string] bool (= (cstring s) (cstring s)))\n"
  "(fn cstr-eq-diff [a string b string] bool (= (cstring a) (cstring b)))\n"
  "\n"
  "(fn vec2-size [] u64 (sizeof Vec2))\n"
  "(fn vec2-align [] u64 (alignof Vec2))\n"
  "(fn vec2-y-offset [] u64 (member-offset Vec2 y))\n"
  "\n"
  "(fn zero-i [] i32 (zero i32))\n"
  "(fn zero-vec2-x [] f32 (let [z Vec2 (zero Vec2)] (. z x)))\n"
  "\n"
  "(fn is-nil [p i32*] bool (= p nil))\n"
  "\n"
  "(fn main [] i32 0)\n";

static Checker
check_fixture(TypedAst* tast, TypedIndex* out_root) {
  Ast ast;
  ast_init(&ast, ctx_perm());

  String8 src     = str8_cstring((char*)(g_fixture_source));
  u32     file_id = source_file_register(str8_lit("bcgen_ops_test_fixture.3b"), src);

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
  xassert(prog.ok); // this fixture is entirely within the operator slice under test

  u32 and_b_fn              = bc_program_find_fn(&prog, str8_lit("and-b"));
  u32 or_b_fn                = bc_program_find_fn(&prog, str8_lit("or-b"));
  u32 not_b_fn                = bc_program_find_fn(&prog, str8_lit("not-b"));
  u32 and_short_circuits_fn = bc_program_find_fn(&prog, str8_lit("and-short-circuits"));
  u32 or_short_circuits_fn  = bc_program_find_fn(&prog, str8_lit("or-short-circuits"));
  u32 neg_i_fn                = bc_program_find_fn(&prog, str8_lit("neg-i"));
  u32 neg_f64_fn             = bc_program_find_fn(&prog, str8_lit("neg-f64"));
  u32 pos_i_fn                = bc_program_find_fn(&prog, str8_lit("pos-i"));
  u32 str_len_fn              = bc_program_find_fn(&prog, str8_lit("str-len"));
  u32 cstr_eq_self_fn        = bc_program_find_fn(&prog, str8_lit("cstr-eq-self"));
  u32 cstr_eq_diff_fn        = bc_program_find_fn(&prog, str8_lit("cstr-eq-diff"));
  u32 vec2_size_fn            = bc_program_find_fn(&prog, str8_lit("vec2-size"));
  u32 vec2_align_fn           = bc_program_find_fn(&prog, str8_lit("vec2-align"));
  u32 vec2_y_offset_fn        = bc_program_find_fn(&prog, str8_lit("vec2-y-offset"));
  u32 zero_i_fn                = bc_program_find_fn(&prog, str8_lit("zero-i"));
  u32 zero_vec2_x_fn          = bc_program_find_fn(&prog, str8_lit("zero-vec2-x"));
  u32 is_nil_fn                = bc_program_find_fn(&prog, str8_lit("is-nil"));

  Arena* heap = ctx_perm();

  // ~~ `and`/`or` truth tables.
  { i64 args[2] = {1, 1}; expect_eq_i64("and(T,T)", bc_run_in_program(&prog, and_b_fn, args, 2, heap, &host_imports).value, 1); }
  { i64 args[2] = {1, 0}; expect_eq_i64("and(T,F)", bc_run_in_program(&prog, and_b_fn, args, 2, heap, &host_imports).value, 0); }
  { i64 args[2] = {0, 1}; expect_eq_i64("and(F,T)", bc_run_in_program(&prog, and_b_fn, args, 2, heap, &host_imports).value, 0); }
  { i64 args[2] = {0, 0}; expect_eq_i64("or(F,F)",  bc_run_in_program(&prog, or_b_fn,  args, 2, heap, &host_imports).value, 0); }
  { i64 args[2] = {0, 1}; expect_eq_i64("or(F,T)",  bc_run_in_program(&prog, or_b_fn,  args, 2, heap, &host_imports).value, 1); }
  { i64 args[2] = {1, 0}; expect_eq_i64("or(T,F)",  bc_run_in_program(&prog, or_b_fn,  args, 2, heap, &host_imports).value, 1); }
  { i64 args[1] = {1};    expect_eq_i64("not(T)",   bc_run_in_program(&prog, not_b_fn, args, 1, heap, &host_imports).value, 0); }
  { i64 args[1] = {0};    expect_eq_i64("not(F)",   bc_run_in_program(&prog, not_b_fn, args, 1, heap, &host_imports).value, 1); }

  // ~~ Real short-circuit -- a div-by-zero rhs would SIGFPE-crash this
  // whole process if it were (wrongly) evaluated anyway.
  expect_eq_i64("and-short-circuits()", bc_run_in_program(&prog, and_short_circuits_fn, NULL, 0, heap, &host_imports).value, 0);
  expect_eq_i64("or-short-circuits()",  bc_run_in_program(&prog, or_short_circuits_fn,  NULL, 0, heap, &host_imports).value, 1);

  // ~~ Unary `-`/`+`.
  { i64 args[1] = {5};                    expect_eq_i64("neg-i(5)", bc_run_in_program(&prog, neg_i_fn, args, 1, heap, &host_imports).value, -5); }
  { i64 args[1] = {-7};                   expect_eq_i64("neg-i(-7)", bc_run_in_program(&prog, neg_i_fn, args, 1, heap, &host_imports).value, 7); }
  { f64 x = 3.5; i64 bits; memcpy(&bits, &x, sizeof(bits));
    i64 args[1] = {bits};
    expect_eq_f64("neg-f64(3.5)", bc_run_in_program(&prog, neg_f64_fn, args, 1, heap, &host_imports).value, -3.5);
  }
  { i64 args[1] = {42}; expect_eq_i64("pos-i(42)", bc_run_in_program(&prog, pos_i_fn, args, 1, heap, &host_imports).value, 42); }

  // ~~ `string-len`/`cstring`. A string PARAMETER is one register holding
  // the address of an already-boxed {ptr,size} header (this backend's
  // usual "embedded value's representation is its address" convention) --
  // bc_run_in_program only takes raw i64 args, so pass that header's own
  // address, same as bcgen_types_test.c's own string-parameter case.
  { String8 s = str8_lit("hello");
    i64 args[1] = {(i64)(intptr_t)&s};
    expect_eq_i64("str-len(\"hello\")", bc_run_in_program(&prog, str_len_fn, args, 1, heap, &host_imports).value, 5);
  }
  { String8 s = str8_lit("world");
    i64 args[1] = {(i64)(intptr_t)&s};
    expect_eq_i64("cstr-eq-self same string", bc_run_in_program(&prog, cstr_eq_self_fn, args, 1, heap, &host_imports).value, 1);
  }
  { String8 a = str8_lit("aaa"), b = str8_lit("bbb");
    i64 args[2] = {(i64)(intptr_t)&a, (i64)(intptr_t)&b};
    expect_eq_i64("cstr-eq-diff different strings", bc_run_in_program(&prog, cstr_eq_diff_fn, args, 2, heap, &host_imports).value, 0);
  }

  // ~~ `sizeof`/`alignof`/`member-offset` against a real struct -- cross-
  // checked against layout.c's own already-tested numbers for a plain
  // two-f32-field struct.
  { Layout expect = layout_of(&layout_cache, &ck, (TypeRef){.kind = TypeKind_Named, .name = str8_lit("Vec2")});
    expect_eq_i64("sizeof(Vec2)",  bc_run_in_program(&prog, vec2_size_fn,  NULL, 0, heap, &host_imports).value, (i64)expect.size);
    expect_eq_i64("alignof(Vec2)", bc_run_in_program(&prog, vec2_align_fn, NULL, 0, heap, &host_imports).value, (i64)expect.align);
  }
  expect_eq_i64("member-offset(Vec2,y)", bc_run_in_program(&prog, vec2_y_offset_fn, NULL, 0, heap, &host_imports).value, 4);

  // ~~ `zero` on a scalar and on an embedded struct field.
  expect_eq_i64("zero(i32)", bc_run_in_program(&prog, zero_i_fn, NULL, 0, heap, &host_imports).value, 0);
  { BcResult r = bc_run_in_program(&prog, zero_vec2_x_fn, NULL, 0, heap, &host_imports);
    expect_eq_f32("zero(Vec2).x", r.value, 0.0f);
  }

  // ~~ `nil` -- a genuinely NULL pointer passed in from the C harness.
  { i64 args[1] = {0}; expect_eq_i64("is-nil(NULL)", bc_run_in_program(&prog, is_nil_fn, args, 1, heap, &host_imports).value, 1); }
  { i64 args[1] = {(i64)(intptr_t)&args}; expect_eq_i64("is-nil(non-null)", bc_run_in_program(&prog, is_nil_fn, args, 1, heap, &host_imports).value, 0); }

  arena_temp_end(&fn_temp);

  if (g_failures == 0) printf("bcgen_ops_test: all checks passed\n");
  else                 printf("bcgen_ops_test: %d check(s) FAILED\n", g_failures);

  ctx_free();
  return g_failures == 0 ? 0 : 1;
}
