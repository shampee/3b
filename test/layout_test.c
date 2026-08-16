// layout_test.c -- validates layout.c two ways: (1) hand-derived expected
// values for every case, computed by the same System V rules layout.c
// itself claims to implement, and (2) a generated C probe compiled with
// the HOST's real `cc`, so drift from what an actual C compiler produces
// (the whole risk layout.c's own top-of-file comment calls out) shows up
// as a test failure instead of a silent future ABI mismatch.
//
// A standalone binary rather than part of another suite: this needs a live
// Checker (to call layout_of/
// layout_field_offset against), which compile_package/compile_all_packages
// deliberately don't expose past their own return (see PackageBuild's
// comment on why only resolved_types survives) -- so this hand-rolls the
// same parse -> lower -> check sequence compile_package uses internally,
// directly against an in-memory fixture, single file, no imports.
#include "3b.h"
#include "layout.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;

static void
expect_eq_u64(const char* what, u64 got, u64 want) {
  if (got != want) {
    fprintf(stderr, "FAIL %s: got %llu, want %llu\n", what, (unsigned long long)got, (unsigned long long)want);
    g_failures += 1;
  }
}

// ~~ Fixture: every shape layout.c's algorithm branches on -- plain
// primitives, a by-value nested struct, pointer/handle/array/string
// fields, `packed`, `align`, `union`, and an anonymous member.
static const char* g_fixture_source =
  "(package layout_test)\n"
  "(struct Sister [age i32])\n"
  "(struct Plain [a i8 b i32 c i8])\n"
  "(struct Nested [s Sister n i32])\n"
  "(struct PtrHolder [p i32* n i32])\n"
  "(struct StringHolder [s string n i32])\n"
  "(struct ArrayHolder [items [i32 4] count i32])\n"
  "(struct Mesh [id i32])\n"
  "(handle Mesh)\n"
  "(struct HandleHolder [h Mesh^ n i32])\n"
  "(packed (struct PackedS [a i8 b i32 c i8]))\n"
  "(align 16 (struct AlignedS [a i8]))\n"
  "(union UnionS [i i32 f f32 b [i8 8]])\n"
  "(struct Inner [ix i32 iy i32])\n"
  "(struct Outer [_ Inner extra i32])\n"
  "(fn main [] i32 0)\n";

static Checker
check_fixture(TypedAst* tast) {
  Ast ast;
  ast_init(&ast, ctx_perm());

  String8 src     = str8_cstring((char*)(g_fixture_source));
  u32     file_id = source_file_register(str8_lit("layout_test_fixture.3b"), src);

  Parser p;
  parser_init(&p, src, &ast, file_id);
  NodeIndex root = parse_program(&p);
  if (p.had_error) { fprintf(stderr, "FATAL: fixture failed to parse\n"); exit(1); }

  // Strip the leading `(package layout_test)` form -- compile_package does
  // this via validate_and_strip_package_form/strip_import_forms; there are
  // no imports here, so all that's needed is dropping child 0.
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

  return check_program(tast, own_root, /*is_root_package=*/true, /*scope_query=*/NULL);
}

typedef struct StructCase {
  const char* name;
  u64         expect_size;
  u64         expect_align;
} StructCase;

typedef struct FieldCase {
  const char* struct_name;
  const char* field_name;
  u64         expect_offset;
} FieldCase;

int
main(void) {
  Context ctx;
  ctx_init(&ctx, MB(16));

  TypedAst tast;
  Checker  ck = check_fixture(&tast);

  LayoutCache cache;
  layout_cache_init(&cache, ctx_perm());

  // ~~ Hand-derived expectations (System V: sequential fields, natural
  // alignment, no reordering; packed => align-1 fields, no trailing
  // padding; align(N) can only RAISE alignment; union => every field at
  // offset 0).
  StructCase struct_cases[] = {
    { "Sister",       4,  4 },
    { "Plain",        12, 4 },  // a@0(1) b@4(4, padded from 1) c@8(1) -> 9, rounded to align 4 -> 12
    { "Nested",       8,  4 },  // s@0(4) n@4(4) -> 8
    { "PtrHolder",    16, 8 },  // p@0(8) n@8(4) -> 12, rounded to align 8 -> 16
    { "StringHolder", 24, 8 },  // s@0(16) n@16(4) -> 20, rounded to align 8 -> 24
    { "ArrayHolder",  20, 4 },  // items@0(16, 4x i32) count@16(4) -> 20
    { "HandleHolder", 12, 4 },  // h@0(8, align 4) n@8(4) -> 12
    { "PackedS",      6,  1 },  // a@0(1) b@1(4) c@5(1) -> 6, no rounding (align 1)
    { "AlignedS",     16, 16 }, // a@0(1) -> 1, rounded to align(16) override -> 16
    { "UnionS",       8,  4 },  // max(i=4,f=4,b=8)=8, rounded to align 4 -> 8
    { "Inner",        8,  4 },  // ix@0(4) iy@4(4) -> 8
    { "Outer",        12, 4 },  // anon Inner@0(8) extra@8(4) -> 12
  };
  foreach_index(i, sizeof(struct_cases) / sizeof(struct_cases[0])) {
    StructCase*  sc = &struct_cases[i];
    StructEntry* se = struct_table_lookup(&ck, str8_cstring((char*)(sc->name)));
    if (!se) { fprintf(stderr, "FATAL: fixture struct `%s` not found\n", sc->name); exit(1); }
    Layout got = layout_of(&cache, &ck, type_ref_from_atom(ctx_perm(), str8_cstring((char*)(sc->name))));
    char buf[256];
    snprintf(buf, sizeof(buf), "%s.size", sc->name);  expect_eq_u64(buf, got.size,  sc->expect_size);
    snprintf(buf, sizeof(buf), "%s.align", sc->name); expect_eq_u64(buf, got.align, sc->expect_align);
  }

  FieldCase field_cases[] = {
    { "Plain",        "a",     0 }, { "Plain",        "b",     4 }, { "Plain", "c", 8 },
    { "Nested",       "s",     0 }, { "Nested",       "n",     4 },
    { "PtrHolder",    "p",     0 }, { "PtrHolder",    "n",     8 },
    { "StringHolder", "s",     0 }, { "StringHolder", "n",     16 },
    { "ArrayHolder",  "items", 0 }, { "ArrayHolder",  "count", 16 },
    { "HandleHolder", "h",     0 }, { "HandleHolder", "n",     8 },
    { "PackedS",      "a",     0 }, { "PackedS",      "b",     1 }, { "PackedS", "c", 5 },
    { "Outer",        "ix",    0 }, { "Outer",        "iy",    4 }, { "Outer",   "extra", 8 }, // flattened through the anon member
  };
  foreach_index(i, sizeof(field_cases) / sizeof(field_cases[0])) {
    FieldCase*   fc = &field_cases[i];
    StructEntry* se = struct_table_lookup(&ck, str8_cstring((char*)(fc->struct_name)));
    if (!se) { fprintf(stderr, "FATAL: fixture struct `%s` not found\n", fc->struct_name); exit(1); }
    FieldLayout got = layout_field_offset(&cache, &ck, se, str8_cstring((char*)(fc->field_name)));
    char buf[256];
    snprintf(buf, sizeof(buf), "%s.%s offset", fc->struct_name, fc->field_name);
    if (!got.found) { fprintf(stderr, "FAIL %s: field not found\n", buf); g_failures += 1; }
    else            { expect_eq_u64(buf, got.offset, fc->expect_offset); }
  }

  // ~~ Non-struct primitives (bare TypeRefs, no fixture struct needed --
  // these sizes/aligns come straight from base.h/runtime/bbb_hashtable.h,
  // see layout.c's own comments).
  TypeRef i32_ty = type_ref_from_atom(ctx_perm(), str8_lit("i32"));
  TypeRef handle_ty = (TypeRef){0};
  handle_ty.kind = TypeKind_Handle;
  handle_ty.name = str8_lit("Mesh");
  Layout handle_l = layout_of(&cache, &ck, handle_ty);
  expect_eq_u64("Handle.size", handle_l.size, 8); expect_eq_u64("Handle.align", handle_l.align, 4);

  TypeRef string_ty = (TypeRef){0}; string_ty.kind = TypeKind_String;
  Layout string_l = layout_of(&cache, &ck, string_ty);
  expect_eq_u64("String.size", string_l.size, 16); expect_eq_u64("String.align", string_l.align, 8);

  TypeRef arena_ty = (TypeRef){0}; arena_ty.kind = TypeKind_Arena;
  Layout arena_l = layout_of(&cache, &ck, arena_ty);
  expect_eq_u64("Arena.size", arena_l.size, 16); expect_eq_u64("Arena.align", arena_l.align, 8);

  TypeRef arena_mark_ty = (TypeRef){0}; arena_mark_ty.kind = TypeKind_ArenaMark;
  Layout arena_mark_l = layout_of(&cache, &ck, arena_mark_ty);
  expect_eq_u64("ArenaMark.size", arena_mark_l.size, 8); expect_eq_u64("ArenaMark.align", arena_mark_l.align, 8);

  TypeRef vector_ty = (TypeRef){0}; vector_ty.kind = TypeKind_Vector; vector_ty.pointee = &i32_ty;
  Layout vector_l = layout_of(&cache, &ck, vector_ty);
  expect_eq_u64("Vector.size", vector_l.size, 8); expect_eq_u64("Vector.align", vector_l.align, 8);

  TypeRef map_ty = (TypeRef){0}; map_ty.kind = TypeKind_Map; map_ty.map_key = &i32_ty; map_ty.pointee = &i32_ty;
  Layout map_l = layout_of(&cache, &ck, map_ty);
  expect_eq_u64("Map.size", map_l.size, 32); expect_eq_u64("Map.align", map_l.align, 8);

  TypeRef set_ty = (TypeRef){0}; set_ty.kind = TypeKind_Set; set_ty.pointee = &i32_ty;
  Layout set_l = layout_of(&cache, &ck, set_ty);
  expect_eq_u64("Set.size", set_l.size, 32); expect_eq_u64("Set.align", set_l.align, 8);

  // ~~ Cross-check against a REAL C compiler: generate a probe mirroring
  // exactly what cg_struct_decl (codegen.c) would emit for these same
  // struct shapes, compile it with the host's `cc`, and diff sizeof/
  // offsetof against layout.c's own numbers -- catches drift from the
  // real ABI, not just from this file's own hand math above.
  const char* probe_src =
    "#include <stdio.h>\n#include <stddef.h>\n#include <stdint.h>\n"
    "typedef int8_t i8; typedef int32_t i32; typedef float f32;\n"
    "typedef struct { uint32_t index; uint32_t generation; } MeshHandle;\n" // base.h's Handle
    "typedef struct { unsigned char* str; uint64_t size; } bbb_String8;\n"  // base.h's String8
    "typedef struct Sister { i32 age; } Sister;\n"
    "typedef struct Plain { i8 a; i32 b; i8 c; } Plain;\n"
    "typedef struct Nested { Sister s; i32 n; } Nested;\n"
    "typedef struct PtrHolder { i32* p; i32 n; } PtrHolder;\n"
    "typedef struct StringHolder { bbb_String8 s; i32 n; } StringHolder;\n"
    "typedef struct ArrayHolder { i32 items[4]; i32 count; } ArrayHolder;\n"
    "typedef struct Mesh { i32 id; } Mesh;\n"
    "typedef struct HandleHolder { MeshHandle h; i32 n; } HandleHolder;\n"
    "typedef struct PackedS { i8 a; i32 b; i8 c; } __attribute__((packed)) PackedS;\n"
    "typedef struct AlignedS { i8 a; } __attribute__((aligned(16))) AlignedS;\n"
    "typedef union UnionS { i32 i; f32 f; i8 b[8]; } UnionS;\n"
    "typedef struct Inner { i32 ix; i32 iy; } Inner;\n"
    "typedef struct Outer { struct { i32 ix; i32 iy; }; i32 extra; } Outer;\n"
    "int main(void) {\n"
    "#define SZ(T) printf(\"SIZE %s %zu %zu\\n\", #T, sizeof(T), _Alignof(T))\n"
    "#define OFF(T, F) printf(\"OFFSET %s %s %zu\\n\", #T, #F, offsetof(T, F))\n"
    "  SZ(Sister); SZ(Plain); SZ(Nested); SZ(PtrHolder); SZ(StringHolder); SZ(ArrayHolder);\n"
    "  SZ(HandleHolder); SZ(PackedS); SZ(AlignedS); SZ(UnionS); SZ(Inner); SZ(Outer);\n"
    "  OFF(Plain, a); OFF(Plain, b); OFF(Plain, c);\n"
    "  OFF(Nested, s); OFF(Nested, n);\n"
    "  OFF(PtrHolder, p); OFF(PtrHolder, n);\n"
    "  OFF(StringHolder, s); OFF(StringHolder, n);\n"
    "  OFF(ArrayHolder, items); OFF(ArrayHolder, count);\n"
    "  OFF(HandleHolder, h); OFF(HandleHolder, n);\n"
    "  OFF(PackedS, a); OFF(PackedS, b); OFF(PackedS, c);\n"
    "  OFF(Outer, ix); OFF(Outer, iy); OFF(Outer, extra);\n"
    "  return 0;\n}\n";

  const char* probe_path = "/tmp/3b_layout_probe.c";
  const char* probe_bin  = "/tmp/3b_layout_probe";
  FILE* pf = fopen(probe_path, "w");
  if (!pf) { fprintf(stderr, "FATAL: can't write probe source\n"); exit(1); }
  fputs(probe_src, pf);
  fclose(pf);

  char cmd[512];
  snprintf(cmd, sizeof(cmd), "cc -std=c11 -O0 -o %s %s 2>&1", probe_bin, probe_path);
  FILE* build_pipe = popen(cmd, "r");
  if (!build_pipe) { fprintf(stderr, "FATAL: can't invoke cc\n"); exit(1); }
  char line[256];
  b32 build_failed = false;
  while (fgets(line, sizeof(line), build_pipe)) { fputs(line, stderr); build_failed = true; }
  int build_status = pclose(build_pipe);
  if (build_status != 0) { fprintf(stderr, "FATAL: probe failed to compile\n"); exit(1); }
  (void)build_failed;

  FILE* run_pipe = popen(probe_bin, "r");
  if (!run_pipe) { fprintf(stderr, "FATAL: can't run probe\n"); exit(1); }
  while (fgets(line, sizeof(line), run_pipe)) {
    char kind[16], name[64], field[64];
    unsigned long long a, b;
    if (sscanf(line, "SIZE %63s %llu %llu", name, &a, &b) == 3) {
      StructEntry* se = struct_table_lookup(&ck, str8_cstring((char*)(name)));
      if (!se) continue; // Outer's anon inner isn't looked up this way; every case here is
      Layout got = layout_of(&cache, &ck, type_ref_from_atom(ctx_perm(), str8_cstring((char*)(name))));
      char buf[256];
      snprintf(buf, sizeof(buf), "gcc-cross-check %s.size", name);  expect_eq_u64(buf, got.size, (u64)a);
      snprintf(buf, sizeof(buf), "gcc-cross-check %s.align", name); expect_eq_u64(buf, got.align, (u64)b);
    } else if (sscanf(line, "OFFSET %63s %63s %llu", name, field, &a) == 3) {
      StructEntry* se = struct_table_lookup(&ck, str8_cstring((char*)(name)));
      if (!se) continue;
      FieldLayout got = layout_field_offset(&cache, &ck, se, str8_cstring((char*)(field)));
      char buf[256];
      snprintf(buf, sizeof(buf), "gcc-cross-check %s.%s offset", name, field);
      if (!got.found) { fprintf(stderr, "FAIL %s: field not found\n", buf); g_failures += 1; }
      else            { expect_eq_u64(buf, got.offset, (u64)a); }
    }
    (void)kind;
  }
  pclose(run_pipe);

  if (g_failures == 0) printf("layout_test: all checks passed\n");
  else                 printf("layout_test: %d check(s) FAILED\n", g_failures);

  ctx_free();
  return g_failures == 0 ? 0 : 1;
}
