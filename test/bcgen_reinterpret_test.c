// bcgen_reinterpret_test.c -- validates `(reinterpret Type value)`
// (TypedNodeKind_BinaryReinterpret) in bcgen.c/bcvm.c. Same rig as the
// other bcgen_*_test.c files (see bcgen_cast_test.c, which this mirrors).
//
// Exercises:
//  - f32<->i32 and f64<->i64 real bit-pattern round trips against known
//    IEEE-754 constants -- proves this does NOT go through BcOp_IntToF64/
//    F32ToInt etc (that would give the wrong numbers entirely; a real
//    numeric conversion of 1.0f32's bit pattern 1065353216 to f64 is
//    1065353216.0, not 1.0).
//  - i32->u32 and i16->u16 reinterpret of a NEGATIVE value -- proves
//    bc_compile_int_narrow's sign/zero-extension normalization still runs
//    (the raw register bit pattern is unchanged either way; only the
//    UPPER, unread-by-this-width bits differ, and only matters once the
//    result is used by an operation that reads the full register).
//  - Same-type reinterpret is a true no-op.
//  - pointer<->any round trip through a REAL C-owned address, proving the
//    "just pass the bits through" path doesn't corrupt a genuine pointer
//    (same proof bcgen_cast_test.c's own ptr-round-trip gives for `cast`).
#include "3b.h"
#include "bcgen.h"
#include "bcvm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static int g_failures = 0;

static void
expect_eq_i64(const char* what, i64 got, i64 want) {
  if (got != want) {
    fprintf(stderr, "FAIL %s: got %lld, want %lld\n", what, (long long)got, (long long)want);
    g_failures += 1;
  }
}

static void
expect_eq_f64(const char* what, i64 got_bits, f64 want) {
  f64 got; memcpy(&got, &got_bits, sizeof(got));
  if (got != want) {
    fprintf(stderr, "FAIL %s: got %g, want %g\n", what, got, want);
    g_failures += 1;
  }
}

static i64
f64_bits(f64 v) { i64 b; memcpy(&b, &v, sizeof(b)); return b; }

static i64
f32_bits(f32 v) { u32 b; memcpy(&b, &v, sizeof(b)); return (i64)(u64)b; }

static const char* g_fixture_source =
  "(package bcgen_reinterpret_test)\n"
  "\n"
  "(fn f32-bits [x f32] i32 (reinterpret i32 x))\n"
  "(fn bits-f32 [x i32] f32 (reinterpret f32 x))\n"
  "(fn f64-bits [x f64] i64 (reinterpret i64 x))\n"
  "(fn bits-f64 [x i64] f64 (reinterpret f64 x))\n"
  "\n"
  "(fn i32-as-u32 [x i32] u32 (reinterpret u32 x))\n"
  "(fn i16-as-u16 [x i16] u16 (reinterpret u16 x))\n"
  "\n"
  "(fn same-type-noop [x i32] i32 (reinterpret i32 x))\n"
  "\n"
  "(fn ptr-round-trip [p i32*] i32*\n"
  "  (let [a any (reinterpret any p)]\n"
  "    (reinterpret i32* a)))\n"
  "\n"
  "(fn main [] i32 0)\n";

static Checker
check_fixture(TypedAst* tast, TypedIndex* out_root) {
  Ast ast;
  ast_init(&ast, ctx_perm());

  String8 src     = str8_cstring((char*)(g_fixture_source));
  u32     file_id = source_file_register(str8_lit("bcgen_reinterpret_test_fixture.3b"), src);

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
  return check_program(tast, own_root, /*is_root_package=*/true, /*scope_query=*/NULL);
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

  u32 f32_bits_fn        = bc_program_find_fn(&prog, str8_lit("f32-bits"));
  u32 bits_f32_fn        = bc_program_find_fn(&prog, str8_lit("bits-f32"));
  u32 f64_bits_fn        = bc_program_find_fn(&prog, str8_lit("f64-bits"));
  u32 bits_f64_fn        = bc_program_find_fn(&prog, str8_lit("bits-f64"));
  u32 i32_as_u32_fn      = bc_program_find_fn(&prog, str8_lit("i32-as-u32"));
  u32 i16_as_u16_fn      = bc_program_find_fn(&prog, str8_lit("i16-as-u16"));
  u32 same_type_noop_fn  = bc_program_find_fn(&prog, str8_lit("same-type-noop"));
  u32 ptr_round_trip_fn  = bc_program_find_fn(&prog, str8_lit("ptr-round-trip"));

  Arena* heap = ctx_perm();

  // ~~ f32<->i32 real bit-pattern round trip (NOT a numeric conversion).
  { i64 args[1] = {f32_bits(1.0f)};
    expect_eq_i64("f32-bits(1.0f)", bc_run_in_program(&prog, f32_bits_fn, args, 1, heap, &host_imports).value, 1065353216);
  }
  { i64 args[1] = {1065353216};
    f32 got; u32 bits32 = (u32)bc_run_in_program(&prog, bits_f32_fn, args, 1, heap, &host_imports).value;
    memcpy(&got, &bits32, sizeof(got));
    if (got != 1.0f) { fprintf(stderr, "FAIL bits-f32(1065353216): got %g, want 1.0\n", (double)got); g_failures += 1; }
  }

  // ~~ f64<->i64 real bit-pattern round trip.
  { i64 args[1] = {f64_bits(1.0)};
    expect_eq_i64("f64-bits(1.0)", bc_run_in_program(&prog, f64_bits_fn, args, 1, heap, &host_imports).value, 4607182418800017408LL);
  }
  { i64 args[1] = {4607182418800017408LL};
    expect_eq_f64("bits-f64(<1.0's bits>)", bc_run_in_program(&prog, bits_f64_fn, args, 1, heap, &host_imports).value, 1.0);
  }

  // ~~ int<->int reinterpret of a NEGATIVE value -- same bit pattern,
  // different upper-bits normalization once read back as the new type.
  { i64 args[1] = {-1}; // i32(-1) == 0xFFFFFFFF
    expect_eq_i64("i32-as-u32(-1)", bc_run_in_program(&prog, i32_as_u32_fn, args, 1, heap, &host_imports).value, 4294967295LL);
  }
  { i64 args[1] = {-1}; // i16(-1) == 0xFFFF
    expect_eq_i64("i16-as-u16(-1)", bc_run_in_program(&prog, i16_as_u16_fn, args, 1, heap, &host_imports).value, 65535);
  }

  // ~~ Same-type reinterpret is a true no-op.
  { i64 args[1] = {123}; expect_eq_i64("same-type-noop(123)", bc_run_in_program(&prog, same_type_noop_fn, args, 1, heap, &host_imports).value, 123); }

  // ~~ pointer<->any round trip through a REAL C-owned address.
  { i32 real_value = 999;
    i64 args[1] = {(i64)(intptr_t)&real_value};
    BcResult r = bc_run_in_program(&prog, ptr_round_trip_fn, args, 1, heap, &host_imports);
    i32* round_tripped = (i32*)(intptr_t)r.value;
    if (round_tripped != &real_value || *round_tripped != 999) {
      fprintf(stderr, "FAIL ptr-round-trip: address corrupted (got %p, want %p)\n", (void*)round_tripped, (void*)&real_value);
      g_failures += 1;
    }
  }

  arena_temp_end(&fn_temp);

  if (g_failures == 0) printf("bcgen_reinterpret_test: all checks passed\n");
  else                 printf("bcgen_reinterpret_test: %d check(s) FAILED\n", g_failures);

  ctx_free();
  return g_failures == 0 ? 0 : 1;
}
