// bcgen_globals_test.c -- validates module-level `var`/`val` (global)
// support added to bcgen.c/bcvm.c/bcio.c: BcOp_LoadGlobal/BcOp_StoreGlobal,
// BcProgram.globals, and the synthesized `#init_globals` chunk
// bc_compile_program/bc_program_load both run automatically. Same rig as
// the other bcgen_*_test.c files, plus a save/load round trip (bcio.h) at
// the end.
//
// Exercises:
//  - A `var` global MUTATED via one compiled function and READ BACK via a
//    SEPARATE, later bc_run_in_program call on the SAME BcProgram --
//    proving the value survives across calls (unlike a local, which dies
//    with its own frame). A `var` missing from bc_compile_program's gather
//    loop aborts the process on the first reference to it.
//  - A `val` global with a literal initializer.
//  - An ARRAY-typed `var` global with an OMITTED initializer (checker.c
//    only allows omission for array types -- see check_init_expr's own
//    comment) -- must default to all-zero, exercising
//    bc_compile_zero_value's embedded-type path from inside
//    `#init_globals` specifically, not just `(zero T)`.
//  - A STRUCT-typed `var` global with a real StructLiteral initializer,
//    read back through ordinary field access.
//  - A LOCAL `var` of the SAME NAME as a global correctly SHADOWS it
//    (checker's own Scope precedence, mirrored by bc_local_try_lookup
//    trying locals before fc->global_table).
//  - A save (bcio.c) / load round trip: the loaded program's global gets
//    RE-INITIALIZED (not loaded stale) -- proven by mutating the global via
//    the ORIGINAL (pre-save) program first, then confirming the freshly
//    LOADED program starts back at the real initial value, not the
//    mutated one (which was never serialized at all -- see
//    bc_program_save's own comment on why only the COUNT is saved).
#include "3b.h"
#include "bcgen.h"
#include "bcvm.h"
#include "bcio.h"
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
expect_eq_f64(const char* what, i64 got_bits, f64 want) {
  f64 got; memcpy(&got, &got_bits, sizeof(got));
  if (got != want) {
    fprintf(stderr, "FAIL %s: got %g, want %g\n", what, got, want);
    g_failures += 1;
  }
}

// An f32 result lives in the LOW 4 bytes of its i64 register only --
// everything above is don't-care -- so it must be narrowed before
// reinterpreting, unlike expect_eq_f64 above (a genuine 8-byte value).
static void
expect_eq_f32(const char* what, i64 got_bits, f32 want) {
  f32 got; u32 bits32 = (u32)got_bits; memcpy(&got, &bits32, sizeof(got));
  if (got != want) {
    fprintf(stderr, "FAIL %s: got %g, want %g\n", what, (double)got, (double)want);
    g_failures += 1;
  }
}

static const char* g_fixture_source =
  "(package bcgen_globals_test)\n"
  "(struct Vec2 [x f32 y f32])\n"
  "\n"
  "(var counter i32 0)\n"
  "(val PI f64 3.14159f64)\n"
  "(var untouched [i32 3])\n"
  "(var origin Vec2 (Vec2 {:x 1.0f32 :y 2.0f32}))\n"
  "\n"
  "(fn bump [] i32 (set counter (+ counter 1)) counter)\n"
  "(fn read-counter [] i32 counter)\n"
  "(fn read-pi [] f64 PI)\n"
  "(fn read-untouched-0 [] i32 (nth untouched 0))\n"
  "(fn read-origin-x [] f32 (. origin x))\n"
  "(fn shadow-test [] i32 (let [counter i32 999] counter))\n"
  "\n"
  "(fn main [] i32 0)\n";

static Checker
check_fixture(TypedAst* tast, TypedIndex* out_root) {
  Ast ast;
  ast_init(&ast, ctx_perm());

  String8 src     = str8_cstring((char*)(g_fixture_source));
  u32     file_id = source_file_register(str8_lit("bcgen_globals_test_fixture.3b"), src);

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
  xassert(prog.ok);

  u32 bump_fn              = bc_program_find_fn(&prog, str8_lit("bump"));
  u32 read_counter_fn      = bc_program_find_fn(&prog, str8_lit("read-counter"));
  u32 read_pi_fn           = bc_program_find_fn(&prog, str8_lit("read-pi"));
  u32 read_untouched_0_fn  = bc_program_find_fn(&prog, str8_lit("read-untouched-0"));
  u32 read_origin_x_fn     = bc_program_find_fn(&prog, str8_lit("read-origin-x"));
  u32 shadow_test_fn       = bc_program_find_fn(&prog, str8_lit("shadow-test"));

  Arena* heap = ctx_perm();

  // ~~ Already initialized before ANY of these calls -- #init_globals ran
  // inside bc_compile_program itself.
  expect_eq_i64("read-counter() initial", bc_run_in_program(&prog, read_counter_fn, NULL, 0, heap, &host_imports).value, 0);
  expect_eq_f64("read-pi()",              bc_run_in_program(&prog, read_pi_fn,      NULL, 0, heap, &host_imports).value, 3.14159);

  // ~~ Mutated by ONE call, read back by a SEPARATE, later call on the
  // SAME BcProgram -- the actual crash this feature closes.
  expect_eq_i64("bump() #1",              bc_run_in_program(&prog, bump_fn, NULL, 0, heap, &host_imports).value, 1);
  expect_eq_i64("bump() #2",              bc_run_in_program(&prog, bump_fn, NULL, 0, heap, &host_imports).value, 2);
  expect_eq_i64("read-counter() after 2 bumps", bc_run_in_program(&prog, read_counter_fn, NULL, 0, heap, &host_imports).value, 2);

  // ~~ Array-typed global, OMITTED initializer -- must default to zero.
  expect_eq_i64("read-untouched-0()", bc_run_in_program(&prog, read_untouched_0_fn, NULL, 0, heap, &host_imports).value, 0);

  // ~~ Struct-typed global, real StructLiteral initializer.
  { BcResult r = bc_run_in_program(&prog, read_origin_x_fn, NULL, 0, heap, &host_imports);
    expect_eq_f32("read-origin-x()", r.value, 1.0f);
  }

  // ~~ A local `var` of the same name SHADOWS the global.
  expect_eq_i64("shadow-test()", bc_run_in_program(&prog, shadow_test_fn, NULL, 0, heap, &host_imports).value, 999);
  expect_eq_i64("read-counter() unaffected by shadow-test", bc_run_in_program(&prog, read_counter_fn, NULL, 0, heap, &host_imports).value, 2);

  // ~~ Save/load round trip -- the LOADED program's global must be
  // RE-INITIALIZED (back to 0), NOT loaded stale as the mutated value 2 --
  // only the COUNT is ever serialized, never the current values.
  {
    ArenaTemp save_temp = arena_temp_begin(ctx_scratch());
    String8   path       = str8_lit("/tmp/bcgen_globals_test.bc");
    b32       saved       = bc_program_save(&prog, &host_imports, path, 0);
    if (!saved) { fprintf(stderr, "FATAL: bc_program_save failed\n"); exit(1); }

    BcLoadResult loaded = bc_program_load(path, &host_imports, NULL, save_temp.arena, ctx_perm());
    if (!loaded.ok) { fprintf(stderr, "FATAL: bc_program_load failed\n"); exit(1); }

    u32 loaded_read_counter_fn = bc_program_find_fn(&loaded.program, str8_lit("read-counter"));
    u32 loaded_bump_fn          = bc_program_find_fn(&loaded.program, str8_lit("bump"));
    expect_eq_i64("loaded read-counter() re-initialized, not stale",
                  bc_run_in_program(&loaded.program, loaded_read_counter_fn, NULL, 0, heap, &host_imports).value, 0);
    expect_eq_i64("loaded bump() works on the loaded program's OWN globals array",
                  bc_run_in_program(&loaded.program, loaded_bump_fn, NULL, 0, heap, &host_imports).value, 1);
    // The ORIGINAL program's own globals array is untouched by the above --
    // separate BcProgram, separate `globals` array.
    expect_eq_i64("original read-counter() still 2, unaffected by the loaded copy",
                  bc_run_in_program(&prog, read_counter_fn, NULL, 0, heap, &host_imports).value, 2);

    bc_program_unload(&loaded);
    arena_temp_end(&save_temp);
    remove("/tmp/bcgen_globals_test.bc");
  }

  arena_temp_end(&fn_temp);

  if (g_failures == 0) printf("bcgen_globals_test: all checks passed\n");
  else                 printf("bcgen_globals_test: %d check(s) FAILED\n", g_failures);

  ctx_free();
  return g_failures == 0 ? 0 : 1;
}
