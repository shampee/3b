// compiler.c -- the recursive package/import compiler. compiler.h carries the
// design overview: one directory is one package, `import` resolves a sibling
// directory, and an import's public surface is spliced into the importer's own
// TypedAst. compile_all_packages is the only entry point; everything else here
// is internal to the recursion.
#include "compiler.h"
#include "file.h"
#include "native_pkgs_embed.h" // g_embed_native_pkgs_* -- see known_embedded_native_package_source
#include <limits.h>
#include <string.h>
#include <sys/stat.h>

////////////////////////////////
//~ Embedded native packages ("os", "vm", "rng")
//
// These ship inside the compiler binary itself (Makefile's
// NATIVE_PKG_EMBED_FILES -> tools/embed_runtime -> native_pkgs_embed.h, the
// same technique runtime_embed.h and script_embed.h use), so `(import os)`
// needs no `os/` directory in the project. Output paths are keyed by package
// name rather than by source directory, so the generated output/os.h/.c still
// lands exactly where a real package's would. compile_package reaches for
// these only when canonicalize_path fails.

// Reassembles an embed_runtime-style per-line array into one String8. script.c
// has its own copy for the `.3bs` driver's embedded modules.
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

// Looks up `name` -- a package name no real directory resolved for -- among the
// embedded packages, returning its unparsed `.3b` source via `*out_src`.
static b32
known_embedded_native_package_source(String8 name, String8* out_src) {
  if (str8_match_lit("os", name, 0))  { *out_src = join_embedded_lines(ctx_perm(), g_embed_native_pkgs_os_os_3b);   return true; }
  if (str8_match_lit("vm", name, 0))  { *out_src = join_embedded_lines(ctx_perm(), g_embed_native_pkgs_vm_vm_3b);   return true; }
  if (str8_match_lit("rng", name, 0)) { *out_src = join_embedded_lines(ctx_perm(), g_embed_native_pkgs_rng_rng_3b); return true; }
  return false;
}

////////////////////////////////
//~ Path helpers

// Last path component, trailing slashes ignored: "foo/bar/" and "foo/bar" both
// give "bar". Doubles as a basename extractor for file paths.
static String8
package_name_from_dir_path(String8 dir_path) {
  String8 trimmed = dir_path;
  while (trimmed.size > 0 && char_is_slash(trimmed.str[trimmed.size - 1])) trimmed.size -= 1;

  u64 last_slash_end = 0;
  for (u64 i = trimmed.size; i > 0; i -= 1) {
    if (char_is_slash(trimmed.str[i - 1])) { last_slash_end = i; break; }
  }
  return str8_substr(trimmed, rng_1u64(last_slash_end, trimmed.size));
}

// `base` + "/" + `leaf`, trimming any trailing slash off `base` so double
// slashes never reach paths or diagnostics.
static String8
str8_join_path(String8 base, String8 leaf) {
  String8 trimmed = base;
  while (trimmed.size > 0 && char_is_slash(trimmed.str[trimmed.size - 1])) trimmed.size -= 1;
  String8 with_slash = str8_cat(ctx_perm(), trimmed, str8_lit("/"));
  return str8_cat(ctx_perm(), with_slash, leaf);
}

// Resolves `.`, `..` and symlinks so one physical directory reached by two
// different relative paths registers as the same package build -- both for
// memoization (a diamond dependency compiles once) and for cycle detection.
// Returns an empty String8 if the path doesn't resolve.
static String8
canonicalize_path(String8 path) {
  char resolved[PATH_MAX];
  // Windows has no realpath; _fullpath resolves `.`/`..` the same way, minus
  // symlink resolution.
#if defined(_WIN32)
  if (!_fullpath(resolved, (char*)cstr_from_str8_temp(path), PATH_MAX)) return (String8){0};
#else
  if (!realpath((char*)cstr_from_str8_temp(path), resolved)) return (String8){0};
#endif
  return str8_copy(ctx_perm(), str8_cstring(resolved));
}

////////////////////////////////
//~ Package build registry -- memoization and cycle detection across the whole
// compilation run.

static PackageBuild*
find_package_build(PackageBuild** registry, String8 canonical_dir) {
  foreach_index(i, dyn_count(registry)) {
    if (str8_match(registry[i]->canonical_dir, canonical_dir, 0)) return registry[i];
  }
  return NULL;
}

// Package names must be unique across the whole program. Since a package's name
// is its directory's basename, this amounts to "no two directories share a
// basename", and catches two distinct packages both named e.g. "utils" as a
// clear diagnostic rather than a later file and C symbol collision.
static PackageBuild*
find_package_build_by_name(PackageBuild** registry, String8 pkg_name) {
  foreach_index(i, dyn_count(registry)) {
    if (str8_match(registry[i]->pkg_name, pkg_name, 0)) return registry[i];
  }
  return NULL;
}

////////////////////////////////
//~ Splicing another package's public surface into an importer

static String8
qualify_name(String8 pkg_name, String8 member_name) {
  return str8_cat(ctx_perm(), str8_cat(ctx_perm(), pkg_name, str8_lit("/")), member_name);
}

// Builds dep->public_toplevel_names in one walk of dep's top-level forms,
// rather than re-walking them for every type reference
// requalify_type_for_import processes -- for a dep with a large public surface
// (a GL binding, say) that walk dominated the whole compile.
// cg_build_public_toplevel_names in codegen.c is the same thing over a Codegen.
static void
build_public_toplevel_names(PackageBuild* dep) {
  hashtable_init(ctx_perm(), &dep->public_toplevel_names, 64);
  TypedNode* root_n = &dep->tast->nodes[dep->root];
  foreach_index(i, root_n->block.stmt_count) {
    TypedIndex stmt = dep->tast->extra[root_n->block.stmt_first + i];
    if (stmt == TYPED_NIL) continue;
    TypedNode* sn = &dep->tast->nodes[stmt];
    if (sn->is_private || sn->is_imported) continue;
    String8 decl_name;
    switch (sn->kind) {
      case TypedNodeKind_FunctionDecl:
        if (sn->func.body == TYPED_NIL) continue; // real `extern` -- not one of dep's own symbols
        decl_name = sn->func.name;
        break;
      case TypedNodeKind_StructDecl: decl_name = sn->struct_decl.name; break;
      case TypedNodeKind_EnumDecl:   decl_name = sn->enum_decl.name;   break;
      case TypedNodeKind_AliasDecl:  decl_name = sn->alias_decl.name;  break;
      case TypedNodeKind_ConstDecl:  decl_name = sn->const_decl.name;  break;
      case TypedNodeKind_VarDecl:    decl_name = sn->var_decl.name;    break;
      default: continue;
    }
    hashtable_insert(ctx_perm(), &dep->public_toplevel_names, decl_name, (void*)1, true);
  }
}

// Is `name` (exact, unqualified) one of `dep`'s own public top-level
// declarations, as opposed to something `dep` itself spliced in from one of its
// imports? O(1) after the first call for a given dep.
static b32
is_own_public_toplevel_name(PackageBuild* dep, String8 name) {
  if (!dep->public_toplevel_names_built) {
    build_public_toplevel_names(dep);
    dep->public_toplevel_names_built = true;
  }
  return hashtable_lookup(&dep->public_toplevel_names, name) != NULL;
}

// A spliced signature -- a fn's params and return type, a struct's field types,
// a global's type -- can reference one of `dep`'s own top-level types by its
// bare name, since dep's source never had to qualify a reference to itself.
// Inside the importer that bare name would resolve to nothing, or worse to an
// unrelated same-named local type, so every Named/Handle/alias reference is
// rewritten to "dep/Name" here, recursively through Pointer and Array wrapping.
// A name that is already qualified came from one of dep's own imports and is
// left alone -- see compiler.h on transitive re-export.
static TypeRef
requalify_type_for_import(PackageBuild* dep, TypeRef t) {
  if (t.kind == TypeKind_Pointer || t.kind == TypeKind_Array) {
    TypeRef* boxed = push_one(ctx_perm(), TypeRef);
    *boxed = requalify_type_for_import(dep, *t.pointee);
    t.pointee = boxed;
    return t;
  }
  if (t.alias_name.size > 0 && is_own_public_toplevel_name(dep, t.alias_name)) {
    t.alias_name = qualify_name(dep->pkg_name, t.alias_name);
  } else if ((t.kind == TypeKind_Named || t.kind == TypeKind_Handle) && is_own_public_toplevel_name(dep, t.name)) {
    t.name = qualify_name(dep->pkg_name, t.name);
  }
  return t;
}

// Copies one of `dep`'s public top-level declarations into `dst`, the importing
// package's TypedAst, renamed to `qualified_name` ("pkg/member") and marked
// `is_imported` so codegen skips it. Params, fields and variants live in
// per-TypedAst parallel arrays (TypedAst.params, .enum_variants), so those
// ranges are copied across as well; everything else (String8 text,
// TypeRef.pointee) already lives in the single ctx_perm() arena every package
// in the run allocates from, so a plain struct copy suffices. Bodies and
// initializers are never carried across: the real definition lives in dep's own
// emitted .c.
static TypedIndex
splice_public_decl(TypedAst* dst, PackageBuild* dep, TypedIndex src_idx, String8 qualified_name) {
  TypedNode copy = dep->tast->nodes[src_idx];
  copy.is_private  = false;
  copy.is_imported = true;
  switch (copy.kind) {
    case TypedNodeKind_FunctionDecl: {
      copy.func.name        = qualified_name;
      copy.func.body        = TYPED_NIL;
      copy.func.return_type = requalify_type_for_import(dep, copy.func.return_type);
      u32 new_first = (u32)dyn_count(dst->params);
      foreach_index(i, copy.func.param_count) {
        Param p = dep->tast->params[copy.func.param_first + i];
        p.type  = requalify_type_for_import(dep, p.type);
        dyn_push(dst->arena, dst->params, p);
      }
      copy.func.param_first = new_first;
    } break;
    case TypedNodeKind_StructDecl: {
      copy.struct_decl.name = qualified_name;
      u32 new_first = (u32)dyn_count(dst->params);
      foreach_index(i, copy.struct_decl.field_count) {
        Param f = dep->tast->params[copy.struct_decl.field_first + i];
        f.type  = requalify_type_for_import(dep, f.type);
        dyn_push(dst->arena, dst->params, f);
      }
      copy.struct_decl.field_first = new_first;
    } break;
    case TypedNodeKind_EnumDecl: {
      copy.enum_decl.name = qualified_name;
      u32 new_first = (u32)dyn_count(dst->enum_variants);
      foreach_index(i, copy.enum_decl.variant_count) {
        dyn_push(dst->arena, dst->enum_variants, dep->tast->enum_variants[copy.enum_decl.variant_first + i]);
      }
      copy.enum_decl.variant_first = new_first;
    } break;
    case TypedNodeKind_AliasDecl: {
      copy.alias_decl.name = qualified_name;
      copy.alias_decl.type = requalify_type_for_import(dep, copy.alias_decl.type);
    } break;
    case TypedNodeKind_ConstDecl: {
      copy.const_decl.name = qualified_name;
      copy.const_decl.type = requalify_type_for_import(dep, copy.const_decl.type);
      copy.const_decl.init = TYPED_NIL;
    } break;
    case TypedNodeKind_VarDecl: {
      copy.var_decl.name = qualified_name;
      copy.var_decl.type = requalify_type_for_import(dep, copy.var_decl.type);
      copy.var_decl.init = TYPED_NIL;
    } break;
    default: return TYPED_NIL; // not a top-level decl kind -- nothing to splice
  }
  return typed_push(dst, copy);
}

// Seeds an importing package's Lowerer with `dep`'s public struct, enum and
// alias names, each registered under "dep/OwnName" -- the text a `dep/Name/...`
// slash-split in lower_atom (EnumAccess) or a `dep/Name` construction-site
// lookup in lower_list (is_known_struct_name) produces. This is the only
// seeding lowering needs; value and call references need none.
static void
seed_lowerer_with_import(Lowerer* low, PackageBuild* dep) {
  TypedNode* root_n = &dep->tast->nodes[dep->root];
  foreach_index(i, root_n->block.stmt_count) {
    TypedIndex stmt = dep->tast->extra[root_n->block.stmt_first + i];
    if (stmt == TYPED_NIL) continue;
    TypedNode* sn = &dep->tast->nodes[stmt];
    if (sn->is_private || sn->is_imported) continue;
    if (sn->kind == TypedNodeKind_StructDecl) {
      // The struct's declaration form lives in `dep`'s Ast, a different
      // node-index space than `low->ast`, so there is no valid NodeIndex to
      // record. NODE_NIL is the same placeholder lower_anon_struct_type uses,
      // and lower_struct_construct turns it into a "use named construction"
      // error rather than crashing.
      lower_register_struct_name(low, qualify_name(dep->pkg_name, sn->struct_decl.name), NODE_NIL);
    } else if (sn->kind == TypedNodeKind_EnumDecl) {
      lower_register_enum_name(low, qualify_name(dep->pkg_name, sn->enum_decl.name));
    } else if (sn->kind == TypedNodeKind_AliasDecl) {
      TypeAlias alias = {0};
      alias.name       = qualify_name(dep->pkg_name, sn->alias_decl.name);
      alias.type        = requalify_type_for_import(dep, sn->alias_decl.type);
      lower_register_alias(low, alias);
    }
  }
}

////////////////////////////////
//~ Per-file form validation: strip `(package name)` and `(import name)`

// Validates and strips a file's leading `(package <name>)` form. On success
// writes the remaining forms into `out_forms`/`out_count` -- a slice into the
// shared Ast's `extra` array, not a copy. Prints a diagnostic and returns false
// otherwise.
static b32
validate_and_strip_package_form(Ast* ast, NodeIndex file_root, u32 file_id,
                                 String8 expected_pkg_name, NodeIndex** out_forms, u16* out_count) {
  u16        fc;
  NodeIndex* fchildren = ast_seq_children(ast, file_root, &fc);
  if (fc < 1) {
    Token empty_tok = {0};
    empty_tok.line    = 1;
    empty_tok.col     = 1;
    empty_tok.file_id = file_id;
    diag_error(empty_tok, "expected `(package %.*s)` as the first form, file is empty",
               str8_varg(expected_pkg_name));
    return false;
  }

  AstNode* first = ast_get(ast, fchildren[0]);
  u16      pc    = 0;
  NodeIndex* pchildren = (first->kind == AstNodeKind_List) ? ast_seq_children(ast, fchildren[0], &pc) : NULL;
  AstNode*   phead      = (pc > 0) ? ast_get(ast, pchildren[0]) : NULL;
  b32        shape_ok   = first->kind == AstNodeKind_List && pc == 2
                        && phead && phead->kind == AstNodeKind_Atom
                        && str8_match_lit("package", phead->token.text, 0);
  if (!shape_ok) {
    diag_error(first->token, "expected `(package %.*s)` as the first form", str8_varg(expected_pkg_name));
    return false;
  }

  AstNode* name_node = ast_get(ast, pchildren[1]);
  if (name_node->kind != AstNodeKind_Atom || !str8_match(expected_pkg_name, name_node->token.text, 0)) {
    diag_error(name_node->token, "package name `%.*s` doesn't match this directory's name `%.*s`",
               str8_varg(name_node->token.text), str8_varg(expected_pkg_name));
    return false;
  }

  *out_forms = fchildren + 1;
  *out_count = (u16)(fc - 1);
  return true;
}

// Collects and removes every `(import name)` form in `forms`. Imports may
// appear anywhere in the list, not just up front, the same freedom `private`
// and `extern` have. Names are appended to `*import_names`, deduplicated; the
// returned array holds every other form in its original order. Non-static:
// script.c reuses this scan for `.3bs` files, whose imports resolve differently
// (splicing an embedded module's source rather than compiling a sibling
// directory) but need the same strip-and-collect step first.
NodeIndex*
strip_import_forms(Ast* ast, NodeIndex* forms, u16 form_count, u16* out_count, String8** import_names) {
  NodeIndex* filtered = NULL;
  String8*   names     = *import_names; // dyn_push expands to `arr[_c] = val`, so `*import_names`
                                         // there would parse as `*(import_names[_c])`. A local
                                         // sidesteps it, synced back once at the end.
  foreach_index(i, form_count) {
    AstNode* stmt = ast_get(ast, forms[i]);
    if (stmt->kind == AstNodeKind_List) {
      u16        sc;
      NodeIndex* schildren = ast_seq_children(ast, forms[i], &sc);
      AstNode*   shead     = (sc > 0) ? ast_get(ast, schildren[0]) : NULL;
      if (shead && shead->kind == AstNodeKind_Atom && str8_match_lit("import", shead->token.text, 0)) {
        if (sc != 2 || ast_get(ast, schildren[1])->kind != AstNodeKind_Atom) {
          diag_error(stmt->token, "expected `(import name)`");
        } else {
          String8 name  = ast_get(ast, schildren[1])->token.text;
          b32     known = false;
          foreach_index(j, dyn_count(names)) {
            if (str8_match(names[j], name, 0)) { known = true; break; }
          }
          if (!known) dyn_push(ctx_perm(), names, name);
        }
        continue; // stripped either way -- an import is never a real top-level decl
      }
    }
    dyn_push(ctx_perm(), filtered, forms[i]);
  }
  *import_names = names;
  *out_count    = (u16)dyn_count(filtered);
  return filtered;
}

////////////////////////////////
//~ Output directory
//
// `<root package dir>/output`, not a bare relative "output": generated files
// have to land where build.c goes looking for them, and build.c resolves every
// path it builds against the package directory it was handed. Writing them
// relative to the process's own working directory instead meant that running
// `3b build <dir>` from anywhere but <dir> itself compiled and linked whatever
// stale output/ happened to already sit next to the package, silently, with a
// successful exit status. One run-scoped global rather than a parameter
// threaded through compile_package's recursion: every package in a run,
// imports and embedded packages included, shares the root's output directory.

static String8 g_output_root;

// `<g_output_root>/<name>`, the only way this file names a generated file.
static String8
output_path(Arena* arena, String8 name) {
  return str8f(arena, "%.*s/%.*s", str8_varg(g_output_root), str8_varg(name));
}

static void
ensure_output_dir(void) {
  const char* dir = cstr_from_str8_temp(g_output_root);
  // mingw's mkdir takes one argument -- Windows has no Unix permission bits.
#if defined(_WIN32)
  if (mkdir(dir) != 0 && errno != EEXIST) {
#else
  if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
#endif
    fprintf(stderr, "could not create '%s' directory: %s\n", dir, strerror(errno));
  }
}

////////////////////////////////
//~ The recursive compiler itself

// Matches `path` -- always a full path, as dir_list_files_with_ext produces --
// against every overlay entry by basename; see SourceOverlay in 3b.h for why a
// filename comparison suffices. Returns NULL when `path` isn't overridden,
// which is the usual case.
static String8*
overlay_lookup(const SourceOverlay* overlays, u64 overlay_count, String8 path) {
  String8 base = package_name_from_dir_path(path); // basename extraction, for a file path
  foreach_index(i, overlay_count) {
    if (str8_match(base, package_name_from_dir_path(overlays[i].path), 0)) {
      return (String8*)&overlays[i].content;
    }
  }
  return NULL;
}

// Compiles the package rooted at `dir_path`, and recursively every package it
// transitively imports, memoized and cycle-checked through `*registry`. Writes
// this package's `<name>.h`/`<name>.c` into g_output_root on success -- the
// root package's output directory, not this one's, since an import is compiled
// for whoever pulled it in -- each package exactly once however many importers
// share it. Returns NULL on failure, with a diagnostic already printed.
static PackageBuild*
compile_package(PackageBuild*** registry, String8 dir_path, b32 is_root, PackageKind kind, b32 verbose,
                 const SourceOverlay* overlays, u64 overlay_count, b32 tolerate_check_errors,
                 const ScopeQuery* scope_query) {
  // A real directory always wins over an embedded package of the same name:
  // embedded source is a fallback for "nothing resolved", never a hijack, so a
  // project wanting its own `os`/`vm` can simply have one.
  String8 canonical    = canonicalize_path(dir_path);
  String8 embedded_src = {0};
  b32     is_embedded  = false;
  if (canonical.size == 0 && !is_root) {
    String8 attempted_name = package_name_from_dir_path(dir_path); // pure string op -- safe on a
                                                                       // path that resolves to nothing
    if (known_embedded_native_package_source(attempted_name, &embedded_src)) {
      is_embedded = true;
      // Distinct from any real canonicalize_path result, which is always an
      // absolute path, so memoization and cycle detection work unchanged.
      canonical = str8f(ctx_perm(), "<embedded:%.*s>", str8_varg(attempted_name)); // ctx_perm(), not
                                                                       // scratch -- this outlives
                                                                       // the whole compile
    }
  }
  if (canonical.size == 0) {
    if (verbose) fprintf(stderr, "'%.*s': can't resolve this package directory\n", str8_varg(dir_path));
    return NULL;
  }

  PackageBuild* existing = find_package_build(*registry, canonical);
  if (existing) {
    if (existing->in_progress) {
      if (verbose) fprintf(stderr, "'%.*s': import cycle detected (this package (transitively) imports itself)\n",
                            str8_varg(dir_path));
      return NULL;
    }
    return existing; // already compiled
  }

  String8 pkg_name = is_embedded ? package_name_from_dir_path(dir_path) : package_name_from_dir_path(canonical);
  if (pkg_name.size == 0) {
    if (verbose) fprintf(stderr, "'%.*s': can't determine a package name from this path\n", str8_varg(dir_path));
    return NULL;
  }
  PackageBuild* name_collision = find_package_build_by_name(*registry, pkg_name);
  if (name_collision) {
    if (verbose) fprintf(stderr,
            "package name `%.*s` is used by two different directories: '%.*s' and '%.*s'\n",
            str8_varg(pkg_name), str8_varg(name_collision->dir_path), str8_varg(dir_path));
    return NULL;
  }

  PackageBuild* pb = push_one(ctx_perm(), PackageBuild);
  *pb = (PackageBuild){0};
  pb->canonical_dir = canonical;
  pb->dir_path      = dir_path;
  pb->pkg_name      = pkg_name;
  pb->in_progress   = true;
  pb->is_root       = is_root;
  pb->kind          = kind; // meaningful only when is_root
  PackageBuild** reg = *registry; // see strip_import_forms for why dyn_push needs a plain lvalue
  dyn_push(ctx_perm(), reg, pb);
  *registry = reg;

  u64      file_count;
  String8* files;
  if (is_embedded) {
    // One synthetic "file": never opened, only a label for
    // source_file_register and the diagnostics keyed off it.
    file_count = 1;
    files      = push_array(ctx_perm(), String8, 1);
    files[0]   = str8f(ctx_perm(), "<embedded %.*s>", str8_varg(pkg_name));
  } else {
    file_count = 0;
    files      = dir_list_files_with_ext(ctx_perm(), dir_path, ".3b", &file_count);
    // `*.cfg.3b` -- translate/config.c's translator config, build.c's
    // build.cfg.3b manifest -- share the `.3b` lexer and parser but are never
    // package source. Filtering here rather than in each DSL reader keeps a
    // stray `(headers ...)`/`(binary ...)` form from being silently absorbed
    // as a top-level package statement.
    u64 real_file_count = 0;
    foreach_index(i, file_count) {
      if (str8_ends_with(files[i], str8_lit(".cfg.3b"), 0)) continue;
      files[real_file_count] = files[i];
      real_file_count += 1;
    }
    file_count = real_file_count;
    if (file_count == 0) {
      if (verbose) fprintf(stderr, "'%.*s': no `.3b` files found\n", str8_varg(dir_path));
      return NULL;
    }
  }

  b32 timing = getenv("3B_TIMING") != NULL;
  Time t_parse_start = time_now();

  Ast ast;
  ast_init(&ast, ctx_perm());

  NodeIndex* combined    = NULL;
  String8*   import_names = NULL;
  b32        had_error   = false;
  foreach_index(i, file_count) {
    String8* overlay = overlay_lookup(overlays, overlay_count, files[i]);
    String8  src      = is_embedded ? embedded_src
                       : (overlay ? str8_copy(ctx_perm(), *overlay) : file_load_str8(ctx_perm(), files[i]));
    if (src.size == 0) { had_error = true; continue; }
    u32 file_id = source_file_register(files[i], src);

    Parser p;
    parser_init(&p, src, &ast, file_id);
    NodeIndex root = parse_program(&p);
    if (p.had_error) { had_error = true; continue; }

    NodeIndex* forms;
    u16        form_count;
    if (!validate_and_strip_package_form(&ast, root, file_id, pkg_name, &forms, &form_count)) {
      had_error = true;
      continue;
    }
    u16        filtered_count;
    NodeIndex* filtered = strip_import_forms(&ast, forms, form_count, &filtered_count, &import_names);
    foreach_index(j, filtered_count) { dyn_push(ctx_perm(), combined, filtered[j]); }
  }

  if (had_error) {
    if (verbose) fprintf(stderr, "package '%.*s': compilation failed (see errors above)\n", str8_varg(pkg_name));
    return NULL;
  }
  if (timing) fprintf(stderr, "[timing] %.*s: parse    %.3fms (%llu nodes)\n",
                       str8_varg(pkg_name), duration_milliseconds(time_since(t_parse_start)),
                       (unsigned long long)dyn_count(ast.nodes));

  // Imports are resolved and compiled before this package's own forms are
  // lowered: the Lowerer needs each import's public struct/enum/alias names
  // seeded up front (seed_lowerer_with_import) for slash-qualified enum-access
  // and construction syntax to resolve.
  PackageBuild** deps = NULL;
  foreach_index(i, dyn_count(import_names)) {
    String8       import_dir = str8_join_path(dir_path, import_names[i]);
    PackageBuild* dep        = compile_package(registry, import_dir, false, PackageKind_Binary, verbose,
                                                overlays, overlay_count, tolerate_check_errors, scope_query);
    if (!dep) {
      if (verbose) fprintf(stderr, "package '%.*s': failed to import '%.*s' (see errors above)\n",
                            str8_varg(pkg_name), str8_varg(import_names[i]));
      return NULL;
    }
    dyn_push(ctx_perm(), deps, dep);
    dyn_push(ctx_perm(), pb->imported_pkg_names, dep->pkg_name);
  }

  TypedAst* tast = push_one(ctx_perm(), TypedAst);
  typed_ast_init(tast, ctx_perm());
  Lowerer low = {0};
  low.ast     = &ast;
  low.tast    = tast;
  foreach_index(i, dyn_count(deps)) { seed_lowerer_with_import(&low, deps[i]); }

  Token synth_open = {0};
  synth_open.line  = 1;
  synth_open.col   = 1;
  u64 combined_count = dyn_count(combined);
  if (combined_count > max_u16) {
    diag_error(synth_open, "package '%.*s' has too many combined top-level forms (%llu > %u)",
               str8_varg(pkg_name), (unsigned long long)combined_count, (u32)max_u16);
    if (verbose) fprintf(stderr, "package '%.*s': compilation failed (see errors above)\n", str8_varg(pkg_name));
    return NULL;
  }
  NodeIndex combined_ast_root = ast_push_seq(&ast, AstNodeKind_List, synth_open, combined, (u16)combined_count);

  Time t_lower_start = time_now();
  TypedIndex own_root = lower_program(&low, combined_ast_root);
  if (low.had_error) {
    if (verbose) fprintf(stderr, "package '%.*s': lowering failed (see errors above)\n", str8_varg(pkg_name));
    return NULL;
  }
  if (timing) fprintf(stderr, "[timing] %.*s: lower   %.3fms\n",
                       str8_varg(pkg_name), duration_milliseconds(time_since(t_lower_start)));

  // Each import's public surface is spliced in ahead of this package's own
  // forms, so check_program's up-front gathering pass sees it and its
  // global_scope binding pass binds imported globals before any of this
  // package's functions -- which may reference them -- are checked.
  TypedIndex* prepend = NULL;
  foreach_index(i, dyn_count(deps)) {
    PackageBuild* dep     = deps[i];
    TypedNode*    dep_root = &dep->tast->nodes[dep->root];
    foreach_index(j, dep_root->block.stmt_count) {
      TypedIndex src_idx = dep->tast->extra[dep_root->block.stmt_first + j];
      if (src_idx == TYPED_NIL) continue;
      TypedNode* sn = &dep->tast->nodes[src_idx];
      if (sn->is_private || sn->is_imported) continue; // dep's own public surface only -- see
                                                        // compiler.h on transitive re-export
      String8    qualified;
      switch (sn->kind) {
        case TypedNodeKind_FunctionDecl:
          if (sn->func.body == TYPED_NIL) continue; // real `extern` -- not dep's own symbol to re-export
          qualified = qualify_name(dep->pkg_name, sn->func.name);
          break;
        case TypedNodeKind_StructDecl: qualified = qualify_name(dep->pkg_name, sn->struct_decl.name); break;
        case TypedNodeKind_EnumDecl:   qualified = qualify_name(dep->pkg_name, sn->enum_decl.name);   break;
        case TypedNodeKind_AliasDecl:  qualified = qualify_name(dep->pkg_name, sn->alias_decl.name);  break;
        case TypedNodeKind_ConstDecl:  qualified = qualify_name(dep->pkg_name, sn->const_decl.name);  break;
        case TypedNodeKind_VarDecl:    qualified = qualify_name(dep->pkg_name, sn->var_decl.name);    break;
        default: continue;
      }
      TypedIndex spliced = splice_public_decl(tast, dep, src_idx, qualified);
      if (spliced != TYPED_NIL) dyn_push(ctx_perm(), prepend, spliced);
    }
  }

  TypedNode* own_root_n = &tast->nodes[own_root];
  u32 final_first = (u32)dyn_count(tast->extra);
  foreach_index(i, dyn_count(prepend)) { dyn_push(tast->arena, tast->extra, prepend[i]); }
  foreach_index(i, own_root_n->block.stmt_count) {
    dyn_push(tast->arena, tast->extra, tast->extra[own_root_n->block.stmt_first + i]);
  }
  TypedNode final_block      = {0};
  final_block.kind           = TypedNodeKind_Block;
  final_block.token          = own_root_n->token;
  final_block.block.stmt_first = final_first;
  final_block.block.stmt_count = (u16)(dyn_count(prepend) + own_root_n->block.stmt_count);
  TypedIndex final_root = typed_push(tast, final_block);

  // A root package's `fn main` becomes the C entry point only when the package
  // is also declared a binary; in a library it stays an ordinary function.
  // Dependencies are never entry points, whatever their own `kind` says.
  b32 is_entrypoint = pb->is_root && pb->kind == PackageKind_Binary;

  Time t_check_start = time_now();
  Checker ck = check_program(tast, final_root, is_entrypoint, scope_query);
  // Native codegen only: catches two top-level fns -- an `extern` bound to a
  // hand-picked C name and this package's own wrapper, say -- mangling to the
  // same C symbol. It runs here rather than inside check_program because it
  // needs pkg_name and is_entrypoint, which check_program never takes.
  // script.c's bytecode path skips it: bytecode calls resolve by module and
  // index, so no such collision exists there.
  check_mangled_name_collisions(&ck, pkg_name, is_entrypoint);
  pb->had_check_error = ck.had_error;
  if (ck.had_error) {
    if (verbose) fprintf(stderr, "package '%.*s': type checking failed (see errors above)\n", str8_varg(pkg_name));
    if (!tolerate_check_errors) return NULL;
  }
  // `ck` is local; these are copied out for callers that outlive it, such as
  // lib3b.h's hover and scope queries. See PackageBuild in compiler.h.
  pb->resolved_types     = ck.resolved_types;
  pb->scope_query_result = ck.scope_query_result;
  pb->scope_query_count  = ck.scope_query_count;
  pb->ck                 = ck; // full by-value copy
  if (timing) fprintf(stderr, "[timing] %.*s: check   %.3fms\n",
                       str8_varg(pkg_name), duration_milliseconds(time_since(t_check_start)));

  // Non-verbose mode is check-only: `pb->tast` and `pb->root` are already
  // populated above, so a caller that only wants diagnostics (lib3b.h) never
  // touches output/ or pays for C emission.
  if (verbose) {
    String8 mangled_pkg    = c_mangle_name(ctx_perm(), pkg_name);
    String8 h_path          = output_path(ctx_perm(), str8f(ctx_perm(), "%.*s.h", str8_varg(mangled_pkg)));
    String8 c_path          = output_path(ctx_perm(), str8f(ctx_perm(), "%.*s.c", str8_varg(mangled_pkg)));
    const char* h_path_cstr = cstr_from_str8_temp(h_path);
    const char* c_path_cstr = cstr_from_str8_temp(c_path);

    FILE* h_out = fopen(h_path_cstr, "w");
    FILE* c_out = fopen(c_path_cstr, "w");
    if (!h_out || !c_out) {
      fprintf(stderr, "could not open %s/%s for writing\n", h_path_cstr, c_path_cstr);
      if (h_out) fclose(h_out);
      if (c_out) fclose(c_out);
      return NULL;
    }

    Codegen cg = {0};
    cg.tast               = tast;
    cg.resolved_types     = ck.resolved_types;
    cg.structs            = ck.structs;
    cg.fns_by_name        = ck.fns_by_name;
    cg.package_name       = pkg_name;
    cg.imported_pkg_names = pb->imported_pkg_names;
    cg.is_root_package    = is_entrypoint;
    cg.has_parallel       = ck.has_parallel;

    Time t_codegen_start = time_now();
    cg.out = h_out;
    cg_program_header(&cg, final_root);
    fclose(h_out);

    cg.out = c_out;
    cg_program(&cg, final_root);
    fclose(c_out);
    // Codegen.had_error means a construct was diagnosed in 3b terms already.
    // Stopping here keeps the C compiler from reporting the same problem a
    // second time, in terms of the generated file, under the 3b error.
    if (cg.had_error) {
      if (verbose) fprintf(stderr, "package '%.*s': code generation failed (see errors above)\n",
                            str8_varg(pkg_name));
      return NULL;
    }
    if (timing) fprintf(stderr, "[timing] %.*s: codegen %.3fms\n",
                         str8_varg(pkg_name), duration_milliseconds(time_since(t_codegen_start)));

    // PROTOTYPE: re-runs codegen twice in memory, never touching the .h/.c
    // written above, once serially and once through cg_program_parallel, to
    // measure whether base/base.h's fork-join lane system is worth wiring in.
    if (getenv("3B_BENCH_CODEGEN")) {
      FILE* smem = mem_stream_open();
      Codegen cg_s          = {0};
      cg_s.tast             = tast;
      cg_s.resolved_types   = ck.resolved_types;
      cg_s.structs          = ck.structs;
      cg_s.fns_by_name      = ck.fns_by_name;
      cg_s.package_name     = pkg_name;
      cg_s.imported_pkg_names = pb->imported_pkg_names;
      cg_s.is_root_package  = is_entrypoint;
      cg_s.has_parallel     = ck.has_parallel;
      cg_s.out              = smem;
      Time t_s = time_now();
      cg_program(&cg_s, final_root);
      char* sbuf; u64 ssz;
      mem_stream_close(smem, &sbuf, &ssz);
      Duration serial_dt = time_since(t_s);

      FILE* pmem = mem_stream_open();
      Codegen cg_p          = {0};
      cg_p.tast             = tast;
      cg_p.resolved_types   = ck.resolved_types;
      cg_p.structs          = ck.structs;
      cg_p.fns_by_name      = ck.fns_by_name;
      cg_p.package_name     = pkg_name;
      cg_p.imported_pkg_names = pb->imported_pkg_names;
      cg_p.is_root_package  = is_entrypoint;
      cg_p.has_parallel     = ck.has_parallel;
      cg_p.out              = pmem;
      Time t_p = time_now();
      cg_program_parallel(&cg_p, final_root);
      char* pbuf; u64 psz;
      mem_stream_close(pmem, &pbuf, &psz);
      Duration parallel_dt = time_since(t_p);

      b32 match = (ssz == psz) && (memcmp(sbuf, pbuf, ssz) == 0);
      fprintf(stderr,
              "[bench-codegen] %.*s: serial %.3fms, parallel %.3fms, speedup %.2fx, output %s (%llu vs %llu bytes)\n",
              str8_varg(pkg_name), duration_milliseconds(serial_dt), duration_milliseconds(parallel_dt),
              duration_milliseconds(serial_dt) / duration_milliseconds(parallel_dt),
              match ? "MATCH" : "MISMATCH", ssz, psz);
      free(sbuf);
      free(pbuf);
    }

    printf("package '%.*s': compiled %llu file(s)%s%s -> %s, %s\n",
           str8_varg(pkg_name), (unsigned long long)file_count,
           dyn_count(deps) > 0 ? ", imports: " : "",
           dyn_count(deps) > 0 ? "(see above)" : "",
           h_path_cstr, c_path_cstr);
  }

  pb->tast        = tast;
  pb->root        = final_root;
  pb->in_progress = false;
  return pb;
}

PackageBuild*
compile_all_packages(const char* dir_path_cstr, PackageBuild*** out_registry,
                     PackageKind root_kind, b32 verbose,
                     const SourceOverlay* overlays, u64 overlay_count,
                     b32 tolerate_check_errors, const ScopeQuery* scope_query) {
  b32 bench_codegen = getenv("3B_BENCH_CODEGEN") != NULL;
  if (bench_codegen) { os_state_init(); async_threads_init(); } // PROTOTYPE -- see cg_program_parallel

  String8 dir_path = str8_cstring((char*)dir_path_cstr);

  // Built off the caller's own dir_path, exactly as build.c builds its half, so
  // the two always name the same directory. Trailing slashes come off first --
  // `<dir>//output` resolves the same but reads badly in the compiled-to line.
  String8 root_dir = dir_path;
  while (root_dir.size > 1 && char_is_slash(root_dir.str[root_dir.size - 1])) root_dir.size -= 1;
  g_output_root = str8f(ctx_perm(), "%.*s/" OUTPUT_DIR, str8_varg(root_dir));

  // A dir_path that isn't a directory at all falls through untouched, so
  // compile_package below reports it rather than this producing a confusing
  // "could not create output" first.
  struct stat root_st;
  b32         root_is_dir = stat(dir_path_cstr, &root_st) == 0 && S_ISDIR(root_st.st_mode);

  if (verbose && root_is_dir) {
    ensure_output_dir();

    // Shared runtime support: package-agnostic, written once up front however
    // many packages this run compiles.
    String8 runtime_h_path = output_path(ctx_perm(), str8_lit("3b_runtime.h"));
    String8 runtime_c_path = output_path(ctx_perm(), str8_lit("3b_runtime.c"));
    FILE*   runtime_h      = fopen(cstr_from_str8_temp(runtime_h_path), "w");
    FILE*   runtime_c      = fopen(cstr_from_str8_temp(runtime_c_path), "w");
    if (!runtime_h || !runtime_c) {
      fprintf(stderr, "could not open %.*s/3b_runtime.h/.c for writing\n", str8_varg(g_output_root));
      if (runtime_h) fclose(runtime_h);
      if (runtime_c) fclose(runtime_c);
      return NULL;
    }
    cg_write_runtime_header(runtime_h);
    cg_write_runtime_source(runtime_c);
    fclose(runtime_h);
    fclose(runtime_c);
  }

  PackageBuild** registry = NULL;
  PackageBuild*  root_pkg = compile_package(&registry, dir_path, true, root_kind, verbose, overlays, overlay_count,
                                              tolerate_check_errors, scope_query);

  if (bench_codegen) async_threads_shutdown();
  if (out_registry) *out_registry = registry;
  return root_pkg;
}
