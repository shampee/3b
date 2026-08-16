// bcgen_index_of_test.c -- validates `vector-index-of`
// (TypedNodeKind_IndexOf) support added to bcgen.c/bcvm.c: a linear-
// search loop built from BcOp_DynCount (task 6) + Mul/Add addressing +
// a scalar-or-embedded comparison dispatch, landing in the shared
// bc_compile_bool_t_result `(bool u64)` struct. Same rig as the other
// bcgen_*_test.c files.
//
// Exercises:
//  - A match at index 0 (the FIRST element) -- the trivial case.
//  - A match in the MIDDLE and at the LAST index -- these are the cases
//    that actually exercise the loop's "no match, advance index, keep
//    looping" path more than once; a register-lifetime bug in that path
//    (an uninitialized "advance by 1" register read as 0) would hang
//    forever on exactly these instead of just returning a wrong answer,
//    so these cases are the real regression check.
//  - A value NOT present at all -- proves the loop terminates and
//    reports not-found rather than hanging or reading out of bounds.
//  - An EMPTY Vector -- the loop body must never run.
//  - A struct-element Vector (embedded comparison path, not the scalar
//    Eq/FEq/F32Eq path) matching in the middle.
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
  "(package bcgen_index_of_test)\n"
  "(struct Vec2 [x f32 y f32])\n"
  "\n"
  "(fn find-i32 [v [i32] needle i32] bool (. (vector-index-of v needle) _0))\n"
  "(fn find-i32-idx [v [i32] needle i32] u64 (. (vector-index-of v needle) _1))\n"
  "\n"
  "(fn find-vec2 [v [Vec2] needle Vec2] bool (. (vector-index-of v needle) _0))\n"
  "(fn find-vec2-idx [v [Vec2] needle Vec2] u64 (. (vector-index-of v needle) _1))\n"
  "\n"
  "(fn main [] i32 0)\n";

static Checker
check_fixture(TypedAst* tast, TypedIndex* out_root) {
  Ast ast;
  ast_init(&ast, ctx_perm());

  String8 src     = str8_cstring((char*)(g_fixture_source));
  u32     file_id = source_file_register(str8_lit("bcgen_index_of_test_fixture.3b"), src);

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

  u32 find_i32_fn      = bc_program_find_fn(&prog, str8_lit("find-i32"));
  u32 find_i32_idx_fn  = bc_program_find_fn(&prog, str8_lit("find-i32-idx"));
  u32 find_vec2_fn      = bc_program_find_fn(&prog, str8_lit("find-vec2"));
  u32 find_vec2_idx_fn  = bc_program_find_fn(&prog, str8_lit("find-vec2-idx"));

  Arena* heap = ctx_perm();

  // ~~ i32 Vector: [10 20 30 40 50] ~~
  { i32* v = NULL;
    dyn_push(heap, v, 10);
    dyn_push(heap, v, 20);
    dyn_push(heap, v, 30);
    dyn_push(heap, v, 40);
    dyn_push(heap, v, 50);

    { i64 args[2] = {(i64)(intptr_t)v, 10}; // FIRST element
      expect_eq_i64("find-i32(10)",     bc_run_in_program(&prog, find_i32_fn,     args, 2, heap, &host_imports).value, 1);
      expect_eq_i64("find-i32-idx(10)", bc_run_in_program(&prog, find_i32_idx_fn, args, 2, heap, &host_imports).value, 0);
    }
    { i64 args[2] = {(i64)(intptr_t)v, 30}; // MIDDLE element -- exercises the loop-advance path
      expect_eq_i64("find-i32(30)",     bc_run_in_program(&prog, find_i32_fn,     args, 2, heap, &host_imports).value, 1);
      expect_eq_i64("find-i32-idx(30)", bc_run_in_program(&prog, find_i32_idx_fn, args, 2, heap, &host_imports).value, 2);
    }
    { i64 args[2] = {(i64)(intptr_t)v, 50}; // LAST element -- exercises the loop-advance path 4x
      expect_eq_i64("find-i32(50)",     bc_run_in_program(&prog, find_i32_fn,     args, 2, heap, &host_imports).value, 1);
      expect_eq_i64("find-i32-idx(50)", bc_run_in_program(&prog, find_i32_idx_fn, args, 2, heap, &host_imports).value, 4);
    }
    { i64 args[2] = {(i64)(intptr_t)v, 999}; // NOT present -- must terminate, not hang
      expect_eq_i64("find-i32(999)",     bc_run_in_program(&prog, find_i32_fn,     args, 2, heap, &host_imports).value, 0);
      expect_eq_i64("find-i32-idx(999)", bc_run_in_program(&prog, find_i32_idx_fn, args, 2, heap, &host_imports).value, 5);
    }
  }
  { i32* empty = NULL; // empty Vector -- loop body must never run
    i64 args[2] = {(i64)(intptr_t)empty, 7};
    expect_eq_i64("find-i32(empty)",     bc_run_in_program(&prog, find_i32_fn,     args, 2, heap, &host_imports).value, 0);
    expect_eq_i64("find-i32-idx(empty)", bc_run_in_program(&prog, find_i32_idx_fn, args, 2, heap, &host_imports).value, 0);
  }

  // ~~ struct-element Vector: [(1,1) (2,2) (3,3)] -- embedded comparison path ~~
  { Vec2C* v = NULL;
    Vec2C a = {1.0f, 1.0f}; dyn_push(heap, v, a);
    Vec2C b = {2.0f, 2.0f}; dyn_push(heap, v, b);
    Vec2C c = {3.0f, 3.0f}; dyn_push(heap, v, c);

    Vec2C needle = {2.0f, 2.0f}; // MIDDLE
    // needle is passed embedded (by address) like any other struct arg --
    // stash it on the heap and pass its address, matching how the
    // compiler expects a Named-type argument register to be an address.
    Vec2C* needle_slot = (Vec2C*)arena_push(heap, sizeof(Vec2C), _Alignof(Vec2C));
    *needle_slot = needle;

    i64 args[2] = {(i64)(intptr_t)v, (i64)(intptr_t)needle_slot};
    expect_eq_i64("find-vec2(2,2)",     bc_run_in_program(&prog, find_vec2_fn,     args, 2, heap, &host_imports).value, 1);
    expect_eq_i64("find-vec2-idx(2,2)", bc_run_in_program(&prog, find_vec2_idx_fn, args, 2, heap, &host_imports).value, 1);
  }
  { Vec2C* v = NULL;
    Vec2C a = {1.0f, 1.0f}; dyn_push(heap, v, a);

    Vec2C needle = {9.0f, 9.0f}; // not present
    Vec2C* needle_slot = (Vec2C*)arena_push(heap, sizeof(Vec2C), _Alignof(Vec2C));
    *needle_slot = needle;

    i64 args[2] = {(i64)(intptr_t)v, (i64)(intptr_t)needle_slot};
    expect_eq_i64("find-vec2(9,9)", bc_run_in_program(&prog, find_vec2_fn, args, 2, heap, &host_imports).value, 0);
  }

  arena_temp_end(&fn_temp);

  if (g_failures == 0) printf("bcgen_index_of_test: all checks passed\n");
  else                 printf("bcgen_index_of_test: %d check(s) FAILED\n", g_failures);

  ctx_free();
  return g_failures == 0 ? 0 : 1;
}
