// bcgen_io_test.c -- validates bcio.c's serialization/caching (see that
// file's own top-of-file note on the two correctness problems it had to
// solve: persisting string-literal pointers, and host-import indices that
// aren't safe to bake in as absolute numbers across a save/reload). Same
// rig as the other bcgen_*_test.c files.
//
// Exercises:
//  - Round-trip correctness for plain arithmetic, cross-function calls
//    (BcOp_Call), structs, and a string-returning function.
//  - Host-import re-resolution against a DELIBERATELY DIFFERENTLY-ORDERED
//    BcHostImportTable at load time -- native_double is registered SECOND
//    when compiling but FIRST when loading, so if bc_program_load's name-
//    based re-resolution didn't actually work (i.e. it just trusted the
//    original baked-in index), this would silently call native_noop
//    instead and the test would catch it.
//  - A real cold/warm caching workflow: no cache file exists (cold) ->
//    compile fresh, save; cache file exists and its content hash matches
//    the source (warm) -> load ONLY, the compiler is never invoked again
//    for that path. Both paths are checked to produce identical results.
//  - The warm path is now a real mmap (see bcio.c's own top-of-file note)
//    -- bc_program_unload releases it once `loaded.program` is no longer
//    needed, exercised explicitly here rather than just letting the
//    process exit clean it up.
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

static i64
native_noop(i64* args, u32 arg_count, Arena* heap, void* userdata) {
  (void)args; (void)arg_count; (void)heap; (void)userdata;
  return -1; // a distinctive wrong-answer sentinel -- if re-resolution is broken and this gets
                // called instead of native_double, the mismatch is unambiguous, not a coincidence
}

static i64
native_double(i64* args, u32 arg_count, Arena* heap, void* userdata) {
  (void)arg_count; (void)heap; (void)userdata;
  return args[0] * 2;
}

static const char* g_fixture_source =
  "(package bcgen_io_test)\n"
  "(extern (fn native-double [n i32] i32))\n"
  "(extern (fn native-noop [n i32] i32))\n"
  "\n"
  "(struct Point [x i32 y i32])\n"
  "(fn make-point [x i32 y i32] Point (Point {:x x :y y}))\n"
  "(fn point-sum [p Point] i32 (+ (get p x) (get p y)))\n"
  "\n"
  "(fn greeting [] string \"hello from cache\")\n"
  "\n"
  "(fn add [a i32 b i32] i32 (+ a b))\n"
  "(fn double-via-host [n i32] i32 (native-double n))\n"
  "(fn add-then-double [a i32 b i32] i32 (double-via-host (add a b)))\n"
  "\n"
  "(fn main [] i32 0)\n";

static Checker
check_fixture(TypedAst* tast, TypedIndex* out_root) {
  Ast ast;
  ast_init(&ast, ctx_perm());

  String8 src     = str8_cstring((char*)(g_fixture_source));
  u32     file_id = source_file_register(str8_lit("bcgen_io_test_fixture.3b"), src);

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

// Verifies a handful of functions in `prog` against expected values --
// shared by the "compiled fresh" check and the "loaded from cache" check,
// so both paths are proven to produce IDENTICAL results via the exact
// same assertions, not two hand-copied lists that could quietly diverge.
static void
verify_program(const char* label, BcProgram* prog, BcHostImportTable* host_imports, Arena* heap) {
  char buf[128];
  u32 add_fn             = bc_program_find_fn(prog, str8_lit("add"));
  u32 double_via_host_fn = bc_program_find_fn(prog, str8_lit("double-via-host"));
  u32 add_then_double_fn = bc_program_find_fn(prog, str8_lit("add-then-double"));
  u32 make_point_fn      = bc_program_find_fn(prog, str8_lit("make-point"));
  u32 point_sum_fn       = bc_program_find_fn(prog, str8_lit("point-sum"));
  u32 greeting_fn        = bc_program_find_fn(prog, str8_lit("greeting"));

  { i64 args[2] = {3, 4};
    snprintf(buf, sizeof(buf), "%s: add(3,4)", label);
    expect_eq_i64(buf, bc_run_in_program(prog, add_fn, args, 2, heap, host_imports).value, 7); }

  { i64 args[1] = {21};
    snprintf(buf, sizeof(buf), "%s: double-via-host(21)", label);
    expect_eq_i64(buf, bc_run_in_program(prog, double_via_host_fn, args, 1, heap, host_imports).value, 42); }

  { i64 args[2] = {5, 6};
    snprintf(buf, sizeof(buf), "%s: add-then-double(5,6)", label); // (5+6)*2 = 22, a CALL into a
                                                                      // CallHost, exercising both
                                                                      // call kinds composed together
    expect_eq_i64(buf, bc_run_in_program(prog, add_then_double_fn, args, 2, heap, host_imports).value, 22); }

  { i64 args[2] = {10, 32};
    i64 point_ptr = bc_run_in_program(prog, make_point_fn, args, 2, heap, host_imports).value;
    i64 sargs[1] = { point_ptr };
    snprintf(buf, sizeof(buf), "%s: point-sum(make-point(10,32))", label);
    expect_eq_i64(buf, bc_run_in_program(prog, point_sum_fn, sargs, 1, heap, host_imports).value, 42);
  }

  { BcResult r = bc_run_in_program(prog, greeting_fn, NULL, 0, heap, host_imports);
    u8* base = (u8*)(intptr_t)r.value;
    u8*  str;  memcpy(&str,  base + 0, sizeof(str));
    u64  size; memcpy(&size, base + 8, sizeof(size));
    snprintf(buf, sizeof(buf), "%s: greeting()", label);
    if (size != 16 || memcmp(str, "hello from cache", 16) != 0) {
      fprintf(stderr, "FAIL %s: got %.*s (size %llu), want \"hello from cache\" (size 16)\n",
              buf, (int)size, (char*)str, (unsigned long long)size);
      g_failures += 1;
    }
  }
}

int
main(void) {
  Context ctx;
  ctx_init(&ctx, MB(16));

  const char* cache_path_cstr = "/tmp/3b_bcio_test_cache.3bc";
  String8     cache_path      = str8_cstring((char*)cache_path_cstr);
  remove(cache_path_cstr); // start from a genuinely clean (no pre-existing cache) state

  TypedAst   tast;
  TypedIndex root;
  Checker    ck = check_fixture(&tast, &root);
  xassert(tast.nodes[root].kind == TypedNodeKind_Block);

  // The COMPILING host_imports table -- native_noop registered FIRST.
  ArenaTemp compile_temp = arena_temp_begin(ctx_scratch());
  LayoutCache layout_cache;
  layout_cache_init(&layout_cache, ctx_perm());
  TypeRef i32_ty = {0}; i32_ty.kind = TypeKind_I32;
  TypeRef one_i32_param[1] = { i32_ty };

  BcHostImportTable compile_host_imports = {0};
  bc_host_import_table_add(&compile_host_imports, ctx_perm(), str8_lit("native-noop"), native_noop,
                            one_i32_param, 1, i32_ty);
  bc_host_import_table_add(&compile_host_imports, ctx_perm(), str8_lit("native-double"), native_double,
                            one_i32_param, 1, i32_ty);

  Arena* heap = ctx_perm();
  String8 source_view = str8_cstring((char*)g_fixture_source);
  u64     source_hash = bc_content_hash(source_view);

  // ~~ Cold path: no cache file exists yet -- compile fresh, verify, save.
  BcProgram fresh_prog = bc_compile_program(&ck, &tast, root, compile_temp.arena, ctx_perm(), &layout_cache, &compile_host_imports, NULL);
  verify_program("compiled-fresh", &fresh_prog, &compile_host_imports, heap);

  b32 saved = bc_program_save(&fresh_prog, &compile_host_imports, cache_path, 0);
  if (!saved) { fprintf(stderr, "FATAL: bc_program_save failed\n"); exit(1); }

  // ~~ Warm path: the cache file now exists. A real caller would check a
  // stored hash before even getting here (see bcio.h's own note that
  // cache-invalidation POLICY is deliberately the caller's job, not
  // bcio.c's) -- confirm bc_content_hash is at least stable/deterministic
  // for that check to be meaningful.
  expect_eq_i64("bc_content_hash is deterministic for the same source",
                (i64)bc_content_hash(source_view), (i64)source_hash);

  // Load into a COMPLETELY FRESH arena, using a host_imports table with
  // native_double registered FIRST this time (the OPPOSITE order from
  // compile time) -- this is the whole point: bc_compile_program/
  // bc_program_load are never called together here, only bc_program_load,
  // proving recompilation is genuinely skippable, and proving the
  // reordered table still resolves correctly by name.
  ArenaTemp load_temp = arena_temp_begin(ctx_scratch());
  BcHostImportTable load_host_imports = {0};
  bc_host_import_table_add(&load_host_imports, ctx_perm(), str8_lit("native-double"), native_double,
                            one_i32_param, 1, i32_ty);
  bc_host_import_table_add(&load_host_imports, ctx_perm(), str8_lit("native-noop"), native_noop,
                            one_i32_param, 1, i32_ty);

  BcLoadResult loaded = bc_program_load(cache_path, &load_host_imports, NULL, load_temp.arena, ctx_perm());
  if (!loaded.ok) { fprintf(stderr, "FATAL: bc_program_load failed (expected a valid cache file)\n"); exit(1); }
  verify_program("loaded-from-cache", &loaded.program, &load_host_imports, heap);

  // `loaded.program`'s code/const arrays and string constants point
  // DIRECTLY into the mmap'd cache file now (see bcio.c's own top-of-file
  // note) -- bc_program_unload (NOT just arena_temp_end) is what actually
  // releases that mapping.
  bc_program_unload(&loaded);

  arena_temp_end(&load_temp);
  arena_temp_end(&compile_temp);
  remove(cache_path_cstr); // leave no artifact behind

  if (g_failures == 0) printf("bcgen_io_test: all checks passed\n");
  else                 printf("bcgen_io_test: %d check(s) FAILED\n", g_failures);

  ctx_free();
  return g_failures == 0 ? 0 : 1;
}
