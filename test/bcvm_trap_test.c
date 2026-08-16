// bcvm_trap_test.c -- validates that a VM trap (bcvm.c's BcOp_Div/BcOp_Mod
// zero-divisor bail-out) reaches the caller as a trap, carrying enough to say
// what happened and where, and never as a plain value.
//
// Programs here are HAND-BUILT out of BcInstr rather than compiled from 3b
// source, for two reasons. The interesting case is BcOp_CallModule, which
// bcgen.c only emits against a BcModuleTable an embedder set up (script.c is
// the only one), so no single-file source fixture can reach it. And a trap's
// whole point is what happens to the frames AROUND it -- writing those frames
// out directly makes each case's before/after state its own three lines of
// bytecode instead of something inferred from generated code.
//
// The bug this pins down: BcOp_CallModule used to store `result.value` from
// the nested run unconditionally, so a division by zero inside an imported
// module surfaced in the importing script as the legitimate answer 0, and
// execution carried on from there.
#include "3b.h"
#include "bytecode.h"
#include "bcvm.h"
#include <stdio.h>

static int g_failures = 0;

static void
expect_true(const char* what, b32 got) {
  if (!got) {
    fprintf(stderr, "FAIL %s: expected true\n", what);
    g_failures += 1;
  }
}

static void
expect_eq_i64(const char* what, i64 got, i64 want) {
  if (got != want) {
    fprintf(stderr, "FAIL %s: got %lld, want %lld\n", what, (long long)got, (long long)want);
    g_failures += 1;
  }
}

static void
expect_eq_str(const char* what, String8 got, const char* want) {
  if (!str8_match(got, str8_cstring((char*)want), 0)) {
    fprintf(stderr, "FAIL %s: got '%.*s', want '%s'\n", what, str8_varg(got), want);
    g_failures += 1;
  }
}

////////////////////////////////
//~ Chunk-building helpers

static BcChunk
chunk_new(const char* name, u32 param_count, u32 num_registers) {
  BcChunk c = {0};
  c.name          = str8_cstring((char*)name);
  c.param_count   = param_count;
  c.num_registers = num_registers;
  return c;
}

static void
emit(BcChunk* c, BcOp kind, u32 a, u32 b, u32 cc) {
  BcInstr ins = { .kind = kind, .a = a, .b = b, .c = cc };
  dyn_push(ctx_perm(), c->code, ins);
}

// Returns the const-pool slot `v` landed in, for the BcOp_LoadConst that reads it.
static u32
add_const(BcChunk* c, i64 v) {
  u32 slot = (u32)dyn_count(c->consts);
  dyn_push(ctx_perm(), c->consts, v);
  return slot;
}

static void
program_add_chunk(BcProgram* p, BcChunk c) {
  dyn_push(ctx_perm(), p->chunks, c);
}

// `(fn NAME [x] (OP x 0))` -- a one-parameter function that divides its
// argument by a zero constant with the given opcode, and would return the
// result if it ever got one.
static BcChunk
chunk_divide_by_zero(const char* name, BcOp op) {
  BcChunk c    = chunk_new(name, 1, 3);
  u32     zero = add_const(&c, 0);
  emit(&c, BcOp_LoadConst, 1, zero, 0);
  emit(&c, op, 2, 0, 1);
  emit(&c, BcOp_Return, 2, 0, 0);
  return c;
}

////////////////////////////////
//~ Cases

// The two opcodes that trap, each naming itself and the function it was in.
static void
test_direct_trap(Arena* heap) {
  BcProgram prog = {0};
  program_add_chunk(&prog, chunk_divide_by_zero("divider", BcOp_Div));
  program_add_chunk(&prog, chunk_divide_by_zero("remainderer", BcOp_Mod));

  i64      arg = 10;
  BcResult div = bc_run_in_program(&prog, 0, &arg, 1, heap, NULL);
  expect_true("div-by-zero traps", div.trapped);
  expect_eq_str("div trap message", div.trap_message, "division by zero");
  expect_eq_str("div trap fn", div.trap_fn, "divider");

  BcResult mod = bc_run_in_program(&prog, 1, &arg, 1, heap, NULL);
  expect_true("mod-by-zero traps", mod.trapped);
  expect_eq_str("mod trap message", mod.trap_message, "remainder by zero");
  expect_eq_str("mod trap fn", mod.trap_fn, "remainderer");
}

// A normal division, to prove the guard only fires on a zero divisor.
static void
test_no_trap_without_zero(Arena* heap) {
  BcProgram prog = {0};
  BcChunk   c    = chunk_new("halve", 1, 3);
  u32       two  = add_const(&c, 2);
  emit(&c, BcOp_LoadConst, 1, two, 0);
  emit(&c, BcOp_Div, 2, 0, 1);
  emit(&c, BcOp_Return, 2, 0, 0);
  program_add_chunk(&prog, c);

  i64      arg = 84;
  BcResult r   = bc_run_in_program(&prog, 0, &arg, 1, heap, NULL);
  expect_true("plain divide does not trap", !r.trapped);
  expect_true("plain divide has a value", r.has_value);
  expect_eq_i64("plain divide value", r.value, 42);
  expect_eq_i64("no-trap message is empty", (i64)r.trap_message.size, 0);
}

// A trap several BcOp_Call frames deep names the INNERMOST function -- the
// one the divisor actually went to zero in, not the entry point. `chunk` in
// bcvm.c's dispatch loop is the executing frame, which is what makes this
// hold; reading the entry chunk instead would name "outer" for all three.
static void
test_trap_names_innermost_frame(Arena* heap) {
  BcProgram prog = {0};
  program_add_chunk(&prog, chunk_divide_by_zero("inner", BcOp_Div)); // chunk 0

  BcChunk mid = chunk_new("middle", 1, 3); // chunk 1: `(inner x)`
  emit(&mid, BcOp_Move, 1, 0, 0);          // arg block for the callee starts at r1
  emit(&mid, BcOp_Call, 2, 0, 1);
  emit(&mid, BcOp_Return, 2, 0, 0);
  program_add_chunk(&prog, mid);

  BcChunk outer = chunk_new("outer", 0, 3); // chunk 2: `(middle 10)`
  u32     ten   = add_const(&outer, 10);
  emit(&outer, BcOp_LoadConst, 0, ten, 0);
  emit(&outer, BcOp_Call, 1, 1, 0);
  emit(&outer, BcOp_Return, 1, 0, 0);
  program_add_chunk(&prog, outer);

  BcResult r = bc_run_in_program(&prog, 2, NULL, 0, heap, NULL);
  expect_true("nested trap propagates through BcOp_Call frames", r.trapped);
  expect_eq_str("nested trap names innermost fn", r.trap_fn, "inner");
}

// THE REGRESSION CASE. A trap inside an imported module has to keep unwinding
// out through BcOp_CallModule, which runs a genuinely separate BcProgram via a
// nested bc_run_in_program (the one place this VM recurses in C). Storing that
// nested call's `value` on a trap turned "this module divided by zero" into
// "this module returned 0", and the importing function ran on to completion.
//
// `kept_running` is a global the caller stores 1 into AFTER the module call:
// still 0 afterwards is the proof that the caller really stopped, as opposed
// to merely reporting a trap it then ignored.
static void
test_trap_propagates_out_of_module_call(Arena* heap) {
  BcProgram module = {0};
  program_add_chunk(&module, chunk_divide_by_zero("module-fn", BcOp_Div));

  BcModuleTable table = {0};
  bc_module_table_add(&table, ctx_perm(), str8_lit("m/module-fn"), &module, 0);

  BcProgram caller = {0};
  caller.module_table = &table;
  dyn_push(ctx_perm(), caller.globals, (i64)0); // globals[0] = `kept_running`

  BcChunk c   = chunk_new("importer", 0, 4);
  u32     ten = add_const(&c, 10);
  u32     one = add_const(&c, 1);
  emit(&c, BcOp_LoadConst, 0, ten, 0);
  emit(&c, BcOp_CallModule, 1, 0, 0); // r1 = m/module-fn(r0)
  emit(&c, BcOp_LoadConst, 2, one, 0);
  emit(&c, BcOp_StoreGlobal, 0, 2, 0); // kept_running = 1 -- must never execute
  emit(&c, BcOp_Return, 1, 0, 0);
  program_add_chunk(&caller, c);

  BcResult r = bc_run_in_program(&caller, 0, NULL, 0, heap, NULL);
  expect_true("module trap reaches the importing program", r.trapped);
  expect_eq_str("module trap names the module's own fn", r.trap_fn, "module-fn");
  expect_eq_str("module trap keeps its message", r.trap_message, "division by zero");
  expect_eq_i64("importer stopped at the trapping call", caller.globals[0], 0);
}

// The same wiring WITHOUT a trap, so the fix can't have been "stop returning
// module call results at all": a normal cross-module call still delivers its
// value and the caller still runs on past it.
static void
test_module_call_still_works(Arena* heap) {
  BcProgram module = {0};
  BcChunk   m      = chunk_new("double", 1, 3);
  u32       two    = add_const(&m, 2);
  emit(&m, BcOp_LoadConst, 1, two, 0);
  emit(&m, BcOp_Mul, 2, 0, 1);
  emit(&m, BcOp_Return, 2, 0, 0);
  program_add_chunk(&module, m);

  BcModuleTable table = {0};
  bc_module_table_add(&table, ctx_perm(), str8_lit("m/double"), &module, 0);

  BcProgram caller = {0};
  caller.module_table = &table;
  dyn_push(ctx_perm(), caller.globals, (i64)0);

  BcChunk c   = chunk_new("importer", 0, 4);
  u32     ten = add_const(&c, 10);
  u32     one = add_const(&c, 1);
  emit(&c, BcOp_LoadConst, 0, ten, 0);
  emit(&c, BcOp_CallModule, 1, 0, 0);
  emit(&c, BcOp_LoadConst, 2, one, 0);
  emit(&c, BcOp_StoreGlobal, 0, 2, 0);
  emit(&c, BcOp_Return, 1, 0, 0);
  program_add_chunk(&caller, c);

  BcResult r = bc_run_in_program(&caller, 0, NULL, 0, heap, NULL);
  expect_true("untrapped module call does not trap", !r.trapped);
  expect_eq_i64("untrapped module call value", r.value, 20);
  expect_eq_i64("importer ran past an untrapped module call", caller.globals[0], 1);
}

int
main(void) {
  Context ctx;
  ctx_init(&ctx, MB(16));
  Arena* heap = ctx_perm();

  test_direct_trap(heap);
  test_no_trap_without_zero(heap);
  test_trap_names_innermost_frame(heap);
  test_trap_propagates_out_of_module_call(heap);
  test_module_call_still_works(heap);

  if (g_failures == 0) printf("bcvm_trap_test: all checks passed\n");
  else                 fprintf(stderr, "bcvm_trap_test: %d check(s) failed\n", g_failures);

  ctx_free();
  return g_failures == 0 ? 0 : 1;
}
