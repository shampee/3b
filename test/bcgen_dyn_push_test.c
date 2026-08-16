// bcgen_dyn_push_test.c -- validates `dyn-push`/`vector-push` (TypedNodeKind_
// DynPush) and `commit` (TypedNodeKind_CommitExpr) support added to bcgen.c/
// bcvm.c: base.h's own `dyn_push` is a STATEMENT MACRO (reseats the
// caller's own pointer variable on growth), not a callable function, so
// this needed real bytecode-level growth logic (BcOp_DynGrow, reimplementing
// `arena_dyn_grow` exactly) rather than a thin opcode wrapper the way
// arena support's `push`/`push0` could just call `arena_push` directly.
// Same rig as the other bcgen_*_test.c files.
//
// Exercises:
//  - Pushing enough elements (20) to force THREE growth doublings
//    (capacity 0 -> 8 -> 16 -> 32), verifying both the final count AND
//    that every previously-pushed value survived each reallocation.
//  - `dyn-push` on a MODULE-LEVEL GLOBAL Vector, across TWO SEPARATE
//    bc_run_in_program calls, proving growth persists (both the new
//    pointer AND the accumulated data) across calls the same way
//    module-level globals already had to for `var`/`val`.
//  - `dyn-push` of a STRUCT-typed element (the embedded bc_store_value
//    path, not just a scalar).
//  - `commit`: push into a scratch-Arena-backed Vector (leaving real
//    growth-by-doubling slack), commit into a SEPARATE destination
//    arena, and confirm the result is EXACTLY right-sized (count ==
//    capacity == pushed count, no leftover slack) with correct values.
//  - `commit` of an EMPTY (never-pushed, still-nil) Vector returns NULL,
//    not a zero-length allocation.
//  - `nth` READ, `set` WRITE, and `addr`/`deref` all through a Vector base
//    (not just Array/Pointer) -- bc_resolve_index_access (bcgen.c) used to
//    xassert on a Vector-typed base (checker.c's own IndexAccess case
//    already allowed it, so this was a compiler-crash-on-valid-input bug,
//    not a rejected program), and checker.c's SetTargetKind_Index case
//    separately rejected `(set (nth vector-var ...) ...)` outright with a
//    real diagnostic. Both are fixed since a Vector's runtime
//    representation is already a bare T* -- identical arithmetic to the
//    Pointer case, see bc_resolve_index_access's own updated comment.
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
  "(package bcgen_dyn_push_test)\n"
  "(struct Vec2 [x f32 y f32])\n"
  "\n"
  "(var g-vec (Vector i32))\n"
  "\n"
  // `a` is caller-supplied rather than `(create)`d internally -- the returned
  // Vector's own backing memory lives IN `a`, so this function could never
  // legally destroy it before returning anyway; the caller owns that
  // lifetime instead, same as any real 3b program would do.
  "(fn push-range [n i32 a arena] [i32]\n"
  "  (var vec (Vector i32))\n"
  "  (for [i 0 n] (dyn-push a vec i))\n"
  "  vec)\n"
  "\n"
  "(fn push-global [n i32 a arena] [i32]\n"
  "  (for [i 0 n] (dyn-push a g-vec i))\n"
  "  g-vec)\n"
  "\n"
  "(fn push-struct [a arena] [Vec2]\n"
  "  (var vec (Vector Vec2))\n"
  "  (dyn-push a vec (Vec2 {:x 1.0f32 :y 2.0f32}))\n"
  "  (dyn-push a vec (Vec2 {:x 3.0f32 :y 4.0f32}))\n"
  "  vec)\n"
  "\n"
  // Growth happens into a genuinely throwaway `scratch` arena (self-
  // cleaning on the block's own close -- no leak), unlike the three
  // functions above: `commit`'s whole POINT is trimming that growth
  // slack into a right-sized copy in `dst`, so nothing from the scratch
  // region needs to survive past this call.
  "(fn push-then-commit [n i32 dst arena] i32*\n"
  "  (var vec i32* nil)\n"
  "  (scratch [t]\n"
  "    (for [i 0 n] (dyn-push t vec i))\n"
  "    (commit dst vec)))\n"
  "\n"
  "(fn commit-empty [dst arena] i32*\n"
  "  (var vec i32* nil)\n"
  "  (commit dst vec))\n"
  "\n"
  "(fn nth-vector-roundtrip [a arena] i32\n"
  "  (var vec (Vector i32))\n"
  "  (dyn-push a vec 10)\n"
  "  (dyn-push a vec 20)\n"
  "  (dyn-push a vec 30)\n"
  "  (set (nth vec 1) 99)\n"
  "  (var p i32* (addr (nth vec 2)))\n"
  "  (set (deref p) (+ (deref p) 1))\n"
  "  (+ (* (nth vec 0) 1000) (+ (* (nth vec 1) 10) (nth vec 2))))\n"
  "\n"
  "(fn main [] i32 0)\n";

static Checker
check_fixture(TypedAst* tast, TypedIndex* out_root) {
  Ast ast;
  ast_init(&ast, ctx_perm());

  String8 src     = str8_cstring((char*)(g_fixture_source));
  u32     file_id = source_file_register(str8_lit("bcgen_dyn_push_test_fixture.3b"), src);

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

typedef struct { f32 x, y; } Vec2C;

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

  u32 push_range_fn      = bc_program_find_fn(&prog, str8_lit("push-range"));
  u32 push_global_fn      = bc_program_find_fn(&prog, str8_lit("push-global"));
  u32 push_struct_fn      = bc_program_find_fn(&prog, str8_lit("push-struct"));
  u32 push_then_commit_fn = bc_program_find_fn(&prog, str8_lit("push-then-commit"));
  u32 commit_empty_fn      = bc_program_find_fn(&prog, str8_lit("commit-empty"));
  u32 nth_vector_fn        = bc_program_find_fn(&prog, str8_lit("nth-vector-roundtrip"));

  Arena* heap = ctx_perm();

  // Caller-owned, for whatever needs its data to outlive the call that
  // wrote it (push-range/push-global/push-struct's returned Vectors all
  // live in here) -- destroyed once, explicitly, at the very end, rather
  // than each `.3bs` function trying (and being unable) to free memory
  // its own return value still points into.
  Arena data_arena = arena_create_vm(MB(1));

  // ~~ 20 pushes -- forces THREE growth doublings (0 -> 8 -> 16 -> 32) ~~
  { i64 args[2] = {20, (i64)(intptr_t)&data_arena};
    i64 result = bc_run_in_program(&prog, push_range_fn, args, 2, heap, &host_imports).value;
    i32* v = (i32*)(intptr_t)result;
    expect_eq_i64("push-range(20) count",    (i64)dyn_count(v),    20);
    expect_eq_i64("push-range(20) capacity", (i64)dyn_capacity(v), 32);
    b32 all_correct = true;
    for (i32 i = 0; i < 20; i += 1) if (v[i] != i) all_correct = false;
    if (!all_correct) { fprintf(stderr, "FAIL push-range(20): values corrupted across growth\n"); g_failures += 1; }
  }

  // ~~ module-level global Vector, mutated across TWO SEPARATE calls ~~
  { i64 args[2] = {3, (i64)(intptr_t)&data_arena};
    i64 result = bc_run_in_program(&prog, push_global_fn, args, 2, heap, &host_imports).value;
    i32* v = (i32*)(intptr_t)result;
    expect_eq_i64("push-global(3) count", (i64)dyn_count(v), 3);
  }
  { i64 args[2] = {2, (i64)(intptr_t)&data_arena}; // a SEPARATE call -- the global should already hold 3, growing to 5
    i64 result = bc_run_in_program(&prog, push_global_fn, args, 2, heap, &host_imports).value;
    i32* v = (i32*)(intptr_t)result;
    expect_eq_i64("push-global(2) after push-global(3): count", (i64)dyn_count(v), 5);
    i32 want[5] = {0, 1, 2, 0, 1}; // first call's 0,1,2 then second call's own 0,1
    b32 all_correct = true;
    for (i32 i = 0; i < 5; i += 1) if (v[i] != want[i]) all_correct = false;
    if (!all_correct) { fprintf(stderr, "FAIL push-global: accumulated values wrong -- global didn't persist growth/data across calls\n"); g_failures += 1; }
  }

  // ~~ struct-element push (embedded bc_store_value path) ~~
  { i64 args[1] = {(i64)(intptr_t)&data_arena};
    i64 result = bc_run_in_program(&prog, push_struct_fn, args, 1, heap, &host_imports).value;
    Vec2C* v = (Vec2C*)(intptr_t)result;
    expect_eq_i64("push-struct() count", (i64)dyn_count(v), 2);
    if (v[0].x != 1.0f || v[0].y != 2.0f || v[1].x != 3.0f || v[1].y != 4.0f) {
      fprintf(stderr, "FAIL push-struct(): got (%g,%g) (%g,%g)\n", (double)v[0].x, (double)v[0].y, (double)v[1].x, (double)v[1].y);
      g_failures += 1;
    }
  }

  // ~~ commit: trims growth-by-doubling slack down to an exact fit ~~
  { Arena dst = arena_create_vm(MB(1));
    i64 args[2] = {5, (i64)(intptr_t)&dst}; // 5 elements -- source Vector would have capacity 8 (slack)
    i64 result = bc_run_in_program(&prog, push_then_commit_fn, args, 2, heap, &host_imports).value;
    i32* v = (i32*)(intptr_t)result;
    expect_eq_i64("push-then-commit(5) count",    (i64)dyn_count(v),    5);
    expect_eq_i64("push-then-commit(5) capacity", (i64)dyn_capacity(v), 5); // EXACTLY 5, no leftover slack
    b32 all_correct = true;
    for (i32 i = 0; i < 5; i += 1) if (v[i] != i) all_correct = false;
    if (!all_correct) { fprintf(stderr, "FAIL push-then-commit(5): values wrong after commit\n"); g_failures += 1; }
    arena_destroy(&dst);
  }

  // ~~ commit of a never-pushed (still-nil) Vector -- NULL, not a
  // zero-length allocation ~~
  { Arena dst = arena_create_vm(MB(1));
    i64 args[1] = {(i64)(intptr_t)&dst};
    i64 result = bc_run_in_program(&prog, commit_empty_fn, args, 1, heap, &host_imports).value;
    expect_eq_i64("commit-empty() is NULL", result, 0);
    arena_destroy(&dst);
  }

  // ~~ nth read/set/addr through a Vector base -- see this file's
  // top-of-file note ~~
  { i64 args[1] = {(i64)(intptr_t)&data_arena};
    i64 result = bc_run_in_program(&prog, nth_vector_fn, args, 1, heap, &host_imports).value;
    expect_eq_i64("nth-vector-roundtrip()", result, 11021); // 10*1000 + 99*10 + (30+1)
  }

  arena_destroy(&data_arena);
  arena_temp_end(&fn_temp);

  if (g_failures == 0) printf("bcgen_dyn_push_test: all checks passed\n");
  else                 printf("bcgen_dyn_push_test: %d check(s) FAILED\n", g_failures);

  ctx_free();
  return g_failures == 0 ? 0 : 1;
}
