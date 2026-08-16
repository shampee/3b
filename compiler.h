#ifndef COMPILER_H
#define COMPILER_H
#include "3b.h"

////////////////////////////////
//~ Package compiler
//
// One directory is one package. Every `.3b` file directly inside it must
// open with `(package <name>)`, where <name> matches the directory's
// basename. All of a package's files are parsed into one shared Ast, so
// NodeIndex values stay valid across them, and their forms are concatenated
// into a single top-level list before lowering.
//
// `(import gl)` resolves `gl/` relative to the importing package's own
// directory. Imports are package-wide: every file sees every other file's
// imports. Named collections (`-collection:name=path`) are not implemented.
//
// Qualified references (`gl/gen-textures`, `gl/LightType/Spot`) need no
// special handling in lower.c or checker.c:
//   - Callee and identifier text lowers verbatim, slash included.
//   - Importing seeds this package's struct/enum/alias name tables with the
//     import's public names spelled "pkg/Name" (seed_lowerer_with_import),
//     so the existing lookups just succeed.
//   - Each imported public decl is spliced into the importing package's
//     TypedAst as a synthetic top-level node tagged `is_imported`
//     (splice_public_decl); check_program's gathering pass picks it up.
//   - codegen.c's cg_symbol_name prefixes definitions with `PackageName_`,
//     matching what c_mangle_name's '/' -> '_' produces for references.
//
// Not implemented: multi-segment import paths; transitive re-export (a
// package can pass around a type it received through an indirect import,
// but cannot construct a literal or name an enum variant of it without a
// direct import); privacy diagnostics (a private symbol is never spliced
// anywhere, so it is invisible to importers by construction).

// Generated files -- 3b_runtime.h/.c plus <name>.h/.c per package -- land in
// `<root package dir>/OUTPUT_DIR`, never relative to the process's working
// directory: build.c resolves every path it builds against the package
// directory it was handed, so writing them anywhere else left it compiling and
// linking a stale output/ next to the package instead, silently and with a
// successful exit status. Shared with build.c, which has to find exactly what
// codegen wrote.
#define OUTPUT_DIR "output"

// Individually arena-allocated: a compile run's registry is a dyn array of
// PackageBuild*, never of the structs themselves, since dyn_push may
// reallocate the array while compile_package is still holding a pointer
// across its recursive calls into imports.
typedef struct PackageBuild {
  String8    canonical_dir; // realpath -- the memoization and cycle-detection key
  String8    dir_path;      // as given, for diagnostics
  String8    pkg_name;
  TypedAst*  tast;
  TypedIndex root;               // imported placeholders ++ this package's own forms
  TypeRef*   resolved_types;     // one entry per tast->nodes element, indexed by TypedIndex;
                                 // copied out of check_program's otherwise local Checker so
                                 // later queries can use it. NULL unless the check succeeded.
  String8*   imported_pkg_names; // dyn array -- this package's direct imports, by package name
  b32        in_progress;        // cycle sentinel: true while this package's own
                                 // compile_package call is still on the stack
  b32        is_root;            // true only for the package named on the command line. Only the
                                 // root's `main` becomes the real C entry point, and only when
                                 // `kind` is Binary -- see cg_symbol_name, check_main_signature.
  PackageKind kind;              // meaningful only when is_root; Library leaves a public
                                 // `fn main` an ordinary function
  HashTable  public_toplevel_names; // memoized for is_own_public_toplevel_name, built lazily on
                                    // first lookup (a compiled dep's root never changes again)
  b32        public_toplevel_names_built;
  b32        had_check_error;    // set whenever check_program reported an error, whether or not
                                 // that error aborted the compile (see tolerate_check_errors)
  ScopeEntry* scope_query_result; // malloc'd, not arena. NULL unless the caller passed a
                                  // ScopeQuery and it matched a node in this package.
  u64         scope_query_count;
  Checker     ck; // the local Checker copied by value. Everything it owns lives in ctx_perm(),
                  // which outlives this PackageBuild, so the copy is safe. Needed because
                  // bc_compile_program/bc_compile_function want a Checker* for struct/enum/
                  // global lookups, which resolved_types alone can't answer. Set on success only.
} PackageBuild;

// Compiles the package at `dir_path_cstr` and, recursively, every package it
// imports, into `<dir_path_cstr>/OUTPUT_DIR` -- one output directory per run,
// shared by the root and every package it pulls in, embedded ones included.
// Returns NULL on failure, having already printed a diagnostic unless
// `verbose` is false. Every PackageBuild is allocated on
// ctx_perm() and stays valid until the caller's ctx_free(); the caller owns
// the Context, as with every top-level compiler entrypoint.
//
//   out_registry           if non-NULL, receives every compiled PackageBuild*,
//                          root included. `3b build` needs the full package list
//                          to know which generated .c files to hand the C
//                          toolchain; plain `3b <dir>` passes NULL.
//   root_kind              the root package's declared kind, which the caller
//                          reads from build.cfg.3b beforehand -- it has to be
//                          known before the checker and codegen run.
//   verbose                false suppresses codegen entirely (no output/ files
//                          are written) along with all progress and failure
//                          printing that isn't already a diagnostic.
//   overlays               substitutes in-memory content for files instead of
//                          reading them from disk, matched by filename against
//                          every package's file listing, imports included.
//   tolerate_check_errors  keeps a checker error from aborting the compile, so
//                          tast/root/resolved_types survive a package that has a
//                          real type error somewhere.
//   scope_query            asks the checker to snapshot every name in lexical
//                          scope -- params, let-bindings, loop vars -- at one
//                          source position, into scope_query_result above.
//
// The last three exist for lib3b.h's LSP queries, which inspect a buffer that
// is mid-edit and therefore likely to have an error at the cursor itself. CLI
// callers pass true, NULL/0, false, NULL.
PackageBuild* compile_all_packages(const char* dir_path_cstr, PackageBuild*** out_registry,
                                   PackageKind root_kind, b32 verbose,
                                   const SourceOverlay* overlays, u64 overlay_count,
                                   b32 tolerate_check_errors, const ScopeQuery* scope_query);

// Non-static so script.c can reuse it for a `.3bs` file's own `(import name)`
// forms.
NodeIndex* strip_import_forms(Ast* ast, NodeIndex* forms, u16 form_count, u16* out_count, String8** import_names);

#endif
