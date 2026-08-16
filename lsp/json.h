#ifndef LSP_JSON_H
#define LSP_JSON_H
// A minimal JSON value tree, parser and serializer, scoped to what
// JSON-RPC/LSP messages need: numbers are plain doubles and object members
// keep insertion order in a linked list.
//
// Standalone by design: plain malloc/free throughout, with no dependency on
// base.h's arena system. See lsp_main.c for why lsp/ stays off ctx_init.
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef enum JsonKind {
  Json_Null,
  Json_Bool,
  Json_Number,
  Json_String,
  Json_Array,
  Json_Object,
} JsonKind;

typedef struct JsonValue JsonValue;

typedef struct JsonArrayItem {
  JsonValue*             value;
  struct JsonArrayItem* next;
} JsonArrayItem;

typedef struct JsonMember {
  char*               key;   // malloc'd, NUL-terminated
  JsonValue*          value;
  struct JsonMember* next;
} JsonMember;

struct JsonValue {
  JsonKind kind;
  union {
    bool            boolean;
    double          number;
    char*           string;      // malloc'd, NUL-terminated, already unescaped
    JsonArrayItem* array_first;  // Json_Array
    JsonMember*     object_first; // Json_Object
  };
};

// Parses `len` bytes at `text` into a value tree. Returns NULL on malformed
// input: unterminated string, bad escape, trailing garbage, or nesting past
// the parser's depth limit.
JsonValue* json_parse(const char* text, size_t len);

// Recursively frees a tree from json_parse or from the constructors below,
// including every string, array item and object member.
void json_free(JsonValue* v);

// Constructors, for building a tree to hand to json_write. json_new_string
// copies `s`; the copy is owned by the returned value and freed by json_free.
JsonValue* json_new_null(void);
JsonValue* json_new_bool(bool b);
JsonValue* json_new_number(double n);
JsonValue* json_new_string(const char* s);
JsonValue* json_new_array(void);
JsonValue* json_new_object(void);

// Append to a container. The kind is not checked: passing anything but the
// matching Json_Array/Json_Object is undefined.
void json_array_push(JsonValue* array, JsonValue* item);
void json_obj_set(JsonValue* object, const char* key, JsonValue* value);

// Looks up an object member by key, by linear scan. Returns NULL if `object`
// is not an object or has no such member.
JsonValue* json_obj_get(JsonValue* object, const char* key);

// Growable output buffer for json_write: doubles on overflow, never shrinks.
// `data` is not NUL-terminated, so consumers must respect `len` -- lsp_main.c
// writes exactly that many bytes to the wire.
typedef struct JsonBuf {
  char*  data;
  size_t len;
  size_t cap;
} JsonBuf;

void json_buf_free(JsonBuf* buf);
// Serializes `v` onto the end of `buf`, growing it as needed. A NULL `v`
// writes `null`.
void json_write(JsonBuf* buf, JsonValue* v);

#endif
