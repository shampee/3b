// bcgen_wrap_test.c -- validates that integer arithmetic on the `.3bs`
// bytecode VM WRAPS at its own type's width, the way native codegen's real
// C types do. Same rig as the other bcgen_*_test.c files.
//
// The bug this pins down: bytecode registers are all 64-bit, so every op
// used to be computed at 64 bits and only narrowed on an explicit `cast`.
// `(* x 3u32)` on a u32 printed 12000000000 under `3b run` and 3410065408
// from the native build -- same source, different numbers. PCG32 in
// native_pkgs/rng is built on exactly this wraparound, so `examples/dice`
// rolled a different sequence per backend from a fixed seed.
//
// Every expectation below is computed HERE, in C, with real C types --
// deliberately, since "agrees with what a C compiler does" is the whole
// specification: native codegen emits those C types, so a hand-written
// constant would only prove the VM is self-consistent.
//
// Exercises:
//  - add/sub/mul/shl on u8/u16/u32 and i8/i16/i32, each with operands
//    chosen to overflow that width (and subtraction chosen to go NEGATIVE
//    on the unsigned ones, which is where the old backend printed a
//    64-bit -2 instead of 4294967294).
//  - the three cases from the original bug report, verbatim.
//  - an intermediate result inside a larger expression, and a value
//    accumulated through a `var`/`set` across loop iterations -- wrapping
//    has to survive the round trip through storage, not just the one op.
//  - a comparison and a `%` on a wrapped value: those read the register
//    as a signed 64-bit number, so they are only correct if the wrap
//    already happened.
//  - `bit-not` and unary negation, the two unary ops that can leave a
//    value outside its type's range.
//  - `bit-shr` signedness (BcOp_Shr vs BcOp_ShrU): logical on unsigned,
//    arithmetic on signed. Unlike the ops above this bites at 64 bits,
//    where no narrowing can paper over it -- it is what made rng's
//    `(bit-shr state 59u64)` return a rotation of ~4 billion.
//  - divide/remainder/ordering signedness (BcOp_Div vs BcOp_DivU and
//    friends), the other half of that same 64-bit blind spot: `(/ big
//    3u64)` on a u64 past 2^63 divided a negative number and returned 0.
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
  "(package bcgen_wrap_test)\n"
  "\n"
  "(fn u8-add [a u8 b u8] u8 (+ a b))\n"
  "(fn u8-sub [a u8 b u8] u8 (- a b))\n"
  "(fn u8-mul [a u8 b u8] u8 (* a b))\n"
  "(fn u8-shl [a u8 s u8] u8 (bit-shl a s))\n"
  "\n"
  "(fn u16-add [a u16 b u16] u16 (+ a b))\n"
  "(fn u16-sub [a u16 b u16] u16 (- a b))\n"
  "(fn u16-mul [a u16 b u16] u16 (* a b))\n"
  "(fn u16-shl [a u16 s u16] u16 (bit-shl a s))\n"
  "\n"
  "(fn u32-add [a u32 b u32] u32 (+ a b))\n"
  "(fn u32-sub [a u32 b u32] u32 (- a b))\n"
  "(fn u32-mul [a u32 b u32] u32 (* a b))\n"
  "(fn u32-shl [a u32 s u32] u32 (bit-shl a s))\n"
  "\n"
  "(fn i8-add [a i8 b i8] i8 (+ a b))\n"
  "(fn i8-sub [a i8 b i8] i8 (- a b))\n"
  "(fn i8-mul [a i8 b i8] i8 (* a b))\n"
  "(fn i8-shl [a i8 s i8] i8 (bit-shl a s))\n"
  "\n"
  "(fn i16-add [a i16 b i16] i16 (+ a b))\n"
  "(fn i16-sub [a i16 b i16] i16 (- a b))\n"
  "(fn i16-mul [a i16 b i16] i16 (* a b))\n"
  "(fn i16-shl [a i16 s i16] i16 (bit-shl a s))\n"
  "\n"
  "(fn i32-add [a i32 b i32] i32 (+ a b))\n"
  "(fn i32-sub [a i32 b i32] i32 (- a b))\n"
  "(fn i32-mul [a i32 b i32] i32 (* a b))\n"
  "(fn i32-shl [a i32 s i32] i32 (bit-shl a s))\n"
  "\n"
  ";; The bug report's own three cases, spelled exactly as reported.\n"
  "(fn report-mul [] u32 (var x u32 4000000000u32) (* x 3u32))\n"
  "(fn report-square [] u32 (var s u32 (bit-shl 1u32 20u32)) (* s s))\n"
  "(fn report-underflow [] u32 (var a u32 7u32) (- a 9u32))\n"
  "\n"
  ";; An intermediate result: `(* a b)` has to wrap BEFORE `c` is added,\n"
  ";; not once at the end.\n"
  "(fn u16-chain [a u16 b u16 c u16] u16 (+ (* a b) c))\n"
  "\n"
  ";; Wrapping survives a round trip through a `var`, once per iteration.\n"
  "(fn u8-accum [n i32 step u8] u8\n"
  "  (var acc u8 0u8)\n"
  "  (for [i 0 n] (void (set acc (+ acc step))))\n"
  "  acc)\n"
  "\n"
  ";; Consumers that read the raw register: a signed comparison and a `%`.\n"
  ";; Both are wrong unless the wrap already happened.\n"
  "(fn u8-add-lt [a u8 b u8 t u8] bool (< (+ a b) t))\n"
  "(fn u32-mul-mod [a u32 b u32 m u32] u32 (% (* a b) m))\n"
  "\n"
  "(fn u8-not [a u8] u8 (bit-not a))\n"
  "(fn i8-neg [a i8] i8 (- a))\n"
  "\n"
  ";; Right-shift signedness -- logical on unsigned, arithmetic on signed.\n"
  "(fn u64-shr [x u64 s u64] u64 (bit-shr x s))\n"
  "\n"
  "(fn u64-div [a u64 b u64] u64 (/ a b))\n"
  "(fn u64-mod [a u64 b u64] u64 (% a b))\n"
  "(fn u64-lt [a u64 b u64] bool (< a b))\n"
  "(fn u64-ge [a u64 b u64] bool (>= a b))\n"
  "(fn i64-div [a i64 b i64] i64 (/ a b))\n"
  "(fn i64-lt [a i64 b i64] bool (< a b))\n"
  "(fn i64-shr [x i64 s i64] i64 (bit-shr x s))\n"
  "(fn u32-shr [x u32 s u32] u32 (bit-shr x s))\n"
  "(fn i32-shr [x i32 s i32] i32 (bit-shr x s))\n"
  "\n"
  "(fn main [] i32 0)\n";

static Checker
check_fixture(TypedAst* tast, TypedIndex* out_root) {
  Ast ast;
  ast_init(&ast, ctx_perm());

  String8 src     = str8_cstring((char*)(g_fixture_source));
  u32     file_id = source_file_register(str8_lit("bcgen_wrap_test_fixture.3b"), src);

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

static BcProgram        g_prog;
static BcHostImportTable g_host_imports = {0}; // no host calls in this fixture

static i64
call2(const char* fn_name, i64 a, i64 b) {
  u32 fn      = bc_program_find_fn(&g_prog, str8_cstring((char*)fn_name));
  i64 args[2] = { a, b };
  return bc_run_in_program(&g_prog, fn, args, 2, ctx_perm(), &g_host_imports).value;
}

static i64
call3(const char* fn_name, i64 a, i64 b, i64 c) {
  u32 fn      = bc_program_find_fn(&g_prog, str8_cstring((char*)fn_name));
  i64 args[3] = { a, b, c };
  return bc_run_in_program(&g_prog, fn, args, 3, ctx_perm(), &g_host_imports).value;
}

static i64
call1(const char* fn_name, i64 a) {
  u32 fn      = bc_program_find_fn(&g_prog, str8_cstring((char*)fn_name));
  i64 args[1] = { a };
  return bc_run_in_program(&g_prog, fn, args, 1, ctx_perm(), &g_host_imports).value;
}

static i64
call0(const char* fn_name) {
  u32 fn = bc_program_find_fn(&g_prog, str8_cstring((char*)fn_name));
  return bc_run_in_program(&g_prog, fn, NULL, 0, ctx_perm(), &g_host_imports).value;
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

  g_prog = bc_compile_program(&ck, &tast, root, fn_temp.arena, ctx_perm(), &layout_cache, &g_host_imports, NULL);
  xassert(g_prog.ok);

  // ~~ unsigned add/sub/mul/shl at each width. The `sub` cases go negative,
  // which is where the 64-bit register used to surface as a huge signed
  // number rather than the type's own wraparound. ~~
  expect_eq_i64("u8 add",  call2("u8-add", 200, 100), (i64)(u8)(200 + 100));
  expect_eq_i64("u8 sub",  call2("u8-sub", 5, 10),    (i64)(u8)(5 - 10));
  expect_eq_i64("u8 mul",  call2("u8-mul", 20, 20),   (i64)(u8)(20 * 20));
  expect_eq_i64("u8 shl",  call2("u8-shl", 3, 7),     (i64)(u8)(3u << 7));

  expect_eq_i64("u16 add", call2("u16-add", 60000, 10000), (i64)(u16)(60000 + 10000));
  expect_eq_i64("u16 sub", call2("u16-sub", 5, 10),        (i64)(u16)(5 - 10));
  expect_eq_i64("u16 mul", call2("u16-mul", 300, 300),     (i64)(u16)(300 * 300));
  expect_eq_i64("u16 shl", call2("u16-shl", 5, 15),        (i64)(u16)(5u << 15));

  expect_eq_i64("u32 add", call2("u32-add", 4000000000u, 400000000u), (i64)(u32)(4000000000u + 400000000u));
  expect_eq_i64("u32 sub", call2("u32-sub", 7, 9),                    (i64)(u32)(7u - 9u));
  expect_eq_i64("u32 mul", call2("u32-mul", 4000000000u, 3),          (i64)(u32)(4000000000u * 3u));
  expect_eq_i64("u32 shl", call2("u32-shl", 5, 31),                   (i64)(u32)(5u << 31));

  // ~~ signed add/sub/mul/shl. Overflow here wraps two's-complement, so the
  // results go NEGATIVE rather than merely truncating. ~~
  expect_eq_i64("i8 add",  call2("i8-add", 100, 100),   (i64)(i8)(100 + 100));
  expect_eq_i64("i8 sub",  call2("i8-sub", -100, 100),  (i64)(i8)(-100 - 100));
  expect_eq_i64("i8 mul",  call2("i8-mul", 20, 20),     (i64)(i8)(20 * 20));
  expect_eq_i64("i8 shl",  call2("i8-shl", 1, 7),       (i64)(i8)(1u << 7));

  expect_eq_i64("i16 add", call2("i16-add", 30000, 10000),  (i64)(i16)(30000 + 10000));
  expect_eq_i64("i16 sub", call2("i16-sub", -30000, 10000), (i64)(i16)(-30000 - 10000));
  expect_eq_i64("i16 mul", call2("i16-mul", 200, 200),      (i64)(i16)(200 * 200));
  expect_eq_i64("i16 shl", call2("i16-shl", 1, 15),         (i64)(i16)(1u << 15));

  // Computed through u32 (then reinterpreted) rather than as a C i32
  // expression: signed overflow is undefined behavior in C, so writing
  // 2000000000 + 2000000000 in i32 would be UB in this very test. The
  // unsigned computation is the defined spelling of the same bit pattern,
  // and matches what the generated C actually does on this target.
  expect_eq_i64("i32 add", call2("i32-add", 2000000000, 2000000000), (i64)(i32)(u32)(2000000000u + 2000000000u));
  expect_eq_i64("i32 sub", call2("i32-sub", -2000000000, 2000000000), (i64)(i32)(u32)((u32)(-2000000000) - 2000000000u));
  expect_eq_i64("i32 mul", call2("i32-mul", 100000, 100000),          (i64)(i32)(u32)(100000u * 100000u));
  expect_eq_i64("i32 shl", call2("i32-shl", 1, 31),                   (i64)(i32)(u32)(1u << 31));

  // ~~ the bug report's own three cases ~~
  expect_eq_i64("report: (* 4000000000u32 3u32)",   call0("report-mul"),       (i64)(u32)(4000000000u * 3u));
  expect_eq_i64("report: (1u32<<20) squared",       call0("report-square"),    (i64)(u32)((1u << 20) * (1u << 20)));
  expect_eq_i64("report: (- 7u32 9u32)",            call0("report-underflow"), (i64)(u32)(7u - 9u));

  // ~~ wrapping survives an intermediate result and a `var` round trip ~~
  expect_eq_i64("u16 chained (* then +)", call3("u16-chain", 300, 300, 100), (i64)(u16)((u16)(300 * 300) + 100));
  {
    u8 acc = 0;
    for (int i = 0; i < 10; i += 1) acc = (u8)(acc + 100);
    expect_eq_i64("u8 accumulated across 10 iterations", call2("u8-accum", 10, 100), (i64)acc);
  }

  // ~~ consumers that read the register directly ~~
  // 200 + 100 wraps to 44, which IS < 50; unwrapped it would be 300, which
  // is not -- the comparison flips outright, it does not merely print odd.
  expect_eq_i64("comparison sees the wrapped value", call3("u8-add-lt", 200, 100, 50), (u8)(200 + 100) < 50 ? 1 : 0);
  expect_eq_i64("`%` sees the wrapped value", call3("u32-mul-mod", 4000000000u, 3, 1000u), (i64)((u32)(4000000000u * 3u) % 1000u));

  // ~~ the unary ops ~~
  expect_eq_i64("bit-not on u8",     call1("u8-not", 0),    (i64)(u8)(~0u));
  expect_eq_i64("bit-not on u8 (1)", call1("u8-not", 1),    (i64)(u8)(~1u));
  expect_eq_i64("negate i8 -128",    call1("i8-neg", -128), (i64)(i8)(u8)(-(-128)));
  expect_eq_i64("negate i8 42",      call1("i8-neg", 42),   (i64)(i8)(-42));

  // ~~ right-shift signedness ~~
  // A u64 past 2^63 sits in the register as a NEGATIVE i64, so an
  // arithmetic shift would drag ones down through it.
  {
    u64 big = 0x8000000000000000ull;
    expect_eq_i64("u64 >> 60 is logical",  call2("u64-shr", (i64)big, 60), (i64)(big >> 60));
    expect_eq_i64("u64 >> 1 is logical",   call2("u64-shr", (i64)big, 1),  (i64)(big >> 1));
  }
  expect_eq_i64("i64 >> 2 stays arithmetic", call2("i64-shr", -16, 2), (i64)(-16 >> 2));
  expect_eq_i64("u32 >> 4 is logical",       call2("u32-shr", 0x80000000, 4), (i64)(u32)(0x80000000u >> 4));
  expect_eq_i64("i32 >> 4 stays arithmetic", call2("i32-shr", -16, 4), (i64)(i32)(-16 >> 4));

  // ~~ divide/remainder/ordering signedness ~~
  // Same 64-bit blind spot as the shift above, from the other side: these
  // read bits already in the register rather than deciding which come in.
  // A u64 past 2^63 is a negative i64 there, so the signed opcodes divide
  // and order it as one -- U64_MAX / 3 came back 0 instead of a number a
  // third its size, and U64_MAX > 1 came back false.
  {
    u64 big = 18446744073709551615ull; // U64_MAX -- every bit set
    expect_eq_i64("u64 / 3 is unsigned",  call2("u64-div", (i64)big, 3),  (i64)(big / 3ull));
    expect_eq_i64("u64 % 10 is unsigned", call2("u64-mod", (i64)big, 10), (i64)(big % 10ull));
    expect_eq_i64("u64 < 1 is unsigned",  call2("u64-lt",  (i64)big, 1),  (big <  1ull) ? 1 : 0);
    expect_eq_i64("u64 >= 1 is unsigned", call2("u64-ge",  (i64)big, 1),  (big >= 1ull) ? 1 : 0);
  }
  // The signed halves must be untouched by that -- same opcodes' pair, and
  // a negative i64 really is less than 1.
  expect_eq_i64("i64 / 2 stays signed", call2("i64-div", -9, 2), (i64)(-9 / 2));
  expect_eq_i64("i64 < 1 stays signed", call2("i64-lt",  -1, 1), (-1 < 1) ? 1 : 0);

  arena_temp_end(&fn_temp);

  if (g_failures == 0) printf("bcgen_wrap_test: all checks passed\n");
  else                 printf("bcgen_wrap_test: %d check(s) FAILED\n", g_failures);

  ctx_free();
  return g_failures == 0 ? 0 : 1;
}
