// Recursive-descent JSON parser and tree serializer. See json.h.
#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

////////////////////////////////
//~ Constructors

JsonValue*
json_new_null(void) {
  JsonValue* v = calloc(1, sizeof(JsonValue));
  v->kind = Json_Null;
  return v;
}

JsonValue*
json_new_bool(bool b) {
  JsonValue* v = calloc(1, sizeof(JsonValue));
  v->kind    = Json_Bool;
  v->boolean = b;
  return v;
}

JsonValue*
json_new_number(double n) {
  JsonValue* v = calloc(1, sizeof(JsonValue));
  v->kind   = Json_Number;
  v->number = n;
  return v;
}

JsonValue*
json_new_string(const char* s) {
  JsonValue* v = calloc(1, sizeof(JsonValue));
  v->kind   = Json_String;
  v->string = strdup(s ? s : "");
  return v;
}

JsonValue*
json_new_array(void) {
  JsonValue* v = calloc(1, sizeof(JsonValue));
  v->kind = Json_Array;
  return v;
}

JsonValue*
json_new_object(void) {
  JsonValue* v = calloc(1, sizeof(JsonValue));
  v->kind = Json_Object;
  return v;
}

void
json_array_push(JsonValue* array, JsonValue* item) {
  JsonArrayItem* node = calloc(1, sizeof(JsonArrayItem));
  node->value = item;
  if (!array->array_first) {
    array->array_first = node;
  } else {
    JsonArrayItem* last = array->array_first;
    while (last->next) last = last->next;
    last->next = node;
  }
}

void
json_obj_set(JsonValue* object, const char* key, JsonValue* value) {
  JsonMember* node = calloc(1, sizeof(JsonMember));
  node->key   = strdup(key);
  node->value = value;
  if (!object->object_first) {
    object->object_first = node;
  } else {
    JsonMember* last = object->object_first;
    while (last->next) last = last->next;
    last->next = node;
  }
}

JsonValue*
json_obj_get(JsonValue* object, const char* key) {
  if (!object || object->kind != Json_Object) return NULL;
  for (JsonMember* m = object->object_first; m; m = m->next) {
    if (strcmp(m->key, key) == 0) return m->value;
  }
  return NULL;
}

void
json_free(JsonValue* v) {
  if (!v) return;
  switch (v->kind) {
    case Json_String:
      free(v->string);
      break;
    case Json_Array:
      for (JsonArrayItem* it = v->array_first; it;) {
        JsonArrayItem* next = it->next;
        json_free(it->value);
        free(it);
        it = next;
      }
      break;
    case Json_Object:
      for (JsonMember* m = v->object_first; m;) {
        JsonMember* next = m->next;
        free(m->key);
        json_free(m->value);
        free(m);
        m = next;
      }
      break;
    default:
      break;
  }
  free(v);
}

////////////////////////////////
//~ Parser

// Nesting limit. parse_value recurses once per enclosing array or object, so
// without a cap a long run of `[` overflows the stack instead of failing the
// parse. LSP messages nest around a dozen deep at most.
#define JSON_MAX_DEPTH 128

typedef struct Cursor {
  const char* p;
  const char* end;
  int         depth; // enclosing arrays and objects, capped at JSON_MAX_DEPTH
  bool        ok;
} Cursor;

static void
skip_ws(Cursor* c) {
  while (c->p < c->end && (*c->p == ' ' || *c->p == '\t' || *c->p == '\n' || *c->p == '\r')) c->p += 1;
}

static char
peek(Cursor* c) {
  return c->p < c->end ? *c->p : '\0';
}

static bool
eat(Cursor* c, char ch) {
  if (peek(c) != ch) return false;
  c->p += 1;
  return true;
}

static JsonValue* parse_value(Cursor* c);

// Appends `cp`'s UTF-8 encoding to a growable buffer, doubling capacity as
// needed. Unescaping runs before any JsonValue exists, so it cannot reuse
// json_buf_append below.
static void
append_utf8(char** buf, size_t* len, size_t* cap, uint32_t cp) {
  char enc[4];
  int  n;
  if (cp <= 0x7F) {
    enc[0] = (char)cp;
    n = 1;
  } else if (cp <= 0x7FF) {
    enc[0] = (char)(0xC0 | (cp >> 6));
    enc[1] = (char)(0x80 | (cp & 0x3F));
    n = 2;
  } else if (cp <= 0xFFFF) {
    enc[0] = (char)(0xE0 | (cp >> 12));
    enc[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    enc[2] = (char)(0x80 | (cp & 0x3F));
    n = 3;
  } else {
    enc[0] = (char)(0xF0 | (cp >> 18));
    enc[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    enc[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    enc[3] = (char)(0x80 | (cp & 0x3F));
    n = 4;
  }
  if (*len + (size_t)n > *cap) {
    *cap = (*cap == 0) ? 64 : (*cap * 2);
    while (*len + (size_t)n > *cap) *cap *= 2;
    *buf = realloc(*buf, *cap);
  }
  memcpy(*buf + *len, enc, (size_t)n);
  *len += (size_t)n;
}

static bool
parse_hex4(Cursor* c, uint32_t* out) {
  if (c->end - c->p < 4) return false;
  uint32_t v = 0;
  for (int i = 0; i < 4; i += 1) {
    char ch = c->p[i];
    v <<= 4;
         if (ch >= '0' && ch <= '9') v |= (uint32_t)(ch - '0');
    else if (ch >= 'a' && ch <= 'f') v |= (uint32_t)(ch - 'a' + 10);
    else if (ch >= 'A' && ch <= 'F') v |= (uint32_t)(ch - 'A' + 10);
    else return false;
  }
  c->p += 4;
  *out = v;
  return true;
}

// Assumes the opening `"` is already consumed. Returns a malloc'd,
// NUL-terminated, unescaped C string, or NULL with c->ok cleared on a
// malformed escape or unterminated string.
static char*
parse_string_body(Cursor* c) {
  char*  buf = NULL;
  size_t len = 0, cap = 0;
  for (;;) {
    if (c->p >= c->end) { c->ok = false; free(buf); return NULL; }
    char ch = *c->p;
    if (ch == '"') { c->p += 1; break; }
    if ((unsigned char)ch < 0x20) { c->ok = false; free(buf); return NULL; } // raw control char
    if (ch != '\\') {
      append_utf8(&buf, &len, &cap, (uint32_t)(unsigned char)ch);
      c->p += 1;
      continue;
    }
    c->p += 1; // consume backslash
    if (c->p >= c->end) { c->ok = false; free(buf); return NULL; }
    char esc = *c->p;
    c->p += 1;
    switch (esc) {
      case '"':  append_utf8(&buf, &len, &cap, '"');  break;
      case '\\': append_utf8(&buf, &len, &cap, '\\'); break;
      case '/':  append_utf8(&buf, &len, &cap, '/');  break;
      case 'b':  append_utf8(&buf, &len, &cap, '\b'); break;
      case 'f':  append_utf8(&buf, &len, &cap, '\f'); break;
      case 'n':  append_utf8(&buf, &len, &cap, '\n'); break;
      case 'r':  append_utf8(&buf, &len, &cap, '\r'); break;
      case 't':  append_utf8(&buf, &len, &cap, '\t'); break;
      case 'u': {
        uint32_t cp;
        if (!parse_hex4(c, &cp)) { c->ok = false; free(buf); return NULL; }
        if (cp >= 0xD800 && cp <= 0xDBFF) { // high surrogate -- expect a low surrogate next
          if (c->end - c->p < 6 || c->p[0] != '\\' || c->p[1] != 'u') { c->ok = false; free(buf); return NULL; }
          c->p += 2;
          uint32_t low;
          if (!parse_hex4(c, &low) || low < 0xDC00 || low > 0xDFFF) { c->ok = false; free(buf); return NULL; }
          cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
        }
        append_utf8(&buf, &len, &cap, cp);
      } break;
      default: c->ok = false; free(buf); return NULL;
    }
  }
  if (len + 1 > cap) { cap = len + 1; buf = realloc(buf, cap); }
  if (!buf) buf = malloc(1); // empty string still needs a NUL to terminate
  buf[len] = 0;
  return buf;
}

static JsonValue*
parse_string(Cursor* c) {
  char* s = parse_string_body(c);
  if (!c->ok) return NULL;
  JsonValue* v = calloc(1, sizeof(JsonValue));
  v->kind   = Json_String;
  v->string = s;
  return v;
}

static JsonValue*
parse_number(Cursor* c) {
  char tmp[64];
  size_t n = 0;
  while (c->p < c->end && n + 1 < sizeof(tmp)
         && (*c->p == '-' || *c->p == '+' || *c->p == '.' || *c->p == 'e' || *c->p == 'E'
             || (*c->p >= '0' && *c->p <= '9'))) {
    tmp[n] = *c->p;
    n += 1;
    c->p += 1;
  }
  if (n == 0) { c->ok = false; return NULL; }
  tmp[n] = 0;
  char* endp;
  double val = strtod(tmp, &endp);
  if (endp == tmp) { c->ok = false; return NULL; }
  return json_new_number(val);
}

static JsonValue*
parse_array(Cursor* c) {
  JsonValue* arr = json_new_array();
  skip_ws(c);
  if (eat(c, ']')) return arr; // empty array
  for (;;) {
    JsonValue* item = parse_value(c);
    if (!c->ok) { json_free(arr); return NULL; }
    json_array_push(arr, item);
    skip_ws(c);
    if (eat(c, ',')) { skip_ws(c); continue; }
    if (eat(c, ']')) break;
    c->ok = false;
    json_free(arr);
    return NULL;
  }
  return arr;
}

static JsonValue*
parse_object(Cursor* c) {
  JsonValue* obj = json_new_object();
  skip_ws(c);
  if (eat(c, '}')) return obj; // empty object
  for (;;) {
    skip_ws(c);
    if (peek(c) != '"') { c->ok = false; json_free(obj); return NULL; }
    c->p += 1;
    char* key = parse_string_body(c);
    if (!c->ok) { json_free(obj); return NULL; }
    skip_ws(c);
    if (!eat(c, ':')) { free(key); c->ok = false; json_free(obj); return NULL; }
    skip_ws(c);
    JsonValue* val = parse_value(c);
    if (!c->ok) { free(key); json_free(obj); return NULL; }
    JsonMember* node = calloc(1, sizeof(JsonMember));
    node->key   = key;
    node->value = val;
    if (!obj->object_first) {
      obj->object_first = node;
    } else {
      JsonMember* last = obj->object_first;
      while (last->next) last = last->next;
      last->next = node;
    }
    skip_ws(c);
    if (eat(c, ',')) continue;
    if (eat(c, '}')) break;
    c->ok = false;
    json_free(obj);
    return NULL;
  }
  return obj;
}

static bool
eat_literal(Cursor* c, const char* lit) {
  size_t n = strlen(lit);
  if ((size_t)(c->end - c->p) < n || memcmp(c->p, lit, n) != 0) return false;
  c->p += n;
  return true;
}

static JsonValue*
parse_value(Cursor* c) {
  skip_ws(c);
  char ch = peek(c);
  if (ch == '"') { c->p += 1; return parse_string(c); }
  // Containers are only ever entered here, so this is the one place the depth
  // has to be tracked.
  if (ch == '{' || ch == '[') {
    if (c->depth >= JSON_MAX_DEPTH) { c->ok = false; return NULL; }
    c->depth += 1;
    c->p += 1;
    JsonValue* v = (ch == '{') ? parse_object(c) : parse_array(c);
    c->depth -= 1;
    return v;
  }
  if (ch == '-' || (ch >= '0' && ch <= '9')) return parse_number(c);
  if (eat_literal(c, "true"))  return json_new_bool(true);
  if (eat_literal(c, "false")) return json_new_bool(false);
  if (eat_literal(c, "null"))  return json_new_null();
  c->ok = false;
  return NULL;
}

JsonValue*
json_parse(const char* text, size_t len) {
  Cursor c = {0};
  c.p   = text;
  c.end = text + len;
  c.ok  = true;
  JsonValue* v = parse_value(&c);
  if (!c.ok) { json_free(v); return NULL; }
  skip_ws(&c);
  if (c.p != c.end) { json_free(v); return NULL; } // trailing garbage
  return v;
}

////////////////////////////////
//~ Serializer

static void
json_buf_append(JsonBuf* buf, const char* data, size_t n) {
  if (buf->len + n > buf->cap) {
    size_t new_cap = (buf->cap == 0) ? 256 : (buf->cap * 2);
    while (buf->len + n > new_cap) new_cap *= 2;
    buf->data = realloc(buf->data, new_cap);
    buf->cap  = new_cap;
  }
  memcpy(buf->data + buf->len, data, n);
  buf->len += n;
}

static void
json_buf_append_cstr(JsonBuf* buf, const char* s) {
  json_buf_append(buf, s, strlen(s));
}

void
json_buf_free(JsonBuf* buf) {
  free(buf->data);
  buf->data = NULL;
  buf->len  = 0;
  buf->cap  = 0;
}

static void
write_escaped_string(JsonBuf* buf, const char* s) {
  json_buf_append(buf, "\"", 1);
  for (const unsigned char* p = (const unsigned char*)s; *p; p += 1) {
    switch (*p) {
      case '"':  json_buf_append(buf, "\\\"", 2); break;
      case '\\': json_buf_append(buf, "\\\\", 2); break;
      case '\n': json_buf_append(buf, "\\n", 2);  break;
      case '\r': json_buf_append(buf, "\\r", 2);  break;
      case '\t': json_buf_append(buf, "\\t", 2);  break;
      default:
        if (*p < 0x20) {
          char esc[8];
          int n = snprintf(esc, sizeof(esc), "\\u%04x", *p);
          json_buf_append(buf, esc, (size_t)n);
        } else {
          json_buf_append(buf, (const char*)p, 1); // valid UTF-8 passes through as-is
        }
    }
  }
  json_buf_append(buf, "\"", 1);
}

void
json_write(JsonBuf* buf, JsonValue* v) {
  if (!v) { json_buf_append_cstr(buf, "null"); return; }
  switch (v->kind) {
    case Json_Null: json_buf_append_cstr(buf, "null"); break;
    case Json_Bool:  json_buf_append_cstr(buf, v->boolean ? "true" : "false"); break;
    case Json_Number: {
      char tmp[32];
      double n = v->number;
      int len;
      if (n == (double)(long long)n) len = snprintf(tmp, sizeof(tmp), "%lld", (long long)n);
      else                            len = snprintf(tmp, sizeof(tmp), "%g", n);
      json_buf_append(buf, tmp, (size_t)len);
    } break;
    case Json_String: write_escaped_string(buf, v->string); break;
    case Json_Array: {
      json_buf_append(buf, "[", 1);
      bool first = true;
      for (JsonArrayItem* it = v->array_first; it; it = it->next) {
        if (!first) json_buf_append(buf, ",", 1);
        first = false;
        json_write(buf, it->value);
      }
      json_buf_append(buf, "]", 1);
    } break;
    case Json_Object: {
      json_buf_append(buf, "{", 1);
      bool first = true;
      for (JsonMember* m = v->object_first; m; m = m->next) {
        if (!first) json_buf_append(buf, ",", 1);
        first = false;
        write_escaped_string(buf, m->key);
        json_buf_append(buf, ":", 1);
        json_write(buf, m->value);
      }
      json_buf_append(buf, "}", 1);
    } break;
  }
}
