// bcgen_print_test.c -- validates `print`/`println` (special-cased inside
// TypedNodeKind_Call, same as `create`/`destroy`/etc.) support added to
// bcgen.c/bcvm.c: unlike codegen.c's native backend (which synthesizes ONE
// big C `printf` format string out of the template, escaping the user's
// own text), this backend has no such call to build -- it walks the
// (already checker-validated) template at COMPILE TIME and emits a flat
// SEQUENCE of new BcOp_Print* opcodes (one per literal chunk/placeholder),
// each doing exactly one fixed-format fprintf/fwrite call. Same rig as the
// other bcgen_*_test.c files, plus a portable stdout-capture helper since
// this is the first bcgen_*_test.c that needs to observe REAL program
// output rather than a returned register value.
//
// Exercises:
//  - Every printable TypeKind: signed/unsigned integers (including
//    negative), f32 (proves the widen-via-BcOp_F32ToF64 path) and f64,
//    bool, char, a `string`-typed value computed at RUNTIME (not just a
//    literal), and `char*` (cstring).
//  - `{{`/`}}` escapes rendering as literal braces.
//  - A literal `%` in the template passing through completely unescaped
//    (this backend never builds a printf format string out of user text
//    at all, so there's nothing for a stray `%` to be misread as -- a
//    REAL behavioral difference from the native backend, deliberately
//    verified here, not just asserted in a comment).
//  - `print` (no trailing newline) vs `println` (adds exactly one `\n`).
//  - Multiple print/println calls within one function concatenate in
//    the correct order.
//  - ARGUMENT EVALUATION ORDER (see the note above cg_print_hoist_args in
//    codegen.c for the language rule): arguments that themselves PRINT run
//    left to right and entirely BEFORE the line they are arguments to, so a
//    nested print can never land in the middle of it; and an argument is
//    snapshotted when evaluated, so a later argument assigning to the same
//    local doesn't retroactively change what an earlier `{}` shows. The
//    two backends used to print the same source DIFFERENTLY here -- native
//    compiles to a single printf, which cannot interleave, while this
//    backend emitted the template as it went.
//  - A TRAPPING argument (division by zero) leaving NOTHING on stdout, not
//    the half-written line the emit-as-you-go shape produced.
#include "3b.h"
#include "bcgen.h"
#include "bcvm.h"
#include <stdio.h>
#include <stdlib.h>
#if defined(_WIN32)
#include <io.h> // _dup/_dup2/_close/_fileno
#endif
#include <string.h>

static int g_failures = 0;

static void
expect_true(const char* what, b32 got) {
  if (!got) {
    fprintf(stderr, "FAIL %s: expected true\n", what);
    g_failures += 1;
  }
}

static void
expect_str_eq(const char* what, const char* got, const char* want) {
  if (strcmp(got, want) != 0) {
    fprintf(stderr, "FAIL %s: got %s%s%s, want %s%s%s\n", what, "\"", got, "\"", "\"", want, "\"");
    g_failures += 1;
  }
}

// Redirects stdout's underlying fd into an anonymous temp file for the
// duration of one `bc_run_in_program` call, then reads back exactly what
// was written -- portable POSIX/Windows split (dup2/fileno vs _dup2/
// _fileno), same convention test/bcosprims_test.c's own per-primitive
// #if defined(_WIN32) split already established.
//
// `out_result` (may be NULL) receives the run's BcResult, which the
// trapping-argument case needs: "nothing was printed" only means what it
// should if the run actually trapped rather than quietly returning.
static char*
capture_print_output(BcProgram* prog, u32 fn, Arena* heap, BcHostImportTable* host_imports,
                     BcResult* out_result) {
  fflush(stdout);
  FILE* tmpf = tmpfile();
  xassert(tmpf);
#if defined(_WIN32)
  int saved_fd = _dup(_fileno(stdout));
  _dup2(_fileno(tmpf), _fileno(stdout));
#else
  int saved_fd = dup(fileno(stdout));
  dup2(fileno(tmpf), fileno(stdout));
#endif

  BcResult result = bc_run_in_program(prog, fn, NULL, 0, heap, host_imports);
  if (out_result) *out_result = result;

  fflush(stdout);
#if defined(_WIN32)
  _dup2(saved_fd, _fileno(stdout));
  _close(saved_fd);
#else
  dup2(saved_fd, fileno(stdout));
  close(saved_fd);
#endif

  fseek(tmpf, 0, SEEK_END);
  long size = ftell(tmpf);
  fseek(tmpf, 0, SEEK_SET);
  char* buf = (char*)malloc((size_t)size + 1);
  xassert(buf);
  size_t got = fread(buf, 1, (size_t)size, tmpf);
  buf[got] = 0;
  fclose(tmpf);
  return buf;
}

static const char* g_fixture_source =
  "(package bcgen_print_test)\n"
  "\n"
  "(fn runtime-greeting [] string \"computed-at-runtime\")\n"
  "\n"
  "(fn print-mixed [] i32\n"
  "  (println \"i32={} u32={} i64={} u64={} f32={} f64={} bool={} char={} str={}\"\n"
  "    -7i32 42u32 -9000000000i64 9000000000u64 3.5f32 2.5f64 true (cast char 65) (runtime-greeting))\n"
  "  0)\n"
  "\n"
  "(fn print-escapes [] i32\n"
  "  (println \"literal braces: {{}} and a bare percent: 100% done\")\n"
  "  0)\n"
  "\n"
  "(fn print-no-newline [] i32\n"
  "  (print \"a\")\n"
  "  (print \"b\")\n"
  "  (println \"c\")\n"
  "  (print \"d\")\n"
  "  0)\n"
  "\n"
  "(fn print-cstring [] i32\n"
  "  (println \"cstr={}\" (cstring \"hi-cstring\"))\n"
  "  0)\n"
  "\n"
  ";; An argument with output of its own: everything it prints must appear\n"
  ";; BEFORE the line it is an argument to, never inside it.\n"
  "(fn noisy [n i32] i32\n"
  "  (print \"[noisy {}]\" n)\n"
  "  n)\n"
  "\n"
  "(fn print-inside-print [] i32\n"
  "  (println \"a={} b={}\" (noisy 1) (noisy 2))\n"
  "  0)\n"
  "\n"
  ";; A later argument assigning to the same local as an earlier one: `{}`\n"
  ";; number one shows the value its argument had when its turn came.\n"
  "(fn print-arg-snapshot [] i32\n"
  "  (var i i32 1)\n"
  "  (println \"{} then {}\" i (set i 5))\n"
  "  0)\n"
  "\n"
  ";; The same, for a `string` argument -- whose register is the ADDRESS of a\n"
  ";; boxed {ptr,size} header, so its snapshot has to be a header COPY. Here\n"
  ";; the later argument writes THROUGH the field address the earlier one was\n"
  ";; read from, which a register-only snapshot would not survive.\n"
  "(struct Boxed [name string])\n"
  "\n"
  "(fn print-string-arg-snapshot [] i32\n"
  "  (var p Boxed (Boxed {:name \"one\"}))\n"
  "  (println \"{} / {}\" (. p name) (set (. p name) \"two\"))\n"
  "  0)\n"
  "\n"
  ";; Divides by zero to trap PART WAY through the arguments -- with the\n"
  ";; literal text of the template on either side of it, so a backend that\n"
  ";; emitted as it went would leave `before ` on stdout.\n"
  "(fn print-trapping-arg [] i32\n"
  "  (var z i32 0)\n"
  "  (println \"before {} after\" (/ 10 z))\n"
  "  0)\n"
  "\n"
  "(fn main [] i32 0)\n";

static Checker
check_fixture(TypedAst* tast, TypedIndex* out_root) {
  Ast ast;
  ast_init(&ast, ctx_perm());

  String8 src     = str8_cstring((char*)(g_fixture_source));
  u32     file_id = source_file_register(str8_lit("bcgen_print_test_fixture.3b"), src);

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

  u32 print_mixed_fn      = bc_program_find_fn(&prog, str8_lit("print-mixed"));
  u32 print_escapes_fn      = bc_program_find_fn(&prog, str8_lit("print-escapes"));
  u32 print_no_newline_fn  = bc_program_find_fn(&prog, str8_lit("print-no-newline"));
  u32 print_cstring_fn      = bc_program_find_fn(&prog, str8_lit("print-cstring"));
  u32 print_inside_fn       = bc_program_find_fn(&prog, str8_lit("print-inside-print"));
  u32 print_snapshot_fn     = bc_program_find_fn(&prog, str8_lit("print-arg-snapshot"));
  u32 print_str_snapshot_fn = bc_program_find_fn(&prog, str8_lit("print-string-arg-snapshot"));
  u32 print_trapping_fn     = bc_program_find_fn(&prog, str8_lit("print-trapping-arg"));

  Arena* heap = ctx_perm();

  { char* out = capture_print_output(&prog, print_mixed_fn, heap, &host_imports, NULL);
    expect_str_eq("print-mixed()", out,
      "i32=-7 u32=42 i64=-9000000000 u64=9000000000 f32=3.5 f64=2.5 bool=true char=A str=computed-at-runtime\n");
    free(out);
  }
  { char* out = capture_print_output(&prog, print_escapes_fn, heap, &host_imports, NULL);
    expect_str_eq("print-escapes()", out, "literal braces: {} and a bare percent: 100% done\n");
    free(out);
  }
  { char* out = capture_print_output(&prog, print_no_newline_fn, heap, &host_imports, NULL);
    expect_str_eq("print-no-newline()", out, "abc\nd");
    free(out);
  }
  { char* out = capture_print_output(&prog, print_cstring_fn, heap, &host_imports, NULL);
    expect_str_eq("print-cstring()", out, "cstr=hi-cstring\n");
    free(out);
  }
  // Both nested prints complete, in source order, before `a=` -- NOT
  // "a=[noisy 1] b=[noisy 2]", which is what evaluating each `{}` as the
  // template is emitted produces, and which native's single printf can't
  // produce at all.
  { char* out = capture_print_output(&prog, print_inside_fn, heap, &host_imports, NULL);
    expect_str_eq("print-inside-print()", out, "[noisy 1][noisy 2]a=1 b=2\n");
    free(out);
  }
  { char* out = capture_print_output(&prog, print_snapshot_fn, heap, &host_imports, NULL);
    expect_str_eq("print-arg-snapshot()", out, "1 then 5\n");
    free(out);
  }
  { char* out = capture_print_output(&prog, print_str_snapshot_fn, heap, &host_imports, NULL);
    expect_str_eq("print-string-arg-snapshot()", out, "one / two\n");
    free(out);
  }
  // A trap part way through the arguments takes the WHOLE line with it:
  // stdout stays untouched, because nothing had been written yet.
  { BcResult result = {0};
    char*    out    = capture_print_output(&prog, print_trapping_fn, heap, &host_imports, &result);
    expect_str_eq("print-trapping-arg() output", out, "");
    expect_true("print-trapping-arg() trapped", result.trapped);
    free(out);
  }

  arena_temp_end(&fn_temp);

  if (g_failures == 0) printf("bcgen_print_test: all checks passed\n");
  else                 printf("bcgen_print_test: %d check(s) FAILED\n", g_failures);

  ctx_free();
  return g_failures == 0 ? 0 : 1;
}
