// bcgen_handle_test.c -- validates handle pool support added to bcgen.c/
// bcvm.c: `(handle Name)`/`handle-pool-init`/`handle-alloc`/`handle-deref`/
// `handle-free`/`handle-valid?`. base.h's `HandlePool` struct is already
// fully generic/type-erased (confirmed by reading DEFINE_HANDLE_POOL
// directly) -- 5 new opcodes operate on a real `HandlePool*` for any
// pooled type, no per-type codegen needed. Same rig as the other
// bcgen_*_test.c files.
//
// Exercises:
//  - `handle-pool-init` + `handle-alloc` + `handle-deref`, mutating
//    through the deref'd pointer and reading it back through a SEPARATE
//    deref call.
//  - `handle-valid?` true for a freshly-allocated handle.
//  - `handle-free` + the generation-counter invalidation it's SUPPOSED to
//    cause: `handle-valid?` becomes false and `handle-deref` returns nil
//    for the SAME handle value afterward -- proves this is a REAL
//    index+generation check, not just "is this the zero handle", the
//    distinction base.h's DEFINE_HANDLE_POOL comment calls out.
//  - TWO separate handles from the SAME pool resolve to DIFFERENT,
//    independently-mutable slots.
//  - Pool exhaustion: allocating past `capacity` returns the nil handle
//    (index 0), not a crash or a corrupted slot.
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
  "(package bcgen_handle_test)\n"
  "(struct Mesh [vao i32 vbo i32])\n"
  "(handle Mesh)\n"
  "\n"
  "(fn alloc-mutate-read [] i32\n"
  "  (let [a arena (create)]\n"
  "    (handle-pool-init Mesh 8 a)\n"
  "    (let [h Mesh^ (handle-alloc Mesh)]\n"
  "      (set (. (deref (handle-deref h)) vao) 42)\n"
  "      (let [v i32 (. (deref (handle-deref h)) vao)]\n"
  "        (destroy a)\n"
  "        v))))\n"
  "\n"
  "(fn fresh-is-valid [] bool\n"
  "  (let [a arena (create)]\n"
  "    (handle-pool-init Mesh 8 a)\n"
  "    (let [h Mesh^ (handle-alloc Mesh)]\n"
  "      (let [v bool (handle-valid? h)]\n"
  "        (destroy a)\n"
  "        v))))\n"
  "\n"
  "(fn free-then-invalid [] bool\n"
  "  (let [a arena (create)]\n"
  "    (handle-pool-init Mesh 8 a)\n"
  "    (let [h Mesh^ (handle-alloc Mesh)]\n"
  "      (handle-free h)\n"
  "      (let [v bool (handle-valid? h)]\n"
  "        (destroy a)\n"
  "        v))))\n"
  "\n"
  "(fn free-then-deref-nil [] bool\n"
  "  (let [a arena (create)]\n"
  "    (handle-pool-init Mesh 8 a)\n"
  "    (let [h Mesh^ (handle-alloc Mesh)]\n"
  "      (handle-free h)\n"
  "      (let [v bool (= (handle-deref h) nil)]\n"
  "        (destroy a)\n"
  "        v))))\n"
  "\n"
  "(fn two-handles-independent [] i32\n"
  "  (let [a arena (create)]\n"
  "    (handle-pool-init Mesh 8 a)\n"
  "    (let [h1 Mesh^ (handle-alloc Mesh)]\n"
  "      (let [h2 Mesh^ (handle-alloc Mesh)]\n"
  "        (set (. (deref (handle-deref h1)) vao) 100)\n"
  "        (set (. (deref (handle-deref h2)) vao) 200)\n"
  "        (let [total i32 (+ (. (deref (handle-deref h1)) vao) (. (deref (handle-deref h2)) vao))]\n"
  "          (destroy a)\n"
  "          total)))))\n"
  "\n"
  "(fn exhaustion-returns-nil-handle [] bool\n"
  "  (let [a arena (create)]\n"
  "    (handle-pool-init Mesh 3 a) ; slot 0 is a reserved null sentinel -- only 2 REAL usable slots\n"
  "    (let [h1 Mesh^ (handle-alloc Mesh)]\n"
  "      (let [h2 Mesh^ (handle-alloc Mesh)]\n"
  "        (let [ok bool (and (handle-valid? h1) (handle-valid? h2))]\n"
  "          (let [h3 Mesh^ (handle-alloc Mesh)]\n"
  "            (let [v bool (and ok (not (handle-valid? h3)))]\n"
  "              (destroy a)\n"
  "              v)))))))\n"
  "\n"
  "(fn main [] i32 0)\n";

static Checker
check_fixture(TypedAst* tast, TypedIndex* out_root) {
  Ast ast;
  ast_init(&ast, ctx_perm());

  String8 src     = str8_cstring((char*)(g_fixture_source));
  u32     file_id = source_file_register(str8_lit("bcgen_handle_test_fixture.3b"), src);

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

  u32 alloc_mutate_read_fn      = bc_program_find_fn(&prog, str8_lit("alloc-mutate-read"));
  u32 fresh_is_valid_fn          = bc_program_find_fn(&prog, str8_lit("fresh-is-valid"));
  u32 free_then_invalid_fn       = bc_program_find_fn(&prog, str8_lit("free-then-invalid"));
  u32 free_then_deref_nil_fn     = bc_program_find_fn(&prog, str8_lit("free-then-deref-nil"));
  u32 two_handles_independent_fn = bc_program_find_fn(&prog, str8_lit("two-handles-independent"));
  u32 exhaustion_fn                = bc_program_find_fn(&prog, str8_lit("exhaustion-returns-nil-handle"));

  Arena* heap = ctx_perm();

  expect_eq_i64("alloc-mutate-read()",      bc_run_in_program(&prog, alloc_mutate_read_fn,      NULL, 0, heap, &host_imports).value, 42);
  expect_eq_i64("fresh-is-valid()",          bc_run_in_program(&prog, fresh_is_valid_fn,          NULL, 0, heap, &host_imports).value, 1);
  expect_eq_i64("free-then-invalid()",       bc_run_in_program(&prog, free_then_invalid_fn,       NULL, 0, heap, &host_imports).value, 0);
  expect_eq_i64("free-then-deref-nil()",     bc_run_in_program(&prog, free_then_deref_nil_fn,     NULL, 0, heap, &host_imports).value, 1);
  expect_eq_i64("two-handles-independent()", bc_run_in_program(&prog, two_handles_independent_fn, NULL, 0, heap, &host_imports).value, 300);
  expect_eq_i64("exhaustion-returns-nil-handle()", bc_run_in_program(&prog, exhaustion_fn, NULL, 0, heap, &host_imports).value, 1);

  arena_temp_end(&fn_temp);

  if (g_failures == 0) printf("bcgen_handle_test: all checks passed\n");
  else                 printf("bcgen_handle_test: %d check(s) FAILED\n", g_failures);

  ctx_free();
  return g_failures == 0 ? 0 : 1;
}
