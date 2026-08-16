// bcgen_mem_test.c -- validates the `(mem-set dst byte size)` /
// `(mem-copy dst src size)` / `(mem-zero dst size)` / `(mem-compare a b
// size)` family added to bcgen.c/bcvm.c (BcOp_MemSet/MemCopy/MemZero/
// MemCompare). The whole family was previously unimplemented on this
// backend -- every one of them failed with "no compiled function or
// registered host import by that name", even though the checker accepted
// them and codegen.c emitted them fine, so the two backends genuinely
// could not run the same source.
//
// Exercises:
//  - Each op's SIZE boundary, by reading a byte just inside the affected
//    range and one just past it. A wrong size is the failure mode that
//    matters here (these ops write raw memory), and it's exactly what a
//    plain "did the bytes change?" check would miss.
//  - mem-compare's sign in both directions, proving its 3-register
//    contiguous-block argument convention (a, b, size) puts the operands
//    in the right ORDER -- a swapped pair still compares "not equal", so
//    only the sign distinguishes it.
//  - A FLOAT size/byte argument. The checker admits any numeric type
//    there, which costs codegen.c nothing (C converts the double) but
//    would reinterpret an f64 bit pattern as an enormous garbage length
//    here without bcgen.c's bc_compile_size_arg normalization.
//  - mem-copy/mem-compare over a real struct via `(addr s)`/`(sizeof T)`,
//    the family's most realistic use.
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

// memcmp's MAGNITUDE is implementation-defined (only the sign is
// specified), so the ordering checks compare sign, not value.
static void
expect_sign(const char* what, i64 got, int want_sign) {
  int got_sign = (got < 0) ? -1 : (got > 0) ? 1 : 0;
  if (got_sign != want_sign) {
    fprintf(stderr, "FAIL %s: got %lld (sign %d), want sign %d\n",
            what, (long long)got, got_sign, want_sign);
    g_failures += 1;
  }
}

static const char* g_fixture_source =
  "(package bcgen_mem_test)\n"
  "\n"
  "(struct Pt [x i32 y i32])\n"
  "\n"
  // mem-set 8 of 16 bytes: idx 0..7 are `b`, idx 8..15 stay 0.
  "(fn set-byte [b i32 idx i32] i32\n"
  "  (var p u8* (alloc u8 16))\n"
  "  (mem-set p 0 16)\n"
  "  (mem-set p (cast u8 b) 8)\n"
  "  (var out i32 (cast i32 (nth p idx)))\n"
  "  (free p)\n"
  "  out)\n"
  "\n"
  // mem-copy 8 of 16 bytes from an all-3 source over an all-9 dest.
  "(fn copy-byte [idx i32] i32\n"
  "  (var a u8* (alloc u8 16))\n"
  "  (var b u8* (alloc u8 16))\n"
  "  (mem-set a 3 16)\n"
  "  (mem-set b 9 16)\n"
  "  (mem-copy b a 8)\n"
  "  (var out i32 (cast i32 (nth b idx)))\n"
  "  (free a)\n"
  "  (free b)\n"
  "  out)\n"
  "\n"
  // mem-zero 8 of 16 bytes of an all-7 buffer.
  "(fn zero-byte [idx i32] i32\n"
  "  (var p u8* (alloc u8 16))\n"
  "  (mem-set p 7 16)\n"
  "  (mem-zero p 8)\n"
  "  (var out i32 (cast i32 (nth p idx)))\n"
  "  (free p)\n"
  "  out)\n"
  "\n"
  "(fn cmp [av i32 bv i32] i32\n"
  "  (var a u8* (alloc u8 8))\n"
  "  (var b u8* (alloc u8 8))\n"
  "  (mem-set a (cast u8 av) 8)\n"
  "  (mem-set b (cast u8 bv) 8)\n"
  "  (var out i32 (mem-compare a b 8))\n"
  "  (free a)\n"
  "  (free b)\n"
  "  out)\n"
  "\n"
  // An f64-literal size -- must set exactly 8 bytes, not a garbage length.
  "(fn float-size [idx i32] i32\n"
  "  (var p u8* (alloc u8 16))\n"
  "  (mem-set p 0 16)\n"
  "  (mem-set p 4 8.0)\n"
  "  (var out i32 (cast i32 (nth p idx)))\n"
  "  (free p)\n"
  "  out)\n"
  "\n"
  // ...and an f32-typed one, which bcgen.c widens before converting.
  "(fn f32-size [idx i32] i32\n"
  "  (var p u8* (alloc u8 16))\n"
  "  (mem-set p 0 16)\n"
  "  (var n f32 4.0)\n"
  "  (mem-set p 6 n)\n"
  "  (var out i32 (cast i32 (nth p idx)))\n"
  "  (free p)\n"
  "  out)\n"
  "\n"
  "(fn struct-copy [] i32\n"
  "  (var a Pt (zero Pt))\n"
  "  (var b Pt (zero Pt))\n"
  "  (set (. a x) 11)\n"
  "  (set (. a y) 22)\n"
  "  (mem-copy (addr b) (addr a) (sizeof Pt))\n"
  "  (+ (. b x) (. b y)))\n"
  "\n"
  "(fn struct-cmp [] i32\n"
  "  (var a Pt (zero Pt))\n"
  "  (var b Pt (zero Pt))\n"
  "  (set (. a x) 11)\n"
  "  (mem-copy (addr b) (addr a) (sizeof Pt))\n"
  "  (mem-compare (addr a) (addr b) (sizeof Pt)))\n"
  "\n"
  "(fn struct-zero [] i32\n"
  "  (var a Pt (zero Pt))\n"
  "  (set (. a x) 11)\n"
  "  (set (. a y) 22)\n"
  "  (mem-zero (addr a) (sizeof Pt))\n"
  "  (+ (. a x) (. a y)))\n"
  "\n"
  "(fn main [] i32 0)\n";

static Checker
check_fixture(TypedAst* tast, TypedIndex* out_root) {
  Ast ast;
  ast_init(&ast, ctx_perm());

  String8 src     = str8_cstring((char*)(g_fixture_source));
  u32     file_id = source_file_register(str8_lit("bcgen_mem_test_fixture.3b"), src);

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

  u32 set_byte_fn    = bc_program_find_fn(&prog, str8_lit("set-byte"));
  u32 copy_byte_fn   = bc_program_find_fn(&prog, str8_lit("copy-byte"));
  u32 zero_byte_fn   = bc_program_find_fn(&prog, str8_lit("zero-byte"));
  u32 cmp_fn         = bc_program_find_fn(&prog, str8_lit("cmp"));
  u32 float_size_fn  = bc_program_find_fn(&prog, str8_lit("float-size"));
  u32 f32_size_fn    = bc_program_find_fn(&prog, str8_lit("f32-size"));
  u32 struct_copy_fn = bc_program_find_fn(&prog, str8_lit("struct-copy"));
  u32 struct_cmp_fn  = bc_program_find_fn(&prog, str8_lit("struct-cmp"));
  u32 struct_zero_fn = bc_program_find_fn(&prog, str8_lit("struct-zero"));

  Arena* heap = ctx_perm();

  // mem-set: the byte value lands, and stops exactly at `size`.
  { i64 args[2] = {5, 0};
    expect_eq_i64("set-byte(5, 0)", bc_run_in_program(&prog, set_byte_fn, args, 2, heap, &host_imports).value, 5); }
  { i64 args[2] = {5, 7};
    expect_eq_i64("set-byte(5, 7)", bc_run_in_program(&prog, set_byte_fn, args, 2, heap, &host_imports).value, 5); }
  { i64 args[2] = {5, 8}; // one past the 8-byte range -- must be untouched
    expect_eq_i64("set-byte(5, 8)", bc_run_in_program(&prog, set_byte_fn, args, 2, heap, &host_imports).value, 0); }
  { i64 args[2] = {255, 0}; // a byte value with the high bit set
    expect_eq_i64("set-byte(255, 0)", bc_run_in_program(&prog, set_byte_fn, args, 2, heap, &host_imports).value, 255); }

  // mem-copy: source bytes land, and stop exactly at `size`.
  { i64 args[1] = {0};
    expect_eq_i64("copy-byte(0)", bc_run_in_program(&prog, copy_byte_fn, args, 1, heap, &host_imports).value, 3); }
  { i64 args[1] = {7};
    expect_eq_i64("copy-byte(7)", bc_run_in_program(&prog, copy_byte_fn, args, 1, heap, &host_imports).value, 3); }
  { i64 args[1] = {8}; // past the copied range -- still the dest's own 9
    expect_eq_i64("copy-byte(8)", bc_run_in_program(&prog, copy_byte_fn, args, 1, heap, &host_imports).value, 9); }

  // mem-zero: zeroes, and stops exactly at `size`.
  { i64 args[1] = {0};
    expect_eq_i64("zero-byte(0)", bc_run_in_program(&prog, zero_byte_fn, args, 1, heap, &host_imports).value, 0); }
  { i64 args[1] = {7};
    expect_eq_i64("zero-byte(7)", bc_run_in_program(&prog, zero_byte_fn, args, 1, heap, &host_imports).value, 0); }
  { i64 args[1] = {8}; // past the zeroed range -- still 7
    expect_eq_i64("zero-byte(8)", bc_run_in_program(&prog, zero_byte_fn, args, 1, heap, &host_imports).value, 7); }

  // mem-compare: equal is 0, and the sign follows argument ORDER.
  { i64 args[2] = {4, 4};
    expect_sign("cmp(4, 4)", bc_run_in_program(&prog, cmp_fn, args, 2, heap, &host_imports).value, 0); }
  { i64 args[2] = {3, 9};
    expect_sign("cmp(3, 9)", bc_run_in_program(&prog, cmp_fn, args, 2, heap, &host_imports).value, -1); }
  { i64 args[2] = {9, 3};
    expect_sign("cmp(9, 3)", bc_run_in_program(&prog, cmp_fn, args, 2, heap, &host_imports).value, 1); }

  // A float size must behave exactly like the equivalent integer one.
  { i64 args[1] = {7};
    expect_eq_i64("float-size(7)", bc_run_in_program(&prog, float_size_fn, args, 1, heap, &host_imports).value, 4); }
  { i64 args[1] = {8};
    expect_eq_i64("float-size(8)", bc_run_in_program(&prog, float_size_fn, args, 1, heap, &host_imports).value, 0); }
  { i64 args[1] = {3};
    expect_eq_i64("f32-size(3)", bc_run_in_program(&prog, f32_size_fn, args, 1, heap, &host_imports).value, 6); }
  { i64 args[1] = {4};
    expect_eq_i64("f32-size(4)", bc_run_in_program(&prog, f32_size_fn, args, 1, heap, &host_imports).value, 0); }

  // The `(addr s)`/`(sizeof T)` struct path.
  expect_eq_i64("struct-copy()", bc_run_in_program(&prog, struct_copy_fn, NULL, 0, heap, &host_imports).value, 33);
  expect_sign("struct-cmp()",   bc_run_in_program(&prog, struct_cmp_fn,  NULL, 0, heap, &host_imports).value, 0);
  expect_eq_i64("struct-zero()", bc_run_in_program(&prog, struct_zero_fn, NULL, 0, heap, &host_imports).value, 0);

  arena_temp_end(&fn_temp);

  if (g_failures == 0) printf("bcgen_mem_test: all checks passed\n");
  else                 printf("bcgen_mem_test: %d check(s) FAILED\n", g_failures);

  ctx_free();
  return g_failures == 0 ? 0 : 1;
}
