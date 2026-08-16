// bcgen_foreach_test.c -- validates collection `for` (TypedNodeKind_
// ForEachExpr) over Array/Vector added to bcgen.c/bcvm.c. Set/Map are
// deliberately NOT covered (need to walk a HashTable's own slot array,
// skipping empty/tombstone entries -- a genuinely separate mechanism, see
// this file's own top-of-file note in bcgen.c). Same rig as the other
// bcgen_*_test.c files.
//
// Exercises:
//  - Plain `[item arr]` over a fixed-size Array.
//  - `[[i item] arr]` (WITH index) over an Array -- proves the internal
//    counter is correctly ALSO exposed as a real, readable local when
//    requested.
//  - A STRUCT-element Array, read through ordinary field access on the
//    loop variable -- proves the embedded-element (address-only, no load)
//    path works, not just the scalar-element path.
//  - `[item v]` over a REAL Vector (constructed via base.h's own
//    `dyn_push` directly in this C harness -- a genuine DynHdr-backed
//    array, not a plain `push`-allocated pointer, which has NO hidden
//    header at all) -- proves BcOp_DynCount correctly reads the count a
//    Vector actually has at RUNTIME, not a compile-time-baked one the
//    way Array's own count is.
//  - An EMPTY Vector -- the loop body must never run.
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
expect_eq_f32(const char* what, i64 got_bits, f32 want) {
  f32 got; u32 bits32 = (u32)got_bits; memcpy(&got, &bits32, sizeof(got));
  if (got != want) {
    fprintf(stderr, "FAIL %s: got %g, want %g\n", what, (double)got, (double)want);
    g_failures += 1;
  }
}

static const char* g_fixture_source =
  "(package bcgen_foreach_test)\n"
  "(struct Vec2 [x f32 y f32])\n"
  "\n"
  "(fn sum-array [] i32\n"
  "  (var arr [i32 5] [1 2 3 4 5])\n"
  "  (var total i32 0)\n"
  "  (for [x arr] (set total (+ total x)))\n"
  "  total)\n"
  "\n"
  "(fn sum-array-indexed [] i32\n"
  "  (var arr [i32 3] [10 20 30])\n"
  "  (var total i32 0)\n"
  "  (for [[i x] arr] (set total (+ total (* (cast i32 i) x))))\n"
  "  total)\n"
  "\n"
  "(fn sum-struct-array-field [] f32\n"
  "  (var arr [Vec2 2] [(Vec2 {:x 1.0f32 :y 2.0f32}) (Vec2 {:x 3.0f32 :y 4.0f32})])\n"
  "  (var total f32 0.0f32)\n"
  "  (for [v arr] (set total (+ total (. v x))))\n"
  "  total)\n"
  "\n"
  "(fn sum-vector [v [i32]] i32\n"
  "  (var total i32 0)\n"
  "  (for [x v] (set total (+ total x)))\n"
  "  total)\n"
  "\n"
  "(fn main [] i32 0)\n";

static Checker
check_fixture(TypedAst* tast, TypedIndex* out_root) {
  Ast ast;
  ast_init(&ast, ctx_perm());

  String8 src     = str8_cstring((char*)(g_fixture_source));
  u32     file_id = source_file_register(str8_lit("bcgen_foreach_test_fixture.3b"), src);

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

  u32 sum_array_fn                = bc_program_find_fn(&prog, str8_lit("sum-array"));
  u32 sum_array_indexed_fn        = bc_program_find_fn(&prog, str8_lit("sum-array-indexed"));
  u32 sum_struct_array_field_fn  = bc_program_find_fn(&prog, str8_lit("sum-struct-array-field"));
  u32 sum_vector_fn                = bc_program_find_fn(&prog, str8_lit("sum-vector"));

  Arena* heap = ctx_perm();

  expect_eq_i64("sum-array()",         bc_run_in_program(&prog, sum_array_fn,         NULL, 0, heap, &host_imports).value, 15);
  expect_eq_i64("sum-array-indexed()", bc_run_in_program(&prog, sum_array_indexed_fn, NULL, 0, heap, &host_imports).value, 80);
  { BcResult r = bc_run_in_program(&prog, sum_struct_array_field_fn, NULL, 0, heap, &host_imports);
    expect_eq_f32("sum-struct-array-field()", r.value, 4.0f);
  }

  // ~~ A REAL Vector, constructed via base.h's own `dyn_push` directly --
  // a genuine DynHdr-backed array (unlike `push`, which has NO hidden
  // header at all), exactly what BcOp_DynCount needs to read.
  { i32* v = NULL;
    dyn_push(heap, v, 100);
    dyn_push(heap, v, 200);
    dyn_push(heap, v, 300);
    dyn_push(heap, v, 400);
    i64 args[1] = {(i64)(intptr_t)v};
    expect_eq_i64("sum-vector(4 elems)", bc_run_in_program(&prog, sum_vector_fn, args, 1, heap, &host_imports).value, 1000);
  }
  { i32* empty = NULL; // an empty Vector -- the loop body must never run
    i64 args[1] = {(i64)(intptr_t)empty};
    expect_eq_i64("sum-vector(empty)", bc_run_in_program(&prog, sum_vector_fn, args, 1, heap, &host_imports).value, 0);
  }

  arena_temp_end(&fn_temp);

  if (g_failures == 0) printf("bcgen_foreach_test: all checks passed\n");
  else                 printf("bcgen_foreach_test: %d check(s) FAILED\n", g_failures);

  ctx_free();
  return g_failures == 0 ? 0 : 1;
}
