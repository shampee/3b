// format_test.c -- validates format.c, the one component in this tree that
// REWRITES A USER'S SOURCE FILE IN PLACE (`3b format -w`). Two properties
// matter more than any individual rendering decision, and both are checked
// here against the whole committed `.3b` corpus rather than against
// hand-picked snippets:
//
//   (1) IDEMPOTENCE -- format(format(x)) == format(x). A formatter that
//       drifts on a second pass turns "run the formatter" into a diff
//       generator, and the drift is usually invisible on the small inputs a
//       unit test would otherwise use.
//   (2) SEMANTIC PRESERVATION -- the C that codegen.c emits for a file is
//       byte-identical before and after formatting. This is the property
//       that makes `-w` safe at all; idempotence alone would still be
//       satisfied by a formatter that silently dropped a form.
//
// (2) runs here only on the self-contained fixtures below, since a corpus
// file that belongs to a multi-file package or imports another package
// cannot be type-checked standing alone. The corpus-scale version of the
// same property -- build every example package, format it, rebuild, diff
// output/ -- is `make format-corpus` (tools/format_corpus.sh), which drives
// the real `3b` binary over real package directories. The two are
// complementary: this file is fast and runs in `make check` unconditionally;
// that one is the end-to-end article.
//
// Same rig as test/layout_test.c and test/bcgen_test.c: hand-rolls parse ->
// lower -> check directly against an in-memory fixture rather than going
// through compile_package, which doesn't expose the live Checker its callers
// would need. Formatting itself needs none of that -- format.c is
// parser-only by design -- so the corpus walk stops at the parser.
#include "3b.h"
#include "file.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int g_failures = 0;
static int g_checks   = 0;

static void
fail(const char* what, const char* detail) {
  fprintf(stderr, "FAIL %s: %s\n", what, detail);
  g_failures += 1;
}

// Every routine below runs inside its own Context: the corpus is 76 files and
// each one is parsed at least twice, so sharing a single arena across the
// whole run would only measure how big an arena had to be. ctx_init/ctx_free
// is already a supported repeat-call cycle (see lib3b.h).
enum { FMT_TEST_ARENA = MB(32) };

////////////////////////////////
//~ Formatting a buffer in memory.

// Formats `src` and returns the result as a malloc'd NUL-terminated string, or
// NULL if it did not parse. `name` only ever shows up in diagnostics.
// The caller owns the returned buffer; the Context is torn down before
// returning, so nothing in it points back into arena memory.
static char*
format_source(String8 src, const char* name, u32 hang) {
  Context ctx;
  ctx_init(&ctx, FMT_TEST_ARENA);
  source_registry_reset();

  u32 file_id = source_file_register(str8_cstring((char*)name), src);

  Ast ast;
  ast_init(&ast, ctx_perm());
  Parser p;
  parser_init(&p, src, &ast, file_id);
  NodeIndex root = parse_program(&p);
  if (p.had_error) {
    ctx_free();
    return NULL;
  }

  char*  buf  = NULL;
  size_t size = 0;
  FILE*  out  = open_memstream(&buf, &size);
  if (!out) {
    fprintf(stderr, "FATAL: open_memstream failed\n");
    exit(1);
  }
  fmt_program(out, &ast, root, src, hang);
  fclose(out); // flushes and NUL-terminates buf

  ctx_free();
  return buf; // malloc'd by open_memstream
}

static char*
format_cstr(const char* src_cstr, const char* name, u32 hang) {
  return format_source(str8_cstring((char*)src_cstr), name, hang);
}

////////////////////////////////
//~ Property 1: idempotence, plus the weaker property it depends on --
// formatter output must itself still parse.

static void
check_idempotent(String8 src, const char* name) {
  g_checks += 1;
  char* once = format_source(src, name, 0);
  if (!once) return; // an input that doesn't parse is not this property's business

  char* twice = format_cstr(once, name, 0);
  if (!twice) {
    // The important half of this failure: the formatter emitted something it
    // cannot itself read back.
    fail(name, "formatter output does not re-parse");
    free(once);
    return;
  }

  if (strcmp(once, twice) != 0) {
    // Show the first differing line -- a whole-file dump of a 900-line corpus
    // member buries the one line that moved.
    u64 line = 1, i = 0;
    while (once[i] && twice[i] && once[i] == twice[i]) {
      if (once[i] == '\n') line += 1;
      i += 1;
    }
    char detail[256];
    snprintf(detail, sizeof(detail), "format twice != format once, first diverging at line %llu",
             (unsigned long long)line);
    fail(name, detail);
  }

  free(once);
  free(twice);
}

////////////////////////////////
//~ The corpus walk. Every `.3b` file in the tree, found by directory walk
// rather than a committed list, so a file added later is covered with no edit
// here. `output/` is skipped: it holds generated C, never `.3b` sources, and
// `.git` is skipped for the obvious reason.
//
// examples/game and examples/game3d are deliberately INCLUDED. They're the
// largest and least regular sources in the tree -- exactly the input a
// formatter is most likely to mishandle -- and nothing here needs them to
// build, only to parse. Their symlinked-in dependencies are a separate matter,
// handled at the lstat below.

static u32 g_corpus_files = 0;

static void
walk_corpus(const char* dir_path) {
  DIR* dir = opendir(dir_path);
  if (!dir) return;

  struct dirent* entry;
  while ((entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
    if (strcmp(entry->d_name, ".git") == 0 || strcmp(entry->d_name, "output") == 0) continue;

    char path[4096];
    snprintf(path, sizeof(path), "%s/%s", dir_path, entry->d_name);

    // lstat, not stat: examples/game3d/ symlinks in five package directories
    // (sdl, gl, glad, stbimg, stbtt) that live OUTSIDE this repo. Following
    // them would make the corpus depend on files nothing here commits -- and
    // did, silently: it inflated the walk from the tree's real 76 `.3b` files
    // to 84.
    struct stat st;
    if (lstat(path, &st) != 0) continue;
    if (S_ISLNK(st.st_mode)) continue;

    if (S_ISDIR(st.st_mode)) {
      walk_corpus(path);
      continue;
    }

    u64 len = strlen(entry->d_name);
    if (len < 4 || strcmp(entry->d_name + len - 3, ".3b") != 0) continue;

    // file_load_str8 needs an arena; borrow a throwaway Context for the read
    // so the bytes outlive it as a malloc'd copy the checks below own.
    Context ctx;
    ctx_init(&ctx, FMT_TEST_ARENA);
    String8 src = file_load_str8(ctx_perm(), str8_cstring(path));
    char*   own = NULL;
    u64     n   = 0;
    if (src.str) {
      n   = src.size;
      own = (char*)malloc(n + 1);
      memcpy(own, src.str, n);
      own[n] = 0;
    }
    ctx_free();

    if (!own) {
      fail(path, "could not be read");
      continue;
    }

    g_corpus_files += 1;
    check_idempotent(str8((u8*)own, n), path);
    free(own);
  }
  closedir(dir);
}

////////////////////////////////
//~ Property 2: semantic preservation, checked by running the real backend over
// both versions and comparing the C it emits, byte for byte. Reasoning from the
// formatter's source about which renderings cannot change meaning does not
// settle it; the emitted C does.

// Self-contained sources: single file, no imports, type-checks standing
// alone, and between them they exercise the forms whose rendering the
// formatter actually rearranges -- wrapped signatures, nested blocks,
// comments in every position, string literals containing characters the
// comment scanner must not mistake for syntax.
static const char* g_semantic_fixtures[] = {
  // Wrapped signatures and long argument lists -- the hang-indent paths.
  "(package fmt_sem_a)\n"
  "(struct Point [x i32 y i32 z i32])\n"
  "(fn sum-of-many [a i32 b i32 c i32 d i32 e i32 f i32 g i32 h i32] i32\n"
  "  (+ (+ (+ a b) (+ c d)) (+ (+ e f) (+ g h))))\n"
  "(fn make-point [x i32 y i32 z i32] Point (Point {:x x :y y :z z}))\n"
  "(fn main [] i32 (let [p Point (make-point 1 2 3)] (+ (. p x) (sum-of-many 1 2 3 4 5 6 7 8))))\n",

  // Comments in every position the side-channel has to place: above a form,
  // trailing a line, inside a body, and at end of file.
  "(package fmt_sem_b)\n"
  "; leading comment\n"
  "\n"
  "; second block after a blank line\n"
  "(fn twice [n i32] i32 ; trailing on the signature\n"
  "  ; inside the body\n"
  "  (* n 2))\n"
  "\n"
  "(fn main [] i32\n"
  "  (let [a i32 (twice 21)] ; trailing on a let\n"
  "    a))\n"
  "; comment at end of file\n",

  // String literals holding `;`, `(`, `)` and escapes -- the characters
  // fmt_scan_comments must not misread as comment or structure.
  "(package fmt_sem_c)\n"
  "(fn describe [] string \"semi; paren ( close ) quote \\\" done\")\n"
  "(fn main [] i32 (do (println \"{}\" (describe)) 0))\n",

  // Control flow, loops and mutation -- deeply nested bodies, the case where
  // the multi-line printers do the most rearranging.
  "(package fmt_sem_d)\n"
  "(fn classify [n i32] i32\n"
  "  (if (< n 0) (- 0 1) (if (= n 0) 0 1)))\n"
  "(fn total [n i32] i32\n"
  "  (var sum i32 0)\n"
  "  (for [i 0 n] (set sum (+ sum (classify (- i 2)))))\n"
  "  (while (> sum 100) (set sum (- sum 100)))\n"
  "  sum)\n"
  "(fn main [] i32 (total 10))\n",

  // Enums, flags, aliases and dense val/var runs -- the top-level forms whose
  // BLANK-LINE handling the formatter special-cases (fmt_is_no_gap_op).
  "(package fmt_sem_e)\n"
  "(enum Color [Red Green Blue])\n"
  "(alias Count i32)\n"
  "(val LIMIT i32 10)\n"
  "(val OTHER i32 20)\n"
  "(var running bool true)\n"
  "(fn pick [c Color] i32 (match c [Color/Red 1 Color/Green 2 else 3]))\n"
  "(fn main [] i32 (+ (pick Color/Green) (+ LIMIT OTHER)))\n",

  // A translated binding's shape: struct and enum mirrors carrying the C
  // spelling they mirror, and a bodyless `extern` that is the one place
  // codegen reads it. The extern is what makes this a SEMANTIC fixture rather
  // than an idempotence one -- a formatter that dropped the `"c-spelling"`
  // operand would still format stably, but the FFI casts in the generated C
  // would come out spelled in the package's own types instead.
  "(package fmt_sem_f)\n"
  "(struct Doc \"cgltf_data\" [n i32])\n"
  "(enum Status \"enum cgltf_result\" [Ok Err])\n"
  "(extern (fn c_load [d Doc*] Status))\n"
  "(fn load [d Doc*] Status (c_load d))\n"
  "(fn main [] i32 (let [d Doc (zero Doc)] (match (load (addr d)) [Status/Ok 0 else 1])))\n",
};

// Compiles `src` all the way to C and returns that C as a malloc'd string, or
// NULL if any stage failed. Mirrors what compile_package does internally, minus
// output/ writes and the toolchain.
static char*
generate_c(String8 src, const char* name) {
  Context ctx;
  ctx_init(&ctx, FMT_TEST_ARENA);
  source_registry_reset();

  u32 file_id = source_file_register(str8_cstring((char*)name), src);

  Ast ast;
  ast_init(&ast, ctx_perm());
  Parser p;
  parser_init(&p, src, &ast, file_id);
  NodeIndex root = parse_program(&p);
  if (p.had_error) { ctx_free(); return NULL; }

  // Strip the leading `(package ...)` form, as compile_package's
  // validate_and_strip_package_form does; there are no imports in any fixture
  // here, so dropping child 0 is the whole job.
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
  if (low.had_error) { ctx_free(); return NULL; }

  Checker ck = check_program(&tast, own_root, /*is_root_package=*/true, /*scope_query=*/NULL);
  if (ck.had_error) { ctx_free(); return NULL; }

  char*  buf  = NULL;
  size_t size = 0;
  FILE*  out  = open_memstream(&buf, &size);
  if (!out) { fprintf(stderr, "FATAL: open_memstream failed\n"); exit(1); }

  Codegen cg        = {0};
  cg.tast           = &tast;
  cg.resolved_types = ck.resolved_types;
  cg.structs        = ck.structs;
  cg.fns_by_name    = ck.fns_by_name;
  cg.out            = out;
  // A non-empty package name puts this on the same codegen path a real build
  // takes -- package-prefixed symbol names and all -- rather than the "demo
  // path" an empty name selects. Which name doesn't matter (the `(package ...)`
  // form was stripped above); that it isn't empty does, since prefixing is
  // exactly the kind of emission a formatting change could perturb.
  cg.package_name    = str8_lit("fmtsem");
  cg.program_root    = own_root;
  cg.has_parallel    = ck.has_parallel;
  cg.is_root_package = true;
  cg_program(&cg, own_root);
  fclose(out);

  b32 bad = cg.had_error;
  ctx_free();
  if (bad) { free(buf); return NULL; }
  return buf;
}

static void
check_semantics_preserved(const char* src_cstr, const char* name) {
  g_checks += 1;

  char* before = generate_c(str8_cstring((char*)src_cstr), name);
  if (!before) { fail(name, "fixture did not compile to C before formatting"); return; }

  char* formatted = format_cstr(src_cstr, name, 0);
  if (!formatted) { fail(name, "fixture did not format"); free(before); return; }

  char* after = generate_c(str8_cstring(formatted), name);
  if (!after) {
    fail(name, "FORMATTED fixture did not compile to C -- formatting changed the program");
    free(before);
    free(formatted);
    return;
  }

  if (strcmp(before, after) != 0) {
    u64 line = 1, i = 0;
    while (before[i] && after[i] && before[i] == after[i]) {
      if (before[i] == '\n') line += 1;
      i += 1;
    }
    char detail[256];
    snprintf(detail, sizeof(detail),
             "generated C differs after formatting, first diverging at emitted line %llu",
             (unsigned long long)line);
    fail(name, detail);
  }

  free(before);
  free(formatted);
  free(after);
}

////////////////////////////////
//~ Rendering goldens. Small, exact, and deliberately few: these pin the
// decisions a reader would want to see change in a diff -- comment placement,
// the blank line between top-level forms, the dense val/var run -- without
// turning every whitespace tweak into a wall of churn.

typedef struct RenderCase {
  const char* name;
  const char* input;
  const char* expect;
} RenderCase;

static const RenderCase g_render_cases[] = {
  {
    // Runs of spaces collapse. Note what else this pins: a `fn` with TWO OR
    // MORE parameters has its signature exploded one-per-line even when the
    // whole form would fit in FMT_WIDTH -- fmt_fn gates the inline header on
    // `pair_count <= 1` before it ever consults the width. A one-param fn
    // (next case) stays on one line. That asymmetry is current behavior, not
    // an endorsement of it; pinned here so changing it shows up as a diff.
    "normalizes-spacing",
    "(package p)\n(fn  add   [a i32   b i32]  i32   (+   a  b))\n",
    "(package p)\n"
    "\n"
    "(fn add\n"
    "  [a i32\n"
    "   b i32] i32\n"
    "  (+ a b))\n",
  },
  {
    "one-param-fn-keeps-inline-signature",
    "(package p)\n(fn twice [n i32] i32 (* n 2))\n",
    "(package p)\n"
    "\n"
    "(fn twice [n i32] i32\n"
    "  (* n 2))\n",
  },
  {
    // A blank line between two top-level forms is imposed, not copied: input
    // with none must come out with one.
    "inserts-blank-line-between-forms",
    "(package p)\n(struct A [x i32])\n(struct B [y i32])\n",
    "(package p)\n"
    "\n"
    "(struct A [x i32])\n"
    "\n"
    "(struct B [y i32])\n",
  },
  {
    // ...except between consecutive val/var/alias, which pack tight
    // (fmt_is_no_gap_op).
    "packs-consecutive-vals",
    "(package p)\n(val A i32 1)\n(val B i32 2)\n(var C i32 3)\n",
    "(package p)\n"
    "\n"
    "(val A i32 1)\n"
    "(val B i32 2)\n"
    "(var C i32 3)\n",
  },
  {
    "keeps-leading-and-trailing-comments",
    "(package p)\n; above\n(fn f [] i32 0)\n; at eof\n",
    "(package p)\n"
    "\n"
    "; above\n"
    "(fn f [] i32\n"
    "  0)\n"
    "; at eof\n",
  },
  {
    // A `;` inside a string literal is not a comment -- the case
    // fmt_scan_comments reproduces lexer.c's string handling for. If the scan
    // got this wrong, `; b\")` would be hoisted out as a comment line.
    "semicolon-in-string-is-not-a-comment",
    "(package p)\n(fn f [] string \"a ; b\")\n",
    "(package p)\n"
    "\n"
    "(fn f [] string\n"
    "  \"a ; b\")\n",
  },
};

static void
check_render(const RenderCase* c) {
  g_checks += 1;
  char* got = format_cstr(c->input, c->name, 0);
  if (!got) { fail(c->name, "did not parse"); return; }
  if (strcmp(got, c->expect) != 0) {
    fprintf(stderr, "FAIL %s: rendering mismatch\n--- want ---\n%s--- got ---\n%s------\n",
            c->name, c->expect, got);
    g_failures += 1;
  }
  free(got);
}

////////////////////////////////
//~ `--hang N`, the one formatter knob with a CLI flag behind it. Checked for
// the property that matters (a wider hang indents the wrapped continuation
// further) rather than pinned to an exact rendering, which would just restate
// the wrapping algorithm.

static const char* g_hang_source =
  "(package p)\n"
  "(fn a-function-with-a-genuinely-long-name [alpha i32 bravo i32 charlie i32 delta i32 echo i32 foxtrot i32 golf i32] i32 alpha)\n";

// Leading spaces on the first line that starts with whitespace -- the wrapped
// continuation. Returns 0 if the rendering did not wrap at all.
static u32
first_continuation_indent(const char* text) {
  const char* p = text;
  while (*p) {
    const char* line_start = p;
    while (*p && *p != '\n') p += 1;
    if (*line_start == ' ') {
      u32 n = 0;
      while (line_start[n] == ' ') n += 1;
      return n;
    }
    if (*p == '\n') p += 1;
  }
  return 0;
}

static void
check_hang(void) {
  g_checks += 1;
  char* narrow = format_cstr(g_hang_source, "hang2", 2);
  char* wide   = format_cstr(g_hang_source, "hang8", 8);
  if (!narrow || !wide) { fail("hang", "did not parse"); free(narrow); free(wide); return; }

  u32 n = first_continuation_indent(narrow);
  u32 w = first_continuation_indent(wide);
  if (n == 0 || w == 0) {
    fail("hang", "fixture did not wrap -- the knob has nothing to act on");
  } else if (!(w > n)) {
    char detail[128];
    snprintf(detail, sizeof(detail), "--hang 8 indent %u is not wider than --hang 2 indent %u", w, n);
    fail("hang", detail);
  }

  // Whatever the hang, the result must still be idempotent and still parse.
  char* wide_again = format_cstr(wide, "hang8", 8);
  if (!wide_again) fail("hang", "--hang 8 output does not re-parse");
  else if (strcmp(wide, wide_again) != 0) fail("hang", "--hang 8 is not idempotent");
  free(wide_again);

  free(narrow);
  free(wide);
}

////////////////////////////////

int
main(int argc, char** argv) {
  // Where the corpus lives. `make check` runs this from the repo root, so "."
  // is the working default; an explicit path is accepted for running it by
  // hand from elsewhere.
  const char* root = argc > 1 ? argv[1] : ".";

  for (u64 i = 0; i < sizeof(g_render_cases) / sizeof(g_render_cases[0]); i += 1) {
    check_render(&g_render_cases[i]);
  }
  check_hang();

  for (u64 i = 0; i < sizeof(g_semantic_fixtures) / sizeof(g_semantic_fixtures[0]); i += 1) {
    char name[64];
    snprintf(name, sizeof(name), "semantic-fixture-%llu", (unsigned long long)i);
    check_semantics_preserved(g_semantic_fixtures[i], name);
  }

  walk_corpus(root);
  // A corpus walk that silently found nothing would report success while
  // testing nothing at all -- the one failure mode this whole file has that
  // wouldn't announce itself.
  if (g_corpus_files < 20) {
    char detail[128];
    snprintf(detail, sizeof(detail), "found only %u .3b files under '%s' -- corpus walk is not finding the tree",
             g_corpus_files, root);
    fail("corpus", detail);
  }

  if (g_failures == 0) {
    printf("format_test: all checks passed (%d checks, %u corpus files)\n", g_checks, g_corpus_files);
  } else {
    printf("format_test: %d check(s) FAILED\n", g_failures);
  }
  return g_failures == 0 ? 0 : 1;
}
