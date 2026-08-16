// bcosprims_test.c -- validates the generic OS-facing host imports
// (bcosprims.c/h): os/getenv, os/exec-capture, os/file-exists,
// os/dir-exists, and the whole-file/listing/timestamp family
// (os/read-file, os/write-file, os/list-dir, os/file-mtime). Same rig as
// the other bcgen_*_test.c files, PLUS a real end-to-end check through
// script.c's `(import build)` splicing (script_run_file), since these
// primitives only matter in practice via the `build` module.
//
// What this file does NOT check is that these agree with the NATIVE `os`
// module (native_pkgs/os/os.3b) they mirror -- that needs both backends
// running the same source, which is `make os-parity` over
// examples/os-portable. This suite is the unit-level half: that each
// primitive does the right thing at all, and that a failure points at one
// of them rather than at "the two backends disagree somewhere".
//
// String results are decoded manually (base+0 = str ptr, base+8 = size,
// String8's own layout -- same pattern test/bcgen_io_test.c's own
// `greeting()` check already uses): every check here either compares
// scalars (bool/i64) or decodes a returned string's raw bytes in C via
// memcmp, rather than asking `=` inside a fixture body.
//
// That started as a workaround -- bcgen.c compiled `=` on two strings to a
// raw ADDRESS comparison, so a fixture couldn't have checked its own
// result -- and outlived it: BcOp_StrCmp does real content comparison now.
// Deciding in C is still the better shape for a test of the PRIMITIVES,
// since a wrong answer then can't be the comparison's fault.
//
// Exercises:
//  - os/getenv: a real env var this test sets itself (setenv/_putenv_s),
//    read back correctly; an UNSET var returns empty string, not garbage.
//    Its leading `arena` parameter is a real one supplied from C here,
//    and the returned bytes are checked to actually live in it.
//  - os/exec-capture: `echo` (works under both POSIX sh and cmd.exe),
//    output captured correctly with the trailing newline trimmed.
//  - os/write-file + os/read-file: a whole-file round trip, plus the
//    empty-string result a missing path gives.
//  - os/file-mtime: a real file's mtime is a plausible recent timestamp;
//    a missing path is exactly -1, not 0.
//  - os/list-dir: returns a real `[string]` Vector -- decoded in C
//    through the same hidden DynHdr layout bytecode-compiled code reads
//    (BcOp_DynCount), which is the actual contract that makes a
//    Vector-returning host import work at all.
//  - os/file-exists/os/dir-exists: a real temp file/dir this test creates
//    itself report true; a path that doesn't exist reports false; a
//    FILE checked with os/dir-exists (and vice versa) correctly reports
//    false (kind-specific, not just "something is there").
//  - A real end-to-end run through `script_run_file` (splicing `(import
//    build)`, not a hand-rolled fixture) using ONLY constructs bcgen.c
//    already supports (`if`, file-exists?/dir-exists?, no `=` on
//    strings) -- proves the FULL pipeline (primitive registration wired
//    into run_script_cmd's own shape, splicing, extern resolution) works,
//    not just bcgen.c-level host calls in isolation.
#include "3b.h"
#include "bcgen.h"
#include "bcvm.h"
#include "bcosprims.h"
#include "script.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#if defined(_WIN32)
# include <direct.h>
#else
# include <sys/stat.h>
# include <unistd.h>
#endif

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

// Decodes a boxed String8 result (base+0 = str ptr, base+8 = size) and
// compares its raw bytes against `want` -- same pattern
// test/bcgen_io_test.c's own `greeting()` check already uses.
static void
expect_string_result(const char* what, i64 boxed, const char* want) {
  u8* base = (u8*)(intptr_t)boxed;
  u8* str;  memcpy(&str,  base + 0, sizeof(str));
  u64 size; memcpy(&size, base + 8, sizeof(size));
  u64 want_len = strlen(want);
  if (size != want_len || (want_len > 0 && memcmp(str, want, want_len) != 0)) {
    fprintf(stderr, "FAIL %s: got \"%.*s\" (size %llu), want \"%s\" (size %llu)\n",
            what, (int)size, (char*)str, (unsigned long long)size, want, (unsigned long long)want_len);
    g_failures += 1;
  }
}

static void
set_env_var(const char* name, const char* value) {
#if defined(_WIN32)
  _putenv_s(name, value);
#else
  setenv(name, value, 1);
#endif
}

// Every wrapper that returns freshly-allocated memory takes the arena to
// allocate it in, threaded straight through -- the convention shared with
// native_pkgs/os/os.3b, and what lets this test hand in an arena it
// controls and then read the result afterwards.
static const char* g_fixture_source =
  "(package bcosprims_test)\n"
  "(extern (fn os/getenv [arena arena name string] string))\n"
  "(extern (fn os/exec-capture [arena arena cmd string] string))\n"
  "(extern (fn os/file-exists [path string] bool))\n"
  "(extern (fn os/dir-exists [path string] bool))\n"
  "(extern (fn os/read-file [arena arena path string] string))\n"
  "(extern (fn os/write-file [path string contents string] bool))\n"
  "(extern (fn os/file-mtime [path string] i64))\n"
  "(extern (fn os/list-dir [arena arena dir string] [string]))\n"
  "\n"
  "(fn get-env-var [a arena name string] string (os/getenv a name))\n"
  "(fn run-cmd [a arena cmd string] string (os/exec-capture a cmd))\n"
  "(fn check-file [path string] bool (os/file-exists path))\n"
  "(fn check-dir [path string] bool (os/dir-exists path))\n"
  "(fn slurp [a arena path string] string (os/read-file a path))\n"
  "(fn spit [path string contents string] bool (os/write-file path contents))\n"
  "(fn mtime-of [path string] i64 (os/file-mtime path))\n"
  "(fn entries-in [a arena dir string] [string] (os/list-dir a dir))\n"
  "\n"
  "(fn main [] i32 0)\n";

static Checker
check_fixture(TypedAst* tast, TypedIndex* out_root) {
  Ast ast;
  ast_init(&ast, ctx_perm());

  String8 src     = str8_cstring((char*)(g_fixture_source));
  u32     file_id = source_file_register(str8_lit("bcosprims_test_fixture.3b"), src);

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

  set_env_var("BCOSPRIMS_TEST_VAR", "hello123");

  TypedAst   tast;
  TypedIndex root;
  Checker    ck = check_fixture(&tast, &root);
  xassert(tast.nodes[root].kind == TypedNodeKind_Block);

  ArenaTemp   fn_temp = arena_temp_begin(ctx_scratch());
  LayoutCache layout_cache;
  layout_cache_init(&layout_cache, ctx_perm());

  BcHostImportTable host_imports = {0};
  bc_register_os_primitives(&host_imports, ctx_perm());

  BcProgram prog = bc_compile_program(&ck, &tast, root, fn_temp.arena, ctx_perm(), &layout_cache, &host_imports, NULL);
  u32 get_env_fn   = bc_program_find_fn(&prog, str8_lit("get-env-var"));
  u32 run_cmd_fn    = bc_program_find_fn(&prog, str8_lit("run-cmd"));
  u32 check_file_fn = bc_program_find_fn(&prog, str8_lit("check-file"));
  u32 check_dir_fn  = bc_program_find_fn(&prog, str8_lit("check-dir"));
  u32 slurp_fn      = bc_program_find_fn(&prog, str8_lit("slurp"));
  u32 spit_fn       = bc_program_find_fn(&prog, str8_lit("spit"));
  u32 mtime_fn      = bc_program_find_fn(&prog, str8_lit("mtime-of"));
  u32 entries_fn    = bc_program_find_fn(&prog, str8_lit("entries-in"));

  Arena* heap = ctx_perm();
  // A VM `arena` register holds a real base.h Arena*, so the arena these
  // wrappers take is passed exactly like any other argument.
  i64 heap_arg = (i64)(intptr_t)heap;

  // ~~ os/getenv: a real, known env var, AND an unset one.
  { i64 args[2] = { heap_arg, box_string_arg(heap, "BCOSPRIMS_TEST_VAR") };
    BcResult r = bc_run_in_program(&prog, get_env_fn, args, 2, heap, &host_imports);
    expect_string_result("get-env-var(BCOSPRIMS_TEST_VAR)", r.value, "hello123"); }
  { i64 args[2] = { heap_arg, box_string_arg(heap, "BCOSPRIMS_TEST_VAR_DOES_NOT_EXIST") };
    BcResult r = bc_run_in_program(&prog, get_env_fn, args, 2, heap, &host_imports);
    expect_string_result("get-env-var(unset) is empty", r.value, ""); }

  // The arena argument is genuinely honored, not accepted and ignored:
  // pushing into a SEPARATE arena and checking the result landed inside
  // its bounds is the only way to tell those two apart from out here.
  { Arena    side  = arena_create_vm(MB(1));
    u8*      low   = push_array(&side, u8, 1); // everything getenv pushes lands above this
    i64      args[2] = { (i64)(intptr_t)&side, box_string_arg(heap, "BCOSPRIMS_TEST_VAR") };
    BcResult r     = bc_run_in_program(&prog, get_env_fn, args, 2, heap, &host_imports);
    u8*      high  = push_array(&side, u8, 1); // ...and below this
    u8*      boxed = (u8*)(intptr_t)r.value;
    expect_true("getenv allocated into the arena it was given, not the VM heap",
                boxed > low && boxed < high);
    expect_string_result("getenv into a caller arena still returns the right bytes", r.value, "hello123");
    arena_release(&side); }

  // ~~ os/exec-capture: `echo` works under both POSIX sh and cmd.exe.
  { i64 args[2] = { heap_arg, box_string_arg(heap, "echo hello-from-exec-capture") };
    BcResult r = bc_run_in_program(&prog, run_cmd_fn, args, 2, heap, &host_imports);
    expect_string_result("run-cmd(echo ...) -- trailing newline trimmed", r.value, "hello-from-exec-capture"); }

  // ~~ os/file-exists/os/dir-exists: real temp file/dir this test creates.
  { const char* dir_path  = "/tmp/3b_bcosprims_test_dir";
    const char* file_path = "/tmp/3b_bcosprims_test_dir/file.txt";
    const char* missing_path = "/tmp/3b_bcosprims_test_dir/does-not-exist.txt";
#if defined(_WIN32)
    _mkdir(dir_path);
#else
    mkdir(dir_path, 0755);
#endif
    FILE* f = fopen(file_path, "w");
    xassert(f);
    fputs("hi", f);
    fclose(f);

    { i64 args[1] = { box_string_arg(heap, file_path) };
      expect_eq_i64("check-file(real file)", bc_run_in_program(&prog, check_file_fn, args, 1, heap, &host_imports).value, 1); }
    { i64 args[1] = { box_string_arg(heap, missing_path) };
      expect_eq_i64("check-file(missing)", bc_run_in_program(&prog, check_file_fn, args, 1, heap, &host_imports).value, 0); }
    { i64 args[1] = { box_string_arg(heap, dir_path) };
      expect_eq_i64("check-dir(real dir)", bc_run_in_program(&prog, check_dir_fn, args, 1, heap, &host_imports).value, 1); }
    { i64 args[1] = { box_string_arg(heap, file_path) };
      expect_eq_i64("check-dir(a FILE, not a dir) is false", bc_run_in_program(&prog, check_dir_fn, args, 1, heap, &host_imports).value, 0); }
    { i64 args[1] = { box_string_arg(heap, dir_path) };
      expect_eq_i64("check-file(a DIR, not a file) is false", bc_run_in_program(&prog, check_file_fn, args, 1, heap, &host_imports).value, 0); }

    remove(file_path);
#if defined(_WIN32)
    _rmdir(dir_path);
#else
    rmdir(dir_path);
#endif
  }

  // ~~ os/write-file + os/read-file + os/file-mtime + os/list-dir, over a
  // directory this test owns outright, so the listing count is exact.
  { const char* dir_path = "/tmp/3b_bcosprims_test_files";
    const char* a_path   = "/tmp/3b_bcosprims_test_files/a.txt";
    const char* missing  = "/tmp/3b_bcosprims_test_files/nope.txt";
    const char* body     = "one\ntwo\n";
#if defined(_WIN32)
    _mkdir(dir_path);
#else
    mkdir(dir_path, 0755);
#endif

    { i64 args[2] = { box_string_arg(heap, a_path), box_string_arg(heap, body) };
      expect_eq_i64("spit(a.txt)", bc_run_in_program(&prog, spit_fn, args, 2, heap, &host_imports).value, 1); }
    { i64 args[2] = { heap_arg, box_string_arg(heap, a_path) };
      BcResult r = bc_run_in_program(&prog, slurp_fn, args, 2, heap, &host_imports);
      expect_string_result("slurp(a.txt) round-trips write-file's bytes", r.value, body); }
    // The same "empty means absence" a missing env var gets, not a crash
    // and not a partially-filled buffer.
    { i64 args[2] = { heap_arg, box_string_arg(heap, missing) };
      BcResult r = bc_run_in_program(&prog, slurp_fn, args, 2, heap, &host_imports);
      expect_string_result("slurp(missing) is empty", r.value, ""); }

    { i64 args[1] = { box_string_arg(heap, a_path) };
      i64 mtime = bc_run_in_program(&prog, mtime_fn, args, 1, heap, &host_imports).value;
      expect_true("mtime of a just-written file is recent", mtime > 1577836800 && mtime <= (i64)time(NULL)); }
    // -1 exactly, never 0 -- 0 is a real timestamp, and a caller polling
    // for changes must not read a vanished file as merely an old one.
    { i64 args[1] = { box_string_arg(heap, missing) };
      expect_eq_i64("mtime of a missing path is -1",
                    bc_run_in_program(&prog, mtime_fn, args, 1, heap, &host_imports).value, -1); }

    // os/list-dir hands back a real `[string]`: a T* with a hidden
    // {count,capacity} DynHdr immediately before it, `string` elements
    // laid out inline as 16-byte {ptr,size}. Decoded here exactly the way
    // BcOp_DynCount and a compiled `(nth entries i)` would, since that
    // representation IS the contract -- a host import returning a Vector
    // works only for as long as the two agree on it.
    { i64 args[2] = { heap_arg, box_string_arg(heap, dir_path) };
      BcResult r       = bc_run_in_program(&prog, entries_fn, args, 2, heap, &host_imports);
      String8* entries = (String8*)(intptr_t)r.value;
      expect_eq_i64("list-dir found exactly the one file in the directory", (i64)dyn_count(entries), 1);
      if (dyn_count(entries) == 1) {
        expect_true("list-dir returns the BARE filename, not the full path",
                    entries[0].size == 5 && memcmp(entries[0].str, "a.txt", 5) == 0);
      } }
    // A missing directory is an EMPTY Vector -- and an empty Vector is the
    // NULL pointer a freshly declared `(var v [string])` already holds, so
    // this must come back as literal 0 rather than a header with count 0.
    { i64 args[2] = { heap_arg, box_string_arg(heap, "/tmp/3b_bcosprims_no_such_dir") };
      BcResult r = bc_run_in_program(&prog, entries_fn, args, 2, heap, &host_imports);
      expect_eq_i64("list-dir of a missing directory is an empty (NULL) Vector", r.value, 0); }

    remove(a_path);
#if defined(_WIN32)
    _rmdir(dir_path);
#else
    rmdir(dir_path);
#endif
  }

  arena_temp_end(&fn_temp);

  // ~~ End-to-end through script_run_file itself -- a REAL cross-package
  // bytecode import (import build) + BcOp_CallModule, main.c-equivalent
  // primitive registration, and calling build/file-exists?/build/dir-
  // exists? (build's own wrapper names, QUALIFIED -- matching native 3b's
  // own package-import syntax) from a REAL `.3bs` script file, using only
  // `if` (no `=` on strings, which bcgen.c doesn't support content-wise
  // yet).
  {
    const char* script_path = "/tmp/3b_bcosprims_test_script.3bs";
    FILE* f = fopen(script_path, "w");
    xassert(f);
    fputs(
      "(package bcosprims_script_test)\n"
      "(import build)\n"
      "(fn main [] i32\n"
      "  (if (build/dir-exists? \"/tmp\")\n"
      "    (if (build/file-exists? \"/tmp/3b_bcosprims_test_script.3bs\") 1 0)\n"
      "    0))\n", f);
    fclose(f);

    BcHostImportTable script_host_imports = {0};
    bc_register_os_primitives(&script_host_imports, ctx_perm());
    BcResult result;
    b32 ok = script_run_file(script_path, &script_host_imports, &result);
    expect_true("end-to-end script_run_file succeeds", ok);
    if (ok) expect_eq_i64("end-to-end script correctly reports both paths exist", result.value, 1);

    remove(script_path);
  }

  // ~~ End-to-end through script_run_file for the STREAM verbs -- these
  // can't be driven the way getenv/file-exists are above (a direct
  // bc_host_import call against a hand-checked fixture), because what
  // makes them work is precisely the parts that only exist on the real
  // `(import os)` path: multi-parameter extern splicing (os/seek takes
  // three), CONSTANT splicing (os/mode-write is a spliced ConstDecl, not
  // a host import at all), and `print`/`println`'s optional leading
  // `stream` argument, which is a bcgen.c/bcvm.c opcode-operand feature
  // rather than a host import.
  //
  // The script returns 1 only if every stage agreed; the C side then
  // ALSO verifies the file's exact bytes on disk, which is the only way
  // to prove `(println f ...)` wrote to the FILE rather than to stdout
  // (a redirected print that silently went to stdout would leave the
  // script's own read-back checks passing on a shorter file otherwise).
  {
    const char* script_path = "/tmp/3b_bcosprims_test_stream.3bs";
    const char* data_path   = "/tmp/3b_bcosprims_test_stream.txt";
    remove(data_path);

    FILE* f = fopen(script_path, "w");
    xassert(f);
    fputs(
      "(package bcosprims_stream_test)\n"
      "(import os)\n"
      "(val path string \"/tmp/3b_bcosprims_test_stream.txt\")\n"
      "\n"
      "(fn write-it [] bool\n"
      "  (let [f (os/open path os/mode-write)]\n"
      "    (when (not f) (return false))\n"
      "    (println f \"hdr {} {}\" 7 \"x\")\n"
      "    (os/write-string f \"raw\")\n"
      "    (var bytes [u8 3])\n"
      "    (for [i 0 3] (set (nth bytes i) (cast u8 (+ 65 i))))\n"
      "    (os/write f (cast any (addr bytes)) 3u64)\n"
      "    (os/write-string f \"\\n\")\n"
      "    (os/close f)))\n"
      "\n"
      "(fn count-lines [] i32\n"
      "  (let [g (os/open path os/mode-read)]\n"
      "    (when (not g) (return -1))\n"
      "    (var n i32 0)\n"
      "    (scratch [temp]\n"
      "      (while (not (os/at-end? g))\n"
      "        (let [line (os/read-line temp g)]\n"
      "          (when (> (string-len line) 0u64) (++ n)))))\n"
      "    (os/close g)\n"
      "    n))\n"
      "\n"
      "(fn seek-it [] bool\n"
      "  (let [h (os/open path os/mode-read)]\n"
      "    (when (not h) (return false))\n"
      "    (when (not (os/seek h 4i64 os/seek-start)) (os/close h) (return false))\n"
      "    (let [at (os/tell h)]\n"
      "      (os/close h)\n"
      "      (= at 4i64))))\n"
      "\n"
      "(fn main [] i32\n"
      "  (when (not (write-it)) (return 0))\n"
      "  (when (not (= (count-lines) 2)) (return 0))\n"
      "  (when (not (seek-it)) (return 0))\n"
      "  1)\n", f);
    fclose(f);

    BcHostImportTable script_host_imports = {0};
    bc_register_os_primitives(&script_host_imports, ctx_perm());
    BcResult result;
    b32 ok = script_run_file(script_path, &script_host_imports, &result);
    expect_true("end-to-end stream script runs", ok);
    if (ok) expect_eq_i64("stream script's own write/read-line/seek checks all agree", result.value, 1);

    // "hdr 7 x\n" (a REDIRECTED println -- template, placeholders and
    // all) + "raw" + the 3 raw bytes "ABC" + "\n".
    const char* want = "hdr 7 x\nrawABC\n";
    char        got[64] = {0};
    FILE*       rf      = fopen(data_path, "rb");
    expect_true("stream script actually created its output file", rf != NULL);
    if (rf) {
      size_t n = fread(got, 1, sizeof(got) - 1, rf);
      fclose(rf);
      expect_eq_i64("stream output file length", (i64)n, (i64)strlen(want));
      expect_true("stream output file bytes (println really went to the FILE)",
                  n == strlen(want) && memcmp(got, want, n) == 0);
      if (n != strlen(want) || memcmp(got, want, n) != 0) {
        fprintf(stderr, "  got \"%.*s\", want \"%s\"\n", (int)n, got, want);
      }
    }

    remove(script_path);
    remove(data_path);
  }

  if (g_failures == 0) printf("bcosprims_test: all checks passed\n");
  else                 printf("bcosprims_test: %d check(s) FAILED\n", g_failures);

  ctx_free();
  return g_failures == 0 ? 0 : 1;
}
