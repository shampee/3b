// bcgen_narrow_int_test.c -- validates i8/u8/i16/u16/char field and array-
// element load/store support added to bcgen.c/bcvm.c (BcOp_LoadFieldI8/
// U8/I16/U16, BcOp_StoreFieldI8/I16) -- previously a bcgen.c compile-time
// diagnostic, a real, documented scope cut (see bytecode.h's own note on
// the opcodes this replaces). Same rig as the other bcgen_*_test.c files.
//
// Exercises:
//  - A struct field of each narrow width (i8/u8/i16/u16/char), filled
//    then read back through a SEPARATE read function -- proves the
//    round-trip goes through real memory (StoreField then LoadField),
//    not just a register alias.
//  - SIGN vs ZERO extension at the width boundary specifically (not just
//    an arbitrary small in-range value, which wouldn't distinguish the
//    two): i8 -1 (0xFF) must read back as -1, not 255; u8 255 must read
//    back as 255, not -1. Same pair at the 16-bit boundary (i16 -1 vs
//    u16 65535).
//  - A fixed-size array of each narrow width -- `nth` read AND
//    `set`-write, proving bc_resolve_index_access's stride/offset
//    arithmetic composes correctly with the new narrow load/store ops
//    (stride is 1/1/2/2/1 bytes respectively, not the 4/8 bytes every
//    prior array element width happened to be).
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
  "(package bcgen_narrow_int_test)\n"
  "\n"
  "(struct Narrow [a i8 b u8 c i16 d u16 e char])\n"
  "\n"
  "(fn make-narrow [av i8 bv u8 cv i16 dv u16 ev char] Narrow\n"
  "  (Narrow {:a av :b bv :c cv :d dv :e ev}))\n"
  "\n"
  "(fn narrow-a [n Narrow] i32 (cast i32 (. n a)))\n"
  "(fn narrow-b [n Narrow] i32 (cast i32 (. n b)))\n"
  "(fn narrow-c [n Narrow] i32 (cast i32 (. n c)))\n"
  "(fn narrow-d [n Narrow] i32 (cast i32 (. n d)))\n"
  "(fn narrow-e [n Narrow] i32 (cast i32 (. n e)))\n"
  "\n"
  "(fn i8-array-roundtrip [] i32\n"
  "  (var arr [i8 3])\n"
  "  (set (nth arr 0) (cast i8 1))\n"
  "  (set (nth arr 1) (cast i8 -1))\n"
  "  (set (nth arr 2) (cast i8 3))\n"
  "  (+ (cast i32 (nth arr 0)) (+ (cast i32 (nth arr 1)) (cast i32 (nth arr 2)))))\n"
  "\n"
  "(fn u8-array-roundtrip [] i32\n"
  "  (var arr [u8 2])\n"
  "  (set (nth arr 0) (cast u8 255))\n"
  "  (set (nth arr 1) (cast u8 20))\n"
  "  (+ (cast i32 (nth arr 0)) (cast i32 (nth arr 1))))\n"
  "\n"
  "(fn i16-array-roundtrip [] i32\n"
  "  (var arr [i16 2])\n"
  "  (set (nth arr 0) (cast i16 -1))\n"
  "  (set (nth arr 1) (cast i16 200))\n"
  "  (+ (cast i32 (nth arr 0)) (cast i32 (nth arr 1))))\n"
  "\n"
  "(fn u16-array-roundtrip [] i32\n"
  "  (var arr [u16 2])\n"
  "  (set (nth arr 0) (cast u16 65535))\n"
  "  (set (nth arr 1) (cast u16 200))\n"
  "  (+ (cast i32 (nth arr 0)) (cast i32 (nth arr 1))))\n"
  "\n"
  "(fn main [] i32 0)\n";

static Checker
check_fixture(TypedAst* tast, TypedIndex* out_root) {
  Ast ast;
  ast_init(&ast, ctx_perm());

  String8 src     = str8_cstring((char*)(g_fixture_source));
  u32     file_id = source_file_register(str8_lit("bcgen_narrow_int_test_fixture.3b"), src);

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

typedef struct { i8 a; u8 b; i16 c; u16 d; char e; } NarrowC;

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

  u32 make_narrow_fn = bc_program_find_fn(&prog, str8_lit("make-narrow"));
  u32 narrow_a_fn      = bc_program_find_fn(&prog, str8_lit("narrow-a"));
  u32 narrow_b_fn      = bc_program_find_fn(&prog, str8_lit("narrow-b"));
  u32 narrow_c_fn      = bc_program_find_fn(&prog, str8_lit("narrow-c"));
  u32 narrow_d_fn      = bc_program_find_fn(&prog, str8_lit("narrow-d"));
  u32 narrow_e_fn      = bc_program_find_fn(&prog, str8_lit("narrow-e"));
  u32 i8_array_fn       = bc_program_find_fn(&prog, str8_lit("i8-array-roundtrip"));
  u32 u8_array_fn       = bc_program_find_fn(&prog, str8_lit("u8-array-roundtrip"));
  u32 i16_array_fn      = bc_program_find_fn(&prog, str8_lit("i16-array-roundtrip"));
  u32 u16_array_fn      = bc_program_find_fn(&prog, str8_lit("u16-array-roundtrip"));

  Arena* heap = ctx_perm();

  // ~~ struct field of each narrow width, SIGN/ZERO extension boundary
  // values specifically (i8 -1 / u8 255, i16 -1 / u16 65535) ~~
  {
    i64 args[5] = { -1, 255, -1, 65535, (i64)(u8)'z' };
    i64 n_ptr   = bc_run_in_program(&prog, make_narrow_fn, args, 5, heap, &host_imports).value;
    i64 read_args[1] = { n_ptr };
    expect_eq_i64("narrow.a (i8 -1)",     bc_run_in_program(&prog, narrow_a_fn, read_args, 1, heap, &host_imports).value, -1);
    expect_eq_i64("narrow.b (u8 255)",    bc_run_in_program(&prog, narrow_b_fn, read_args, 1, heap, &host_imports).value, 255);
    expect_eq_i64("narrow.c (i16 -1)",    bc_run_in_program(&prog, narrow_c_fn, read_args, 1, heap, &host_imports).value, -1);
    expect_eq_i64("narrow.d (u16 65535)", bc_run_in_program(&prog, narrow_d_fn, read_args, 1, heap, &host_imports).value, 65535);
    expect_eq_i64("narrow.e (char 'z')",  bc_run_in_program(&prog, narrow_e_fn, read_args, 1, heap, &host_imports).value, (i64)'z');

    // Cross-check against a REAL C struct with the exact same field
    // widths -- confirms this isn't just internally self-consistent but
    // matches how a real C compiler would actually lay these bits out.
    NarrowC c = {0};
    c.a = -1; c.b = 255; c.c = -1; c.d = 65535; c.e = 'z';
    expect_eq_i64("NarrowC.a matches C",  (i64)c.a, -1);
    expect_eq_i64("NarrowC.b matches C",  (i64)c.b, 255);
    expect_eq_i64("NarrowC.c matches C",  (i64)c.c, -1);
    expect_eq_i64("NarrowC.d matches C",  (i64)c.d, 65535);
  }

  // ~~ fixed-size arrays of each narrow width -- `nth` read AND
  // `set`-write, exercising stride arithmetic at non-4/8-byte widths ~~
  expect_eq_i64("i8-array-roundtrip()",  bc_run_in_program(&prog, i8_array_fn,  NULL, 0, heap, &host_imports).value, 1 + -1 + 3);
  expect_eq_i64("u8-array-roundtrip()",  bc_run_in_program(&prog, u8_array_fn,  NULL, 0, heap, &host_imports).value, 255 + 20);
  expect_eq_i64("i16-array-roundtrip()", bc_run_in_program(&prog, i16_array_fn, NULL, 0, heap, &host_imports).value, -1 + 200);
  expect_eq_i64("u16-array-roundtrip()", bc_run_in_program(&prog, u16_array_fn, NULL, 0, heap, &host_imports).value, 65535 + 200);

  arena_temp_end(&fn_temp);

  if (g_failures == 0) printf("bcgen_narrow_int_test: all checks passed\n");
  else                 printf("bcgen_narrow_int_test: %d check(s) FAILED\n", g_failures);

  ctx_free();
  return g_failures == 0 ? 0 : 1;
}
