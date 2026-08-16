// bcgen_cast_test.c -- validates `(cast Type value)` (TypedNodeKind_
// BinaryCast) and the 5 bitwise binops + `bit-not` added to bcgen.c/
// bcvm.c in the same pass. Same rig as the other bcgen_*_test.c files.
//
// Exercises:
//  - Bitwise ops (`bit-and`/`bit-or`/`bit-xor`/`bit-shl`/`bit-shr`/
//    `bit-not`) via plain integer truth-table-style cases.
//  - int -> int width adjustment: SIGNED narrowing (i64 -> i32, a value
//    whose low 32 bits look negative once isolated) and UNSIGNED
//    narrowing (i64 -> u8, masked not sign-extended) -- proves
//    bc_compile_int_narrow picks the right extension per target kind, not
//    just "does something."
//  - `char` narrowing specifically, against `u8` on the same input: plain
//    C `char` is SIGNED on this target, so codegen.c's `(char)` cast
//    sign-extends and this backend has to match. The two kinds share a
//    width and differ only in extension, so they pin the one bit that
//    once diverged (`(cast char -1)` read back as 255 here, -1 natively).
//  - int <-> float: real numeric conversion both ways, including negative
//    values and float-to-int TRUNCATION (toward zero, not rounding).
//  - float <-> float (f32<->f64) precision conversion.
//  - `(cast bool x)` C-style truthiness (not a bit copy) from both an int
//    and a float operand.
//  - `(cast void x)` -- the discard idiom -- still runs `x`'s side effects.
//  - Same-type cast is a true no-op (returns the identical value).
//  - pointer<->any round trip through a REAL C-owned address, proving the
//    "just pass the bits through" path doesn't corrupt a genuine pointer.
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

static void
expect_eq_f32(const char* what, i64 got_bits, f32 want) {
  f32 got; u32 bits32 = (u32)got_bits; memcpy(&got, &bits32, sizeof(got));
  if (got != want) {
    fprintf(stderr, "FAIL %s: got %g, want %g\n", what, (double)got, (double)want);
    g_failures += 1;
  }
}

static i64
f64_bits(f64 v) { i64 b; memcpy(&b, &v, sizeof(b)); return b; }

static i64
f32_bits(f32 v) { u32 b; memcpy(&b, &v, sizeof(b)); return (i64)(u64)b; }

static const char* g_fixture_source =
  "(package bcgen_cast_test)\n"
  "\n"
  "(fn bitand [a i32 b i32] i32 (bit-and a b))\n"
  "(fn bitor  [a i32 b i32] i32 (bit-or a b))\n"
  "(fn bitxor [a i32 b i32] i32 (bit-xor a b))\n"
  "(fn bitshl [a i32 b i32] i32 (bit-shl a b))\n"
  "(fn bitshr [a i32 b i32] i32 (bit-shr a b))\n"
  "(fn bitnot [a i32] i32 (bit-not a))\n"
  "\n"
  "(fn i64-to-i32 [x i64] i32 (cast i32 x))\n"
  "(fn i64-to-u8  [x i64] u8  (cast u8 x))\n"
  "(fn i32-to-i64 [x i32] i64 (cast i64 x))\n"
  "(fn i32-to-char [x i32] char (cast char x))\n"
  "(fn char-to-i32 [x char] i32 (cast i32 x))\n"
  "(fn char-round-trip [x i32] i32 (cast i32 (cast char x)))\n"
  "\n"
  "(fn i-to-f64 [x i32] f64 (cast f64 x))\n"
  "(fn i-to-f32 [x i32] f32 (cast f32 x))\n"
  "(fn f64-to-i [x f64] i32 (cast i32 x))\n"
  "(fn f32-to-i [x f32] i32 (cast i32 x))\n"
  "(fn f32-to-f64 [x f32] f64 (cast f64 x))\n"
  "(fn f64-to-f32 [x f64] f32 (cast f32 x))\n"
  "\n"
  "(fn int-to-bool [x i32] bool (cast bool x))\n"
  "(fn float-to-bool [x f64] bool (cast bool x))\n"
  "\n"
  "(var side_effect_ran bool false)\n"
  "(fn bump-side-effect [] i32 (set side_effect_ran true) 7)\n"
  "(fn void-discard [] void (cast void (bump-side-effect)))\n"
  "(fn read-side-effect [] bool side_effect_ran)\n"
  "\n"
  "(fn same-type-noop [x i32] i32 (cast i32 x))\n"
  "\n"
  "(fn ptr-round-trip [p i32*] i32*\n"
  "  (let [a any (cast any p)]\n"
  "    (cast i32* a)))\n"
  "\n"
  "(fn main [] i32 0)\n";

static Checker
check_fixture(TypedAst* tast, TypedIndex* out_root) {
  Ast ast;
  ast_init(&ast, ctx_perm());

  String8 src     = str8_cstring((char*)(g_fixture_source));
  u32     file_id = source_file_register(str8_lit("bcgen_cast_test_fixture.3b"), src);

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

  u32 bitand_fn         = bc_program_find_fn(&prog, str8_lit("bitand"));
  u32 bitor_fn           = bc_program_find_fn(&prog, str8_lit("bitor"));
  u32 bitxor_fn          = bc_program_find_fn(&prog, str8_lit("bitxor"));
  u32 bitshl_fn          = bc_program_find_fn(&prog, str8_lit("bitshl"));
  u32 bitshr_fn          = bc_program_find_fn(&prog, str8_lit("bitshr"));
  u32 bitnot_fn          = bc_program_find_fn(&prog, str8_lit("bitnot"));
  u32 i64_to_i32_fn      = bc_program_find_fn(&prog, str8_lit("i64-to-i32"));
  u32 i64_to_u8_fn       = bc_program_find_fn(&prog, str8_lit("i64-to-u8"));
  u32 i32_to_i64_fn      = bc_program_find_fn(&prog, str8_lit("i32-to-i64"));
  u32 i32_to_char_fn     = bc_program_find_fn(&prog, str8_lit("i32-to-char"));
  u32 char_to_i32_fn     = bc_program_find_fn(&prog, str8_lit("char-to-i32"));
  u32 char_round_trip_fn = bc_program_find_fn(&prog, str8_lit("char-round-trip"));
  u32 i_to_f64_fn        = bc_program_find_fn(&prog, str8_lit("i-to-f64"));
  u32 i_to_f32_fn        = bc_program_find_fn(&prog, str8_lit("i-to-f32"));
  u32 f64_to_i_fn        = bc_program_find_fn(&prog, str8_lit("f64-to-i"));
  u32 f32_to_i_fn        = bc_program_find_fn(&prog, str8_lit("f32-to-i"));
  u32 f32_to_f64_fn      = bc_program_find_fn(&prog, str8_lit("f32-to-f64"));
  u32 f64_to_f32_fn      = bc_program_find_fn(&prog, str8_lit("f64-to-f32"));
  u32 int_to_bool_fn     = bc_program_find_fn(&prog, str8_lit("int-to-bool"));
  u32 float_to_bool_fn   = bc_program_find_fn(&prog, str8_lit("float-to-bool"));
  u32 void_discard_fn    = bc_program_find_fn(&prog, str8_lit("void-discard"));
  u32 read_side_effect_fn = bc_program_find_fn(&prog, str8_lit("read-side-effect"));
  u32 same_type_noop_fn  = bc_program_find_fn(&prog, str8_lit("same-type-noop"));
  u32 ptr_round_trip_fn  = bc_program_find_fn(&prog, str8_lit("ptr-round-trip"));

  Arena* heap = ctx_perm();

  // ~~ Bitwise ops.
  { i64 args[2] = {12, 10}; expect_eq_i64("bit-and(12,10)", bc_run_in_program(&prog, bitand_fn, args, 2, heap, &host_imports).value, 8); }
  { i64 args[2] = {12, 10}; expect_eq_i64("bit-or(12,10)",  bc_run_in_program(&prog, bitor_fn,  args, 2, heap, &host_imports).value, 14); }
  { i64 args[2] = {12, 10}; expect_eq_i64("bit-xor(12,10)", bc_run_in_program(&prog, bitxor_fn, args, 2, heap, &host_imports).value, 6); }
  { i64 args[2] = {1, 4};   expect_eq_i64("bit-shl(1,4)",   bc_run_in_program(&prog, bitshl_fn, args, 2, heap, &host_imports).value, 16); }
  { i64 args[2] = {16, 4};  expect_eq_i64("bit-shr(16,4)",  bc_run_in_program(&prog, bitshr_fn, args, 2, heap, &host_imports).value, 1); }
  { i64 args[1] = {0};      expect_eq_i64("bit-not(0)",     bc_run_in_program(&prog, bitnot_fn, args, 1, heap, &host_imports).value, -1); }

  // ~~ int width adjustment -- signed vs unsigned narrowing must differ.
  { i64 args[1] = {0xFFFFFFFF}; // low 32 bits all-ones
    expect_eq_i64("i64->i32(0xFFFFFFFF) sign-extends to -1", bc_run_in_program(&prog, i64_to_i32_fn, args, 1, heap, &host_imports).value, -1);
  }
  { i64 args[1] = {0x1FF}; // low 8 bits = 0xFF
    expect_eq_i64("i64->u8(0x1FF) zero-extends to 255 (not sign-extended)", bc_run_in_program(&prog, i64_to_u8_fn, args, 1, heap, &host_imports).value, 255);
  }
  { i64 args[1] = {-1}; expect_eq_i64("i32->i64(-1) stays -1 (already full width)", bc_run_in_program(&prog, i32_to_i64_fn, args, 1, heap, &host_imports).value, -1); }

  // ~~ `char` is SIGNED, matching codegen.c's plain C `(char)` cast. The
  // 0x1FF case below is deliberately the same input as the i64->u8 case
  // above: same width, opposite extension, so the two can't both pass
  // unless the per-kind choice is actually right.
  { i64 args[1] = {-1};
    expect_eq_i64("i32->char(-1) sign-extends to -1 (char is signed, unlike u8)", bc_run_in_program(&prog, i32_to_char_fn, args, 1, heap, &host_imports).value, -1);
  }
  { i64 args[1] = {0x1FF}; // same input as the i64->u8 case, which gives 255
    expect_eq_i64("i32->char(0x1FF) sign-extends to -1", bc_run_in_program(&prog, i32_to_char_fn, args, 1, heap, &host_imports).value, -1);
  }
  { i64 args[1] = {65}; expect_eq_i64("i32->char(65) positive is unaffected", bc_run_in_program(&prog, i32_to_char_fn, args, 1, heap, &host_imports).value, 65); }
  { i64 args[1] = {-1}; expect_eq_i64("char->i32(-1) widens back to -1", bc_run_in_program(&prog, char_to_i32_fn, args, 1, heap, &host_imports).value, -1); }
  { i64 args[1] = {-1}; expect_eq_i64("i32 -> char -> i32 round trip of -1", bc_run_in_program(&prog, char_round_trip_fn, args, 1, heap, &host_imports).value, -1); }
  { i64 args[1] = {200}; // 200 doesn't fit a signed char -- wraps to -56, as C does
    expect_eq_i64("i32 -> char -> i32 round trip of 200 wraps to -56", bc_run_in_program(&prog, char_round_trip_fn, args, 1, heap, &host_imports).value, -56);
  }

  // ~~ int <-> float real conversion.
  { i64 args[1] = {-42}; expect_eq_f64("i-to-f64(-42)", bc_run_in_program(&prog, i_to_f64_fn, args, 1, heap, &host_imports).value, -42.0); }
  { i64 args[1] = {-42}; expect_eq_f32("i-to-f32(-42)", bc_run_in_program(&prog, i_to_f32_fn, args, 1, heap, &host_imports).value, -42.0f); }
  { i64 args[1] = {f64_bits(3.9)};  expect_eq_i64("f64-to-i(3.9) truncates toward zero", bc_run_in_program(&prog, f64_to_i_fn, args, 1, heap, &host_imports).value, 3); }
  { i64 args[1] = {f64_bits(-3.9)}; expect_eq_i64("f64-to-i(-3.9) truncates toward zero", bc_run_in_program(&prog, f64_to_i_fn, args, 1, heap, &host_imports).value, -3); }
  { i64 args[1] = {f32_bits(3.9f)}; expect_eq_i64("f32-to-i(3.9)", bc_run_in_program(&prog, f32_to_i_fn, args, 1, heap, &host_imports).value, 3); }

  // ~~ float <-> float precision conversion.
  { i64 args[1] = {f32_bits(3.5f)}; expect_eq_f64("f32-to-f64(3.5)", bc_run_in_program(&prog, f32_to_f64_fn, args, 1, heap, &host_imports).value, 3.5); }
  { i64 args[1] = {f64_bits(3.5)};  expect_eq_f32("f64-to-f32(3.5)", bc_run_in_program(&prog, f64_to_f32_fn, args, 1, heap, &host_imports).value, 3.5f); }

  // ~~ `(cast bool x)` -- truthiness, not a bit copy.
  { i64 args[1] = {0}; expect_eq_i64("int-to-bool(0)", bc_run_in_program(&prog, int_to_bool_fn, args, 1, heap, &host_imports).value, 0); }
  { i64 args[1] = {5}; expect_eq_i64("int-to-bool(5)", bc_run_in_program(&prog, int_to_bool_fn, args, 1, heap, &host_imports).value, 1); }
  { i64 args[1] = {f64_bits(0.0)}; expect_eq_i64("float-to-bool(0.0)", bc_run_in_program(&prog, float_to_bool_fn, args, 1, heap, &host_imports).value, 0); }
  { i64 args[1] = {f64_bits(1.5)}; expect_eq_i64("float-to-bool(1.5)", bc_run_in_program(&prog, float_to_bool_fn, args, 1, heap, &host_imports).value, 1); }

  // ~~ `(cast void x)` still runs `x`'s side effects.
  expect_eq_i64("read-side-effect() before void-discard", bc_run_in_program(&prog, read_side_effect_fn, NULL, 0, heap, &host_imports).value, 0);
  bc_run_in_program(&prog, void_discard_fn, NULL, 0, heap, &host_imports);
  expect_eq_i64("read-side-effect() after void-discard", bc_run_in_program(&prog, read_side_effect_fn, NULL, 0, heap, &host_imports).value, 1);

  // ~~ Same-type cast is a true no-op.
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

  if (g_failures == 0) printf("bcgen_cast_test: all checks passed\n");
  else                 printf("bcgen_cast_test: %d check(s) FAILED\n", g_failures);

  ctx_free();
  return g_failures == 0 ? 0 : 1;
}
