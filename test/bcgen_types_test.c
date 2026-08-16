// bcgen_types_test.c -- validates float/string/handle support wired into
// bcgen.c/bcvm.c on top of the earlier int/bool/struct slice (see
// bcgen.c's top-of-file note). Same rig as the other bcgen_*_test.c files
// (hand-rolled parse -> lower -> check against an in-memory fixture).
//
// Exercises: f64 arithmetic/comparisons (register values reinterpreted as
// bit patterns, not a separate register file), an f64 struct field, a
// string struct field filled from BOTH a literal (compile-time bytes) and
// a plain parameter (a runtime copy of an already-boxed {ptr,size}
// header -- not just re-storing the pointer), and a handle-typed field/
// param/return round-tripped as an opaque 8-byte value (no handle-pool
// builtins needed -- Mesh^ never has to come from handle-alloc to prove
// the FIELD STORAGE path works; it's just passed in as an arbitrary i64
// bit pattern and checked for exact round-trip).
#include "3b.h"
#include "bcgen.h"
#include "bcvm.h"
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

static i64
f64_bits(f64 v) {
  i64 bits; memcpy(&bits, &v, sizeof(bits));
  return bits;
}

static const char* g_fixture_source =
  "(package bcgen_types_test)\n"
  "(struct Mesh [id i32])\n"
  "(handle Mesh)\n"
  "\n"
  "(fn fadd [a f64 b f64] f64 (+ a b))\n"
  "(fn fsub [a f64 b f64] f64 (- a b))\n"
  "(fn fmul [a f64 b f64] f64 (* a b))\n"
  "(fn fdiv [a f64 b f64] f64 (/ a b))\n"
  "(fn flt [a f64 b f64] bool (< a b))\n"
  "\n"
  "(struct Circle [radius f64 label string])\n"
  "(fn make-circle-lit [r f64] Circle\n"
  "  (Circle {:radius r :label \"hello\"}))\n"
  "(fn make-circle-param [r f64 lbl string] Circle\n"
  "  (Circle {:radius r :label lbl}))\n"
  "(fn circle-radius [c Circle] f64 (get c radius))\n"
  "\n"
  "(struct Entity [mesh Mesh^ id i32])\n"
  "(fn make-entity [m Mesh^ eid i32] Entity\n"
  "  (Entity {:mesh m :id eid}))\n"
  "(fn entity-mesh [e Entity] Mesh^ (get e mesh))\n"
  "(fn entity-id [e Entity] i32 (get e id))\n"
  "\n"
  "(fn main [] i32 0)\n";

static Checker
check_fixture(TypedAst* tast, TypedIndex* out_root) {
  Ast ast;
  ast_init(&ast, ctx_perm());

  String8 src     = str8_cstring((char*)(g_fixture_source));
  u32     file_id = source_file_register(str8_lit("bcgen_types_test_fixture.3b"), src);

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
  u32 fadd_fn             = bc_program_find_fn(&prog, str8_lit("fadd"));
  u32 fsub_fn             = bc_program_find_fn(&prog, str8_lit("fsub"));
  u32 fmul_fn             = bc_program_find_fn(&prog, str8_lit("fmul"));
  u32 fdiv_fn             = bc_program_find_fn(&prog, str8_lit("fdiv"));
  u32 flt_fn              = bc_program_find_fn(&prog, str8_lit("flt"));
  u32 make_circle_lit_fn  = bc_program_find_fn(&prog, str8_lit("make-circle-lit"));
  u32 make_circle_param_fn = bc_program_find_fn(&prog, str8_lit("make-circle-param"));
  u32 circle_radius_fn    = bc_program_find_fn(&prog, str8_lit("circle-radius"));
  u32 make_entity_fn      = bc_program_find_fn(&prog, str8_lit("make-entity"));
  u32 entity_mesh_fn      = bc_program_find_fn(&prog, str8_lit("entity-mesh"));
  u32 entity_id_fn        = bc_program_find_fn(&prog, str8_lit("entity-id"));

  Arena* heap = ctx_perm();

  // ~~ f64 arithmetic/comparisons.
  { i64 args[2] = { f64_bits(2.5), f64_bits(4.25) };
    expect_eq_f64("fadd(2.5,4.25)", bc_run_in_program(&prog, fadd_fn, args, 2, heap, &host_imports).value, 6.75); }
  { i64 args[2] = { f64_bits(10.0), f64_bits(3.5) };
    expect_eq_f64("fsub(10,3.5)", bc_run_in_program(&prog, fsub_fn, args, 2, heap, &host_imports).value, 6.5); }
  { i64 args[2] = { f64_bits(3.0), f64_bits(4.0) };
    expect_eq_f64("fmul(3,4)", bc_run_in_program(&prog, fmul_fn, args, 2, heap, &host_imports).value, 12.0); }
  { i64 args[2] = { f64_bits(10.0), f64_bits(4.0) };
    expect_eq_f64("fdiv(10,4)", bc_run_in_program(&prog, fdiv_fn, args, 2, heap, &host_imports).value, 2.5); }
  { i64 args[2] = { f64_bits(1.0), f64_bits(2.0) };
    expect_eq_i64("flt(1,2)", bc_run_in_program(&prog, flt_fn, args, 2, heap, &host_imports).value, 1); }
  { i64 args[2] = { f64_bits(2.0), f64_bits(1.0) };
    expect_eq_i64("flt(2,1)", bc_run_in_program(&prog, flt_fn, args, 2, heap, &host_imports).value, 0); }

  // ~~ f64 + string struct fields.
  { i64 args[1] = { f64_bits(9.5) };
    BcResult r = bc_run_in_program(&prog, make_circle_lit_fn, args, 1, heap, &host_imports);
    i64 circle_ptr = r.value;
    { i64 rargs[1] = { circle_ptr };
      expect_eq_f64("circle-radius(make-circle-lit(9.5)).radius", bc_run_in_program(&prog, circle_radius_fn, rargs, 1, heap, &host_imports).value, 9.5); }
    // Read the embedded string field's raw bytes directly through the
    // returned struct pointer -- radius (f64, offset 0) then label
    // (string, offset 8: {u8* str; u64 size;}) per layout.c's own String
    // primitive.
    u8* base = (u8*)(intptr_t)circle_ptr;
    u8*  label_str;  memcpy(&label_str,  base + 8, sizeof(label_str));
    u64  label_size; memcpy(&label_size, base + 16, sizeof(label_size));
    if (label_size != 5 || memcmp(label_str, "hello", 5) != 0) {
      fprintf(stderr, "FAIL make-circle-lit(9.5).label: got %.*s (size %llu), want \"hello\" (size 5)\n",
              (int)label_size, (char*)label_str, (unsigned long long)label_size);
      g_failures += 1;
    }
  }

  { const char* lbl_data = "orbit";
    String8 lbl = { (u8*)lbl_data, 5 };
    // A string PARAMETER (not a literal) filled into a struct field -- the
    // runtime-copy path (LoadFieldI64 x2 / StoreFieldI64 x2), not the
    // compile-time bc_fill_string_field path make-circle-lit exercises above.
    // bc_run_in_program only takes i64 args, so pass the already-boxed
    // {ptr,size} header's OWN address the same way bc_compile_string_literal
    // would have produced it.
    i64 args[2] = { f64_bits(3.0), (i64)(intptr_t)&lbl };
    BcResult r = bc_run_in_program(&prog, make_circle_param_fn, args, 2, heap, &host_imports);
    i64 circle_ptr = r.value;
    u8* base = (u8*)(intptr_t)circle_ptr;
    u8*  label_str;  memcpy(&label_str,  base + 8, sizeof(label_str));
    u64  label_size; memcpy(&label_size, base + 16, sizeof(label_size));
    if (label_size != 5 || memcmp(label_str, "orbit", 5) != 0) {
      fprintf(stderr, "FAIL make-circle-param(3.0,\"orbit\").label: got %.*s (size %llu), want \"orbit\" (size 5)\n",
              (int)label_size, (char*)label_str, (unsigned long long)label_size);
      g_failures += 1;
    }
  }

  // ~~ Handle-typed field/param/return -- round-tripped as an opaque
  // 8-byte value, no handle-pool builtins involved.
  { i64 fake_handle = 0x0000000300000005LL; // arbitrary bit pattern -- never interpreted, just round-tripped
    i64 args[2] = { fake_handle, 42 };
    BcResult r = bc_run_in_program(&prog, make_entity_fn, args, 2, heap, &host_imports);
    i64 entity_ptr = r.value;
    { i64 margs[1] = { entity_ptr };
      expect_eq_i64("entity-mesh(make-entity(h,42))", bc_run_in_program(&prog, entity_mesh_fn, margs, 1, heap, &host_imports).value, fake_handle); }
    { i64 iargs[1] = { entity_ptr };
      expect_eq_i64("entity-id(make-entity(h,42))", bc_run_in_program(&prog, entity_id_fn, iargs, 1, heap, &host_imports).value, 42); }
  }

  arena_temp_end(&fn_temp);

  if (g_failures == 0) printf("bcgen_types_test: all checks passed\n");
  else                 printf("bcgen_types_test: %d check(s) FAILED\n", g_failures);

  ctx_free();
  return g_failures == 0 ? 0 : 1;
}
