// lib3b.c -- see lib3b.h. A wrapper around compile_all_packages in its
// check-only mode (verbose=false), with diag.c's capture API standing in for
// stderr printing, plus the hover, goto-definition and completion queries
// layered on top of the resulting tree.
#include "lib3b.h"
#include "build.h"
#include "compiler.h"
#include "bcgen.h"
#include "bcvm.h"
#include <stdio.h> // open_memstream, for lib3b_format
#include <stdlib.h>
#include <string.h>

// Deep-copies a String8 out of the about-to-be-destroyed arena into a malloc'd,
// NUL-terminated buffer -- see lib3b.h on why Diagnostic's fields cross to
// plain malloc at this boundary.
static String8
str8_heap_copy(String8 s) {
  String8 out = {0};
  out.str  = malloc(s.size + 1);
  out.size = s.size;
  memcpy(out.str, s.str, s.size);
  out.str[s.size] = 0;
  return out;
}

static char*
str8_to_cstr(String8 s) {
  char* out = malloc(s.size + 1);
  memcpy(out, s.str, s.size);
  out[s.size] = 0;
  return out;
}

// Every malloc'd field of one symbol, freed in one place -- Lib3bSymbol arrays
// are torn down from four sites (a check result's own symbols and each
// import's, plus a completion context's symbols and locals), and a field added
// to the struct without all four learning about it leaks silently.
static void
lib3b_symbol_free_fields(Lib3bSymbol* s) {
  free(s->name);
  free(s->file_path);
  free(s->detail);
  free(s->doc);
}

// Defined further down, with the rest of the hover machinery they were written
// for, and forward-declared here because collect_toplevel_symbols wants the
// same signature line and the same doc comment for a completion item that
// hover shows in its popup -- the point being that the two never disagree.
static String8 format_function_signature(TypedAst* tast, TypedNode* fn);
static String8 format_struct_signature(TypedAst* tast, TypedNode* sd);
static String8 format_enum_signature(TypedAst* tast, TypedNode* ed);
static String8 doc_comment_above(String8 text, u32 line);

// Every top-level fn/struct/enum/const/var/alias in `tast`'s own package --
// the same top-level walk find_toplevel_decl (below) does for goto-definition,
// collecting instead of searching. `include_private` is true for the root
// package's own completion cache, since privacy doesn't apply to completion
// within your own source, and false for a direct import's public surface, which
// additionally skips `is_imported` nodes -- the same `is_private ||
// is_imported` filter compiler.c applies when splicing an import surface, for
// the same reason: re-exporting a transitively-imported symbol isn't supported.
static void
collect_toplevel_symbols(TypedAst* tast, TypedIndex root, b32 include_private,
                          Lib3bSymbol** out_symbols, u64* out_count) {
  TypedNode*   root_n = &tast->nodes[root];
  Lib3bSymbol* syms    = NULL;
  u64          count    = 0, cap = 0;
  foreach_index(j, root_n->block.stmt_count) {
    TypedIndex idx = tast->extra[root_n->block.stmt_first + j];
    if (idx == TYPED_NIL) continue;
    TypedNode* n = &tast->nodes[idx];
    if (!include_private && (n->is_private || n->is_imported)) continue;
    String8         name = {0};
    Lib3bSymbolKind kind;
    switch (n->kind) {
      case TypedNodeKind_FunctionDecl: name = n->func.name;        kind = Lib3bSymbol_Function; break;
      case TypedNodeKind_StructDecl:   name = n->struct_decl.name; kind = Lib3bSymbol_Struct;   break;
      case TypedNodeKind_EnumDecl:     name = n->enum_decl.name;   kind = Lib3bSymbol_Enum;     break;
      case TypedNodeKind_ConstDecl:    name = n->const_decl.name;  kind = Lib3bSymbol_Const;    break;
      case TypedNodeKind_VarDecl:      name = n->var_decl.name;    kind = Lib3bSymbol_Var;      break;
      case TypedNodeKind_AliasDecl:    name = n->alias_decl.name;  kind = Lib3bSymbol_Alias;    break;
      default: continue;
    }
    if (count == cap) {
      cap  = cap ? cap * 2 : 8;
      syms = realloc(syms, cap * sizeof(Lib3bSymbol));
    }
    // Only the three declaration kinds with a shape worth spelling out get a
    // detail line; a const/var/alias's own name plus its kind is already the
    // whole story a completion list has room for.
    String8 detail = {0};
    switch (n->kind) {
      case TypedNodeKind_FunctionDecl: detail = format_function_signature(tast, n); break;
      case TypedNodeKind_StructDecl:   detail = format_struct_signature(tast, n);   break;
      case TypedNodeKind_EnumDecl:     detail = format_enum_signature(tast, n);     break;
      default: break;
    }
    SourceFile* sf   = source_file_get(n->token.file_id);
    String8     doc = doc_comment_above(sf->text, n->token.line);
    syms[count].name      = str8_to_cstr(name);
    syms[count].kind      = kind;
    syms[count].file_path = str8_to_cstr(sf->path);
    syms[count].line      = n->token.line;
    syms[count].col       = n->token.col;
    syms[count].detail    = detail.size ? str8_to_cstr(detail) : NULL;
    syms[count].doc       = doc.size ? str8_to_cstr(doc) : NULL;
    count += 1;
  }
  *out_symbols = syms;
  *out_count   = count;
}

// Converts a ScopeQuery snapshot (Checker.scope_query_result -- every
// ScopeEntry visible at the query position, in binding order) into a malloc'd
// Lib3bSymbol array. Two filters, both applied while walking the snapshot
// backwards: shadowing dedup, since an entry nearer the end was bound later and
// so more deeply nested, matching scope_lookup_entry's reverse-scan precedence
// exactly -- the first occurrence hit walking backwards is the one that would
// actually resolve -- and "already in `existing`" dedup against
// collect_toplevel_symbols' output, since a function body's scope still
// contains every top-level const/var declared before it in the same file
// (check_program uses one shared global_scope), which would otherwise list each
// such name twice.
static void
collect_scope_locals(ScopeEntry* entries, u64 entry_count,
                      Lib3bSymbol* existing, u64 existing_count,
                      Lib3bSymbol** out_locals, u64* out_count) {
  Lib3bSymbol* locals = NULL;
  u64          count = 0, cap = 0;
  for (u64 ri = 0; ri < entry_count; ri += 1) {
    u64     i    = entry_count - 1 - ri;
    String8 name = entries[i].name;
    b32     dup  = false;
    foreach_index(j, existing_count) {
      if (str8_match_cstr(existing[j].name, name, 0)) { dup = true; break; }
    }
    for (u64 j = 0; !dup && j < count; j += 1) {
      if (str8_match_cstr(locals[j].name, name, 0)) dup = true;
    }
    if (dup) continue;

    if (count == cap) { cap = cap ? cap * 2 : 8; locals = realloc(locals, cap * sizeof(Lib3bSymbol)); }
    locals[count].name   = str8_to_cstr(name);
    locals[count].kind   = Lib3bSymbol_Var;
    locals[count].detail = NULL; // a param or `let`-local has no declaration header to show, and
    locals[count].doc    = NULL; // nobody writes a comment block above one
    Token decl = entries[i].decl_token;
    if (decl.file_id != SOURCE_FILE_UNKNOWN) {
      SourceFile* sf           = source_file_get(decl.file_id);
      locals[count].file_path = str8_to_cstr(sf->path);
      locals[count].line      = decl.line;
      locals[count].col       = decl.col;
    } else {
      locals[count].file_path = NULL;
      locals[count].line      = 0;
      locals[count].col       = 0;
    }
    count += 1;
  }
  *out_locals = locals;
  *out_count  = count;
}

// Every direct import's public surface -- see Lib3bImportedPackage (lib3b.h)
// for why this never recurses into transitive imports. `registry` is the full
// package graph compile_all_packages compiled, root plus every dependency at
// any depth; each of `root_pkg`'s `imported_pkg_names` is looked up in it by
// name to find that dependency's already-checked PackageBuild.
static void
collect_imports(PackageBuild* root_pkg, PackageBuild** registry, u64 registry_count,
                 Lib3bImportedPackage** out_imports, u64* out_count) {
  u64                    import_count = dyn_count(root_pkg->imported_pkg_names);
  Lib3bImportedPackage* imports       = import_count ? malloc(import_count * sizeof(Lib3bImportedPackage)) : NULL;
  foreach_index(i, import_count) {
    String8 want = root_pkg->imported_pkg_names[i];
    imports[i]   = (Lib3bImportedPackage){0};
    imports[i].name = str8_to_cstr(want);
    foreach_index(j, registry_count) {
      if (!str8_match(registry[j]->pkg_name, want, 0)) continue;
      collect_toplevel_symbols(registry[j]->tast, registry[j]->root, /*include_private=*/false,
                                &imports[i].public_symbols, &imports[i].public_symbol_count);
      break;
    }
  }
  *out_imports = imports;
  *out_count   = import_count;
}

Lib3bCheckResult
lib3b_check_package_with_overlays(const char* dir_path_cstr, const SourceOverlay* overlays, u64 overlay_count) {
  Lib3bCheckResult result = {0};

  Context ctx;
  ctx_init(&ctx, MB(16));
  source_registry_reset();

  PackageKind root_kind = build_config_read_kind(ctx_perm(), dir_path_cstr);

  diag_capture_begin(/*also_print=*/false);
  PackageBuild** registry = NULL;
  PackageBuild*  root_pkg = compile_all_packages(dir_path_cstr, &registry, root_kind, /*verbose=*/false,
                                                  overlays, overlay_count, /*tolerate_check_errors=*/false,
                                                  /*scope_query=*/NULL);
  u64         count      = 0;
  Diagnostic* diags       = diag_capture_end(&count);

  result.ok               = root_pkg != NULL;
  result.diagnostic_count = count;
  if (count > 0) {
    result.diagnostics = malloc(count * sizeof(Diagnostic));
    foreach_index(i, count) {
      result.diagnostics[i].message   = str8_heap_copy(diags[i].message);
      result.diagnostics[i].file_path = str8_heap_copy(diags[i].file_path);
      result.diagnostics[i].line      = diags[i].line;
      result.diagnostics[i].col       = diags[i].col;
    }
  }
  if (root_pkg) {
    collect_toplevel_symbols(root_pkg->tast, root_pkg->root, /*include_private=*/true,
                              &result.symbols, &result.symbol_count);
    collect_imports(root_pkg, registry, dyn_count(registry), &result.imports, &result.import_count);
  }

  ctx_free();
  return result;
}

Lib3bCheckResult
lib3b_check_package(const char* dir_path_cstr) {
  return lib3b_check_package_with_overlays(dir_path_cstr, NULL, 0);
}

void
lib3b_free_result(Lib3bCheckResult* result) {
  if (!result) return;
  foreach_index(i, result->diagnostic_count) {
    free(result->diagnostics[i].message.str);
    free(result->diagnostics[i].file_path.str);
  }
  free(result->diagnostics);
  foreach_index(i, result->symbol_count) lib3b_symbol_free_fields(&result->symbols[i]);
  free(result->symbols);
  foreach_index(i, result->import_count) {
    free(result->imports[i].name);
    foreach_index(j, result->imports[i].public_symbol_count) {
      lib3b_symbol_free_fields(&result->imports[i].public_symbols[j]);
    }
    free(result->imports[i].public_symbols);
  }
  free(result->imports);
  MemoryZeroStruct(result);
}

////////////////////////////////
//~ Queries -- see lib3b.h on why these are one-shot (compile fully, answer,
// tear down) rather than a persistent session.

// Shared by lib3b_hover/lib3b_definition: runs the same one-shot compile
// lib3b_check_package_with_overlays does, silencing diagnostics a query caller doesn't
// want, and leaves `*ctx` alive so the caller can read pb->tast and
// pb->resolved_types before tearing it down. Returns NULL, with `*ctx` still
// needing ctx_free(), if the package failed to compile: compile_package sets
// tast/root/resolved_types only on success, so a package with even one type
// error anywhere has nothing partial to query.
static PackageBuild*
compile_for_query(Context* ctx, const char* dir_path_cstr, const SourceOverlay* overlays, u64 overlay_count) {
  ctx_init(ctx, MB(16));
  source_registry_reset();
  PackageKind root_kind = build_config_read_kind(ctx_perm(), dir_path_cstr);
  diag_capture_begin(/*also_print=*/false);
  PackageBuild* root_pkg = compile_all_packages(dir_path_cstr, NULL, root_kind, /*verbose=*/false,
                                                 overlays, overlay_count, /*tolerate_check_errors=*/false,
                                                 /*scope_query=*/NULL);
  diag_capture_end(NULL);
  return root_pkg;
}

static String8
str8_basename(String8 path) {
  u64 after_slash = str8_find_needle_reverse(path, 0, str8_lit("/"), 0);
  return str8_skip(path, after_slash);
}

// A typed node's `.token` is where its source form starts. For most kinds
// (Identifier, IntLiteral, EnumAccess) that is exactly the name or literal text
// an LSP query wants to match against, but for list-shaped forms it is the
// opening paren instead: `(helper 1 2)` and `(Vector2 {:x 1})` both set
// `.token` to `(`'s position (see lower_call/lower_struct_construct), one or
// more characters before "helper"/"Vector2" starts. Those two kinds carry a
// second, more specific token for exactly this reason -- use it when present.
static Token
query_token_for(TypedNode* n) {
  switch (n->kind) {
    case TypedNodeKind_Call:          return n->call.callee_token;
    case TypedNodeKind_StructLiteral: return n->struct_lit.type_name_token;
    // Every hop of `(. a b c)` shares the whole form's token, so a hop can only be told apart by
    // where its field name was written. Synthesized accesses (destructuring) have no such token
    // and fall back to the node's own, which is where they were written from.
    case TypedNodeKind_FieldAccess:
      return n->field_access.field_token.file_id != SOURCE_FILE_UNKNOWN ? n->field_access.field_token
                                                                       : n->token;
    default:                          return n->token;
  }
}

// Linear scan over every typed node for the one whose query-relevant token
// spans (line, col) in the named file, matched by basename -- see SourceOverlay
// (3b.h) for why a filename comparison suffices within one already-resolved
// package directory. Package sizes make a linear scan fine; no spatial index is
// worth building. Prefers the smallest matching span, so a token nested inside
// a larger one wins over its container.
static TypedIndex
find_node_at_position(TypedAst* tast, const char* file_path_cstr, u32 line, u32 col) {
  String8    target    = str8_basename(str8_cstring((char*)file_path_cstr));
  TypedIndex best       = TYPED_NIL;
  u32        best_span = 0xFFFFFFFFu;
  u64        count      = dyn_count(tast->nodes);
  foreach_index(i, count) {
    if (tast->nodes[i].kind == TypedNodeKind_Nil) continue; // a node the checker retired -- see the
                                                             // DotHop case in check_expr
    Token tok = query_token_for(&tast->nodes[i]);
    if (tok.line != line) continue;
    SourceFile* sf = source_file_get(tok.file_id);
    if (!str8_match(str8_basename(sf->path), target, 0)) continue;
    u32 span = (u32)tok.text.size;
    if (span == 0) span = 1;
    if (col < tok.col || col >= tok.col + span) continue;
    if (span < best_span) { best = (TypedIndex)i; best_span = span; }
  }
  return best;
}

// The same smallest-span linear scan as find_node_at_position, but over
// TypedAst.type_annotations instead of .nodes: a type annotation (a
// param/field/var/return type, sizeof/zero/handle-alloc's type argument) is not
// a TypedNode itself (see TypeAnnotation, 3b.h) and needs its own position
// index. Spans here never overlap a TypedNode's query token, so falling back to
// this after find_node_at_position comes up empty never masks a real match.
static TypeRef*
find_type_annotation_at_position(TypedAst* tast, const char* file_path_cstr, u32 line, u32 col) {
  String8  target    = str8_basename(str8_cstring((char*)file_path_cstr));
  TypeRef* best       = NULL;
  u32      best_span = 0xFFFFFFFFu;
  u64      count      = dyn_count(tast->type_annotations);
  foreach_index(i, count) {
    TypeAnnotation* ann = &tast->type_annotations[i];
    if (ann->token.line != line) continue;
    SourceFile* sf = source_file_get(ann->token.file_id);
    if (!str8_match(str8_basename(sf->path), target, 0)) continue;
    u32 span = (u32)ann->token.text.size;
    if (span == 0) span = 1;
    if (col < ann->token.col || col >= ann->token.col + span) continue;
    if (span < best_span) { best = &ann->type; best_span = span; }
  }
  return best;
}

// Unwraps `T*`/`T**` to arbitrary pointer depth, then returns the base's
// declared name if it names a user type: TypeKind_Named (a struct or enum) or
// TypeKind_Handle (a `T^`, which always names a struct). Empty String8 for
// anything else -- a primitive, `nil`'s wildcard pointer, Vector/Map/Set/Array/
// Fn. This is what lets goto-definition on a pointer or handle annotation land
// on the same declaration a bare named type would.
static String8
type_ref_named_target(TypeRef t) {
  while (t.kind == TypeKind_Pointer && t.pointee) t = *t.pointee;
  if (t.kind == TypeKind_Named || t.kind == TypeKind_Handle) return t.name;
  return (String8){0};
}

// Name -> declaring node, top-level only (fn/struct/enum) -- the same walk over
// TypedAst.extra[block.stmt_first..] compiler.c does when splicing, just
// reading instead. checker.c's tables (fns_by_name and friends) aren't needed:
// the tree itself already has everything, and those live inside a Checker that
// doesn't escape compile_package.
static TypedIndex
find_toplevel_decl(TypedAst* tast, TypedIndex root, String8 name) {
  TypedNode* root_n = &tast->nodes[root];
  foreach_index(j, root_n->block.stmt_count) {
    TypedIndex idx = tast->extra[root_n->block.stmt_first + j];
    if (idx == TYPED_NIL) continue;
    TypedNode* n = &tast->nodes[idx];
    String8    decl_name = {0};
    switch (n->kind) {
      case TypedNodeKind_FunctionDecl: decl_name = n->func.name;        break;
      case TypedNodeKind_StructDecl:   decl_name = n->struct_decl.name; break;
      case TypedNodeKind_EnumDecl:     decl_name = n->enum_decl.name;   break;
      default: continue;
    }
    if (str8_match(decl_name, name, 0)) return idx;
  }
  return TYPED_NIL;
}

// The struct or union a field access lands on: the base's resolved type with
// any pointer levels peeled, since `.` derefs one automatically and `get`
// requires it spelled out -- either way what is left names the struct.
// TYPED_NIL for a Map hop the checker rewrote, or a base that failed to check.
static TypedIndex
find_field_owner_decl(PackageBuild* pb, TypedIndex base_idx) {
  if (base_idx == TYPED_NIL || !pb->resolved_types) return TYPED_NIL;
  String8 name = type_ref_named_target(pb->resolved_types[base_idx]);
  if (name.size == 0) return TYPED_NIL;
  TypedIndex decl_idx = find_toplevel_decl(pb->tast, pb->root, name);
  if (decl_idx == TYPED_NIL) return TYPED_NIL;
  return pb->tast->nodes[decl_idx].kind == TypedNodeKind_StructDecl ? decl_idx : TYPED_NIL;
}

// The Param a field name resolves to on `decl_idx`, mirroring checker.c's
// find_field_recursive: declared fields first, then a second pass through
// anonymous (`_`) members, whose fields are reachable with no extra path
// segment. NULL when the field is not there -- a checker error the query side
// just skips over rather than reports.
//
// `*io_decl_idx` is updated to whichever struct actually declared the field,
// which for an anonymous member is not the one the access was written against.
static Param*
find_field_param(PackageBuild* pb, TypedIndex* io_decl_idx, String8 field) {
  TypedNode* decl = &pb->tast->nodes[*io_decl_idx];
  foreach_index(j, decl->struct_decl.field_count) {
    Param* f = &pb->tast->params[decl->struct_decl.field_first + j];
    if (!f->is_anon && str8_match(f->name, field, 0)) return f;
  }
  foreach_index(j, decl->struct_decl.field_count) {
    Param* f = &pb->tast->params[decl->struct_decl.field_first + j];
    if (!f->is_anon || f->type.kind != TypeKind_Named) continue;
    TypedIndex nested = find_toplevel_decl(pb->tast, pb->root, f->type.name);
    if (nested == TYPED_NIL || pb->tast->nodes[nested].kind != TypedNodeKind_StructDecl) continue;
    Param* found = find_field_param(pb, &nested, field);
    if (found) { *io_decl_idx = nested; return found; } // `nested` may have been pushed deeper still
  }
  return NULL;
}

// "[name1 type1 name2 type2 ...]" -- shared by function params and struct
// fields, which are the same Param{name,type} array; struct fields index
// TypedAst.params too (see struct_decl in 3b.h).
static String8
format_param_list(TypedAst* tast, u32 first, u16 count) {
  String8 out = str8_lit("[");
  foreach_index(i, count) {
    Param* p = &tast->params[first + i];
    if (i > 0) out = str8_cat(ctx_perm(), out, str8_lit(" "));
    out = str8_cat(ctx_perm(), out, p->name);
    out = str8_cat(ctx_perm(), out, str8_lit(" "));
    out = str8_cat(ctx_perm(), out, type_ref_display(ctx_scratch(), p->type));
  }
  out = str8_cat(ctx_perm(), out, str8_lit("]"));
  return out;
}

// "fn name [params] rettype" -- the declaration header, source syntax,
// body omitted -- for hovering over a call's callee.
static String8
format_function_signature(TypedAst* tast, TypedNode* fn) {
  String8 out = str8_lit("fn ");
  out = str8_cat(ctx_perm(), out, fn->func.name);
  out = str8_cat(ctx_perm(), out, str8_lit(" "));
  out = str8_cat(ctx_perm(), out, format_param_list(tast, fn->func.param_first, fn->func.param_count));
  out = str8_cat(ctx_perm(), out, str8_lit(" "));
  out = str8_cat(ctx_perm(), out, type_ref_display(ctx_scratch(), fn->func.return_type));
  return out;
}

// Builtins have no declaration anywhere for a hover or a completion item to
// point at -- check_expr dispatches them by name (checker.c) -- so the ones
// whose accepted call shapes aren't obvious from the name carry a hand-written
// display here. One line per shape, in the same "form then result type" order
// format_function_signature uses.
//
// Only print/println are listed: an optional leading `stream` is the one
// builtin overload a reader can't guess, and a wrong guess is silent, since
// `(print f "x")` without stream support would just be a template argument
// count error. Every other builtin still hovers as its resolved type.
typedef struct BuiltinShapes {
  const char* name;
  const char* shapes;
} BuiltinShapes;

static const BuiltinShapes BUILTIN_SHAPES[] = {
  { "print",   "(print \"template {}\" values...) void\n"
               "(print stream \"template {}\" values...) void" },
  { "println", "(println \"template {}\" values...) void\n"
               "(println stream \"template {}\" values...) void" },
};

static const char*
builtin_shapes_for(String8 name) {
  foreach_index(i, ArrayCount(BUILTIN_SHAPES)) {
    if (str8_match_cstr((char*)BUILTIN_SHAPES[i].name, name, 0)) return BUILTIN_SHAPES[i].shapes;
  }
  return NULL;
}

const char*
lib3b_builtin_shapes(const char* name) {
  return name ? builtin_shapes_for(str8_cstring((char*)name)) : NULL;
}

// "struct name [fields]" (or "union") -- for hovering over a struct
// construction's type name.
static String8
format_struct_signature(TypedAst* tast, TypedNode* sd) {
  String8 out = sd->struct_decl.is_union ? str8_lit("union") : str8_lit("struct");
  out = str8_cat(ctx_perm(), out, str8_lit(" "));
  out = str8_cat(ctx_perm(), out, sd->struct_decl.name);
  out = str8_cat(ctx_perm(), out, str8_lit(" "));
  out = str8_cat(ctx_perm(), out, format_param_list(tast, sd->struct_decl.field_first, sd->struct_decl.field_count));
  return out;
}

// "enum name [variants]" (or "flags") -- for hovering over an
// `Enum/Variant` access.
static String8
format_enum_signature(TypedAst* tast, TypedNode* ed) {
  String8 out = ed->enum_decl.is_flags ? str8_lit("flags") : str8_lit("enum");
  out = str8_cat(ctx_perm(), out, str8_lit(" "));
  out = str8_cat(ctx_perm(), out, ed->enum_decl.name);
  out = str8_cat(ctx_perm(), out, str8_lit(" ["));
  foreach_index(i, ed->enum_decl.variant_count) {
    EnumVariant* v = &tast->enum_variants[ed->enum_decl.variant_first + i];
    if (i > 0) out = str8_cat(ctx_perm(), out, str8_lit(" "));
    out = str8_cat(ctx_perm(), out, v->name);
  }
  out = str8_cat(ctx_perm(), out, str8_lit("]"));
  return out;
}

////////////////////////////////
//~ Doc comments -- the prose a hover popup shows above a declaration's
// signature. 3b has no doc-comment syntax and the lexer throws comments away
// (lexer_skip_ignorable), so rather than teach the token stream to carry them,
// this reads them back out of the source text the registry already keeps alive
// for diagnostics (SourceFile.text, 3b.h) once a declaration's line is known.
//
// A declaration's doc is the run of comment lines directly above it, with
// nothing in between: the first line up from the declaration that is blank, or
// code, or code-with-a-trailing-comment, ends the run. Requiring the `;` to be
// the first thing on its line is what keeps `(val x 3) ; why` from being read
// as documentation for whatever is declared underneath it.

// The 1-indexed `line`'th line of `text`, without its newline; empty if there
// is no such line. Same walk diag.c does to quote a source line, which isn't
// shared because that copy is static to the diagnostic printer.
static String8
source_text_line(String8 text, u32 line) {
  u64 pos = 0;
  u32 cur = 1;
  while (cur < line && pos < text.size) {
    if (text.str[pos] == '\n') cur += 1;
    pos += 1;
  }
  if (cur != line) return (String8){0};
  u64 start = pos;
  while (pos < text.size && text.str[pos] != '\n') pos += 1;
  return str8_range(text.str + start, text.str + pos);
}

// True if `line_text` is a whole-line comment, writing what follows its marker
// (`;`, `;;`, `;;;`...) and one optional space to `*out_body`, so a run reads
// back as prose rather than as source. The return value is what says whether
// the line is a comment, never `*out_body`: a bare `;;` is an empty body but a
// perfectly good blank line of prose, and must not end the run.
static b32
comment_line_body(String8 line_text, String8* out_body) {
  String8 trimmed = str8_skip_chop_whitespace(line_text);
  if (trimmed.size == 0 || trimmed.str[0] != ';') return false;
  u64 i = 0;
  while (i < trimmed.size && trimmed.str[i] == ';') i += 1;
  if (i < trimmed.size && trimmed.str[i] == ' ') i += 1;
  *out_body = str8_skip(trimmed, i);
  return true;
}

// The doc comment for a declaration starting at 1-indexed `line` of `text`:
// every whole-line comment immediately above it, markers stripped, joined
// top-to-bottom with newlines. Empty String8 when the line above isn't a
// comment, which is the common case.
static String8
doc_comment_above(String8 text, u32 line) {
  u32 first = line; // becomes the topmost comment line of the run
  while (first > 1) {
    String8 body;
    if (!comment_line_body(source_text_line(text, first - 1), &body)) break;
    first -= 1;
  }
  if (first == line) return (String8){0};
  // Seeded from the first line rather than concatenated onto an empty String8,
  // whose null `.str` str8_cat would memcpy from -- undefined even at length
  // zero. A one-line doc therefore just slices `text`, allocating nothing.
  String8 out = {0};
  for (u32 l = first; l < line; l += 1) {
    String8 body;
    comment_line_body(source_text_line(text, l), &body); // known to be a comment line
    if (l == first) { out = body; continue; }
    out = str8_cat(ctx_perm(), out, str8_lit("\n"));
    out = str8_cat(ctx_perm(), out, body);
  }
  return str8_skip_chop_whitespace(out);
}

// Fills in a hover result's declaration site -- where the name came from, and
// whatever prose was written above it. `tok` is the declaring token, so its
// file is the one to read the comment run out of.
static void
hover_fill_decl_site(Lib3bHoverResult* result, Token tok) {
  if (tok.file_id == SOURCE_FILE_UNKNOWN) return;
  SourceFile* sf         = source_file_get(tok.file_id);
  result->decl_file_path = str8_to_cstr(sf->path);
  result->decl_line      = tok.line;
  result->decl_col       = tok.col;
  String8 doc            = doc_comment_above(sf->text, tok.line);
  if (doc.size > 0) result->doc = str8_to_cstr(doc);
}

////////////////////////////////
//~ Hover-eval -- when a hovered expression is built entirely from literals,
// pure arithmetic and logic, `cast`, `Enum/Variant` access, and references to
// other top-level immutable `val`s (transitively, under the same restriction),
// evaluate it through the 3bscript bytecode VM (bcgen.c/bcvm.c) and append the
// computed value to the ordinary type-only hover text.
//
// The design exists to avoid one hazard: bc_compile_program always compiles and
// RUNS a `#init_globals` chunk as part of compiling anything at all (see
// bcgen.h), so naively compiling the real package root would execute that
// package's actual global side effects -- GL/SDL setup, file I/O, whatever a
// real `val`/`var` initializer does -- on every hover. Instead hover_eval_is_pure
// recursively verifies that the hovered expression, and transitively every
// top-level `val` it references, touches none of that: no `Call`, no heap
// `Alloc` (StructLiteral/ArrayLiteral/AllocExpr/a struct-typed ZeroExpr), no
// pointer/field/index dereference, no mutation. hover_eval_build_synthetic_root
// then hands bc_compile_program a minimal root Block holding only the approved
// val decls plus one synthesized wrapper function, so `#init_globals`
// structurally cannot reach outside that verified-pure closure. The one
// residual risk the whitelist can't design away -- integer division or modulo
// by a runtime-zero divisor, as in `(/ 10 (- 5 5))` -- is guarded at the VM
// level instead (BcResult.trapped, bcvm.c's Div/Mod cases).

// The same top-level walk find_toplevel_decl does, but matching the reference's
// exact declaring token instead of its name. hover_eval_is_pure's Identifier
// case needs to know whether an already-resolved reference's decl_token names a
// top-level immutable `val`, and matching by token is what keeps that safe
// against shadowing: a same-named local `let` binding's decl_token points at the
// `let` form, never at a ConstDecl, so it can never match here. A `var` is
// rejected by never matching -- only TypedNodeKind_ConstDecl is checked -- since
// its current value isn't knowable from source alone.
static TypedIndex
find_toplevel_val_decl(TypedAst* tast, TypedIndex root, Token decl_token) {
  TypedNode* root_n = &tast->nodes[root];
  foreach_index(j, root_n->block.stmt_count) {
    TypedIndex idx = tast->extra[root_n->block.stmt_first + j];
    if (idx == TYPED_NIL) continue;
    TypedNode* n = &tast->nodes[idx];
    if (n->kind != TypedNodeKind_ConstDecl) continue;
    if (n->token.file_id != decl_token.file_id) continue;
    if (n->token.line != decl_token.line || n->token.col != decl_token.col) continue;
    return idx;
  }
  return TYPED_NIL;
}

// Accumulates the top-level `val` decls hover_eval_is_pure has transitively
// verified pure for the current hover query. These become the synthetic root's
// top-level statements verbatim -- their real, already-checked TypedIndex, no
// copying -- alongside the one synthesized wrapper function. A plain array
// beats a HashTable: a real expression's val-closure is a handful of names.
typedef struct HoverEvalClosure {
  TypedIndex* val_decls; // dyn array (tast->arena), ConstDecl TypedIndex, first-needed order
} HoverEvalClosure;

static b32
hover_eval_closure_has(HoverEvalClosure* closure, TypedIndex decl_idx) {
  foreach_index(i, dyn_count(closure->val_decls)) {
    if (closure->val_decls[i] == decl_idx) return true;
  }
  return false;
}

// Recursively verifies `idx` is safe for hover-eval -- see this section's top
// note for the whitelist and the hazard it exists to avoid. `closure->val_decls`
// grows as a side effect rather than in a separate pass, so by the time this
// returns true `closure` already holds the complete verified-pure set of `val`s
// the expression transitively depends on, ready for
// hover_eval_build_synthetic_root to use without a second walk.
static b32
hover_eval_is_pure(TypedAst* tast, TypedIndex root, TypedIndex idx, HoverEvalClosure* closure) {
  TypedNode* n = &tast->nodes[idx];
  switch (n->kind) {
    case TypedNodeKind_IntLiteral:
    case TypedNodeKind_FloatLiteral:
    case TypedNodeKind_BoolLiteral:
    case TypedNodeKind_StringLiteral:
    case TypedNodeKind_NilLiteral:
    case TypedNodeKind_EnumAccess:
      return true;

    case TypedNodeKind_Identifier: {
      // Only a bare reference that resolved through the real scope chain and
      // lands on a top-level `val`: never a local, param or `var`, and never a
      // top-level `fn` referenced as a value, whose decl_token stays zeroed --
      // the same gap lib3b.h notes for goto-definition.
      if (n->ident.decl_token.file_id == SOURCE_FILE_UNKNOWN) return false;
      TypedIndex decl_idx = find_toplevel_val_decl(tast, root, n->ident.decl_token);
      if (decl_idx == TYPED_NIL) return false;
      if (hover_eval_closure_has(closure, decl_idx)) return true; // already verified pure
      TypedNode* decl = &tast->nodes[decl_idx];
      // An imported package's public `val` is spliced in with its initializer
      // nulled out (splice_public_decl, compiler.c -- the real definition lives
      // in that other package's TypedAst). Treating that missing init as "pure,
      // zero value" would show the wrong number, so it is an explicit reject,
      // not the same case as a genuinely absent initializer.
      if (decl->is_imported) return false;
      // Recorded before recursing, so a self-referential `val` can't recurse
      // forever even though the checker already forbids one.
      dyn_push(tast->arena, closure->val_decls, decl_idx);
      if (decl->const_decl.init == TYPED_NIL) return true; // no initializer -- zero value, still pure
      return hover_eval_is_pure(tast, root, decl->const_decl.init, closure);
    }

    case TypedNodeKind_BinaryAdd: case TypedNodeKind_BinarySub:
    case TypedNodeKind_BinaryMul: case TypedNodeKind_BinaryDiv: case TypedNodeKind_BinaryMod:
    case TypedNodeKind_BinaryEq:  case TypedNodeKind_BinaryNeq:
    case TypedNodeKind_BinaryLt:  case TypedNodeKind_BinaryLe:
    case TypedNodeKind_BinaryGt:  case TypedNodeKind_BinaryGe:
    case TypedNodeKind_BinaryBitAnd: case TypedNodeKind_BinaryBitOr: case TypedNodeKind_BinaryBitXor:
    case TypedNodeKind_BinaryShl:    case TypedNodeKind_BinaryShr:
    case TypedNodeKind_LogicalAnd:   case TypedNodeKind_LogicalOr:
      return hover_eval_is_pure(tast, root, n->binary.lhs, closure)
          && hover_eval_is_pure(tast, root, n->binary.rhs, closure);

    // `binary.lhs` is a type-name placeholder, not a value (see bcgen.c's
    // BinaryCast case), so only `binary.rhs` needs checking.
    // BinaryReinterpret has the same shape.
    case TypedNodeKind_BinaryCast:
    case TypedNodeKind_BinaryReinterpret:
      return hover_eval_is_pure(tast, root, n->binary.rhs, closure);

    case TypedNodeKind_UnaryPos: case TypedNodeKind_UnaryNeg: case TypedNodeKind_UnaryBitNot:
    case TypedNodeKind_LogicalNot:
      return hover_eval_is_pure(tast, root, n->unary.expr, closure);

    // Compile-time constants from type and layout info alone -- bcgen.c emits a
    // single LoadConst for these -- with no operand to recurse into.
    // TypedNodeKind_ZeroExpr is NOT included: on a struct type it compiles to a
    // real BcOp_Alloc (bc_compile_zero_value), and telling that apart from the
    // safe scalar case isn't worth it for what `(zero T)` hovers are worth.
    case TypedNodeKind_SizeofExpr: case TypedNodeKind_AlignofExpr: case TypedNodeKind_MemberOffsetExpr:
      return true;

    // Everything else -- Call, FieldAccess/IndexAccess/UnaryDeref/UnaryAddr,
    // StructLiteral/ArrayLiteral/AllocExpr/ZeroExpr, SetExpr, any collection or
    // handle-pool op, any control-flow form, TypeNameExpr (which bc_compile_expr
    // doesn't support at all) -- falls back to plain type-only hover.
    default:
      return false;
  }
}

// Splices a minimal new root Block into `tast` holding exactly `closure`'s val
// decls -- real, already-checked TypedIndex, no copying -- followed by one
// zero-param FunctionDecl named "#hover_eval", wrapping `expr_idx` as its single
// implicit-return body statement (bc_compile_function treats a body's last
// statement as the return, so no explicit ReturnExpr is needed). A `#` can't
// appear in a real 3b identifier, the same collision-proofing `#init_globals`
// uses. Every tast->nodes[...] read is re-indexed fresh rather than cached
// across a typed_push, since typed_push can grow and move that array.
static TypedIndex
hover_eval_build_synthetic_root(TypedAst* tast, HoverEvalClosure* closure, TypedIndex expr_idx,
                                 TypeRef expr_type, String8* out_fn_name) {
  TypedNode body_block = {0};
  body_block.kind             = TypedNodeKind_Block;
  body_block.token            = tast->nodes[expr_idx].token;
  body_block.block.stmt_first = (u32)dyn_count(tast->extra);
  dyn_push(tast->arena, tast->extra, expr_idx);
  body_block.block.stmt_count = 1;
  TypedIndex body_idx = typed_push(tast, body_block);

  String8 fn_name = str8_lit("#hover_eval");
  TypedNode fn_decl = {0};
  fn_decl.kind             = TypedNodeKind_FunctionDecl;
  fn_decl.token            = tast->nodes[expr_idx].token;
  fn_decl.func.name        = fn_name;
  fn_decl.func.param_count = 0;
  fn_decl.func.return_type = expr_type;
  fn_decl.func.body        = body_idx;
  TypedIndex fn_idx = typed_push(tast, fn_decl);
  *out_fn_name = fn_name;

  TypedNode root_block = {0};
  root_block.kind             = TypedNodeKind_Block;
  root_block.token            = tast->nodes[expr_idx].token;
  root_block.block.stmt_first = (u32)dyn_count(tast->extra);
  foreach_index(i, dyn_count(closure->val_decls)) {
    dyn_push(tast->arena, tast->extra, closure->val_decls[i]);
  }
  dyn_push(tast->arena, tast->extra, fn_idx);
  root_block.block.stmt_count = (u16)(dyn_count(closure->val_decls) + 1);
  return typed_push(tast, root_block);
}

// Formats a hover-eval BcResult into display text, on the same TypeKind
// dispatch bcgen.c uses to pick a BcOp_Print* opcode -- producing a string
// host-side instead of emitting an opcode. bcvm.c's BC_TARGET(PrintI64) and
// friends use the identical bit-reinterpretation idioms, just writing into a
// live VM register.
//
// Every value hover-eval can produce is scalar: nothing in hover_eval_is_pure's
// whitelist constructs a struct value, since the only route in would be a `val`
// whose own initializer is a StructLiteral, which is never pure. So TypeKind_
// String and an enum-named TypeKind_Named are the only non-scalar-looking cases
// worth handling -- checker.c resolves an EnumAccess to `{Named, enum_name}`
// rather than a dedicated enum TypeKind, and an enum value's register
// representation is just its integer. A Named type that isn't an enum can't
// occur, but returns an empty String8 (no "=" suffix) rather than asserting.
static String8
hover_eval_format_result(Arena* arena, Checker* ck, TypeRef type, BcResult r) {
  if (!r.has_value) return (String8){0};
  switch (type.kind) {
    case TypeKind_I8: case TypeKind_I16: case TypeKind_I32: case TypeKind_I64:
      return str8f(arena, "%lld", (long long)r.value);
    case TypeKind_U8: case TypeKind_U16: case TypeKind_U32: case TypeKind_U64:
      return str8f(arena, "%llu", (unsigned long long)r.value);
    case TypeKind_Bool:
      return str8_lit(r.value ? "true" : "false");
    case TypeKind_Char:
      return str8f(arena, "%c", (int)r.value);
    case TypeKind_F32: {
      u32 bits = (u32)r.value;
      f32 v; memcpy(&v, &bits, sizeof(v));
      return str8f(arena, "%g", (f64)v);
    }
    case TypeKind_F64: {
      f64 v; MemoryCopy(&v, &r.value, sizeof(v));
      return str8f(arena, "%g", v);
    }
    case TypeKind_String: {
      u8* hdr = (u8*)(intptr_t)r.value;
      u8* str;  MemoryCopy(&str,  hdr + 0, sizeof(str));
      u64 size; MemoryCopy(&size, hdr + 8, sizeof(size));
      return str8f(arena, "\"%.*s\"", (int)size, str);
    }
    case TypeKind_Named:
      if (enum_table_lookup(ck, type.name)) return str8f(arena, "%lld", (long long)r.value);
      return (String8){0};
    default:
      return (String8){0};
  }
}

// Attempts hover-eval for the already-resolved hover target `idx`, given
// `base_text`, the ordinary type or signature display lib3b_hover computed.
// Returns `base_text` unchanged when `idx` isn't eligible, the VM run trapped
// (BcResult.trapped), or the synthetic program failed to compile; otherwise
// appends " = <value>". `pb` supplies the live Checker (PackageBuild.ck)
// bc_compile_program needs for struct, enum and global lookups.
static String8
hover_eval_try_append(PackageBuild* pb, TypedIndex idx, String8 base_text) {
  HoverEvalClosure closure = {0};
  if (!hover_eval_is_pure(pb->tast, pb->root, idx, &closure)) return base_text;

  TypeRef expr_type = pb->resolved_types[idx];
  String8 fn_name;
  TypedIndex synth_root = hover_eval_build_synthetic_root(pb->tast, &closure, idx, expr_type, &fn_name);

  ArenaTemp scratch = arena_temp_begin(ctx_scratch());
  LayoutCache layout_cache;
  layout_cache_init(&layout_cache, scratch.arena);
  // `arena` (compile-time only) and `heap` (backing any BcOp_Alloc) both point
  // at the same short-lived scratch here. bc_compile_program warns against
  // aliasing them, but it is safe in this one case: hover_eval_is_pure's
  // whitelist guarantees this program never executes a BcOp_Alloc at all.
  BcProgram prog = bc_compile_program(&pb->ck, pb->tast, synth_root, scratch.arena, scratch.arena,
                                       &layout_cache, /*host_imports=*/NULL, /*module_table=*/NULL);
  String8 result = base_text;
  if (prog.ok) {
    u32      fn_idx = bc_program_find_fn(&prog, fn_name);
    BcResult r       = bc_run_in_program(&prog, fn_idx, NULL, 0, scratch.arena, NULL);
    if (!r.trapped) {
      String8 formatted = hover_eval_format_result(scratch.arena, &pb->ck, expr_type, r);
      if (formatted.size > 0) {
        // str8_cat copies bytes, not just views, so this is safe even though
        // `formatted` lives in `scratch`, about to be popped.
        result = str8_cat(ctx_perm(), base_text, str8_lit(" = "));
        result = str8_cat(ctx_perm(), result, formatted);
      }
    }
  }
  arena_temp_end(&scratch);
  return result;
}

Lib3bHoverResult
lib3b_hover(const char* dir_path_cstr, const char* file_path_cstr, u32 line, u32 col,
            const SourceOverlay* overlays, u64 overlay_count) {
  Lib3bHoverResult result = {0};
  Context          ctx;
  PackageBuild*    pb = compile_for_query(&ctx, dir_path_cstr, overlays, overlay_count);
  if (pb && pb->resolved_types) {
    TypedIndex idx = find_node_at_position(pb->tast, file_path_cstr, line, col);
    if (idx != TYPED_NIL) {
      TypedNode* n         = &pb->tast->nodes[idx];
      String8    ref_name = {0};
      switch (n->kind) {
        case TypedNodeKind_Call:          ref_name = n->call.callee;           break;
        case TypedNodeKind_StructLiteral: ref_name = n->struct_lit.type_name;  break;
        case TypedNodeKind_EnumAccess:    ref_name = n->enum_access.enum_name; break;
        default: break;
      }
      // Call/StructLiteral/EnumAccess show the full declaration -- params,
      // fields or variants -- rather than just the reference's resolved type,
      // since a call's own type is only its return type. Everything else keeps
      // the plain type display.
      String8 text = {0};
      // A builtin's call shapes come first, before any declaration lookup,
      // because check_expr's builtin dispatch also wins over a same-named
      // top-level fn -- so that fn is not what this call resolves to.
      if (n->kind == TypedNodeKind_Call) {
        const char* shapes = builtin_shapes_for(ref_name);
        if (shapes) {
          text         = str8_cstring((char*)shapes);
          result.kind = Lib3bHover_Builtin;
          result.name = str8_to_cstr(ref_name);
        }
      }
      TypedIndex decl_idx = (text.size == 0 && ref_name.size > 0)
                              ? find_toplevel_decl(pb->tast, pb->root, ref_name) : TYPED_NIL;
      if (decl_idx != TYPED_NIL) {
        TypedNode* decl = &pb->tast->nodes[decl_idx];
        switch (decl->kind) {
          case TypedNodeKind_FunctionDecl:
            text         = format_function_signature(pb->tast, decl);
            result.kind = Lib3bHover_Function;
            break;
          case TypedNodeKind_StructDecl:
            text         = format_struct_signature(pb->tast, decl);
            result.kind = Lib3bHover_Struct;
            break;
          case TypedNodeKind_EnumDecl:
            text         = format_enum_signature(pb->tast, decl);
            result.kind = Lib3bHover_Enum;
            break;
          default: break;
        }
        if (result.kind != Lib3bHover_Expression) {
          result.name = str8_to_cstr(ref_name);
          hover_fill_decl_site(&result, decl->token);
        }
      }
      // A field name in `(. ost frame)`, `(get ost frame)` or a `{}`
      // destructuring pattern. The node's own resolved type is already the
      // field's, so this adds what the plain type display cannot say: which
      // struct declares the field, and where -- reached through the base's type
      // rather than by name, so two structs sharing a field name stay distinct.
      if (text.size == 0 && n->kind == TypedNodeKind_FieldAccess) {
        TypedIndex owner_idx = find_field_owner_decl(pb, n->field_access.base);
        TypedIndex decl_idx  = owner_idx;
        if (owner_idx != TYPED_NIL) {
          Param* field = find_field_param(pb, &decl_idx, n->field_access.field);
          if (field) {
            // Qualified with the struct that DECLARES the field, which an
            // anonymous (`_`) member makes different from the one the access
            // was written against -- unless lowering synthesized that struct,
            // whose `AnonN` name the user never wrote and cannot look up.
            TypedNode* qualifier = &pb->tast->nodes[decl_idx];
            if (qualifier->struct_decl.is_synthesized) qualifier = &pb->tast->nodes[owner_idx];
            result.kind = Lib3bHover_Field;
            result.name = qualifier->struct_decl.is_synthesized
                            ? str8_to_cstr(n->field_access.field) // an access written directly
                                                                   // against an inline struct: no
                                                                   // name to qualify it with
                            : str8_to_cstr(str8f(ctx_perm(), "%.*s.%.*s",
                                                    str8_varg(qualifier->struct_decl.name),
                                                    str8_varg(n->field_access.field)));
            hover_fill_decl_site(&result, field->name_token);
            // A field written on the same line as `(struct ...)` has no comment
            // run of its own: whatever sits above that line documents the
            // struct, and crediting it to the field would misattribute it.
            if (field->name_token.line == pb->tast->nodes[decl_idx].token.line) {
              free(result.doc);
              result.doc = NULL;
            }
          }
        }
      }
      // A bare identifier reports the binding the checker actually resolved it
      // to -- the same recorded ScopeEntry token goto-definition reads, so a
      // shadowed name credits its inner binding, not the outer one it hides.
      if (text.size == 0 && n->kind == TypedNodeKind_Identifier) {
        result.kind = Lib3bHover_Binding;
        result.name = str8_to_cstr(n->token.text);
        hover_fill_decl_site(&result, n->ident.decl_token);
      }
      if (text.size == 0) text = type_ref_display(ctx_scratch(), pb->resolved_types[idx]);
      result.found     = true;
      // hover-eval appends " = <value>" when `idx` is a pure, closed
      // expression, and returns `text` unchanged otherwise, which is the
      // ordinary case.
      result.type_text = str8_to_cstr(hover_eval_try_append(pb, idx, text));
    }
    // No typed node covers this position -- try a type annotation, the way
    // goto-definition does. Hovering `Mesh^` in a parameter list shows the
    // struct it names, the same jump goto-definition offers there.
    if (!result.found) {
      TypeRef* ty = find_type_annotation_at_position(pb->tast, file_path_cstr, line, col);
      if (ty) {
        String8    named    = type_ref_named_target(*ty);
        TypedIndex decl_idx = named.size > 0 ? find_toplevel_decl(pb->tast, pb->root, named) : TYPED_NIL;
        String8    text     = {0};
        if (decl_idx != TYPED_NIL) {
          TypedNode* decl = &pb->tast->nodes[decl_idx];
          switch (decl->kind) {
            case TypedNodeKind_StructDecl:
              text         = format_struct_signature(pb->tast, decl);
              result.kind = Lib3bHover_Struct;
              break;
            case TypedNodeKind_EnumDecl:
              text         = format_enum_signature(pb->tast, decl);
              result.kind = Lib3bHover_Enum;
              break;
            default: break;
          }
        }
        if (text.size > 0) {
          result.name = str8_to_cstr(named);
          hover_fill_decl_site(&result, pb->tast->nodes[decl_idx].token);
        } else {
          text = type_ref_display(ctx_scratch(), *ty); // a primitive, or a Vector/Map/Fn -- nothing to point at,
                                         // but the annotation as the checker resolved it still beats
                                         // showing nothing at all
        }
        result.found     = true;
        result.type_text = str8_to_cstr(text);
      }
    }
  }
  ctx_free();
  return result;
}

Lib3bLocation
lib3b_definition(const char* dir_path_cstr, const char* file_path_cstr, u32 line, u32 col,
                  const SourceOverlay* overlays, u64 overlay_count) {
  Lib3bLocation result = {0};
  Context       ctx;
  PackageBuild* pb = compile_for_query(&ctx, dir_path_cstr, overlays, overlay_count);
  if (pb) {
    TypedIndex idx = find_node_at_position(pb->tast, file_path_cstr, line, col);
    if (idx != TYPED_NIL) {
      TypedNode* n = &pb->tast->nodes[idx];
      // A bare identifier that resolved through the checker's shadowing-aware
      // Scope chain (n->ident.decl_token, set in check_expr's
      // TypedNodeKind_Identifier case). Covers locals and params, and -- since
      // top-level const/vars share that same scope chain -- references to those
      // too. Read straight off the recorded token: no name lookup, and none of
      // the shadowing risk a query-time re-derivation would carry.
      if (n->kind == TypedNodeKind_Identifier && n->ident.decl_token.file_id != SOURCE_FILE_UNKNOWN) {
        Token       decl_tok = n->ident.decl_token;
        SourceFile* sf        = source_file_get(decl_tok.file_id);
        result.found     = true;
        result.file_path = str8_to_cstr(sf->path);
        result.line      = decl_tok.line;
        result.col       = decl_tok.col;
      } else if (n->kind == TypedNodeKind_FieldAccess) {
        // The field name in `(. ost frame)` and friends jumps to the field's own line in the
        // struct that declares it, resolved through the base's type the way hover does.
        TypedIndex owner_idx = find_field_owner_decl(pb, n->field_access.base);
        Param*     field     = owner_idx != TYPED_NIL
                                 ? find_field_param(pb, &owner_idx, n->field_access.field) : NULL;
        if (field && field->name_token.file_id != SOURCE_FILE_UNKNOWN) {
          SourceFile* sf = source_file_get(field->name_token.file_id);
          result.found     = true;
          result.file_path = str8_to_cstr(sf->path);
          result.line      = field->name_token.line;
          result.col       = field->name_token.col;
        }
      } else {
        String8 ref_name = {0};
        switch (n->kind) {
          case TypedNodeKind_Call:          ref_name = n->call.callee;            break;
          case TypedNodeKind_StructLiteral: ref_name = n->struct_lit.type_name;   break;
          case TypedNodeKind_EnumAccess:    ref_name = n->enum_access.enum_name;  break;
          default: break; // a value-reference to a top-level fn and the like -- not resolvable
                           // here; type annotations are handled below, once this node-based
                           // attempt finds nothing
        }
        TypedIndex decl_idx = ref_name.size > 0 ? find_toplevel_decl(pb->tast, pb->root, ref_name) : TYPED_NIL;
        if (decl_idx != TYPED_NIL) {
          Token       decl_tok = pb->tast->nodes[decl_idx].token;
          SourceFile* sf        = source_file_get(decl_tok.file_id);
          result.found     = true;
          result.file_path = str8_to_cstr(sf->path);
          result.line      = decl_tok.line;
          result.col       = decl_tok.col;
        }
      }
    }
    // Nothing found among ordinary typed nodes -- try a type annotation instead
    // (a param, field, var or return type). Unwrapping through pointer and
    // handle levels is what lets `Mesh*`/`Mesh^` jump to the same `struct Mesh`
    // declaration a bare `Mesh` would (type_ref_named_target).
    if (!result.found) {
      TypeRef* ty = find_type_annotation_at_position(pb->tast, file_path_cstr, line, col);
      if (ty) {
        String8    name     = type_ref_named_target(*ty);
        TypedIndex decl_idx = name.size > 0 ? find_toplevel_decl(pb->tast, pb->root, name) : TYPED_NIL;
        if (decl_idx != TYPED_NIL) {
          Token       decl_tok = pb->tast->nodes[decl_idx].token;
          SourceFile* sf        = source_file_get(decl_tok.file_id);
          result.found     = true;
          result.file_path = str8_to_cstr(sf->path);
          result.line      = decl_tok.line;
          result.col       = decl_tok.col;
        }
      }
    }
  }
  ctx_free();
  return result;
}

void
lib3b_free_hover(Lib3bHoverResult* result) {
  if (!result) return;
  free(result->type_text);
  free(result->name);
  free(result->doc);
  free(result->decl_file_path);
  MemoryZeroStruct(result);
}

void
lib3b_free_location(Lib3bLocation* result) {
  if (!result) return;
  free(result->file_path);
  MemoryZeroStruct(result);
}

////////////////////////////////
//~ Completion context -- see lib3b.h.

// Drives the real lexer token by token up to `offset`, tracking a stack of
// still-open `(`/`[`/`{`, and returns a malloc'd copy of `text[0..offset)` with
// the right closers appended. Using the lexer rather than a naive character
// scan means a '(' inside a string literal or after a ';' comment is skipped
// instead of taken for a real delimiter. Never fails: an unparseable prefix,
// truncated mid-string say, is still tracked byte by byte through `lex.pos`,
// and the only visible effect of anything going wrong is a patched buffer that
// still doesn't compile, which the caller already treats as "give up, use the
// cache" whatever the reason.
static char*
patch_unclosed_delimiters(const char* text, u64 offset) {
  u64 len = (u64)strlen(text);
  if (offset > len) offset = len;

  String8 truncated = {0};
  truncated.str  = (u8*)text;
  truncated.size = offset;

  Lexer lex;
  lexer_init(&lex, truncated, SOURCE_FILE_UNKNOWN);

  TokenKind stack[256];
  int       depth = 0;
  for (;;) {
    u64   pos_before = lex.pos;
    Token tok         = lexer_next(&lex, ctx_perm());
    if (tok.kind == TokenKind_EOF) break;
    if (lex.pos == pos_before) break; // safety net -- didn't advance, don't loop forever
    switch (tok.kind) {
      case TokenKind_LParen: case TokenKind_LBracket: case TokenKind_LBrace:
        if (depth < (int)(sizeof(stack) / sizeof(stack[0]))) stack[depth] = tok.kind;
        depth += 1;
        break;
      case TokenKind_RParen: case TokenKind_RBracket: case TokenKind_RBrace:
        if (depth > 0) depth -= 1;
        break;
      default: break;
    }
  }

  char* result = malloc(offset + (u64)depth + 1);
  memcpy(result, text, offset);
  for (int i = depth - 1; i >= 0; i -= 1) {
    TokenKind k = (i < (int)(sizeof(stack) / sizeof(stack[0]))) ? stack[i] : TokenKind_LParen;
    char      c = (k == TokenKind_LParen) ? ')' : (k == TokenKind_LBracket) ? ']' : '}';
    result[offset + (u64)(depth - 1 - i)] = c;
  }
  result[offset + (u64)depth] = 0;
  return result;
}

// Byte offset -> 1-based (line, col), using the same increment rule
// lexer_advance does; mirrored rather than shared, since this needs the rule
// but no live Lexer. Only ever called on `buffer_text` itself, never the
// patched copy: patch_unclosed_delimiters touches nothing before `offset`, so
// both agree on where line/col land there, and using the original avoids
// another malloc'd intermediate.
static void
line_col_from_offset(const char* text, u64 offset, u32* out_line, u32* out_col) {
  u32 line = 1, col = 1;
  u64 len  = (u64)strlen(text);
  if (offset > len) offset = len;
  for (u64 i = 0; i < offset; i += 1) {
    if (text[i] == '\n') { line += 1; col = 1; }
    else                  { col += 1; }
  }
  *out_line = line;
  *out_col  = col;
}

Lib3bCompletionContext
lib3b_completion_context(const char* dir_path_cstr, const char* file_path_cstr,
                          const char* buffer_text, u64 cursor_offset,
                          const SourceOverlay* overlays, u64 overlay_count) {
  Lib3bCompletionContext result = {0};

  Context ctx;
  ctx_init(&ctx, MB(16));
  source_registry_reset();

  char* patched = patch_unclosed_delimiters(buffer_text, cursor_offset);

  // The caller's overlays (build_overlays_for_dir, lsp/lsp_main.c) may already
  // include an entry for file_path_cstr itself, holding its unpatched,
  // still-broken content, since that is what the open-document table has. That
  // entry must not also reach compile_all_packages: overlay_lookup is a
  // first-match linear scan, so with both an unpatched and a patched entry for
  // one file, whichever came first would silently win. Drop the caller's
  // version of this one file, keep everything else, append the patched
  // replacement.
  String8        target_base = str8_basename(str8_cstring((char*)file_path_cstr));
  SourceOverlay* combined     = push_array(ctx_perm(), SourceOverlay, overlay_count + 1);
  u64            combined_count = 0;
  foreach_index(i, overlay_count) {
    if (str8_match(str8_basename(overlays[i].path), target_base, 0)) continue;
    combined[combined_count] = overlays[i];
    combined_count += 1;
  }
  combined[combined_count].path    = str8_cstring((char*)file_path_cstr);
  combined[combined_count].content = str8_cstring(patched);
  combined_count += 1;

  ScopeQuery query = {0};
  query.file_basename = target_base;
  line_col_from_offset(buffer_text, cursor_offset, &query.line, &query.col);

  PackageKind root_kind = build_config_read_kind(ctx_perm(), dir_path_cstr);
  diag_capture_begin(/*also_print=*/false);
  PackageBuild* root_pkg = compile_all_packages(dir_path_cstr, NULL, root_kind, /*verbose=*/false,
                                                 combined, combined_count, /*tolerate_check_errors=*/true,
                                                 &query);
  diag_capture_end(NULL);

  if (root_pkg) {
    result.ok = true;
    collect_toplevel_symbols(root_pkg->tast, root_pkg->root, /*include_private=*/true,
                              &result.symbols, &result.symbol_count);
    if (root_pkg->scope_query_result) {
      collect_scope_locals(root_pkg->scope_query_result, root_pkg->scope_query_count,
                            result.symbols, result.symbol_count,
                            &result.locals, &result.local_count);
      free(root_pkg->scope_query_result); // plain malloc'd by checker.c's ScopeQuery hook;
                                            // collect_scope_locals has already copied out what
                                            // it needs, so nothing here crosses ctx_free
    }
  }

  free(patched);
  ctx_free();
  return result;
}

void
lib3b_free_completion_context(Lib3bCompletionContext* result) {
  if (!result) return;
  foreach_index(i, result->symbol_count) lib3b_symbol_free_fields(&result->symbols[i]);
  free(result->symbols);
  foreach_index(i, result->local_count) lib3b_symbol_free_fields(&result->locals[i]);
  free(result->locals);
  MemoryZeroStruct(result);
}

////////////////////////////////
//~ Formatting -- see lib3b.h. The parse half of main.c's format_file_cmd,
// reading a buffer the caller already holds instead of a path, and writing
// into memory instead of a FILE on disk.

char*
lib3b_format(const char* file_path_cstr, const char* buffer_text) {
  Context ctx;
  ctx_init(&ctx, MB(16));
  source_registry_reset();

  String8 src     = str8_cstring((char*)buffer_text);
  u32     file_id = source_file_register(str8_cstring((char*)file_path_cstr), src);

  Ast ast;
  ast_init(&ast, ctx_perm());
  Parser p;
  parser_init(&p, src, &ast, file_id);
  // Captured and dropped rather than printed: a buffer mid-edit fails to parse
  // constantly, and this entry point reports that by returning NULL, not by
  // writing to the host's stderr.
  diag_capture_begin(/*also_print=*/false);
  NodeIndex root = parse_program(&p);
  diag_capture_end(NULL);
  if (p.had_error) {
    ctx_free();
    return NULL;
  }

  char*  buf  = NULL;
  size_t size = 0;
  FILE*  out  = open_memstream(&buf, &size);
  if (!out) {
    ctx_free();
    return NULL;
  }
  fmt_program(out, &ast, root, src, /*hang=*/0); // 0 leaves fmt_program's own default
  fclose(out);                                    // flushes and NUL-terminates buf

  ctx_free();
  return buf; // malloc'd by open_memstream
}
