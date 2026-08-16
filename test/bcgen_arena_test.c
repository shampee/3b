// bcgen_arena_test.c -- validates Arena support added to bcgen.c/bcvm.c:
// `create`/`destroy`/`reset`/`release`/`mark`/`pop` (Call-name-special-
// cased builtins, not distinct TypedNodeKinds -- checker.c/codegen.c treat
// them the same way), `push`/`push0`/`push`-with-value (TypedNodeKind_
// PushAlloc/PushCopy), `scratch` (TypedNodeKind_ScratchExpr), and
// `alloc`/`free` (TypedNodeKind_AllocExpr, malloc-backed, not arena-
// backed). Same rig as the other bcgen_*_test.c files.
//
// Exercises:
//  - `create`/`push`/`destroy` round trip: an i32 pushed into a real
//    VM-backed arena, read back correctly, arena destroyed cleanly.
//  - `push` with an explicit COUNT (`(push a i32 3)`), including writing
//    to and reading back the LAST element -- proves the byte-size
//    computation (stride * runtime count) is correct, not just "some
//    allocation happened."
//  - `push0` (zeroed) actually zeroes.
//  - `push` with a VALUE (PushCopy) -- a struct copied in, read back
//    through ordinary field access.
//  - `mark`/`pop` -- an allocation made AFTER a mark, rewound by `pop`,
//    with a SECOND push after the rewind landing at the SAME address the
//    first one did (proves the cursor really rewound, not just "didn't
//    crash").
//  - `reset` -- same idea as mark/pop but rewinding the WHOLE arena back
//    to empty.
//  - `scratch` -- a real allocation inside the block, used inside the
//    block (the one place its value is guaranteed still valid).
//  - `alloc`/`free` (malloc-backed) -- independent of the arena family
//    entirely.
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
  "(package bcgen_arena_test)\n"
  "(struct Vec2 [x f32 y f32])\n"
  "\n"
  "(fn create-push-destroy [] i32\n"
  "  (let [a arena (create)]\n"
  "    (let [p i32* (push a i32)]\n"
  "      (set (deref p) 42)\n"
  "      (let [v i32 (deref p)]\n"
  "        (destroy a)\n"
  "        v))))\n"
  "\n"
  "(fn push-count [] i32\n"
  "  (let [a arena (create)]\n"
  "    (let [p i32* (push a i32 3)]\n"
  "      (set (nth p 0) 10)\n"
  "      (set (nth p 1) 20)\n"
  "      (set (nth p 2) 30)\n"
  "      (let [total i32 (+ (nth p 0) (+ (nth p 1) (nth p 2)))]\n"
  "        (destroy a)\n"
  "        total))))\n"
  "\n"
  "(fn push-zero [] i32\n"
  "  (let [a arena (create)]\n"
  "    (let [p i32* (push a i32)]\n"
  "      (set (deref p) 123))\n"
  "    (destroy a))\n"
  "  (let [a2 arena (create)]\n"
  "    (let [p2 i32* (push0 a2 i32)]\n"
  "      (let [v i32 (deref p2)]\n"
  "        (destroy a2)\n"
  "        v))))\n"
  "\n"
  "(fn push-value [] f32\n"
  "  (let [a arena (create)]\n"
  "    (let [v Vec2 (Vec2 {:x 3.5f32 :y 7.5f32})]\n"
  "      (let [pv Vec2* (push a v)]\n"
  "        (let [y f32 (. (deref pv) y)]\n"
  "          (destroy a)\n"
  "          y)))))\n"
  "\n"
  "(fn mark-pop-same-address [] bool\n"
  "  (let [a arena (create)]\n"
  "    (let [p1 i32* (push a i32)]\n"
  "      (let [m ArenaMark (mark a)]\n"
  "        (let [p2 i32* (push a i32)]\n"
  "          (pop a m)\n"
  "          (let [p3 i32* (push a i32)]\n"
  "            (let [same bool (= (cast i64 p3) (cast i64 p2))]\n"
  "              (destroy a)\n"
  "              same)))))))\n"
  "\n"
  "(fn reset-rewinds [] bool\n"
  "  (let [a arena (create)]\n"
  "    (let [p1 i32* (push a i32)]\n"
  "      (reset a)\n"
  "      (let [p2 i32* (push a i32)]\n"
  "        (let [same bool (= (cast i64 p1) (cast i64 p2))]\n"
  "          (destroy a)\n"
  "          same)))))\n"
  "\n"
  "(fn scratch-basic [] i32\n"
  "  (scratch [t]\n"
  "    (let [p i32* (push t i32)]\n"
  "      (set (deref p) 77)\n"
  "      (deref p))))\n"
  "\n"
  "(fn alloc-free [] i32\n"
  "  (let [p i32* (alloc i32)]\n"
  "    (set (deref p) 55)\n"
  "    (let [v i32 (deref p)]\n"
  "      (free p)\n"
  "      v)))\n"
  "\n"
  "(fn main [] i32 0)\n";

static Checker
check_fixture(TypedAst* tast, TypedIndex* out_root) {
  Ast ast;
  ast_init(&ast, ctx_perm());

  String8 src     = str8_cstring((char*)(g_fixture_source));
  u32     file_id = source_file_register(str8_lit("bcgen_arena_test_fixture.3b"), src);

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

  u32 create_push_destroy_fn = bc_program_find_fn(&prog, str8_lit("create-push-destroy"));
  u32 push_count_fn            = bc_program_find_fn(&prog, str8_lit("push-count"));
  u32 push_zero_fn              = bc_program_find_fn(&prog, str8_lit("push-zero"));
  u32 push_value_fn             = bc_program_find_fn(&prog, str8_lit("push-value"));
  u32 mark_pop_same_address_fn = bc_program_find_fn(&prog, str8_lit("mark-pop-same-address"));
  u32 reset_rewinds_fn          = bc_program_find_fn(&prog, str8_lit("reset-rewinds"));
  u32 scratch_basic_fn          = bc_program_find_fn(&prog, str8_lit("scratch-basic"));
  u32 alloc_free_fn             = bc_program_find_fn(&prog, str8_lit("alloc-free"));

  Arena* heap = ctx_perm();

  expect_eq_i64("create-push-destroy()", bc_run_in_program(&prog, create_push_destroy_fn, NULL, 0, heap, &host_imports).value, 42);
  expect_eq_i64("push-count()",           bc_run_in_program(&prog, push_count_fn,           NULL, 0, heap, &host_imports).value, 60);
  expect_eq_i64("push-zero()",             bc_run_in_program(&prog, push_zero_fn,             NULL, 0, heap, &host_imports).value, 0);
  { BcResult r = bc_run_in_program(&prog, push_value_fn, NULL, 0, heap, &host_imports);
    expect_eq_f32("push-value()", r.value, 7.5f);
  }
  expect_eq_i64("mark-pop-same-address()", bc_run_in_program(&prog, mark_pop_same_address_fn, NULL, 0, heap, &host_imports).value, 1);
  expect_eq_i64("reset-rewinds()",          bc_run_in_program(&prog, reset_rewinds_fn,          NULL, 0, heap, &host_imports).value, 1);
  expect_eq_i64("scratch-basic()",          bc_run_in_program(&prog, scratch_basic_fn,          NULL, 0, heap, &host_imports).value, 77);
  expect_eq_i64("alloc-free()",             bc_run_in_program(&prog, alloc_free_fn,             NULL, 0, heap, &host_imports).value, 55);

  arena_temp_end(&fn_temp);

  if (g_failures == 0) printf("bcgen_arena_test: all checks passed\n");
  else                 printf("bcgen_arena_test: %d check(s) FAILED\n", g_failures);

  ctx_free();
  return g_failures == 0 ? 0 : 1;
}
