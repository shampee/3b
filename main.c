// main.c -- the command-line entrypoint: argv parsing and dispatch, plus the
// thin per-subcommand wrappers that own a Context around a call into the real
// logic elsewhere. The package/import compiler lives in compiler.c (see
// compiler.h for its design overview); `3b build`/`3b run`/`3b clean`'s
// toolchain invocation lives in build.c (see build.h).

#include "3b.h"
#include "build.h"
#include "compiler.h"
#include "file.h"
#include "script.h"
#include "bcosprims.h"
// `3b translate`, the C header -> 3b binding generator, links against libclang
// -- a separate dependency this build can be configured without (see the
// Makefile's WITHOUT_TRANSLATE), e.g. on a platform with no matching libclang
// available yet.
#ifndef BBB_NO_TRANSLATE
# include "translate/translate.h"
#endif
#include <dirent.h>
#include <limits.h>
#include <sys/stat.h>

static int
compile_package_dir(const char* dir_path_cstr) {
  Context ctx;
  ctx_init(&ctx, MB(16));
  PackageKind   kind    = build_config_read_kind(ctx_perm(), dir_path_cstr);
  PackageBuild* root_pkg = compile_all_packages(dir_path_cstr, NULL, kind, /*verbose=*/true, NULL, 0,
                                                 /*tolerate_check_errors=*/false, /*scope_query=*/NULL);
  ctx_free();
  return root_pkg ? 0 : 1;
}

// `3b build <dir>` / `3b run <dir>` -- compiles like plain `3b <dir>` above,
// then hands off to build.c to invoke the C toolchain directly, reading
// `<dir>/build.cfg.3b` if present, instead of requiring a hand-written
// Makefile. `run_after` also executes the resulting binary; `release` selects
// optimized CFLAGS over the debug-assertions default (see build.c);
// `no_compile`, meaningful for `build` but never `run`, stops after emitting
// the generated .c/.h files, equivalent to plain `3b <dir>`.
static int
build_project_cmd(const char* dir_path_cstr, b32 run_after, b32 release, b32 no_compile) {
  Context ctx;
  ctx_init(&ctx, MB(16));

  PackageKind kind = build_config_read_kind(ctx_perm(), dir_path_cstr);
  if (run_after && kind == PackageKind_Library) {
    fprintf(stderr, "3b run: '%s' is a library (`(kind library)` in build.cfg.3b) -- nothing to run\n", dir_path_cstr);
    ctx_free();
    return 1;
  }

  PackageBuild** registry = NULL;
  PackageBuild*  root_pkg = compile_all_packages(dir_path_cstr, &registry, kind, /*verbose=*/true, NULL, 0,
                                                  /*tolerate_check_errors=*/false, /*scope_query=*/NULL);
  if (!root_pkg) {
    ctx_free();
    return 1;
  }

  if (no_compile) {
    ctx_free();
    return 0;
  }

  u64      pkg_count = dyn_count(registry);
  String8* pkg_names = push_array(ctx_perm(), String8, pkg_count);
  String8* pkg_dirs  = push_array(ctx_perm(), String8, pkg_count);
  foreach_index(i, pkg_count) {
    pkg_names[i] = registry[i]->pkg_name;
    pkg_dirs[i]  = registry[i]->dir_path;
  }

  int result = build_invoke_toolchain(ctx_perm(), dir_path_cstr, root_pkg->pkg_name, pkg_names, pkg_dirs,
                                       pkg_count, run_after, release);
  ctx_free();
  return result;
}

// `3b run <script.3bs>`, or bare `3b <script.3bs>` -- interprets a script
// through the bytecode VM instead of compiling a native package directory (see
// script.c). A `.3b` package works too (see path_is_regular_file):
// `3b run examples/arrays/main.3b` runs that same source through the VM, which
// exercises bcgen.c/bcvm.c against the example suite without a
// `.3bs`-suffixed copy of anything. Naming ONE file of a multi-file package is
// enough -- script.c's script_compile_unit picks up its same-package siblings
// in the same directory, matching what `3b <dir>` compiles.
//
// What's still off the table is a real cross-PACKAGE `(import ...)` of
// anything but `build`/`config`/`os`/`rng`: that hits script.c's "imports
// unknown module" error, since the bytecode driver's import mechanism knows
// only the modules built into `3b` itself.
//
// Registering the OS-facing host imports (bcosprims.c) is what lets a script
// that imports `build` actually call one of its os-* wrappers; without them
// bcgen.c asserts on a call it can't resolve. Compiling and running aren't
// separable here the way `3b <dir>` and `3b run <dir>` are -- there is no
// C-toolchain step to skip.
static int
run_script_cmd(const char* path_cstr) {
  Context ctx;
  ctx_init(&ctx, MB(16));
  BcHostImportTable host_imports = {0};
  bc_register_os_primitives(&host_imports, ctx_perm());
  BcResult result;
  b32 ok = script_run_file(path_cstr, &host_imports, &result);
  int status = ok ? 0 : 1;
  if (ok && result.has_value) printf("%lld\n", (long long)result.value);
  ctx_free();
  return status;
}

// True iff `path` names an existing regular file, as opposed to a directory or
// nothing at all -- routes a bare `.3b`/`.3bs` file argument to run_script_cmd,
// the bytecode-VM driver, instead of the native package-directory pipeline.
// Stat-based rather than an extension check: script.c's script_run_file just
// opens the path it is given and has never cared about the suffix, so a plain
// single-file `.3b` package is equally valid input to the same pipeline.
static b32
path_is_regular_file(const char* path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

// True iff `path` names anything at all -- file, directory, or otherwise. Used
// only to tell a mistyped subcommand apart from a real path argument, so the
// kind of thing found doesn't matter, just that something is there.
static b32
path_exists(const char* path) {
  struct stat st;
  return stat(path, &st) == 0;
}

// `3b clean <dir>` -- see build_clean in build.c. Compiles nothing itself,
// since cleaning has to work on a project that is currently broken, so it gets
// its own minimal Context instead of going through compile_all_packages.
static int
clean_project_cmd(const char* dir_path_cstr) {
  Context ctx;
  ctx_init(&ctx, MB(16));
  int result = build_clean(ctx_perm(), dir_path_cstr);
  ctx_free();
  return result;
}

// `3b format [-w] [--hang N] <file.3b>` -- prints a formatted rendering of the
// file to stdout, or with `-w` rewrites it in place, gofmt-style. Parses only,
// so it works on code that wouldn't pass the checker. Reading then overwriting
// the same path is safe: file_load_str8 reads the whole file into memory up
// front, so nothing still depends on the on-disk bytes when `-w` truncates it.
static int
format_file_cmd(const char* path_cstr, b32 write_in_place, u32 hang) {
  Context ctx;
  ctx_init(&ctx, MB(16));

  String8 path = str8_cstring((char*)path_cstr);
  String8 src  = file_load_str8(ctx_perm(), path);
  if (src.str == NULL) {
    ctx_free();
    return 1;
  }
  u32 file_id = source_file_register(path, src);

  Ast ast;
  ast_init(&ast, ctx_perm());
  Parser p;
  parser_init(&p, src, &ast, file_id);
  NodeIndex root = parse_program(&p);
  if (p.had_error) {
    ctx_free();
    return 1;
  }

  FILE* out = stdout;
  if (write_in_place) {
    out = fopen(path_cstr, "w");
    if (!out) {
      fprintf(stderr, "3b format: could not open '%s' for writing: %s\n", path_cstr, strerror(errno));
      ctx_free();
      return 1;
    }
  }

  fmt_program(out, &ast, root, src, hang);

  if (write_in_place) fclose(out);
  ctx_free();
  return 0;
}

// Does `dir_cstr` directly contain at least one real `.3b` file, a `*.cfg.3b`
// manifest not counting? Plain POSIX opendir/readdir, no arena -- this runs
// before any subcommand's Context exists.
static b32
dir_has_3b_source(const char* dir_cstr) {
  DIR* dir = opendir(dir_cstr);
  if (!dir) return false;
  b32            found = false;
  struct dirent* entry;
  while ((entry = readdir(dir)) != NULL) {
    size_t len = strlen(entry->d_name);
    if (len > 3 && strcmp(entry->d_name + len - 3, ".3b") == 0
        && !(len > 7 && strcmp(entry->d_name + len - 7, ".cfg.3b") == 0)) {
      found = true;
      break;
    }
  }
  closedir(dir);
  return found;
}

// Walks upward from the current working directory, cwd itself first, for the
// nearest directory with real `.3b` source in it, so `3b build`/`3b run`/`3b
// clean` can be invoked with no <package-dir> from inside a project (`cd game
// && 3b run`). Writes the path into `out`, of capacity `out_size`, and returns
// true; returns false with `out` untouched if nothing turns up before the
// filesystem root.
static b32
find_project_root(char* out, size_t out_size) {
  char dir[PATH_MAX];
  if (!getcwd(dir, sizeof(dir))) return false;

  for (;;) {
    if (dir_has_3b_source(dir)) {
      strncpy(out, dir, out_size - 1);
      out[out_size - 1] = 0;
      return true;
    }
    char* slash = strrchr(dir, '/');
    if (!slash || slash == dir) return false; // reached "/" with nothing found
    *slash = 0;
  }
}

static void
print_help(FILE* out) {
  fprintf(out, "usage: 3b <package-dir>              compile a package directory (writes into <dir>/output/, no toolchain invocation)\n");
  fprintf(out, "       3b build [--release] [--no-compile] [package-dir]\n");
  fprintf(out, "                                     compile, then invoke the C toolchain to link a binary (reads <dir>/build.cfg.3b if present)\n");
  fprintf(out, "                                     --release: optimized, no debug assertions (default: debug build, assertions on)\n");
  fprintf(out, "                                     --no-compile: stop after emitting output/ -- same as plain `3b <package-dir>`\n");
  fprintf(out, "       3b run [--release] [package-dir]\n");
  fprintf(out, "                                     `3b build`, then execute the resulting binary\n");
  fprintf(out, "       3b run <file.3bs>|<file.3b>   interpret a script or a package (parse/check/compile/run via the\n");
  fprintf(out, "                                     bytecode VM, no C toolchain involved) -- bare `3b <file>` also works\n");
  fprintf(out, "                                     naming one `.3b` file also compiles its same-package siblings in\n");
  fprintf(out, "                                     that directory, as `3b <package-dir>` does; `(import ...)` is limited\n");
  fprintf(out, "                                     to the modules built into `3b` itself, not other package directories\n");
  fprintf(out, "       3b clean [package-dir]        remove output/ and the linked binary\n");
  fprintf(out, "                                     [package-dir] on build/run/clean defaults to the nearest directory (starting from\n");
  fprintf(out, "                                     the current one, walking up) that has `.3b` files in it\n");
  fprintf(out, "       3b format [-w] [--hang N] <file.3b>\n");
  fprintf(out, "                                     print a formatted rendering of a file (-w rewrites in place)\n");
  fprintf(out, "                                     --hang N: columns to indent a wrapped fn param / struct field\n");
  fprintf(out, "                                     / enum variant list (default 2)\n");
  fprintf(out, "       3b translate <config.3b>|--walk|--config|--extract|--translate ...\n");
  fprintf(out, "                                     C header -> 3b package translator (see `3b translate` for its own usage)\n");
  fprintf(out, "       3b help                       show this message\n");
  fprintf(out, "       3b --version                  print the version\n");
  fprintf(out, "\n");
  fprintf(out, "`3b build|run|clean|format --help` details that one subcommand.\n");
}

// The usage block for one subcommand, for `3b <cmd> --help` and for the error
// paths that want to restate the shape they expected. Kept separate from
// print_help's full listing so asking about one subcommand doesn't answer with
// all of them; the wording is deliberately the same in both places.
static void
print_subcommand_usage(FILE* out, const char* cmd) {
  if (strcmp(cmd, "build") == 0) {
    fprintf(out, "usage: 3b build [--release] [--no-compile] [package-dir]\n");
    fprintf(out, "  compile a package, then invoke the C toolchain to link a binary\n");
    fprintf(out, "  --release      optimized, no debug assertions (default: debug, assertions on)\n");
    fprintf(out, "  --no-compile   stop after emitting output/ -- same as plain `3b <package-dir>`\n");
    fprintf(out, "  [package-dir]  defaults to the nearest directory, here or above, holding `.3b` files\n");
  } else if (strcmp(cmd, "run") == 0) {
    fprintf(out, "usage: 3b run [--release] [package-dir]\n");
    fprintf(out, "       3b run <file.3b>|<file.3bs>\n");
    fprintf(out, "  a package directory: `3b build`, then execute the resulting binary\n");
    fprintf(out, "  a single file: interpret it on the bytecode VM, no C toolchain involved\n");
    fprintf(out, "  --release      optimized, no debug assertions (package directories only)\n");
    fprintf(out, "  [package-dir]  defaults to the nearest directory, here or above, holding `.3b` files\n");
  } else if (strcmp(cmd, "clean") == 0) {
    fprintf(out, "usage: 3b clean [package-dir]\n");
    fprintf(out, "  remove output/ and the linked binary\n");
    fprintf(out, "  [package-dir]  defaults to the nearest directory, here or above, holding `.3b` files\n");
  } else if (strcmp(cmd, "format") == 0) {
    fprintf(out, "usage: 3b format [-w] [--hang N] <file.3b>\n");
    fprintf(out, "  print a formatted rendering of a file to stdout\n");
    fprintf(out, "  -w        rewrite the file in place instead\n");
    fprintf(out, "  --hang N  columns to indent a wrapped fn param / struct field / enum\n");
    fprintf(out, "            variant list (1-16, default 2)\n");
  }
}

// True for the two spellings of "explain this subcommand", accepted wherever a
// subcommand parses its own arguments -- reaching for `--help` after a
// subcommand name is reflex, and it used to be read as a package directory
// named "--help".
static b32
is_help_flag(const char* arg) {
  return strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0;
}

// Names the build in a bug report. The libclang note is part of it because
// `3b translate` is the one subcommand a build can legitimately not have (see
// the Makefile's WITHOUT_TRANSLATE), so "translate does nothing for me" is
// answerable from this line alone.
static void
print_version(FILE* out) {
#ifdef BBB_NO_TRANSLATE
  fprintf(out, "3b %s (built without libclang -- no `3b translate`)\n", BBB_VERSION);
#else
  fprintf(out, "3b %s\n", BBB_VERSION);
#endif
}

int
main(int argc, char** argv) {
  if (argc < 2) {
    print_help(stdout);
    return 0;
  }
  if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "help") == 0) {
    print_help(stdout);
    return 0;
  }
  if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-v") == 0 || strcmp(argv[1], "version") == 0) {
    print_version(stdout);
    return 0;
  }
  if (strcmp(argv[1], "translate") == 0) {
#ifdef BBB_NO_TRANSLATE
    fprintf(stderr, "3b translate: not available -- this build was compiled without libclang (see the Makefile's WITHOUT_TRANSLATE)\n");
    return 1;
#else
    return translate_main(argc - 1, argv + 1);
#endif
  }
  if (strcmp(argv[1], "clean") == 0) {
    char        found[PATH_MAX];
    const char* dir_path_cstr = (argc >= 3) ? argv[2] : NULL;
    if (dir_path_cstr && is_help_flag(dir_path_cstr)) {
      print_subcommand_usage(stdout, "clean");
      return 0;
    }
    if (dir_path_cstr && dir_path_cstr[0] == '-') {
      fprintf(stderr, "3b clean: unknown option '%s'\n", dir_path_cstr);
      print_subcommand_usage(stderr, "clean");
      return 1;
    }
    if (!dir_path_cstr) {
      if (!find_project_root(found, sizeof(found))) {
        fprintf(stderr, "3b clean: no <package-dir> given, and no 3b project (a directory with `.3b` files) "
                         "found here or in any parent directory\n");
        return 1;
      }
      dir_path_cstr = found;
    }
    return clean_project_cmd(dir_path_cstr);
  }
  if (strcmp(argv[1], "build") == 0 || strcmp(argv[1], "run") == 0) {
    b32         run_after   = strcmp(argv[1], "run") == 0;
    b32         release     = false;
    b32         no_compile  = false;
    const char* dir_path_cstr = NULL;
    for (int i = 2; i < argc; i += 1) {
      if      (strcmp(argv[i], "--release") == 0)    release    = true;
      else if (strcmp(argv[i], "--no-compile") == 0) no_compile = true;
      else if (is_help_flag(argv[i])) {
        print_subcommand_usage(stdout, argv[1]);
        return 0;
      } else if (argv[i][0] == '-') {
        // Without this an unrecognized flag became the package directory, and
        // a typo like `--releese` was reported as an unresolvable path.
        fprintf(stderr, "3b %s: unknown option '%s'\n", argv[1], argv[i]);
        print_subcommand_usage(stderr, argv[1]);
        return 1;
      } else                                          dir_path_cstr = argv[i];
    }
    if (run_after && dir_path_cstr && path_is_regular_file(dir_path_cstr)) {
      if (release || no_compile) {
        fprintf(stderr, "3b run: --release/--no-compile don't apply to a `.3b`/`.3bs` FILE "
                         "(runs through the bytecode VM, not the C toolchain)\n");
        return 1;
      }
      return run_script_cmd(dir_path_cstr);
    }
    char found[PATH_MAX];
    if (!dir_path_cstr && find_project_root(found, sizeof(found))) dir_path_cstr = found;
    if (!dir_path_cstr) {
      fprintf(stderr, "usage: 3b %s [--release]%s <package-dir>\n", argv[1], run_after ? "" : " [--no-compile]");
      fprintf(stderr, "3b %s: no <package-dir> given, and no 3b project (a directory with `.3b` files) "
                       "found here or in any parent directory\n", argv[1]);
      return 1;
    }
    if (run_after && no_compile) {
      fprintf(stderr, "3b run: --no-compile doesn't make sense here -- `run` has to build before it can execute\n");
      return 1;
    }
    return build_project_cmd(dir_path_cstr, run_after, release, no_compile);
  }
  if (strcmp(argv[1], "format") == 0) {
    b32         write_in_place = false;
    u32         hang           = 0; // 0 = leave fmt_program's default in place
    const char* path_cstr      = NULL;
    for (int i = 2; i < argc; i += 1) {
      if (strcmp(argv[i], "-w") == 0) {
        write_in_place = true;
      } else if (is_help_flag(argv[i])) {
        print_subcommand_usage(stdout, "format");
        return 0;
      } else if (strcmp(argv[i], "--hang") == 0) {
        if (i + 1 >= argc) {
          fprintf(stderr, "3b format: --hang needs a column count, e.g. `--hang 4`\n");
          return 1;
        }
        i += 1;
        char* end   = NULL;
        long  value = strtol(argv[i], &end, 10);
        if (*argv[i] == 0 || (end && *end != 0) || value < 1 || value > 16) {
          fprintf(stderr, "3b format: --hang '%s' is not a column count between 1 and 16\n", argv[i]);
          return 1;
        }
        hang = (u32)value;
      } else if (argv[i][0] == '-') {
        fprintf(stderr, "3b format: unknown option '%s'\n", argv[i]);
        print_subcommand_usage(stderr, "format");
        return 1;
      } else {
        path_cstr = argv[i];
      }
    }
    if (!path_cstr) {
      fprintf(stderr, "3b format: no <file.3b> given\n");
      print_subcommand_usage(stderr, "format");
      return 1;
    }
    return format_file_cmd(path_cstr, write_in_place, hang);
  }
  // Everything below treats argv[1] as a path. A leading '-' never is one --
  // no subcommand above takes an option in this position -- and letting it
  // fall through reported an unknown flag as "can't resolve this package
  // directory", which describes the wrong problem entirely.
  if (argv[1][0] == '-') {
    fprintf(stderr, "3b: unknown option '%s' -- see `3b help`\n", argv[1]);
    return 1;
  }
  if (path_is_regular_file(argv[1])) return run_script_cmd(argv[1]);
  // A bare word that names nothing on disk is far more likely a mistyped
  // subcommand (`3b buidl`) than a package directory, and "can't resolve this
  // package directory" answers a question that wasn't asked. Anything with a
  // '/' or a '.' in it was clearly meant as a path, so it keeps the
  // path-shaped diagnostic from compile_package_dir.
  if (!path_exists(argv[1]) && !strchr(argv[1], '/') && !strchr(argv[1], '.')) {
    fprintf(stderr, "3b: unknown subcommand '%s' (and no such package directory) -- see `3b help`\n", argv[1]);
    return 1;
  }
  return compile_package_dir(argv[1]);
}
