// Reader for the config DSL; see translate.h. Reuses 3b's lexer and parser for
// a generic atom/list/vector/map AST, then walks that into a Config. Nothing
// here is lowered, checked or run.
#include "translate.h"
#include <stdarg.h>

static b32 g_had_error;

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

// Strips a leading ':' off a keyword atom (`:common` -> `common`). The ':' is
// not validated; callers only use this on nodes already in keyword position.
static String8
strip_keyword_colon(String8 s) {
  if (s.size > 0 && s.str[0] == ':') return str8(s.str + 1, s.size - 1);
  return s;
}

static NodeIndex*
node_seq_children(Ast* ast, NodeIndex idx, AstNodeKind want_kind, u16* out_count) {
  AstNode* n = ast_get(ast, idx);
  if (n->kind != want_kind) {
    cfg_error(n->token, "expected a %s here, got a %s",
               ast_node_kind_name(want_kind), ast_node_kind_name(n->kind));
    *out_count = 0;
    return NULL;
  }
  return ast_seq_children(ast, idx, out_count);
}

// strtoull answers a non-numeric atom with 0 and an out-of-range one with u64's
// max, so reading one bare turns `[out-param abc]` into param 0 -- a real rule
// pointed at the wrong argument, with nothing said about it.
static u64
node_atom_u64(Ast* ast, NodeIndex idx) {
  String8         text = node_atom_text(ast, idx);
  NumericAtomInfo num  = atom_classify_numeric(text);
  u64             v    = 0;
  if (!num.is_numeric || num.is_float || !atom_parse_u64(num.body, num.is_hex, &v)) {
    cfg_error(ast_get(ast, idx)->token, "expected a non-negative integer here, got `%.*s`",
               str8_varg(text));
    return 0;
  }
  return v;
}

////////////////////////////////
//~ Per-form handlers

static void
handle_package(Arena* arena, Ast* ast, NodeIndex* fc, u16 n, Config* cfg) {
  (void)arena;
  if (n != 2) { cfg_error(ast_get(ast, fc[0])->token, "`package` takes exactly one name"); return; }
  cfg->package_name = node_atom_text(ast, fc[1]);
}

static void
handle_headers(Arena* arena, Ast* ast, NodeIndex* fc, u16 n, Config* cfg) {
  if (n != 2) { cfg_error(ast_get(ast, fc[0])->token, "`headers` takes exactly one `[...]` vector"); return; }
  u16        vc;
  NodeIndex* vchildren = node_seq_children(ast, fc[1], AstNodeKind_Vector, &vc);
  foreach_index(i, vc) {
    dyn_push(arena, cfg->headers, node_string_text(ast, vchildren[i]));
  }
}

static void
handle_ifdef(Arena* arena, Ast* ast, NodeIndex* fc, u16 n, Config* cfg) {
  if (n < 3 || (n % 2) != 1) {
    cfg_error(ast_get(ast, fc[0])->token, "`ifdef` takes `:platform [NAME VALUE ...]` pairs");
    return;
  }
  for (u32 i = 1; i < n; i += 2) {
    DefineGroup group = {0};
    group.platform    = strip_keyword_colon(node_atom_text(ast, fc[i]));
    u16        dc;
    NodeIndex* dchildren = node_seq_children(ast, fc[i + 1], AstNodeKind_Vector, &dc);
    if ((dc % 2) != 0) {
      cfg_error(ast_get(ast, fc[i + 1])->token, "`:%.*s` group must have an even number of NAME VALUE atoms",
                 str8_varg(group.platform));
      continue;
    }
    for (u32 d = 0; d < dc; d += 2) {
      DefineKV kv = {0};
      kv.name     = node_atom_text(ast, dchildren[d]);
      kv.value    = node_atom_text(ast, dchildren[d + 1]);
      dyn_push(arena, group.defines, kv);
    }
    dyn_push(arena, cfg->define_groups, group);
  }
}

static void
handle_strip_prefix(Arena* arena, Ast* ast, NodeIndex* fc, u16 n, String8* out) {
  (void)arena;
  if (n != 2) { cfg_error(ast_get(ast, fc[0])->token, "expects exactly one \"prefix\" string"); return; }
  *out = node_string_text(ast, fc[1]);
}

static void
handle_type_map(Arena* arena, Ast* ast, NodeIndex* fc, u16 n, Config* cfg) {
  if (n != 2) { cfg_error(ast_get(ast, fc[0])->token, "`type-map` takes exactly one `[...]` vector"); return; }
  u16        vc;
  NodeIndex* vc_children = node_seq_children(ast, fc[1], AstNodeKind_Vector, &vc);
  if ((vc % 2) != 0) {
    cfg_error(ast_get(ast, fc[1])->token, "`type-map` vector must have an even number of C_NAME B3_NAME atoms");
    return;
  }
  for (u32 i = 0; i < vc; i += 2) {
    TypeMapEntry e = {0};
    e.c_name       = node_atom_text(ast, vc_children[i]);
    e.b3_name      = node_atom_text(ast, vc_children[i + 1]);
    dyn_push(arena, cfg->type_map, e);
  }
}

// `(pin-type [size_t u64 ...])`. C_NAME B3_NAME pairs like `type-map`, but the
// C name survives into the generated C as a pinned alias rather than being
// substituted away. See emit_pinned_type_aliases.
//
// An entry may carry a third element, a quoted C spelling for the emitted
// typedef to stand on: `[Uint64 u64 "uint64_t"]` pins SDL's Uint64 under its
// own name while emitting `typedef uint64_t sdl_Uint64;`, because Uint64
// itself is not visible in the generated header. Being a string rather than a
// bare name is what keeps the flat vector unambiguous -- a pair's B3_NAME is
// always followed by either the next pair's C_NAME (an atom) or that pair's
// own spelling (a string).
static void
handle_pin_type(Arena* arena, Ast* ast, NodeIndex* fc, u16 n, Config* cfg) {
  if (n != 2) { cfg_error(ast_get(ast, fc[0])->token, "`pin-type` takes exactly one `[...]` vector"); return; }
  u16        vc;
  NodeIndex* vc_children = node_seq_children(ast, fc[1], AstNodeKind_Vector, &vc);
  u32        i           = 0;
  while (i < vc) {
    if ((u32)vc - i < 2) {
      cfg_error(ast_get(ast, fc[1])->token,
                "`pin-type` entries are C_NAME B3_NAME, each optionally followed by a \"c-spelling\" string");
      return;
    }
    PinTypeEntry e = {0};
    e.c_name       = node_atom_text(ast, vc_children[i]);
    e.b3_name      = node_atom_text(ast, vc_children[i + 1]);
    i += 2;
    if (i < vc && ast_get(ast, vc_children[i])->kind == AstNodeKind_String) {
      e.c_spelling = node_string_text(ast, vc_children[i]);
      i += 1;
    }
    dyn_push(arena, cfg->pin_type, e);
  }
}

static void
handle_const_group(Arena* arena, Ast* ast, NodeIndex* fc, u16 n, Config* cfg, ConstGroupKind kind) {
  const char* form_name = kind == ConstGroupKind_Enum ? "enum-group" : "flags-group";
  if (n != 3) { cfg_error(ast_get(ast, fc[0])->token, "`%s` takes a name and either a `[...]` member list or a `(match \"...\")`", form_name); return; }

  ConstGroup group = {0};
  group.kind       = kind;
  group.name       = node_atom_text(ast, fc[1]);

  AstNode* third = ast_get(ast, fc[2]);
  if (third->kind == AstNodeKind_Vector) {
    u16        mc;
    NodeIndex* mchildren = ast_seq_children(ast, fc[2], &mc);
    foreach_index(i, mc) {
      dyn_push(arena, group.members, node_atom_text(ast, mchildren[i]));
    }
  } else if (third->kind == AstNodeKind_List) {
    u16        lc;
    NodeIndex* lchildren = ast_seq_children(ast, fc[2], &lc);
    if (lc != 2 || !str8_match_lit("match", node_atom_text(ast, lchildren[0]), 0)) {
      cfg_error(third->token, "`%s`'s third argument must be `[Member ...]` or `(match \"pattern\")`", form_name);
      return;
    }
    group.has_pattern = true;
    group.pattern      = node_string_text(ast, lchildren[1]);
  } else {
    cfg_error(third->token, "`%s`'s third argument must be `[Member ...]` or `(match \"pattern\")`", form_name);
    return;
  }
  dyn_push(arena, cfg->const_groups, group);
}

static void
handle_force_opaque(Arena* arena, Ast* ast, NodeIndex* fc, u16 n, Config* cfg) {
  if (n != 2) { cfg_error(ast_get(ast, fc[0])->token, "`force-opaque` takes exactly one `[...]` vector"); return; }
  u16        vc;
  NodeIndex* vchildren = node_seq_children(ast, fc[1], AstNodeKind_Vector, &vc);
  foreach_index(i, vc) {
    dyn_push(arena, cfg->force_opaque, node_atom_text(ast, vchildren[i]));
  }
}

// Scans a `[tag N tag2 M ...]` vector for `tag` and returns the following atom
// as a u64. Order-independent, since `arena-outparam`'s `count-param` and
// `out-param` may appear either way round. Errors if `tag` is absent.
static u64
find_tagged_u64(Ast* ast, NodeIndex* children, u16 count, const char* tag, Token err_tok, b32* found) {
  for (u32 i = 0; i + 1 < count; i += 1) {
    AstNode* n = ast_get(ast, children[i]);
    if (n->kind == AstNodeKind_Atom && str8_match(str8_cstring((char*)tag), n->token.text, 0)) {
      *found = true;
      return node_atom_u64(ast, children[i + 1]);
    }
  }
  cfg_error(err_tok, "missing `%s N` entry", tag);
  *found = false;
  return 0;
}

static void
handle_outparam(Arena* arena, Ast* ast, NodeIndex* fc, u16 n, Config* cfg, OutparamKind kind) {
  const char* form_name = kind == OutparamKind_Construct ? "construct-outparam"
                         : kind == OutparamKind_Mutate    ? "mutate-outparam"
                                                             : "arena-outparam";
  if (n != 3) { cfg_error(ast_get(ast, fc[0])->token, "`%s` takes a function name and a `[...]` tag vector", form_name); return; }

  OutparamRule rule = {0};
  rule.kind         = kind;
  rule.func_name    = node_atom_text(ast, fc[1]);

  u16        vc;
  NodeIndex* vchildren = node_seq_children(ast, fc[2], AstNodeKind_Vector, &vc);
  b32 found = false;
  if (kind == OutparamKind_Arena) {
    rule.count_param_index = (u32)find_tagged_u64(ast, vchildren, vc, "count-param", ast_get(ast, fc[2])->token, &found);
    if (!found) return;
    rule.out_param_index = (u32)find_tagged_u64(ast, vchildren, vc, "out-param", ast_get(ast, fc[2])->token, &found);
    if (!found) return;
  } else {
    rule.param_index = (u32)find_tagged_u64(ast, vchildren, vc, "param", ast_get(ast, fc[2])->token, &found);
    if (!found) return;
  }
  dyn_push(arena, cfg->outparam_rules, rule);
}

static void
handle_rename_func(Arena* arena, Ast* ast, NodeIndex* fc, u16 n, Config* cfg) {
  if (n != 3) { cfg_error(ast_get(ast, fc[0])->token, "`rename-func` takes exactly a C name and a new 3b name"); return; }
  RenameRule r = {0};
  r.from       = node_atom_text(ast, fc[1]);
  r.to         = node_atom_text(ast, fc[2]);
  dyn_push(arena, cfg->func_renames, r);
}

// `(rename-const [LOGICAL_NAME1 NewName1 ...])` and
// `(rename-type [CName1 NewName1 ...])`. Flat pairs like `type-map` rather than
// one form per rename: a C enum can need spelling overrides for hundreds of
// members (see translate.h's Config.const_renames), which one form per line
// would swamp a config file with.
static void
handle_rename_pairs(Arena* arena, Ast* ast, NodeIndex* fc, u16 n, RenameRule** out, const char* form_name) {
  if (n != 2) { cfg_error(ast_get(ast, fc[0])->token, "`%s` takes exactly one `[...]` vector", form_name); return; }
  u16        vc;
  NodeIndex* vchildren = node_seq_children(ast, fc[1], AstNodeKind_Vector, &vc);
  if ((vc % 2) != 0) {
    cfg_error(ast_get(ast, fc[1])->token, "`%s` vector must have an even number of OLD_NAME NewName atoms", form_name);
    return;
  }
  for (u32 i = 0; i < vc; i += 2) {
    RenameRule r = {0};
    r.from       = node_atom_text(ast, vchildren[i]);
    r.to         = node_atom_text(ast, vchildren[i + 1]);
    dyn_push(arena, *out, r);
  }
}

// `(rename-const-pattern "^CODEC_ID_(.*)$" "{1:pascal}")` and its func/type
// siblings. One form per rule, unlike the flat vectors above: each rule is two
// long quoted strings, and pairing those positionally inside a `[...]` would be
// unreadable exactly where it matters, since a wrong regex silently renames
// hundreds of declarations.
static void
handle_rename_pattern(Arena* arena, Ast* ast, NodeIndex* fc, u16 n, RenamePattern** out, const char* form_name) {
  if (n != 3) {
    cfg_error(ast_get(ast, fc[0])->token, "`%s` takes a \"regex\" and a \"replacement template\" string", form_name);
    return;
  }
  RenamePattern p = {0};
  p.pattern       = node_string_text(ast, fc[1]);
  p.replacement   = node_string_text(ast, fc[2]);
  dyn_push(arena, *out, p);
}

static void
handle_name_list(Arena* arena, Ast* ast, NodeIndex* fc, u16 n, String8** out, const char* form_name) {
  if (n != 2) { cfg_error(ast_get(ast, fc[0])->token, "`%s` takes exactly one `[...]` vector", form_name); return; }
  u16        vc;
  NodeIndex* vchildren = node_seq_children(ast, fc[1], AstNodeKind_Vector, &vc);
  foreach_index(i, vc) {
    dyn_push(arena, *out, node_atom_text(ast, vchildren[i]));
  }
}

b32
config_read(Arena* arena, String8 src, const char* platform, Config* out_config, u32 file_id) {
  (void)platform; // platform selection happens where define_groups are consumed (cwalk.c), not here
  g_had_error = false;
  MemoryZeroStruct(out_config);

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
    if (op.size == 0) continue; // node_atom_text already reported the error

         if (str8_match_lit("package",            op, 0)) handle_package(arena, &ast, fchildren, fc, out_config);
    else if (str8_match_lit("headers",             op, 0)) handle_headers(arena, &ast, fchildren, fc, out_config);
    else if (str8_match_lit("ifdef",               op, 0)) handle_ifdef(arena, &ast, fchildren, fc, out_config);
    else if (str8_match_lit("strip-const-prefix",  op, 0)) handle_strip_prefix(arena, &ast, fchildren, fc, &out_config->strip_const_prefix);
    else if (str8_match_lit("strip-func-prefix",   op, 0)) handle_strip_prefix(arena, &ast, fchildren, fc, &out_config->strip_func_prefix);
    else if (str8_match_lit("strip-struct-prefix", op, 0)) handle_strip_prefix(arena, &ast, fchildren, fc, &out_config->strip_struct_prefix);
    else if (str8_match_lit("type-map",            op, 0)) handle_type_map(arena, &ast, fchildren, fc, out_config);
    else if (str8_match_lit("pin-type",            op, 0)) handle_pin_type(arena, &ast, fchildren, fc, out_config);
    else if (str8_match_lit("enum-group",          op, 0)) handle_const_group(arena, &ast, fchildren, fc, out_config, ConstGroupKind_Enum);
    else if (str8_match_lit("flags-group",         op, 0)) handle_const_group(arena, &ast, fchildren, fc, out_config, ConstGroupKind_Flags);
    else if (str8_match_lit("force-opaque",        op, 0)) handle_force_opaque(arena, &ast, fchildren, fc, out_config);
    else if (str8_match_lit("construct-outparam",  op, 0)) handle_outparam(arena, &ast, fchildren, fc, out_config, OutparamKind_Construct);
    else if (str8_match_lit("mutate-outparam",     op, 0)) handle_outparam(arena, &ast, fchildren, fc, out_config, OutparamKind_Mutate);
    else if (str8_match_lit("arena-outparam",      op, 0)) handle_outparam(arena, &ast, fchildren, fc, out_config, OutparamKind_Arena);
    else if (str8_match_lit("rename-func",         op, 0)) handle_rename_func(arena, &ast, fchildren, fc, out_config);
    else if (str8_match_lit("rename-const",        op, 0)) handle_rename_pairs(arena, &ast, fchildren, fc, &out_config->const_renames, "rename-const");
    else if (str8_match_lit("rename-type",         op, 0)) handle_rename_pairs(arena, &ast, fchildren, fc, &out_config->type_renames, "rename-type");
    else if (str8_match_lit("rename-const-pattern", op, 0)) handle_rename_pattern(arena, &ast, fchildren, fc, &out_config->const_rename_patterns, "rename-const-pattern");
    else if (str8_match_lit("rename-func-pattern",  op, 0)) handle_rename_pattern(arena, &ast, fchildren, fc, &out_config->func_rename_patterns, "rename-func-pattern");
    else if (str8_match_lit("rename-type-pattern",  op, 0)) handle_rename_pattern(arena, &ast, fchildren, fc, &out_config->type_rename_patterns, "rename-type-pattern");
    else if (str8_match_lit("skip-deprecated",     op, 0)) out_config->skip_deprecated = true;
    else if (str8_match_lit("exclude-func",        op, 0)) handle_name_list(arena, &ast, fchildren, fc, &out_config->excluded_funcs, "exclude-func");
    else if (str8_match_lit("exclude-const",       op, 0)) handle_name_list(arena, &ast, fchildren, fc, &out_config->excluded_consts, "exclude-const");
    else cfg_error(form->token, "unknown config form `%.*s`", str8_varg(op));
  }

  return !g_had_error;
}

////////////////////////////////
//~ Debug printer

static void
print_rename_patterns(const char* label, RenamePattern* list) {
  printf("%s rename patterns (%llu):\n", label, (unsigned long long)dyn_count(list));
  foreach_index(i, dyn_count(list)) {
    printf("  /%.*s/ -> \"%.*s\"\n", str8_varg(list[i].pattern), str8_varg(list[i].replacement));
  }
}

void
config_print(Config* c) {
  printf("package: %.*s\n", str8_varg(c->package_name));
  printf("headers (%llu):\n", (unsigned long long)dyn_count(c->headers));
  foreach_index(i, dyn_count(c->headers)) printf("  \"%.*s\"\n", str8_varg(c->headers[i]));

  printf("define groups (%llu):\n", (unsigned long long)dyn_count(c->define_groups));
  foreach_index(i, dyn_count(c->define_groups)) {
    DefineGroup* g = &c->define_groups[i];
    printf("  :%.*s\n", str8_varg(g->platform));
    foreach_index(j, dyn_count(g->defines)) {
      printf("    %.*s = %.*s\n", str8_varg(g->defines[j].name), str8_varg(g->defines[j].value));
    }
  }

  printf("strip-const-prefix:  \"%.*s\"\n", str8_varg(c->strip_const_prefix));
  printf("strip-func-prefix:   \"%.*s\"\n", str8_varg(c->strip_func_prefix));
  printf("strip-struct-prefix: \"%.*s\"\n", str8_varg(c->strip_struct_prefix));

  printf("type-map (%llu):\n", (unsigned long long)dyn_count(c->type_map));
  foreach_index(i, dyn_count(c->type_map)) {
    printf("  %.*s -> %.*s\n", str8_varg(c->type_map[i].c_name), str8_varg(c->type_map[i].b3_name));
  }

  printf("pin-type (%llu):\n", (unsigned long long)dyn_count(c->pin_type));
  foreach_index(i, dyn_count(c->pin_type)) {
    String8 spelling = c->pin_type[i].c_spelling.size > 0 ? c->pin_type[i].c_spelling : c->pin_type[i].c_name;
    printf("  %.*s -> %.*s (emitted as C `%.*s`)\n",
           str8_varg(c->pin_type[i].c_name), str8_varg(c->pin_type[i].b3_name), str8_varg(spelling));
  }

  printf("const groups (%llu):\n", (unsigned long long)dyn_count(c->const_groups));
  foreach_index(i, dyn_count(c->const_groups)) {
    ConstGroup* g = &c->const_groups[i];
    printf("  %s %.*s: ", g->kind == ConstGroupKind_Enum ? "enum" : "flags", str8_varg(g->name));
    if (g->has_pattern) {
      printf("(match \"%.*s\")\n", str8_varg(g->pattern));
    } else {
      foreach_index(j, dyn_count(g->members)) printf("%.*s ", str8_varg(g->members[j]));
      printf("\n");
    }
  }

  printf("force-opaque (%llu):", (unsigned long long)dyn_count(c->force_opaque));
  foreach_index(i, dyn_count(c->force_opaque)) printf(" %.*s", str8_varg(c->force_opaque[i]));
  printf("\n");

  printf("outparam rules (%llu):\n", (unsigned long long)dyn_count(c->outparam_rules));
  foreach_index(i, dyn_count(c->outparam_rules)) {
    OutparamRule* r = &c->outparam_rules[i];
    if (r->kind == OutparamKind_Arena) {
      printf("  arena     %.*s [count-param %u out-param %u]\n", str8_varg(r->func_name), r->count_param_index, r->out_param_index);
    } else {
      printf("  %-9s %.*s [param %u]\n", r->kind == OutparamKind_Construct ? "construct" : "mutate", str8_varg(r->func_name), r->param_index);
    }
  }

  printf("func renames (%llu):\n", (unsigned long long)dyn_count(c->func_renames));
  foreach_index(i, dyn_count(c->func_renames)) {
    printf("  %.*s -> %.*s\n", str8_varg(c->func_renames[i].from), str8_varg(c->func_renames[i].to));
  }

  printf("const renames (%llu):\n", (unsigned long long)dyn_count(c->const_renames));
  foreach_index(i, dyn_count(c->const_renames)) {
    printf("  %.*s -> %.*s\n", str8_varg(c->const_renames[i].from), str8_varg(c->const_renames[i].to));
  }

  printf("type renames (%llu):\n", (unsigned long long)dyn_count(c->type_renames));
  foreach_index(i, dyn_count(c->type_renames)) {
    printf("  %.*s -> %.*s\n", str8_varg(c->type_renames[i].from), str8_varg(c->type_renames[i].to));
  }

  print_rename_patterns("const", c->const_rename_patterns);
  print_rename_patterns("func", c->func_rename_patterns);
  print_rename_patterns("type", c->type_rename_patterns);

  printf("excluded funcs (%llu):", (unsigned long long)dyn_count(c->excluded_funcs));
  foreach_index(i, dyn_count(c->excluded_funcs)) printf(" %.*s", str8_varg(c->excluded_funcs[i]));
  printf("\n");

  printf("excluded consts (%llu):", (unsigned long long)dyn_count(c->excluded_consts));
  foreach_index(i, dyn_count(c->excluded_consts)) printf(" %.*s", str8_varg(c->excluded_consts[i]));
  printf("\n");

  printf("skip deprecated: %s\n", c->skip_deprecated ? "yes" : "no");
}
