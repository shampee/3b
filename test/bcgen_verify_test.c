// bcgen_verify_test.c -- validates bc_verify_host_imports (see bcgen.h's
// own comment): cross-checking an `(extern (fn ...))` declaration's real
// signature against what the EMBEDDING PROGRAM separately registered for
// that name, catching the two independent sources of truth silently
// drifting apart -- something no prior increment actually checked (only
// `arg_count`, not real types, was ever cross-checked before this). Same
// rig as the other bcgen_*_test.c files.
//
// Exercises:
//  - A CORRECT registration: bc_verify_host_imports reports no mismatches,
//    AND the program actually compiles and runs correctly end to end
//    (bc_compile_program calls this same verification internally and
//    would assert on a real mismatch, so successfully compiling here is
//    itself part of the proof).
//  - A DELIBERATELY WRONG registration, exercising every mismatch kind
//    this function detects: wrong arg count, wrong parameter type, wrong
//    return type, and -- registered as BcHostImportKind_Direct despite
//    exactly matching an f64-involving extern declaration -- the
//    Direct-mode-can't-do-floats rejection, which is independent of
//    plain type-equality (the types match perfectly; Direct mode still
//    can't be used for them). Also covers the newer Direct-mode-arity
//    rejection the same way: native-many-args' 13 params exactly match
//    its extern declaration, but exceed BC_NATIVE_DIRECT_MAX_ARGS (12),
//    which is ALSO categorically incompatible with Direct mode regardless
//    of type-matching. Checked via bc_verify_host_imports called
//    DIRECTLY, never through bc_compile_program, so the deliberately
//    broken case doesn't abort this test process.
#include "3b.h"
#include "bcgen.h"
#include "bcvm.h"
#include "bcnative.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
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

static i64 native_add_impl(i64* args, u32 argc, Arena* heap, void* userdata)     { (void)argc; (void)heap; (void)userdata; return args[0] + args[1]; }
static i64 native_triple_impl(i64* args, u32 argc, Arena* heap, void* userdata)  { (void)argc; (void)heap; (void)userdata; return args[0] * 3; }
static i64 native_label_impl(i64* args, u32 argc, Arena* heap, void* userdata)   { (void)args; (void)argc; (void)heap; (void)userdata; return 42; }
static i64
native_sqrt_impl(i64* args, u32 argc, Arena* heap, void* userdata) {
  (void)argc; (void)heap; (void)userdata;
  f64 v; memcpy(&v, &args[0], sizeof(v));
  f64 r = sqrt(v);
  i64 bits; memcpy(&bits, &r, sizeof(bits));
  return bits;
}

// 13 params -- one past BC_NATIVE_DIRECT_MAX_ARGS (12) -- registered as
// Trampoline in the CORRECT table below (Trampoline has no arg-count
// cap at all) and as Direct (deliberately wrong -- 13 exceeds Direct's
// own cap) in the broken table, to exercise the new arity-rejection
// check specifically.
static i64
native_many_args_impl(i64* args, u32 argc, Arena* heap, void* userdata) {
  (void)heap; (void)userdata;
  i64 sum = 0;
  foreach_index(i, argc) sum += args[i];
  return sum;
}

static const char* g_fixture_source =
  "(package bcgen_verify_test)\n"
  "(extern (fn native-add [a i32 b i32] i32))\n"
  "(extern (fn native-triple [n i32] i32))\n"
  "(extern (fn native-label [] i32))\n"
  "(extern (fn native-sqrt [x f64] f64))\n"
  "(extern (fn native-many-args [a i32 b i32 c i32 d i32 e i32 f i32 g i32 h i32 i i32 j i32 k i32 l i32 m i32] i32))\n"
  "\n"
  "(fn use-add [a i32 b i32] i32 (native-add a b))\n"
  "(fn use-triple [n i32] i32 (native-triple n))\n"
  "(fn use-label [] i32 (native-label))\n"
  "(fn use-sqrt [x f64] f64 (native-sqrt x))\n"
  "(fn use-many-args [a i32 b i32 c i32 d i32 e i32 f i32 g i32 h i32 i i32 j i32 k i32 l i32 m i32] i32\n"
  "  (native-many-args a b c d e f g h i j k l m))\n"
  "\n"
  "(fn main [] i32 0)\n";

static Checker
check_fixture(TypedAst* tast, TypedIndex* out_root) {
  Ast ast;
  ast_init(&ast, ctx_perm());

  String8 src     = str8_cstring((char*)(g_fixture_source));
  u32     file_id = source_file_register(str8_lit("bcgen_verify_test_fixture.3b"), src);

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
  (void)ck; // only needed to have run the checker; verification itself doesn't need it

  TypeRef i32_ty = {0}; i32_ty.kind = TypeKind_I32;
  TypeRef f64_ty = {0}; f64_ty.kind = TypeKind_F64;
  TypeRef add_params[2]    = { i32_ty, i32_ty };
  TypeRef triple_params[1] = { i32_ty };
  TypeRef sqrt_params[1]   = { f64_ty };
  TypeRef many_args_params[13] = { i32_ty, i32_ty, i32_ty, i32_ty, i32_ty, i32_ty, i32_ty,
                                    i32_ty, i32_ty, i32_ty, i32_ty, i32_ty, i32_ty };

  // ~~ CORRECT registration: verification passes AND the program actually runs.
  {
    ArenaTemp   fn_temp = arena_temp_begin(ctx_scratch());
    LayoutCache layout_cache;
    layout_cache_init(&layout_cache, ctx_perm());

    BcHostImportTable good = {0};
    bc_host_import_table_add(&good, ctx_perm(), str8_lit("native-add"), native_add_impl, add_params, 2, i32_ty);
    bc_host_import_table_add(&good, ctx_perm(), str8_lit("native-triple"), native_triple_impl, triple_params, 1, i32_ty);
    bc_host_import_table_add(&good, ctx_perm(), str8_lit("native-label"), native_label_impl, NULL, 0, i32_ty);
    bc_host_import_table_add(&good, ctx_perm(), str8_lit("native-sqrt"), native_sqrt_impl, sqrt_params, 1, f64_ty);
    // Trampoline -- correct choice for 13 args, since Direct's cap is 12.
    bc_host_import_table_add(&good, ctx_perm(), str8_lit("native-many-args"), native_many_args_impl,
                              many_args_params, 13, i32_ty);

    BcHostSignatureMismatch* mismatches = NULL;
    b32 ok = bc_verify_host_imports(&tast, root, &good, ctx_scratch(), &mismatches);
    expect_true("correct registration: bc_verify_host_imports reports ok", ok);
    expect_eq_i64("correct registration: zero mismatches", (i64)dyn_count(mismatches), 0);

    // bc_compile_program calls this same verification internally and
    // would assert on a real mismatch -- successfully compiling AND
    // running here is itself part of the proof this registration is genuinely correct.
    BcProgram prog = bc_compile_program(&ck, &tast, root, fn_temp.arena, ctx_perm(), &layout_cache, &good, NULL);
    u32 use_add_fn    = bc_program_find_fn(&prog, str8_lit("use-add"));
    u32 use_triple_fn = bc_program_find_fn(&prog, str8_lit("use-triple"));
    Arena* heap = ctx_perm();
    { i64 args[2] = {3, 4}; expect_eq_i64("use-add(3,4)", bc_run_in_program(&prog, use_add_fn, args, 2, heap, &good).value, 7); }
    { i64 args[1] = {5};    expect_eq_i64("use-triple(5)", bc_run_in_program(&prog, use_triple_fn, args, 1, heap, &good).value, 15); }

    arena_temp_end(&fn_temp);
  }

  // ~~ DELIBERATELY WRONG registration: exercises every mismatch kind,
  // checked via bc_verify_host_imports DIRECTLY (never bc_compile_program,
  // which would assert and abort this process).
  {
    TypeRef i64_ty = {0}; i64_ty.kind = TypeKind_I64;
    TypeRef wrong_add_params[3]    = { i32_ty, i32_ty, i32_ty }; // WRONG: extern declares 2, this says 3
    TypeRef wrong_triple_params[1] = { i64_ty };                  // WRONG: extern declares i32, this says i64

    BcHostImportTable bad = {0};
    bc_host_import_table_add(&bad, ctx_perm(), str8_lit("native-add"), native_add_impl, wrong_add_params, 3, i32_ty); // bad arg count
    bc_host_import_table_add(&bad, ctx_perm(), str8_lit("native-triple"), native_triple_impl, wrong_triple_params, 1, i32_ty); // bad param type
    bc_host_import_table_add(&bad, ctx_perm(), str8_lit("native-label"), native_label_impl, NULL, 0, i64_ty); // bad return type (i64 vs i32)
    // native-sqrt: types match the extern EXACTLY (f64 param, f64 return), but registered
    // as Direct -- Direct mode categorically can't do floats, regardless of type-matching.
    bc_host_import_table_add_direct(&bad, ctx_perm(), str8_lit("native-sqrt"),
                                     (void*)(intptr_t)native_sqrt_impl, sqrt_params, 1, f64_ty);
    // native-many-args: types match the extern EXACTLY (13 i32 params, i32 return), but
    // registered as Direct -- 13 exceeds BC_NATIVE_DIRECT_MAX_ARGS (12), regardless of
    // type-matching, same "categorically incompatible" shape as the float case above.
    bc_host_import_table_add_direct(&bad, ctx_perm(), str8_lit("native-many-args"),
                                     (void*)(intptr_t)native_many_args_impl, many_args_params, 13, i32_ty);

    BcHostSignatureMismatch* mismatches = NULL;
    b32 ok = bc_verify_host_imports(&tast, root, &bad, ctx_scratch(), &mismatches);
    expect_true("broken registration: bc_verify_host_imports correctly reports NOT ok", !ok);

    u64 count = dyn_count(mismatches);
    if (count == 0) {
      fprintf(stderr, "FAIL broken registration: expected mismatches to be reported, got none\n");
      g_failures += 1;
    } else {
      expect_true("mismatch list mentions native-add (arg count)",    mismatches_mention(mismatches, count, "native-add"));
      expect_true("mismatch list mentions native-triple (param type)", mismatches_mention(mismatches, count, "native-triple"));
      expect_true("mismatch list mentions native-label (return type)", mismatches_mention(mismatches, count, "native-label"));
      expect_true("mismatch list mentions native-sqrt (Direct+float)", mismatches_mention(mismatches, count, "native-sqrt"));
      // native-sqrt alone should contribute TWO mismatches (param is float, AND return is
      // float) since Direct mode's float rejection checks both independently.
      u32 sqrt_mentions = 0;
      foreach_index(i, count) if (str8_match(mismatches[i].name, str8_lit("native-sqrt"), 0)) sqrt_mentions += 1;
      expect_eq_i64("native-sqrt contributes exactly 2 mismatches (float param + float return)", sqrt_mentions, 2);
      expect_true("mismatch list mentions native-many-args (Direct+over-arity)",
                  mismatches_mention(mismatches, count, "native-many-args"));
    }
  }

  if (g_failures == 0) printf("bcgen_verify_test: all checks passed\n");
  else                 printf("bcgen_verify_test: %d check(s) FAILED\n", g_failures);

  ctx_free();
  return g_failures == 0 ? 0 : 1;
}
