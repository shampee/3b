// bcgen_map_test.c -- validates `Map<K,V>`/`Set<T>` support added to
// bcgen.c/bcvm.c/bcmap.c: BcOp_MapSet/MapGet/MapRemove, backing map-set/
// map-get/map-contains?/map-remove/set-add/set-contains?/set-remove, PLUS
// Map/Set collection `for` (TypedNodeKind_ForEachExpr's new branch). A
// real gap found by running examples/{map,set,type-shorthand}/main.3b
// through the bytecode VM -- previously "no compiled function or
// registered host import by that name" for every one of these. See
// bcmap.h/bcmap.c for the actual algorithm (ONE generic runtime
// implementation, parameterized by a compile-time-computed
// BcHashSlotLayout descriptor, rather than per-(K,V)-monomorphization
// generated code the way codegen.c's native backend needs). Same rig as
// the other bcgen_*_test.c files.
//
// Exercises:
//  - map-set/map-get/map-contains?/map-remove on a string-keyed AND an
//    i32-keyed Map, including overwriting an existing key and a resize
//    (200 insertions, forcing at least one capacity doubling).
//  - A Map whose VALUE is a STRUCT -- exercises the embedded-value byte-
//    copy path, not just a scalar value.
//  - set-add/set-contains?/set-remove, including the "already present"
//    case (set-add returns false, no mutation) -- the one place bc_map_
//    set's own Map-vs-Set branch actually differs in behavior.
//  - A never-initialized (still all-zero) Map/Set -- map-get/contains?/
//    remove must return NULL/false without crashing (matches
//    bc_map_ensure_init's own lazy-init contract).
//  - Map iteration (`[[k v] m]`) and Set iteration (both `[x s]` and
//    `[[i x] s]`), checking both the total count AND an accumulated sum
//    (proves the slot walk visits every OCCUPIED slot exactly once, not
//    just that SOME loop runs).
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
  "(package bcgen_map_test)\n"
  "\n"
  "(struct Player [name string level i32])\n"
  "\n"
  "(fn string-map-roundtrip [a arena] i32\n"
  "  (var m (Map string i32))\n"
  "  (map-set a m \"one\" 1)\n"
  "  (map-set a m \"two\" 2)\n"
  "  (map-set a m \"two\" 22) ;; overwrite\n"
  "  (var ok bool true)\n"
  "  (when (not (map-contains? m \"two\")) (set ok false))\n"
  "  (when (map-contains? m \"missing\") (set ok false))\n"
  "  (let [v (map-get m \"two\")]\n"
  "    (when (or (not v) (not (= (deref v) 22))) (set ok false)))\n"
  "  (when (map-get m \"missing\") (set ok false))\n"
  "  (map-remove m \"two\")\n"
  "  (when (map-contains? m \"two\") (set ok false))\n"
  "  (when (not (map-contains? m \"one\")) (set ok false))\n"
  "  (if ok 1 0))\n"
  "\n"
  "(fn numeric-map-resize [a arena] i32\n"
  "  (var m (Map i32 i32))\n"
  "  (for [i 0 200] (map-set a m i (* i i)))\n"
  "  (var ok bool true)\n"
  "  (for [i 0 200]\n"
  "    (let [p (map-get m i)]\n"
  "      (when (or (not p) (not (= (deref p) (* i i)))) (set ok false))))\n"
  "  (when (map-get m 99999) (set ok false))\n"
  "  (if ok 1 0))\n"
  "\n"
  "(fn struct-value-map [a arena] i32\n"
  "  (var m (Map string Player))\n"
  "  (map-set a m \"p1\" (Player {:name \"Ada\" :level 7}))\n"
  "  (let [p (map-get m \"p1\")]\n"
  "    (if (and p (= (. (deref p) level) 7)) 1 0)))\n"
  "\n"
  "(fn set-roundtrip [a arena] i32\n"
  "  (var s (Set string))\n"
  "  (val added-a bool (set-add a s \"a\"))\n"
  "  (set-add a s \"b\")\n"
  "  (val added-a-again bool (set-add a s \"a\"))\n"
  "  (var ok bool true)\n"
  "  (when (not added-a) (set ok false))\n"
  "  (when added-a-again (set ok false)) ;; already present -- no re-add\n"
  "  (when (not (set-contains? s \"b\")) (set ok false))\n"
  "  (set-remove s \"b\")\n"
  "  (when (set-contains? s \"b\") (set ok false))\n"
  "  (when (not (set-contains? s \"a\")) (set ok false))\n"
  "  (if ok 1 0))\n"
  "\n"
  "(fn never-initialized-map [] i32\n"
  "  (var m (Map string i32))\n"
  "  (if (and (not (map-contains? m \"x\")) (not (map-get m \"x\")) (not (map-remove m \"x\"))) 1 0))\n"
  "\n"
  "(fn never-initialized-set [] i32\n"
  "  (var s (Set i32))\n"
  "  (if (and (not (set-contains? s 5)) (not (set-remove s 5))) 1 0))\n"
  "\n"
  "(fn map-iteration [a arena] i32\n"
  "  (var m (Map i32 i32))\n"
  "  (for [i 0 50] (map-set a m i (* i 2)))\n"
  "  (var count i32 0)\n"
  "  (var val-sum i32 0)\n"
  "  (for [[k v] m]\n"
  "    (set count (+ count 1))\n"
  "    (set val-sum (+ val-sum v)))\n"
  "  (+ (* count 1000000) val-sum))\n"
  "\n"
  "(fn set-iteration-element-only [a arena] i32\n"
  "  (var s (Set i32))\n"
  "  (for [i 0 50] (set-add a s i))\n"
  "  (var count i32 0)\n"
  "  (var elem-sum i32 0)\n"
  "  (for [x s]\n"
  "    (set count (+ count 1))\n"
  "    (set elem-sum (+ elem-sum x)))\n"
  "  (+ (* count 1000000) elem-sum))\n"
  "\n"
  "(fn set-iteration-with-index [a arena] i32\n"
  "  (var s (Set i32))\n"
  "  (for [i 0 50] (set-add a s i))\n"
  "  (var count i32 0)\n"
  "  (var elem-sum i32 0)\n"
  "  (for [[idx x] s]\n"
  "    (set count (+ count 1))\n"
  "    (set elem-sum (+ elem-sum x)))\n"
  "  (+ (* count 1000000) elem-sum))\n"
  "\n"
  "(fn main [] i32 0)\n";

static Checker
check_fixture(TypedAst* tast, TypedIndex* out_root) {
  Ast ast;
  ast_init(&ast, ctx_perm());

  String8 src     = str8_cstring((char*)(g_fixture_source));
  u32     file_id = source_file_register(str8_lit("bcgen_map_test_fixture.3b"), src);

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

  u32 string_map_fn      = bc_program_find_fn(&prog, str8_lit("string-map-roundtrip"));
  u32 numeric_resize_fn   = bc_program_find_fn(&prog, str8_lit("numeric-map-resize"));
  u32 struct_value_fn      = bc_program_find_fn(&prog, str8_lit("struct-value-map"));
  u32 set_roundtrip_fn      = bc_program_find_fn(&prog, str8_lit("set-roundtrip"));
  u32 never_init_map_fn      = bc_program_find_fn(&prog, str8_lit("never-initialized-map"));
  u32 never_init_set_fn        = bc_program_find_fn(&prog, str8_lit("never-initialized-set"));
  u32 map_iter_fn                = bc_program_find_fn(&prog, str8_lit("map-iteration"));
  u32 set_iter_elem_fn             = bc_program_find_fn(&prog, str8_lit("set-iteration-element-only"));
  u32 set_iter_idx_fn                = bc_program_find_fn(&prog, str8_lit("set-iteration-with-index"));

  Arena* heap = ctx_perm();
  Arena  data_arena = arena_create_vm(MB(1));
  i64    args[1] = {(i64)(intptr_t)&data_arena};

  expect_eq_i64("string-map-roundtrip()", bc_run_in_program(&prog, string_map_fn,    args, 1, heap, &host_imports).value, 1);
  expect_eq_i64("numeric-map-resize()",   bc_run_in_program(&prog, numeric_resize_fn, args, 1, heap, &host_imports).value, 1);
  expect_eq_i64("struct-value-map()",     bc_run_in_program(&prog, struct_value_fn,   args, 1, heap, &host_imports).value, 1);
  expect_eq_i64("set-roundtrip()",        bc_run_in_program(&prog, set_roundtrip_fn,  args, 1, heap, &host_imports).value, 1);
  expect_eq_i64("never-initialized-map()", bc_run_in_program(&prog, never_init_map_fn, NULL, 0, heap, &host_imports).value, 1);
  expect_eq_i64("never-initialized-set()", bc_run_in_program(&prog, never_init_set_fn, NULL, 0, heap, &host_imports).value, 1);

  // sum(0..49)*2 = 1225*2 = 2450, count = 50 -> 50*1000000 + 2450
  expect_eq_i64("map-iteration()", bc_run_in_program(&prog, map_iter_fn, args, 1, heap, &host_imports).value, 50000000 + 2450);
  // sum(0..49) = 1225, count = 50 -> 50*1000000 + 1225
  expect_eq_i64("set-iteration-element-only()", bc_run_in_program(&prog, set_iter_elem_fn, args, 1, heap, &host_imports).value, 50000000 + 1225);
  expect_eq_i64("set-iteration-with-index()",   bc_run_in_program(&prog, set_iter_idx_fn,  args, 1, heap, &host_imports).value, 50000000 + 1225);

  arena_destroy(&data_arena);
  arena_temp_end(&fn_temp);

  if (g_failures == 0) printf("bcgen_map_test: all checks passed\n");
  else                 printf("bcgen_map_test: %d check(s) FAILED\n", g_failures);

  ctx_free();
  return g_failures == 0 ? 0 : 1;
}
