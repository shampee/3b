// format.c -- a syntax-level pretty-printer for `.3b` source.
//
// Operates purely on the untyped tree from parser.c (the same tree lower.c
// consumes before type resolution), so it can format source that doesn't
// type-check and needs no checker/lower/codegen. Every form is rendered on
// one line first (`fmt_render_inline`); that rendering serves as both the
// width-fit measurement and, when it fits, the output. The per-form
// handlers below run only when it doesn't fit.
//
// The lexer discards `;` comments as trivia before the parser sees a token,
// so the AST carries none. `fmt_scan_comments` re-scans the raw source into
// a flat, line-ordered list that the printer consumes in source order via
// `fmt_flush_comments_before`/`fmt_flush_trailing`; teaching the shared
// lexer to retain trivia instead would ripple into every other consumer for
// a detail only this file needs. Blank lines between ordinary code are not
// tracked, but a blank line between two consecutive comments is preserved.
#include "3b.h"

enum {
  FMT_WIDTH        = 100, // one-line budget, in columns
  FMT_INDENT       = 2,   // normal body/block indent step
  FMT_HANG_DEFAULT = 2,   // default continuation indent for a wrapped fn/struct signature
};

// Continuation indent for a wrapped `fn` param / `struct` field / `enum`
// variant vector, `3b format --hang`'s knob. Process-wide for the same
// reason the comment side-channel below is: `fmt_program` has one caller,
// and threading a settings struct through every printer would touch each
// one to serve a single number.
static u32 g_fmt_hang = FMT_HANG_DEFAULT;

////////////////////////////////
//~ Comment side-channel. Populated once per `fmt_program` call and consumed
// strictly left-to-right as the printer walks the tree in source order. A
// single process-wide instance suffices: `fmt_program` has one caller,
// main.c's `3b format`.

static void fmt_write_indent(FILE* out, u32 n); // defined below, in the multi-line printer section

typedef struct FmtComment {
  String8 text; // raw comment text incl. leading `;`, trailing whitespace trimmed
  u32     line; // 1-based source line the `;` starts on
} FmtComment;

typedef struct FmtComments {
  FmtComment* items;  // dyn array, ascending by line (scan is one forward pass)
  u32         cursor; // index of the next not-yet-printed comment
} FmtComments;

static FmtComments g_fmt_comments;

// Reproduces just enough of lexer.c's string-literal handling
// (quote/backslash tracking) to avoid mistaking a `;` or a line break inside
// a string literal for a comment start or a line boundary. Numbers,
// brackets and atoms need no handling, since none of them can hide a `;`.
static void
fmt_scan_comments(Arena* arena, String8 src) {
  g_fmt_comments.items  = NULL;
  g_fmt_comments.cursor = 0;

  u64 i      = 0;
  u32 line   = 1;
  b32 in_str = false;
  while (i < src.size) {
    u8 c = src.str[i];
    if (in_str) {
      if (c == '\\' && i + 1 < src.size) {
        if (src.str[i + 1] == '\n') line += 1;
        i += 2;
        continue;
      }
      if (c == '"') in_str = false;
      if (c == '\n') line += 1;
      i += 1;
      continue;
    }
    if (c == '"') {
      in_str = true;
      i += 1;
      continue;
    }
    if (c == ';') {
      u64 start = i;
      while (i < src.size && src.str[i] != '\n') i += 1;
      String8    text    = str8_skip_chop_whitespace(str8_substr(src, r1u64(start, i)));
      FmtComment comment = {text, line};
      dyn_push(arena, g_fmt_comments.items, comment);
      continue; // '\n' (if any) is handled by the next loop iteration
    }
    if (c == '\n') line += 1;
    i += 1;
  }
}

// Non-consuming peek: is any unconsumed comment interior to
// [start_line, end_line)? A node containing one must be forced into its
// multi-line form, since `fmt_try_inline` measures width alone and would
// render the comment away. The range excludes `end_line` itself: a comment
// on the node's last line is a trailing comment, left for whichever
// `fmt_flush_trailing` call reaches it first.
static b32
fmt_has_pending_comment_in(u32 start_line, u32 end_line) {
  u32 i = g_fmt_comments.cursor;
  u32 n = (u32)dyn_count(g_fmt_comments.items);
  while (i < n && g_fmt_comments.items[i].line < end_line) {
    if (g_fmt_comments.items[i].line >= start_line) return true;
    i += 1;
  }
  return false;
}

// Non-consuming peek: is there an unconsumed comment on exactly `line`?
// `fmt_pair_vector` aligns a run's comment column, so it must know which
// rows carry a trailing comment before printing any of them; consuming
// still happens later and in source order, via `fmt_flush_trailing`.
static b32
fmt_has_comment_at_line(u32 line) {
  u32 i = g_fmt_comments.cursor;
  u32 n = (u32)dyn_count(g_fmt_comments.items);
  while (i < n && g_fmt_comments.items[i].line < line) i += 1;
  return i < n && g_fmt_comments.items[i].line == line;
}

// Prints (and consumes) every pending comment before `before_line`, each on
// its own indented line. Must be called only at the start of a fresh output
// line: it emits indent+text+"\n" per comment and nothing else. A source
// blank line between two consecutive comments is preserved as one blank
// output line, collapsing runs of 2+. Comments consumed in one call are
// never separated by real code, so a line-number gap can only mean blanks.
static void
fmt_flush_comments_before(FILE* out, u32 indent, u32 before_line) {
  b32 first     = true;
  u32 prev_line = 0;
  while (g_fmt_comments.cursor < dyn_count(g_fmt_comments.items) &&
         g_fmt_comments.items[g_fmt_comments.cursor].line < before_line) {
    u32 line = g_fmt_comments.items[g_fmt_comments.cursor].line;
    if (!first && line > prev_line + 1) fprintf(out, "\n");
    fmt_write_indent(out, indent);
    fprintf(out, "%.*s\n", str8_varg(g_fmt_comments.items[g_fmt_comments.cursor].text));
    prev_line = line;
    first     = false;
    g_fmt_comments.cursor += 1;
  }
}

// Prints (and consumes) a pending comment on the line just finished, right
// after whatever was last written. Returns whether it printed one: a caller
// that hugs a closing bracket onto the same line must break to a fresh line
// instead, or the bracket lands after the `;` and is swallowed into the
// comment on re-parse.
static b32
fmt_flush_trailing(FILE* out, u32 line) {
  if (g_fmt_comments.cursor < dyn_count(g_fmt_comments.items) &&
      g_fmt_comments.items[g_fmt_comments.cursor].line == line) {
    fprintf(out, " %.*s", str8_varg(g_fmt_comments.items[g_fmt_comments.cursor].text));
    g_fmt_comments.cursor += 1;
    return true;
  }
  return false;
}

// The last source line `idx` touches: its last descendant leaf token's
// line, or its own open-bracket token line if it has no children. Locates
// the trailing same-line comment of a node that may span many lines.
static u32
fmt_node_end_line(Ast* ast, NodeIndex idx) {
  AstNode* node = ast_get(ast, idx);
  if (node->kind == AstNodeKind_Atom || node->kind == AstNodeKind_String) return node->token.line;
  u16        count;
  NodeIndex* children = ast_seq_children(ast, idx, &count);
  if (count == 0) return node->token.line;
  return fmt_node_end_line(ast, children[count - 1]);
}

////////////////////////////////
//~ Known-type-name pre-pass. Telling a 2-slot `name init` let binding from
// a 3-slot `name type init` one hinges on whether the middle slot names a
// known type, per lower.c's `let_slot_looks_like_type`. Primitives are
// answered by `is_primitive_type_name` (3b.c); struct/union/enum/flags/
// alias names are collected here in one syntactic sweep of the file
// (recursing into `private`/`extern` wrappers) rather than by building a
// real symbol table.

typedef struct FmtTypeNames {
  String8* names; // dyn array
} FmtTypeNames;

static void
fmt_collect_type_names_from_form(Ast* ast, NodeIndex idx, FmtTypeNames* out) {
  AstNode* node = ast_get(ast, idx);
  if (node->kind != AstNodeKind_List) return;
  u16        count;
  NodeIndex* children = ast_seq_children(ast, idx, &count);
  if (count == 0) return;
  AstNode* head = ast_get(ast, children[0]);
  if (head->kind != AstNodeKind_Atom) return;
  String8 op = head->token.text;

  if (str8_match_lit("private", op, 0) || str8_match_lit("extern", op, 0)) {
    for (u32 i = 1; i < count; i += 1) fmt_collect_type_names_from_form(ast, children[i], out);
    return;
  }

  b32 is_type_decl = str8_match_lit("struct", op, 0) || str8_match_lit("union", op, 0) ||
                      str8_match_lit("enum", op, 0) || str8_match_lit("flags", op, 0) ||
                      str8_match_lit("alias", op, 0);
  if (is_type_decl && count >= 2) {
    AstNode* name_node = ast_get(ast, children[1]);
    if (name_node->kind == AstNodeKind_Atom) {
      dyn_push(ctx_perm(), out->names, name_node->token.text);
    }
  }
}

static FmtTypeNames
fmt_collect_type_names(Ast* ast, NodeIndex root) {
  FmtTypeNames result = {0};
  u16          count;
  NodeIndex*   children = ast_seq_children(ast, root, &count);
  foreach_index(i, count) fmt_collect_type_names_from_form(ast, children[i], &result);
  return result;
}

static b32
fmt_is_known_type_atom(FmtTypeNames* types, String8 name) {
  String8 base = name;
  while (base.size > 0 && base.str[base.size - 1] == '*') base = str8_chop(base, 1);
  if (base.size == 0) return false;
  if (is_primitive_type_name(base)) return true;
  foreach_index(i, dyn_count(types->names)) {
    if (str8_match(types->names[i], base, 0)) return true;
  }
  return false;
}

// Mirrors lower.c's `let_slot_looks_like_type`, against the syntactic name
// set collected above instead of a Lowerer's symbol tables.
static b32
fmt_let_slot_looks_like_type(Ast* ast, NodeIndex idx, FmtTypeNames* types) {
  AstNode* node = ast_get(ast, idx);
  if (node->kind == AstNodeKind_Atom) return fmt_is_known_type_atom(types, node->token.text);
  if (node->kind == AstNodeKind_Vector) return true;
  if (node->kind == AstNodeKind_List) {
    u16        lc;
    NodeIndex* lchildren = ast_seq_children(ast, idx, &lc);
    AstNode*   lhead     = lc > 0 ? ast_get(ast, lchildren[0]) : NULL;
    return lhead && lhead->kind == AstNodeKind_Atom && str8_match_lit("member-type", lhead->token.text, 0);
  }
  return false;
}

////////////////////////////////
//~ Inline (single-line) rendering

// Builds directly into the caller's `arena`. A nested scratch temp would
// not work: callers routinely pass a ctx_scratch()-backed arena that also
// backs an outer ArenaTemp, so rolling back here frees the returned
// string's bytes out from under them.
static String8
fmt_escape_string(Arena* arena, String8 s) {
  u8* buf = NULL;
  dyn_push(arena, buf, (u8)'"');
  foreach_index(i, s.size) {
    u8 c   = s.str[i];
    u8 esc = 0;
    switch (c) {
      case '\n': esc = 'n';  break;
      case '\t': esc = 't';  break;
      case '\r': esc = 'r';  break;
      case '\\': esc = '\\'; break;
      case '"':  esc = '"';  break;
      case 0:    esc = '0';  break;
      default:                break;
    }
    if (esc) {
      dyn_push(arena, buf, (u8)'\\');
      dyn_push(arena, buf, esc);
    } else {
      dyn_push(arena, buf, c);
    }
  }
  dyn_push(arena, buf, (u8)'"');
  return str8(buf, dyn_count(buf));
}

static String8 fmt_render_inline(Arena* arena, Ast* ast, NodeIndex idx);

static String8
fmt_render_seq_inline(Arena* arena, Ast* ast, NodeIndex idx, String8 open, String8 close) {
  u16         count;
  NodeIndex*  children = ast_seq_children(ast, idx, &count);
  String8List parts     = {0};
  str8_list_push(arena, &parts, open);
  foreach_index(i, count) {
    if (i > 0) str8_list_push(arena, &parts, str8_lit(" "));
    str8_list_push(arena, &parts, fmt_render_inline(arena, ast, children[i]));
  }
  str8_list_push(arena, &parts, close);
  return str8_list_join(arena, &parts, NULL);
}

static String8
fmt_render_inline(Arena* arena, Ast* ast, NodeIndex idx) {
  AstNode* node = ast_get(ast, idx);
  switch (node->kind) {
    case AstNodeKind_Atom:   return node->token.text;
    case AstNodeKind_String: return fmt_escape_string(arena, node->token.text);
    case AstNodeKind_List:   return fmt_render_seq_inline(arena, ast, idx, str8_lit("("), str8_lit(")"));
    case AstNodeKind_Vector: return fmt_render_seq_inline(arena, ast, idx, str8_lit("["), str8_lit("]"));
    case AstNodeKind_Map:    return fmt_render_seq_inline(arena, ast, idx, str8_lit("{"), str8_lit("}"));
    default:                 return str8_lit("");
  }
}

////////////////////////////////
//~ Multi-line printer

static void
fmt_write_indent(FILE* out, u32 n) {
  for (u32 i = 0; i < n; i += 1) fputc(' ', out);
}

// Does `idx` render as a single output line at the given column: within
// FMT_WIDTH, and with no comment inside its line range? (`fmt_render_inline`
// sees only tokens, so it would render straight past a comment that depends
// on a line break.) Writes the rendered text to `*out` if so. Separate from
// `fmt_try_inline` for callers that must know several nodes' widths before
// printing any of them, such as `fmt_pair_vector`'s comment alignment.
static b32
fmt_measure_inline(Arena* arena, Ast* ast, NodeIndex idx, u32 indent, String8* out) {
  AstNode* node = ast_get(ast, idx);
  if (fmt_has_pending_comment_in(node->token.line, fmt_node_end_line(ast, idx))) return false;
  String8 s = fmt_render_inline(arena, ast, idx);
  if (indent + s.size > FMT_WIDTH) return false;
  *out = s;
  return true;
}

// Renders `idx` inline and prints it iff it fits within FMT_WIDTH at the
// given column; returns whether it printed anything.
static b32
fmt_try_inline(FILE* out, Ast* ast, NodeIndex idx, u32 indent) {
  ArenaTemp temp = arena_temp_begin(ctx_scratch());
  String8   s;
  b32       fits = fmt_measure_inline(temp.arena, ast, idx, indent, &s);
  if (fits) fprintf(out, "%.*s", str8_varg(s));
  arena_temp_end(&temp);
  return fits;
}

static void fmt_node(FILE* out, Ast* ast, NodeIndex idx, u32 indent, FmtTypeNames* types);

// Shared by `fn` params and `struct`/`union` fields: a vector of name/type
// pairs, one pair per line once broken, names left-padded so every type
// lands in a shared column. Prints the opening `[` and every pair; the
// caller appends what follows the closing pair (`] ret)` for `fn`, `])` for
// `struct`). Returns whether a trailing comment was printed after the last
// pair, in which case the caller must break to a fresh line before hugging
// its own closer on, or the closer lands inside the comment.
//
// A comment between `[` and the first pair is not placed specially; it
// bubbles up to the next flush point, as in fmt_call/fmt_seq_generic.
static b32
fmt_pair_vector(FILE* out, Ast* ast, NodeIndex vec_idx, u32 col, FmtTypeNames* types) {
  u16        count;
  NodeIndex* children   = ast_seq_children(ast, vec_idx, &count);
  u16        pair_count = count / 2;

  u32 max_name = 0;
  for (u32 i = 0; i < pair_count; i += 1) {
    AstNode* name_node = ast_get(ast, children[i * 2]);
    u32      w         = (u32)name_node->token.text.size;
    if (w > max_name) max_name = w;
  }
  u32 value_col = col + 1 + max_name + 1;

  // Comment-column alignment pre-pass: measuring every pair's value up
  // front lets a run of consecutive one-line pairs that all carry trailing
  // comments push those comments out to a shared column, the widest value
  // in the run. A pair whose value wraps, or which has no trailing comment,
  // ends the run and gets a single space before its comment instead.
  ArenaTemp temp     = arena_temp_begin(ctx_scratch());
  String8*  rendered = NULL; // valid iff simple[i]
  b32*      simple   = NULL;
  b32*      has_cmt  = NULL;
  for (u32 i = 0; i < pair_count; i += 1) {
    NodeIndex value_idx = children[i * 2 + 1];
    String8   s         = {0};
    b32       ok        = fmt_measure_inline(temp.arena, ast, value_idx, value_col, &s);
    dyn_push(temp.arena, rendered, s);
    dyn_push(temp.arena, simple, ok);
    dyn_push(temp.arena, has_cmt, ok && fmt_has_comment_at_line(fmt_node_end_line(ast, value_idx)));
  }
  u32* pad_width = NULL; // per-pair target value width; unused where !has_cmt[i]
  for (u32 i = 0; i < pair_count;) {
    if (!has_cmt[i]) {
      dyn_push(temp.arena, pad_width, (u32)0);
      i += 1;
      continue;
    }
    u32 run_start = i;
    u32 max_w     = 0;
    while (i < pair_count && has_cmt[i]) {
      if ((u32)rendered[i].size > max_w) max_w = (u32)rendered[i].size;
      i += 1;
    }
    for (u32 k = run_start; k < i; k += 1) dyn_push(temp.arena, pad_width, max_w);
  }

  fprintf(out, "[");
  b32 trailing = false;
  for (u32 i = 0; i < pair_count; i += 1) {
    AstNode* name_node = ast_get(ast, children[i * 2]);
    if (i > 0) {
      fprintf(out, "\n");
      fmt_flush_comments_before(out, col + 1, name_node->token.line);
      fmt_write_indent(out, col + 1);
    }
    fprintf(out, "%.*s", str8_varg(name_node->token.text));
    fmt_write_indent(out, max_name - (u32)name_node->token.text.size + 1);
    if (simple[i]) {
      fprintf(out, "%.*s", str8_varg(rendered[i]));
      if (has_cmt[i] && pad_width[i] > rendered[i].size) fmt_write_indent(out, pad_width[i] - (u32)rendered[i].size);
    } else {
      fmt_node(out, ast, children[i * 2 + 1], value_col, types);
    }
    trailing = fmt_flush_trailing(out, fmt_node_end_line(ast, children[i * 2 + 1]));
  }
  arena_temp_end(&temp);
  return trailing;
}

// Generic call fallback: one line if it fits, else `(head arg1` on the
// opening line with the rest one per line aligned under arg1's column and
// the closing paren hugging the last argument. Matches the house style of
// hand-written .3b source: no dangling closer-only lines.
static void
fmt_call(FILE* out, Ast* ast, NodeIndex idx, NodeIndex* children, u16 count, u32 indent, FmtTypeNames* types) {
  if (fmt_try_inline(out, ast, idx, indent)) return;
  if (count == 0) {
    fprintf(out, "()");
    return;
  }

  ArenaTemp temp   = arena_temp_begin(ctx_scratch());
  String8   head_s = fmt_render_inline(temp.arena, ast, children[0]);
  fprintf(out, "(%.*s", str8_varg(head_s));
  u32 align_col = indent + 1 + (u32)head_s.size + 1;
  arena_temp_end(&temp);

  b32 trailing = false;
  if (count > 1) {
    fprintf(out, " ");
    fmt_node(out, ast, children[1], align_col, types);
    trailing = fmt_flush_trailing(out, fmt_node_end_line(ast, children[1]));
    for (u32 i = 2; i < count; i += 1) {
      fprintf(out, "\n");
      fmt_flush_comments_before(out, align_col, ast_get(ast, children[i])->token.line);
      fmt_write_indent(out, align_col);
      fmt_node(out, ast, children[i], align_col, types);
      trailing = fmt_flush_trailing(out, fmt_node_end_line(ast, children[i]));
    }
  }
  if (trailing) { fprintf(out, "\n"); fmt_write_indent(out, indent); }
  fprintf(out, ")");
}

// Generic Vector/Map fallback (array literals, and anything not otherwise
// special-cased below). Map entries are kept as key/value pairs when
// broken; everything else breaks one child per line, aligned under the
// first child's column.
static void
fmt_seq_generic(FILE* out, Ast* ast, NodeIndex idx, u32 indent, FmtTypeNames* types) {
  if (fmt_try_inline(out, ast, idx, indent)) return;

  AstNode* node   = ast_get(ast, idx);
  b32      is_map = node->kind == AstNodeKind_Map;
  String8  open   = is_map ? str8_lit("{") : str8_lit("[");
  String8  close  = is_map ? str8_lit("}") : str8_lit("]");

  u16        count;
  NodeIndex* children = ast_seq_children(ast, idx, &count);
  fprintf(out, "%.*s", str8_varg(open));
  if (count == 0) {
    fprintf(out, "%.*s", str8_varg(close));
    return;
  }

  u32 align_col = indent + 1;
  u32 step      = (is_map && count % 2 == 0) ? 2 : 1;
  b32 trailing  = false;
  for (u32 i = 0; i < count; i += step) {
    if (i > 0) {
      fprintf(out, "\n");
      fmt_flush_comments_before(out, align_col, ast_get(ast, children[i])->token.line);
      fmt_write_indent(out, align_col);
    }
    fmt_node(out, ast, children[i], align_col, types);
    trailing = fmt_flush_trailing(out, fmt_node_end_line(ast, children[i]));
    if (step == 2 && i + 1 < count) {
      fprintf(out, " ");
      fmt_node(out, ast, children[i + 1], align_col, types);
      trailing = fmt_flush_trailing(out, fmt_node_end_line(ast, children[i + 1]));
    }
  }
  if (trailing) { fprintf(out, "\n"); fmt_write_indent(out, indent); }
  fprintf(out, "%.*s", str8_varg(close));
}

// `(fn name [param-pairs] ret body...)`. A bodyless form (count == 4) is an
// extern signature (lower.c's lower_extern_fn) and stays on one line
// regardless of arity or length. A body-bearing form breaks into the
// aligned multi-line shape once it has more than one param.
static void
fmt_fn(FILE* out, Ast* ast, NodeIndex idx, NodeIndex* children, u16 count, u32 indent, FmtTypeNames* types) {
  if (count < 4) {
    fmt_call(out, ast, idx, children, count, indent, types);
    return;
  }
  if (count == 4) {
    ArenaTemp temp = arena_temp_begin(ctx_scratch());
    String8   s    = fmt_render_inline(temp.arena, ast, idx);
    fprintf(out, "%.*s", str8_varg(s));
    arena_temp_end(&temp);
    return;
  }

  NodeIndex name_idx   = children[1];
  NodeIndex params_idx = children[2];
  NodeIndex ret_idx    = children[3];
  AstNode*  name_node  = ast_get(ast, name_idx);

  u16 flat_count;
  ast_seq_children(ast, params_idx, &flat_count);
  u16 pair_count = flat_count / 2;

  b32 header_inline = false;
  if (pair_count <= 1 && !fmt_has_pending_comment_in(name_node->token.line, fmt_node_end_line(ast, ret_idx))) {
    ArenaTemp   temp  = arena_temp_begin(ctx_scratch());
    String8List parts = {0};
    str8_list_push(temp.arena, &parts, str8_lit("(fn "));
    str8_list_push(temp.arena, &parts, name_node->token.text);
    str8_list_push(temp.arena, &parts, str8_lit(" "));
    str8_list_push(temp.arena, &parts, fmt_render_inline(temp.arena, ast, params_idx));
    str8_list_push(temp.arena, &parts, str8_lit(" "));
    str8_list_push(temp.arena, &parts, fmt_render_inline(temp.arena, ast, ret_idx));
    String8 header = str8_list_join(temp.arena, &parts, NULL);
    if (indent + header.size <= FMT_WIDTH) {
      fprintf(out, "%.*s", str8_varg(header));
      header_inline = true;
    }
    arena_temp_end(&temp);
  }

  if (!header_inline) {
    fprintf(out, "(fn %.*s\n", str8_varg(name_node->token.text));
    fmt_write_indent(out, indent + g_fmt_hang);
    b32 params_trailing = fmt_pair_vector(out, ast, params_idx, indent + g_fmt_hang, types);
    if (params_trailing) { fprintf(out, "\n"); fmt_write_indent(out, indent + g_fmt_hang); }
    fprintf(out, "] ");
    ArenaTemp temp  = arena_temp_begin(ctx_scratch());
    String8   ret_s = fmt_render_inline(temp.arena, ast, ret_idx);
    fprintf(out, "%.*s", str8_varg(ret_s));
    arena_temp_end(&temp);
  }

  fprintf(out, "\n");
  b32 body_trailing = false;
  for (u32 i = 4; i < count; i += 1) {
    fmt_flush_comments_before(out, indent + FMT_INDENT, ast_get(ast, children[i])->token.line);
    fmt_write_indent(out, indent + FMT_INDENT);
    fmt_node(out, ast, children[i], indent + FMT_INDENT, types);
    body_trailing = fmt_flush_trailing(out, fmt_node_end_line(ast, children[i]));
    if (i + 1 < count) fprintf(out, "\n");
  }
  if (body_trailing) { fprintf(out, "\n"); fmt_write_indent(out, indent); }
  fprintf(out, ")");
}

// `(struct name [field-pairs])` / `(union ...)` -- 3 children, or 4 with a
// translated mirror's pinned C spelling in between (lower_struct_decl), never
// a body. Same pair-vector treatment as `fn` params, keyed off field-pair
// count instead of param count.
static void
fmt_struct(FILE* out, Ast* ast, NodeIndex idx, NodeIndex* children, u16 count, u32 indent, FmtTypeNames* types) {
  if (count != 3 && count != 4) {
    fmt_call(out, ast, idx, children, count, indent, types);
    return;
  }
  NodeIndex fields_idx = children[count - 1];
  u16       flat_count;
  ast_seq_children(ast, fields_idx, &flat_count);
  u16 pair_count = flat_count / 2;

  if (pair_count <= 1 && fmt_try_inline(out, ast, idx, indent)) return;

  AstNode* head_node = ast_get(ast, children[0]);
  AstNode* name_node = ast_get(ast, children[1]);
  fprintf(out, "(%.*s %.*s", str8_varg(head_node->token.text), str8_varg(name_node->token.text));
  if (count == 4) fprintf(out, " \"%.*s\"", str8_varg(ast_get(ast, children[2])->token.text));
  fprintf(out, "\n");
  fmt_write_indent(out, indent + g_fmt_hang);
  b32 trailing = fmt_pair_vector(out, ast, fields_idx, indent + g_fmt_hang, types);
  if (trailing) { fprintf(out, "\n"); fmt_write_indent(out, indent + g_fmt_hang); }
  fprintf(out, "])");
}

// `(enum name [variants])` / `(flags ...)` -- 3 children, or 4 with a pinned C
// spelling, like fmt_struct above. Each variant is `name` or `name value`,
// with `value` recognized syntactically via atom_looks_numeric, as in
// lower_enum_decl. No column alignment: the value suffix is optional per
// variant, unlike fn/struct's uniform name+type pairs.
static void
fmt_enum(FILE* out, Ast* ast, NodeIndex idx, NodeIndex* children, u16 count, u32 indent, FmtTypeNames* types) {
  (void)types;
  if (count != 3 && count != 4) {
    fmt_call(out, ast, idx, children, count, indent, types);
    return;
  }
  if (fmt_try_inline(out, ast, idx, indent)) return;

  AstNode*   head_node    = ast_get(ast, children[0]);
  AstNode*   name_node    = ast_get(ast, children[1]);
  NodeIndex  variants_idx = children[count - 1];
  u16        vcount;
  NodeIndex* variants = ast_seq_children(ast, variants_idx, &vcount);

  fprintf(out, "(%.*s %.*s", str8_varg(head_node->token.text), str8_varg(name_node->token.text));
  if (count == 4) fprintf(out, " \"%.*s\"", str8_varg(ast_get(ast, children[2])->token.text));
  fprintf(out, "\n");
  u32 col = indent + g_fmt_hang;
  fmt_write_indent(out, col);
  fprintf(out, "[");
  b32 first    = true;
  b32 trailing = false;
  for (u32 i = 0; i < vcount;) {
    AstNode* vname = ast_get(ast, variants[i]);
    if (!first) {
      fprintf(out, "\n");
      fmt_flush_comments_before(out, col + 1, vname->token.line);
      fmt_write_indent(out, col + 1);
    }
    first = false;
    fprintf(out, "%.*s", str8_varg(vname->token.text));
    i += 1;
    NodeIndex last_child = variants[i - 1];
    if (i < vcount) {
      AstNode* maybe_val = ast_get(ast, variants[i]);
      if (maybe_val->kind == AstNodeKind_Atom && atom_looks_numeric(maybe_val->token.text)) {
        fprintf(out, " %.*s", str8_varg(maybe_val->token.text));
        last_child = variants[i];
        i += 1;
      }
    }
    trailing = fmt_flush_trailing(out, ast_get(ast, last_child)->token.line);
  }
  if (trailing) { fprintf(out, "\n"); fmt_write_indent(out, col); }
  fprintf(out, "])");
}

// `(let [bindings] body...)`. Bindings are grouped as lower_let groups
// them: a `{}`/`[]` destructuring entry is 2 slots (pattern, source); an
// atom entry is 3 slots (name type init) if the next slot looks like a type
// (fmt_let_slot_looks_like_type), else 2 (name init). One group per line
// once there is more than one, aligned under the bindings vector's `[`.
// Init expressions are often nested, so unlike fn/struct's name+type pairs
// they get no column alignment. Body forms go one per line under `(let`.
static void
fmt_let(FILE* out, Ast* ast, NodeIndex idx, NodeIndex* children, u16 count, u32 indent, FmtTypeNames* types) {
  if (count < 3) {
    fmt_call(out, ast, idx, children, count, indent, types);
    return;
  }
  NodeIndex  bindings_idx = children[1];
  u16        flat_count;
  NodeIndex* flat = ast_seq_children(ast, bindings_idx, &flat_count);

  typedef struct { u32 start, len; } FmtGroup;
  ArenaTemp gtemp  = arena_temp_begin(ctx_scratch());
  FmtGroup* groups = NULL;
  u32       i      = 0;
  while (i < flat_count) {
    AstNode* slot0 = ast_get(ast, flat[i]);
    FmtGroup g     = {0};
    g.start        = i;
    if (slot0->kind == AstNodeKind_Map || slot0->kind == AstNodeKind_Vector) {
      g.len = (i + 1 < flat_count) ? 2 : 1;
    } else if (i + 1 < flat_count && fmt_let_slot_looks_like_type(ast, flat[i + 1], types)) {
      g.len = (i + 2 < flat_count) ? 3 : 2;
    } else {
      g.len = (i + 1 < flat_count) ? 2 : 1;
    }
    dyn_push(gtemp.arena, groups, g);
    i += g.len;
  }
  u32 group_count = (u32)dyn_count(groups);

  fprintf(out, "(let [");
  u32 cont_col = indent + 6; // "(let [" is 6 chars -- aligns continuation groups under '['
  b32 trailing = false;
  for (u32 g = 0; g < group_count; g += 1) {
    FmtGroup grp = groups[g];
    if (g > 0) {
      fprintf(out, "\n");
      fmt_flush_comments_before(out, cont_col, ast_get(ast, flat[grp.start])->token.line);
      fmt_write_indent(out, cont_col);
    }
    for (u32 k = 0; k < grp.len; k += 1) {
      if (k > 0) fprintf(out, " ");
      fmt_node(out, ast, flat[grp.start + k], cont_col, types);
    }
    trailing = fmt_flush_trailing(out, fmt_node_end_line(ast, flat[grp.start + grp.len - 1]));
  }
  arena_temp_end(&gtemp);
  if (trailing) { fprintf(out, "\n"); fmt_write_indent(out, cont_col); }
  fprintf(out, "]");

  b32 body_trailing = false;
  for (u32 b = 2; b < count; b += 1) {
    fprintf(out, "\n");
    fmt_flush_comments_before(out, indent + FMT_INDENT, ast_get(ast, children[b])->token.line);
    fmt_write_indent(out, indent + FMT_INDENT);
    fmt_node(out, ast, children[b], indent + FMT_INDENT, types);
    body_trailing = fmt_flush_trailing(out, fmt_node_end_line(ast, children[b]));
  }
  if (body_trailing) { fprintf(out, "\n"); fmt_write_indent(out, indent); }
  fprintf(out, ")");
}

// `(private decl...)` / `(extern decl...)` -- wraps one or more full
// declaration forms. Collapses to one line only when there's a single
// child and the whole thing fits; otherwise each declaration goes on its
// own line.
static void
fmt_private_extern(FILE* out, Ast* ast, NodeIndex idx, NodeIndex* children, u16 count, u32 indent,
                    FmtTypeNames* types) {
  if (count < 2) {
    fmt_call(out, ast, idx, children, count, indent, types);
    return;
  }
  if (count == 2 && fmt_try_inline(out, ast, idx, indent)) return;

  AstNode* head_node = ast_get(ast, children[0]);
  fprintf(out, "(%.*s\n", str8_varg(head_node->token.text));
  b32 trailing = false;
  for (u32 i = 1; i < count; i += 1) {
    fmt_flush_comments_before(out, indent + FMT_INDENT, ast_get(ast, children[i])->token.line);
    fmt_write_indent(out, indent + FMT_INDENT);
    fmt_node(out, ast, children[i], indent + FMT_INDENT, types);
    trailing = fmt_flush_trailing(out, fmt_node_end_line(ast, children[i]));
    if (i + 1 < count) fprintf(out, "\n");
  }
  if (trailing) { fprintf(out, "\n"); fmt_write_indent(out, indent); }
  fprintf(out, ")");
}

// `if`/`when`/`while`/`for`/`do`: fits on one line, else the keyword plus
// `header_arity` header children (the condition for if/when/while, the
// clause vector for for, none for do) stay on the opening line, and every
// remaining child becomes a body line indented under it.
static void
fmt_block_form(FILE* out, Ast* ast, NodeIndex idx, NodeIndex* children, u16 count, u32 indent, FmtTypeNames* types,
                u32 header_arity) {
  if (fmt_try_inline(out, ast, idx, indent)) return;

  AstNode* head_node = ast_get(ast, children[0]);
  fprintf(out, "(%.*s", str8_varg(head_node->token.text));
  for (u32 i = 1; i <= header_arity && i < count; i += 1) {
    fprintf(out, " ");
    ArenaTemp temp = arena_temp_begin(ctx_scratch());
    String8   hs   = fmt_render_inline(temp.arena, ast, children[i]);
    fprintf(out, "%.*s", str8_varg(hs));
    arena_temp_end(&temp);
  }
  fprintf(out, "\n");
  u32 body_start = 1 + header_arity;
  b32 trailing   = false;
  for (u32 i = body_start; i < count; i += 1) {
    fmt_flush_comments_before(out, indent + FMT_INDENT, ast_get(ast, children[i])->token.line);
    fmt_write_indent(out, indent + FMT_INDENT);
    fmt_node(out, ast, children[i], indent + FMT_INDENT, types);
    trailing = fmt_flush_trailing(out, fmt_node_end_line(ast, children[i]));
    if (i + 1 < count) fprintf(out, "\n");
  }
  if (trailing) { fprintf(out, "\n"); fmt_write_indent(out, indent); }
  fprintf(out, ")");
}

// Main recursive dispatcher, mirroring lower_list's head-atom dispatch to
// route to the per-form handlers above. Anything not special-cased
// (val/var/alias, operators, plain calls, struct construction, threading
// macros) falls through to the generic call formatter, which suffices.
static void
fmt_node(FILE* out, Ast* ast, NodeIndex idx, u32 indent, FmtTypeNames* types) {
  AstNode* node = ast_get(ast, idx);

  if (node->kind == AstNodeKind_Atom || node->kind == AstNodeKind_String) {
    ArenaTemp temp = arena_temp_begin(ctx_scratch());
    String8   s    = fmt_render_inline(temp.arena, ast, idx);
    fprintf(out, "%.*s", str8_varg(s));
    arena_temp_end(&temp);
    return;
  }

  if (node->kind == AstNodeKind_List) {
    u16        count;
    NodeIndex* children = ast_seq_children(ast, idx, &count);
    if (count > 0) {
      AstNode* head = ast_get(ast, children[0]);
      if (head->kind == AstNodeKind_Atom) {
        String8 op = head->token.text;
        if (str8_match_lit("fn", op, 0)) {
          fmt_fn(out, ast, idx, children, count, indent, types);
          return;
        }
        if (str8_match_lit("struct", op, 0) || str8_match_lit("union", op, 0)) {
          fmt_struct(out, ast, idx, children, count, indent, types);
          return;
        }
        if (str8_match_lit("enum", op, 0) || str8_match_lit("flags", op, 0)) {
          fmt_enum(out, ast, idx, children, count, indent, types);
          return;
        }
        if (str8_match_lit("let", op, 0)) {
          fmt_let(out, ast, idx, children, count, indent, types);
          return;
        }
        if (str8_match_lit("private", op, 0) || str8_match_lit("extern", op, 0)) {
          fmt_private_extern(out, ast, idx, children, count, indent, types);
          return;
        }
        if (str8_match_lit("if", op, 0) || str8_match_lit("when", op, 0) || str8_match_lit("while", op, 0) ||
            str8_match_lit("for", op, 0)) {
          fmt_block_form(out, ast, idx, children, count, indent, types, 1);
          return;
        }
        if (str8_match_lit("do", op, 0)) {
          fmt_block_form(out, ast, idx, children, count, indent, types, 0);
          return;
        }
      }
    }
    fmt_call(out, ast, idx, children, count, indent, types);
    return;
  }

  fmt_seq_generic(out, ast, idx, indent, types);
}

// Head atom of a top-level form (`val`/`var`/`fn`/...), or an empty string
// if `idx` is not a headed list. Only decides top-level blank-line spacing
// below, so an unrecognized head falls back to the default (blank line).
static String8
fmt_top_level_op(Ast* ast, NodeIndex idx) {
  AstNode* node = ast_get(ast, idx);
  if (node->kind != AstNodeKind_List) return str8_lit("");
  u16        count;
  NodeIndex* children = ast_seq_children(ast, idx, &count);
  if (count == 0) return str8_lit("");
  AstNode* head = ast_get(ast, children[0]);
  if (head->kind != AstNodeKind_Atom) return str8_lit("");
  return head->token.text;
}

// `val`/`var`/`alias` are dense one-liner declarations that read better
// packed tight, so consecutive ones get no forced blank line between them.
// Every other top-level form does.
static b32
fmt_is_no_gap_op(String8 op) {
  return str8_match_lit("val", op, 0) || str8_match_lit("var", op, 0) || str8_match_lit("alias", op, 0);
}

////////////////////////////////
//~ Public entry point

// Formats a whole parsed file to `out`. `src` must be the exact text `ast`
// was parsed from, since the comment side-channel re-scans it by line.
// `hang` is the wrapped-signature continuation indent (`3b format --hang`);
// 0 asks for the default.
void
fmt_program(FILE* out, Ast* ast, NodeIndex root, String8 src, u32 hang) {
  g_fmt_hang = hang > 0 ? hang : FMT_HANG_DEFAULT;
  fmt_scan_comments(ctx_perm(), src);

  FmtTypeNames types = fmt_collect_type_names(ast, root);
  u16          count;
  NodeIndex*   children = ast_seq_children(ast, root, &count);
  for (u32 i = 0; i < count; i += 1) {
    fmt_flush_comments_before(out, 0, ast_get(ast, children[i])->token.line);
    fmt_node(out, ast, children[i], 0, &types);
    fmt_flush_trailing(out, fmt_node_end_line(ast, children[i]));
    fprintf(out, "\n");
    if (i + 1 < count) {
      String8 cur_op   = fmt_top_level_op(ast, children[i]);
      String8 next_op  = fmt_top_level_op(ast, children[i + 1]);
      b32     both_dense = fmt_is_no_gap_op(cur_op) && fmt_is_no_gap_op(next_op);
      b32     either_fn  = str8_match_lit("fn", cur_op, 0) || str8_match_lit("fn", next_op, 0);
      if (either_fn || !both_dense) fprintf(out, "\n");
    }
  }
  fmt_flush_comments_before(out, 0, max_u32); // catch any comment trailing the last form, e.g. an EOF comment
}
