// bcio_verify_test.c -- validates bc_verify_cached_host_imports (bcio.h):
// cross-checking a LOADED cache file's own recorded host-import
// signatures against the LOADING run's host_imports table -- the
// load-time counterpart of bcgen_verify_test.c's bc_verify_host_imports,
// closing the gap that function's own comment flags (it only ever runs
// against a live TypedAst, which a program loaded from cache never has).
// Same rig as the other bcgen_*_test.c/bcio_*_test.c files.
//
// Exercises:
//  - A REAL bc_program_save -> bc_program_load round trip through a host
//    import with NON-TRIVIAL parameter types (a Pointer and a Named
//    struct type, not just scalars) -- proves bcio.c's recursive TypeRef
//    serializer/deserializer (bcio_write_type_ref/bcio_mmap_read_type_ref)
//    actually round-trips correctly: a MATCHING reload table produces no
//    false-positive mismatch (bc_program_load would have asserted and
//    aborted this process if it had), and the loaded program still runs
//    correctly end to end.
//  - bc_verify_cached_host_imports called DIRECTLY against a hand-built
//    BcCachedHostImportSig list (standing in for "what a file recorded")
//    and a DELIBERATELY WRONG host_imports table, exercising the same
//    mismatch kinds bcgen_verify_test.c already proved for the compile-time
//    path -- arg count, parameter type, return type, and Direct+float --
//    checked without going through bc_program_load itself (which would
//    assert and abort this process on a real mismatch).
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
expect_true(const char* what, b32 got) {
  if (!got) {
    fprintf(stderr, "FAIL %s: got false, want true\n", what);
    g_failures += 1;
  }
}

static b32
mismatches_mention(BcHostSignatureMismatch* mismatches, u64 count, const char* name) {
  foreach_index(i, count) {
    if (str8_match(mismatches[i].name, str8_cstring((char*)name), 0)) return true;
  }
  return false;
}

// ~~ Native side of the round-trip fixture's two host imports.
static i64
native_sum_array(i64* args, u32 argc, Arena* heap, void* userdata) {
  (void)argc; (void)heap; (void)userdata;
  i32* arr = (i32*)(intptr_t)args[0];
  i32  n   = (i32)args[1];
  i32  sum = 0;
  for (i32 i = 0; i < n; i += 1) sum += arr[i];
  return sum;
}

static i64
native_vec2_sum(i64* args, u32 argc, Arena* heap, void* userdata) {
  (void)argc; (void)heap; (void)userdata;
  i32* v = (i32*)(intptr_t)args[0]; // Vec2 = {x i32, y i32}, passed as its own address
  return v[0] + v[1];
}

static const char* g_fixture_source =
  "(package bcio_verify_test)\n"
  "(extern (fn native-sum-array [arr i32* n i32] i32))\n"
  "(fn use-sum-array [arr i32* n i32] i32 (native-sum-array arr n))\n"
  "\n"
  "(struct Vec2 [x i32 y i32])\n"
  "(extern (fn native-vec2-sum [v Vec2] i32))\n"
  "(fn use-vec2-sum [v Vec2] i32 (native-vec2-sum v))\n"
  "\n"
  "(fn main [] i32 0)\n";

static Checker
check_fixture(TypedAst* tast, TypedIndex* out_root) {
  Ast ast;
  ast_init(&ast, ctx_perm());

  String8 src     = str8_cstring((char*)(g_fixture_source));
  u32     file_id = source_file_register(str8_lit("bcio_verify_test_fixture.3b"), src);

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

  TypeRef i32_ty     = {0}; i32_ty.kind = TypeKind_I32;
  TypeRef ptr_i32_ty = {0}; ptr_i32_ty.kind = TypeKind_Pointer; ptr_i32_ty.pointee = &i32_ty;
  TypeRef vec2_ty    = {0}; vec2_ty.kind = TypeKind_Named; vec2_ty.name = str8_lit("Vec2");

  TypeRef sum_array_params[2] = { ptr_i32_ty, i32_ty };
  TypeRef vec2_sum_params[1]  = { vec2_ty };

  // ~~ Real save -> load round trip through NON-TRIVIAL (Pointer, Named)
  // parameter types.
  {
    const char* cache_path_cstr = "/tmp/3b_bcio_verify_test_cache.3bc";
    String8     cache_path      = str8_cstring((char*)cache_path_cstr);
    remove(cache_path_cstr);

    ArenaTemp   compile_temp = arena_temp_begin(ctx_scratch());
    LayoutCache layout_cache;
    layout_cache_init(&layout_cache, ctx_perm());

    BcHostImportTable compile_host_imports = {0};
    bc_host_import_table_add(&compile_host_imports, ctx_perm(), str8_lit("native-sum-array"), native_sum_array,
                              sum_array_params, 2, i32_ty);
    bc_host_import_table_add(&compile_host_imports, ctx_perm(), str8_lit("native-vec2-sum"), native_vec2_sum,
                              vec2_sum_params, 1, i32_ty);

    BcProgram prog = bc_compile_program(&ck, &tast, root, compile_temp.arena, ctx_perm(), &layout_cache, &compile_host_imports, NULL);
    u64 content_hash = 0xfeedfacecafef00dULL; // arbitrary -- just checking round-trip below
    b32 saved = bc_program_save(&prog, &compile_host_imports, cache_path, content_hash);
    if (!saved) { fprintf(stderr, "FATAL: bc_program_save failed\n"); exit(1); }

    // A FRESH, but SIGNATURE-MATCHING, host_imports table -- same types,
    // same order doesn't matter (name-based resolution). If the recursive
    // TypeRef round trip were broken (e.g. a Pointer's pointee lost, or a
    // Named type's `.name` string corrupted), bc_program_load would
    // assert and abort this process right here.
    ArenaTemp load_temp = arena_temp_begin(ctx_scratch());
    BcHostImportTable load_host_imports = {0};
    bc_host_import_table_add(&load_host_imports, ctx_perm(), str8_lit("native-vec2-sum"), native_vec2_sum,
                              vec2_sum_params, 1, i32_ty);
    bc_host_import_table_add(&load_host_imports, ctx_perm(), str8_lit("native-sum-array"), native_sum_array,
                              sum_array_params, 2, i32_ty);

    BcLoadResult loaded = bc_program_load(cache_path, &load_host_imports, NULL, load_temp.arena, ctx_perm());
    expect_true("round-trip load succeeds (no false-positive signature mismatch)", loaded.ok);
    expect_true("round-trip content_hash matches what was saved", loaded.content_hash == content_hash);

    if (loaded.ok) {
      Arena* heap = ctx_perm();
      u32 sum_array_fn = bc_program_find_fn(&loaded.program, str8_lit("use-sum-array"));
      u32 vec2_sum_fn  = bc_program_find_fn(&loaded.program, str8_lit("use-vec2-sum"));

      i32 arr[4] = {10, 20, 30, 40};
      { i64 args[2] = { (i64)(intptr_t)arr, 4 };
        expect_eq_i64("use-sum-array(arr,4) after reload", bc_run_in_program(&loaded.program, sum_array_fn, args, 2, heap, &load_host_imports).value, 100); }

      i32 vec2[2] = {7, 35};
      { i64 args[1] = { (i64)(intptr_t)vec2 };
        expect_eq_i64("use-vec2-sum(vec2) after reload", bc_run_in_program(&loaded.program, vec2_sum_fn, args, 1, heap, &load_host_imports).value, 42); }
    }

    bc_program_unload(&loaded);
    arena_temp_end(&load_temp);
    arena_temp_end(&compile_temp);
    remove(cache_path_cstr);
  }

  // ~~ bc_verify_cached_host_imports called DIRECTLY against a hand-built
  // "what a file recorded" signature list and a DELIBERATELY WRONG
  // host_imports table -- exercises every mismatch kind without going
  // through bc_program_load (which would assert and abort on a real one).
  {
    TypeRef i64_ty = {0}; i64_ty.kind = TypeKind_I64;
    TypeRef f64_ty = {0}; f64_ty.kind = TypeKind_F64;

    TypeRef sum_array_expected[2] = { ptr_i32_ty, i32_ty };
    TypeRef vec2_sum_expected[1]  = { vec2_ty };
    TypeRef sqrt_expected[1]      = { f64_ty };

    BcCachedHostImportSig sigs[3] = {0};
    sigs[0].name = str8_lit("native-sum-array"); sigs[0].kind = BcHostImportKind_Trampoline;
    sigs[0].param_types = sum_array_expected; sigs[0].param_count = 2; sigs[0].return_type = i32_ty;
    sigs[1].name = str8_lit("native-vec2-sum"); sigs[1].kind = BcHostImportKind_Trampoline;
    sigs[1].param_types = vec2_sum_expected; sigs[1].param_count = 1; sigs[1].return_type = i32_ty;
    sigs[2].name = str8_lit("native-sqrt"); sigs[2].kind = BcHostImportKind_Trampoline;
    sigs[2].param_types = sqrt_expected; sigs[2].param_count = 1; sigs[2].return_type = f64_ty;

    TypeRef wrong_sum_array_params[3] = { ptr_i32_ty, i32_ty, i32_ty }; // WRONG: file says 2 params
    TypeRef wrong_vec2_sum_params[1]  = { i64_ty };                     // WRONG: file says Vec2, this says i64

    BcHostImportTable bad = {0};
    bc_host_import_table_add(&bad, ctx_perm(), str8_lit("native-sum-array"), native_sum_array,
                              wrong_sum_array_params, 3, i32_ty); // bad arg count
    bc_host_import_table_add(&bad, ctx_perm(), str8_lit("native-vec2-sum"), native_vec2_sum,
                              wrong_vec2_sum_params, 1, i64_ty); // bad param type AND bad return type
    // native-sqrt: types match the recorded signature EXACTLY (f64 param, f64 return), but
    // registered as Direct -- categorically incompatible regardless of type-matching.
    bc_host_import_table_add_direct(&bad, ctx_perm(), str8_lit("native-sqrt"),
                                     (void*)(intptr_t)native_sum_array, sqrt_expected, 1, f64_ty);

    BcHostSignatureMismatch* mismatches = NULL;
    b32 ok = bc_verify_cached_host_imports(sigs, 3, &bad, ctx_scratch(), &mismatches);
    expect_true("broken reload table: bc_verify_cached_host_imports correctly reports NOT ok", !ok);

    u64 count = dyn_count(mismatches);
    if (count == 0) {
      fprintf(stderr, "FAIL broken reload table: expected mismatches to be reported, got none\n");
      g_failures += 1;
    } else {
      expect_true("mismatch list mentions native-sum-array (arg count)", mismatches_mention(mismatches, count, "native-sum-array"));
      expect_true("mismatch list mentions native-vec2-sum (param+return type)", mismatches_mention(mismatches, count, "native-vec2-sum"));
      expect_true("mismatch list mentions native-sqrt (Direct+float)", mismatches_mention(mismatches, count, "native-sqrt"));
      u32 sqrt_mentions = 0;
      foreach_index(i, count) if (str8_match(mismatches[i].name, str8_lit("native-sqrt"), 0)) sqrt_mentions += 1;
      expect_eq_i64("native-sqrt contributes exactly 2 mismatches (float param + float return)", sqrt_mentions, 2);
    }

    // A signature the file DIDN'T record anything about (name not present
    // in `sigs`) is simply not checked here -- that's bc_program_load's
    // OWN concern (a missing per-call-site resolution), not this
    // function's job. Confirmed by construction: `bad` only registers the
    // three names above, and verification only ever iterates `sigs`.
  }

  // ~~ A STALE cache: a real save -> load round trip where the LOADING
  // run's host_imports table is missing a name the file's BcOp_CallHost
  // fixups actually need (standing in for a game renaming/removing one of
  // its own registered callbacks between when the cache was written and
  // the next run -- see game3d's thing/get-offset-x etc. -> thing/get+
  // thing/set consolidation, which hit exactly this in practice). Before
  // this test was added, bc_program_load's fixup loop asserted and
  // aborted the whole process here instead of returning `ok == false` for
  // the caller's normal "no usable cache, recompile from source" fallback
  // (script.c's load_or_compile_script) to handle.
  {
    const char* cache_path_cstr = "/tmp/3b_bcio_verify_test_stale_cache.3bc";
    String8     cache_path      = str8_cstring((char*)cache_path_cstr);
    remove(cache_path_cstr);

    ArenaTemp   compile_temp = arena_temp_begin(ctx_scratch());
    LayoutCache layout_cache;
    layout_cache_init(&layout_cache, ctx_perm());

    BcHostImportTable compile_host_imports = {0};
    bc_host_import_table_add(&compile_host_imports, ctx_perm(), str8_lit("native-sum-array"), native_sum_array,
                              sum_array_params, 2, i32_ty);
    bc_host_import_table_add(&compile_host_imports, ctx_perm(), str8_lit("native-vec2-sum"), native_vec2_sum,
                              vec2_sum_params, 1, i32_ty);

    BcProgram prog = bc_compile_program(&ck, &tast, root, compile_temp.arena, ctx_perm(), &layout_cache, &compile_host_imports, NULL);
    b32 saved = bc_program_save(&prog, &compile_host_imports, cache_path, 0xdeadbeefULL);
    if (!saved) { fprintf(stderr, "FATAL: bc_program_save failed\n"); exit(1); }

    // The LOADING run's table only knows "native-vec2-sum" under a RENAMED
    // name -- "native-sum-array" (which the saved program's BcOp_CallHost
    // fixups actually reference) is nowhere in this table at all.
    ArenaTemp load_temp = arena_temp_begin(ctx_scratch());
    BcHostImportTable renamed_host_imports = {0};
    bc_host_import_table_add(&renamed_host_imports, ctx_perm(), str8_lit("native-vec2-sum-renamed"), native_vec2_sum,
                              vec2_sum_params, 1, i32_ty);

    BcLoadResult loaded = bc_program_load(cache_path, &renamed_host_imports, NULL, load_temp.arena, ctx_perm());
    expect_true("stale cache (renamed host import): load returns ok==false, doesn't abort", !loaded.ok);

    if (loaded.ok) bc_program_unload(&loaded);
    arena_temp_end(&load_temp);
    arena_temp_end(&compile_temp);
    remove(cache_path_cstr);
  }

  if (g_failures == 0) printf("bcio_verify_test: all checks passed\n");
  else                 printf("bcio_verify_test: %d check(s) FAILED\n", g_failures);

  ctx_free();
  return g_failures == 0 ? 0 : 1;
}
