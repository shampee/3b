#ifndef BUILD_H
#define BUILD_H
#include "3b.h"

// `3b build`/`3b run` -- turns a compiled package's OUTPUT_DIR (see compiler.h)
// into a linked binary without a hand-written Makefile. 3b already knows which
// packages exist and where their generated .c files are; the optional
// `build.cfg.3b` manifest, dropped next to the root package, supplies what it
// can't infer: extra hand-written C sources, `-include` shims some generated
// bindings need (GLAD, stb_image), pkg-config packages, extra link libraries,
// the binary's name. Most projects have no manifest and just link the generated
// .c files with libm.

// `(include-first PKG [header ...])` -- extra `-include header.h` flags one
// generated package's .c needs, matched by package name, e.g. `-include
// glad/gl.h` for gl.c's bare GL calls. Required for any package with a
// `(private (extern (fn ...)))`, since codegen.c emits no prototype for one;
// build_invoke_toolchain compiles generated packages with
// `-Werror=implicit-function-declaration` so a missing entry is a build error
// rather than a silently miscompiled call.
typedef struct BuildIncludeFirst {
  String8  package_name;
  String8* headers; // dyn array
} BuildIncludeFirst;

typedef struct BuildConfig {
  String8             binary_name;     // (binary "name") -- defaults to the root package's name
  String8*            c_sources;       // (c-sources [...]) -- extra hand-written .c, relative to the project dir
  String8*            pkg_config_pkgs; // (pkg-config [...]) -- package names passed to `pkg-config --cflags/--libs`
  String8*            libs;            // (libs [...]) -- bare names; `-l` is added at invocation time
  String8*            static_libs;     // (static-libs [...]) -- literal paths to prebuilt .a archives,
                                          // project-relative or absolute, appended to the link line
                                          // verbatim (unlike `libs`, which only becomes `-lname` and
                                          // relies on the default linker search path)
  BuildIncludeFirst*   include_first;   // (include-first pkg [...] pkg [...] ...)
  PackageKind          kind;            // (kind library) / (kind binary) -- defaults to Binary (see PackageKind)
} BuildConfig;

// Parses `src`, the text of a `build.cfg.3b` file, into `out`. Reuses 3b's own
// lexer and parser for a generic atom/list/vector AST, the same approach as
// translate/config.c's Config DSL; never lowered or checked. Returns false,
// with diagnostics already printed, on any malformed form.
b32 build_config_read(Arena* arena, String8 src, BuildConfig* out, u32 file_id);

// Best-effort read of `<dir_path_cstr>/build.cfg.3b`'s `(kind ...)` form. A
// missing manifest, a missing form, and an unparseable manifest all silently
// default to PackageKind_Binary; a real parse error is reported later by the
// full build_config_read that `3b build`/`3b run` perform anyway, so this never
// prints a diagnostic of its own. main.c needs the kind before compiling, to
// gate whether `fn main` becomes the C entry point and to fast-fail `3b run` on
// a library.
PackageKind build_config_read_kind(Arena* arena, const char* dir_path_cstr);

// Called by main.c's `3b build`/`3b run` handlers once the package graph is
// already compiled to OUTPUT_DIR; compiler.c owns the compile step, and this
// module never sees a PackageBuild, only the resulting package names. Reads
// `<dir_path_cstr>/build.cfg.3b` if present, shells out to the C toolchain to
// compile and link every generated `output/<pkg>.c` plus any manifest-declared
// sources into a binary, and executes it when `run_after`. `pkg_names` lists
// every compiled package, in any order, and `pkg_dirs` each one's source
// directory (parallel to `pkg_names`, used only to find a translated binding's
// generated `<pkg>_byval.h`); `root_pkg_name` is the default binary name absent
// a manifest `(binary ...)`. `release` selects `-O2` with assertions compiled
// out over the default `-O0 -DXDEBUG`; `-g` is kept in both. Returns a process
// exit code (0 on success).
int build_invoke_toolchain(Arena* arena, const char* dir_path_cstr, String8 root_pkg_name,
                            String8* pkg_names, String8* pkg_dirs, u64 pkg_count,
                            b32 run_after, b32 release);

// `3b clean <dir>` -- removes OUTPUT_DIR and the linked binary, without
// requiring the package to compile.
int build_clean(Arena* arena, const char* dir_path_cstr);

#endif
