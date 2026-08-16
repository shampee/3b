// struct_cycle_test.c -- validates check_struct_cycles (checker.c): a struct
// that contains itself by value has no finite size and no valid layout, and
// must be rejected with a diagnostic rather than recursed on forever.
//
// This is a regression test for a real crash, not a hypothetical. Before the
// check existed, `(struct A [x A])` segfaulted every consumer of the typed
// AST -- `3b <dir>` (via codegen's synthesized comparators), `3b run x.3bs`
// (via bcgen's), and `3b-lsp` (via the checker's own comparability query),
// which is the worst of the three: it is reachable by deleting one `*` from
// a `(struct Node [next Node*])` mid-edit, and it killed the language server.
//
// So the assertion that matters most here is the one the harness makes just
// by finishing: every case below TERMINATES. A regression shows up as a hang
// or a SIGSEGV from stack exhaustion, not as a failed comparison.
//
// Same rig as layout_test.c -- hand-rolled parse -> lower -> check against an
// in-memory fixture, one Checker per case, no imports, no files on disk.
// Diagnostics are captured as data (diag_capture_begin) rather than printed,
// so an expected error doesn't look like a test failure in the output.
#include "3b.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;

// Runs one fixture through parse -> lower -> check, returning the diagnostics
// check_program produced. A parse or lower failure is fatal: every fixture
// here is syntactically valid on purpose, so that means the FIXTURE is wrong,
// which would otherwise masquerade as the checker correctly rejecting a cycle.
static Diagnostic*
check_source(const char* name, const char* source, u64* out_count) {
  Ast ast;
  ast_init(&ast, ctx_perm());

  String8 src     = str8_cstring((char*)source);
  u32     file_id = source_file_register(str8_cstring((char*)name), src);

  Parser p;
  parser_init(&p, src, &ast, file_id);
  NodeIndex root = parse_program(&p);
  if (p.had_error) { fprintf(stderr, "FATAL: fixture `%s` failed to parse\n", name); exit(1); }

  // Drop the leading `(package ...)` form, as layout_test.c does -- there are
  // no imports here, so that is all compile_package's own stripping amounts to.
  u16        form_count;
  NodeIndex* forms = ast_seq_children(&ast, root, &form_count);
  Token synth_open = {0};
  synth_open.line  = 1;
  synth_open.col   = 1;
  NodeIndex combined_root = ast_push_seq(&ast, AstNodeKind_List, synth_open, forms + 1, (u16)(form_count - 1));

  TypedAst tast;
  typed_ast_init(&tast, ctx_perm());
  Lowerer low = {0};
  low.ast  = &ast;
  low.tast = &tast;
  TypedIndex own_root = lower_program(&low, combined_root);
  if (low.had_error) { fprintf(stderr, "FATAL: fixture `%s` failed to lower\n", name); exit(1); }

  diag_capture_begin(/*also_print=*/false);
  check_program(&tast, own_root, /*is_root_package=*/true, /*scope_query=*/NULL);
  return diag_capture_end(out_count);
}

static b32
any_diag_contains(Diagnostic* diags, u64 count, const char* needle) {
  foreach_index(i, count) {
    if (strstr((const char*)diags[i].message.str, needle) != NULL) return true;
  }
  return false;
}

// A fixture that must be rejected, with a diagnostic naming the cycle.
static void
expect_cycle_error(const char* what, const char* source) {
  u64         count = 0;
  Diagnostic* diags = check_source(what, source, &count);
  if (count == 0) {
    fprintf(stderr, "FAIL %s: expected a diagnostic, got none\n", what);
    g_failures += 1;
    return;
  }
  if (!any_diag_contains(diags, count, "contains itself by value")) {
    fprintf(stderr, "FAIL %s: no `contains itself by value` diagnostic; got:\n", what);
    foreach_index(i, count) fprintf(stderr, "    %.*s\n", str8_varg(diags[i].message));
    g_failures += 1;
  }
}

// A fixture that must be accepted. Guards the other direction: a check that
// rejects legal recursion (through a pointer) or legal by-value nesting would
// be worse than the crash it replaced.
static void
expect_accepted(const char* what, const char* source) {
  u64         count = 0;
  Diagnostic* diags = check_source(what, source, &count);
  if (count != 0) {
    fprintf(stderr, "FAIL %s: expected no diagnostics, got %llu:\n", what, (unsigned long long)count);
    foreach_index(i, count) fprintf(stderr, "    %.*s\n", str8_varg(diags[i].message));
    g_failures += 1;
  }
}

int
main(void) {
  Context ctx;
  ctx_init(&ctx, MB(64));

  // ~~ Rejected: every shape that closes a by-value cycle.

  expect_cycle_error("direct self-reference",
    "(package t)\n"
    "(struct A [x A])\n"
    "(fn main [] i32 0)\n");

  // The crash's most likely real-world origin: a correct self-referencing
  // struct with the `*` removed.
  expect_cycle_error("self-reference among other fields",
    "(package t)\n"
    "(struct Node [value i32 next Node tag i32])\n"
    "(fn main [] i32 0)\n");

  expect_cycle_error("mutual recursion",
    "(package t)\n"
    "(struct A [x B])\n"
    "(struct B [y A])\n"
    "(fn main [] i32 0)\n");

  expect_cycle_error("three-struct cycle",
    "(package t)\n"
    "(struct A [x B])\n"
    "(struct B [y C])\n"
    "(struct C [z A])\n"
    "(fn main [] i32 0)\n");

  // An array does not break the cycle the way a pointer does: `[A 4]` is four
  // copies of A laid out inline, so its size depends on A's just as directly.
  expect_cycle_error("self-reference through a fixed-size array",
    "(package t)\n"
    "(struct A [x [A 4]])\n"
    "(fn main [] i32 0)\n");

  // The path that actually crashed the LSP: reaching the cycle through the
  // checker's struct-comparability query rather than through layout.
  expect_cycle_error("cyclic struct used in a comparison",
    "(package t)\n"
    "(struct A [x A])\n"
    "(fn main [] i32\n"
    "  (var p A (zero A))\n"
    "  (var q A (zero A))\n"
    "  (if (= p q) 1 0))\n");

  // Functions declared before structs: check_struct_cycles runs as a pre-pass
  // precisely so a function body cannot reach the recursion first.
  expect_cycle_error("function declared before the cyclic struct",
    "(package t)\n"
    "(fn use [a A] i32 (. a x))\n"
    "(struct A [x A])\n"
    "(fn main [] i32 0)\n");

  // ~~ Accepted: legal recursion, and by-value nesting deep enough to prove
  // the walk's depth bound doesn't cut a valid chain short.

  expect_accepted("self-reference through a pointer",
    "(package t)\n"
    "(struct Node [value i32 next Node*])\n"
    "(fn main [] i32 0)\n");

  expect_accepted("mutual recursion through pointers",
    "(package t)\n"
    "(struct A [x B*])\n"
    "(struct B [y A*])\n"
    "(fn main [] i32 0)\n");

  expect_accepted("acyclic by-value nesting",
    "(package t)\n"
    "(struct C [n i32])\n"
    "(struct B [c C])\n"
    "(struct A [b B c C])\n"
    "(fn main [] i32 0)\n");

  expect_accepted("acyclic by-value nesting through an array",
    "(package t)\n"
    "(struct C [n i32])\n"
    "(struct B [cs [C 4]])\n"
    "(struct A [bs [B 2]])\n"
    "(fn main [] i32 0)\n");

  // One chain longer than the walk's `struct_count` depth bound would be if it
  // were counted per-field rather than per-struct -- a diamond, where A reaches
  // D by two distinct paths. Legal, and must stay legal.
  expect_accepted("diamond-shaped by-value nesting",
    "(package t)\n"
    "(struct D [n i32])\n"
    "(struct B [d D])\n"
    "(struct C [d D])\n"
    "(struct A [b B c C])\n"
    "(fn main [] i32 0)\n");

  ctx_free();
  if (g_failures > 0) {
    fprintf(stderr, "struct_cycle_test: %d failure(s)\n", g_failures);
    return 1;
  }
  printf("struct_cycle_test: all checks passed\n");
  return 0;
}
