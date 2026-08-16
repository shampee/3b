// Builds the generic AST: `expr := atom | string | list`, with `[...]` and
// `{...}` as two further sequence kinds. No special form is recognized here --
// `fn`, `let` and the rest first mean something in lower.c. This file also
// owns the Ast node constructors and the debug printer.
#include "3b.h"

void
parser_advance(Parser* p) {
  p->cur = lexer_next(&p->lex, p->arena);
}

void
parser_init(Parser* p, String8 src, Ast* ast, u32 file_id) {
  lexer_init(&p->lex, src, file_id);
  p->ast       = ast;
  p->arena     = ast->arena;
  p->had_error = false;
  parser_advance(p);
}

static const char*
seq_kind_label(AstNodeKind kind) {
  switch (kind) {
  case AstNodeKind_List  : return "list";
  case AstNodeKind_Vector: return "vector";
  case AstNodeKind_Map   : return "map";
  default                : return "sequence";
  }
}

static b32
token_is_any_closer(TokenKind kind) {
  return kind == TokenKind_RParen || kind == TokenKind_RBracket || kind == TokenKind_RBrace;
}

// Shared by (...), [...] and {...}, which differ only in delimiter and node
// kind. Any closer other than `close_kind` is a mismatch -- a stray ')' inside
// a '[...]' -- and is reported without being consumed, so that whichever
// enclosing call is looking for it still finds it.
NodeIndex
parse_bracketed(Parser* p, TokenKind close_kind, const char* close_str, AstNodeKind node_kind) {
  Token open_tok = p->cur; // the opening delimiter token, kept for position info
  parser_advance(p);       // consume the opening delimiter

  ArenaTemp  temp     = arena_temp_begin(ctx_scratch());
  NodeIndex* children = NULL;

  while (p->cur.kind != close_kind) {
    if (p->cur.kind == TokenKind_EOF) {
      diag_error(open_tok, "unterminated %s (expected `%s`)", seq_kind_label(node_kind), close_str);
      p->had_error = true;
      break;
    }
    if (token_is_any_closer(p->cur.kind)) {
      diag_error(p->cur,
                 "mismatched closing delimiter `%.*s` -- expected `%s` to close the %s opened at %u:%u",
                 str8_varg(p->cur.text), close_str, seq_kind_label(node_kind), open_tok.line, open_tok.col);
      p->had_error = true;
      break; // don't consume the wrong closer; let the real owner find it
    }
    NodeIndex child = parse_expr(p);
    dyn_push(temp.arena, children, child);
  }
  if (p->cur.kind == close_kind) {
    parser_advance(p); // consume the matching closer
  }

  u64 raw_count = dyn_count(children);
  if (raw_count > max_u16) {
    diag_error(open_tok, "%s has too many children (%llu > %u)", seq_kind_label(node_kind),
               (unsigned long long)raw_count, (u32)max_u16);
    p->had_error = true;
    raw_count    = max_u16;
  }
  NodeIndex result = ast_push_seq(p->ast, node_kind, open_tok, children, (u16)raw_count);
  arena_temp_end(&temp);
  return result;
}

NodeIndex
parse_expr(Parser* p) {
  switch (p->cur.kind) {
  case TokenKind_LParen: {
    return parse_bracketed(p, TokenKind_RParen, ")", AstNodeKind_List);
  }
  case TokenKind_LBracket: {
    return parse_bracketed(p, TokenKind_RBracket, "]", AstNodeKind_Vector);
  }
  case TokenKind_LBrace: {
    return parse_bracketed(p, TokenKind_RBrace, "}", AstNodeKind_Map);
  }
  case TokenKind_Atom: {
    NodeIndex n = ast_push_atom(p->ast, p->cur);
    parser_advance(p);
    return n;
  }
  case TokenKind_String: {
    NodeIndex n = ast_push_string(p->ast, p->cur);
    parser_advance(p);
    return n;
  }
  case TokenKind_RParen:
  case TokenKind_RBracket:
  case TokenKind_RBrace  : {
    diag_error(p->cur, "unexpected `%.*s`", str8_varg(p->cur.text));
    p->had_error = true;
    parser_advance(p);
    return NODE_NIL;
  }
  case TokenKind_Error: {
    diag_error(p->cur, "%.*s", str8_varg(p->cur.text));
    p->had_error = true;
    parser_advance(p);
    return NODE_NIL;
  }
  default: {
    diag_error(p->cur, "unexpected token");
    p->had_error = true;
    parser_advance(p);
    return NODE_NIL;
  }
  }
}

// Parses a whole file as one implicit top-level list, so callers get a single
// root NodeIndex back.
NodeIndex
parse_program(Parser* p) {
  Token synth_open = { 0 };
  synth_open.line  = 1;
  synth_open.col   = 1;

  ArenaTemp  temp     = arena_temp_begin(ctx_scratch());
  NodeIndex* children = NULL;

  while (p->cur.kind != TokenKind_EOF) {
    NodeIndex child = parse_expr(p);
    dyn_push(temp.arena, children, child);
  }

  u64 raw_count = dyn_count(children);
  if (raw_count > max_u16) raw_count = max_u16; // same clamp as parse_bracketed
  NodeIndex root = ast_push_seq(p->ast, AstNodeKind_List, synth_open, children, (u16)raw_count);
  arena_temp_end(&temp);
  return root;
}

void
ast_init(Ast* ast, Arena* arena) {
  ast->arena = arena;
  ast->nodes = NULL;
  ast->extra = NULL;
  AstNode nil_node = {0};
  nil_node.kind    = AstNodeKind_Nil;
  dyn_push(ast->arena, ast->nodes, nil_node); // reserve index 0
}

NodeIndex
ast_push_node(Ast* ast, AstNode node) {
  dyn_push(ast->arena, ast->nodes, node);
  return (NodeIndex)(dyn_count(ast->nodes) - 1);
}

NodeIndex
ast_push_atom(Ast* ast, Token tok) {
  AstNode node = {0};
  node.kind    = AstNodeKind_Atom;
  node.token   = tok;
  return ast_push_node(ast, node);
}

NodeIndex
ast_push_string(Ast* ast, Token tok) {
  AstNode node = {0};
  node.kind    = AstNodeKind_String;
  node.token   = tok;
  return ast_push_node(ast, node);
}

// Shared by List, Vector and Map: the storage -- a contiguous run of child
// indices in Ast.extra -- is identical for all three.
NodeIndex
ast_push_seq(Ast* ast, AstNodeKind kind, Token open_tok, NodeIndex* children, u16 count) {
  u32 first_child = (u32)dyn_count(ast->extra);
  foreach_index(i, count) {
    dyn_push(ast->arena, ast->extra, children[i]);
  }
  AstNode node      = {0};
  node.kind         = kind;
  node.token        = open_tok;
  node.first_child  = first_child;
  node.child_count  = count;
  return ast_push_node(ast, node);
}

////////////////////////////////
//~ Debug printer

const char*
ast_node_kind_name(AstNodeKind kind) {
  switch (kind) {
    case AstNodeKind_Nil:    return "Nil";
    case AstNodeKind_Atom:   return "Atom";
    case AstNodeKind_String: return "String";
    case AstNodeKind_List:   return "List";
    case AstNodeKind_Vector: return "Vector";
    case AstNodeKind_Map:    return "Map";
  }
  return "?";
}

void
ast_print(Ast* ast, NodeIndex idx, u32 depth) {
  AstNode* node = &ast->nodes[idx];
  foreach_index(i, depth) { printf("  "); }
  switch (node->kind) {
    case AstNodeKind_Atom: {
      printf("Atom `%.*s`  (%u:%u)\n", str8_varg(node->token.text), node->token.line, node->token.col);
    } break;
    case AstNodeKind_String: {
      printf("String \"%.*s\"  (%u:%u)\n", str8_varg(node->token.text), node->token.line, node->token.col);
    } break;
    case AstNodeKind_List:
    case AstNodeKind_Vector:
    case AstNodeKind_Map: {
      printf("%s [%u children]  (%u:%u)\n", ast_node_kind_name(node->kind),
             (u32)node->child_count, node->token.line, node->token.col);
      foreach_index(i, node->child_count) {
        NodeIndex child = ast->extra[node->first_child + i];
        ast_print(ast, child, depth + 1);
      }
    } break;
    default: {
      printf("%s\n", ast_node_kind_name(node->kind));
    } break;
  }
}


AstNode*
ast_get(Ast* ast, NodeIndex idx) {
  return &ast->nodes[idx];
}

// Works uniformly across List, Vector and Map, which share the same storage.
// Callers check node->kind themselves when the distinction matters.
NodeIndex*
ast_seq_children(Ast* ast, NodeIndex idx, u16* out_count) {
  AstNode* node = ast_get(ast, idx);
  xassert(node->kind == AstNodeKind_List || node->kind == AstNodeKind_Vector || node->kind == AstNodeKind_Map);
  if (out_count) *out_count = node->child_count;
  return ast->extra + node->first_child;
}
