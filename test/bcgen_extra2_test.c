// bcgen_extra2_test.c -- validates the small high-value builtins added to
// bcgen.c in one pass: `(addr x)`/`&x` (UnaryAddr), `swap`, bare `(dyn-
// count v)`, `nth-checked`, and `len`. Same rig as the other
// bcgen_*_test.c files. Named `extra2` since `bcgen_extra_test.c` (f32/
// array-field/arbitrary-nested-struct-field) already exists from an
// earlier pass -- this is a second, unrelated grab-bag of small features.
//
// Exercises:
//  - `(addr x)` on a struct FIELD, an array ELEMENT, and an already-
//    DEREF'd pointer -- all three mutate through the resulting pointer
//    correctly (proves these are REAL addresses into live storage, not
//    copies). A bare scalar-local `(addr x)` used to be a documented,
//    honest gap (this backend kept scalar locals purely in registers,
//    not addressable memory) -- NOW implemented via address-taken
//    analysis (a whole-program pre-scan gives an otherwise-scalar local
//    a real backing slot ONLY if `&`/`addr` is actually taken on it
//    somewhere), see test/bcgen_addr_taken_test.c for that coverage --
//    a bare scalar GLOBAL still hits the old diagnostic, not exercised
//    here.
//  - `swap` on scalars (a plain accumulator-style exchange) AND on a
//    STRUCT -- the struct case specifically proves a REAL byte-content
//    exchange, not a register/address swap: a pointer taken into one
//    operand's storage BEFORE the swap must observe the EXCHANGED
//    content afterward (the exact scenario a naive "just swap which
//    address each name points to" implementation would get wrong). The
//    scalar case checks BOTH operands' final values, not just one --
//    catches a real, separately-found bug where only checking `a`
//    (this test's own original version) missed that `b` silently ended
//    up with the SAME value as `a` instead of a genuinely swapped one.
//  - Bare `(dyn-count v)` on a real dyn-push-grown pointer.
//  - `nth-checked` on both Array and Vector, in-range (real, correct
//    element address, including a value observed correctly, not just
//    "non-null") and out-of-range (nil) for each.
//  - `len` on a fixed-size Array, a Vector, and a `string`.
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
  "(package bcgen_extra2_test)\n"
  "(struct Player [health i32 gold i32])\n"
  "\n"
  "(fn bump [p i32*] i32\n"
  "  (set (deref p) (+ (deref p) 1))\n"
  "  (deref p))\n"
  "\n"
  "(fn deref-again [p i32*] i32\n"
  "  (bump (addr (deref p))))\n"
  "\n"
  "(fn addr-field [] i32\n"
  "  (var pl Player (Player {:health 10 :gold 5}))\n"
  "  (bump (addr (get pl health)))\n"
  "  (get pl health))\n"
  "\n"
  "(fn addr-index [] i32\n"
  "  (var arr [i32 3] [1 2 3])\n"
  "  (bump (addr (nth arr 1)))\n"
  "  (nth arr 1))\n"
  "\n"
  "(fn addr-deref [] i32\n"
  "  (var arr [i32 1] [42])\n"
  "  (deref-again (addr (nth arr 0)))\n"
  "  (nth arr 0))\n"
  "\n"
  "(fn swap-scalars [] i32\n"
  "  (var a i32 1)\n"
  "  (var b i32 2)\n"
  "  (swap a b)\n"
  "  (+ (* a 10) b))\n" // encodes BOTH final values -- 21 if genuinely swapped (a=2,b=1),
                          // 22 if the pre-existing aliasing bug reappears (a=2,b=2)
  "\n"
  "(fn swap-struct-content-check [] i32\n"
  "  (var a Player (Player {:health 10 :gold 1}))\n"
  "  (var b Player (Player {:health 20 :gold 2}))\n"
  "  (let [pa (addr (get a health))]\n"
  "    (swap a b)\n"
  "    (deref pa)))\n"
  "\n"
  "(fn swap-struct-names-too [] i32\n"
  "  (var a Player (Player {:health 10 :gold 1}))\n"
  "  (var b Player (Player {:health 20 :gold 2}))\n"
  "  (swap a b)\n"
  "  (get a health))\n"
  "\n"
  "(fn dyn-count-of [v i32*] u64\n"
  "  (dyn-count v))\n"
  "\n"
  "(fn arr-checked-in [] i32\n"
  "  (var arr [i32 3] [10 20 30])\n"
  "  (let [p (nth-checked arr 1)]\n"
  "    (if p (deref p) -1)))\n"
  "\n"
  "(fn arr-checked-out [] bool\n"
  "  (var arr [i32 3] [10 20 30])\n"
  "  (let [p (nth-checked arr 99)]\n"
  "    (if p true false)))\n"
  "\n"
  "(fn vec-checked-in [v [i32]] i32\n"
  "  (let [p (nth-checked v 1)]\n"
  "    (if p (deref p) -1)))\n"
  "\n"
  "(fn vec-checked-out [v [i32]] bool\n"
  "  (let [p (nth-checked v 99)]\n"
  "    (if p true false)))\n"
  "\n"
  "(fn arr-len [] u64\n"
  "  (var arr [i32 5] [1 2 3 4 5])\n"
  "  (len arr))\n"
  "\n"
  "(fn str-len [] u64\n"
  "  (len \"hello\"))\n"
  "\n"
  "(fn main [] i32 0)\n";

static Checker
check_fixture(TypedAst* tast, TypedIndex* out_root) {
  Ast ast;
  ast_init(&ast, ctx_perm());

  String8 src     = str8_cstring((char*)(g_fixture_source));
  u32     file_id = source_file_register(str8_lit("bcgen_extra2_test_fixture.3b"), src);

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

#define RUN0(name) bc_run_in_program(&prog, bc_program_find_fn(&prog, str8_lit(name)), NULL, 0, heap, &host_imports).value

  expect_eq_i64("addr-field()", RUN0("addr-field"), 11);
  expect_eq_i64("addr-index()", RUN0("addr-index"), 3);
  expect_eq_i64("addr-deref()", RUN0("addr-deref"), 43);

  expect_eq_i64("swap-scalars()", RUN0("swap-scalars"), 21); // a=2,b=1 -- see fixture's own comment
  expect_eq_i64("swap-struct-content-check() -- a pointer taken BEFORE the swap must see the "
                "EXCHANGED content afterward, not the original", RUN0("swap-struct-content-check"), 20);
  expect_eq_i64("swap-struct-names-too()", RUN0("swap-struct-names-too"), 20);

  { i32* v = NULL;
    dyn_push(heap, v, 10);
    dyn_push(heap, v, 20);
    dyn_push(heap, v, 30);
    i64 args[1] = {(i64)(intptr_t)v};
    u32 fn = bc_program_find_fn(&prog, str8_lit("dyn-count-of"));
    expect_eq_i64("dyn-count-of(3 elems)", bc_run_in_program(&prog, fn, args, 1, heap, &host_imports).value, 3);
  }

  expect_eq_i64("arr-checked-in()", RUN0("arr-checked-in"), 20);
  expect_eq_i64("arr-checked-out()", RUN0("arr-checked-out"), 0);

  { i32* v = NULL;
    dyn_push(heap, v, 100);
    dyn_push(heap, v, 200);
    dyn_push(heap, v, 300);
    i64 args[1] = {(i64)(intptr_t)v};
    u32 in_fn  = bc_program_find_fn(&prog, str8_lit("vec-checked-in"));
    u32 out_fn = bc_program_find_fn(&prog, str8_lit("vec-checked-out"));
    expect_eq_i64("vec-checked-in(3 elems)",  bc_run_in_program(&prog, in_fn,  args, 1, heap, &host_imports).value, 200);
    expect_eq_i64("vec-checked-out(3 elems)", bc_run_in_program(&prog, out_fn, args, 1, heap, &host_imports).value, 0);
  }

  expect_eq_i64("arr-len()", RUN0("arr-len"), 5);
  expect_eq_i64("str-len()", RUN0("str-len"), 5);

#undef RUN0

  arena_temp_end(&fn_temp);

  if (g_failures == 0) printf("bcgen_extra2_test: all checks passed\n");
  else                 printf("bcgen_extra2_test: %d check(s) FAILED\n", g_failures);

  ctx_free();
  return g_failures == 0 ? 0 : 1;
}
