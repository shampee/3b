// `3b build`/`3b run` -- see build.h. Two halves: a small config DSL reader,
// which reuses 3b's own lexer and parser for a generic atom/list/vector AST and
// walks it by hand (the same approach as translate/config.c), and a backend
// that shells out to `cc` the way a hand-written Makefile would, assembled here
// instead of typed out per project.
#include "build.h"
#include "compiler.h" // OUTPUT_DIR
#include "file.h"
#include "native_pkgs_embed.h" // g_embed_native_pkgs_vm_* -- see `has_vm` in build_invoke_toolchain
#include <limits.h>
#include <sys/stat.h>
#if !defined(_WIN32)
# include <sys/wait.h>
# include <unistd.h>
#endif

static b32 g_had_error;

////////////////////////////////
//~ Self-location -- finding liblib3b.a (see maybe_link_vm_support below)

// Absolute path of the currently running `3b` binary. The compiler has no
// install step -- it is built and run out of its own source tree -- so
// liblib3b.a, a sibling artifact of the same Makefile (LIB_TARGET), always sits
// next to wherever `3b` lives. Empty String8 on any failure; callers must treat
// that as "couldn't find it" rather than crash, since `vm` support is opt-in.
// Linux and Windows only; macOS would need `_NSGetExecutablePath`.
static String8
self_binary_path(Arena* arena) {
#if defined(_WIN32)
  char  buf[PATH_MAX];
  DWORD len = GetModuleFileNameA(NULL, buf, sizeof(buf));
  if (len == 0 || len == sizeof(buf)) return (String8){0};
  return str8_copy(arena, str8((u8*)buf, len));
#else
  char    buf[PATH_MAX];
  ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (len <= 0) return (String8){0};
  return str8_copy(arena, str8((u8*)buf, (u64)len));
#endif
}

// liblib3b.a next to the running `3b` binary, or an empty String8 if
// self-location failed or the file isn't there (`make lib` never run) -- either
// way "not available", not a hard error.
static String8
self_liblib3b_path(Arena* arena) {
  String8 exe = self_binary_path(arena);
  if (exe.size == 0) return (String8){0};

  u64     after_slash = str8_find_needle_reverse(exe, 0, str8_lit("/"), 0);
  String8 exe_dir     = str8_prefix(exe, after_slash); // includes the trailing slash itself
  String8 lib_path    = str8_cat(arena, exe_dir, str8_lit("liblib3b.a"));

  struct stat st;
  if (stat((char*)cstr_from_str8_temp(lib_path), &st) != 0) return (String8){0};
  return lib_path;
}

// Quotes `s` as a single shell word. Everything below builds command strings by
// direct concatenation (see run_shell_script), so any dynamic value that can
// contain a space or shell metacharacter -- a project dir_path, a
// manifest-supplied binary/lib/pkg-config name, a c-sources entry -- must pass
// through this where it is interpolated. Otherwise a space splits one path into
// two argv words, or a crafted manifest field runs arbitrary shell. POSIX wraps
// in '...' with embedded ' written as '\''; cmd.exe, also reached through
// system(), wraps in "..." and doubles embedded ".
static String8
shell_quote(Arena* arena, String8 s) {
  String8List list = {0};
#if defined(_WIN32)
  str8_list_push(arena, &list, str8_lit("\""));
  foreach_index(i, s.size) {
    String8 lit = str8_range(s.str + i, s.str + i + 1);
    str8_list_push(arena, &list, lit);
    if (s.str[i] == '"') str8_list_push(arena, &list, lit); // double it
  }
  str8_list_push(arena, &list, str8_lit("\""));
#else
  str8_list_push(arena, &list, str8_lit("'"));
  foreach_index(i, s.size) {
    if (s.str[i] == '\'') str8_list_push(arena, &list, str8_lit("'\\''"));
    else str8_list_push(arena, &list, str8_range(s.str + i, s.str + i + 1));
  }
  str8_list_push(arena, &list, str8_lit("'"));
#endif
  StringJoin no_join = {0};
  return str8_list_join(arena, &list, &no_join);
}

////////////////////////////////
//~ pkg-config

// Splits pkg-config's output into individual flags, undoing the shell quoting
// it applies to any flag containing a space: `-I/opt/my\ sdk/include`, or the
// same path wrapped in '...' or "...". A plain split on whitespace would tear
// such a flag into two words. Unescaping here, instead of handing the raw text
// to the shell, is what lets each flag be re-quoted as exactly one word by
// pkg_config_query below.
static String8List
pkg_config_split_flags(Arena* arena, String8 text) {
  String8List out     = {0};
  u8*         buf     = NULL; // dyn array -- the current flag's unescaped bytes
  b32         in_flag = false;
  u8          quote   = 0;
  for (u64 i = 0; i < text.size; i += 1) {
    u8 c = text.str[i];
    if (quote != 0) {
      if (c == quote) { quote = 0; continue; }
      // A backslash is literal inside '...', an escape inside "...".
      if (c == '\\' && quote == '"' && i + 1 < text.size) { i += 1; c = text.str[i]; }
      dyn_push(arena, buf, c);
      continue;
    }
    if (c == '\\' && i + 1 < text.size) {
      i += 1;
      dyn_push(arena, buf, text.str[i]);
      in_flag = true;
      continue;
    }
    if (c == '\'' || c == '"') { quote = c; in_flag = true; continue; }
    if (char_is_space(c)) {
      if (in_flag) {
        str8_list_push(arena, &out, str8(buf, dyn_count(buf)));
        buf     = NULL;
        in_flag = false;
      }
      continue;
    }
    dyn_push(arena, buf, c);
    in_flag = true;
  }
  if (in_flag) str8_list_push(arena, &out, str8(buf, dyn_count(buf)));
  return out;
}

// Runs `pkg-config <mode> <pkg>...` for every manifest `(pkg-config [...])`
// package and returns its flags, each re-quoted as a single shell word (see
// shell_quote) and joined by spaces, ready to splice into a command line.
//
// This used to be spelled `$(pkg-config --cflags ...)` straight into the
// command string handed to system(). That only works because a POSIX shell
// performs command substitution; cmd.exe has no such syntax and passes the text
// through to the compiler verbatim, so on Windows a pkg-config manifest failed
// with something unrecognizable rather than reporting a real problem. Running
// pkg-config here, through popen, behaves identically on both platforms -- and
// takes one more thing off the list of what the build path needs a shell for.
//
// Returns false, having said why, if pkg-config is missing or reports an error;
// an empty flag string would otherwise surface much later as a pile of missing
// headers or undefined symbols.
static b32
pkg_config_query(Arena* arena, const char* mode, String8* pkgs, String8* out_flags) {
  String8 cmd = str8f(arena, "pkg-config %s", mode);
  foreach_index(i, dyn_count(pkgs)) {
    cmd = str8f(arena, "%.*s %.*s", str8_varg(cmd), str8_varg(shell_quote(arena, pkgs[i])));
  }

  // pkg-config's own stderr is deliberately left attached to ours: its
  // "Package foo was not found in the pkg-config search path" is a better
  // diagnostic than anything reconstructible from an exit code.
#if defined(_WIN32)
  FILE* p = _popen(cstr_from_str8_temp(cmd), "r");
#else
  FILE* p = popen(cstr_from_str8_temp(cmd), "r");
#endif
  if (!p) {
    fprintf(stderr, "3b build: failed to run `%.*s`: %s\n", str8_varg(cmd), strerror(errno));
    return false;
  }

  u8*    out = NULL; // dyn array -- pkg-config's stdout
  u8     chunk[1024];
  size_t n;
  while ((n = fread(chunk, 1, sizeof(chunk), p)) > 0) {
    foreach_index(i, n) dyn_push(arena, out, chunk[i]);
  }

#if defined(_WIN32)
  int exit_code = _pclose(p); // cmd.exe hands back the child's exit code directly
#else
  int status    = pclose(p);
  int exit_code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
#endif
  if (exit_code != 0) {
    // "command not found" -- 127 from a POSIX shell, 9009 from cmd.exe -- is
    // worth naming: a missing pkg-config is a different fix from a missing
    // .pc file, and there is no standard pkg-config on Windows at all.
    b32         missing = exit_code == 127 || exit_code == 9009;
    const char* hint    = missing ? " -- is pkg-config installed and on PATH?"
                                  : " -- see its message above";
    fprintf(stderr, "3b build: `%.*s` failed (exit %d)%s\n", str8_varg(cmd), exit_code, hint);
    return false;
  }

  String8List flags   = pkg_config_split_flags(arena, str8(out, dyn_count(out)));
  StringJoin  join_sp = {.sep = str8_lit(" ")};
  String8List quoted  = {0};
  for (String8Node* node = flags.first; node != NULL; node = node->next) {
    str8_list_push(arena, &quoted, shell_quote(arena, node->string));
  }
  *out_flags = str8_list_join(arena, &quoted, &join_sp);
  return true;
}

static void
cfg_error(Token tok, const char* fmt, ...) {
  g_had_error = true;
  va_list args;
  va_start(args, fmt);
  diag_errorv(tok, fmt, args);
  va_end(args);
}

static String8
node_atom_text(Ast* ast, NodeIndex idx) {
  AstNode* n = ast_get(ast, idx);
  if (n->kind != AstNodeKind_Atom) {
    cfg_error(n->token, "expected a bare name here, got a %s", ast_node_kind_name(n->kind));
    return str8_lit("");
  }
  return n->token.text;
}

static String8
node_string_text(Ast* ast, NodeIndex idx) {
  AstNode* n = ast_get(ast, idx);
  if (n->kind != AstNodeKind_String) {
    cfg_error(n->token, "expected a \"quoted string\" here, got a %s", ast_node_kind_name(n->kind));
    return str8_lit("");
  }
  return n->token.text;
}

static NodeIndex*
node_seq_children(Ast* ast, NodeIndex idx, AstNodeKind want_kind, u16* out_count) {
  AstNode* n = ast_get(ast, idx);
  if (n->kind != want_kind) {
    cfg_error(n->token, "expected a %s here, got a %s", ast_node_kind_name(want_kind), ast_node_kind_name(n->kind));
    *out_count = 0;
    return NULL;
  }
  return ast_seq_children(ast, idx, out_count);
}

////////////////////////////////
//~ Per-form handlers

static void
handle_binary(Arena* arena, Ast* ast, NodeIndex* fc, u16 n, BuildConfig* cfg) {
  (void)arena;
  if (n != 2) { cfg_error(ast_get(ast, fc[0])->token, "`binary` takes exactly one \"name\""); return; }
  cfg->binary_name = node_string_text(ast, fc[1]);
}

static void
handle_string_list(Arena* arena, Ast* ast, NodeIndex* fc, u16 n, String8** out, const char* form_name) {
  if (n != 2) { cfg_error(ast_get(ast, fc[0])->token, "`%s` takes exactly one `[...]` vector", form_name); return; }
  u16        vc;
  NodeIndex* vchildren = node_seq_children(ast, fc[1], AstNodeKind_Vector, &vc);
  foreach_index(i, vc) {
    dyn_push(arena, *out, node_string_text(ast, vchildren[i]));
  }
}

static void
handle_kind(Ast* ast, NodeIndex* fc, u16 n, BuildConfig* cfg) {
  if (n != 2) { cfg_error(ast_get(ast, fc[0])->token, "`kind` takes exactly one bare name (`library` or `binary`)"); return; }
  String8 val = node_atom_text(ast, fc[1]);
       if (str8_match_lit("binary",  val, 0)) cfg->kind = PackageKind_Binary;
  else if (str8_match_lit("library", val, 0)) cfg->kind = PackageKind_Library;
  else cfg_error(ast_get(ast, fc[1])->token, "`kind` must be `library` or `binary`, got `%.*s`", str8_varg(val));
}

static void
handle_include_first(Arena* arena, Ast* ast, NodeIndex* fc, u16 n, BuildConfig* cfg) {
  if (n < 3 || (n % 2) != 1) {
    cfg_error(ast_get(ast, fc[0])->token, "`include-first` takes `pkg [header ...]` pairs");
    return;
  }
  for (u32 i = 1; i < n; i += 2) {
    BuildIncludeFirst entry = {0};
    entry.package_name       = node_atom_text(ast, fc[i]);
    u16        hc;
    NodeIndex* hchildren = node_seq_children(ast, fc[i + 1], AstNodeKind_Vector, &hc);
    foreach_index(h, hc) {
      dyn_push(arena, entry.headers, node_string_text(ast, hchildren[h]));
    }
    dyn_push(arena, cfg->include_first, entry);
  }
}

b32
build_config_read(Arena* arena, String8 src, BuildConfig* out, u32 file_id) {
  g_had_error = false;
  MemoryZeroStruct(out);

  Ast ast;
  ast_init(&ast, arena);
  Parser p;
  parser_init(&p, src, &ast, file_id);
  NodeIndex root = parse_program(&p);
  if (p.had_error) return false;

  u16        form_count;
  NodeIndex* forms = ast_seq_children(&ast, root, &form_count);
  foreach_index(i, form_count) {
    NodeIndex form_idx = forms[i];
    AstNode*  form      = ast_get(&ast, form_idx);
    if (form->kind != AstNodeKind_List) {
      cfg_error(form->token, "top-level config entries must be `(form ...)` lists");
      continue;
    }
    u16        fc;
    NodeIndex* fchildren = ast_seq_children(&ast, form_idx, &fc);
    if (fc == 0) { cfg_error(form->token, "empty top-level form"); continue; }

    String8 op = node_atom_text(&ast, fchildren[0]);
    if (op.size == 0) continue;

         if (str8_match_lit("binary",         op, 0)) handle_binary(arena, &ast, fchildren, fc, out);
    else if (str8_match_lit("c-sources",      op, 0)) handle_string_list(arena, &ast, fchildren, fc, &out->c_sources, "c-sources");
    else if (str8_match_lit("pkg-config",     op, 0)) handle_string_list(arena, &ast, fchildren, fc, &out->pkg_config_pkgs, "pkg-config");
    else if (str8_match_lit("libs",           op, 0)) handle_string_list(arena, &ast, fchildren, fc, &out->libs, "libs");
    else if (str8_match_lit("static-libs",    op, 0)) handle_string_list(arena, &ast, fchildren, fc, &out->static_libs, "static-libs");
    else if (str8_match_lit("include-first",  op, 0)) handle_include_first(arena, &ast, fchildren, fc, out);
    else if (str8_match_lit("kind",           op, 0)) handle_kind(&ast, fchildren, fc, out);
    else cfg_error(form->token, "unknown build config form `%.*s`", str8_varg(op));
  }

  return !g_had_error;
}

////////////////////////////////
//~ Toolchain invocation

// Strips directory components off `path`: "glad/glad_impl.c" ->
// "glad_impl.c". Manifest `c-sources` entries may nest under subdirectories,
// but their object files all land flat in OUTPUT_DIR alongside every generated
// package's own `.o` -- OUTPUT_DIR is the only directory compiler.c's
// ensure_output_dir guarantees exists.
static String8
str8_basename(String8 path) {
  u64 after_slash = str8_find_needle_reverse(path, 0, str8_lit("/"), 0);
  return str8_skip(path, after_slash);
}

static String8*
include_first_headers_for(BuildConfig* cfg, String8 pkg_name) {
  foreach_index(i, dyn_count(cfg->include_first)) {
    if (str8_match(cfg->include_first[i].package_name, pkg_name, 0)) return cfg->include_first[i].headers;
  }
  return NULL;
}

// Appends `-include <header>` for every header include_first_headers_for found
// for this package, then `-c <src> -o <obj>` -- one line of the shell script
// build_invoke_toolchain assembles below.
static String8
append_compile_line(Arena* arena, String8 cmd, String8 cc_var, String8 cflags, String8* headers, String8 src, String8 obj) {
  cmd = str8_cat(arena, cmd, shell_quote(arena, cc_var));
  cmd = str8_cat(arena, cmd, str8_lit(" "));
  cmd = str8_cat(arena, cmd, cflags); // pre-assembled and multi-word by design; its dynamic
                                          // pieces are quoted where cflags is built
  foreach_index(i, dyn_count(headers)) {
    cmd = str8_cat(arena, cmd, str8_lit(" -include "));
    cmd = str8_cat(arena, cmd, shell_quote(arena, headers[i]));
  }
  cmd = str8_cat(arena, cmd, str8_lit(" -c "));
  cmd = str8_cat(arena, cmd, shell_quote(arena, src));
  cmd = str8_cat(arena, cmd, str8_lit(" -o "));
  cmd = str8_cat(arena, cmd, shell_quote(arena, obj));
  return cmd;
}

// Appends `line` to `script` as its own `&&`-chained step; `*first_line` tracks
// whether a separator is needed yet. Under /bin/sh the separator ends in a
// newline, so a build echoes as readably as Make's own multi-line commands.
// cmd.exe has no shell-level line continuation and treats each line of a
// multi-line string as a separate command, so there the whole chain has to stay
// on one line.
static String8
append_script_line(Arena* arena, String8 script, String8 line, b32* first_line) {
#if defined(_WIN32)
  String8 sep = str8_lit(" && ");
#else
  String8 sep = str8_lit(" &&\n");
#endif
  script = str8_cat(arena, script, *first_line ? str8_lit("") : sep);
  script = str8_cat(arena, script, line);
  *first_line = false;
  return script;
}

// Runs `script`, a `&&`-chained shell script (see append_script_line), through
// whatever shell system() uses on this platform. Echoes it first, so a build
// reads the same way `make`'s command echoing does.
static int
run_shell_script(String8 script) {
  printf("%.*s\n", str8_varg(script));
#if defined(_WIN32)
  // Wrapped in one more pair of quotes than it needs. `cmd /c` strips the
  // first and last quote character of its argument whenever the argument
  // starts with a quote and isn't the two-quotes-around-a-bare-program-name
  // special case -- and every line here starts with a quoted compiler name
  // (shell_quote on cc_var). Without the extra pair, cmd eats the quote
  // before `gcc` and the closing quote of the last argument, then reports the
  // whole mangled line as an unrecognized command. With it, cmd strips the
  // pair we added and runs what was actually meant.
  String8 to_run = str8f(ctx_scratch(), "\"%.*s\"", str8_varg(script));
#else
  String8 to_run = script;
#endif
  int status = system(cstr_from_str8_temp(to_run));
  if (status == -1) {
    fprintf(stderr, "3b build: failed to launch shell\n");
    return 1;
  }
#if defined(_WIN32)
  // Windows' system(), via cmd.exe /c, returns the child's exit code directly
  // -- no WIFEXITED/WEXITSTATUS encoding to unpack.
  return status;
#else
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  return 1;
#endif
}

// Reads `<dir_path>/build.cfg.3b` into `out` if it exists. Leaves `*out` zeroed
// -- a valid, empty BuildConfig -- when there is no manifest, which is the
// common case, not an error. Returns false only on a real failure (unreadable
// file, malformed manifest), with a diagnostic already printed to stderr
// prefixed by `context`, the calling subcommand's name.
static b32
read_manifest_if_present(Arena* arena, String8 dir_path, BuildConfig* out, const char* context) {
  MemoryZeroStruct(out);
  String8 manifest_path = str8_cat(arena, dir_path, str8_lit("/build.cfg.3b"));
  if (access((char*)cstr_from_str8_temp(manifest_path), F_OK) != 0) return true;

  String8 src = file_load_str8(arena, manifest_path);
  if (src.str == NULL) {
    fprintf(stderr, "%s: could not read '%.*s'\n", context, str8_varg(manifest_path));
    return false;
  }
  u32 file_id = source_file_register(manifest_path, src);
  if (!build_config_read(arena, src, out, file_id)) {
    fprintf(stderr, "%s: '%.*s' failed to parse (see errors above)\n", context, str8_varg(manifest_path));
    return false;
  }
  return true;
}

// See build.h. Diagnostics are captured and discarded around the read so a
// malformed manifest doesn't print twice -- the real `3b build`/`3b run` path
// reports it properly.
PackageKind
build_config_read_kind(Arena* arena, const char* dir_path_cstr) {
  String8 dir_path = str8_cstring((char*)dir_path_cstr);
  BuildConfig cfg = {0};
  diag_capture_begin(/*also_print=*/false);
  read_manifest_if_present(arena, dir_path, &cfg, "3b (kind)");
  diag_capture_end(NULL);
  return cfg.kind;
}

// The default binary name for `dir_path` when no manifest `(binary ...)`
// overrides it. A package's `(package X)` name always matches its directory's
// basename, so resolving that basename -- through realpath, so a relative path
// like `.` or a trailing slash doesn't become the name -- gives what
// build_invoke_toolchain would default to without compiling anything. `3b
// clean` uses it to find the binary to remove even when the project no longer
// compiles.
static String8
dir_default_name(Arena* arena, String8 dir_path) {
  char resolved[PATH_MAX];
#if defined(_WIN32)
  b32 resolved_ok = _fullpath(resolved, (char*)cstr_from_str8_temp(dir_path), PATH_MAX) != NULL;
#else
  b32 resolved_ok = realpath((char*)cstr_from_str8_temp(dir_path), resolved) != NULL;
#endif
  if (resolved_ok) {
    return str8_basename(str8_copy(arena, str8_cstring(resolved)));
  }
  String8 trimmed = dir_path;
  while (trimmed.size > 0 && char_is_slash(trimmed.str[trimmed.size - 1])) trimmed.size -= 1;
  return str8_basename(trimmed);
}

// Writes an embed_runtime-style per-line array straight to `out`; codegen.c's
// cg_fputs_lines is its counterpart.
static void
write_embedded_lines(FILE* out, const char* const* lines) {
  for (const char* const* p = lines; *p; p += 1) fputs(*p, out);
}

int
build_invoke_toolchain(Arena* arena, const char* dir_path_cstr, String8 root_pkg_name,
                        String8* pkg_names, String8* pkg_dirs, u64 pkg_count,
                        b32 run_after, b32 release) {
  String8 dir_path   = str8_cstring((char*)dir_path_cstr);
  String8 dir_path_q = shell_quote(arena, dir_path); // command lines only -- fopen/stat/access
                                                          // below keep using the raw dir_path

  BuildConfig cfg = {0};
  if (!read_manifest_if_present(arena, dir_path, &cfg, "3b build")) return 1;

  // `vm`, compiler.c's embedded native package, needs script_native_abi.h
  // `-include`d to compile against and liblib3b.a linked in. Both happen
  // automatically here, independent of `cfg`, so importing `vm` needs no
  // build.cfg.3b entries at all -- unlike a real third-party binding such as gl
  // or sdl, which still needs its own `include-first`/`pkg-config`/`libs`.
  b32 has_vm = false;
  foreach_index(i, pkg_count) {
    if (str8_match_lit("vm", pkg_names[i], 0)) { has_vm = true; break; }
  }
  String8 vm_abi_header_path = {0};
  if (has_vm) {
    vm_abi_header_path = str8f(arena, "%.*s/" OUTPUT_DIR "/script_native_abi.h", str8_varg(dir_path));
    FILE* header_out = fopen(cstr_from_str8_temp(vm_abi_header_path), "w");
    if (header_out) {
      write_embedded_lines(header_out, g_embed_native_pkgs_vm_script_native_abi_h);
      fclose(header_out);
    } else {
      vm_abi_header_path = (String8){0}; // vm's compile line below then gets no -include, and
                                            // fails loudly with an unknown-type error rather
                                            // than miscompiling silently
    }
  }

  String8 binary_name = cfg.binary_name.size > 0 ? cfg.binary_name : root_pkg_name;

  // Resolved once and embedded literally into every compile and link line
  // below, rather than assigning `CC=...` and referencing `$CC`: cmd.exe has
  // neither that assignment syntax nor `$VAR` expansion.
  String8 cc_var = str8_cstring(getenv("CC") ? getenv("CC") : "gcc");

  // CFLAGS: `-O2` without assertions for `--release`, `-O0 -DXDEBUG` (xassert
  // compiled in) otherwise, plus the project dir itself (so a manifest's
  // hand-written sources can `#include` their own local headers) and OUTPUT_DIR
  // (so generated packages can include each other and 3b_runtime.h by bare
  // name), plus every manifest pkg-config package's `--cflags`.
  String8 opt_flags = release ? str8_lit("-O2") : str8_lit("-O0 -DXDEBUG");
  String8 cflags     = str8f(arena, "-g -Wall -Wno-missing-braces -pthread %.*s -I%.*s -I%.*s/" OUTPUT_DIR,
                                 str8_varg(opt_flags), str8_varg(dir_path_q), str8_varg(dir_path_q));

  // Extra flags for the generated program's OWN toolchain, spliced verbatim
  // into every compile line and the link line. Sanitizing generated code is
  // what this exists for -- `BBB_EXTRA_CFLAGS="-fsanitize=thread" 3b build
  // examples/lanes` puts TSAN on the lane fork-join runtime -- and both halves
  // are required: a sanitizer passed only to the compile lines links with
  // undefined `__asan_*`.
  //
  // Not spelled `CFLAGS`, which the surrounding `make` exports with flags for
  // building the COMPILER, and not folded into `CC`: cc_var is shell-quoted as
  // one word, so a multi-word `CC` becomes a command named "gcc -fsanitize=...".
  // `BBB_`, not the `3B_` of compiler.c's own debug knobs, because a name
  // starting with a digit is not a valid shell assignment: `3B_X=1 cmd` is a
  // syntax error, and this one is meant to be set on a command line.
  String8 extra_cflags = str8_cstring(getenv("BBB_EXTRA_CFLAGS") ? getenv("BBB_EXTRA_CFLAGS") : "");
  if (extra_cflags.size > 0) {
    cflags = str8f(arena, "%.*s %.*s", str8_varg(cflags), str8_varg(extra_cflags));
  }
  if (dyn_count(cfg.pkg_config_pkgs) > 0) {
    String8 pc_cflags = {0};
    if (!pkg_config_query(arena, "--cflags", cfg.pkg_config_pkgs, &pc_cflags)) return 1;
    cflags = str8f(arena, "%.*s %.*s", str8_varg(cflags), str8_varg(pc_cflags));
  }

  String8 script     = str8_lit("");
  String8 obj_list   = str8_lit("");
  b32     first_line = true;

  // Generated package sources only, never the hand-written C a manifest points
  // at (`c-sources`) or the runtime: codegen.c emits no prototype for a
  // `(private (extern (fn ...)))`, so a binding's only real declaration of the C
  // function it wraps is whatever `include-first` puts in scope. Miss that
  // header -- or list one that doesn't cover every function actually called --
  // and C quietly falls back to a K&R implicit declaration, which is not an
  // error and not, in general, harmless: arguments take the no-prototype default
  // promotions and the return type is assumed `int`. That has already cost real
  // debugging time twice, once for a `float` param arriving as garbage (see
  // examples/game3d/build.cfg.3b) and once for a pointer return truncated to 32
  // bits, both of which present as a wrong VALUE far from the actual cause.
  //
  // Promoting it to an error is what makes `include-first` self-enforcing:
  // there is no longer a way to forget it and get a build that merely
  // misbehaves. The message names the exact function whose header is missing,
  // which is the thing you need to know.
  String8 pkg_cflags = str8_cat(arena, cflags, str8_lit(" -Werror=implicit-function-declaration"));

  // One compile line per generated package .c (root + every import).
  foreach_index(i, pkg_count) {
    String8 mangled = c_mangle_name(arena, pkg_names[i]);
    String8 src     = str8f(arena, "%.*s/" OUTPUT_DIR "/%.*s.c", str8_varg(dir_path), str8_varg(mangled));
    String8 obj     = str8f(arena, "%.*s/" OUTPUT_DIR "/%.*s.o", str8_varg(dir_path), str8_varg(mangled));
    String8* headers = include_first_headers_for(&cfg, pkg_names[i]);
    // `vm`'s compile line always needs script_native_abi.h -include'd, since
    // codegen.c emits no prototypes for `extern fn` -- appended to whatever the
    // manifest asked for as well.
    if (vm_abi_header_path.size > 0 && str8_match_lit("vm", pkg_names[i], 0)) {
      dyn_push(arena, headers, vm_abi_header_path);
    }
    // A translated binding whose C library passes structs BY VALUE ships a
    // generated `<pkg>_byval.h` beside its `<pkg>.3b`, holding the shims those
    // functions are routed through (see translate/emit.c's "By-value struct
    // bridge"). It is found by name rather than declared in the manifest: it is
    // not a header the project chose to depend on, it is part of the binding,
    // regenerated with it and meaningless without it.
    if (pkg_dirs && pkg_dirs[i].size > 0) {
      String8 byval = str8f(arena, "%.*s/%.*s_byval.h", str8_varg(pkg_dirs[i]), str8_varg(pkg_names[i]));
      if (access((char*)cstr_from_str8_temp(byval), F_OK) == 0) dyn_push(arena, headers, byval);
    }
    String8 line = append_compile_line(arena, str8_lit(""), cc_var, pkg_cflags, headers, src, obj);
    script           = append_script_line(arena, script, line, &first_line);
    obj_list         = str8f(arena, "%.*s %.*s", str8_varg(obj_list), str8_varg(shell_quote(arena, obj)));
  }

  // The self-contained runtime prelude every generated package pulls in (see
  // cg_write_runtime_header/source in codegen.c).
  {
    String8 src  = str8f(arena, "%.*s/" OUTPUT_DIR "/3b_runtime.c", str8_varg(dir_path));
    String8 obj  = str8f(arena, "%.*s/" OUTPUT_DIR "/3b_runtime.o", str8_varg(dir_path));
    String8 line = append_compile_line(arena, str8_lit(""), cc_var, cflags, NULL, src, obj);
    script        = append_script_line(arena, script, line, &first_line);
    obj_list      = str8f(arena, "%.*s %.*s", str8_varg(obj_list), str8_varg(shell_quote(arena, obj)));
  }

  // Extra hand-written C sources from the manifest (GLAD/stb impl files and the
  // like), relative to the project dir, objects landing flat in OUTPUT_DIR
  // alongside everything else (see str8_basename).
  foreach_index(i, dyn_count(cfg.c_sources)) {
    String8 base_no_ext = str8_basename(cfg.c_sources[i]);
    if (str8_ends_with(base_no_ext, str8_lit(".c"), 0)) base_no_ext = str8_chop(base_no_ext, 2);
    String8 src  = str8f(arena, "%.*s/%.*s", str8_varg(dir_path), str8_varg(cfg.c_sources[i]));
    String8 obj  = str8f(arena, "%.*s/" OUTPUT_DIR "/%.*s.o", str8_varg(dir_path), str8_varg(base_no_ext));
    String8 line = append_compile_line(arena, str8_lit(""), cc_var, cflags, NULL, src, obj);
    script        = append_script_line(arena, script, line, &first_line);
    obj_list      = str8f(arena, "%.*s %.*s", str8_varg(obj_list), str8_varg(shell_quote(arena, obj)));
  }

  // A `(kind library)` package has no `main` to link into an executable; it is
  // consumed through another package's `(import ...)`, which works straight
  // from source. Every .c is still built to .o above, but the link is skipped.
  // `run_after` is never true here -- main.c's `3b run` fast-fails on a library
  // before this function is called.
  if (cfg.kind == PackageKind_Library) {
    return run_shell_script(script);
  }

  // Link: `pkg-config --libs` for the manifest's packages, then `-l<name>` for every
  // bare manifest lib, then `-lm -pthread`. `-lm` covers the math functions
  // generated code reaches for through `extern`; `-pthread` covers the runtime
  // prelude's lane and OS-threading layer (cg_write_runtime_source in
  // codegen.c), which every build needs now, not just ones using `parallel`.
  // Anything else is the manifest's `(libs [...])` to ask for.
  String8 link_line = str8f(arena, "%.*s %.*s", str8_varg(shell_quote(arena, cc_var)), str8_varg(obj_list));
  if (extra_cflags.size > 0) { // see its own comment above -- a sanitizer must reach the link too
    link_line = str8f(arena, "%.*s %.*s", str8_varg(link_line), str8_varg(extra_cflags));
  }
  if (dyn_count(cfg.pkg_config_pkgs) > 0) {
    String8 pc_libs = {0};
    if (!pkg_config_query(arena, "--libs", cfg.pkg_config_pkgs, &pc_libs)) return 1;
    link_line = str8f(arena, "%.*s %.*s", str8_varg(link_line), str8_varg(pc_libs));
  }
  foreach_index(i, dyn_count(cfg.libs)) {
    link_line = str8f(arena, "%.*s -l%.*s", str8_varg(link_line), str8_varg(shell_quote(arena, cfg.libs[i])));
  }
  // `static-libs` entries are literal archive paths, not `-lname` lookups. An
  // entry starting with `/` is used as-is, anything else is joined under the
  // project dir, the same convention `c-sources` uses.
  foreach_index(i, dyn_count(cfg.static_libs)) {
    String8 lib_path = str8_starts_with(cfg.static_libs[i], str8_lit("/"), 0)
                      ? cfg.static_libs[i]
                      : str8f(arena, "%.*s/%.*s", str8_varg(dir_path), str8_varg(cfg.static_libs[i]));
    link_line = str8f(arena, "%.*s %.*s", str8_varg(link_line), str8_varg(shell_quote(arena, lib_path)));
  }
  // `vm`'s real definitions (script_native.c) live in liblib3b.a, found next to
  // whichever `3b` binary is running (self_liblib3b_path) rather than at a
  // project-relative manifest path, so importing `vm` needs no `(static-libs
  // [...])` entry. Failing to find it is a loud error rather than a silent
  // skip; the alternative is a far more confusing "ld: undefined reference to
  // native_script_load" further down.
  if (has_vm) {
    String8 liblib3b = self_liblib3b_path(arena);
    if (liblib3b.size == 0) {
      fprintf(stderr, "3b build: '%.*s' imports `vm` but liblib3b.a couldn't be found next to this "
                       "3b binary -- build it with `make lib` in the 3b compiler's own source tree\n",
                       str8_varg(dir_path));
      return 1;
    }
    link_line = str8f(arena, "%.*s %.*s", str8_varg(link_line), str8_varg(shell_quote(arena, liblib3b)));
  }
  link_line = str8f(arena, "%.*s -lm -pthread -o %.*s/%.*s",
                        str8_varg(link_line), str8_varg(dir_path_q), str8_varg(shell_quote(arena, binary_name)));
  script = append_script_line(arena, script, link_line, &first_line);

  int status = run_shell_script(script);
  if (status != 0) return status;

  if (run_after) {
    String8 run_cmd = str8f(arena, "%.*s/%.*s", str8_varg(dir_path_q), str8_varg(shell_quote(arena, binary_name)));
    return run_shell_script(run_cmd);
  }
  return 0;
}

// `3b clean <dir>` -- removes everything `3b build`, `3b run` and plain
// `3b <dir>` can leave behind: OUTPUT_DIR, holding every generated .c/.h and
// .o, and the linked binary at the project root, named by the manifest's
// `(binary ...)` if it has one, else the project directory's basename (see
// dir_default_name). The package need not compile: the manifest read is
// best-effort, so a broken build.cfg.3b still lets `output/` be removed, and a
// missing `output/` or binary is nothing to do rather than an error.
//
// The package directory itself missing is the one exception: an already-clean
// project and a mistyped path both have nothing to remove, but only one of
// them is what the user meant, and reporting "nothing to clean" for both hides
// the typo behind a success.
int
build_clean(Arena* arena, const char* dir_path_cstr) {
  String8 dir_path = str8_cstring((char*)dir_path_cstr);

  struct stat dir_st;
  if (stat(dir_path_cstr, &dir_st) != 0 || !S_ISDIR(dir_st.st_mode)) {
    fprintf(stderr, "3b clean: '%s' is not a directory\n", dir_path_cstr);
    return 1;
  }

  BuildConfig cfg = {0};
  read_manifest_if_present(arena, dir_path, &cfg, "3b clean"); // best-effort -- see comment above
  String8 binary_name = cfg.binary_name.size > 0 ? cfg.binary_name : dir_default_name(arena, dir_path);

  String8 output_dir  = str8f(arena, "%.*s/" OUTPUT_DIR, str8_varg(dir_path));
  String8 binary_path = str8f(arena, "%.*s/%.*s", str8_varg(dir_path), str8_varg(binary_name));

  b32 removed_anything = false;

  struct stat st;
  if (stat((char*)cstr_from_str8_temp(output_dir), &st) == 0 && S_ISDIR(st.st_mode)) {
    String8 cmd = str8f(arena, "rm -rf %.*s", str8_varg(shell_quote(arena, output_dir)));
    if (system(cstr_from_str8_temp(cmd)) == 0) {
      printf("removed %.*s\n", str8_varg(output_dir));
      removed_anything = true;
    } else {
      fprintf(stderr, "3b clean: failed to remove %.*s\n", str8_varg(output_dir));
    }
  }

  if (stat((char*)cstr_from_str8_temp(binary_path), &st) == 0 && S_ISREG(st.st_mode)) {
    if (unlink((char*)cstr_from_str8_temp(binary_path)) == 0) {
      printf("removed %.*s\n", str8_varg(binary_path));
      removed_anything = true;
    } else {
      fprintf(stderr, "3b clean: failed to remove %.*s: %s\n", str8_varg(binary_path), strerror(errno));
    }
  }

  if (!removed_anything) printf("3b clean: nothing to clean in %.*s\n", str8_varg(dir_path));
  return 0;
}
