// bcgen_struct_test.c -- validates the struct support wired into bcgen.c/
// bcvm.c (see bcgen.c's top-of-file note on the "every struct value is a
// pointer register" representation). Same rig as test/bcgen_test.c/
// test/layout_test.c (hand-rolled parse -> lower -> check against an
// in-memory fixture).
//
// Exercises: struct construction (BcOp_Alloc + field stores), a NESTED
// by-value struct field filled inline from an embedded StructLiteral (not
// a separate allocation), and reading through that nesting (get/get-in ->
// chained TypedNodeKind_FieldAccess). Also chains two SEPARATE
// bc_run_in_program calls in C -- passing the first's returned struct
// pointer as the second's argument -- to prove a struct value survives
// past the call that produced it (the whole reason bc_run_in_program takes
// an explicit `heap` distinct from each call's own register file); AND a
// real bytecode-level call chain (total-for-new-player calling both
// make-player and player-total from WITHIN compiled bytecode) proving the
// same thing without hand-sequencing in C.
#include "3b.h"
#include "bcgen.h"
#include "bcvm.h"
#include <stdio.h>
#include <stdlib.h>

static int g_failures = 0;

static void
expect_eq_i64(const char* what, i64 got, i64 want) {
  if (got != want) {
    fprintf(stderr, "FAIL %s: got %lld, want %lld\n", what, (long long)got, (long long)want);
    g_failures += 1;
  }
}

static const char* g_fixture_source =
  "(package bcgen_struct_test)\n"
  "(struct Vector2 [x i32 y i32])\n"
  "(struct Player [health i32 pos Vector2])\n"
  "(fn make-player [hp i32 px i32 py i32] Player\n"
  "  (Player {:health hp :pos (Vector2 {:x px :y py})}))\n"
  "(fn player-total [p Player] i32\n"
  "  (+ (get p health) (+ (get-in p [pos x]) (get-in p [pos y]))))\n"
  "(fn bump-health [p Player amount i32] i32\n"
  "  (+ (get p health) amount))\n"
  "(fn total-for-new-player [hp i32 px i32 py i32] i32\n"
  "  (player-total (make-player hp px py)))\n"
  // `packed` is the only way to get a struct whose size is not a multiple of
  // 4, which is what makes bc_copy_struct_bytes decompose a tail into 2- and
  // 1-byte steps. One struct per remainder class it has to handle: 3 and 6 are
  // pure tail, 15 is one whole word plus a 4/2/1 tail.
  "(packed (struct P3  [a i16 b i8]))\n"
  "(packed (struct P6  [a i32 b i16]))\n"
  "(packed (struct P15 [a i64 b i32 c i16 d i8]))\n"
  // Each of these binds a COPY, then clobbers every field of the original.
  // A copy that stopped at the whole words would return the clobbered tail;
  // one that aliased instead of copying would return zero throughout.
  "(fn packed3-tail [a i16 b i8] i32\n"
  "  (var src P3 (P3 a b))\n"
  "  (var dst P3 src)\n"
  "  (void (set (. src a) 0i16))\n"
  "  (void (set (. src b) 0i8))\n"
  "  (+ (cast i32 (. dst a)) (cast i32 (. dst b))))\n"
  "(fn packed6-tail [a i32 b i16] i32\n"
  "  (var src P6 (P6 a b))\n"
  "  (var dst P6 src)\n"
  "  (void (set (. src a) 0))\n"
  "  (void (set (. src b) 0i16))\n"
  "  (+ (. dst a) (cast i32 (. dst b))))\n"
  "(fn packed15-tail [a i64 b i32 c i16 d i8] i64\n"
  "  (var src P15 (P15 a b c d))\n"
  "  (var dst P15 src)\n"
  "  (void (set (. src a) 0i64))\n"
  "  (void (set (. src b) 0))\n"
  "  (void (set (. src c) 0i16))\n"
  "  (void (set (. src d) 0i8))\n"
  "  (+ (. dst a) (cast i64 (+ (. dst b) (+ (cast i32 (. dst c)) (cast i32 (. dst d)))))))\n"
  "(fn main [] i32 0)\n";

static Checker
check_fixture(TypedAst* tast, TypedIndex* out_root) {
  Ast ast;
  ast_init(&ast, ctx_perm());

  String8 src     = str8_cstring((char*)(g_fixture_source));
  u32     file_id = source_file_register(str8_lit("bcgen_struct_test_fixture.3b"), src);

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
  u32 make_player_fn        = bc_program_find_fn(&prog, str8_lit("make-player"));
  u32 player_total_fn       = bc_program_find_fn(&prog, str8_lit("player-total"));
  u32 bump_health_fn        = bc_program_find_fn(&prog, str8_lit("bump-health"));
  u32 total_for_new_player_fn = bc_program_find_fn(&prog, str8_lit("total-for-new-player"));

  // A real heap, distinct from bc_run_in_program's own call-scoped
  // register files -- the Player pointer make-player returns has to
  // outlive that call so player-total/bump-health (run as separate
  // bc_run_in_program calls here) can read it afterward.
  Arena* heap = ctx_perm();

  { i64 args[3] = {100, 3, 4};
    BcResult r = bc_run_in_program(&prog, make_player_fn, args, 3, heap, &host_imports);
    i64 player_ptr = r.value;

    { i64 targs[1] = {player_ptr};
      expect_eq_i64("player-total(make-player(100,3,4))", bc_run_in_program(&prog, player_total_fn, targs, 1, heap, &host_imports).value, 107); } // 100+3+4

    { i64 bargs[2] = {player_ptr, 25};
      expect_eq_i64("bump-health(make-player(100,3,4), 25)", bc_run_in_program(&prog, bump_health_fn, bargs, 2, heap, &host_imports).value, 125); }
  }

  { i64 args[3] = {0, -5, 10};
    BcResult r = bc_run_in_program(&prog, make_player_fn, args, 3, heap, &host_imports);
    i64 player_ptr = r.value;
    i64 targs[1] = {player_ptr};
    expect_eq_i64("player-total(make-player(0,-5,10))", bc_run_in_program(&prog, player_total_fn, targs, 1, heap, &host_imports).value, 5); // 0+(-5)+10
  }

  // Same computation as the first block above, but with make-player AND
  // player-total called from WITHIN compiled bytecode (a real
  // TypedNodeKind_Call, not hand-sequenced C) -- proving a struct value
  // returned by one compiled function can be passed directly into another
  // as a bytecode-level call argument.
  { i64 args[3] = {100, 3, 4};
    expect_eq_i64("total-for-new-player(100,3,4)", bc_run_in_program(&prog, total_for_new_player_fn, args, 3, heap, &host_imports).value, 107);
  }

  // A packed struct's by-value copy, whose byte count is not a multiple of 4.
  // The 4-byte-remainder case was the only tail bc_copy_struct_bytes handled;
  // anything else hit an xassert that aborted `3b run` outright with XDEBUG on,
  // and silently copied only the whole words without it.
  { u32 packed3_fn  = bc_program_find_fn(&prog, str8_lit("packed3-tail"));
    u32 packed6_fn  = bc_program_find_fn(&prog, str8_lit("packed6-tail"));
    u32 packed15_fn = bc_program_find_fn(&prog, str8_lit("packed15-tail"));

    { i64 args[2] = {300, 33};
      expect_eq_i64("packed3-tail(300,33) -- 3-byte struct, 2+1 tail",
                     bc_run_in_program(&prog, packed3_fn, args, 2, heap, &host_imports).value, 333); }
    { i64 args[2] = {600000, 6666};
      expect_eq_i64("packed6-tail(600000,6666) -- 6-byte struct, 4+2 tail",
                     bc_run_in_program(&prog, packed6_fn, args, 2, heap, &host_imports).value, 606666); }
    { i64 args[4] = {151515151515LL, 151515, 1515, 15};
      expect_eq_i64("packed15-tail(...) -- 15-byte struct, one word + 4+2+1 tail",
                     bc_run_in_program(&prog, packed15_fn, args, 4, heap, &host_imports).value,
                     151515151515LL + 151515 + 1515 + 15); }
  }

  arena_temp_end(&fn_temp);

  if (g_failures == 0) printf("bcgen_struct_test: all checks passed\n");
  else                 printf("bcgen_struct_test: %d check(s) FAILED\n", g_failures);

  ctx_free();
  return g_failures == 0 ? 0 : 1;
}
