// script_test.c -- validates script.c's `.3bs` driver (script_run_file):
// parse -> lower -> check -> bc_compile_program -> bc_run_in_program for a
// standalone script file, including `(import build)` -- a REAL cross-
// package bytecode import (see script.c's own top-of-file note): the
// embedded `build` module (translate/build.3bs) gets its OWN independent
// compile pass into its OWN BcProgram, and a qualified call
// (`build/double-if-positive`) crosses into it at runtime via
// BcOp_CallModule, not a textual splice. Same `/tmp`-file rig as
// test/bcgen_io_test.c's own cache-file tests (write, run, remove).
//
// Exercises:
//  - A self-contained script (no imports) with a zero-arg `main`.
//  - `(import build)` calling a REAL module-defined function through a
//    genuine BcOp_CallModule, not just parsing. Needs a REAL host_imports
//    table (bc_register_os_primitives) even for a script that never
//    itself CALLS an os-* primitive -- build.3bs's own wrapper functions
//    (getenv/exec-capture/file-exists?/dir-exists?) get compiled
//    unconditionally as part of build's OWN separate bc_compile_program
//    pass (which compiles every top-level function with a body, not just
//    ones the SCRIPT actually reaches), and THEIR bodies reference the
//    raw os-* externs -- NULL would hit bcgen.c's "call to a function
//    this compiler slice can't resolve" assert.
//  - Every documented failure mode returns FALSE with a diagnostic printed
//    (via diag_error or fprintf), never crashes/asserts: an unknown
//    import, a missing `main`, a `main` with the wrong arity, and an
//    ordinary type error inside the script body.
//  - A `.3bc` cache written by a genuinely SEPARATE PROCESS still loads and
//    runs correctly, for a Map-using script and for a cross-package
//    `(import build)` one -- see cold_load_in_subprocess below for why
//    those two specifically need a second process to test at all.
#include "3b.h"
#include "script.h"
#include "bcio.h" // bc_program_load/bc_content_hash -- the cached-reload block reads the `.3bc` back
#include "bcosprims.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h> // strcmp -- see main's own `--cold-load` subprocess mode
#if defined(_WIN32)
# include <direct.h> // _mkdir -- see the multi-file package block's own temp directory
#else
# include <sys/stat.h> // mkdir -- ditto
#endif

// The one env var the build/getenv round-trip below needs, set by this
// process so the test doesn't depend on anything already in the
// environment.
static void
set_env_var(const char* name, const char* value) {
#if defined(_WIN32)
  _putenv_s(name, value);
#else
  setenv(name, value, 1);
#endif
}

static int g_failures = 0;

static void
expect_true(const char* what, b32 got) {
  if (!got) {
    fprintf(stderr, "FAIL %s: got false, want true\n", what);
    g_failures += 1;
  }
}

static void
expect_false(const char* what, b32 got) {
  if (got) {
    fprintf(stderr, "FAIL %s: got true, want false\n", what);
    g_failures += 1;
  }
}

static void
expect_eq_i64(const char* what, i64 got, i64 want) {
  if (got != want) {
    fprintf(stderr, "FAIL %s: got %lld, want %lld\n", what, (long long)got, (long long)want);
    g_failures += 1;
  }
}

static void
write_script(const char* path, const char* content) {
  FILE* f = fopen(path, "w");
  xassert(f);
  fputs(content, f);
  fclose(f);
}

// Removes the `.3bc` bytecode cache file script_load/script_poll_reload
// would have written for `path_cstr` -- ONE fixed name per source path
// now (script.c's own cache_path_for: the extension replaced with
// `.3bc`, overwritten in place on every recompile rather than one file
// per content-hash), so unlike the scheme this replaced, no `content`
// argument is needed to compute it anymore. Keeps /tmp tidy across
// repeated test runs, same spirit as write_script's own paired
// remove(path) calls at each test block's end. Mirrors script.c's own
// cache_path_for logic exactly (strip the extension, stopping at the
// last `/`, append `.3bc`) rather than depending on it directly, so this
// test would still catch cache_path_for itself silently changing shape.
static String8
cache_path_of(const char* path_cstr) {
  String8 path = str8_cstring((char*)path_cstr);
  u64     dot  = path.size;
  for (u64 i = path.size; i-- > 0; ) {
    if (path.str[i] == '/') break;
    if (path.str[i] == '.') { dot = i; break; }
  }
  return str8f(ctx_scratch(), "%.*s.3bc", (int)dot, path_cstr);
}

static void
remove_script_cache(const char* path_cstr) {
  remove(cstr_from_str8_temp(cache_path_of(path_cstr)));
}

// Loads `path` written from `content_fmt` (one `%u`, so each rewrite is a
// genuinely different source and every poll is a REAL reload), then reloads it
// `reloads` times, returning how many bytes of the PERMANENT arena that cost.
//
// The cold load itself is deliberately outside the measurement: it is the one
// compile that legitimately allocates a module, and what the caller compares is
// the STEADY-STATE per-reload cost. ctx_init gives perm a VM-backed arena --
// one contiguous reservation -- so the distance between two marks is a real
// byte count.
static u64
reload_growth_bytes(const char* path, u32 reloads, BcHostImportTable* host_imports,
                     const char* content_fmt) {
  write_script(path, cstr_from_str8_temp(str8f(ctx_scratch(), (char*)content_fmt,0u)));

  ScriptTable  table = {0};
  ScriptHandle h;
  expect_true("leak check: cold load succeeds", script_load(&table, str8_cstring((char*)path), host_imports, &h));

  ArenaMark before = arena_mark(ctx_perm());
  for (u32 i = 1; i <= reloads; i += 1) {
    write_script(path, cstr_from_str8_temp(str8f(ctx_scratch(), (char*)content_fmt,i)));
    expect_true("leak check: each edit reloads", script_poll_reload(&table, h, host_imports));
  }
  ArenaMark after = arena_mark(ctx_perm());

  script_unload(&table, h);
  return (u64)(after.at - before.at);
}

// Re-runs THIS SAME test binary in a genuinely SEPARATE PROCESS (see main's
// own `--cold-load` mode below) purely to cold-load `script_path`, which
// compiles it and writes its `.3bc` cache. The warm load then happens back
// here, in a process that never compiled that script at all.
//
// WHY A SECOND PROCESS RATHER THAN JUST TWO ScriptTables. A cache file holds
// COMPILE-TIME POINTERS in its const pool -- string literals, and a Map/Set
// call site's BcHashSlotLayout descriptor (see bcio.c's own top-of-file
// note). Every one of those has to be re-materialized at load time, because
// an address from the run that WROTE the file means nothing to the run that
// reads it. Within ONE process that reconstruction is untestable: the
// compiling load's own arena allocations are still live at the very same
// addresses, so even a completely unserialized pointer keeps "working" and a
// same-process test passes whether the load side reconstructs anything or
// not. A second process is what makes the difference observable -- and it's
// the REAL shipping scenario anyway (a build step, or yesterday's run of the
// game, writes the `.3bc`; today's run loads it).
//
// `self` is argv[0]; the Makefile invokes this test as `./test/script_test`
// from the repo root, so it re-execs fine. Paths here are all fixed
// space-free /tmp literals, so plain `system` needs no quoting care.
static b32
cold_load_in_subprocess(const char* self, const char* script_path) {
  String8 cmd = str8f(ctx_scratch(), "%s --cold-load %s", self, script_path);
  return system(cstr_from_str8_temp(cmd)) == 0;
}

int
main(int argc, char** argv) {
  Context ctx;
  ctx_init(&ctx, MB(16));

  // ~~ Subprocess mode: cold-load one script (compiling it, and writing its
  // `.3bc` cache as a side effect) and exit. Not a test itself -- it's the
  // OTHER PROCESS half of cold_load_in_subprocess above, so the cache files
  // the two tests at the end of main read back were genuinely written by a
  // different process.
  if (argc == 3 && strcmp(argv[1], "--cold-load") == 0) {
    BcHostImportTable host_imports = {0};
    bc_register_os_primitives(&host_imports, ctx_perm()); // harmless for a script with no host
                                                              // calls; required for `(import build)`
    ScriptTable  table = {0};
    ScriptHandle h;
    b32          ok = script_load(&table, str8_cstring(argv[2]), &host_imports, &h);
    ctx_free();
    return ok ? 0 : 1;
  }

  // ~~ Self-contained script, no imports.
  { const char* path = "/tmp/3b_script_test_plain.3bs";
    write_script(path,
      "(package plain)\n"
      "(fn double [x i32] i32 (* x 2))\n"
      "(fn main [] i32 (double 21))\n");
    BcResult r;
    b32      ok = script_run_file(path, NULL, &r);
    expect_true("plain script runs", ok);
    if (ok) {
      expect_true("plain script main returns a value", r.has_value);
      expect_eq_i64("plain script main() == 42", r.value, 42);
    }
    remove(path);
  }

  // ~~ `(import build)` -- compiles translate/build.3bs as its OWN
  // separate BcProgram, and calls a REAL function it defines through a
  // genuine BcOp_CallModule (a real cross-package bytecode import, not a
  // textual splice -- see script.c's own top-of-file note). Qualified
  // call syntax (`build/double-if-positive`), matching native 3b's own
  // package-import syntax exactly.
  { const char* path = "/tmp/3b_script_test_import_build.3bs";
    write_script(path,
      "(package usesbuild)\n"
      "(import build)\n"
      "(fn main [] i32 (build/double-if-positive 21))\n");
    BcHostImportTable host_imports = {0};
    bc_register_os_primitives(&host_imports, ctx_perm());
    BcResult r;
    b32      ok = script_run_file(path, &host_imports, &r);
    expect_true("(import build) script runs", ok);
    if (ok) expect_eq_i64("(import build) main() == 42", r.value, 42);
    remove(path);
  }

  { const char* path = "/tmp/3b_script_test_import_build_negative.3bs";
    write_script(path,
      "(package usesbuild2)\n"
      "(import build)\n"
      "(fn main [] i32 (build/double-if-positive -5))\n");
    BcHostImportTable host_imports = {0};
    bc_register_os_primitives(&host_imports, ctx_perm());
    BcResult r;
    b32      ok = script_run_file(path, &host_imports, &r);
    expect_true("(import build) script runs (negative branch)", ok);
    if (ok) expect_eq_i64("double-if-positive(-5) == -5 (unchanged)", r.value, -5);
    remove(path);
  }

  // ~~ A STRING round-trip through the module boundary -- a genuinely
  // more complex case than the plain-i32 ones above, since a `string`
  // value is EMBEDDED (a register holds the address of a boxed
  // {ptr,size} header, not the bytes themselves -- see bcgen.c's own
  // top-of-file convention), proving BcOp_CallModule's nested
  // bc_run_in_program call correctly passes/returns an ADDRESS across the
  // module boundary, not just a plain scalar.
  { const char* path = "/tmp/3b_script_test_import_build_string.3bs";
    write_script(path,
      "(package usesbuild3)\n"
      "(import build)\n"
      "(fn main [] i32\n"
      "  (if (build/dir-exists? \"/tmp\") 1 0))\n");
    BcHostImportTable host_imports = {0};
    bc_register_os_primitives(&host_imports, ctx_perm());
    BcResult r;
    b32      ok = script_run_file(path, &host_imports, &r);
    expect_true("(import build) string-arg/bool-result call runs", ok);
    if (ok) expect_eq_i64("build/dir-exists?(\"/tmp\") == true", r.value, 1);
    remove(path);
  }

  // ~~ An ARENA across the module boundary, plus a string coming BACK.
  // build/getenv takes the arena to allocate its result in and threads it
  // down to the os/getenv host import -- so this covers a `arena`-typed
  // register surviving BcOp_CallModule, a caller-scoped `(scratch ...)`
  // arena reaching a host import two frames down, and the returned bytes
  // still being readable at the call site.
  { set_env_var("SCRIPT_TEST_GETENV_VAR", "round-trip");
    const char* path = "/tmp/3b_script_test_import_build_getenv.3bs";
    write_script(path,
      "(package usesbuild4)\n"
      "(import build)\n"
      "(fn main [] i32\n"
      "  (scratch [temp]\n"
      "    (let [v (build/getenv temp \"SCRIPT_TEST_GETENV_VAR\")]\n"
      "      (when (= v \"round-trip\") (return 1))\n"
      "      (when (> (string-len v) 0u64) (return 2))))\n"
      "  0)\n");
    BcHostImportTable host_imports = {0};
    bc_register_os_primitives(&host_imports, ctx_perm());
    BcResult r;
    b32      ok = script_run_file(path, &host_imports, &r);
    expect_true("(import build) arena-arg/string-result call runs", ok);
    if (ok) expect_eq_i64("build/getenv returns the variable's exact value", r.value, 1);
    remove(path);
  }

  // ~~ The SAME module imported from TWO SEPARATE script_run_file calls
  // (each its own top-level `3b run`-equivalent invocation, not a single
  // script importing it twice) -- proves compile_module's own per-call
  // registry doesn't leak/corrupt state ACROSS separate runs (each
  // script_run_file call builds a fresh registry -- see script.c's own
  // comment on why a persistent static one would be wrong).
  { const char* path_a = "/tmp/3b_script_test_import_build_a.3bs";
    const char* path_b = "/tmp/3b_script_test_import_build_b.3bs";
    write_script(path_a, "(package usesbuild_a)\n(import build)\n(fn main [] i32 (build/double-if-positive 5))\n");
    write_script(path_b, "(package usesbuild_b)\n(import build)\n(fn main [] i32 (build/double-if-positive 100))\n");
    BcHostImportTable host_imports = {0};
    bc_register_os_primitives(&host_imports, ctx_perm());
    BcResult ra, rb;
    b32      ok_a = script_run_file(path_a, &host_imports, &ra);
    b32      ok_b = script_run_file(path_b, &host_imports, &rb);
    expect_true("first independent (import build) run succeeds", ok_a);
    expect_true("second independent (import build) run succeeds", ok_b);
    if (ok_a) expect_eq_i64("first run: double-if-positive(5) == 10", ra.value, 10);
    if (ok_b) expect_eq_i64("second run: double-if-positive(100) == 200", rb.value, 200);
    remove(path_a);
    remove(path_b);
  }

  // ~~ Unknown import -- reported, not crashed.
  { const char* path = "/tmp/3b_script_test_bad_import.3bs";
    write_script(path,
      "(package badimport)\n"
      "(import nonexistent)\n"
      "(fn main [] i32 0)\n");
    BcResult r;
    expect_false("unknown import fails cleanly", script_run_file(path, NULL, &r));
    remove(path);
  }

  // ~~ Missing `main` -- reported, not crashed (bc_program_find_fn would
  // otherwise assert).
  { const char* path = "/tmp/3b_script_test_no_main.3bs";
    write_script(path,
      "(package nomain)\n"
      "(fn helper [] i32 1)\n");
    BcResult r;
    expect_false("missing main fails cleanly", script_run_file(path, NULL, &r));
    remove(path);
  }

  // ~~ The OTHER entry-point shape a native 3b program can have,
  // `[argc i32 argv string*]` -- script_run_file hands it a one-entry argv
  // holding the program's own path, so `argc` is 1 and reading `argv[0]`
  // gives back a real, non-empty string rather than a dangling pointer.
  { const char* path = "/tmp/3b_script_test_argv_main.3bs";
    write_script(path,
      "(package argvmain)\n"
      "(fn main [argc i32 argv string*] i32\n"
      "  (if (!= argc 1) -1\n"
      "    (if (= (nth argv 0) \"/tmp/3b_script_test_argv_main.3bs\") 42 -2)))\n");
    BcResult r;
    b32      ok = script_run_file(path, NULL, &r);
    expect_true("argc/argv main runs", ok);
    if (ok) expect_eq_i64("argc == 1 and argv[0] is the script's own path", r.value, 42);
    remove(path);
  }

  // ~~ A MULTI-FILE `.3b` PACKAGE run by naming one of its files. In 3b a
  // directory is a package, so `helper.3b` here is not an `(import ...)` of
  // anything -- it is more of the same program, and naming `main.3b` has to
  // compile it too (script.c's script_compile_unit), the way `3b <dir>` on
  // the native path always has.
  //
  // The same directory deliberately also holds two files that must NOT be
  // pulled in: `other.3b`, a same-directory `.3b` declaring a DIFFERENT
  // package (a scratch directory of unrelated one-file programs stays
  // runnable), and `sidecar.3bs`, a script (never a package member). Both
  // define a colliding `helper-value`, so including either would fail the
  // compile outright rather than pass quietly.
  { const char* dir       = "/tmp/3b_script_test_pkg";
    const char* main_path = "/tmp/3b_script_test_pkg/main.3b";
    const char* help_path = "/tmp/3b_script_test_pkg/helper.3b";
    const char* other_path = "/tmp/3b_script_test_pkg/other.3b";
    const char* sidecar_path = "/tmp/3b_script_test_pkg/sidecar.3bs";
#if defined(_WIN32)
    _mkdir(dir);
#else
    mkdir(dir, 0755);
#endif
    write_script(main_path,
      "(package multi)\n"
      "(fn main [] i32 (+ (helper-value) (if (= (thing-of) Thing/Second) 1 0)))\n");
    write_script(help_path,
      "(package multi)\n"
      "(enum Thing [First Second])\n"
      "(fn thing-of [] Thing Thing/Second)\n"
      "(fn helper-value [] i32 41)\n");
    write_script(other_path,
      "(package unrelated)\n"
      "(fn helper-value [] i32 999)\n");
    write_script(sidecar_path,
      "(package sidecar)\n"
      "(fn helper-value [] i32 999)\n");
    BcResult r;
    b32      ok = script_run_file(main_path, NULL, &r);
    expect_true("a multi-file package runs from one of its files", ok);
    if (ok) expect_eq_i64("sibling file's fn and enum are both visible", r.value, 42);

    // ~~ ...and naming the SIBLING instead is the same compile unit, so the
    // `main` living in the other file is still found and run.
    BcResult r2;
    b32      ok2 = script_run_file(help_path, NULL, &r2);
    expect_true("naming the sibling runs the same package", ok2);
    if (ok2) expect_eq_i64("sibling entry point returns the same value", r2.value, 42);

    // ~~ The unrelated same-directory `.3b` still runs on its own, seeing
    // only itself: were the whole directory swept in regardless of package,
    // `multi`'s own `helper-value` and `main` would collide with it here.
    write_script(other_path,
      "(package unrelated)\n"
      "(fn helper-value [] i32 999)\n"
      "(fn main [] i32 (helper-value))\n");
    BcResult r3;
    b32      ok3 = script_run_file(other_path, NULL, &r3);
    expect_true("an unrelated same-directory .3b still runs alone", ok3);
    if (ok3) expect_eq_i64("unrelated file sees only its own package", r3.value, 999);

    remove(main_path);
    remove(help_path);
    remove(other_path);
    remove(sidecar_path);
    remove(dir);
  }

  // ~~ An ordinary type error inside the script body -- reported by the
  // checker (diag_error), not a crash.
  { const char* path = "/tmp/3b_script_test_type_error.3bs";
    write_script(path,
      "(package typeerr)\n"
      "(fn main [] i32 (+ 1 \"oops\"))\n");
    BcResult r;
    expect_false("a real type error fails cleanly", script_run_file(path, NULL, &r));
    remove(path);
  }

  // ~~ A nonexistent file path -- reported, not crashed.
  { BcResult r;
    expect_false("a nonexistent script path fails cleanly",
                 script_run_file("/tmp/3b_script_test_does_not_exist.3bs", NULL, &r));
  }

  // ~~ script_load/script_call: load a script (no `main` needed), call a
  // named function with arguments, get a result back.
  { const char* path = "/tmp/3b_script_test_load_call.3bs";
    write_script(path,
      "(package fireball)\n"
      "(fn on_cast [power i32] i32 (* power 3))\n");
    ScriptTable  table = {0};
    ScriptHandle h;
    expect_true("script_load succeeds", script_load(&table, str8_cstring((char*)path), NULL, &h));
    i64      args[1] = { 7 };
    BcResult r;
    expect_true("script_call resolves on_cast by name",
                script_call(&table, h, str8_lit("on_cast"), args, 1, ctx_perm(), NULL, &r));
    expect_eq_i64("on_cast(7) == 21", r.value, 21);

    // Wrong function name / wrong arity both fail cleanly, not assert.
    expect_false("script_call: unknown function name fails cleanly",
                 script_call(&table, h, str8_lit("no_such_fn"), args, 1, ctx_perm(), NULL, &r));
    expect_false("script_call: wrong arg count fails cleanly",
                 script_call(&table, h, str8_lit("on_cast"), args, 0, ctx_perm(), NULL, &r));

    // A stale handle (index 0, or past unload) fails cleanly too.
    ScriptHandle null_handle = {0};
    expect_false("script_call on the null handle fails cleanly",
                 script_call(&table, null_handle, str8_lit("on_cast"), args, 1, ctx_perm(), NULL, &r));

    remove_script_cache(path);
    remove(path);
  }

  // ~~ script_poll_reload: edit the file on disk, poll, see new behavior --
  // all through the SAME handle, no re-fetch.
  { const char* path = "/tmp/3b_script_test_reload.3bs";
    write_script(path,
      "(package reloadme)\n"
      "(fn on_tick [x i32] i32 (+ x 1))\n");
    ScriptTable  table = {0};
    ScriptHandle h;
    expect_true("script_load (reload test) succeeds", script_load(&table, str8_cstring((char*)path), NULL, &h));

    i64      args[1] = { 10 };
    BcResult r;
    expect_true("on_tick(10) call ok before edit", script_call(&table, h, str8_lit("on_tick"), args, 1, ctx_perm(), NULL, &r));
    expect_eq_i64("on_tick(10) == 11 before edit", r.value, 11);

    expect_false("poll with no file change reports no reload",
                 script_poll_reload(&table, h, NULL));

    write_script(path,
      "(package reloadme)\n"
      "(fn on_tick [x i32] i32 (+ x 100))\n");
    expect_true("poll after a real edit reports a reload", script_poll_reload(&table, h, NULL));

    expect_true("on_tick(10) call ok after edit (SAME handle)",
                script_call(&table, h, str8_lit("on_tick"), args, 1, ctx_perm(), NULL, &r));
    expect_eq_i64("on_tick(10) == 110 after edit -- new code, same handle", r.value, 110);

    // A broken edit: old code keeps running instead of the session dying.
    write_script(path,
      "(package reloadme)\n"
      "(fn on_tick [x i32] i32 (+ x \"oops\"))\n");
    expect_false("poll on a broken edit reports no reload (keeps old code)",
                 script_poll_reload(&table, h, NULL));
    expect_true("on_tick still callable after a failed reload",
                script_call(&table, h, str8_lit("on_tick"), args, 1, ctx_perm(), NULL, &r));
    expect_eq_i64("on_tick(10) == 110 still -- last GOOD version kept running", r.value, 110);

    script_unload(&table, h);
    expect_false("script_call after unload fails cleanly",
                 script_call(&table, h, str8_lit("on_tick"), args, 1, ctx_perm(), NULL, &r));

    remove_script_cache(path);
    remove(path);
  }

  // ~~ A reload whose OUTGOING program is CACHE-BACKED, i.e. the mmap being
  // replaced is live at the moment the fresh cache is written. script.c
  // writes to a temp path and moves it over the cache for exactly this
  // case; the move also has to actually HAPPEN, or the file left on disk
  // keeps a content hash that can never match again and the script silently
  // recompiles from source on every load from then on.
  //
  // The two loads are what sets it up: the first compiles and writes the
  // cache, the second (unchanged source) mmaps that file -- so table_b's
  // slot holds a live mapping of it when the edit below lands.
  { const char* path = "/tmp/3b_script_test_reload_cached.3bs";
    write_script(path,
      "(package reloadcached)\n"
      "(fn on_tick [x i32] i32 (+ x 1))\n");
    remove_script_cache(path); // in case a prior failed run left one behind

    ScriptTable  table_a = {0};
    ScriptHandle ha;
    expect_true("cached-reload: cold load succeeds", script_load(&table_a, str8_cstring((char*)path), NULL, &ha));

    ScriptTable  table_b = {0};
    ScriptHandle hb;
    expect_true("cached-reload: warm (mmap-backed) load succeeds",
                script_load(&table_b, str8_cstring((char*)path), NULL, &hb));

    const char* edited =
      "(package reloadcached)\n"
      "(fn on_tick [x i32] i32 (+ x 100))\n";
    write_script(path, edited);
    expect_true("cached-reload: poll after edit reloads over a live mapping",
                script_poll_reload(&table_b, hb, NULL));

    i64      args[1] = { 10 };
    BcResult r;
    expect_true("cached-reload: new code callable after the reload",
                script_call(&table_b, hb, str8_lit("on_tick"), args, 1, ctx_perm(), NULL, &r));
    expect_eq_i64("cached-reload: on_tick(10) == 110 -- reloaded code, not the mapped-out old one",
                  r.value, 110);

    // The cache on disk must now describe the EDITED source. Checked by
    // content hash rather than by timing a later load: a stale cache is not
    // a load FAILURE, it is a silent permanent fallback to recompiling, and
    // a hash comparison is the only thing that tells the two apart.
    BcLoadResult reread = bc_program_load(cache_path_of(path), NULL, NULL, ctx_perm(), ctx_perm());
    expect_true("cached-reload: the .3bc still loads after being replaced", reread.ok);
    expect_true("cached-reload: the .3bc on disk was refreshed to the EDITED source",
                reread.ok && reread.content_hash == bc_content_hash(str8_cstring((char*)edited)));
    bc_program_unload(&reread);

    script_unload(&table_a, ha);
    script_unload(&table_b, hb);
    remove_script_cache(path);
    remove(path);
  }

  // ~~ Hot-reloading a script that IMPORTS a module must not recompile that
  // module every time. Modules are compiled into the binary and can't change
  // mid-run, so script.c memoizes them process-wide; a registry local to
  // each compile instead meant every save during an editing session re-parsed,
  // re-checked and re-emitted `build` onto the permanent arena, which is never
  // reclaimed -- unbounded growth over a long session.
  //
  // Measured against the SAME number of reloads of an import-free script, so
  // what's being asserted is "importing costs about what not importing costs,
  // per reload", not a byte count that any unrelated allocation change would
  // invalidate. A recompile of `build` per reload overshoots this by far more
  // than the slack here allows.
  { const u32 reloads = 8;
    BcHostImportTable host_imports = {0};
    bc_register_os_primitives(&host_imports, ctx_perm()); // `build`'s own wrappers need these

    const char* plain_path = "/tmp/3b_script_test_leak_plain.3bs";
    const char* imp_path   = "/tmp/3b_script_test_leak_import.3bs";
    remove_script_cache(plain_path);
    remove_script_cache(imp_path);

    u64 plain_growth = reload_growth_bytes(plain_path, reloads, &host_imports,
                                            "(package leakplain)\n"
                                            "(fn compute [x i32] i32 (+ x %u))\n");
    u64 import_growth = reload_growth_bytes(imp_path, reloads, &host_imports,
                                             "(package leakimport)\n"
                                             "(import build)\n"
                                             "(fn compute [x i32] i32 (+ (build/double-if-positive x) %u))\n");

    expect_true("reloading an importing script costs about what reloading a plain one does",
                import_growth < plain_growth * 3);
    if (import_growth >= plain_growth * 3) {
      fprintf(stderr, "  (%u reloads: %llu bytes importing vs %llu plain -- `build` looks like it is "
                       "being recompiled per reload)\n",
                       reloads, (unsigned long long)import_growth, (unsigned long long)plain_growth);
    }

    remove_script_cache(plain_path);
    remove_script_cache(imp_path);
    remove(plain_path);
    remove(imp_path);
  }

  // ~~ Precompiled bytecode cache: script_load writes a `.3bc` cache file
  // next to the source on a cold load, and a SECOND, independent
  // ScriptTable loading the SAME unchanged source picks up that cache
  // (bc_program_load's mmap path) rather than recompiling -- the actual
  // "shippable precompiled bytecode" story: a build step running
  // script_load once ahead of time is enough for every later load of that
  // exact content to skip parse/check/compile entirely.
  { const char* path    = "/tmp/3b_script_test_cache.3bs";
    const char* content = "(package cached)\n(fn compute [x i32] i32 (+ x 5))\n";
    write_script(path, content);

    String8 cache_path = str8_lit("/tmp/3b_script_test_cache.3bc"); // fixed name -- cache_path_for's own comment
    remove(cstr_from_str8_temp(cache_path)); // in case a prior failed run left one behind

    ScriptTable  table_a = {0};
    ScriptHandle ha;
    expect_true("cold load (no cache yet) compiles and succeeds",
                script_load(&table_a, str8_cstring((char*)path), NULL, &ha));

    FILE* cache_check = fopen(cstr_from_str8_temp(cache_path), "rb");
    expect_true("script_load wrote a .3bc cache file", cache_check != NULL);
    if (cache_check) fclose(cache_check);

    // A second, independent table loading the SAME unchanged content should
    // hit that cache (mmap-loaded, not recompiled) and behave identically.
    ScriptTable  table_b = {0};
    ScriptHandle hb;
    expect_true("warm load (cache present) succeeds",
                script_load(&table_b, str8_cstring((char*)path), NULL, &hb));
    i64      args[1] = { 37 };
    BcResult r;
    expect_true("warm-loaded script is callable",
                script_call(&table_b, hb, str8_lit("compute"), args, 1, ctx_perm(), NULL, &r));
    expect_eq_i64("compute(37) == 42 via the cached load", r.value, 42);

    remove(cstr_from_str8_temp(cache_path));
    remove(path);
  }

  // ~~ A Map-using script through a cache written by ANOTHER PROCESS. A
  // Map/Set call site bakes a BcHashSlotLayout descriptor into the const
  // pool as a raw compile-time pointer (bcgen.c's bc_add_layout_const), so
  // this is the test that the SAVED FIELDS of that descriptor -- not the
  // pointer -- are what a load reconstructs from. Same-process it would
  // pass either way; see cold_load_in_subprocess for why.
  { const char* path = "/tmp/3b_script_test_cache_map.3bs";
    write_script(path,
      "(package cachedmap)\n"
      "(fn tally [n i32] i32\n"
      "  (val a arena (create))\n"
      "  (var m {i32 i32})\n"
      "  (for [i 0 n] (map-set a m i (* i 10)))\n"
      "  (var sum i32 0)\n"
      "  (for [i 0 n]\n"
      "    (let [p (map-get m i)]\n"
      "      (when p (set sum (+ sum (deref p))))))\n"
      "  sum)\n");
    remove_script_cache(path); // in case a prior failed run left one behind

    expect_true("map script: cold load in a separate process succeeds",
                cold_load_in_subprocess(argv[0], path));

    BcHostImportTable host_imports = {0};
    bc_register_os_primitives(&host_imports, ctx_perm());
    ScriptTable  table = {0};
    ScriptHandle h;
    expect_true("map script: warm load of another process's cache succeeds",
                script_load(&table, str8_cstring((char*)path), &host_imports, &h));
    i64      args[1] = { 4 };
    BcResult r;
    expect_true("map script: cached tally is callable",
                script_call(&table, h, str8_lit("tally"), args, 1, ctx_perm(), &host_imports, &r));
    expect_eq_i64("tally(4) == 60 (0+10+20+30) through a cross-process cached load", r.value, 60);

    script_unload(&table, h);
    remove_script_cache(path);
    remove(path);
  }

  // ~~ A cross-package `(import build)` script through a cache written by
  // ANOTHER PROCESS. A cached load skips compilation entirely, so nothing
  // recompiles `build` on the way in -- the BcModuleTable every
  // BcOp_CallModule dispatches through has to be rebuilt from what the file
  // itself recorded (see bcio.c), or a qualified call has nothing to resolve
  // against. Unlike the Map case this one is broken in-process too; it still
  // goes through a subprocess so both cache regressions read the same way.
  { const char* path = "/tmp/3b_script_test_cache_import.3bs";
    write_script(path,
      "(package cachedimport)\n"
      "(import build)\n"
      "(fn compute [x i32] i32 (build/double-if-positive x))\n");
    remove_script_cache(path);

    expect_true("import script: cold load in a separate process succeeds",
                cold_load_in_subprocess(argv[0], path));

    BcHostImportTable host_imports = {0};
    bc_register_os_primitives(&host_imports, ctx_perm());
    ScriptTable  table = {0};
    ScriptHandle h;
    expect_true("import script: warm load of another process's cache succeeds",
                script_load(&table, str8_cstring((char*)path), &host_imports, &h));
    i64      args[1] = { 21 };
    BcResult r;
    expect_true("import script: cached compute is callable",
                script_call(&table, h, str8_lit("compute"), args, 1, ctx_perm(), &host_imports, &r));
    expect_eq_i64("compute(21) == 42 -- a BcOp_CallModule through a cross-process cached load",
                  r.value, 42);

    script_unload(&table, h);
    remove_script_cache(path);
    remove(path);
  }

  if (g_failures == 0) printf("script_test: all checks passed\n");
  else                 printf("script_test: %d check(s) FAILED\n", g_failures);

  ctx_free();
  return g_failures == 0 ? 0 : 1;
}
