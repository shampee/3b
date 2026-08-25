// script.c -- the `.3bs` driver: parse -> lower -> check -> bc_compile_program
// -> bc_run_in_program for a standalone script file (see script.h).
//
// MODULE RESOLUTION: real cross-package bytecode imports. Each module
// (g_known_modules) gets its own independent
// parse/lower/check/compile pass with its own Ast/TypedAst/Checker/Scope/
// BcProgram -- genuinely separate compilation units, memoized process-wide (see
// g_module_registry) so a module imported from several places, or by many
// successive hot-reload compiles of the same script, compiles once per run. Not
// a textual or AST splice of the module's whole source into the importer.
//
// This mirrors compiler.c's package-import mechanism as far as the backend
// allows, reusing strip_import_forms verbatim (non-static for exactly that)
// and re-deriving its splice_public_decl/prepend-to-root-Block technique,
// which is static to compiler.c.
//
// ONE NECESSARY DIVERGENCE: compiler.c's splice DISCARDS a spliced function's
// body (`copy.func.body = TYPED_NIL`), because the real definition lives in
// that package's separately-emitted `.c` file and the C linker stitches them
// together. `.3bs` has no linker. So the SIGNATURE is still spliced as a
// bodyless extern, purely so the importing script type-checks, but the real
// body is compiled into the module's OWN BcProgram and a call crosses that
// boundary at runtime via BcOp_CallModule -- see BcModuleTable in bytecode.h.
//
// Call sites are QUALIFIED ("build/getenv"), matching native 3b's
// package-import syntax, and need no parser or lower.c work: lower_call and
// lower_atom's Identifier fallback already store callee text verbatim, slash
// and all.
//
// ENTRY POINT: a script's zero-parameter `main`, the same convention a native
// 3b program has -- no separate script-entry-point concept.
#include "script.h"
#include "compiler.h"
#include "bcgen.h"
#include "bcio.h" // bc_content_hash -- see script_load/script_poll_reload's own staleness check
#include "bcosprims.h" // BcOsPrimitiveDecl/bc_os_primitive_decls -- see resolve_one_import's `os` case
#include "file.h"
#include "script_embed.h" // g_embed_* per-line source arrays -- see tools/embed_runtime.c
#include <stdio.h>
#include <string.h>
#include <sys/stat.h> // stat -- see load_or_compile_script's own cache-existence pre-check

typedef struct KnownModule {
  const char*        name;
  const char* const* lines; // NULL-terminated, one array element per source LINE (embed_runtime's format)
} KnownModule;

// Add an entry here, and to SCRIPT_MODULE_FILES in the Makefile, for every new
// built-in-importable module.
//
// This table governs SPLICING only, never host-import registration, so it
// lists `config` unconditionally even though that module only works when the
// caller separately registered bc_register_config_primitives against a real
// `Config*`. A script importing it without that registration fails to compile
// with a real diagnostic ("no compiled function or registered host import by
// that name"), the same failure any missing registration produces.
static const KnownModule g_known_modules[] = {
  { "build",  g_embed_translate_build_3bs },
  { "config", g_embed_translate_config_3bs },
  { "sort",   g_embed_native_pkgs_sort_sort_3b },
  { "rng",    g_embed_native_pkgs_rng_rng_3b }, // same source native code also uses (see
                                                    // native_pkgs/rng/rng.3b) -- pure 3b arithmetic,
                                                    // no host imports needed, so it works unchanged
                                                    // on this backend too
};

// Reassembles an embed_runtime-style per-line array back into one in-memory
// String8 -- codegen.c's cg_fputs_lines does the same reassembly but
// straight to a FILE* (for writing a generated project's own runtime
// files); a `.3bs` module's source needs to reach the PARSER instead, so
// this builds a real buffer.
static String8
join_embedded_lines(Arena* arena, const char* const* lines) {
  u64 total = 0;
  for (const char* const* p = lines; *p; p += 1) total += strlen(*p);
  u8* buf = push_array(arena, u8, total);
  u64 off = 0;
  for (const char* const* p = lines; *p; p += 1) {
    u64 len = strlen(*p);
    MemoryCopy(buf + off, *p, len);
    off += len;
  }
  return str8(buf, total);
}

static b32
known_module_source(String8 name, Arena* arena, String8* out_src) {
  foreach_index(i, ArrayCount(g_known_modules)) {
    if (str8_match(str8_cstring((char*)g_known_modules[i].name), name, 0)) {
      *out_src = join_embedded_lines(arena, g_known_modules[i].lines);
      return true;
    }
  }
  return false;
}

// Parses `src` into `ast` (fresh, one per compile unit, so a module never
// shares the IMPORTER's Ast/node-index space -- see this file's own
// top-of-file note), validates
// and strips its leading `(package NAME)` form (LOOSELY -- no directory-
// name match the way compiler.c's validate_and_strip_package_form
// enforces, since a `.3bs` script isn't tied to a directory), strips any
// `(import name)` forms via strip_import_forms (appending found names
// into `*import_names`), and appends everything else into `*out_forms`.
static b32
script_parse_source(Ast* ast, String8 src, u32 file_id, const char* label,
                     NodeIndex** out_forms, String8** import_names) {
  Parser p;
  parser_init(&p, src, ast, file_id);
  NodeIndex root = parse_program(&p);
  if (p.had_error) {
    fprintf(stderr, "3b run: %s failed to parse (see errors above)\n", label);
    return false;
  }

  u16        fc;
  NodeIndex* fchildren = ast_seq_children(ast, root, &fc);
  if (fc < 1) {
    fprintf(stderr, "3b run: %s: expected `(package NAME)` as the first form, file is empty\n", label);
    return false;
  }
  AstNode*   first     = ast_get(ast, fchildren[0]);
  u16        pc        = 0;
  NodeIndex* pchildren = (first->kind == AstNodeKind_List) ? ast_seq_children(ast, fchildren[0], &pc) : NULL;
  AstNode*   phead     = (pc > 0) ? ast_get(ast, pchildren[0]) : NULL;
  b32        shape_ok  = first->kind == AstNodeKind_List && pc == 2
                       && phead && phead->kind == AstNodeKind_Atom
                       && str8_match_lit("package", phead->token.text, 0);
  if (!shape_ok) {
    diag_error(first->token, "expected `(package NAME)` as the first form");
    return false;
  }

  u16        filtered_count;
  NodeIndex* filtered = strip_import_forms(ast, fchildren + 1, (u16)(fc - 1), &filtered_count, import_names);
  foreach_index(i, filtered_count) dyn_push(ctx_perm(), *out_forms, filtered[i]);
  return true;
}

static String8
qualify_name(String8 module_name, String8 member_name) {
  return str8_cat(ctx_perm(), str8_cat(ctx_perm(), module_name, str8_lit("/")), member_name);
}

// Enough of one host import to tell whether a later table's entry at the same
// INDEX is the same function -- see find_compiled_module. Snapshotted rather
// than pointing back at the BcHostImportTable a module compiled against,
// because that table is routinely a caller's stack local (translate.c's
// load_config_script builds one) and would be long dead by the time a later
// compile wanted to compare against it. `name` is copied for the same reason.
typedef struct ModuleHostImportKey {
  String8          name;
  BcHostImportKind kind;
  BcHostFn         fn;        // kind == Trampoline; kept as two fields rather than the table's own
  void*            native_fn; // kind == Direct     -- union, since C has no portable way to compare a
                                  // function pointer and an object pointer through one slot
} ModuleHostImportKey;

// A fully, independently compiled `.3bs` module -- own Ast/TypedAst/
// Checker/BcProgram, genuinely separate from whatever imports it (or from
// any OTHER module), matching compiler.c's own PackageBuild in spirit
// (see this file's top-of-file note on the real differences). Arena-
// allocated on ctx_perm() by compile_module below, so it stays valid for
// the rest of this `3b run` invocation, same lifetime PackageBuild
// entries already get from compile_all_packages.
typedef struct CompiledModule {
  String8    name;
  TypedAst   tast;
  TypedIndex root;
  Checker    ck;
  BcProgram  prog;
  ModuleHostImportKey* host_key;       // the host imports this module compiled against -- part of
  u64                  host_key_count; // its identity, see find_compiled_module
} CompiledModule;

// Process-wide, deliberately. Every entry is a built-in module whose source is
// compiled into the `3b` binary itself (g_known_modules), so it cannot change
// while a process runs -- recompiling one could only ever produce the same
// program again.
//
// A registry local to each compile did exactly that: every hot reload
// (script_poll_reload -> load_or_compile_script -> compile_script_program)
// re-parsed, re-checked and re-emitted every module the script imports, onto
// ctx_perm(), which is never reclaimed. A long editing session with a
// module-importing script grew without bound, one full module compile per save.
// Sharing the registry makes the memoization this file already documents
// ("compiles once per run") true ACROSS compiles, not just within one.
//
// Consequence worth knowing: a module's `#init_globals` now runs once per
// PROCESS rather than once per script compile, so module-level state persists
// across a hot reload and is shared by every script importing it -- the same
// one-instance-per-program semantics the native backend gives a package.
static CompiledModule** g_module_registry;

// The layout cache for MODULE compiles, kept next to the registry it serves and
// shared for the same reason: layout_cache_init allocates a 64-slot table on
// ctx_perm(), which a per-compile cache made a hot-reload session repeat on
// every reload. Initialized lazily, since the common case is a script that
// imports nothing at all.
//
// Deliberately NOT the cache a script's own compile uses: layout_of_named keys
// purely on a struct's NAME, so one table shared across unrelated compile units
// would let a script's `Foo` resolve to a module's.
static LayoutCache g_module_layout_cache;
static b32         g_module_layout_cache_ready;

static LayoutCache*
module_layout_cache(void) {
  if (!g_module_layout_cache_ready) {
    layout_cache_init(&g_module_layout_cache, ctx_perm());
    g_module_layout_cache_ready = true;
  }
  return &g_module_layout_cache;
}

// Snapshots the identity of `table`'s current entries, for the compare below.
static ModuleHostImportKey*
snapshot_host_imports(BcHostImportTable* table, u64* out_count) {
  u64 count = table ? dyn_count(table->entries) : 0;
  *out_count = count;
  if (count == 0) return NULL;

  ModuleHostImportKey* key = push_array_zero(ctx_perm(), ModuleHostImportKey, count);
  foreach_index(i, count) {
    BcHostImport* imp = &table->entries[i];
    key[i].name = str8_copy(ctx_perm(), imp->name);
    key[i].kind = imp->kind;
    if (imp->kind == BcHostImportKind_Direct) key[i].native_fn = imp->native_fn;
    else                                       key[i].fn        = imp->fn;
  }
  return key;
}

// A module is identified by its name AND the host imports it compiled against,
// never the name alone: bcgen.c bakes a host import's INDEX into the
// BcOp_CallHost it emits, and the table a program is RUN with is whatever its
// caller passes, so reusing a module against a table that lays those indices
// out differently would call through to the wrong native function.
//
// Compared by CONTENT rather than by table pointer, in both directions
// deliberately. A table is often a stack local, so two unrelated ones can share
// an address and pointer identity would wrongly accept the reuse; equally, a
// host that rebuilds an identical table per run (translate.c's
// load_config_script, once per config it loads) gets a legitimate hit that
// pointer identity would miss.
//
// Only the entries that existed when the module compiled are checked: those are
// the only indices it can hold. Registering FURTHER imports afterwards is fine
// and is what the pre-existing within-one-compile memoization already assumed --
// a table only ever grows, so an index handed out earlier still names the same
// entry.
//
// `userdata` is deliberately NOT part of the key. It is read from the table a
// program is RUN with, not baked into the bytecode, so two config loads that
// register the same primitives against DIFFERENT Config* pointers still each see
// their own -- and including it here would only refuse a reuse that is correct.
static CompiledModule*
find_compiled_module(String8 name, BcHostImportTable* host_imports) {
  u64           avail   = host_imports ? dyn_count(host_imports->entries) : 0;
  foreach_index(i, dyn_count(g_module_registry)) {
    CompiledModule* mod = g_module_registry[i];
    if (!str8_match(mod->name, name, 0)) continue;
    if (mod->host_key_count > avail)     continue;

    b32 same = true;
    foreach_index(j, mod->host_key_count) {
      BcHostImport*        imp = &host_imports->entries[j];
      ModuleHostImportKey* key = &mod->host_key[j];
      b32 same_fn = (imp->kind == BcHostImportKind_Direct) ? (imp->native_fn == key->native_fn)
                                                            : (imp->fn == key->fn);
      if (imp->kind != key->kind || !same_fn || !str8_match(imp->name, key->name, 0)) {
        same = false;
        break;
      }
    }
    if (same) return mod;
  }
  return NULL;
}

// Splices a BODYLESS, qualified-named copy of `src_fn_idx` -- an
// already-checked FunctionDecl with a body in `mod_tast` -- into `dst_tast`,
// purely so the importing compile unit type-checks. Mirrors compiler.c's
// splice_public_decl for the FunctionDecl case only; structs, enums, consts and
// globals aren't spliceable here, because no `.3bs` module exports any.
//
// The real body stays in `mod_tast`, never copied and never referenced by
// TypedIndex from `dst_tast`, and is compiled into that module's own
// BcProgram. Only the signature has to exist in `dst_tast`.
//
// Does NOT requalify param/return TYPES the way compiler.c's
// requalify_type_for_import does. That machinery only matters when a spliced
// signature names the exporting module's own struct or enum by a bare name,
// and neither build nor config does: every signature here uses primitives
// (string/i32/bool/arena/void), whose TypeRefs hold no node index back into
// `mod_tast` and so copy verbatim safely. A module exporting a struct-typed
// signature would need this added.
static TypedIndex
splice_module_fn_extern(TypedAst* dst_tast, TypedAst* mod_tast, TypedIndex src_fn_idx, String8 qualified_name) {
  TypedNode copy = mod_tast->nodes[src_fn_idx];
  copy.is_private = false;
  copy.is_imported = true;
  copy.func.name   = qualified_name;
  copy.func.body   = TYPED_NIL; // signature only -- see this function's own comment
  u32 new_first = (u32)dyn_count(dst_tast->params);
  foreach_index(i, copy.func.param_count) {
    dyn_push(dst_tast->arena, dst_tast->params, mod_tast->params[copy.func.param_first + i]);
  }
  copy.func.param_first = new_first;
  return typed_push(dst_tast, copy);
}

// Walks `dep`'s own top-level PUBLIC function declarations exactly once,
// both splicing a bodyless, qualified signature into `dst_tast` (appended
// to `*prepend`, for `dst_tast`'s own type-checking) AND registering that
// SAME qualified name into `module_table` (resolving it to `dep`'s
// already-compiled BcProgram, for BcOp_CallModule at runtime) -- shared
// by compile_module's own dep-handling step and script_run_file's
// otherwise-identical one, so the two definitions of "public" (is_private/
// is_imported/body != TYPED_NIL) can't independently drift.
static void
splice_dep_into(TypedAst* dst_tast, CompiledModule* dep, TypedIndex** prepend, BcModuleTable* module_table) {
  TypedNode* dep_root = &dep->tast.nodes[dep->root];
  foreach_index(j, dep_root->block.stmt_count) {
    TypedIndex src_idx = dep->tast.extra[dep_root->block.stmt_first + j];
    if (src_idx == TYPED_NIL) continue;
    TypedNode* sn = &dep->tast.nodes[src_idx];
    if (sn->is_private || sn->is_imported) continue; // only dep's OWN public surface, never
                                                         // something IT itself imported (a
                                                         // transitive re-export -- same "known
                                                         // limitation" compiler.h documents for
                                                         // the native backend)
    if (sn->kind != TypedNodeKind_FunctionDecl || sn->func.body == TYPED_NIL) continue; // real
                                                         // `extern` -- not dep's own symbol to splice
    String8    qualified = qualify_name(dep->name, sn->func.name);
    TypedIndex spliced   = splice_module_fn_extern(dst_tast, &dep->tast, src_idx, qualified);
    dyn_push(ctx_perm(), *prepend, spliced);
    u32 fn_index = bc_program_find_fn(&dep->prog, sn->func.name);
    bc_module_table_add(module_table, ctx_perm(), qualified, &dep->prog, fn_index);
  }
}

// Forward-declared because the two are mutually recursive: compile_module's
// dep-handling step calls resolve_one_import for each of its own imports,
// which calls back here for anything that isn't a native pseudo-module.
static CompiledModule*
compile_module(String8 name, BcHostImportTable* host_imports, Arena* heap);

// Splices a bodyless, qualified-named extern FunctionDecl straight from
// bcosprims.h's fixed BcOsPrimitiveDecl table into `dst_tast` -- the
// no-source-to-compile analog of splice_module_fn_extern, for a NATIVE
// pseudo-module (currently just `os`) whose members are raw host imports
// rather than a BcProgram of their own.
//
// No BcModuleTable entry is needed: the call resolves through the same
// host-import-by-verbatim-qualified-name fallback bcgen.c already applies to
// any bodyless extern. This is no different from a script hand-writing
// `(extern (fn os/getenv ...))`, just generated rather than typed out.
static TypedIndex
splice_native_module_fn(TypedAst* dst_tast, BcOsPrimitiveDecl* decl) {
  u32 param_first = (u32)dyn_count(dst_tast->params);
  foreach_index(i, decl->param_count) {
    Param param = {0};
    param.name = str8f(ctx_scratch(), "_%u", (u32)i); // never surfaces at a call site -- any placeholder name is fine
    param.type = decl->param_types[i];
    dyn_push(dst_tast->arena, dst_tast->params, param);
  }

  TypedNode node = {0};
  node.kind             = TypedNodeKind_FunctionDecl;
  node.is_imported       = true;
  node.func.name         = decl->qualified_name;
  node.func.param_first  = param_first;
  node.func.param_count  = (u16)decl->param_count;
  node.func.return_type  = decl->return_type;
  node.func.body         = TYPED_NIL;
  return typed_push(dst_tast, node);
}

// The CONSTANT counterpart to splice_native_module_fn above -- splices one
// `os/mode-write`-style BcOsConstantDecl in as a real top-level ConstDecl
// with a synthesized integer-literal initializer. Unlike a function there
// is no host import behind this: it becomes an ordinary module-level
// global of the importing program (bcgen.c's own global gather picks it up
// from the top-level statement list like any other `val`), which is
// exactly what makes `os/mode-write` usable as a plain expression rather
// than something a script has to call.
static TypedIndex
splice_native_module_const(TypedAst* dst_tast, BcOsConstantDecl* decl) {
  TypedNode lit = {0};
  lit.kind                = TypedNodeKind_IntLiteral;
  lit.int_lit.value       = decl->value;
  lit.int_lit.explicit_type = decl->type.kind; // suffix-equivalent, so the checker doesn't have to infer it
  TypedIndex lit_idx = typed_push(dst_tast, lit);

  TypedNode node = {0};
  node.kind             = TypedNodeKind_ConstDecl;
  node.is_imported       = true;
  node.const_decl.name   = decl->qualified_name;
  node.const_decl.type   = decl->type;
  node.const_decl.init   = lit_idx;
  return typed_push(dst_tast, node);
}

// Resolves ONE `(import name)` -- either a NATIVE pseudo-module (currently
// just `os`: no source, spliced directly from bcosprims.h's fixed table,
// can't fail) or a real compiled `.3bs` module (`build`/`config`:
// recursively compiled via compile_module, then spliced via
// splice_dep_into) -- shared by compile_module's own dep-handling step
// and compile_script_program's otherwise-identical one, so the two can't
// independently drift on how an import name gets resolved. Returns false
// (with a diagnostic already printed) only for a real module's compile
// failure.
static b32
resolve_one_import(String8 import_name, String8 importer_label, TypedAst* dst_tast,
                    BcHostImportTable* host_imports, Arena* heap,
                    TypedIndex** prepend, BcModuleTable* module_table) {
  if (str8_match_lit("os", import_name, 0)) {
    u32                 fn_count;
    BcOsPrimitiveDecl* fns = bc_os_primitive_decls(ctx_perm(), &fn_count);
    foreach_index(j, fn_count) {
      TypedIndex spliced = splice_native_module_fn(dst_tast, &fns[j]);
      dyn_push(ctx_perm(), *prepend, spliced);
    }
    u32                count_count;
    BcOsConstantDecl* consts = bc_os_constant_decls(ctx_perm(), &count_count);
    foreach_index(j, count_count) {
      TypedIndex spliced = splice_native_module_const(dst_tast, &consts[j]);
      dyn_push(ctx_perm(), *prepend, spliced);
    }
    return true;
  }

  CompiledModule* dep = compile_module(import_name, host_imports, heap);
  if (!dep) {
    fprintf(stderr, "3b run: %.*s failed to import `%.*s` (see errors above)\n",
                     str8_varg(importer_label), str8_varg(import_name));
    return false;
  }
  splice_dep_into(dst_tast, dep, prepend, module_table);
  return true;
}

// Recursively compiles `name`: parse, lower, recursively compile its own
// imports, splice their public signatures in, check, then compile to bytecode
// with a local BcModuleTable for its cross-module calls. Memoized in
// g_module_registry, so a module imported from several places -- or by several
// compiles of the same hot-reloaded script -- compiles once per process.
// Returns NULL with a diagnostic already printed on failure.
static CompiledModule*
compile_module(String8 name, BcHostImportTable* host_imports, Arena* heap) {
  CompiledModule* existing = find_compiled_module(name, host_imports);
  if (existing) return existing;

  String8 mod_src;
  if (!known_module_source(name, ctx_perm(), &mod_src)) {
    // Listed FROM g_known_modules (plus `os`, which resolve_one_import handles
    // itself and so never reaches this table) rather than spelled out, so that
    // adding a module can't leave this message quietly naming the wrong set.
    String8 known = str8_lit("`os`");
    foreach_index(i, ArrayCount(g_known_modules)) {
      known = str8f(ctx_scratch(), "%.*s, `%s`", str8_varg(known), g_known_modules[i].name);
    }
    fprintf(stderr, "3b run: imports unknown module `%.*s` -- a script can only import modules built "
                     "into `3b` itself (currently: %.*s), not another package directory\n",
                     str8_varg(name), str8_varg(known));
    return NULL;
  }
  String8 label       = str8f(ctx_scratch(), "<built-in module `%.*s`>", str8_varg(name));
  u32     mod_file_id = source_file_register(label, mod_src);

  CompiledModule* mod = push_one_zero(ctx_perm(), CompiledModule);
  mod->name     = name;
  mod->host_key = snapshot_host_imports(host_imports, &mod->host_key_count);

  Ast ast;
  ast_init(&ast, ctx_perm());
  NodeIndex* forms        = NULL;
  String8*   own_imports  = NULL;
  if (!script_parse_source(&ast, mod_src, mod_file_id, cstr_from_str8_temp(label), &forms, &own_imports)) {
    return NULL;
  }

  Token synth_open   = {0};
  synth_open.line    = 1;
  synth_open.col     = 1;
  synth_open.file_id = mod_file_id;
  NodeIndex ast_root = ast_push_seq(&ast, AstNodeKind_List, synth_open, forms, (u16)dyn_count(forms));

  typed_ast_init(&mod->tast, ctx_perm());
  Lowerer low = {0};
  low.ast  = &ast;
  low.tast = &mod->tast;
  TypedIndex own_root = lower_program(&low, ast_root);
  if (low.had_error) {
    fprintf(stderr, "3b run: %.*s failed to lower (see errors above)\n", str8_varg(label));
    return NULL;
  }

  // Recursively compile THIS module's own imports first, then splice
  // their public signatures in (AND build this module's own BcModuleTable
  // in the same pass -- see splice_dep_into), PREPENDED to this module's
  // own forms -- mirrors compiler.c's own compile_package ordering
  // exactly, and for the identical reason (check_program's up-front
  // gathering pass needs to see them from the very start).
  //
  // ctx_perm(), NOT a local: `mod->prog` keeps a pointer to this table and
  // BcOp_CallModule dereferences it on every cross-module call for the rest
  // of the run, long after this function has returned -- the same lifetime
  // requirement `mod->prog`'s own compile arena has, just below.
  TypedIndex*    prepend   = NULL;
  BcModuleTable* mod_table = push_one_zero(ctx_perm(), BcModuleTable);
  foreach_index(i, dyn_count(own_imports)) {
    if (!resolve_one_import(own_imports[i], label, &mod->tast, host_imports, heap,
                             &prepend, mod_table)) {
      return NULL;
    }
  }

  TypedNode* own_root_n = &mod->tast.nodes[own_root];
  u32 final_first = (u32)dyn_count(mod->tast.extra);
  foreach_index(i, dyn_count(prepend)) { dyn_push(mod->tast.arena, mod->tast.extra, prepend[i]); }
  foreach_index(i, own_root_n->block.stmt_count) {
    dyn_push(mod->tast.arena, mod->tast.extra, mod->tast.extra[own_root_n->block.stmt_first + i]);
  }
  TypedNode final_block        = {0};
  final_block.kind             = TypedNodeKind_Block;
  final_block.token            = own_root_n->token;
  final_block.block.stmt_first = final_first;
  final_block.block.stmt_count = (u16)(dyn_count(prepend) + own_root_n->block.stmt_count);
  mod->root = typed_push(&mod->tast, final_block);

  mod->ck = check_program(&mod->tast, mod->root, /*is_root_package=*/false, /*scope_query=*/NULL);
  if (mod->ck.had_error) {
    fprintf(stderr, "3b run: %.*s failed to type-check (see errors above)\n", str8_varg(label));
    return NULL;
  }

  // `arena` -- bc_compile_program's compile-time storage for chunks, consts and
  // string literals -- MUST be `ctx_perm()`, not a scratch ArenaTemp.
  // `mod->prog` has to outlive this call: BcOp_CallModule reads straight out of
  // `mod->prog.chunks` on behalf of this module's importers for the rest of the
  // run. Popping a scratch arena after compiling would poison every chunk just
  // built -- the same lifetime trap bcgen.h warns about for `heap`. A one-shot
  // test fixture gets away with a temp only because it runs the program in the
  // same function, before the temp ends.
  mod->prog = bc_compile_program(&mod->ck, &mod->tast, mod->root, ctx_perm(), heap,
                                  module_layout_cache(), host_imports,
                                  dyn_count(mod_table->entries) ? mod_table : NULL);
  if (!mod->prog.ok) {
    fprintf(stderr, "3b run: %.*s failed to compile (see errors above)\n", str8_varg(label));
    return NULL;
  }

  dyn_push(ctx_perm(), g_module_registry, mod);
  return mod;
}

////////////////////////////////
//~ The compile unit: one file, or a whole package directory

// One file of a compile unit: its path, for diagnostics, and the source bytes
// already read off disk.
typedef struct ScriptSource {
  String8 path;
  String8 src;
} ScriptSource;

// The `(package NAME)` name `src` declares, or a zero String8 if its leading
// form isn't one. Lexes only the three tokens that answer the question --
// no parser, and so no diagnostics: this also runs over files that may turn
// out NOT to belong to the compile unit at all (an unrelated `.3b` sitting in
// the same directory), and those must not report their problems against a
// program that never included them.
static String8
peek_package_name(String8 src) {
  ArenaTemp temp = arena_temp_begin(ctx_scratch());
  Lexer lex;
  lexer_init(&lex, src, SOURCE_FILE_UNKNOWN);
  Token open = lexer_next(&lex, temp.arena);
  Token head = lexer_next(&lex, temp.arena);
  Token name = lexer_next(&lex, temp.arena);
  String8 result = {0};
  if (open.kind == TokenKind_LParen && head.kind == TokenKind_Atom
      && str8_match_lit("package", head.text, 0) && name.kind == TokenKind_Atom) {
    result = name.text; // an Atom's text borrows `src` (only a String token copies into
                        // the arena), so this outlives the temp popped below
  }
  arena_temp_end(&temp);
  return result;
}

// `path` after its last '/', or all of it when there is none.
static String8
path_basename(String8 path) {
  for (u64 i = path.size; i-- > 0; ) {
    if (path.str[i] == '/') return str8_skip(path, i + 1);
  }
  return path;
}

// The files that make up the compile unit for `path`: `path` itself, plus
// every other `.3b` file in the same directory declaring the same `(package
// NAME)`. A same-package sibling is not an `(import ...)` of anything -- in 3b
// a directory IS a package, so those files are simply more of this same
// program, and `3b <dir>` has always compiled them together. Ordered by
// filename, exactly as compiler.c orders a native package's files, so that a
// top-level initializer reading another file's global sees the same order on
// both backends.
//
// Deliberately narrow, since the same pipeline also serves script_load's
// hot-reloadable single scripts (which pass `whole_package` false outright,
// see compile_script_program). A `.3bs` never gets siblings: a script is a
// standalone file by definition, and several unrelated ones commonly share a
// directory. Neither does a `.3b` whose neighbours declare a DIFFERENT package
// name -- a scratch directory of unrelated one-file programs keeps working,
// and a neighbour that can't be read or doesn't parse is skipped rather than
// failing the program actually being run.
static ScriptSource*
script_compile_unit(Arena* arena, String8 path, String8 src, b32 whole_package, u64* out_count) {
  ScriptSource* unit  = NULL;
  ScriptSource  alone = { path, src };

  String8 pkg = {0};
  if (whole_package && str8_ends_with(path, str8_lit(".3b"), 0)
      && !str8_ends_with(path, str8_lit(".cfg.3b"), 0)) {
    pkg = peek_package_name(src);
  }
  if (pkg.size == 0) goto single;

  String8 dir  = str8_lit(".");
  String8 base = path_basename(path);
  if (base.size < path.size) {
    u64 dir_size = path.size - base.size - 1; // drop the '/' too
    dir = dir_size ? str8_prefix(path, dir_size) : str8_lit("/");
  }

  u64      file_count = 0;
  String8* files      = dir_list_files_with_ext(arena, dir, ".3b", &file_count);
  if (files == NULL) goto single;

  b32 found_self = false;
  foreach_index(i, file_count) {
    String8      file  = files[i];
    ScriptSource entry = {0};
    // `*.cfg.3b` -- build.c's manifest, translate/config.c's translator config
    // -- share the `.3b` lexer but are never package source, exactly as
    // compiler.c filters them out of a native package's file list.
    if (str8_ends_with(file, str8_lit(".cfg.3b"), 0)) continue;
    if (str8_match(path_basename(file), base, 0)) {
      entry      = alone; // the caller's own spelling of the path, and the bytes already read
      found_self = true;
    } else {
      String8 file_src = file_load_str8(arena, file);
      if (file_src.str == NULL) continue;
      if (!str8_match(peek_package_name(file_src), pkg, 0)) continue;
      entry.path = file;
      entry.src  = file_src;
    }
    dyn_push(arena, unit, entry);
  }
  // The listing didn't turn up the file we were handed (a symlink pointing out
  // of the directory, say). Whatever it did turn up isn't this program.
  if (!found_self) { unit = NULL; goto single; }

  *out_count = dyn_count(unit);
  return unit;

single:
  dyn_push(arena, unit, alone);
  *out_count = dyn_count(unit);
  return unit;
}

// The shared compile pipeline behind both script_run_file, which then requires
// and calls `main`, and script_load/script_poll_reload, which don't -- their
// entry points are resolved by name later via script_call. Reads the file (and
// its same-package siblings, when `whole_package` -- see script_compile_unit),
// parses, lowers, recursively compiles and splices its imports
// (compile_module's dep-handling step, for the script itself), type-checks,
// then compiles.
//
// `whole_package` is false for script_load/script_poll_reload: those keep ONE
// named file loaded and hot-reload it against a content hash of that file
// alone (see load_or_compile_script), so silently pulling in whatever else
// shares its directory would both merge unrelated scripts and leave the cache
// unable to notice an edit to half its own input.
//
// `arena`/`heap` pass straight through to bc_compile_program's same-named
// parameters; see bcgen.h for the lifetime distinction. script_run_file's
// one-shot use can pass a scratch arena, popped right after running `main` in
// the same call. script_load must pass ctx_perm(), since `out_prog` has to
// outlive this call for as long as its ScriptTable slot exists -- the same
// reasoning compile_module gives for its own arena.
static b32
compile_script_program(String8 path, BcHostImportTable* host_imports, b32 whole_package,
                        Arena* arena, Arena* heap, BcProgram* out_prog) {
  String8 src = file_load_str8(ctx_perm(), path);
  if (src.str == NULL) {
    fprintf(stderr, "3b run: could not read '%.*s'\n", str8_varg(path));
    return false;
  }

  u64           unit_count = 0;
  ScriptSource* unit       = script_compile_unit(ctx_perm(), path, src, whole_package, &unit_count);

  Ast ast;
  ast_init(&ast, ctx_perm());

  // Every file of the package parses into this ONE Ast and appends to this one
  // form list -- the same flattening compiler.c does, and what makes a sibling
  // file's types and functions visible here without any import machinery.
  NodeIndex* forms        = NULL;
  String8*   import_names = NULL;
  u32        file_id      = SOURCE_FILE_UNKNOWN; // the file the synthetic root token points at, below
  foreach_index(i, unit_count) {
    u32 unit_file_id = source_file_register(unit[i].path, unit[i].src);
    if (str8_match(unit[i].path, path, 0)) file_id = unit_file_id;
    if (!script_parse_source(&ast, unit[i].src, unit_file_id, cstr_from_str8_temp(unit[i].path),
                              &forms, &import_names)) {
      return false;
    }
  }

  Token synth_open   = {0};
  synth_open.line    = 1;
  synth_open.col     = 1;
  synth_open.file_id = file_id;
  NodeIndex ast_root = ast_push_seq(&ast, AstNodeKind_List, synth_open, forms, (u16)dyn_count(forms));

  TypedAst tast;
  typed_ast_init(&tast, ctx_perm());
  Lowerer low = {0};
  low.ast  = &ast;
  low.tast = &tast;
  TypedIndex own_root = lower_program(&low, ast_root);
  if (low.had_error) {
    fprintf(stderr, "3b run: '%.*s' failed to lower (see errors above)\n", str8_varg(path));
    return false;
  }

  // This script's OWN layout cache. Modules compile against a separate,
  // process-wide one (module_layout_cache) -- see g_module_layout_cache on why
  // the two must not be the same table.
  LayoutCache layout_cache;
  layout_cache_init(&layout_cache, ctx_perm());

  // Recursively compile every DIRECTLY imported module (which transitively
  // compiles whatever THEY import too), then splice each one's public
  // signatures in (AND build the script's own BcModuleTable in the same
  // pass -- see splice_dep_into), PREPENDED to the script's own forms --
  // exactly compile_module's own dep-handling step.
  //
  // ctx_perm(), NOT a local: `*out_prog` keeps a pointer to this table and
  // BcOp_CallModule dereferences it on every cross-module call for as long as
  // that program is callable -- which, for script_load's hot-reloadable
  // slots, is the whole session, not just this function.
  TypedIndex*    prepend      = NULL;
  BcModuleTable* module_table = push_one_zero(ctx_perm(), BcModuleTable);
  String8 quoted_path = str8f(ctx_scratch(), "'%.*s'", str8_varg(path)); // matches this function's own
                                                              // quoted-path message style elsewhere
  foreach_index(i, dyn_count(import_names)) {
    if (!resolve_one_import(import_names[i], quoted_path, &tast, host_imports, ctx_perm(),
                             &prepend, module_table)) {
      return false;
    }
  }

  TypedNode* own_root_n = &tast.nodes[own_root];
  u32 final_first = (u32)dyn_count(tast.extra);
  foreach_index(i, dyn_count(prepend)) { dyn_push(tast.arena, tast.extra, prepend[i]); }
  foreach_index(i, own_root_n->block.stmt_count) {
    dyn_push(tast.arena, tast.extra, tast.extra[own_root_n->block.stmt_first + i]);
  }
  TypedNode final_block        = {0};
  final_block.kind             = TypedNodeKind_Block;
  final_block.token            = own_root_n->token;
  final_block.block.stmt_first = final_first;
  final_block.block.stmt_count = (u16)(dyn_count(prepend) + own_root_n->block.stmt_count);
  TypedIndex root = typed_push(&tast, final_block);

  Checker ck = check_program(&tast, root, /*is_root_package=*/true, /*scope_query=*/NULL);
  if (ck.had_error) {
    fprintf(stderr, "3b run: '%.*s' failed to type-check (see errors above)\n", str8_varg(path));
    return false;
  }

  // `module_table` (built above, alongside `prepend`, by splice_dep_into)
  // resolves the script's own qualified calls ("build/getenv") to each
  // DIRECTLY imported module's already-compiled BcProgram -- which, per
  // compile_module's own recursive step, already has ITS OWN
  // transitively-imported modules resolved too, nothing further needed
  // here for a deeper-than-one-level import chain.
  BcProgram prog = bc_compile_program(&ck, &tast, root, arena, heap, &layout_cache, host_imports,
                                       dyn_count(module_table->entries) ? module_table : NULL);
  if (!prog.ok) {
    // bc_compile_program already printed a real diag_error at the exact
    // construct that isn't supported yet -- see bc_unsupported in bcgen.c.
    fprintf(stderr, "3b run: '%.*s' failed to compile (see errors above)\n", str8_varg(path));
    return false;
  }
  *out_prog = prog;
  return true;
}

// Finds a chunk by name WITHOUT bc_program_find_fn's own assert-on-miss --
// used everywhere below an entry point is being resolved for the FIRST
// time (a normal "not found" outcome here, not an embedding-program bug).
static b32
find_chunk(BcProgram* prog, String8 name, u32* out_index) {
  foreach_index(i, dyn_count(prog->chunks)) {
    if (str8_match(prog->chunks[i].name, name, 0)) {
      *out_index = (u32)i;
      return true;
    }
  }
  return false;
}

b32
script_run_file(const char* path_cstr, BcHostImportTable* host_imports, BcResult* out_result) {
  String8 path = str8_cstring((char*)path_cstr);

  BcHostImportTable empty_host_imports = {0};
  BcHostImportTable* imports = host_imports ? host_imports : &empty_host_imports;

  ArenaTemp fn_temp = arena_temp_begin(ctx_scratch());
  BcProgram prog;
  if (!compile_script_program(path, imports, /*whole_package=*/true, fn_temp.arena, ctx_perm(), &prog)) {
    arena_temp_end(&fn_temp);
    return false;
  }

  // Find `main` BEFORE running -- bc_run_in_program itself asserts on an
  // unknown fn_index/arity mismatch (an embedding-program bug, by its own
  // convention), which would be a confusing way for a SCRIPT AUTHOR to learn
  // "you forgot `main`" -- this gives a real diagnostic instead.
  u32 main_fn;
  if (!find_chunk(&prog, str8_lit("main"), &main_fn)) {
    fprintf(stderr, "3b run: '%s' has no `main` function -- a `.3bs` script needs a zero-parameter "
                     "`main` entry point, same as a native 3b program\n", path_cstr);
    arena_temp_end(&fn_temp);
    return false;
  }

  // Either entry-point shape a native 3b program can have: no parameters, or
  // `[argc i32 argv string*]`. check_main_signature ran over this same compile
  // (compile_script_program checks as a root package), so it has already
  // rejected any other arity or types -- a 2-parameter `main` here is known to
  // be exactly (i32, string*), and the guard below is only for a `main`
  // reaching the VM some other way.
  //
  // No marshalling is needed for argv: `string` is the same {u8*, u64} pair as
  // the host's own String8 (layout.c's TypeKind_String) and a VM pointer IS a
  // host pointer (bcvm.c's Alloc), so a plain String8 array is already the
  // `string*` the program expects. It holds one entry, the program's own path,
  // matching argv[0] -- `3b run` forwards nothing beyond it, the same as
  // `3b run <package-dir>`, which executes the linked binary with no arguments
  // of its own either.
  u32     param_count = prog.chunks[main_fn].param_count;
  String8 prog_argv[] = { path };
  i64     main_args[] = { (i64)ArrayCount(prog_argv), (i64)(intptr_t)prog_argv };
  if (param_count != 0 && param_count != ArrayCount(main_args)) {
    fprintf(stderr, "3b run: '%s': `main` must take either no parameters or exactly two (argc, argv)\n",
                     path_cstr);
    arena_temp_end(&fn_temp);
    return false;
  }

  *out_result = bc_run_in_program(&prog, main_fn, param_count ? main_args : NULL, param_count,
                                   ctx_perm(), imports);

  // A trap stopped the script partway through (BcResult.trapped). Reported
  // here, not by the caller: `trap_fn` borrows from a chunk of `prog`, which
  // the arena_temp_end below frees. Reported as a FAILURE for the same reason
  // a parse error is -- a script that quit a third of the way through its work
  // and one that ran to completion must not both be a silent exit 0.
  if (out_result->trapped) {
    fprintf(stderr, "3b run: '%s' stopped on a runtime error in `%.*s`: %.*s\n",
                     path_cstr, str8_varg(out_result->trap_fn), str8_varg(out_result->trap_message));
    arena_temp_end(&fn_temp);
    return false;
  }

  arena_temp_end(&fn_temp);
  return true;
}

////////////////////////////////
//~ Hot-reloadable script embedding -- see script.h's own top-of-section
//~ comment for the design (host owns all state; scripts receive handles).

// The on-disk cache path for `path`: its extension replaced with `.3bc`, so
// "orbiter.3bs" -> "orbiter.3bc". ONE fixed name per source file, overwritten
// in place on every recompile rather than accumulating a file per edit.
// Validity is therefore not encoded in the filename but checked against
// BcLoadResult's `content_hash`, read back out of the file itself.
static String8
cache_path_for(String8 path) {
  u64 dot = path.size; // no extension found -- falls through to "append .3bc to the whole path"
  for (u64 i = path.size; i-- > 0; ) {
    if (path.str[i] == '/') break;   // don't cross a directory separator looking for a dot
    if (path.str[i] == '.') { dot = i; break; }
  }
  String8 base = str8_substr(path, rng_1u64(0, dot));
  return str8f(ctx_scratch(), "%.*s.3bc", str8_varg(base));
}

// Replaces `dst_path` with `tmp_path` in one step, creating `dst_path` if it
// isn't there. Spelled out per-platform because C's own rename() only has these
// semantics on POSIX: MSVCRT's FAILS outright when `dst_path` already exists --
// which, for a cache file being refreshed, is the ordinary case, not the
// exception. MOVEFILE_REPLACE_EXISTING asks Win32 for the POSIX behaviour.
//
// Returns whether it worked, so the caller can say so. A silently unreplaced
// cache is permanent: every later load re-reads a `.3bc` whose content hash can
// no longer match, so the script recompiles from source on every single load
// for the rest of that file's life, with nothing on screen to explain why.
static b32
replace_file(String8 tmp_path, String8 dst_path) {
#if defined(_WIN32)
  return MoveFileExA((char*)cstr_from_str8_temp(tmp_path), (char*)cstr_from_str8_temp(dst_path),
                      MOVEFILE_REPLACE_EXISTING) != 0;
#else
  return rename((char*)cstr_from_str8_temp(tmp_path), (char*)cstr_from_str8_temp(dst_path)) == 0;
#endif
}

// What a CACHED load needs in order to rebuild the BcModuleTable the file only
// recorded qualified NAMES for (see BcModuleResolveFn in bytecode.h): the same
// host-imports/heap pair a from-source compile threads through compile_module.
// Lives on load_or_compile_script's own stack; nothing outlives that call
// except the CompiledModules themselves, which compile_module allocates on
// ctx_perm() precisely because BcOp_CallModule keeps reading out of them for
// the rest of the run.
//
// Holds neither the module registry nor the layout cache: both are
// process-wide (g_module_registry), so a cache-hit load resolves against the
// very same modules a from-source compile does.
typedef struct CachedModuleResolverCtx {
  BcHostImportTable* host_imports;
  Arena*             heap;
} CachedModuleResolverCtx;

// Resolves ONE saved qualified name ("build/getenv") back to a live module.
// The name splits at its single `/` into the module and the function within
// it, and compile_module does the rest -- the SAME memoized compile path a
// from-source run takes, so a cached script and a freshly-compiled one end
// up calling into an identically-produced module BcProgram.
//
// Resolving by NAME rather than trusting a saved chunk index is deliberate,
// the same reasoning bc_program_save applies to host imports: a module's
// own chunk ordering is an artifact of how it happened to compile, not
// something a file written by an older build should get to assert.
//
// `os/...` never reaches here: those members are plain host imports resolved
// through bcgen.c's by-name fallback, and never get a BcModuleTable entry in
// the first place (see resolve_one_import's own `os` case).
static b32
resolve_cached_module_import(String8 qualified_name, void* userdata,
                              BcProgram** out_prog, u32* out_fn_index) {
  CachedModuleResolverCtx* ctx = (CachedModuleResolverCtx*)userdata;

  u64 slash = qualified_name.size;
  foreach_index(i, qualified_name.size) {
    if (qualified_name.str[i] == '/') { slash = i; break; }
  }
  if (slash == qualified_name.size) {
    fprintf(stderr, "3b run: cached module import `%.*s` is not a qualified `module/function` name\n",
                     str8_varg(qualified_name));
    return false;
  }
  String8 module_name = str8_substr(qualified_name, rng_1u64(0, slash));
  String8 member_name = str8_substr(qualified_name, rng_1u64(slash + 1, qualified_name.size));

  CompiledModule* dep = compile_module(module_name, ctx->host_imports, ctx->heap);
  if (!dep) return false; // compile_module already printed a real diagnostic

  u32 fn_index;
  if (!find_chunk(&dep->prog, member_name, &fn_index)) {
    fprintf(stderr, "3b run: cached module import `%.*s` names a function module `%.*s` no longer "
                     "defines\n", str8_varg(qualified_name), str8_varg(module_name));
    return false;
  }
  *out_prog     = &dep->prog;
  *out_fn_index = fn_index;
  return true;
}

// Tries the on-disk cache for `path` first -- a zero-copy mmap load skipping
// parse/lower/check/compile entirely. A hit also requires
// `loaded.content_hash == hash`, since the filename no longer encodes which
// source it was built from; a mismatch is treated like a bad magic/version, as
// "no usable cache" rather than an error.
//
// Falls back to compile_script_program on any miss -- missing file, I/O error,
// bad magic/version, or hash mismatch -- then best-effort writes a fresh cache
// over whatever was at that path. bc_program_save's result isn't fatal: a
// read-only install directory just means no cache this time.
//
// `*out_mapping` is the loaded mmap on a hit, which needs bc_program_unload
// once superseded (see ScriptSlot), or a zeroed -- still safe to unload --
// BcLoadResult otherwise.
//
// `outgoing_mapping` is the caller's CURRENT mapping of this same cache file,
// if it has one (script_poll_reload does whenever the running program came from
// a cache hit; script_load never does, and passes NULL). See the cache-write
// step below for why this function wants it rather than leaving it to the
// caller.
//
// The `stat` GATES the bc_program_load call rather than just checking its `ok`
// result, because a missing cache file is the common case on a script's first
// load, and file.c's file_map unconditionally prints "failed to open" for it.
// That message is for genuinely unexpected I/O failure. A cache file that does
// exist but is corrupt or stale still goes through bc_program_load and gets its
// own diagnostics.
static b32
load_or_compile_script(String8 path, u64 hash, BcHostImportTable* host_imports,
                        BcLoadResult* outgoing_mapping, BcProgram* out_prog,
                        BcLoadResult* out_mapping) {
  String8     cache_path = cache_path_for(path);
  struct stat st;
  if (stat((char*)cstr_from_str8_temp(cache_path), &st) == 0) {
    CachedModuleResolverCtx resolver_ctx = {0};
    resolver_ctx.host_imports = host_imports;
    resolver_ctx.heap         = ctx_perm();
    BcModuleResolver resolver = { resolve_cached_module_import, &resolver_ctx };

    BcLoadResult loaded = bc_program_load(cache_path, host_imports, &resolver, ctx_perm(), ctx_perm());
    if (loaded.ok && loaded.content_hash == hash) {
      *out_prog    = loaded.program;
      *out_mapping = loaded;
      return true;
    }
    if (loaded.ok) bc_program_unload(&loaded); // stale-by-hash -- unmap before recompiling+overwriting
  }
  if (!compile_script_program(path, host_imports, /*whole_package=*/false, ctx_perm(), ctx_perm(), out_prog))
    return false;
  *out_mapping = (BcLoadResult){0};

  // The recompile SUCCEEDED, so the caller's outgoing program -- and with it
  // the mapping of the very cache file about to be replaced -- is already
  // superseded, and its mapping goes NOW rather than after this function
  // returns. Windows keeps a file locked for as long as a mapped view of it
  // exists, so replacing it under a live mapping fails outright; dropping the
  // view first is what lets the cache actually refresh across a hot-reload
  // session there. The caller still calls bc_program_unload on it, which
  // file_unmap makes a harmless no-op the second time.
  //
  // The PLACEMENT matters: every failure path above returns without reaching
  // here, deliberately, because script_poll_reload's contract is that a
  // mid-edit compile error leaves the last-good program running -- and that
  // program is reading straight out of this mapping.
  if (outgoing_mapping) bc_program_unload(outgoing_mapping);

  // Written to a TEMP path first and then moved over `cache_path`, NEVER
  // written to `cache_path` directly: the move swaps the whole file in one
  // step, so a crash or a full disk partway through leaves the OLD cache
  // intact rather than a half-written one that the next load has to detect
  // and reject. (It also leaves any mapping still held elsewhere reading the
  // old, untouched file on POSIX, rather than having bytes rewritten under
  // it.)
  String8 tmp_path = str8f(ctx_scratch(), "%.*s.tmp", str8_varg(cache_path));
  if (bc_program_save(out_prog, host_imports, tmp_path, hash)) {
    if (!replace_file(tmp_path, cache_path)) {
      // Not fatal -- `*out_prog` is compiled and fine -- but not silent
      // either: the stale `.3bc` left on disk can never hash-match again, so
      // without this the script would just quietly recompile from source on
      // every load, forever.
      fprintf(stderr, "3b run: compiled '%.*s' but could not update its bytecode cache '%.*s' -- it "
                       "will recompile from source on every load until that file is writable\n",
                       str8_varg(path), str8_varg(cache_path));
      remove((char*)cstr_from_str8_temp(tmp_path)); // don't leave a stray `.3bc.tmp` behind
    }
  }
  return true;
}

b32
script_load(ScriptTable* table, String8 path, BcHostImportTable* host_imports, ScriptHandle* out_handle) {
  String8 src = file_load_str8(ctx_scratch(), path);
  if (src.str == NULL) {
    fprintf(stderr, "3b run: could not read '%.*s'\n", str8_varg(path));
    return false;
  }
  u64 hash = bc_content_hash(src);

  BcProgram    prog;
  BcLoadResult mapping;
  if (!load_or_compile_script(path, hash, host_imports, /*outgoing_mapping=*/NULL, &prog, &mapping)) {
    return false;
  }

  if (dyn_count(table->slots) == 0) {
    ScriptSlot null_slot = {0}; // index 0 reserved dead, see ScriptTable's own comment
    dyn_push(ctx_perm(), table->slots, null_slot);
  }
  u32 index = (u32)dyn_count(table->slots);
  ScriptSlot slot = {0};
  slot.alive         = true;
  slot.path          = str8_copy(ctx_perm(), path);
  slot.content_hash  = hash;
  slot.prog          = prog;
  slot.cache_mapping = mapping;
  dyn_push(ctx_perm(), table->slots, slot);

  out_handle->index = index;
  return true;
}

b32
script_call(ScriptTable* table, ScriptHandle handle, String8 fn_name, i64* args, u32 argc,
            Arena* heap, BcHostImportTable* host_imports, BcResult* out_result) {
  if (handle.index == 0 || handle.index >= dyn_count(table->slots) || !table->slots[handle.index].alive) {
    fprintf(stderr, "script_call: stale or invalid script handle\n");
    return false;
  }
  ScriptSlot* slot = &table->slots[handle.index];

  u32 fn_index;
  if (!find_chunk(&slot->prog, fn_name, &fn_index)) {
    fprintf(stderr, "script_call: '%.*s' has no function named '%.*s'\n",
                     str8_varg(slot->path), str8_varg(fn_name));
    return false;
  }
  if (slot->prog.chunks[fn_index].param_count != argc) {
    fprintf(stderr, "script_call: '%.*s'.%.*s takes %u argument(s), got %u\n",
                     str8_varg(slot->path), str8_varg(fn_name), slot->prog.chunks[fn_index].param_count, argc);
    return false;
  }

  *out_result = bc_run_in_program(&slot->prog, fn_index, args, argc, heap, host_imports);
  if (out_result->trapped) {
    // Same rule as the three checks above: no value came back, so this is a
    // false. A host calling a script every frame would otherwise read the trap
    // as the number 0 -- and keep doing so, silently, for as long as the
    // script stays broken.
    fprintf(stderr, "script_call: '%.*s'.%.*s stopped on a runtime error in `%.*s`: %.*s\n",
                     str8_varg(slot->path), str8_varg(fn_name),
                     str8_varg(out_result->trap_fn), str8_varg(out_result->trap_message));
    return false;
  }
  return true;
}

b32
script_poll_reload(ScriptTable* table, ScriptHandle handle, BcHostImportTable* host_imports) {
  if (handle.index == 0 || handle.index >= dyn_count(table->slots) || !table->slots[handle.index].alive) {
    fprintf(stderr, "script_poll_reload: stale or invalid script handle\n");
    return false;
  }
  ScriptSlot* slot = &table->slots[handle.index];

  ArenaTemp scratch = arena_temp_begin(ctx_scratch());
  String8   src     = file_load_str8(scratch.arena, slot->path);
  if (src.str == NULL) {
    // file missing/unreadable THIS poll -- keep running the last-good
    // BcProgram (e.g. an editor save-in-progress can transiently do this)
    arena_temp_end(&scratch);
    return false;
  }
  u64 hash = bc_content_hash(src);
  arena_temp_end(&scratch);
  if (hash == slot->content_hash) return false; // unchanged, nothing to do

  BcProgram    new_prog;
  BcLoadResult new_mapping;
  if (!load_or_compile_script(slot->path, hash, host_imports, &slot->cache_mapping, &new_prog,
                               &new_mapping)) {
    // Recompile failed -- `slot->prog` (the old, still-working program) is
    // left completely untouched, see script_poll_reload's own script.h
    // comment on why a mid-edit syntax error must never kill a live
    // session. Store the new hash anyway so a broken save doesn't spam a
    // fresh diagnostic every single poll until the NEXT real edit.
    slot->content_hash = hash;
    return false;
  }
  bc_program_unload(&slot->cache_mapping); // safe no-op if the OUTGOING prog wasn't cache-backed, or
                                               // if load_or_compile_script already dropped it to write
                                               // the cache (see its `outgoing_mapping`)
  slot->content_hash  = hash;
  slot->prog          = new_prog;
  slot->cache_mapping = new_mapping;
  return true;
}

void
script_unload(ScriptTable* table, ScriptHandle handle) {
  if (handle.index == 0 || handle.index >= dyn_count(table->slots)) return;
  ScriptSlot* slot = &table->slots[handle.index];
  bc_program_unload(&slot->cache_mapping);
  slot->alive = false;
}
