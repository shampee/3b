// bcgen_native_test.c -- validates BcHostImportKind_Direct (see
// bcnative.c's own top-of-file note for the mechanism): calling an
// ALREADY-EXISTING native function with its own natural C signature,
// directly, with NO i64*-unpacking trampoline wrapper written for it.
// Same rig as the other bcgen_*_test.c files.
//
// Exercises:
//  - native_gcd(i32,i32)->i32 and native_sum6(six i64 args)->i64 -- both
//    registered via bc_host_import_table_add_direct with their OWN real
//    signatures, proving the function-pointer-cast trick actually works
//    inside the real VM, not just in the standalone verification this was
//    checked with before writing any of this.
//  - native_sum6 specifically exercises ALL 6 of System V's integer
//    argument registers at once, not just a couple.
//  - native_sum12 exercises 12 arguments -- past SysV's own 6-register
//    threshold, into the STACK-passed remainder (see
//    BC_NATIVE_DIRECT_MAX_ARGS in bcnative.h) -- proving Direct-mode host
//    calls work for stack-passed arguments too, not just register-passed
//    ones, through the REAL VM (not just the standalone verification
//    bcnative.c's own top-of-file note describes).
//  - Direct AND Trampoline host imports coexisting in ONE table/program
//    (gcd-then-bump calls a Direct import, then feeds its result into a
//    Trampoline import) -- proving bcvm.c's BcOp_CallHost dispatch
//    branch never cross-contaminates the two kinds.
#include "3b.h"
#include "bcgen.h"
#include "bcvm.h"
#include "bcnative.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

static int g_failures = 0;

static void
expect_eq_i64(const char* what, i64 got, i64 want) {
  if (got != want) {
    fprintf(stderr, "FAIL %s: got %lld, want %lld\n", what, (long long)got, (long long)want);
    g_failures += 1;
  }
}

// ~~ Real native functions with their OWN natural signatures -- no
// i64*-unpacking wrapper exists for either of these; bc_call_native_direct
// calls them exactly as-is.
static i32
native_gcd(i32 a, i32 b) {
  while (b != 0) { i32 t = b; b = a % b; a = t; }
  return a;
}

static i64
native_sum6(i64 a, i64 b, i64 c, i64 d, i64 e, i64 f) {
  return a + b + c + d + e + f;
}

// Exactly BC_NATIVE_DIRECT_MAX_ARGS parameters -- the 7th onward are
// SysV STACK-passed, not register-passed, exercising the part
// bc_call_native_direct's extended prototype specifically added.
static i64
native_sum12(i64 a, i64 b, i64 c, i64 d, i64 e, i64 f, i64 g, i64 h, i64 i, i64 j, i64 k, i64 l) {
  return a + b + c + d + e + f + g + h + i + j + k + l;
}

// ~~ A Trampoline-kind import too, mixed in deliberately -- proves the two
// registration kinds coexist correctly in one table.
static i64 g_native_counter = 0;
static i64
native_bump_counter(i64* args, u32 arg_count, Arena* heap, void* userdata) {
  (void)arg_count; (void)heap; (void)userdata;
  g_native_counter += args[0];
  return g_native_counter;
}

static const char* g_fixture_source =
  "(package bcgen_native_test)\n"
  "(extern (fn native-gcd [a i32 b i32] i32))\n"
  "(extern (fn native-sum6 [a i64 b i64 c i64 d i64 e i64 f i64] i64))\n"
  "(extern (fn native-sum12 [a i64 b i64 c i64 d i64 e i64 f i64 g i64 h i64 i i64 j i64 k i64 l i64] i64))\n"
  "(extern (fn native-bump-counter [amount i32] i32))\n"
  "\n"
  "(fn gcd-of [a i32 b i32] i32 (native-gcd a b))\n"
  "(fn sum-six [a i64 b i64 c i64 d i64 e i64 f i64] i64 (native-sum6 a b c d e f))\n"
  "(fn sum-twelve [a i64 b i64 c i64 d i64 e i64 f i64 g i64 h i64 i i64 j i64 k i64 l i64] i64\n"
  "  (native-sum12 a b c d e f g h i j k l))\n"
  "(fn bump [amount i32] i32 (native-bump-counter amount))\n"
  "(fn gcd-then-bump [a i32 b i32] i32 (native-bump-counter (native-gcd a b)))\n"
  "\n"
  "(fn main [] i32 0)\n";

static Checker
check_fixture(TypedAst* tast, TypedIndex* out_root) {
  Ast ast;
  ast_init(&ast, ctx_perm());

  String8 src     = str8_cstring((char*)(g_fixture_source));
  u32     file_id = source_file_register(str8_lit("bcgen_native_test_fixture.3b"), src);

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

  TypeRef i32_ty = {0}; i32_ty.kind = TypeKind_I32;
  TypeRef i64_ty = {0}; i64_ty.kind = TypeKind_I64;
  TypeRef gcd_params[2]   = { i32_ty, i32_ty };
  TypeRef sum6_params[6]  = { i64_ty, i64_ty, i64_ty, i64_ty, i64_ty, i64_ty };
  TypeRef sum12_params[12] = { i64_ty, i64_ty, i64_ty, i64_ty, i64_ty, i64_ty,
                                i64_ty, i64_ty, i64_ty, i64_ty, i64_ty, i64_ty };
  TypeRef bump_params[1] = { i32_ty };

  BcHostImportTable host_imports = {0};
  bc_host_import_table_add_direct(&host_imports, ctx_perm(), str8_lit("native-gcd"),
                                   (void*)(intptr_t)native_gcd, gcd_params, 2, i32_ty);
  bc_host_import_table_add_direct(&host_imports, ctx_perm(), str8_lit("native-sum6"),
                                   (void*)(intptr_t)native_sum6, sum6_params, 6, i64_ty);
  bc_host_import_table_add_direct(&host_imports, ctx_perm(), str8_lit("native-sum12"),
                                   (void*)(intptr_t)native_sum12, sum12_params, 12, i64_ty);
  bc_host_import_table_add(&host_imports, ctx_perm(), str8_lit("native-bump-counter"),
                            native_bump_counter, bump_params, 1, i32_ty);

  BcProgram prog = bc_compile_program(&ck, &tast, root, fn_temp.arena, ctx_perm(), &layout_cache, &host_imports, NULL);
  u32 gcd_of_fn        = bc_program_find_fn(&prog, str8_lit("gcd-of"));
  u32 sum_six_fn       = bc_program_find_fn(&prog, str8_lit("sum-six"));
  u32 sum_twelve_fn    = bc_program_find_fn(&prog, str8_lit("sum-twelve"));
  u32 bump_fn          = bc_program_find_fn(&prog, str8_lit("bump"));
  u32 gcd_then_bump_fn = bc_program_find_fn(&prog, str8_lit("gcd-then-bump"));

  Arena* heap = ctx_perm();

  { i64 args[2] = {48, 18}; expect_eq_i64("gcd-of(48,18)", bc_run_in_program(&prog, gcd_of_fn, args, 2, heap, &host_imports).value, 6); }
  { i64 args[2] = {17, 5};  expect_eq_i64("gcd-of(17,5)",  bc_run_in_program(&prog, gcd_of_fn, args, 2, heap, &host_imports).value, 1); }
  { i64 args[2] = {0, 9};   expect_eq_i64("gcd-of(0,9)",   bc_run_in_program(&prog, gcd_of_fn, args, 2, heap, &host_imports).value, 9); }

  { i64 args[6] = {1, 2, 3, 4, 5, 6};
    expect_eq_i64("sum-six(1,2,3,4,5,6) -- all 6 SysV integer arg registers at once",
                  bc_run_in_program(&prog, sum_six_fn, args, 6, heap, &host_imports).value, 21); }
  { i64 args[6] = {100, -50, 25, 0, 1, 1};
    expect_eq_i64("sum-six(100,-50,25,0,1,1)", bc_run_in_program(&prog, sum_six_fn, args, 6, heap, &host_imports).value, 77); }

  { i64 args[12] = {1,2,3,4,5,6,7,8,9,10,11,12};
    expect_eq_i64("sum-twelve(1..12) -- past SysV's 6 registers, into stack-passed args",
                  bc_run_in_program(&prog, sum_twelve_fn, args, 12, heap, &host_imports).value, 78); }
  { i64 args[12] = {100,-50,25,0,1,1,-3,7,0,0,0,-30};
    expect_eq_i64("sum-twelve(mixed values)", bc_run_in_program(&prog, sum_twelve_fn, args, 12, heap, &host_imports).value, 51); }

  expect_eq_i64("g_native_counter starts at 0", g_native_counter, 0);
  { i64 args[1] = {10}; expect_eq_i64("bump(10)", bc_run_in_program(&prog, bump_fn, args, 1, heap, &host_imports).value, 10); }
  expect_eq_i64("g_native_counter after bump(10)", g_native_counter, 10);

  // Direct (native-gcd) feeding into Trampoline (native-bump-counter)
  // within the SAME compiled function -- proves the two kinds compose.
  { i64 args[2] = {48, 18}; // gcd = 6
    expect_eq_i64("gcd-then-bump(48,18)", bc_run_in_program(&prog, gcd_then_bump_fn, args, 2, heap, &host_imports).value, 16); } // 10+6
  expect_eq_i64("g_native_counter after gcd-then-bump", g_native_counter, 16);

  arena_temp_end(&fn_temp);

  if (g_failures == 0) printf("bcgen_native_test: all checks passed\n");
  else                 printf("bcgen_native_test: %d check(s) FAILED\n", g_failures);

  ctx_free();
  return g_failures == 0 ? 0 : 1;
}
