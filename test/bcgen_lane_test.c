// bcgen_lane_test.c -- the lane system on the bytecode VM: `parallel`,
// `parallel-for`, `lane-index`, `lane-count`, `lane-sync`, `lane-arena`.
//
// The VM runs these SERIALLY, as one lane (see bcgen.c's LANES note). That
// is a legal lane configuration rather than a stub -- the native backend
// produces exactly it on a single-core machine, `lane_count` being
// `Max(1, core_count - 1)` -- but "legal" is a claim about VALUES, so this
// file asserts the values rather than that compilation succeeded.
//
// The load-bearing case is parallel-for-sums-whole-range. Natively
// `parallel-for` walks only `lane_range(n)`, this lane's SLICE of
// `[0, n)`, and each lane's partial sum is reduced afterwards. Get the
// one-lane fallback wrong -- emit a slice, or an off-by-one bound -- and
// the program still runs and still prints a number, just a smaller one.
// A total of 499500 is only reachable by walking all 1000 indices.
//
// test/backend_diff.sh covers the same ground end-to-end by diffing
// examples/lanes across both backends. This exists underneath it: it names
// which builtin is wrong when that diff goes red, and it runs even when no
// C toolchain is available to build the native side.
#include "3b.h"
#include "bcgen.h"
#include "bcvm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;
static int g_checks   = 0;

static void
expect_eq_i64(const char* what, i64 got, i64 want) {
  g_checks += 1;
  if (got != want) {
    fprintf(stderr, "FAIL %s: got %lld, want %lld\n", what, (long long)got, (long long)want);
    g_failures += 1;
  }
}

// Globals, not locals, are how a `parallel` body reports back: its result has
// to outlive the block, and the checker deliberately hides the enclosing
// function's locals from it (Scope.hidden_lo) precisely because they do not
// exist on the lane thread natively.
//
// These bodies then ACCUMULATE into one shared global, which is sound only
// because this backend runs one lane. Do not copy the shape into real lane
// code -- examples/lanes shows the pattern that survives more than one lane,
// a `lane-count`-sized array with each lane owning the slot at its own
// `lane-index` and a serial reduce afterwards. A fixture that used it here
// would be testing the reduce loop, not the builtins this file is about.
static const char* g_fixture_source =
  "(package bcgen_lane_test)\n"
  "(struct Vec2 [x f32 y f32])\n"
  "(var sink i32 0)\n"
  "(var visits i32 0)\n"
  "\n"
  // A pool-wide constant, readable outside a `parallel` block -- which is what
  // lets a program size an output array before forking.
  "(fn lane-count-is-one [] i32 (cast i32 (lane-count)))\n"
  "\n"
  "(fn lane-index-is-zero [] i32\n"
  "  (set sink -1)\n"
  "  (parallel []\n"
  "    (void (set sink (cast i32 (lane-index)))))\n"
  "  sink)\n"
  "\n"
  // The whole `[0, 1000)` range, not a partition of it. See the header.
  //
  // The sum ALONE would not pin the range down: 499500 is the total whether
  // or not index 0 is visited, 0 contributing nothing -- an off-by-one at the
  // start reads as a pass. Counting the visits separately fixes both ends.
  "(fn parallel-for-sums-whole-range [] i32\n"
  "  (set sink 0)\n"
  "  (set visits 0)\n"
  "  (parallel [n 1000u32]\n"
  "    (parallel-for [i n]\n"
  "      (+= visits 1)\n"
  "      (void (+= sink (cast i32 i)))))\n"
  "  sink)\n"
  "\n"
  "(fn parallel-for-visit-count [] i32 visits)\n"
  "\n"
  // `count` is evaluated ONCE, before the loop. Natively it is an argument to
  // a single `lane_range(...)` call, so a side-effecting count that ran per
  // iteration here would diverge -- and would be invisible in every fixture
  // whose count is a literal. `calls` ends at 1, not at 5.
  "(var calls i32 0)\n"
  "(fn count-source [] u32 (+= calls 1) 5u32)\n"
  "(fn parallel-for-count-evaluated-once [] i32\n"
  "  (set calls 0)\n"
  "  (set visits 0)\n"
  "  (parallel []\n"
  "    (parallel-for [i (count-source)]\n"
  "      (void (+= visits 1))))\n"
  "  calls)\n"
  "\n"
  // Zero work: the body must not run at all, which a bound tested after the
  // first iteration rather than before it would get wrong.
  "(fn parallel-for-empty-range [] i32\n"
  "  (set visits 0)\n"
  "  (parallel [n 0u32]\n"
  "    (parallel-for [i n]\n"
  "      (void (+= visits 1))))\n"
  "  visits)\n"
  "\n"
  // A scalar capture is evaluated once by the caller and read inside the body.
  "(fn scalar-capture [] i32\n"
  "  (set sink 0)\n"
  "  (let [outer i32 21]\n"
  "    (parallel [c outer]\n"
  "      (void (set sink (* c 2)))))\n"
  "  sink)\n"
  "\n"
  // A struct capture is copied BY VALUE natively; mutating the capture inside
  // the body must therefore leave the caller's own value alone.
  "(fn struct_capture_is_a_copy [] i32\n"
  "  (set sink 0)\n"
  "  (let [v Vec2 (Vec2 {:x 3.0f32 :y 4.0f32})]\n"
  "    (parallel [c v]\n"
  "      (set (. c x) 99.0f32)\n"
  "      (void (set sink (cast i32 (. c y)))))\n"
  "    (set sink (+ sink (cast i32 (. v x)))))\n"
  "  sink)\n"
  "\n"
  // `lane-sync` must compile to something inert, and `lane-arena` to a real
  // arena a `push` can be written through and read back.
  "(fn sync-and-arena [] i32\n"
  "  (set sink 0)\n"
  "  (parallel []\n"
  "    (lane-sync)\n"
  "    (void (let [p i32* (push (lane-arena) i32)]\n"
  "            (set (deref p) 7)\n"
  "            (set sink (deref p)))))\n"
  "  sink)\n"
  "\n"
  // Two blocks back to back: each must bind and pop its own captures, so the
  // second one's `c` is its own rather than a leftover register from the first.
  "(fn two-blocks-dont-leak-captures [] i32\n"
  "  (set sink 0)\n"
  "  (parallel [c 10] (void (+= sink c)))\n"
  "  (parallel [c 5]  (void (+= sink c)))\n"
  "  sink)\n"
  "\n"
  "(fn main [] i32 0)\n";

static Checker
check_fixture(TypedAst* tast, TypedIndex* out_root) {
  Ast ast;
  ast_init(&ast, ctx_perm());

  String8 src     = str8_cstring((char*)(g_fixture_source));
  u32     file_id = source_file_register(str8_lit("bcgen_lane_test_fixture.3b"), src);

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

  expect_eq_i64("(lane-count) outside a `parallel` block", RUN("lane-count-is-one"), 1);
  expect_eq_i64("(lane-index) inside a `parallel` block",  RUN("lane-index-is-zero"), 0);
  expect_eq_i64("`parallel-for` walks all of [0, 1000)",   RUN("parallel-for-sums-whole-range"), 499500);
  // Reads back the counter the run above left behind -- must be its own call,
  // since a fn returns one value and both halves of that claim matter.
  expect_eq_i64("`parallel-for` visits exactly 1000 indices", RUN("parallel-for-visit-count"), 1000);
  expect_eq_i64("`parallel-for` evaluates its count once", RUN("parallel-for-count-evaluated-once"), 1);
  expect_eq_i64("...and still walked all 5 of them",       RUN("parallel-for-visit-count"), 5);
  expect_eq_i64("`parallel-for` over an empty range",      RUN("parallel-for-empty-range"), 0);
  expect_eq_i64("a scalar capture reaches the body",       RUN("scalar-capture"), 42);
  // 4 from the capture's own `y`, then 3 from the caller's `x` -- unchanged by
  // the body's write to 99, which landed on the copy.
  expect_eq_i64("a struct capture is copied by value",     RUN("struct_capture_is_a_copy"), 7);
  expect_eq_i64("`lane-sync` and `lane-arena`",            RUN("sync-and-arena"), 7);
  expect_eq_i64("back-to-back blocks bind their own captures", RUN("two-blocks-dont-leak-captures"), 15);

  #undef RUN
  arena_temp_end(&fn_temp);

  if (g_failures > 0) {
    fprintf(stderr, "bcgen_lane_test: %d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
  }
  printf("bcgen_lane_test: all %d checks passed (one lane, serially)\n", g_checks);
  return 0;
}
