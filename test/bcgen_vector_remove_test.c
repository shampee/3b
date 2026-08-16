// bcgen_vector_remove_test.c -- validates `vector-clear`/`vector-swap-
// remove`/`vector-remove-at`/`vector-contains?` support added to bcgen.c
// (no new opcodes -- reuses the existing BcOp_DynCount/DynSetCount plus
// ordinary index*stride pointer arithmetic and bc_store_value's own
// embedded-vs-scalar dispatch). A real gap found by running
// examples/vector/main.3b through the bytecode VM -- previously "no
// compiled function or registered host import by that name" for all four
// (`vector-index-of` alone already worked, via its own dedicated
// TypedNodeKind_IndexOf node -- these four stayed plain, unresolved Call
// nodes). Same rig as the other bcgen_*_test.c files.
//
// Exercises:
//  - `vector-clear` on a non-empty Vector, AND on a never-pushed (still
//    NULL) one -- the NULL-guard path specifically, matching codegen.c's
//    own `if (_3b_v) {...}` guard.
//  - `vector-swap-remove`/`vector-remove-at`, both the IN-RANGE
//    (mutating, returns true) and OUT-OF-RANGE (no mutation, returns
//    false) cases, on a SCALAR (i32) element type.
//  - `vector-swap-remove` on a STRUCT element type -- exercises the
//    embedded (address, not value) dispatch path bc_store_value's own
//    byte-copy machinery needs, not just the scalar load/store path.
//  - `vector-contains?` on scalar, string, AND a comparable struct
//    element type -- proves it shares the exact same embedded-vs-scalar
//    equality dispatch TypedNodeKind_IndexOf already established, not a
//    narrower reimplementation.
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
  "(package bcgen_vector_remove_test)\n"
  "\n"
  "(struct P [x i32 y i32])\n"
  "\n"
  "(fn clear-nonempty [a arena] i32\n"
  "  (var v [i32])\n"
  "  (vector-push a v 1)\n"
  "  (vector-push a v 2)\n"
  "  (vector-clear v)\n"
  "  (cast i32 (len v)))\n"
  "\n"
  "(fn clear-never-pushed [] i32\n"
  "  (var v [i32])\n"
  "  (vector-clear v)\n"
  "  (cast i32 (len v)))\n"
  "\n"
  "(fn swap-remove-in-range [a arena] i32\n"
  "  (var v [i32])\n"
  "  (for [i 0 5] (vector-push a v i)) ;; [0 1 2 3 4]\n"
  "  (val ok bool (vector-swap-remove v 1))\n"
  "  ;; expect ok, v -> [0 4 2 3], len 4\n"
  "  (if (and ok (= (nth v 1) 4) (= (len v) 4u64)) 1 0))\n"
  "\n"
  "(fn swap-remove-out-of-range [a arena] i32\n"
  "  (var v [i32])\n"
  "  (for [i 0 3] (vector-push a v i))\n"
  "  (val ok bool (vector-swap-remove v 99))\n"
  "  (if (and (not ok) (= (len v) 3u64)) 1 0))\n"
  "\n"
  "(fn remove-at-in-range [a arena] i32\n"
  "  (var v [i32])\n"
  "  (for [i 0 5] (vector-push a v i)) ;; [0 1 2 3 4]\n"
  "  (val ok bool (vector-remove-at v 1))\n"
  "  ;; expect ok, v -> [0 2 3 4], len 4\n"
  "  (if (and ok (= (nth v 0) 0) (= (nth v 1) 2) (= (nth v 3) 4) (= (len v) 4u64)) 1 0))\n"
  "\n"
  "(fn remove-at-out-of-range [a arena] i32\n"
  "  (var v [i32])\n"
  "  (for [i 0 3] (vector-push a v i))\n"
  "  (val ok bool (vector-remove-at v 99))\n"
  "  (if (and (not ok) (= (len v) 3u64)) 1 0))\n"
  "\n"
  "(fn swap-remove-struct [a arena] i32\n"
  "  (var v [P])\n"
  "  (vector-push a v (P {:x 1 :y 1}))\n"
  "  (vector-push a v (P {:x 2 :y 2}))\n"
  "  (vector-push a v (P {:x 3 :y 3}))\n"
  "  (val ok bool (vector-swap-remove v 0))\n"
  "  ;; expect ok, v[0] -> (3,3) (last swapped in), v[1] -> (2,2), len 2\n"
  "  (if (and ok (= (. (nth v 0) x) 3) (= (. (nth v 1) x) 2) (= (len v) 2u64)) 1 0))\n"
  "\n"
  "(fn contains-scalar [a arena] i32\n"
  "  (var v [i32])\n"
  "  (for [i 0 5] (vector-push a v (* i 10))) ;; [0 10 20 30 40]\n"
  "  (if (and (vector-contains? v 20) (not (vector-contains? v 99))) 1 0))\n"
  "\n"
  "(fn contains-string [a arena] i32\n"
  "  (var v [string])\n"
  "  (vector-push a v \"alice\")\n"
  "  (vector-push a v \"bob\")\n"
  "  (if (and (vector-contains? v \"bob\") (not (vector-contains? v \"carol\"))) 1 0))\n"
  "\n"
  "(fn contains-struct [a arena] i32\n"
  "  (var v [P])\n"
  "  (vector-push a v (P {:x 1 :y 1}))\n"
  "  (vector-push a v (P {:x 2 :y 2}))\n"
  "  (val target P (P {:x 2 :y 2}))\n"
  "  (val missing P (P {:x 9 :y 9}))\n"
  "  (if (and (vector-contains? v target) (not (vector-contains? v missing))) 1 0))\n"
  "\n"
  "(fn main [] i32 0)\n";

static Checker
check_fixture(TypedAst* tast, TypedIndex* out_root) {
  Ast ast;
  ast_init(&ast, ctx_perm());

  String8 src     = str8_cstring((char*)(g_fixture_source));
  u32     file_id = source_file_register(str8_lit("bcgen_vector_remove_test_fixture.3b"), src);

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

  u32 clear_nonempty_fn        = bc_program_find_fn(&prog, str8_lit("clear-nonempty"));
  u32 clear_never_pushed_fn     = bc_program_find_fn(&prog, str8_lit("clear-never-pushed"));
  u32 swap_in_range_fn           = bc_program_find_fn(&prog, str8_lit("swap-remove-in-range"));
  u32 swap_oob_fn                 = bc_program_find_fn(&prog, str8_lit("swap-remove-out-of-range"));
  u32 remove_in_range_fn          = bc_program_find_fn(&prog, str8_lit("remove-at-in-range"));
  u32 remove_oob_fn               = bc_program_find_fn(&prog, str8_lit("remove-at-out-of-range"));
  u32 swap_struct_fn              = bc_program_find_fn(&prog, str8_lit("swap-remove-struct"));
  u32 contains_scalar_fn          = bc_program_find_fn(&prog, str8_lit("contains-scalar"));
  u32 contains_string_fn          = bc_program_find_fn(&prog, str8_lit("contains-string"));
  u32 contains_struct_fn          = bc_program_find_fn(&prog, str8_lit("contains-struct"));

  Arena* heap = ctx_perm();
  Arena  data_arena = arena_create_vm(MB(1));
  i64    args[1] = {(i64)(intptr_t)&data_arena};

  expect_eq_i64("clear-nonempty()",         bc_run_in_program(&prog, clear_nonempty_fn,     args, 1, heap, &host_imports).value, 0);
  expect_eq_i64("clear-never-pushed()",     bc_run_in_program(&prog, clear_never_pushed_fn,  NULL, 0, heap, &host_imports).value, 0);
  expect_eq_i64("swap-remove-in-range()",   bc_run_in_program(&prog, swap_in_range_fn,       args, 1, heap, &host_imports).value, 1);
  expect_eq_i64("swap-remove-out-of-range()", bc_run_in_program(&prog, swap_oob_fn,          args, 1, heap, &host_imports).value, 1);
  expect_eq_i64("remove-at-in-range()",     bc_run_in_program(&prog, remove_in_range_fn,     args, 1, heap, &host_imports).value, 1);
  expect_eq_i64("remove-at-out-of-range()", bc_run_in_program(&prog, remove_oob_fn,          args, 1, heap, &host_imports).value, 1);
  expect_eq_i64("swap-remove-struct()",     bc_run_in_program(&prog, swap_struct_fn,         args, 1, heap, &host_imports).value, 1);
  expect_eq_i64("contains-scalar()",        bc_run_in_program(&prog, contains_scalar_fn,     args, 1, heap, &host_imports).value, 1);
  expect_eq_i64("contains-string()",        bc_run_in_program(&prog, contains_string_fn,     args, 1, heap, &host_imports).value, 1);
  expect_eq_i64("contains-struct()",        bc_run_in_program(&prog, contains_struct_fn,     args, 1, heap, &host_imports).value, 1);

  arena_destroy(&data_arena);
  arena_temp_end(&fn_temp);

  if (g_failures == 0) printf("bcgen_vector_remove_test: all checks passed\n");
  else                 printf("bcgen_vector_remove_test: %d check(s) FAILED\n", g_failures);

  ctx_free();
  return g_failures == 0 ? 0 : 1;
}
