// codegen_golden_test.c -- golden-output tests for codegen.c: small `.3b`
// inputs in test/golden/, each with the C it is expected to produce checked in
// beside it as `.c.expected`.
//
// The bytecode backend's own ~35 tests assert on VALUES the interpreter
// computes, a different question from what C text codegen.c emits. A golden
// makes the second question reviewable: a codegen change shows up as a diff a
// human reads and approves, rather than as silence.
//
// Fixtures are SMALL and single-concern. A golden big enough that every
// unrelated change churns it stops being read, so each file here covers one
// thing -- name mangling, struct emission, an `if` as a value, enum
// auto-numbering -- and nothing else.
//
// To regenerate after an INTENDED codegen change:
//
//     UPDATE_GOLDENS=1 ./test/codegen_golden_test
//
// then read `git diff test/golden/` before committing. That diff IS the review;
// regenerating without reading it defeats the entire point of the file.
#include "3b.h"
#include "file.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;
static int g_checks   = 0;
static int g_updated  = 0;

enum { GOLDEN_ARENA = MB(32) };

// Compiles one source and returns BOTH emitted files -- the package header
// (cg_program_header) and the package source (cg_program) -- as malloc'd
// strings, or false if any stage failed. Same hand-rolled parse -> lower ->
// check -> emit sequence as test/layout_test.c and test/format_test.c, for the
// same reason: compile_package doesn't expose the live Checker that Codegen
// needs.
//
// `package_name` is set, matching what compiler.c does for a real build, NOT
// left empty. Empty selects codegen's "demo path", which emits a different and
// slightly degenerate preamble (`#include "3b_runtime.h"` twice, since the
// missing package header falls back to the runtime include and the
// unconditional runtime include follows it). Goldens are only worth reading if
// they show what `3b build` actually produces.
//
// The header is half of codegen.c's output -- typedefs, struct forward
// declarations, prototypes -- and nothing else in the tree covers it at all.
//
// Diagnostics print to stderr here rather than being captured -- a fixture that
// stops compiling is a failure whose message the reader wants to see.
static b32
generate_c(String8 src, const char* name, const char* package_name, char** out_h, char** out_c) {
  Context ctx;
  ctx_init(&ctx, GOLDEN_ARENA);
  source_registry_reset();

  u32 file_id = source_file_register(str8_cstring((char*)name), src);

  Ast ast;
  ast_init(&ast, ctx_perm());
  Parser p;
  parser_init(&p, src, &ast, file_id);
  NodeIndex root = parse_program(&p);
  if (p.had_error) { ctx_free(); return false; }

  u16        form_count;
  NodeIndex* forms      = ast_seq_children(&ast, root, &form_count);
  Token      synth_open = {0};
  synth_open.line       = 1;
  synth_open.col        = 1;
  NodeIndex body = ast_push_seq(&ast, AstNodeKind_List, synth_open, forms + 1, (u16)(form_count - 1));

  TypedAst tast;
  typed_ast_init(&tast, ctx_perm());
  Lowerer low = {0};
  low.ast     = &ast;
  low.tast    = &tast;
  TypedIndex own_root = lower_program(&low, body);
  if (low.had_error) { ctx_free(); return false; }

  Checker ck = check_program(&tast, own_root, /*is_root_package=*/true, /*scope_query=*/NULL);
  if (ck.had_error) { ctx_free(); return false; }

  Codegen cg         = {0};
  cg.tast            = &tast;
  cg.resolved_types  = ck.resolved_types;
  cg.structs         = ck.structs;
  cg.fns_by_name     = ck.fns_by_name;
  cg.package_name    = str8_cstring((char*)package_name);
  cg.program_root    = own_root;
  cg.has_parallel    = ck.has_parallel;
  cg.is_root_package = true;

  // Header first, then source -- the order compiler.c emits them in, and the
  // order matters: cg_program_header populates memoized state on `cg`
  // (public_toplevel_names) that cg_program then reads.
  char*  h_buf  = NULL;
  size_t h_size = 0;
  FILE*  h_out  = open_memstream(&h_buf, &h_size);
  if (!h_out) { fprintf(stderr, "FATAL: open_memstream failed\n"); exit(1); }
  cg.out = h_out;
  cg_program_header(&cg, own_root);
  fclose(h_out);

  char*  c_buf  = NULL;
  size_t c_size = 0;
  FILE*  c_out  = open_memstream(&c_buf, &c_size);
  if (!c_out) { fprintf(stderr, "FATAL: open_memstream failed\n"); exit(1); }
  cg.out = c_out;
  cg_program(&cg, own_root);
  fclose(c_out);

  b32 bad = cg.had_error;
  ctx_free();
  if (bad) { free(h_buf); free(c_buf); return false; }
  *out_h = h_buf;
  *out_c = c_buf;
  return true;
}

// Reads a whole file into a malloc'd NUL-terminated buffer, or NULL.
static char*
read_file(const char* path) {
  FILE* f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n < 0) { fclose(f); return NULL; }
  char* buf = (char*)malloc((size_t)n + 1);
  size_t got = fread(buf, 1, (size_t)n, f);
  buf[got]   = 0;
  fclose(f);
  return buf;
}

// Prints the first differing line of got vs want, with a little context. A
// whole-file dump would bury the one line that changed.
static void
report_diff(const char* name, const char* want, const char* got) {
  u64 line = 1, i = 0;
  while (want[i] && got[i] && want[i] == got[i]) {
    if (want[i] == '\n') line += 1;
    i += 1;
  }

  // Back up to the start of the diverging line in each, then print it.
  u64 ws = i, gs = i;
  while (ws > 0 && want[ws - 1] != '\n') ws -= 1;
  while (gs > 0 && got[gs - 1] != '\n') gs -= 1;
  u64 we = ws, ge = gs;
  while (want[we] && want[we] != '\n') we += 1;
  while (got[ge] && got[ge] != '\n') ge += 1;

  fprintf(stderr, "FAIL %s: generated C differs from the golden at line %llu\n",
          name, (unsigned long long)line);
  fprintf(stderr, "    want: %.*s\n", (int)(we - ws), want + ws);
  fprintf(stderr, "    got:  %.*s\n", (int)(ge - gs), got + gs);
  fprintf(stderr, "    (if this change is intended: UPDATE_GOLDENS=1 ./test/codegen_golden_test,"
                  " then read `git diff test/golden/`)\n");
}

// Compares one emitted file against its golden, or rewrites the golden under
// UPDATE_GOLDENS. `label` distinguishes the header from the source in failure
// output.
static void
compare_or_update(const char* label, const char* golden_path, const char* got, b32 update) {
  g_checks += 1;

  if (update) {
    FILE* f = fopen(golden_path, "wb");
    if (!f) {
      fprintf(stderr, "FAIL %s: could not write %s\n", label, golden_path);
      g_failures += 1;
      return;
    }
    fwrite(got, 1, strlen(got), f);
    fclose(f);
    g_updated += 1;
    return;
  }

  char* want = read_file(golden_path);
  if (!want) {
    fprintf(stderr, "FAIL %s: no golden at %s -- run UPDATE_GOLDENS=1 ./test/codegen_golden_test\n",
            label, golden_path);
    g_failures += 1;
    return;
  }

  if (strcmp(want, got) != 0) {
    report_diff(label, want, got);
    g_failures += 1;
  }
  free(want);
}

static void
check_golden(const char* dir, const char* stem, b32 update) {
  char src_path[4096], h_path[4096], c_path[4096];
  snprintf(src_path, sizeof(src_path), "%s/%s.3b", dir, stem);
  snprintf(h_path, sizeof(h_path), "%s/%s.h.expected", dir, stem);
  snprintf(c_path, sizeof(c_path), "%s/%s.c.expected", dir, stem);

  char* src = read_file(src_path);
  if (!src) {
    fprintf(stderr, "FAIL %s: could not read %s\n", stem, src_path);
    g_failures += 1;
    return;
  }

  // Every fixture declares `(package golden)`, so that is the package name the
  // emitted `#include "golden.h"` and any name prefixing must agree with.
  char* got_h = NULL;
  char* got_c = NULL;
  b32   ok    = generate_c(str8((u8*)src, strlen(src)), src_path, "golden", &got_h, &got_c);
  free(src);
  if (!ok) {
    fprintf(stderr, "FAIL %s: fixture did not compile to C (diagnostics above)\n", stem);
    g_failures += 1;
    return;
  }

  char h_label[160], c_label[160];
  snprintf(h_label, sizeof(h_label), "%s (header)", stem);
  snprintf(c_label, sizeof(c_label), "%s (source)", stem);
  compare_or_update(h_label, h_path, got_h, update);
  compare_or_update(c_label, c_path, got_c, update);

  free(got_h);
  free(got_c);
}

int
main(int argc, char** argv) {
  // `make check` runs this from the repo root; an explicit directory is
  // accepted for running it by hand from elsewhere.
  const char* dir = argc > 1 ? argv[1] : "test/golden";

  const char* update_env = getenv("UPDATE_GOLDENS");
  b32         update     = (update_env && update_env[0] == '1');

  DIR* d = opendir(dir);
  if (!d) {
    fprintf(stderr, "FAIL: cannot open golden directory '%s'\n", dir);
    return 1;
  }

  // Collect stems first, then sort, so failures come out in a stable order
  // rather than whatever readdir happens to return.
  char   stems[64][128];
  u32    stem_count = 0;
  struct dirent* e;
  while ((e = readdir(d)) != NULL && stem_count < 64) {
    u64 len = strlen(e->d_name);
    if (len < 4 || strcmp(e->d_name + len - 3, ".3b") != 0) continue;
    u64 stem_len = len - 3;
    if (stem_len >= sizeof(stems[0])) continue;
    memcpy(stems[stem_count], e->d_name, stem_len);
    stems[stem_count][stem_len] = 0;
    stem_count += 1;
  }
  closedir(d);

  for (u32 i = 0; i + 1 < stem_count; i += 1) {
    for (u32 j = i + 1; j < stem_count; j += 1) {
      if (strcmp(stems[j], stems[i]) < 0) {
        char tmp[128];
        memcpy(tmp, stems[i], sizeof(tmp));
        memcpy(stems[i], stems[j], sizeof(tmp));
        memcpy(stems[j], tmp, sizeof(tmp));
      }
    }
  }

  for (u32 i = 0; i < stem_count; i += 1) check_golden(dir, stems[i], update);

  // A golden suite that found no fixtures would pass having compiled nothing.
  if (stem_count < 4) {
    fprintf(stderr, "FAIL: only %u fixture(s) found in '%s'\n", stem_count, dir);
    g_failures += 1;
  }

  if (update) {
    printf("codegen_golden_test: wrote %d golden(s) -- now read `git diff test/golden/`\n", g_updated);
    return g_failures == 0 ? 0 : 1;
  }

  if (g_failures == 0) printf("codegen_golden_test: all checks passed (%u fixtures, %d files)\n", stem_count, g_checks);
  else                 printf("codegen_golden_test: %d check(s) FAILED\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
