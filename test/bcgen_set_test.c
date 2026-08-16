// bcgen_set_test.c -- validates `set`/mutation support in bcgen.c
// (TypedNodeKind_SetExpr's four SetTargetKinds, plus the `var`/`val`
// function-body locals and `deref` that make mutation meaningful). Same
// rig as the other bcgen_*_test.c files.
//
// Exercises:
//  - SetTargetKind_Identifier on a function PARAMETER (params are mutable
//    by default in 3b -- confirmed via checker.c's own scope_bind, which
//    defaults is_mutable to true for fn params, not just `var` locals).
//  - `var` locals (TypedNodeKind_VarDecl as a function-body statement) +
//    repeated `set` on one, an accumulator pattern.
//  - SetTargetKind_Deref through a REAL pointer to a C-owned i32 -- the
//    test supplies the address of its own local variable directly (no 3b-
//    level `addr` support needed), then reads that same C variable back
//    afterward to prove the mutation reached actual memory, not just the
//    returned value.
//  - SetTargetKind_Field, on a Player passed BY VALUE and mutated through
//    `(set (get p health) ...)`. Two DIFFERENT boundaries are checked here,
//    deliberately kept distinct:
//     * Calling `bump-health` directly as the VM ENTRY point (raw i64 args
//       supplied by this C harness) still ALIASES the caller's memory --
//       intentional, not a gap: bc_run_in_program's own `args` are the
//       C-embedding boundary, standing in for values a real native caller
//       already owns, same convention increment-via-pointer's raw pointer
//       arg already relies on. Copy-on-call only exists at a COMPILED call
//       site (TypedNodeKind_Call), which this boundary never goes through.
//     * `bump-health-preserves-original` calls `make-player` then
//       `bump-health` FROM WITHIN compiled bytecode -- a real
//       TypedNodeKind_Call site -- and confirms the CALLER's `original` is
//       UNAFFECTED, proving copy-on-call itself (see bcgen.c's
//       bc_compile_value_copy). This used to be a documented, tested gap
//       (see the project memory's "Known follow-on risk" note).
//  - Whole-ARRAY `set` through SetTargetKind_Field (bc_store_value's
//    previously-asserting TypeKind_Array branch), on a struct field --
//    `replace-verts-preserves-original` mirrors bump-health-preserves-
//    original's shape (an in-bytecode caller + copy-on-call at the Triple
//    argument) to prove the same thing for a nested array field.
//  - A `let`-bound copy of an existing by-value struct PARAMETER: mutating
//    a field through the `let` local must NOT reach the parameter's own
//    storage -- exercises TypedNodeKind_LetExpr's own copy-on-bind fix
//    specifically (decoupled from call-argument copy-on-call -- this one's
//    exercised straight off the raw VM-entry parameter, no wrapper needed,
//    since the `let` itself is the compiled construct under test).
#include "3b.h"
#include "bcgen.h"
#include "bcvm.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
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
  "(package bcgen_set_test)\n"
  "(fn bump-and-double [x i32] i32\n"
  "  (set x (+ x 1))\n"
  "  (set x (* x 2))\n"
  "  x)\n"
  "\n"
  "(fn accumulate [a i32 b i32 c i32] i32\n"
  "  (var total i32 0)\n"
  "  (set total (+ total a))\n"
  "  (set total (+ total b))\n"
  "  (set total (+ total c))\n"
  "  total)\n"
  "\n"
  "(fn increment-via-pointer [p i32*] i32\n"
  "  (set (deref p) (+ (deref p) 1))\n"
  "  (deref p))\n"
  "\n"
  "(struct Player [health i32])\n"
  "(fn make-player [hp i32] Player (Player {:health hp}))\n"
  "(fn bump-health [p Player amount i32] i32\n"
  "  (set (get p health) (+ (get p health) amount))\n"
  "  (get p health))\n"
  "(fn bump-health-preserves-original [hp i32 amount i32] i32\n"
  "  (let [original (make-player hp)\n"
  "        bumped   (bump-health original amount)]\n"
  "    (get original health)))\n"
  "\n"
  "(struct Triple [verts [i32 3]])\n"
  "(fn make-triple [a i32 b i32 c i32] Triple (Triple {:verts [a b c]}))\n"
  "(fn replace-verts [t Triple] i32\n"
  "  (var new-verts [i32 3] [7 8 9])\n"
  "  (set (get t verts) new-verts)\n"
  "  (+ (nth (get t verts) 0) (+ (nth (get t verts) 1) (nth (get t verts) 2))))\n"
  "(fn replace-verts-preserves-original [a i32 b i32 c i32] i32\n"
  "  (let [original (make-triple a b c)\n"
  "        replaced (replace-verts original)]\n"
  "    (nth (get original verts) 0)))\n"
  "\n"
  "(fn let-copy-independent [p Player] i32\n"
  "  (let [q p]\n"
  "    (set (get q health) 999))\n"
  "  (get p health))\n"
  "\n"
  "(fn main [] i32 0)\n";

static Checker
check_fixture(TypedAst* tast, TypedIndex* out_root) {
  Ast ast;
  ast_init(&ast, ctx_perm());

  String8 src     = str8_cstring((char*)(g_fixture_source));
  u32     file_id = source_file_register(str8_lit("bcgen_set_test_fixture.3b"), src);

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
  u32 bump_and_double_fn      = bc_program_find_fn(&prog, str8_lit("bump-and-double"));
  u32 accumulate_fn           = bc_program_find_fn(&prog, str8_lit("accumulate"));
  u32 increment_via_ptr_fn    = bc_program_find_fn(&prog, str8_lit("increment-via-pointer"));
  u32 make_player_fn          = bc_program_find_fn(&prog, str8_lit("make-player"));
  u32 bump_health_fn          = bc_program_find_fn(&prog, str8_lit("bump-health"));
  u32 bump_health_preserves_fn    = bc_program_find_fn(&prog, str8_lit("bump-health-preserves-original"));
  u32 make_triple_fn          = bc_program_find_fn(&prog, str8_lit("make-triple"));
  u32 replace_verts_fn        = bc_program_find_fn(&prog, str8_lit("replace-verts"));
  u32 replace_verts_preserves_fn  = bc_program_find_fn(&prog, str8_lit("replace-verts-preserves-original"));
  u32 let_copy_independent_fn = bc_program_find_fn(&prog, str8_lit("let-copy-independent"));

  Arena* heap = ctx_perm();

  // ~~ set on a function PARAMETER.
  { i64 args[1] = {5}; expect_eq_i64("bump-and-double(5)", bc_run_in_program(&prog, bump_and_double_fn, args, 1, heap, &host_imports).value, 12); } // (5+1)*2
  { i64 args[1] = {0}; expect_eq_i64("bump-and-double(0)", bc_run_in_program(&prog, bump_and_double_fn, args, 1, heap, &host_imports).value, 2); }  // (0+1)*2

  // ~~ `var` local + repeated set (accumulator pattern).
  { i64 args[3] = {1, 2, 3};  expect_eq_i64("accumulate(1,2,3)", bc_run_in_program(&prog, accumulate_fn, args, 3, heap, &host_imports).value, 6); }
  { i64 args[3] = {10, 0, 0}; expect_eq_i64("accumulate(10,0,0)", bc_run_in_program(&prog, accumulate_fn, args, 3, heap, &host_imports).value, 10); }

  // ~~ set through deref of a REAL pointer to C-owned memory -- verifies
  // the mutation reaches actual memory, not just the returned value.
  { i32 c_owned = 41;
    i64 args[1] = { (i64)(intptr_t)&c_owned };
    i64 result = bc_run_in_program(&prog, increment_via_ptr_fn, args, 1, heap, &host_imports).value;
    expect_eq_i64("increment-via-pointer returns 42", result, 42);
    expect_eq_i64("increment-via-pointer actually mutated the C-owned i32", (i64)c_owned, 42);
  }

  // ~~ set on a struct field, through a BY-VALUE Player parameter, called
  // directly as the VM ENTRY point -- this boundary still ALIASES the raw
  // pointer this C harness supplies (intentional, see this file's own
  // top-of-file note: no compiled call site exists here for copy-on-call
  // to trigger against).
  { i64 margs[1] = {100};
    i64 player_ptr = bc_run_in_program(&prog, make_player_fn, margs, 1, heap, &host_imports).value;

    i64 bargs[2] = {player_ptr, 25};
    i64 result = bc_run_in_program(&prog, bump_health_fn, bargs, 2, heap, &host_imports).value;
    expect_eq_i64("bump-health(make-player(100), 25) returns 125", result, 125);

    i32 health_after;
    memcpy(&health_after, (void*)(intptr_t)player_ptr, sizeof(health_after));
    expect_eq_i64("VM-entry boundary still aliases the raw pointer (expected, not a gap)",
                  (i64)health_after, 125);
  }

  // ~~ Same Player/bump-health story, but through a REAL compiled call
  // site (bump-health-preserves-original calls make-player then
  // bump-health FROM bytecode) -- this is what actually proves
  // copy-on-call: the CALLER's own `original` local is unaffected by
  // bump-health's mutation of its own copy.
  { i64 args[2] = {100, 25};
    i64 result = bc_run_in_program(&prog, bump_health_preserves_fn, args, 2, heap, &host_imports).value;
    expect_eq_i64("bump-health-preserves-original(100, 25) leaves original health at 100 (copy-on-call)",
                  result, 100);
  }

  // ~~ Whole-array `set` through a struct field (bc_store_value's
  // previously-asserting TypeKind_Array branch), through the VM ENTRY
  // boundary -- same aliasing story as bump-health above.
  { i64 margs[3] = {1, 2, 3};
    i64 triple_ptr = bc_run_in_program(&prog, make_triple_fn, margs, 3, heap, &host_imports).value;

    i64 rargs[1] = {triple_ptr};
    i64 result = bc_run_in_program(&prog, replace_verts_fn, rargs, 1, heap, &host_imports).value;
    expect_eq_i64("replace-verts(make-triple(1,2,3)) returns 7+8+9", result, 24);

    i32 verts_after[3];
    memcpy(verts_after, (void*)(intptr_t)triple_ptr, sizeof(verts_after));
    expect_eq_i64("VM-entry boundary still aliases verts[0] (expected, not a gap)", (i64)verts_after[0], 7);
    expect_eq_i64("VM-entry boundary still aliases verts[1] (expected, not a gap)", (i64)verts_after[1], 8);
    expect_eq_i64("VM-entry boundary still aliases verts[2] (expected, not a gap)", (i64)verts_after[2], 9);
  }

  // ~~ Same Triple/replace-verts story through a REAL compiled call site --
  // proves copy-on-call protects a struct whose only field is itself a
  // nested by-value array.
  { i64 args[3] = {1, 2, 3};
    i64 result = bc_run_in_program(&prog, replace_verts_preserves_fn, args, 3, heap, &host_imports).value;
    expect_eq_i64("replace-verts-preserves-original(1,2,3) leaves original verts[0] at 1 (copy-on-call)",
                  result, 1);
  }

  // ~~ A `let`-bound copy of an existing by-value struct parameter --
  // mutating the `let` local's field must not reach the parameter's own
  // storage (TypedNodeKind_LetExpr's own copy-on-bind fix, independent of
  // call-argument copy-on-call).
  { i64 margs[1] = {50};
    i64 player_ptr = bc_run_in_program(&prog, make_player_fn, margs, 1, heap, &host_imports).value;

    i64 largs[1] = {player_ptr};
    i64 result = bc_run_in_program(&prog, let_copy_independent_fn, largs, 1, heap, &host_imports).value;
    expect_eq_i64("let-copy-independent(make-player(50)) leaves param's health at 50", result, 50);
  }

  arena_temp_end(&fn_temp);

  if (g_failures == 0) printf("bcgen_set_test: all checks passed\n");
  else                 printf("bcgen_set_test: %d check(s) FAILED\n", g_failures);

  ctx_free();
  return g_failures == 0 ? 0 : 1;
}
