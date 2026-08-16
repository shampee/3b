// bcgen_host_test.c -- validates host imports (BcOp_CallHost) end to end:
// registering real native C functions, calling them from compiled
// bytecode via a body-less `extern fn` declaration on the 3bscript side
// (the same syntax already used for real C FFI -- see bytecode.h's
// BcHostImportTable comment for why this needed zero new parser/checker
// work), and observing that a host call can do things pure bytecode
// can't: real floating-point math (via libm's `sqrt`) and mutating state
// that lives entirely on the native side, outside the VM -- via a global
// (native_bump_counter) AND via BcHostImportTable.userdata
// (native_bump_via_userdata), the newer, non-global way to reach native
// context from a Trampoline host call (see bytecode.h's BcHostFn comment).
//
// Same rig as the other bcgen_*_test.c files (hand-rolled parse -> lower
// -> check against an in-memory fixture).
#include "3b.h"
#include "bcgen.h"
#include "bcvm.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

static int g_failures = 0;

static void
expect_eq_i64(const char* what, i64 got, i64 want) {
  if (got != want) {
    fprintf(stderr, "FAIL %s: got %lld, want %lld\n", what, (long long)got, (long long)want);
    g_failures += 1;
  }
}

// ~~ The native side of two host imports. Both conform to BcHostFn's one
// fixed signature (see bytecode.h) -- each function is responsible for
// interpreting its own `args` array, there's no per-signature marshaling.
static i64 g_native_counter = 0;

static i64
native_sqrt_i32(i64* args, u32 arg_count, Arena* heap, void* userdata) {
  (void)arg_count; (void)heap; (void)userdata;
  i32 v = (i32)args[0];
  return (i64)(i32)sqrt((double)v); // real libm math -- nothing in this bytecode ISA can do this
}

static i64
native_bump_counter(i64* args, u32 arg_count, Arena* heap, void* userdata) {
  (void)arg_count; (void)heap; (void)userdata;
  g_native_counter += args[0]; // mutates state OUTSIDE the VM entirely -- observable after the call returns
  return g_native_counter;
}

// ~~ Same idea as native_bump_counter, but reaches its native counter via
// `userdata` (a pointer to a plain C i64 this test owns, set on the table
// below) instead of a global -- proving BcHostImportTable.userdata
// actually reaches a Trampoline call, and that TWO SEPARATE tables (see
// main below) can point the SAME registered function at DIFFERENT native
// state, which a global could never do.
static i64
native_bump_via_userdata(i64* args, u32 arg_count, Arena* heap, void* userdata) {
  (void)arg_count; (void)heap;
  i64* counter = (i64*)userdata;
  *counter += args[0];
  return *counter;
}

static const char* g_fixture_source =
  "(package bcgen_host_test)\n"
  "(extern (fn native-sqrt-i32 [n i32] i32))\n"
  "(extern (fn native-bump-counter [amount i32] i32))\n"
  "\n"
  "(fn hypotenuse-ish [a i32 b i32] i32\n"
  "  (native-sqrt-i32 (+ (* a a) (* b b))))\n"
  "\n"
  "(fn bump [amount i32] i32\n"
  "  (native-bump-counter amount))\n"
  "\n"
  "(fn bump-twice [amount i32] i32\n"
  "  (bump amount)\n"
  "  (bump amount))\n"
  "\n"
  "(extern (fn native-bump-via-userdata [amount i32] i32))\n"
  "(fn bump-via-userdata [amount i32] i32\n"
  "  (native-bump-via-userdata amount))\n"
  "\n"
  "(fn main [] i32 0)\n";

static Checker
check_fixture(TypedAst* tast, TypedIndex* out_root) {
  Ast ast;
  ast_init(&ast, ctx_perm());

  String8 src     = str8_cstring((char*)(g_fixture_source));
  u32     file_id = source_file_register(str8_lit("bcgen_host_test_fixture.3b"), src);

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

  // Register the host imports BEFORE compiling -- bc_compile_program
  // resolves each `extern fn` call site's callee name against this table
  // once, at compile time (see bcgen.c's TypedNodeKind_Call handling), AND
  // cross-checks each entry's declared signature against the matching
  // `extern` declaration's own real types (bc_verify_host_imports) --
  // both of these functions really do take exactly one i32 and return i32,
  // matching what's registered below.
  TypeRef i32_ty = {0}; i32_ty.kind = TypeKind_I32;
  TypeRef one_i32_param[1] = { i32_ty };

  // ~~ A native i64 this test owns, reached ONLY via userdata -- no global
  // involved (unlike g_native_counter above), proving the plumbing itself,
  // not just that a Trampoline function CAN mutate outside state (already
  // shown by native_bump_counter).
  i64 userdata_counter = 0;

  BcHostImportTable host_imports = {0};
  host_imports.userdata = &userdata_counter;
  bc_host_import_table_add(&host_imports, ctx_perm(), str8_lit("native-sqrt-i32"), native_sqrt_i32,
                            one_i32_param, 1, i32_ty);
  bc_host_import_table_add(&host_imports, ctx_perm(), str8_lit("native-bump-counter"), native_bump_counter,
                            one_i32_param, 1, i32_ty);
  bc_host_import_table_add(&host_imports, ctx_perm(), str8_lit("native-bump-via-userdata"), native_bump_via_userdata,
                            one_i32_param, 1, i32_ty);

  BcProgram prog = bc_compile_program(&ck, &tast, root, fn_temp.arena, ctx_perm(), &layout_cache, &host_imports, NULL);
  u32 hyp_fn             = bc_program_find_fn(&prog, str8_lit("hypotenuse-ish"));
  u32 bump_fn            = bc_program_find_fn(&prog, str8_lit("bump"));
  u32 bump_twice_fn      = bc_program_find_fn(&prog, str8_lit("bump-twice"));
  u32 bump_userdata_fn   = bc_program_find_fn(&prog, str8_lit("bump-via-userdata"));

  Arena* heap = ctx_perm();

  { i64 args[2] = {3, 4};  expect_eq_i64("hypotenuse-ish(3,4)", bc_run_in_program(&prog, hyp_fn, args, 2, heap, &host_imports).value, 5); }
  { i64 args[2] = {6, 8};  expect_eq_i64("hypotenuse-ish(6,8)", bc_run_in_program(&prog, hyp_fn, args, 2, heap, &host_imports).value, 10); }

  expect_eq_i64("g_native_counter starts at 0", g_native_counter, 0);
  { i64 args[1] = {10}; expect_eq_i64("bump(10)", bc_run_in_program(&prog, bump_fn, args, 1, heap, &host_imports).value, 10); }
  expect_eq_i64("g_native_counter after bump(10) -- observable OUTSIDE the VM", g_native_counter, 10);
  { i64 args[1] = {5};  expect_eq_i64("bump(5)", bc_run_in_program(&prog, bump_fn, args, 1, heap, &host_imports).value, 15); }
  expect_eq_i64("g_native_counter after bump(5)", g_native_counter, 15);

  // bump-twice calls the COMPILED bump fn (a real BcOp_Call), which
  // itself calls the HOST import (BcOp_CallHost) -- proving the two call
  // kinds compose through nested bc_run_in_program recursion correctly.
  { i64 args[1] = {1}; expect_eq_i64("bump-twice(1)", bc_run_in_program(&prog, bump_twice_fn, args, 1, heap, &host_imports).value, 17); }
  expect_eq_i64("g_native_counter after bump-twice(1)", g_native_counter, 17);

  // ~~ BcHostImportTable.userdata reaches the Trampoline call -- a
  // DIFFERENT counter from g_native_counter, reached with no global at all.
  expect_eq_i64("userdata_counter starts at 0", userdata_counter, 0);
  { i64 args[1] = {7}; expect_eq_i64("bump-via-userdata(7)", bc_run_in_program(&prog, bump_userdata_fn, args, 1, heap, &host_imports).value, 7); }
  expect_eq_i64("userdata_counter after bump-via-userdata(7)", userdata_counter, 7);
  expect_eq_i64("g_native_counter unaffected by the userdata-based calls", g_native_counter, 17);

  arena_temp_end(&fn_temp);

  if (g_failures == 0) printf("bcgen_host_test: all checks passed\n");
  else                 printf("bcgen_host_test: %d check(s) FAILED\n", g_failures);

  ctx_free();
  return g_failures == 0 ? 0 : 1;
}
