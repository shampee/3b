// bcgen_addr_taken_test.c -- validates `(addr x)`/`&x` on a bare SCALAR
// local (var/val/let/param) -- previously a documented, honest gap
// (bc_unsupported: "this backend keeps scalar locals in VM registers, not
// addressable memory"), found blocking examples/structs/main.3b
// (`(addr counter)` on a plain `var counter i32`).
//
// Implemented via ADDRESS-TAKEN ANALYSIS: bc_scan_address_taken_names
// (bcgen.c) does ONE flat, whole-program pass over every typed node,
// collecting every name that appears as `(addr name)`/`&name`'s
// Identifier operand -- deliberately a simple linear scan over `tast->
// nodes` (every typed node in the program, flat, addressable by
// TypedIndex) rather than a real recursive tree walk, and deliberately
// NAME-based (not per-declaration-site), so it can't miss an occurrence
// buried in some node-kind-shape the scan doesn't need to understand.
// bc_bind_local_typed then gives an otherwise-scalar local (var/val/let/
// param -- NOT a for-loop variable or a module-level global, a real,
// documented, narrower scope cut than the general mechanism could
// support) a REAL backing memory slot (Alloc + store) ONLY if its name
// is in that set -- an ordinary, never-`&`-taken scalar local still
// lives purely in a register, zero overhead. Every read/write of such a
// local (BcLocal.is_addr_taken) then goes through a real load/store
// instead of a direct register reference.
//
// Same rig as the other bcgen_*_test.c files.
//
// Exercises:
//  - `(addr x)` on a `var` param, passed to a function that mutates
//    through the pointer TWICE -- proves the caller's own local is
//    genuinely aliased, not a one-shot snapshot.
//  - The SAME local read as an ordinary VALUE (not through `addr`)
//    elsewhere in the SAME function -- proves the read path correctly
//    loads THROUGH the backing slot instead of treating the register as
//    the value directly.
//  - An address-taken local declared INSIDE a loop body (a fresh backing
//    slot conceptually re-bound each iteration, at the SAME register --
//    proves this composes correctly with bc_compile_block's own per-
//    iteration compile-once/run-many-times shape).
//  - `set` on an address-taken local (SetTargetKind_Identifier) --
//    writes through the slot, not a register rebind.
//  - `swap` between two address-taken locals -- both bc_compile_
//    lvalue_write's Identifier case (used by swap) and the ordinary
//    read path must agree on "this is an address," not just one of them.
//  - A local that's NEVER `&`-taken, in the SAME program as ones that
//    are -- proves the address-taken treatment is genuinely per-name/
//    per-need, not accidentally applied to everything once the feature
//    is used anywhere in a function.
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

static const char* g_fixture_source =
  "(package bcgen_addr_taken_test)\n"
  "\n"
  "(fn bump [p i32*] void (set (deref p) (+ (deref p) 1)))\n"
  "\n"
  "(fn twice-bump-param [x i32] i32\n"
  "  (bump (addr x))\n"
  "  (bump (addr x))\n"
  "  x)\n"
  "\n"
  "(fn addr-and-plain-read [] i32\n"
  "  (var x i32 10)\n"
  "  (bump (addr x))\n"
  "  (+ x x))\n" // plain read (not through addr) -- must see the mutated value, doubled
  "\n"
  "(fn addr-taken-in-loop [] i32\n"
  "  (var total i32 0)\n"
  "  (for [i 0 3]\n"
  "    (var local i32 (* i 10))\n"
  "    (bump (addr local))\n"
  "    (set total (+ total local)))\n"
  "  total)\n" // expect 1+11+21 = 33
  "\n"
  "(fn set-addr-taken [] i32\n"
  "  (var x i32 1)\n"
  "  (val p i32* (addr x))\n"
  "  (set x 99)\n" // ordinary `set` on the address-taken local itself
  "  (deref p))\n" // must observe 99 through the pointer
  "\n"
  "(fn swap-addr-taken [] i32\n"
  "  (var a i32 1)\n"
  "  (var b i32 2)\n"
  "  (val pa i32* (addr a))\n"
  "  (val pb i32* (addr b))\n"
  "  (swap a b)\n"
  "  (+ (* (deref pa) 100) (deref pb)))\n" // pa/pb must see the POST-swap values: a=2,b=1 -> 201
  "\n"
  "(fn never-addr-taken [y i32] i32 (* y 3))\n"
  "\n"
  "(fn mixed [] i32\n"
  "  (+ (twice-bump-param 5) (never-addr-taken 5)))\n" // 7 + 15 = 22
  "\n"
  // An address-taken param that is NOT the last one. Every case above has
  // a single param, so boxing could only ever claim a register past the
  // end of the incoming-argument block; here it would claim `rest`'s.
  "(fn addr-first-of-three [first i32 second i32 rest i32] i32\n"
  "  (bump (addr first))\n"
  "  (+ (* first 100) (+ (* second 10) rest)))\n" // 6,2,3 -> 623
  "\n"
  // The same hazard reached WITHOUT this function taking any address at
  // all: `x` is address-taken over in twice-bump-param, and the scan that
  // drives boxing is whole-program BY NAME, so this function's own `x`
  // gets a backing slot purely because of that collision. `tail` must
  // still read the value the caller actually passed.
  "(fn name-collision-with-x [x i32 tail i32] i32\n"
  "  (+ (* x 10) tail))\n" // 4,7 -> 47
  "\n"
  "(fn main [] i32 0)\n";

static Checker
check_fixture(TypedAst* tast, TypedIndex* out_root) {
  Ast ast;
  ast_init(&ast, ctx_perm());

  String8 src     = str8_cstring((char*)(g_fixture_source));
  u32     file_id = source_file_register(str8_lit("bcgen_addr_taken_test_fixture.3b"), src);

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

  u32 twice_bump_fn        = bc_program_find_fn(&prog, str8_lit("twice-bump-param"));
  u32 addr_plain_read_fn     = bc_program_find_fn(&prog, str8_lit("addr-and-plain-read"));
  u32 addr_in_loop_fn          = bc_program_find_fn(&prog, str8_lit("addr-taken-in-loop"));
  u32 set_addr_taken_fn          = bc_program_find_fn(&prog, str8_lit("set-addr-taken"));
  u32 swap_addr_taken_fn           = bc_program_find_fn(&prog, str8_lit("swap-addr-taken"));
  u32 mixed_fn                       = bc_program_find_fn(&prog, str8_lit("mixed"));
  u32 addr_first_of_three_fn           = bc_program_find_fn(&prog, str8_lit("addr-first-of-three"));
  u32 name_collision_fn                  = bc_program_find_fn(&prog, str8_lit("name-collision-with-x"));

  Arena* heap = ctx_perm();

  { i64 args[1] = {5};
    expect_eq_i64("twice-bump-param(5)", bc_run_in_program(&prog, twice_bump_fn, args, 1, heap, &host_imports).value, 7); }
  expect_eq_i64("addr-and-plain-read()", bc_run_in_program(&prog, addr_plain_read_fn, NULL, 0, heap, &host_imports).value, 22);
  expect_eq_i64("addr-taken-in-loop()",  bc_run_in_program(&prog, addr_in_loop_fn,     NULL, 0, heap, &host_imports).value, 33);
  expect_eq_i64("set-addr-taken()",      bc_run_in_program(&prog, set_addr_taken_fn,   NULL, 0, heap, &host_imports).value, 99);
  expect_eq_i64("swap-addr-taken()",     bc_run_in_program(&prog, swap_addr_taken_fn,  NULL, 0, heap, &host_imports).value, 201);
  expect_eq_i64("mixed()",               bc_run_in_program(&prog, mixed_fn,            NULL, 0, heap, &host_imports).value, 22);

  { i64 args[3] = {5, 2, 3}; // `first` is bumped to 6 -> 600 + 20 + 3
    expect_eq_i64("addr-first-of-three(5, 2, 3)",
                  bc_run_in_program(&prog, addr_first_of_three_fn, args, 3, heap, &host_imports).value, 623); }
  { i64 args[2] = {4, 7};
    expect_eq_i64("name-collision-with-x(4, 7)",
                  bc_run_in_program(&prog, name_collision_fn, args, 2, heap, &host_imports).value, 47); }

  arena_temp_end(&fn_temp);

  if (g_failures == 0) printf("bcgen_addr_taken_test: all checks passed\n");
  else                 printf("bcgen_addr_taken_test: %d check(s) FAILED\n", g_failures);

  ctx_free();
  return g_failures == 0 ? 0 : 1;
}
