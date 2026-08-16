// int_literal_test.c -- how an integer literal gets its type and its bits.
// Same rig as the bcgen_*_test.c files: the fixture is compiled to bytecode
// and run, so this covers the shared frontend (lower.c's parse, checker.c's
// type adoption and range check) rather than any one backend's emission.
//
// Exercises:
//  - Hex as a BIT PATTERN, not a magnitude. `0x8000000000000000` is the i64
//    sign bit and `0xFFFFFFFFFFFFFFFF` is every u64 bit; both used to be
//    rejected outright as "doesn't fit in 64 bits", since an unsuffixed
//    literal was read with strtoll whatever base it was in.
//  - A suffixless literal taking the type it is declared as, so `(val n i64
//    5)` is not "declared as i64 but initializer is i32". Checked at both
//    top level and in a `let`, the two paths through check_init_expr.
//  - The same value reached four ways -- bare hex, hex with an i64 suffix,
//    the decimal spelling, and `reinterpret`/`cast` from the u64 pattern --
//    which must agree, being one bit pattern described four ways.
//  - `-0xFF`, where the unsigned parse wraps and two's complement has to
//    bring it back to -255 rather than leaving a huge positive.
//  - Every context that supplies an expected type, since they are separate
//    call sites and only the val/var/let one is hard to get wrong: a call
//    argument, an explicit `(return ...)`, an implicit tail return (through
//    a Block, a `let` body and both arms of an `if`), and a struct literal
//    field.
//  - The range check still failing on the far side of the adopted type is
//    NOT covered here: a fixture that fails to check aborts this rig. See
//    the `u8-literal-too-large` group in test/checker_error_test.c.
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
  "(package int_literal_test)\n"
  "\n"
  "(val G_NOPTS i64 0x8000000000000000)\n"
  "(val G_ONES  u64 0xFFFFFFFFFFFFFFFF)\n"
  "(val G_SMALL i64 5)\n"
  "\n"
  "(fn global-nopts [] i64 G_NOPTS)\n"
  "(fn global-ones  [] i64 (cast i64 G_ONES))\n"
  "(fn global-small [] i64 G_SMALL)\n"
  "\n"
  "(fn local-hex-min      [] i64 (let [x i64 0x8000000000000000] x))\n"
  "(fn local-hex-min-sfx  [] i64 (let [x i64 0x8000000000000000i64] x))\n"
  "(fn local-dec-min      [] i64 (let [x i64 -9223372036854775808] x))\n"
  "(fn local-reinterpret  [] i64 (let [x i64 (reinterpret i64 0x8000000000000000u64)] x))\n"
  "(fn local-cast         [] i64 (let [x i64 (cast i64 0x8000000000000000u64)] x))\n"
  "\n"
  "(fn local-ones    [] i64 (let [x u64 0xFFFFFFFFFFFFFFFF] (cast i64 x)))\n"
  "(fn local-neg-hex [] i64 (let [x i64 -0xFF] x))\n"
  "(fn local-u8      [] i64 (let [x u8 200] (cast i64 x)))\n"
  "(fn local-mask    [] i64 (let [x u32 0xFF00FF00] (cast i64 x)))\n"
  "(fn local-small   [] i64 (let [x i64 5] x))\n"
  "\n"
  ";; contexts other than a val/var/let initializer that name an expected type\n"
  "(struct Packet [pts i64 flags u32])\n"
  "\n"
  "(fn takes-i64 [v i64] i64 v)\n"
  "\n"
  "(fn arg-hex-min    [] i64 (takes-i64 0x8000000000000000))\n"
  "(fn arg-small      [] i64 (takes-i64 5))\n"
  "(fn return-hex-min [] i64 (return 0x8000000000000000))\n"
  "(fn tail-hex-min   [] i64 0x8000000000000000)\n"
  "(fn tail-in-let    [] i64 (let [n i32 1] 0x7FFFFFFFFFFFFFFF))\n"
  "(fn tail-in-if-t   [] i64 (if true  0x8000000000000000 1))\n"
  "(fn tail-in-if-f   [] i64 (if false 1 0x8000000000000000))\n"
  "(fn field-hex-min  [] i64 (let [p Packet (Packet {:pts 0x8000000000000000 :flags 0xFF00FF00})]\n"
  "                            (. p pts)))\n"
  "(fn field-flags    [] i64 (let [p Packet (Packet {:pts 1 :flags 0xFF00FF00})]\n"
  "                            (cast i64 (. p flags))))\n"
  "\n"
  "(fn main [] i32 0)\n";

static Checker
check_fixture(TypedAst* tast, TypedIndex* out_root) {
  Ast ast;
  ast_init(&ast, ctx_perm());

  String8 src     = str8_cstring((char*)(g_fixture_source));
  u32     file_id = source_file_register(str8_lit("int_literal_test_fixture.3b"), src);

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

  // ~~ top-level `val`, where the declared type is what the literal adopts ~~
  expect_eq_i64("(val G_NOPTS i64 0x8000000000000000)", RUN("global-nopts"), INT64_MIN);
  expect_eq_i64("(val G_ONES u64 0xFFFFFFFFFFFFFFFF)", RUN("global-ones"),  (i64)UINT64_MAX);
  expect_eq_i64("(val G_SMALL i64 5)",                  RUN("global-small"), 5);

  // ~~ the same bit pattern spelled five ways, all of which must agree ~~
  expect_eq_i64("let i64 0x8000000000000000",     RUN("local-hex-min"),     INT64_MIN);
  expect_eq_i64("let i64 0x8000000000000000i64",  RUN("local-hex-min-sfx"), INT64_MIN);
  expect_eq_i64("let i64 -9223372036854775808",   RUN("local-dec-min"),     INT64_MIN);
  expect_eq_i64("let i64 (reinterpret i64 ...)",  RUN("local-reinterpret"), INT64_MIN);
  expect_eq_i64("let i64 (cast i64 ...)",         RUN("local-cast"),        INT64_MIN);

  // ~~ the rest of the range, and the narrower adopted types ~~
  expect_eq_i64("let u64 0xFFFFFFFFFFFFFFFF", RUN("local-ones"),    (i64)UINT64_MAX);
  expect_eq_i64("let i64 -0xFF",              RUN("local-neg-hex"), -255);
  expect_eq_i64("let u8 200",                 RUN("local-u8"),      200);
  expect_eq_i64("let u32 0xFF00FF00",         RUN("local-mask"),    0xFF00FF00);
  expect_eq_i64("let i64 5",                  RUN("local-small"),   5);

  // ~~ the other contexts that name an expected type: call argument, explicit
  // and implicit return, and a struct literal field ~~
  expect_eq_i64("(takes-i64 0x8000000000000000)", RUN("arg-hex-min"),    INT64_MIN);
  expect_eq_i64("(takes-i64 5)",                  RUN("arg-small"),      5);
  expect_eq_i64("(return 0x8000000000000000)",    RUN("return-hex-min"), INT64_MIN);
  expect_eq_i64("fn [] i64 0x8000000000000000",   RUN("tail-hex-min"),   INT64_MIN);
  expect_eq_i64("tail through a `let` body",      RUN("tail-in-let"),    INT64_MAX);
  expect_eq_i64("tail through `if`, then-arm",    RUN("tail-in-if-t"),   INT64_MIN);
  expect_eq_i64("tail through `if`, else-arm",    RUN("tail-in-if-f"),   INT64_MIN);
  expect_eq_i64("struct field :pts i64",          RUN("field-hex-min"),  INT64_MIN);
  expect_eq_i64("struct field :flags u32",        RUN("field-flags"),    0xFF00FF00);

  #undef RUN
  arena_temp_end(&fn_temp);

  if (g_failures == 0) printf("int_literal_test: all checks passed\n");
  else                 printf("int_literal_test: %d check(s) FAILED\n", g_failures);

  ctx_free();
  return g_failures == 0 ? 0 : 1;
}
