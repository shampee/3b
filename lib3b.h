#ifndef LIB3B_H
#define LIB3B_H
#include "3b.h"

// lib3b -- the compiler exposed as a callable library instead of a CLI, for a
// host that wants diagnostics as data (an LSP, first and foremost). Narrow by
// design: no codegen, no output/ writes, no toolchain invocation. The core is
// check-only -- parse, lower and type-check the same recursive-import graph
// compile_all_packages builds -- with hover, goto-definition and completion
// queries layered on below. See compiler.h and build.h for everything
// CLI-facing, which this does not replace.
//
// Unlike every other struct in this codebase, the `Diagnostic`s returned here
// are heap-allocated with plain malloc rather than arena-allocated: this is the
// library's external ABI boundary, and a caller shouldn't have to know this
// codebase's arena and Context conventions just to read a result.

// Top-level (same-package) symbols -- see lib3b_check_package_with_overlays below on why
// these ride along with the check result instead of having their own query.
typedef enum Lib3bSymbolKind {
  Lib3bSymbol_Function,
  Lib3bSymbol_Struct,
  Lib3bSymbol_Enum,
  Lib3bSymbol_Const,
  Lib3bSymbol_Var,
  Lib3bSymbol_Alias,
} Lib3bSymbolKind;

typedef struct Lib3bSymbol {
  char*           name; // malloc'd
  Lib3bSymbolKind kind;
  char*           file_path; // malloc'd -- a jump target for workspace/symbol-style queries;
                              // completion ignores file_path/line/col entirely
  u32             line, col;  // 1-indexed, matching Diagnostic's convention
  char*           detail; // malloc'd, NULL when there is none -- the declaration header, the same
                           // text hover shows (see Lib3bHoverResult.type_text). Only fn/struct/enum
                           // have one; a const/var/alias is just its name
  char*           doc;    // malloc'd, NULL when there is none -- the comment block above the
                           // declaration, same extraction hover uses
} Lib3bSymbol;

// One direct import's public surface -- only what `(import name)` makes
// nameable as `name/member`. Never recurses: per compiler.h, a transitively
// imported package isn't reachable by name without its own direct import.
typedef struct Lib3bImportedPackage {
  char*        name;               // malloc'd -- the import's own package name
  Lib3bSymbol* public_symbols;     // malloc'd -- private/is_imported members excluded
  u64          public_symbol_count;
} Lib3bImportedPackage;

typedef struct Lib3bCheckResult {
  b32                    ok;               // false if any package in the import graph failed
                                            // to parse/lower/type-check
  Diagnostic*            diagnostics;      // malloc'd array -- see lib3b_free_result
  u64                    diagnostic_count;
  Lib3bSymbol*           symbols;          // malloc'd array, populated only when ok is true -- every
                                            // top-level fn/struct/enum/const/var/alias in THIS
                                            // package, private ones included: unlike cross-package
                                            // splicing, privacy doesn't apply to completion within
                                            // your own package. See lib3b_free_result.
  u64                    symbol_count;
  Lib3bImportedPackage*  imports;          // malloc'd array, populated only when ok is true -- one
                                            // entry per package THIS package directly imports, each
                                            // with its own public top-level surface
  u64                    import_count;
} Lib3bCheckResult;

// Parses, lowers and type-checks the package at dir_path_cstr and everything it
// transitively imports, collecting diagnostics instead of printing them to
// stderr, and without writing output/ or invoking any toolchain. Also collects
// the checked package's own top-level symbols (Lib3bCheckResult.symbols) on
// success, riding along with the same compile rather than a separate query
// entry point, since a caller that wants both -- an LSP's completion cache, see
// lsp/lsp_main.c -- would otherwise compile the package twice.
// `overlays`/`overlay_count` (SourceOverlay, 3b.h) substitute in-memory buffer
// content for specific files instead of reading them from disk, the mechanism a
// live-as-you-type LSP needs to check unsaved edits; pass NULL, 0 to check
// what's on disk, exactly what lib3b_check_package below does. Fully
// self-contained -- it owns its own Context, like every other top-level
// entrypoint -- so it is safe to call repeatedly from a long-lived process,
// once per document edit say, with no setup from the caller.
//
// NOT safe to call from more than one thread at a time: the diagnostic-capture
// state it relies on (diag.c's g_source_files/g_captured/g_capturing) is
// process-wide global, not per-thread -- unlike ctx_init/ctx_free's tls_ctx,
// which genuinely is per-thread and would otherwise make this look thread-safe.
// Serialize calls through a single worker or queue, as most LSP servers already
// do for compiler work.
Lib3bCheckResult lib3b_check_package_with_overlays(const char* dir_path_cstr,
                                         const SourceOverlay* overlays, u64 overlay_count);

// lib3b_check_package_with_overlays(dir_path_cstr, NULL, 0) -- checks whatever's on disk.
Lib3bCheckResult lib3b_check_package(const char* dir_path_cstr);

// Frees everything lib3b_check_package/lib3b_check_package_with_overlays
// allocated for `result` (its
// diagnostics array and each diagnostic's message/file_path buffers).
// Safe to call on a zeroed/already-freed result.
void lib3b_free_result(Lib3bCheckResult* result);

////////////////////////////////
//~ Queries: hover (what is the type of the thing at this position?) and
// goto-definition (where was the thing at this position declared?).
//
// Both are one-shot, same lifecycle as lib3b_check_package_with_overlays: compile the
// package fully (verbose=false, no output/ writes), answer the query against
// the resulting tree, tear down. Hover and goto-definition are comparatively
// rare, interactively triggered events doing exactly the work a single
// didChange already does on every keystroke, so a fresh compile per query is
// simple and no slower than what already happens elsewhere. A persistent
// session would have needed a new Context lifecycle design -- ctx_free has no
// save/restore stack, see diag.c -- for no real benefit at this scale.
//
// `line`/`col` are 1-indexed, matching Diagnostic's convention; an LSP caller
// translates from its own 0-indexed Position at the call site (lsp/lsp_main.c).
//
// The two queries resolve a position the same way, and hover reports where it
// landed (Lib3bHoverResult.decl_file_path/decl_line/decl_col) rather than only
// what type it found -- so a hover popup can say where a name comes from
// without the caller running goto-definition as a second query.
//
// Goto-definition resolves a function call's callee, a struct construction, an
// enum-variant access, and a bare identifier read (local variable, parameter,
// or top-level const/var). The bare-identifier case is not a query-time guess:
// checker.c's TypedNodeKind_Identifier case resolves a bare identifier through
// the lexically scoped, shadowable local Scope first, falling back to a
// top-level table only if that fails, and records which ScopeEntry it matched
// (TypedNode.ident.decl_token, 3b.h) -- so this reads back a fact the checker
// already established, with no risk of re-deriving a shadowed reference wrong.
// Type annotations resolve too, but through TypedAst.type_annotations rather
// than the node tree (find_type_annotation_at_position, lib3b.c), since a
// TypeRef is not a TypedNode. Still unresolved: a bare identifier naming a
// top-level `fn` referenced as a value rather than called -- the
// `fn_table_lookup` fallback path leaves decl_token zeroed.

// What the hovered reference turned out to name, so a caller can label the
// popup ("function `draw-model`") the way clangd does. Lib3bHover_Expression is
// the fallback for a reference with no declaration behind it -- an arithmetic
// subexpression, a literal -- where a resolved type is all there is to say.
typedef enum Lib3bHoverKind {
  Lib3bHover_Expression,
  Lib3bHover_Function,
  Lib3bHover_Struct,
  Lib3bHover_Enum,
  Lib3bHover_Builtin,
  Lib3bHover_Binding, // a local, a parameter, or a top-level val/var
  Lib3bHover_Field,   // a struct or union field, named through `.`, `&`, `get`/`get-in` or a
                       // destructuring pattern. `name` is qualified: "OutStream.frame"
} Lib3bHoverKind;

typedef struct Lib3bHoverResult {
  b32   found;
  char* type_text; // malloc'd, NULL if !found -- the plain resolved type ("i32") for most
                    // references, but the full declaration header for a function call's callee
                    // ("fn name [params] rettype"), a struct construction ("struct name
                    // [fields]"/"union ...") or an enum access ("enum name [variants]"/"flags
                    // ...") -- source syntax, no body, the shape you'd write to declare it.
                    // A call to a builtin has no declaration at all and shows its accepted
                    // call shapes instead (lib3b_builtin_shapes), one per line
  Lib3bHoverKind kind;
  char* name;      // malloc'd, NULL when the hovered thing isn't a named reference
  char* doc;       // malloc'd, NULL when there is none -- the comment block written directly
                    // above the declaration, markers stripped, newlines kept (see
                    // doc_comment_above in lib3b.c for exactly which comments qualify)
  char* decl_file_path; // malloc'd, NULL when the reference resolved to no declaration --
                          // notably every builtin, which has none anywhere
  u32   decl_line, decl_col; // 1-indexed, meaningful only alongside decl_file_path
} Lib3bHoverResult;

typedef struct Lib3bLocation {
  b32   found;
  char* file_path; // malloc'd, NULL if !found
  u32   line, col;  // 1-indexed
} Lib3bLocation;

Lib3bHoverResult lib3b_hover(const char* dir_path_cstr, const char* file_path_cstr, u32 line, u32 col,
                              const SourceOverlay* overlays, u64 overlay_count);
Lib3bLocation    lib3b_definition(const char* dir_path_cstr, const char* file_path_cstr, u32 line, u32 col,
                              const SourceOverlay* overlays, u64 overlay_count);

void lib3b_free_hover(Lib3bHoverResult* result);
void lib3b_free_location(Lib3bLocation* result);

// The accepted call shapes of a builtin, one per line, or NULL for a name that
// isn't one of the few builtins carrying a display (see BUILTIN_SHAPES,
// lib3b.c). Static storage, never freed. lib3b_hover uses it directly; a
// completion caller wanting one line's worth should take the first.
const char* lib3b_builtin_shapes(const char* name);

////////////////////////////////
//~ Completion context -- best-effort, fresh top-level symbols and in-scope
// locals for a buffer that is actively being edited, and therefore very often
// doesn't parse as-is: 3b is s-expression-delimited, so an in-progress edit
// commonly means an unclosed paren, which breaks parsing for the rest of the
// whole file, not just the expression being typed.
//
// This auto-closes whatever is still open at `cursor_offset` (a byte offset
// into `buffer_text` -- an LSP caller has line/character and converts once)
// using the real lexer, so string literals and line comments are skipped
// correctly where a naive character scan would be fooled by a literal '('
// inside a string. It then compiles the package TOLERATING type-check errors:
// without that, the checker correctly flagging the partial identifier being
// typed ("pri" in "(helper2 pri") as undefined would discard everything else in
// the file too, through compile_all_packages' ordinary all-or-nothing gate,
// defeating the point on exactly the input this exists for.
//
// One-shot, same lifecycle and self-containment as
// lib3b_hover/lib3b_definition. Returns `ok=false`, not just an empty symbol
// list, if even the patched buffer fails to PARSE -- a real, unrelated syntax
// error elsewhere, rather than the expected "undefined identifier" at the
// cursor. Callers should treat a successful result as an additive improvement
// over their own last-good cache, never its replacement (see lsp/lsp_main.c).
//
// It also asks the checker, via ScopeQuery (3b.h; see check_expr's hook in
// checker.c), to snapshot every name in lexical scope exactly at
// cursor_offset -- function params, `let`/`val`/`var` locals, `for` and
// `parallel-for` loop variables, with shadowing already resolved so an inner
// rebinding wins, matching scope_lookup_entry's own precedence -- into `locals`
// below. Top-level const/vars declared earlier in the same file are technically
// in scope by that same mechanism, since function bodies are checked against a
// scope that already contains them, but are filtered back out here because
// `symbols` (collect_toplevel_symbols) already covers them: `locals` holds only
// the names `symbols` could never have offered.
typedef struct Lib3bCompletionContext {
  b32          ok;
  Lib3bSymbol* symbols; // malloc'd, NULL if !ok
  u64          symbol_count;
  Lib3bSymbol* locals; // malloc'd, NULL if none -- params/let-locals/loop-vars in scope at the
                         // cursor; always kind == Lib3bSymbol_Var (nothing downstream
                         // distinguishes a parameter from a local). file_path/line/col point at
                         // the enclosing binding form -- the whole `fn`/`let`/`for`, not a
                         // per-name position (see ScopeEntry.decl_token, 3b.h) -- the same
                         // imprecision goto-definition already accepts for these.
  u64          local_count;
} Lib3bCompletionContext;

Lib3bCompletionContext lib3b_completion_context(
    const char* dir_path_cstr, const char* file_path_cstr,
    const char* buffer_text, u64 cursor_offset,
    const SourceOverlay* overlays, u64 overlay_count);

void lib3b_free_completion_context(Lib3bCompletionContext* result);

////////////////////////////////
//~ Formatting -- the same rendering `3b format` prints (format.c), against an
// in-memory buffer instead of a path, so an LSP can answer
// textDocument/formatting without round-tripping through the filesystem.
//
// Parser-only, like the CLI: no lowering, no checker, no imports resolved, so
// a file that doesn't type-check still formats. That also makes this the one
// query here that needs neither a package directory nor overlays.
//
// `file_path_cstr` is used only to label the buffer in the source registry,
// which is to say only in a parse diagnostic; it need not exist on disk.
// Returns a malloc'd NUL-terminated string the caller frees, or NULL if the
// buffer failed to PARSE -- the case an editor must treat as "leave the
// document alone", since a formatter cannot render a tree it never built.
//
// Uses format.c's default hang (`3b format --hang`'s knob is not exposed:
// LSP's FormattingOptions carries tabSize and insertSpaces, neither of which
// maps onto it).
char* lib3b_format(const char* file_path_cstr, const char* buffer_text);

#endif
