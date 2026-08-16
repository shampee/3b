// The C header -> 3b package translator, exposed as `3b translate`. It reads a
// config file plus a set of C headers and emits a checked-in 3b package.
//
// It links libclang and lives in its own subdirectory, but is otherwise a mode
// of the 3b binary like `3b format` (see main.c's dispatch on argv[1]). It
// needs none of the lowering, checking or codegen machinery.
//
// Two config formats are accepted, both producing the same `Config`, which
// cwalk_extract and emit_package read without knowing which built it:
//   .cfg.3b   a declarative DSL in 3b's reader syntax with its own top-level
//             forms, read by config.c. Never lowered, checked or run.
//   .3bs      a real 3bscript, compiled and run on the bytecode VM (see
//             load_config_script), for what the DSL cannot express -- shelling
//             out to pkg-config, or reading an env var to decide what to
//             configure.
#include "translate.h"
#include "clang-c/Index.h"
#include "../script.h"
#include "../bcosprims.h"
#include "../bcconfigprims.h"
#include <stdio.h>
#include <sys/stat.h>
#include <errno.h>
#include <string.h>
#include <limits.h>

static enum CXChildVisitResult
smoke_visit_top_level(CXCursor c, CXCursor parent, CXClientData client_data) {
  (void)parent;
  u32* count = (u32*)client_data;
  CXSourceLocation loc = clang_getCursorLocation(c);
  if (!clang_Location_isFromMainFile(loc)) return CXChildVisit_Continue;
  CXString name = clang_getCursorSpelling(c);
  CXString kind = clang_getCursorKindSpelling(clang_getCursorKind(c));
  printf("%s: %s\n", clang_getCString(kind), clang_getCString(name));
  clang_disposeString(kind);
  clang_disposeString(name);
  *count += 1;
  return CXChildVisit_Continue;
}

static int
cmd_walk(const char* header_path) {
  CXIndex index = clang_createIndex(/*excludeDeclsFromPCH*/ 1, /*displayDiagnostics*/ 1);
  const char* args[] = { "-x", "c" };
  CXTranslationUnit tu;
  enum CXErrorCode err = clang_parseTranslationUnit2(
    index, header_path, args, 2, NULL, 0, CXTranslationUnit_None, &tu);
  if (err != CXError_Success) {
    fprintf(stderr, "clang_parseTranslationUnit2 failed: %d\n", err);
    clang_disposeIndex(index);
    return 1;
  }

  CXCursor root = clang_getTranslationUnitCursor(tu);
  u32 top_level_count = 0;
  clang_visitChildren(root, smoke_visit_top_level, &top_level_count);

  printf("-- %u top-level declaration(s) from %s --\n", top_level_count, header_path);

  clang_disposeTranslationUnit(tu);
  clang_disposeIndex(index);
  return 0;
}

// Directory portion of `path`: "foo/bar/baz.3b" -> "foo/bar", "baz.3b" -> ".".
// Trailing slashes need no handling; a config path always names a file.
static String8
str8_dirname(String8 path) {
  for (u64 i = path.size; i > 0; i -= 1) {
    if (char_is_slash(path.str[i - 1])) return str8_substr(path, rng_1u64(0, i - 1));
  }
  return str8_lit(".");
}

// Last path component, ignoring trailing slashes: "foo/bar/" and "foo/bar" both
// give "bar". Mirrors main.c's package_name_from_dir_path, copied rather than
// shared since it is the only thing translate.c would need from main.c.
static String8
str8_basename(String8 path) {
  String8 trimmed = path;
  while (trimmed.size > 0 && char_is_slash(trimmed.str[trimmed.size - 1])) trimmed.size -= 1;
  u64 last_slash_end = 0;
  for (u64 i = trimmed.size; i > 0; i -= 1) {
    if (char_is_slash(trimmed.str[i - 1])) { last_slash_end = i; break; }
  }
  return str8_substr(trimmed, rng_1u64(last_slash_end, trimmed.size));
}

// Makes cwalk_extract's "-I." header search and cmd_translate's output
// placement config-relative rather than CWD-relative. Without this, a config
// invoked by a relative path (`sdl/sdl.cfg.3b`) fails to resolve headers named
// as bare filenames, and writes its output beside the wrong directory.
//
// The CWD is never restored: main.c exits right after translate_main returns,
// so nothing later in the process is CWD-relative.
static b32
enter_config_dir(const char* config_path) {
  String8 dir = str8_dirname(str8_cstring((char*)config_path));
  if (chdir(cstr_from_str8_temp(dir)) != 0) {
    fprintf(stderr, "could not enter config directory '%.*s': %s\n", str8_varg(dir), strerror(errno));
    return false;
  }
  return true;
}

// Runs a `.3bs` config script through script_run_file, rather than reading the
// declarative DSL. Both primitive sets go into one host_imports table: the
// OS-facing ones from bcosprims.c, via `(import build)`, and the
// Config-mutating ones from bcconfigprims.c, via `(import config)`. A config
// script wants both at once -- capture a `pkg-config` invocation, then
// `config-add-header` what it printed. main.c's run_script_cmd registers only
// the OS set, having no `Config*` to bind the other to.
//
// `out_cfg` must be zeroed by the caller, as load_config does:
// bc_register_config_primitives stores the pointer without initializing what it
// points at.
static b32
load_config_script(const char* config_path, Config* out_cfg) {
  Arena*             arena = ctx_perm();
  BcHostImportTable  host_imports = {0};
  bc_register_os_primitives(&host_imports, arena);
  bc_register_config_primitives(&host_imports, arena, out_cfg);
  BcResult result;
  return script_run_file(config_path, &host_imports, &result);
}

static b32
load_config(const char* config_path, const char* platform, Config* out_cfg) {
  MemoryZeroStruct(out_cfg);
  String8 path = str8_cstring((char*)config_path);
  if (str8_ends_with(path, str8_lit(".3bs"), 0)) return load_config_script(config_path, out_cfg);
  File f = file_load(ctx_perm(), path);
  if (!f.view.data) {
    fprintf(stderr, "could not read config file '%s'\n", config_path);
    return false;
  }
  String8 src     = str8_from_view(f.view);
  u32     file_id = source_file_register(path, src);
  return config_read(ctx_perm(), src, platform, out_cfg, file_id);
}

static int
cmd_config(const char* config_path, const char* platform) {
  Config cfg;
  b32    ok = load_config(config_path, platform, &cfg);
  config_print(&cfg);
  return ok ? 0 : 1;
}

static int
cmd_extract(const char* config_path, const char* platform) {
  Config cfg;
  if (!load_config(config_path, platform, &cfg)) return 1;
  if (!enter_config_dir(config_path)) return 1;

  CUnit unit;
  b32 ok = cwalk_extract(ctx_perm(), &cfg, platform, &unit);
  cwalk_print_unit(&unit);
  return ok ? 0 : 1;
}

// `--names`: everything cmd_translate does up to emitting, then the naming
// table instead of a package. It writes to stdout because it exists to be read,
// grepped and diffed against the previous run while tuning a
// `rename-*-pattern`, not to be checked in.
static int
cmd_names(const char* config_path, const char* platform) {
  Config cfg;
  if (!load_config(config_path, platform, &cfg)) return 1;
  if (!enter_config_dir(config_path)) return 1;

  CUnit unit;
  if (!cwalk_extract(ctx_perm(), &cfg, platform, &unit)) return 1;
  emit_name_report(&cfg, &unit, stdout);
  return 0;
}

static int
cmd_translate(const char* config_path, const char* platform) {
  Config cfg;
  if (!load_config(config_path, platform, &cfg)) return 1;
  if (!enter_config_dir(config_path)) return 1;

  CUnit unit;
  b32 ok = cwalk_extract(ctx_perm(), &cfg, platform, &unit);
  if (!ok) return 1;

  const char* pkg_name = cstr_from_str8_temp(cfg.package_name);

  char cwd_buf[PATH_MAX];
  if (!getcwd(cwd_buf, sizeof(cwd_buf))) {
    fprintf(stderr, "could not resolve the config directory's own path: %s\n", strerror(errno));
    return 1;
  }
  String8 dir_name = str8_basename(str8_cstring(cwd_buf));

  // Before anything is created or written: a struct mirror that doesn't match
  // its C original corrupts memory silently, so the only safe response is to
  // produce no package at all, leaving whatever was there last time intact.
  if (!verify_record_layouts(&cfg, &unit)) return 1;
  if (!verify_type_names(&cfg, &unit)) return 1;

  char out_path[1024];
  char byval_path[1024];
  if (str8_match(dir_name, cfg.package_name, 0)) {
    // The config already lives in a directory named after its package, as when
    // re-running `sdl/sdl.cfg.3b` in place. Write back into that directory:
    // creating `<pkg>/<pkg>.3b` underneath would nest another same-named
    // directory on every re-run.
    snprintf(out_path, sizeof(out_path), "%s.3b", pkg_name);
    snprintf(byval_path, sizeof(byval_path), "%s_byval.h", pkg_name);
  } else {
    if (mkdir(pkg_name, 0755) != 0 && errno != EEXIST) {
      fprintf(stderr, "could not create package directory '%s': %s\n", pkg_name, strerror(errno));
      return 1;
    }
    snprintf(out_path, sizeof(out_path), "%s/%s.3b", pkg_name, pkg_name);
    snprintf(byval_path, sizeof(byval_path), "%s/%s_byval.h", pkg_name, pkg_name);
  }
  FILE* out = file_open(out_path, "w");
  if (!out) return 1;

  // The C bridge header, next to the package so `3b build` finds it by name
  // (see build.c). It is opened before anything is known about whether a shim
  // will be needed, and removed below if none was: a binding with no by-value
  // API in it should not acquire a second file. Removing it is also what keeps
  // a regeneration authoritative -- a header left over from a config that used
  // to need one would be `-include`d forever after.
  u32   byval_count = 0;
  FILE* byval_out   = file_open(byval_path, "w");
  if (!byval_out) return 1;

  b32 clean = emit_package(&cfg, &unit, out, byval_out, &byval_count);
  fclose(out);
  fclose(byval_out);

  printf("wrote %s\n", out_path);
  if (byval_count > 0) {
    printf("wrote %s (%u by-value struct bridge(s))\n", byval_path, byval_count);
  } else if (remove(byval_path) != 0) {
    fprintf(stderr, "could not remove the unused bridge header '%s': %s\n", byval_path, strerror(errno));
    return 1;
  }
  return clean ? 0 : 1;
}

int
translate_main(int argc, char** argv) {
  Context ctx;
  ctx_init(&ctx, MB(64));

  int result = 1;
  if (argc >= 3 && str8_match_lit("--walk", str8_cstring(argv[1]), 0)) {
    result = cmd_walk(argv[2]);
  } else if (argc >= 3 && str8_match_lit("--config", str8_cstring(argv[1]), 0)) {
    const char* platform = (argc >= 5 && str8_match_lit("--platform", str8_cstring(argv[3]), 0)) ? argv[4] : NULL;
    result = cmd_config(argv[2], platform);
  } else if (argc >= 3 && str8_match_lit("--extract", str8_cstring(argv[1]), 0)) {
    const char* platform = (argc >= 5 && str8_match_lit("--platform", str8_cstring(argv[3]), 0)) ? argv[4] : NULL;
    result = cmd_extract(argv[2], platform);
  } else if (argc >= 3 && str8_match_lit("--names", str8_cstring(argv[1]), 0)) {
    const char* platform = (argc >= 5 && str8_match_lit("--platform", str8_cstring(argv[3]), 0)) ? argv[4] : NULL;
    result = cmd_names(argv[2], platform);
  } else if (argc >= 3 && str8_match_lit("--translate", str8_cstring(argv[1]), 0)) {
    const char* platform = (argc >= 5 && str8_match_lit("--platform", str8_cstring(argv[3]), 0)) ? argv[4] : NULL;
    result = cmd_translate(argv[2], platform);
  } else if (argc >= 2 && !str8_starts_with(str8_cstring(argv[1]), str8_lit("--"), 0)) {
    // A bare config path means --translate. `.3bs` works here too; load_config
    // dispatches on the extension.
    const char* platform = (argc >= 4 && str8_match_lit("--platform", str8_cstring(argv[2]), 0)) ? argv[3] : NULL;
    result = cmd_translate(argv[1], platform);
  } else {
    fprintf(stderr, "usage: 3b translate <config.3b|config.3bs> [--platform NAME]\n");
    fprintf(stderr, "       3b translate --walk <header.h>\n");
    fprintf(stderr, "       3b translate --config <config.3b|config.3bs> [--platform NAME]\n");
    fprintf(stderr, "       3b translate --extract <config.3b|config.3bs> [--platform NAME]\n");
    fprintf(stderr, "       3b translate --names <config.3b|config.3bs> [--platform NAME]\n");
    fprintf(stderr, "       3b translate --translate <config.3b|config.3bs> [--platform NAME]\n");
  }

  ctx_free();
  return result;
}
