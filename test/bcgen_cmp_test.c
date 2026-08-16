// bcgen_cmp_test.c -- validates bcgen.c's string/struct CONTENT
// comparison (`=`/`!=`/`<`/`<=`/`>`/`>=`), fixed after being caught as a
// real gap while testing bcosprims.c: TypedNodeKind_BinaryEq/etc used to
// fall through to plain integer register comparison for ANY non-float
// operand, which compared string/struct values' own ADDRESSES rather
// than their content (see bcgen.c's own top-of-file note on
// bc_compile_value_cmp for the full story). Same rig as the other
// bcgen_*_test.c files.
//
// Exercises:
//  - String equality/ordering, including the two genuinely tricky cases
//    a naive byte loop gets wrong: one string a strict PREFIX of the
//    other (shorter-is-less), and a case-sensitivity check (byte value,
//    not locale collation).
//  - Struct equality/ordering for FOUR shapes, matching the FOUR distinct
//    field-kind branches bc_compile_value_cmp itself has: a flat scalar
//    struct (Vector2), a struct with a STRING field (Named), a struct
//    with a NESTED STRUCT field (Line), and a struct with an ARRAY field
//    (Triple) -- the array case specifically exercises the real
//    jump-backpatched loop bc_compile_array_cmp emits, not just a single
//    element.
//  - THE key regression check throughout: every "equal" case below
//    constructs its two operands SEPARATELY (two distinct calls to a
//    `make-*` function, two distinct heap addresses) -- if content
//    comparison were STILL secretly comparing addresses, every one of
//    these would incorrectly report "not equal" instead of "equal".
//  - Ordering correctness for nested/array fields: first-differing-field-
//    wins (Line), first-differing-element-wins (Triple) -- not just "are
//    they equal or not".
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

static const char* g_fixture_source =
  "(package bcgen_cmp_test)\n"
  "\n"
  "(fn str-eq [a string b string] bool (= a b))\n"
  "(fn str-neq [a string b string] bool (!= a b))\n"
  "(fn str-lt [a string b string] bool (< a b))\n"
  "(fn str-le [a string b string] bool (<= a b))\n"
  "(fn str-gt [a string b string] bool (> a b))\n"
  "(fn str-ge [a string b string] bool (>= a b))\n"
  "\n"
  "(struct Vector2 [x i32 y i32])\n"
  "(fn make-vec2 [x i32 y i32] Vector2 (Vector2 {:x x :y y}))\n"
  "(fn vec2-eq [a Vector2 b Vector2] bool (= a b))\n"
  "(fn vec2-lt [a Vector2 b Vector2] bool (< a b))\n"
  "\n"
  "(struct Named [label string value i32])\n"
  "(fn make-named [label string value i32] Named (Named {:label label :value value}))\n"
  "(fn named-eq [a Named b Named] bool (= a b))\n"
  "\n"
  "(struct Line [from Vector2 to Vector2])\n"
  "(fn make-line [ax i32 ay i32 bx i32 by i32] Line\n"
  "  (Line {:from (Vector2 {:x ax :y ay}) :to (Vector2 {:x bx :y by})}))\n"
  "(fn line-eq [a Line b Line] bool (= a b))\n"
  "(fn line-lt [a Line b Line] bool (< a b))\n"
  "\n"
  "(struct Triple [verts [i32 3]])\n"
  "(fn make-triple [a i32 b i32 c i32] Triple (Triple {:verts [a b c]}))\n"
  "(fn triple-eq [a Triple b Triple] bool (= a b))\n"
  "(fn triple-lt [a Triple b Triple] bool (< a b))\n"
  "\n"
  "(fn main [] i32 0)\n";

static Checker
check_fixture(TypedAst* tast, TypedIndex* out_root) {
  Ast ast;
  ast_init(&ast, ctx_perm());

  String8 src     = str8_cstring((char*)(g_fixture_source));
  u32     file_id = source_file_register(str8_lit("bcgen_cmp_test_fixture.3b"), src);

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

static i64
box_string_arg(Arena* arena, const char* s) {
  String8* header = push_one(arena, String8);
  header->size = strlen(s);
  header->str  = header->size > 0 ? push_array(arena, u8, header->size) : NULL;
  if (header->size > 0) MemoryCopy(header->str, s, header->size);
  return (i64)(intptr_t)header;
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
  BcHostImportTable host_imports = {0};

  BcProgram prog = bc_compile_program(&ck, &tast, root, fn_temp.arena, ctx_perm(), &layout_cache, &host_imports, NULL);

  u32 str_eq_fn  = bc_program_find_fn(&prog, str8_lit("str-eq"));
  u32 str_neq_fn = bc_program_find_fn(&prog, str8_lit("str-neq"));
  u32 str_lt_fn  = bc_program_find_fn(&prog, str8_lit("str-lt"));
  u32 str_le_fn  = bc_program_find_fn(&prog, str8_lit("str-le"));
  u32 str_gt_fn  = bc_program_find_fn(&prog, str8_lit("str-gt"));
  u32 str_ge_fn  = bc_program_find_fn(&prog, str8_lit("str-ge"));

  u32 make_vec2_fn = bc_program_find_fn(&prog, str8_lit("make-vec2"));
  u32 vec2_eq_fn   = bc_program_find_fn(&prog, str8_lit("vec2-eq"));
  u32 vec2_lt_fn   = bc_program_find_fn(&prog, str8_lit("vec2-lt"));

  u32 make_named_fn = bc_program_find_fn(&prog, str8_lit("make-named"));
  u32 named_eq_fn    = bc_program_find_fn(&prog, str8_lit("named-eq"));

  u32 make_line_fn = bc_program_find_fn(&prog, str8_lit("make-line"));
  u32 line_eq_fn    = bc_program_find_fn(&prog, str8_lit("line-eq"));
  u32 line_lt_fn    = bc_program_find_fn(&prog, str8_lit("line-lt"));

  u32 make_triple_fn = bc_program_find_fn(&prog, str8_lit("make-triple"));
  u32 triple_eq_fn    = bc_program_find_fn(&prog, str8_lit("triple-eq"));
  u32 triple_lt_fn    = bc_program_find_fn(&prog, str8_lit("triple-lt"));

  Arena* heap = ctx_perm();

  // ~~ String equality/inequality.
  { i64 args[2] = { box_string_arg(heap, "hello"), box_string_arg(heap, "hello") };
    expect_eq_i64("str-eq(\"hello\",\"hello\")", bc_run_in_program(&prog, str_eq_fn, args, 2, heap, &host_imports).value, 1); }
  { i64 args[2] = { box_string_arg(heap, "hello"), box_string_arg(heap, "world") };
    expect_eq_i64("str-eq(\"hello\",\"world\")", bc_run_in_program(&prog, str_eq_fn, args, 2, heap, &host_imports).value, 0); }
  { i64 args[2] = { box_string_arg(heap, "hello"), box_string_arg(heap, "world") };
    expect_eq_i64("str-neq(\"hello\",\"world\")", bc_run_in_program(&prog, str_neq_fn, args, 2, heap, &host_imports).value, 1); }

  // ~~ String ordering -- byte-diff, prefix/shorter-is-less, case sensitivity.
  { i64 args[2] = { box_string_arg(heap, "abc"), box_string_arg(heap, "abd") };
    expect_eq_i64("str-lt(\"abc\",\"abd\")", bc_run_in_program(&prog, str_lt_fn, args, 2, heap, &host_imports).value, 1); }
  { i64 args[2] = { box_string_arg(heap, "ab"), box_string_arg(heap, "abc") };
    expect_eq_i64("str-lt(\"ab\",\"abc\") -- shorter prefix is less", bc_run_in_program(&prog, str_lt_fn, args, 2, heap, &host_imports).value, 1); }
  { i64 args[2] = { box_string_arg(heap, "abc"), box_string_arg(heap, "ab") };
    expect_eq_i64("str-lt(\"abc\",\"ab\") is false", bc_run_in_program(&prog, str_lt_fn, args, 2, heap, &host_imports).value, 0); }
  { i64 args[2] = { box_string_arg(heap, "abc"), box_string_arg(heap, "ab") };
    expect_eq_i64("str-gt(\"abc\",\"ab\")", bc_run_in_program(&prog, str_gt_fn, args, 2, heap, &host_imports).value, 1); }
  { i64 args[2] = { box_string_arg(heap, "abc"), box_string_arg(heap, "abc") };
    expect_eq_i64("str-le(\"abc\",\"abc\")", bc_run_in_program(&prog, str_le_fn, args, 2, heap, &host_imports).value, 1); }
  { i64 args[2] = { box_string_arg(heap, "abc"), box_string_arg(heap, "abc") };
    expect_eq_i64("str-ge(\"abc\",\"abc\")", bc_run_in_program(&prog, str_ge_fn, args, 2, heap, &host_imports).value, 1); }
  { i64 args[2] = { box_string_arg(heap, "Zebra"), box_string_arg(heap, "apple") };
    expect_eq_i64("str-lt(\"Zebra\",\"apple\") -- byte value ('Z'=90 < 'a'=97), not locale collation",
                  bc_run_in_program(&prog, str_lt_fn, args, 2, heap, &host_imports).value, 1); }

  // ~~ Flat scalar struct (Vector2) -- THE key regression check: two
  // SEPARATELY constructed structs with identical content must compare
  // equal (address comparison would always say "not equal" here).
  { i64 a1[2] = {1, 2}; i64 a2[2] = {1, 2};
    i64 p1 = bc_run_in_program(&prog, make_vec2_fn, a1, 2, heap, &host_imports).value;
    i64 p2 = bc_run_in_program(&prog, make_vec2_fn, a2, 2, heap, &host_imports).value;
    xassert(p1 != p2); // sanity: genuinely two different addresses
    i64 eq_args[2] = { p1, p2 };
    expect_eq_i64("vec2-eq(make-vec2(1,2), make-vec2(1,2)) -- separately constructed, same content",
                  bc_run_in_program(&prog, vec2_eq_fn, eq_args, 2, heap, &host_imports).value, 1);
  }
  { i64 a1[2] = {1, 2}; i64 a2[2] = {1, 3};
    i64 p1 = bc_run_in_program(&prog, make_vec2_fn, a1, 2, heap, &host_imports).value;
    i64 p2 = bc_run_in_program(&prog, make_vec2_fn, a2, 2, heap, &host_imports).value;
    i64 eq_args[2] = { p1, p2 };
    expect_eq_i64("vec2-eq(make-vec2(1,2), make-vec2(1,3)) -- differ in y", bc_run_in_program(&prog, vec2_eq_fn, eq_args, 2, heap, &host_imports).value, 0);
    expect_eq_i64("vec2-lt(make-vec2(1,2), make-vec2(1,3))", bc_run_in_program(&prog, vec2_lt_fn, eq_args, 2, heap, &host_imports).value, 1);
  }
  { i64 a1[2] = {1, 3}; i64 a2[2] = {2, 1};
    i64 p1 = bc_run_in_program(&prog, make_vec2_fn, a1, 2, heap, &host_imports).value;
    i64 p2 = bc_run_in_program(&prog, make_vec2_fn, a2, 2, heap, &host_imports).value;
    i64 lt_args[2] = { p1, p2 };
    expect_eq_i64("vec2-lt(make-vec2(1,3), make-vec2(2,1)) -- x (first field) wins regardless of y",
                  bc_run_in_program(&prog, vec2_lt_fn, lt_args, 2, heap, &host_imports).value, 1);
  }

  // ~~ Struct with a STRING field (Named).
  { i64 a1[2] = { box_string_arg(heap, "foo"), 1 }; i64 a2[2] = { box_string_arg(heap, "foo"), 1 };
    i64 p1 = bc_run_in_program(&prog, make_named_fn, a1, 2, heap, &host_imports).value;
    i64 p2 = bc_run_in_program(&prog, make_named_fn, a2, 2, heap, &host_imports).value;
    i64 eq_args[2] = { p1, p2 };
    expect_eq_i64("named-eq(same label+value, separately constructed)", bc_run_in_program(&prog, named_eq_fn, eq_args, 2, heap, &host_imports).value, 1);
  }
  { i64 a1[2] = { box_string_arg(heap, "foo"), 1 }; i64 a2[2] = { box_string_arg(heap, "bar"), 1 };
    i64 p1 = bc_run_in_program(&prog, make_named_fn, a1, 2, heap, &host_imports).value;
    i64 p2 = bc_run_in_program(&prog, make_named_fn, a2, 2, heap, &host_imports).value;
    i64 eq_args[2] = { p1, p2 };
    expect_eq_i64("named-eq(different label) is false", bc_run_in_program(&prog, named_eq_fn, eq_args, 2, heap, &host_imports).value, 0);
  }
  { i64 a1[2] = { box_string_arg(heap, "foo"), 1 }; i64 a2[2] = { box_string_arg(heap, "foo"), 2 };
    i64 p1 = bc_run_in_program(&prog, make_named_fn, a1, 2, heap, &host_imports).value;
    i64 p2 = bc_run_in_program(&prog, make_named_fn, a2, 2, heap, &host_imports).value;
    i64 eq_args[2] = { p1, p2 };
    expect_eq_i64("named-eq(different value) is false", bc_run_in_program(&prog, named_eq_fn, eq_args, 2, heap, &host_imports).value, 0);
  }

  // ~~ Struct with a NESTED STRUCT field (Line: {from Vector2, to Vector2}).
  { i64 a1[4] = {1,2,3,4}; i64 a2[4] = {1,2,3,4};
    i64 p1 = bc_run_in_program(&prog, make_line_fn, a1, 4, heap, &host_imports).value;
    i64 p2 = bc_run_in_program(&prog, make_line_fn, a2, 4, heap, &host_imports).value;
    i64 eq_args[2] = { p1, p2 };
    expect_eq_i64("line-eq(same coords, separately constructed)", bc_run_in_program(&prog, line_eq_fn, eq_args, 2, heap, &host_imports).value, 1);
  }
  { i64 a1[4] = {1,2,3,4}; i64 a2[4] = {1,2,3,5};
    i64 p1 = bc_run_in_program(&prog, make_line_fn, a1, 4, heap, &host_imports).value;
    i64 p2 = bc_run_in_program(&prog, make_line_fn, a2, 4, heap, &host_imports).value;
    i64 eq_args[2] = { p1, p2 };
    expect_eq_i64("line-eq(differ in to.y) is false", bc_run_in_program(&prog, line_eq_fn, eq_args, 2, heap, &host_imports).value, 0);
    expect_eq_i64("line-lt(differ in nested to.y)", bc_run_in_program(&prog, line_lt_fn, eq_args, 2, heap, &host_imports).value, 1);
  }

  // ~~ Struct with an ARRAY field (Triple: {verts [i32 3]}) -- exercises
  // the real jump-backpatched loop, not a single comparison.
  { i64 a1[3] = {1,2,3}; i64 a2[3] = {1,2,3};
    i64 p1 = bc_run_in_program(&prog, make_triple_fn, a1, 3, heap, &host_imports).value;
    i64 p2 = bc_run_in_program(&prog, make_triple_fn, a2, 3, heap, &host_imports).value;
    i64 eq_args[2] = { p1, p2 };
    expect_eq_i64("triple-eq(same verts, separately constructed)", bc_run_in_program(&prog, triple_eq_fn, eq_args, 2, heap, &host_imports).value, 1);
  }
  { i64 a1[3] = {1,2,3}; i64 a2[3] = {1,2,4};
    i64 p1 = bc_run_in_program(&prog, make_triple_fn, a1, 3, heap, &host_imports).value;
    i64 p2 = bc_run_in_program(&prog, make_triple_fn, a2, 3, heap, &host_imports).value;
    i64 eq_args[2] = { p1, p2 };
    expect_eq_i64("triple-eq(differ at LAST element) is false", bc_run_in_program(&prog, triple_eq_fn, eq_args, 2, heap, &host_imports).value, 0);
    expect_eq_i64("triple-lt(differ at last element)", bc_run_in_program(&prog, triple_lt_fn, eq_args, 2, heap, &host_imports).value, 1);
  }
  { i64 a1[3] = {1,5,3}; i64 a2[3] = {2,1,1};
    i64 p1 = bc_run_in_program(&prog, make_triple_fn, a1, 3, heap, &host_imports).value;
    i64 p2 = bc_run_in_program(&prog, make_triple_fn, a2, 3, heap, &host_imports).value;
    i64 lt_args[2] = { p1, p2 };
    expect_eq_i64("triple-lt: FIRST differing element wins (index 0: 1<2)",
                  bc_run_in_program(&prog, triple_lt_fn, lt_args, 2, heap, &host_imports).value, 1);
  }

  arena_temp_end(&fn_temp);

  if (g_failures == 0) printf("bcgen_cmp_test: all checks passed\n");
  else                 printf("bcgen_cmp_test: %d check(s) FAILED\n", g_failures);

  ctx_free();
  return g_failures == 0 ? 0 : 1;
}
