// bcgen_unsupported_test.c -- the bytecode compiler's NEGATIVE tests:
// checker-VALID sources bcgen.c cannot compile yet, each pinned to the
// diagnostic it must be rejected with.
//
// The other bcgen_*_test.c files all assert on programs that compile and
// run. Nothing covered the other half of bcgen.c's contract -- the
// SCRIPT-AUTHOR DIAGNOSTICS vs. INTERNAL INVARIANTS split its own header
// comment draws. That split is a real correctness property, not a nicety:
// a gap reachable from source must reach bc_unsupported (a file:line:col
// diagnostic plus `prog.ok == false`), while only conditions the checker
// truly guarantees may sit behind an xassert.
//
// Getting it backwards is how `(val f (fn [x i32] i32) some-fn)` came to
// ABORT THE PROCESS on the VM -- through an xassert claiming "the checker
// should already have caught this", which the checker demonstrably does
// not, since its own Identifier case resolves a function used as a value
// on purpose. In `3b run` that is a confusing crash; in a hot-reload host
// embedding the VM (script_load, see script.h) a single bad gameplay
// script takes the whole game process down.
//
// So each case here asserts BOTH halves: the compile is rejected rather
// than silently miscompiled, AND it is rejected by the diagnostic path.
// The second half is enforced structurally -- an xassert would abort this
// test binary, which the Makefile reports as a failure.
//
// The ACCEPT table is not decoration: a rule that rejects everything
// satisfies a negative test on its own. Each accepted source is the
// nearest thing that MUST still compile -- calling a function directly,
// next to failing to use one as a value.
//
// When a gap here closes, its case moves from REJECT to ACCEPT rather
// than being deleted, so the file records what the backend grew into.
#include "3b.h"
#include "bcgen.h"
#include "bcvm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;
static int g_checks   = 0;

enum { BCU_ARENA = MB(32) };

typedef struct CompileOutcome {
  b32  checker_errored; // the fixture was not checker-valid -- a broken test, not a real result
  b32  prog_ok;
  u64  diag_count;
  char messages[8][512];
  u64  kept;
} CompileOutcome;

// parse -> lower -> check -> bc_compile_program, with diagnostics captured
// as data. Each case gets its own Context so a longjmp out of
// bc_compile_program can't leak state into the next one.
static CompileOutcome
compile_source(const char* src_cstr, const char* name) {
  CompileOutcome out = {0};

  Context ctx;
  ctx_init(&ctx, BCU_ARENA);
  source_registry_reset();

  String8 src     = str8_cstring((char*)src_cstr);
  u32     file_id = source_file_register(str8_cstring((char*)name), src);

  Ast ast;
  ast_init(&ast, ctx_perm());
  Parser p;
  parser_init(&p, src, &ast, file_id);
  NodeIndex root = parse_program(&p);
  if (p.had_error) { fprintf(stderr, "FATAL: %s failed to parse\n", name); exit(1); }

  // Strip the leading `(package ...)` form, as compile_package does.
  u16        form_count;
  NodeIndex* forms      = ast_seq_children(&ast, root, &form_count);
  Token      synth_open = {0};
  synth_open.line       = 1;
  synth_open.col        = 1;
  NodeIndex body = ast_push_seq(&ast, AstNodeKind_List, synth_open, forms + 1, (u16)(form_count - 1));

  TypedAst tast;
  typed_ast_init(&tast, ctx_perm());
  Lowerer low = {0};
  low.ast     = &ast;
  low.tast    = &tast;
  TypedIndex own_root = lower_program(&low, body);
  if (low.had_error) { fprintf(stderr, "FATAL: %s failed to lower\n", name); exit(1); }

  Checker ck = check_program(&tast, own_root, /*is_root_package=*/true, /*scope_query=*/NULL);
  if (ck.had_error) {
    // Every fixture here must be checker-VALID: a case the checker rejects
    // proves nothing about bcgen, and would quietly turn into a test that
    // passes for the wrong reason.
    out.checker_errored = true;
    ctx_free();
    return out;
  }

  // Capture only around bcgen, so a case can't be satisfied by some earlier
  // stage's diagnostic.
  diag_capture_begin(/*also_print=*/false);

  ArenaTemp   fn_temp = arena_temp_begin(ctx_scratch());
  LayoutCache layout_cache;
  layout_cache_init(&layout_cache, ctx_perm());
  BcHostImportTable host_imports = {0};
  BcProgram prog = bc_compile_program(&ck, &tast, own_root, fn_temp.arena, ctx_perm(),
                                       &layout_cache, &host_imports, NULL);
  out.prog_ok = prog.ok;
  arena_temp_end(&fn_temp);

  u64         n     = 0;
  Diagnostic* diags = diag_capture_end(&n);
  out.diag_count    = n;
  for (u64 i = 0; i < n && i < 8; i += 1) {
    u64 len = diags[i].message.size;
    if (len > sizeof(out.messages[0]) - 1) len = sizeof(out.messages[0]) - 1;
    memcpy(out.messages[i], diags[i].message.str, len);
    out.messages[i][len] = 0;
    out.kept += 1;
  }

  ctx_free();
  return out;
}

typedef struct RejectCase {
  const char* name;
  const char* src;
  const char* expect_msg; // substring of the diagnostic that must fire
} RejectCase;

static const RejectCase g_reject[] = {
  // ~~ A function used as a VALUE rather than called. The checker resolves
  // these deliberately (checker.c's Identifier case falls back to
  // fn_table_lookup) and codegen.c compiles them to plain C function
  // pointers, so every one is checker-valid 3b and a real backend
  // asymmetry -- exactly the shape that must be a diagnostic, never an
  // assert. Three positions, because they reach bcgen by different paths.
  { "fn-value bound to a local",
    "(package t)\n"
    "(fn double [x i32] i32 (* x 2))\n"
    "(fn main [] i32 (val f (fn [x i32] i32) double) 0)\n",
    "a function used as a VALUE rather than called" },

  { "fn-value stored in a struct field",
    "(package t)\n"
    "(struct Ops [step (fn [x i32] i32)])\n"
    "(fn double [x i32] i32 (* x 2))\n"
    "(fn main [] i32 (val o Ops (Ops {:step double})) 0)\n",
    "a function used as a VALUE rather than called" },

  { "fn-value passed as a call argument",
    "(package t)\n"
    "(fn double [x i32] i32 (* x 2))\n"
    "(fn takes-fn [f (fn [x i32] i32)] i32 0)\n"
    "(fn main [] i32 (takes-fn double))\n",
    "a function used as a VALUE rather than called" },

  // The CONSUMING half of the same gap. Worth its own case because it used
  // to report "this program has no compiled function or registered host
  // import by that name" -- false, and actively misleading, with `f` bound
  // as a parameter right there on the same line.
  { "indirect call through a fn-valued parameter",
    "(package t)\n"
    "(fn apply-it [f (fn [x i32] i32) v i32] i32 (f v))\n"
    "(fn main [] i32 0)\n",
    "an indirect call through a function-valued local or parameter" },

  // A neighbour from bcgen.c's own NOT SUPPORTED list, pinned so that list
  // stays honest -- it has drifted before.
  { "array element from a non-literal",
    "(package t)\n"
    "(fn main [] i32 (val s string \"x\") (val arr [string 2] [s s]) 0)\n",
    "an array element of an embedded type" },
};

// Sources that must still COMPILE -- the nearest neighbour of each rejection
// above, so none of these rules can be satisfied by rejecting everything.
static const RejectCase g_accept[] = {
  { "an ordinary direct call",
    "(package t)\n"
    "(fn double [x i32] i32 (* x 2))\n"
    "(fn main [] i32 (double 21))\n",
    NULL },

  { "an ArenaMark in a struct field",  // was rejected until the load/store
    "(package t)\n"                     // ops learned TypeKind_ArenaMark
    "(struct Saved [m ArenaMark])\n"
    "(fn main [] i32\n"
    "  (val a arena (create))\n"
    "  (val s Saved (Saved {:m (mark a)}))\n"
    "  (pop a (get s m))\n"
    "  0)\n",
    NULL },

  { "an array element from an inline literal",
    "(package t)\n"
    "(fn main [] i32 (val arr [string 2] [\"a\" \"b\"]) 0)\n",
    NULL },

  // Was rejected outright until the one-lane serial fallback landed (see
  // bcgen.c's LANES note). It compiles and RUNS now -- with lane-count 1 --
  // which is a legal lane configuration, not a stub: the native backend
  // produces exactly this one on a single-core machine.
  { "a `parallel` block with `parallel-for` and the lane builtins",
    "(package t)\n"
    "(fn main [] i32\n"
    "  (val n u32 4u32)\n"
    "  (parallel [total n]\n"
    "    (parallel-for [i total] (void (lane-index)))\n"
    "    (lane-sync)\n"
    "    (void (push (lane-arena) i32)))\n"
    "  (cast i32 (lane-count)))\n",
    NULL },
};

static void
run_reject(const RejectCase* c) {
  g_checks += 1;
  CompileOutcome o = compile_source(c->src, c->name);

  if (o.checker_errored) {
    fprintf(stderr, "FAIL %s: fixture is not checker-valid, so it proves nothing about bcgen\n", c->name);
    g_failures += 1;
    return;
  }
  if (o.prog_ok) {
    fprintf(stderr, "FAIL %s: bc_compile_program returned ok -- expected rejection with \"%s\"\n",
            c->name, c->expect_msg);
    g_failures += 1;
    return;
  }
  if (o.diag_count == 0) {
    fprintf(stderr, "FAIL %s: rejected with NO diagnostic -- a silent failure is the bug this "
                     "file exists to catch\n", c->name);
    g_failures += 1;
    return;
  }
  for (u64 i = 0; i < o.kept; i += 1) {
    if (strstr(o.messages[i], c->expect_msg) != NULL) return; // matched
  }
  fprintf(stderr, "FAIL %s: no diagnostic contained \"%s\". Got:\n", c->name, c->expect_msg);
  for (u64 i = 0; i < o.kept; i += 1) fprintf(stderr, "    %s\n", o.messages[i]);
  g_failures += 1;
}

static void
run_accept(const RejectCase* c) {
  g_checks += 1;
  CompileOutcome o = compile_source(c->src, c->name);

  if (o.checker_errored) {
    fprintf(stderr, "FAIL accept %s: fixture is not checker-valid\n", c->name);
    g_failures += 1;
    return;
  }
  if (!o.prog_ok) {
    fprintf(stderr, "FAIL accept %s: bc_compile_program rejected a source that must compile. Got:\n", c->name);
    for (u64 i = 0; i < o.kept; i += 1) fprintf(stderr, "    %s\n", o.messages[i]);
    g_failures += 1;
  }
}

int
main(void) {
  for (u64 i = 0; i < ArrayCount(g_reject); i += 1) run_reject(&g_reject[i]);
  for (u64 i = 0; i < ArrayCount(g_accept); i += 1) run_accept(&g_accept[i]);

  if (g_failures == 0) {
    printf("bcgen_unsupported_test: all %d checks passed (%d rejected as documented, %d still compile)\n",
           g_checks, (int)ArrayCount(g_reject), (int)ArrayCount(g_accept));
    return 0;
  }
  fprintf(stderr, "bcgen_unsupported_test: %d of %d checks FAILED\n", g_failures, g_checks);
  return 1;
}
