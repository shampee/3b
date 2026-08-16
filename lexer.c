// Turns source text into a flat token stream: parens, brackets, braces,
// string literals, and atoms. Atoms cover identifiers, operators and numbers
// alike; nothing here distinguishes them.
#include "3b.h"

void
lexer_init(Lexer* lex, String8 src, u32 file_id) {
  lex->src     = src;
  lex->pos     = 0;
  lex->line    = 1;
  lex->col     = 1;
  lex->file_id = file_id;
}

u8
lexer_peek(Lexer* lex) {
  return lex->pos < lex->src.size ? lex->src.str[lex->pos] : 0;
}

u8
lexer_advance(Lexer* lex) {
  u8 c = lexer_peek(lex);
  if (c != 0) {
    lex->pos += 1;
    if (c == '\n') {
      lex->line += 1;
      lex->col   = 1;
    } else {
      lex->col += 1;
    }
  }
  return c;
}

static b32
char_is_delim(u8 c) {
  return c == 0 || char_is_space(c) ||
         c == '(' || c == ')' ||
         c == '[' || c == ']' ||
         c == '{' || c == '}' ||
         c == '"' || c == ';';
}

void
lexer_skip_ignorable(Lexer* lex) {
  for (;;) {
    u8 c = lexer_peek(lex);
    if (char_is_space(c)) {
      lexer_advance(lex);
      continue;
    }
    if (c == ';') { // line comment, to end of line
      while (lexer_peek(lex) != 0 && lexer_peek(lex) != '\n') {
        lexer_advance(lex);
      }
      continue;
    }
    break;
  }
}

// Lexes a string literal, consuming both quotes itself. Decodes the
// \n \t \r \\ \" \0 escapes; any other escape passes through literally.
Token
lexer_read_string(Lexer* lex, Arena* dst_arena, u32 line, u32 col) {
  Token tok    = {0};
  tok.line     = line;
  tok.col      = col;
  tok.file_id  = lex->file_id;
  lexer_advance(lex); // consume opening '"'

  ArenaTemp temp = arena_temp_begin(ctx_scratch());
  u8* buf        = NULL;
  for (;;) {
    u8 c = lexer_peek(lex);
    if (c == 0) {
      tok.kind = TokenKind_Error;
      tok.text = str8_lit("unterminated string literal");
      arena_temp_end(&temp);
      return tok;
    }
    if (c == '"') {
      lexer_advance(lex);
      break;
    }
    if (c == '\\') {
      lexer_advance(lex);
      u8 esc = lexer_peek(lex);
      u8 decoded;
      switch (esc) {
        case 'n':  decoded = '\n'; break;
        case 't':  decoded = '\t'; break;
        case 'r':  decoded = '\r'; break;
        case '0':  decoded = 0;    break;
        case '\\': decoded = '\\'; break;
        case '"':  decoded = '"';  break;
        default:   decoded = esc;  break; // unknown escape: pass through literally
      }
      lexer_advance(lex);
      dyn_push(temp.arena, buf, decoded);
    } else {
      lexer_advance(lex);
      dyn_push(temp.arena, buf, c);
    }
  }

  u64 len = dyn_count(buf);
  u8* dst = push_array(dst_arena, u8, len == 0 ? 1 : len);
  if (len != 0) {
    MemoryCopy(dst, buf, len);
  }
  arena_temp_end(&temp);

  tok.kind = TokenKind_String;
  tok.text = str8(dst, len);
  return tok;
}

Token
lexer_next(Lexer* lex, Arena* dst_arena) {
  lexer_skip_ignorable(lex);
  u32 line = lex->line;
  u32 col  = lex->col;
  u8  c    = lexer_peek(lex);

  Token tok   = {0};
  tok.line    = line;
  tok.col     = col;
  tok.file_id = lex->file_id;

  if (c == 0) {
    tok.kind = TokenKind_EOF;
    return tok;
  }
  if (c == '(') {
    lexer_advance(lex);
    tok.kind = TokenKind_LParen;
    tok.text = str8_lit("(");
    return tok;
  }
  if (c == ')') {
    lexer_advance(lex);
    tok.kind = TokenKind_RParen;
    tok.text = str8_lit(")");
    return tok;
  }
  if (c == '[') {
    lexer_advance(lex);
    tok.kind = TokenKind_LBracket;
    tok.text = str8_lit("[");
    return tok;
  }
  if (c == ']') {
    lexer_advance(lex);
    tok.kind = TokenKind_RBracket;
    tok.text = str8_lit("]");
    return tok;
  }
  if (c == '{') {
    lexer_advance(lex);
    tok.kind = TokenKind_LBrace;
    tok.text = str8_lit("{");
    return tok;
  }
  if (c == '}') {
    lexer_advance(lex);
    tok.kind = TokenKind_RBrace;
    tok.text = str8_lit("}");
    return tok;
  }
  if (c == '"') {
    return lexer_read_string(lex, dst_arena, line, col);
  }

  // An atom runs to the next delimiter, covering identifiers, operators and
  // numbers alike.
  u64 start = lex->pos;
  while (!char_is_delim(lexer_peek(lex))) {
    lexer_advance(lex);
  }
  u64 end = lex->pos;
  tok.kind = TokenKind_Atom;
  tok.text = str8_range(lex->src.str + start, lex->src.str + end);
  return tok;
}
