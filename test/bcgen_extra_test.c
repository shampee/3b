// bcgen_extra_test.c -- validates the f32 / array-field / arbitrary-value
// nested-struct-field support added on top of the earlier bcgen.c/bcvm.c
// slices (see bcgen.c's top-of-file note). Same rig as the other
// bcgen_*_test.c files.
//
// Exercises: f32 arithmetic/comparisons (the low 4 bytes of a register,
// real float-precision math -- not widened to f64), a fixed-size array
// field constructed from an ArrayLiteral and read back via `nth`
// (TypedNodeKind_IndexAccess, sharing the plain integer Add/Mul opcodes
// for the address computation rather than a dedicated addressing opcode),
// and a NESTED BY-VALUE STRUCT FIELD filled from an arbitrary expression
// (an existing parameter, not an inline literal) -- using Vector3 (size
// 12, NOT a multiple of 8) specifically to exercise bc_copy_struct_bytes's
// whole-8-byte-words-plus-one-4-byte-remainder unrolled copy, not just the
// all-multiple-of-8 case.
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
expect_eq_f32(const char* what, i64 got_bits, f32 want) {
  f32 got; u32 bits32 = (u32)got_bits; memcpy(&got, &bits32, sizeof(got));
  if (got != want) {
    fprintf(stderr, "FAIL %s: got %g, want %g\n", what, (double)got, (double)want);
    g_failures += 1;
  }
}

static i64
f32_bits(f32 v) {
  u32 bits; memcpy(&bits, &v, sizeof(bits));
  return (i64)(u64)bits;
}

static const char* g_fixture_source =
  "(package bcgen_extra_test)\n"
  "(fn f32add [a f32 b f32] f32 (+ a b))\n"
  "(fn f32mul [a f32 b f32] f32 (* a b))\n"
  "(fn f32lt [a f32 b f32] bool (< a b))\n"
  "\n"
  "(struct Triangle [verts [i32 3]])\n"
  "(fn make-triangle [a i32 b i32 c i32] Triangle\n"
  "  (Triangle {:verts [a b c]}))\n"
  "(fn triangle-vert [t Triangle idx i32] i32\n"
  "  (nth (get t verts) idx))\n"
  "\n"
  "(struct Vector3 [x i32 y i32 z i32])\n"
  "(struct Line3 [from Vector3 to Vector3])\n"
  "(fn make-vector3 [x i32 y i32 z i32] Vector3\n"
  "  (Vector3 {:x x :y y :z z}))\n"
  "(fn make-line3 [p Vector3 q Vector3] Line3\n"
  "  (Line3 {:from p :to q}))\n"
  "(fn line3-from-x [l Line3] i32 (get-in l [from x]))\n"
  "(fn line3-from-z [l Line3] i32 (get-in l [from z]))\n"
  "(fn line3-to-y [l Line3] i32 (get-in l [to y]))\n"
  "\n"
  "(fn main [] i32 0)\n";

static Checker
check_fixture(TypedAst* tast, TypedIndex* out_root) {
  Ast ast;
  ast_init(&ast, ctx_perm());

  String8 src     = str8_cstring((char*)(g_fixture_source));
  u32     file_id = source_file_register(str8_lit("bcgen_extra_test_fixture.3b"), src);

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
  u32 f32add_fn         = bc_program_find_fn(&prog, str8_lit("f32add"));
  u32 f32mul_fn         = bc_program_find_fn(&prog, str8_lit("f32mul"));
  u32 f32lt_fn          = bc_program_find_fn(&prog, str8_lit("f32lt"));
  u32 make_triangle_fn  = bc_program_find_fn(&prog, str8_lit("make-triangle"));
  u32 triangle_vert_fn  = bc_program_find_fn(&prog, str8_lit("triangle-vert"));
  u32 make_vector3_fn   = bc_program_find_fn(&prog, str8_lit("make-vector3"));
  u32 make_line3_fn     = bc_program_find_fn(&prog, str8_lit("make-line3"));
  u32 line3_from_x_fn   = bc_program_find_fn(&prog, str8_lit("line3-from-x"));
  u32 line3_from_z_fn   = bc_program_find_fn(&prog, str8_lit("line3-from-z"));
  u32 line3_to_y_fn     = bc_program_find_fn(&prog, str8_lit("line3-to-y"));

  Arena* heap = ctx_perm();

  // ~~ f32 arithmetic/comparisons.
  { i64 args[2] = { f32_bits(2.5f), f32_bits(4.25f) };
    expect_eq_f32("f32add(2.5,4.25)", bc_run_in_program(&prog, f32add_fn, args, 2, heap, &host_imports).value, 6.75f); }
  { i64 args[2] = { f32_bits(3.0f), f32_bits(4.0f) };
    expect_eq_f32("f32mul(3,4)", bc_run_in_program(&prog, f32mul_fn, args, 2, heap, &host_imports).value, 12.0f); }
  { i64 args[2] = { f32_bits(1.0f), f32_bits(2.0f) };
    expect_eq_i64("f32lt(1,2)", bc_run_in_program(&prog, f32lt_fn, args, 2, heap, &host_imports).value, 1); }
  { i64 args[2] = { f32_bits(2.0f), f32_bits(1.0f) };
    expect_eq_i64("f32lt(2,1)", bc_run_in_program(&prog, f32lt_fn, args, 2, heap, &host_imports).value, 0); }

  // ~~ Array field: construct from an ArrayLiteral, read elements back via `nth`.
  { i64 args[3] = {10, 20, 30};
    BcResult r = bc_run_in_program(&prog, make_triangle_fn, args, 3, heap, &host_imports);
    i64 tri_ptr = r.value;
    { i64 vargs[2] = {tri_ptr, 0}; expect_eq_i64("triangle-vert(t,0)", bc_run_in_program(&prog, triangle_vert_fn, vargs, 2, heap, &host_imports).value, 10); }
    { i64 vargs[2] = {tri_ptr, 1}; expect_eq_i64("triangle-vert(t,1)", bc_run_in_program(&prog, triangle_vert_fn, vargs, 2, heap, &host_imports).value, 20); }
    { i64 vargs[2] = {tri_ptr, 2}; expect_eq_i64("triangle-vert(t,2)", bc_run_in_program(&prog, triangle_vert_fn, vargs, 2, heap, &host_imports).value, 30); }
  }

  // ~~ Nested by-value struct field filled from an ARBITRARY expression
  // (make-line3's own `p`/`q` params, not inline literals) -- Vector3 is
  // 12 bytes (not a multiple of 8), so this exercises bc_copy_struct_bytes's
  // 8-byte-word(s) + 4-byte-remainder unrolled copy.
  { i64 pargs[3] = {1, 2, 3};
    i64 p_ptr = bc_run_in_program(&prog, make_vector3_fn, pargs, 3, heap, &host_imports).value;
    i64 qargs[3] = {7, 8, 9};
    i64 q_ptr = bc_run_in_program(&prog, make_vector3_fn, qargs, 3, heap, &host_imports).value;

    i64 largs[2] = {p_ptr, q_ptr};
    i64 line_ptr = bc_run_in_program(&prog, make_line3_fn, largs, 2, heap, &host_imports).value;

    { i64 a2[1] = {line_ptr}; expect_eq_i64("line3-from-x", bc_run_in_program(&prog, line3_from_x_fn, a2, 1, heap, &host_imports).value, 1); }
    { i64 a2[1] = {line_ptr}; expect_eq_i64("line3-from-z", bc_run_in_program(&prog, line3_from_z_fn, a2, 1, heap, &host_imports).value, 3); }
    { i64 a2[1] = {line_ptr}; expect_eq_i64("line3-to-y",   bc_run_in_program(&prog, line3_to_y_fn,   a2, 1, heap, &host_imports).value, 8); }

    // The embedded copy must be a REAL copy, not aliasing -- mutating the
    // original Vector3's backing bytes directly (raw memory rather than a
    // compiled `set`, so the check does not depend on the write path it is
    // testing) must NOT change what's already inside line_ptr.
    u8* p_base = (u8*)(intptr_t)p_ptr;
    i32 mutated = 999;
    memcpy(p_base + 0, &mutated, sizeof(mutated)); // p.x = 999, directly
    { i64 a2[1] = {line_ptr};
      expect_eq_i64("line3-from-x after mutating the ORIGINAL p (must be unaffected -- real copy, not aliasing)",
                    bc_run_in_program(&prog, line3_from_x_fn, a2, 1, heap, &host_imports).value, 1); }
  }

  arena_temp_end(&fn_temp);

  if (g_failures == 0) printf("bcgen_extra_test: all checks passed\n");
  else                 printf("bcgen_extra_test: %d check(s) FAILED\n", g_failures);

  ctx_free();
  return g_failures == 0 ? 0 : 1;
}
