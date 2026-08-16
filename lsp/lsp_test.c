// Regression test for the 3b-lsp wire protocol. Spawns the built binary with
// pipes on its stdin/stdout, drives the JSON-RPC handshake directly (reusing
// json.c), and asserts on what comes back. Run as
// `./3b-lsp-test <path-to-3b-lsp>`; see the Makefile's `lsp-test` target.
#include "json.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int g_failures = 0;

static void
check(bool cond, const char* fmt, ...) {
  if (cond) return;
  g_failures += 1;
  fprintf(stderr, "FAIL: ");
  va_list args;
  va_start(args, fmt);
  vfprintf(stderr, fmt, args);
  va_end(args);
  fprintf(stderr, "\n");
}

////////////////////////////////
//~ Test fixtures, written into a fresh temp directory so this test depends on
// nothing outside itself. Each block below notes the source positions its
// requests target: source lines and columns are 1-indexed, LSP line/character
// are 0-indexed.

#define BROKEN_CONTENT "(package broken)\n\n(fn bad [] i32\n  (+ 1 \"not a number\"))\n"
#define CLEAN_CONTENT  "(package clean)\n\n(fn double [x i32] i32\n  (* x 2))\n"
// Line 5, "    n))": the `n` use site is at column 5 / character 4.
#define HOVER_CONTENT  "(package hover_pkg)\n\n(fn use-let [] i32\n  (let [n i32 5]\n    n))\n"
// Line 7, "  (helper))": "helper" starts at column 4 / character 3. The
// declaration of `fn helper` is at line 3, column 1 -- the opening paren, per
// lower.c's token convention for FunctionDecl.
#define DEF_CONTENT    "(package def_pkg)\n\n(fn helper [] i32\n  1)\n\n(fn caller [] i32\n  (helper))\n"
// `struct Mesh` is declared at line 3, column 1, same token convention as
// `fn helper` above. Line 7 holds three type annotations to jump from, all in
// one param list: a bare named type ("Mesh", character 16), a pointer
// ("Mesh*", character 23) and a handle ("Mesh^", character 31). All three must
// resolve to that same struct declaration.
#define DEF_TYPE_CONTENT \
  "(package def_type_pkg)\n\n(struct Mesh [x i32])\n\n(handle Mesh)\n\n" \
  "(fn use-mesh [b Mesh m Mesh* h Mesh^] i32\n  0)\n"
// Line 2 (0-indexed), "(fn frobnicate [] i32": character 8 is right after
// "frob".
#define COMPLETION_CONTENT "(package completion_pkg)\n\n(fn frobnicate [] i32\n  1)\n\n(fn frobulate [] i32\n  2)\n"
// The same package with an unclosed paren. Not even the tolerant recompile
// recovers this one, so completion here must come from the cache.
#define COMPLETION_BROKEN_CONTENT COMPLETION_CONTENT "\n(fn broken [] i32\n  (+ 1"
// `(import libpkg)` resolves to a subdirectory of the importer, not a sibling
// (compiler.h's package model); see make_fixtures. Line 2 (0-indexed),
// character 11 is right after "lib"; line 5, character 10 is right after
// "libpkg/".
#define IMPORTER_CONTENT "(package importer_pkg)\n\n(import libpkg)\n\n(fn use-lib [] i32\n  (libpkg/pub-fn))\n"
#define LIBPKG_CONTENT   "(package libpkg)\n\n(fn pub-fn [] i32\n  1)\n\n(private\n  (fn priv-fn [] i32\n    2))\n"
// Richer hover: a call, a struct construction and an enum access.
//  7: (fn helper2 [a i32 b i32] i32
// 11:   (helper2 1 2)                              -- "helper2" at char 3
// 12:   (var v Vector2 (Vector2 {:x 1.0 :y 2.0}))  -- "Vector2" at char 18
// 13:   (var c Color Color/Red)                    -- "Color/Red" at char 15
#define HOVER2_CONTENT \
  "(package hover2_pkg)\n\n(struct Vector2 [x f32 y f32])\n\n(enum Color [Red Green Blue])\n\n" \
  "(fn helper2 [a i32 b i32] i32\n  (+ a b))\n\n(fn use-things [] i32\n  (helper2 1 2)\n" \
  "  (var v Vector2 (Vector2 {:x 1.0 :y 2.0}))\n  (var c Color Color/Red)\n  0)\n"
// Hover-eval: a top-level `val` with a pure arithmetic initializer
// (HOUR_SECONDS = 3600), and an expression dividing by a runtime zero, which
// must neither crash the server nor append an evaluated value.
// 3: (val HOUR_SECONDS i32 (* 60 60))
// 6:   HOUR_SECONDS                  -- identifier at char 2
// 7:   (/ 10 (- 5 5))                -- the outer form's "(" is at char 2
#define HOVER_EVAL_CONTENT \
  "(package hover_eval_pkg)\n\n(val HOUR_SECONDS i32 (* 60 60))\n\n" \
  "(fn use-vals [] i32\n  HOUR_SECONDS\n  (/ 10 (- 5 5))\n  0)\n"

// Builtin hover. `print`/`println` have no declaration to point at, so they
// show their accepted call shapes -- including the optional leading `stream`
// added with the `stream` primitive -- from lib3b.c's BUILTIN_SHAPES table.
// Both a plain call and one actually passing a stream must show the same text.
//  6:   (println "hi {}" 1)   -- "println" at column 4 / character 3
//  8:   (print f "n={}" 2)    -- "print" at column 4 / character 3
#define HOVER_BUILTIN_CONTENT \
  "(package hover_builtin_pkg)\n\n(import os)\n\n(fn use-print [] i32\n  (println \"hi {}\" 1)\n" \
  "  (var f stream (os/open \"/dev/null\" os/mode-write))\n  (print f \"n={}\" 2)\n  (os/close f)\n  0)\n"

// Doc comments -- the comment run written directly above a declaration, shown
// as prose in the hover popup. Every case a reader could get wrong is in this
// one fixture (source lines, 1-indexed):
//  3-5:  a three-line run above `fn documented` (line 6), its middle line an
//        empty `;;` that must survive as a paragraph break, not end the run
//  9:    a trailing comment, on the line above `fn undocumented` (line 10).
//        Not a whole-line comment, so not that function's doc
//  13:   a comment separated from `fn spaced` (line 15) by a blank line --
//        also not its doc, since a run has to reach the declaration
//  18-19: `struct Documented`, hovered through a type annotation rather than a
//        call, to show a doc arrives by that route too
// 21-22: `fn use-doc`, holding every position the requests below target: the
//        `Documented` annotation at line 21 character 18, calls to
//        documented/undocumented/spaced at line 22 characters 8/28/43, and
//        the end of "documented" at line 22 character 16, for completion
#define HOVER_DOC_CONTENT \
  "(package hover_doc_pkg)\n\n" \
  ";; Adds two numbers.\n;;\n;; And explains itself at length.\n" \
  "(fn documented [a i32 b i32] i32\n  (+ a b))\n\n" \
  "(val ignored i32 0) ; trailing, not a doc\n(fn undocumented [] i32\n  1)\n\n" \
  ";; Detached by a blank line.\n\n(fn spaced [] i32\n  2)\n\n" \
  ";; A documented struct.\n(struct Documented [x i32])\n\n" \
  "(fn use-doc [d Documented] i32\n  (+ (documented 1 2) (+ (undocumented) (spaced))))\n"

// Field references -- the field name in a `.` chain, a `get`, or a `{}`
// destructuring pattern, each of which resolves to a field of whatever struct
// the base turned out to be (source lines, 1-indexed):
//   3:    `struct Inner`, both fields on the declaration's own line
//   5:    a comment above `struct Outer` -- documents the struct, and must not
//         be credited to `inner`, the first field inside it
//   7-8:  `inner`, on its own line with a comment run of its own that IS its doc
//   13:   `(. o inner w)` -- two hops sharing one form, resolving against
//         different structs: `inner` on Outer, then `w` on Inner
//   16:   the same field lookup through `get` rather than `.`
//   19:   a destructuring pattern naming `w`
//   23:   a `.` hop on a Map, which is a key expression and not a field at all
//   25-33: anonymous (`_`) members, reachable with no path segment of their
//         own, so the struct an access is written against is not the one that
//         declares the field: `w` comes from Inner and `tag` from the inline
//         struct inside Payload, both reached through a Wrapper
//   36:   `(+ (. wr w) (. wr p tag))`, the two accesses that prove it
#define HOVER_FIELD_CONTENT \
  "(package hover_field_pkg)\n\n" \
  "(struct Inner [w i32 h i32])\n\n" \
  ";; Wraps an inner box.\n(struct Outer [\n" \
  "  ;; The inner box this one wraps.\n  inner Inner*\n  label string\n])\n\n" \
  "(fn use-field [o Outer*] i32\n  (. o inner w))\n\n" \
  "(fn use-get [o Outer] string\n  (get o label))\n\n" \
  "(fn use-destructure [i Inner] i32\n  (let [{w} i]\n    w))\n\n" \
  "(fn use-map [m {string i32}] i32*\n  (. m \"two\"))\n\n" \
  "(union Payload [\n  _ (struct [tag i32])\n  raw [u8 4]\n])\n\n" \
  "(struct Wrapper [\n  _ Inner\n  p Payload\n])\n\n" \
  "(fn use-anon [wr Wrapper*] i32\n  (+ (. wr w) (. wr p tag)))\n"

// Partial-parse completion. PARTIAL_CONTENT checks cleanly, seeding the cache
// with only "existing-fn". PARTIAL_BROKEN_CONTENT adds "brand-new-fn" and
// leaves the last form unclosed, so it can never pass a normal check:
// completing "brand-new-fn" can only mean the tolerant recompile ran.
// Line 9 (0-indexed), "  (brand": character 8 is right after "brand".
#define PARTIAL_CONTENT \
  "(package partial_pkg)\n\n(fn existing-fn [] i32\n  1)\n"
#define PARTIAL_BROKEN_CONTENT \
  "(package partial_pkg)\n\n(fn existing-fn [] i32\n  1)\n\n(fn brand-new-fn [] i32\n  2)\n\n" \
  "(fn use-it [] i32\n  (brand"

// Local and parameter completion. LOCALS_CONTENT is clean: a baseline fn, one
// with a param, one with both a param and a `let`-local.
//
// LOCALS_BROKEN_A mid-types the `let`-local's name ("tot" toward "total"),
// showing a local is offered at all. LOCALS_BROKEN_B mid-types the current
// function's param ("co" toward "count") while an earlier, already-popped
// sibling function has a param sharing that prefix ("co-unrelated"): the
// snapshot must be the live scope stack at the query position, not a union of
// everything bound while checking the file. Line 10 (0-indexed) is "    (+ tot"
// and "    (+ total co"; the cursor sits at character 10 and 15.
#define LOCALS_CONTENT \
  "(package locals_pkg)\n\n(fn helper [] i32\n  1)\n\n" \
  "(fn first-fn [co-unrelated i32] i32\n  (+ co-unrelated 1))\n\n" \
  "(fn use-it [count i32] i32\n  (let [total i32 0]\n    (+ total count)))\n"
#define LOCALS_BROKEN_A \
  "(package locals_pkg)\n\n(fn helper [] i32\n  1)\n\n" \
  "(fn first-fn [co-unrelated i32] i32\n  (+ co-unrelated 1))\n\n" \
  "(fn use-it [count i32] i32\n  (let [total i32 0]\n    (+ tot"
#define LOCALS_BROKEN_B \
  "(package locals_pkg)\n\n(fn helper [] i32\n  1)\n\n" \
  "(fn first-fn [co-unrelated i32] i32\n  (+ co-unrelated 1))\n\n" \
  "(fn use-it [count i32] i32\n  (let [total i32 0]\n    (+ total co"

// documentSymbol and formatting share one fixture, since both want a file
// holding several differently-kinded top-level declarations. Written badly
// laid out -- run-together forms, a stretched `fn` header, doubled inner
// spaces -- so the formatted rendering below is unmistakably a rewrite and not
// an accident of already being tidy. Declaration positions, 1-indexed:
//   2: (struct Point ...)   -- LSP line 1
//   3: (val LIMIT ...)      -- LSP line 2
//   4: (fn area ...)        -- LSP line 3
#define FORMAT_CONTENT \
  "(package format_pkg)\n(struct Point [x i32 y i32])\n(val LIMIT i32 10)\n" \
  "(fn area    [p Point] i32\n(* (. p x)   (. p y)))\n"
// What `3b format` renders FORMAT_CONTENT as, byte for byte. Kept as a literal
// rather than computed so this test pins the wire payload the client actually
// applies, not merely "whatever format.c does today". test/format_test.c owns
// the properties of the rendering itself (idempotence, semantic preservation);
// this only checks it arrives intact as a whole-document TextEdit.
#define FORMAT_EXPECTED \
  "(package format_pkg)\n\n(struct Point\n  [x i32\n   y i32])\n\n" \
  "(val LIMIT i32 10)\n\n(fn area [p Point] i32\n  (* (. p x) (. p y)))\n"
// Unbalanced: the closing paren of `area` is missing, so the buffer does not
// parse and formatting must decline rather than emit a truncated rewrite.
#define FORMAT_BROKEN_CONTENT \
  "(package format_pkg)\n(fn area [p i32] i32\n  (* p p\n"
// A SECOND file of format_pkg. A package's symbols are cached per DIRECTORY,
// so without this the per-file filter in handle_document_symbol would be
// untestable: every symbol of a one-file package trivially belongs to that
// file. `sibling-fn` must never show up in format_pkg.3b's outline.
#define FORMAT_SIBLING_CONTENT "(package format_pkg)\n\n(fn sibling-fn [] i32\n  7)\n"

static void
write_file(const char* path, const char* contents) {
  FILE* f = fopen(path, "w");
  if (!f) { perror(path); exit(1); }
  fputs(contents, f);
  fclose(f);
}

static void
make_pkg_dir(const char* tmp, const char* name, const char* content) {
  char path[4096];
  snprintf(path, sizeof(path), "%s/%s", tmp, name);
  mkdir(path, 0755);
  snprintf(path, sizeof(path), "%s/%s/%s.3b", tmp, name, name);
  write_file(path, content);
}

static char*
make_fixtures(void) {
  char* tmp = strdup("/tmp/3b-lsp-test-XXXXXX");
  if (!mkdtemp(tmp)) { perror("mkdtemp"); exit(1); }

  make_pkg_dir(tmp, "broken", BROKEN_CONTENT);
  make_pkg_dir(tmp, "clean", CLEAN_CONTENT);
  make_pkg_dir(tmp, "hover_pkg", HOVER_CONTENT);
  make_pkg_dir(tmp, "def_pkg", DEF_CONTENT);
  make_pkg_dir(tmp, "def_type_pkg", DEF_TYPE_CONTENT);
  make_pkg_dir(tmp, "completion_pkg", COMPLETION_CONTENT);

  char path[4096];
  snprintf(path, sizeof(path), "%s/importer_pkg", tmp);
  mkdir(path, 0755);
  snprintf(path, sizeof(path), "%s/importer_pkg/importer_pkg.3b", tmp);
  write_file(path, IMPORTER_CONTENT);
  snprintf(path, sizeof(path), "%s/importer_pkg/libpkg", tmp);
  mkdir(path, 0755);
  snprintf(path, sizeof(path), "%s/importer_pkg/libpkg/libpkg.3b", tmp);
  write_file(path, LIBPKG_CONTENT);

  make_pkg_dir(tmp, "hover2_pkg", HOVER2_CONTENT);
  make_pkg_dir(tmp, "hover_eval_pkg", HOVER_EVAL_CONTENT);
  make_pkg_dir(tmp, "hover_builtin_pkg", HOVER_BUILTIN_CONTENT);
  make_pkg_dir(tmp, "hover_doc_pkg", HOVER_DOC_CONTENT);
  make_pkg_dir(tmp, "hover_field_pkg", HOVER_FIELD_CONTENT);
  make_pkg_dir(tmp, "partial_pkg", PARTIAL_CONTENT);
  make_pkg_dir(tmp, "locals_pkg", LOCALS_CONTENT);
  make_pkg_dir(tmp, "format_pkg", FORMAT_CONTENT);
  snprintf(path, sizeof(path), "%s/format_pkg/sibling.3b", tmp);
  write_file(path, FORMAT_SIBLING_CONTENT);

  return tmp;
}

////////////////////////////////
//~ Wire protocol, duplicated from lsp_main.c's read_message/send_message. This
// test links only json.o, never lsp_main.o, so it stays a black-box test of the
// spawned binary rather than sharing its internals.

typedef struct Server {
  pid_t pid;
  FILE* to_server;
  FILE* from_server;
} Server;

static Server
spawn_server(const char* path) {
  int in_pipe[2], out_pipe[2];
  if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0) { perror("pipe"); exit(1); }
  pid_t pid = fork();
  if (pid < 0) { perror("fork"); exit(1); }
  if (pid == 0) {
    dup2(in_pipe[0], STDIN_FILENO);
    dup2(out_pipe[1], STDOUT_FILENO);
    close(in_pipe[0]); close(in_pipe[1]);
    close(out_pipe[0]); close(out_pipe[1]);
    execl(path, path, (char*)NULL);
    perror("execl");
    _exit(127);
  }
  close(in_pipe[0]);
  close(out_pipe[1]);
  Server s;
  s.pid         = pid;
  s.to_server   = fdopen(in_pipe[1], "w");
  s.from_server = fdopen(out_pipe[0], "r");
  return s;
}

static void
send_msg(FILE* out, JsonValue* v) {
  JsonBuf buf = {0};
  json_write(&buf, v);
  fprintf(out, "Content-Length: %zu\r\n\r\n", buf.len);
  fwrite(buf.data, 1, buf.len, out);
  fflush(out);
  json_buf_free(&buf);
}

// Returns NULL on EOF or a malformed frame, as lsp_main.c's does.
static JsonValue*
read_msg(FILE* in) {
  long   content_length = -1;
  char*  line     = NULL;
  size_t line_cap = 0;
  for (;;) {
    ssize_t n = getline(&line, &line_cap, in);
    if (n < 0) { free(line); return NULL; }
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) { line[n - 1] = 0; n -= 1; }
    if (n == 0) break;
    if (strncmp(line, "Content-Length:", 15) == 0) content_length = strtol(line + 15, NULL, 10);
  }
  free(line);
  if (content_length < 0) return NULL;
  char*  body = malloc((size_t)content_length + 1);
  size_t got  = fread(body, 1, (size_t)content_length, in);
  if (got != (size_t)content_length) { free(body); return NULL; }
  body[content_length] = 0;
  JsonValue* v = json_parse(body, (size_t)content_length);
  free(body);
  return v;
}

////////////////////////////////
//~ Small JSON-RPC message builders

static JsonValue*
request(int id, const char* method, JsonValue* params) {
  JsonValue* v = json_new_object();
  json_obj_set(v, "jsonrpc", json_new_string("2.0"));
  json_obj_set(v, "id", json_new_number(id));
  json_obj_set(v, "method", json_new_string(method));
  json_obj_set(v, "params", params ? params : json_new_object());
  return v;
}

static JsonValue*
notification(const char* method, JsonValue* params) {
  JsonValue* v = json_new_object();
  json_obj_set(v, "jsonrpc", json_new_string("2.0"));
  json_obj_set(v, "method", json_new_string(method));
  json_obj_set(v, "params", params ? params : json_new_object());
  return v;
}

static JsonValue*
did_open_params(const char* uri, const char* text) {
  JsonValue* td = json_new_object();
  json_obj_set(td, "uri", json_new_string(uri));
  json_obj_set(td, "languageId", json_new_string("3b"));
  json_obj_set(td, "version", json_new_number(1));
  json_obj_set(td, "text", json_new_string(text));
  JsonValue* params = json_new_object();
  json_obj_set(params, "textDocument", td);
  return params;
}

// Full-document sync: one contentChanges entry whose `text` replaces the whole
// document, matching the TextDocumentSyncKind.Full capability 3b-lsp declares.
static JsonValue*
did_change_params(const char* uri, const char* text) {
  JsonValue* td = json_new_object();
  json_obj_set(td, "uri", json_new_string(uri));
  JsonValue* change = json_new_object();
  json_obj_set(change, "text", json_new_string(text));
  JsonValue* changes = json_new_array();
  json_array_push(changes, change);
  JsonValue* params = json_new_object();
  json_obj_set(params, "textDocument", td);
  json_obj_set(params, "contentChanges", changes);
  return params;
}

// documentSymbol and formatting both take a bare textDocument and nothing
// else. (Formatting's spec also carries FormattingOptions; 3b-lsp reads none
// of it -- see handle_formatting -- so it is left off here.)
static JsonValue*
text_document_params(const char* uri) {
  JsonValue* td = json_new_object();
  json_obj_set(td, "uri", json_new_string(uri));
  JsonValue* params = json_new_object();
  json_obj_set(params, "textDocument", td);
  return params;
}

// `line`/`character` are 0-indexed, as LSP Position is.
static JsonValue*
position_params(const char* uri, int line, int character) {
  JsonValue* td = json_new_object();
  json_obj_set(td, "uri", json_new_string(uri));
  JsonValue* pos = json_new_object();
  json_obj_set(pos, "line", json_new_number(line));
  json_obj_set(pos, "character", json_new_number(character));
  JsonValue* params = json_new_object();
  json_obj_set(params, "textDocument", td);
  json_obj_set(params, "position", pos);
  return params;
}

static bool
has_completion_label(JsonValue* items, const char* label) {
  if (!items || items->kind != Json_Array) return false;
  for (JsonArrayItem* it = items->array_first; it; it = it->next) {
    JsonValue* lbl = json_obj_get(it->value, "label");
    if (lbl && lbl->kind == Json_String && strcmp(lbl->string, label) == 0) return true;
  }
  return false;
}

// The SymbolInformation entry named `name` in a documentSymbol/workspaceSymbol
// result array, or NULL if there is none.
static JsonValue*
find_symbol(JsonValue* arr, const char* name) {
  if (!arr || arr->kind != Json_Array) return NULL;
  for (JsonArrayItem* it = arr->array_first; it; it = it->next) {
    JsonValue* n = json_obj_get(it->value, "name");
    if (n && n->kind == Json_String && strcmp(n->string, name) == 0) return it->value;
  }
  return NULL;
}

// A named item's `field` ("detail"/"documentation"), or NULL when the item is
// absent or carries no such field -- most completion items are label-only.
static const char*
completion_item_field(JsonValue* items, const char* label, const char* field) {
  if (!items || items->kind != Json_Array) return NULL;
  for (JsonArrayItem* it = items->array_first; it; it = it->next) {
    JsonValue* lbl = json_obj_get(it->value, "label");
    if (!lbl || lbl->kind != Json_String || strcmp(lbl->string, label) != 0) continue;
    JsonValue* v = json_obj_get(it->value, field);
    return (v && v->kind == Json_String) ? v->string : NULL;
  }
  return NULL;
}

////////////////////////////////
//~ Test

static char*
uri_for(const char* dir, const char* base) {
  char buf[4096];
  snprintf(buf, sizeof(buf), "file://%s/%s", dir, base);
  return strdup(buf);
}

// `depth` nested empty arrays: well-formed JSON whose only unusual property is
// how deep it goes. Returns a malloc'd, NUL-terminated string.
static char*
nested_json(size_t depth) {
  char* s = malloc(depth * 2 + 1);
  for (size_t i = 0; i < depth; i += 1) s[i] = '[';
  for (size_t i = 0; i < depth; i += 1) s[depth + i] = ']';
  s[depth * 2] = 0;
  return s;
}

// Writes a frame from a literal header and body, bypassing send_msg so a test
// can hand the server a header it would never generate itself.
static void
send_raw(FILE* out, const char* header, const char* body, size_t body_len) {
  fputs(header, out);
  if (body_len) fwrite(body, 1, body_len, out);
  fflush(out);
}

int
main(int argc, char** argv) {
  if (argc < 2) { fprintf(stderr, "usage: %s <path-to-3b-lsp>\n", argv[0]); return 1; }
  const char* server_path = argv[1];

  // json.c's depth cap, checked against the linked parser directly. Nesting
  // well inside the cap parses; nesting far past it is a parse error rather
  // than a recursion deep enough to run out of stack.
  {
    char* shallow = nested_json(64);
    JsonValue* v = json_parse(shallow, strlen(shallow));
    check(v != NULL && v->kind == Json_Array, "64-deep nesting should parse");
    json_free(v);
    free(shallow);

    char* deep = nested_json(500000);
    check(json_parse(deep, strlen(deep)) == NULL, "500000-deep nesting should be a parse error");
    free(deep);
  }

  char* fixtures  = make_fixtures();
  char  broken_dir[4096]; snprintf(broken_dir, sizeof(broken_dir), "%s/broken", fixtures);
  char  clean_dir[4096];  snprintf(clean_dir, sizeof(clean_dir), "%s/clean", fixtures);
  char* broken_uri = uri_for(broken_dir, "broken.3b");
  char* clean_uri  = uri_for(clean_dir, "clean.3b");

  Server s = spawn_server(server_path);

  // initialize -> expect capabilities.textDocumentSync back. rootUri points at
  // the fixtures dir so workspace/symbol has a root to walk later.
  char root_uri[4200]; snprintf(root_uri, sizeof(root_uri), "file://%s", fixtures);
  JsonValue* init_params = json_new_object();
  json_obj_set(init_params, "rootUri", json_new_string(root_uri));
  JsonValue* init = request(1, "initialize", init_params);
  send_msg(s.to_server, init);
  json_free(init);
  JsonValue* init_resp = read_msg(s.from_server);
  check(init_resp != NULL, "no response to initialize");
  if (init_resp) {
    JsonValue* result = json_obj_get(init_resp, "result");
    JsonValue* caps    = result ? json_obj_get(result, "capabilities") : NULL;
    JsonValue* sync     = caps ? json_obj_get(caps, "textDocumentSync") : NULL;
    check(sync != NULL, "initialize result missing capabilities.textDocumentSync");
    // A client only ever sends a request the server advertised, so an
    // unadvertised capability is a handler nobody will ever call.
    JsonValue* ds_cap = caps ? json_obj_get(caps, "documentSymbolProvider") : NULL;
    check(ds_cap && ds_cap->kind == Json_Bool && ds_cap->boolean,
          "initialize result missing capabilities.documentSymbolProvider");
    JsonValue* fmt_cap = caps ? json_obj_get(caps, "documentFormattingProvider") : NULL;
    check(fmt_cap && fmt_cap->kind == Json_Bool && fmt_cap->boolean,
          "initialize result missing capabilities.documentFormattingProvider");
    json_free(init_resp);
  }

  JsonValue* initd = notification("initialized", json_new_object());
  send_msg(s.to_server, initd);
  json_free(initd);

  // didOpen the broken package -> exactly one publishDiagnostics carrying one
  // diagnostic, with the right uri, position and message.
  JsonValue* open_broken = notification("textDocument/didOpen", did_open_params(broken_uri, BROKEN_CONTENT));
  send_msg(s.to_server, open_broken);
  json_free(open_broken);

  JsonValue* diag_msg = read_msg(s.from_server);
  check(diag_msg != NULL, "no publishDiagnostics for broken package");
  if (diag_msg) {
    JsonValue* method = json_obj_get(diag_msg, "method");
    check(method && method->kind == Json_String && strcmp(method->string, "textDocument/publishDiagnostics") == 0,
          "expected textDocument/publishDiagnostics, got %s", (method && method->kind == Json_String) ? method->string : "?");
    JsonValue* params = json_obj_get(diag_msg, "params");
    JsonValue* uri     = params ? json_obj_get(params, "uri") : NULL;
    check(uri && uri->kind == Json_String && strcmp(uri->string, broken_uri) == 0,
          "diagnostics published for wrong uri: %s", (uri && uri->kind == Json_String) ? uri->string : "?");
    JsonValue* diags = params ? json_obj_get(params, "diagnostics") : NULL;
    size_t     count = 0;
    for (JsonArrayItem* it = diags ? diags->array_first : NULL; it; it = it->next) count += 1;
    check(count == 1, "expected exactly 1 diagnostic for broken package, got %zu", count);
    if (diags && diags->array_first) {
      JsonValue* d       = diags->array_first->value;
      JsonValue* message = json_obj_get(d, "message");
      check(message && message->kind == Json_String && strstr(message->string, "mismatched types") != NULL,
            "unexpected diagnostic message: %s", (message && message->kind == Json_String) ? message->string : "?");
      JsonValue* range = json_obj_get(d, "range");
      JsonValue* start  = range ? json_obj_get(range, "start") : NULL;
      JsonValue* line    = start ? json_obj_get(start, "line") : NULL;
      JsonValue* character = start ? json_obj_get(start, "character") : NULL;
      // source line 4, col 3 -> LSP line 3, character 2
      check(line && (int)line->number == 3, "expected line 3, got %d", line ? (int)line->number : -1);
      check(character && (int)character->number == 2, "expected character 2, got %d", character ? (int)character->number : -1);
    }
    json_free(diag_msg);
  }

  // didOpen the clean package -> an explicit empty diagnostics array.
  JsonValue* open_clean = notification("textDocument/didOpen", did_open_params(clean_uri, CLEAN_CONTENT));
  send_msg(s.to_server, open_clean);
  json_free(open_clean);

  JsonValue* clean_msg = read_msg(s.from_server);
  check(clean_msg != NULL, "no publishDiagnostics for clean package");
  if (clean_msg) {
    JsonValue* params = json_obj_get(clean_msg, "params");
    JsonValue* uri     = params ? json_obj_get(params, "uri") : NULL;
    check(uri && uri->kind == Json_String && strcmp(uri->string, clean_uri) == 0,
          "clean diagnostics published for wrong uri: %s", (uri && uri->kind == Json_String) ? uri->string : "?");
    JsonValue* diags = params ? json_obj_get(params, "diagnostics") : NULL;
    check(diags && diags->kind == Json_Array && diags->array_first == NULL,
          "expected an empty diagnostics array for the clean package");
    json_free(clean_msg);
  }

  // didChange the already-open clean package to broken content. The error
  // exists only in the buffer, never on disk, so this proves the overlay
  // reaches the checker rather than just didOpen's initial text.
  JsonValue* change_to_broken = notification("textDocument/didChange", did_change_params(clean_uri, BROKEN_CONTENT));
  send_msg(s.to_server, change_to_broken);
  json_free(change_to_broken);

  JsonValue* change_diag_msg = read_msg(s.from_server);
  check(change_diag_msg != NULL, "no publishDiagnostics after didChange introduced an error");
  if (change_diag_msg) {
    JsonValue* params = json_obj_get(change_diag_msg, "params");
    JsonValue* diags   = params ? json_obj_get(params, "diagnostics") : NULL;
    size_t     count    = 0;
    for (JsonArrayItem* it = diags ? diags->array_first : NULL; it; it = it->next) count += 1;
    check(count == 1, "expected exactly 1 diagnostic after didChange introduced an error, got %zu", count);
    json_free(change_diag_msg);
  }

  // didChange it back to clean: diagnostics must clear.
  JsonValue* change_to_clean = notification("textDocument/didChange", did_change_params(clean_uri, CLEAN_CONTENT));
  send_msg(s.to_server, change_to_clean);
  json_free(change_to_clean);

  JsonValue* change_clean_msg = read_msg(s.from_server);
  check(change_clean_msg != NULL, "no publishDiagnostics after didChange fixed the error");
  if (change_clean_msg) {
    JsonValue* params = json_obj_get(change_clean_msg, "params");
    JsonValue* diags   = params ? json_obj_get(params, "diagnostics") : NULL;
    check(diags && diags->kind == Json_Array && diags->array_first == NULL,
          "expected diagnostics to clear after didChange fixed the error");
    json_free(change_clean_msg);
  }

  // hover over the `n` use site in hover_pkg -> a "variable n" heading, the
  // type, and the `let` line it was bound on. This client declared no
  // hover.contentFormat, so every hover here comes back as the plaintext
  // layout; the markdown one gets its own server at the end of this file.
  char hover_dir[4096]; snprintf(hover_dir, sizeof(hover_dir), "%s/hover_pkg", fixtures);
  char* hover_uri = uri_for(hover_dir, "hover_pkg.3b");

  JsonValue* open_hover = notification("textDocument/didOpen", did_open_params(hover_uri, HOVER_CONTENT));
  send_msg(s.to_server, open_hover);
  json_free(open_hover);
  JsonValue* hover_open_diag = read_msg(s.from_server); // discard -- expected clean
  if (hover_open_diag) json_free(hover_open_diag);

  JsonValue* hover_req = request(3, "textDocument/hover", position_params(hover_uri, 4, 4));
  send_msg(s.to_server, hover_req);
  json_free(hover_req);
  JsonValue* hover_resp = read_msg(s.from_server);
  check(hover_resp != NULL, "no response to hover");
  if (hover_resp) {
    JsonValue* result   = json_obj_get(hover_resp, "result");
    JsonValue* contents = result ? json_obj_get(result, "contents") : NULL;
    JsonValue* value     = contents ? json_obj_get(contents, "value") : NULL;
    const char* expect = "variable n\n\ni32\n\nDefined in hover_pkg.3b:4";
    check(value && value->kind == Json_String && strcmp(value->string, expect) == 0,
          "expected hover \"%s\", got %s", expect,
          (value && value->kind == Json_String) ? value->string : "(null result)");
    json_free(hover_resp);
  }

  // goto-definition on a call's callee in def_pkg -> `fn helper`'s
  // declaration.
  char def_dir[4096]; snprintf(def_dir, sizeof(def_dir), "%s/def_pkg", fixtures);
  char* def_uri = uri_for(def_dir, "def_pkg.3b");

  JsonValue* open_def = notification("textDocument/didOpen", did_open_params(def_uri, DEF_CONTENT));
  send_msg(s.to_server, open_def);
  json_free(open_def);
  JsonValue* def_open_diag = read_msg(s.from_server); // discard -- expected clean
  if (def_open_diag) json_free(def_open_diag);

  JsonValue* def_req = request(4, "textDocument/definition", position_params(def_uri, 6, 3));
  send_msg(s.to_server, def_req);
  json_free(def_req);
  JsonValue* def_resp = read_msg(s.from_server);
  check(def_resp != NULL, "no response to definition");
  if (def_resp) {
    JsonValue* result = json_obj_get(def_resp, "result");
    JsonValue* uri     = result ? json_obj_get(result, "uri") : NULL;
    check(uri && uri->kind == Json_String && strcmp(uri->string, def_uri) == 0,
          "definition resolved to wrong uri: %s", (uri && uri->kind == Json_String) ? uri->string : "(null result)");
    JsonValue* range = result ? json_obj_get(result, "range") : NULL;
    JsonValue* start  = range ? json_obj_get(range, "start") : NULL;
    JsonValue* line    = start ? json_obj_get(start, "line") : NULL;
    JsonValue* character = start ? json_obj_get(start, "character") : NULL;
    // `fn helper` declared at source line 3, col 1 -> line 2, character 0
    check(line && (int)line->number == 2, "expected definition line 2, got %d", line ? (int)line->number : -1);
    check(character && (int)character->number == 0, "expected definition character 0, got %d", character ? (int)character->number : -1);
    json_free(def_resp);
  }

  free(def_uri);

  // goto-definition on a type annotation: a named type, a pointer to it and a
  // handle to it, all in one param list, must all resolve to `struct Mesh` at
  // source line 3, col 1 (line 2, character 0). Type annotations are not
  // TypedNodes, so find_node_at_position alone never covered this; see
  // find_type_annotation_at_position and type_ref_named_target in lib3b.c.
  char def_type_dir[4096]; snprintf(def_type_dir, sizeof(def_type_dir), "%s/def_type_pkg", fixtures);
  char* def_type_uri = uri_for(def_type_dir, "def_type_pkg.3b");

  JsonValue* open_def_type = notification("textDocument/didOpen", did_open_params(def_type_uri, DEF_TYPE_CONTENT));
  send_msg(s.to_server, open_def_type);
  json_free(open_def_type);
  JsonValue* def_type_open_diag = read_msg(s.from_server); // discard -- expected clean
  if (def_type_open_diag) json_free(def_type_open_diag);

  struct { int line, character; const char* label; } def_type_cases[] = {
    {6, 16, "bare named type"},
    {6, 23, "pointer type"},
    {6, 31, "handle type"},
  };
  int def_type_id = 20;
  for (size_t i = 0; i < sizeof(def_type_cases) / sizeof(def_type_cases[0]); i += 1) {
    JsonValue* req = request(def_type_id, "textDocument/definition",
                              position_params(def_type_uri, def_type_cases[i].line, def_type_cases[i].character));
    def_type_id += 1;
    send_msg(s.to_server, req);
    json_free(req);
    JsonValue* resp = read_msg(s.from_server);
    check(resp != NULL, "no response to definition (%s)", def_type_cases[i].label);
    if (resp) {
      JsonValue* result = json_obj_get(resp, "result");
      JsonValue* uri     = result ? json_obj_get(result, "uri") : NULL;
      check(uri && uri->kind == Json_String && strcmp(uri->string, def_type_uri) == 0,
            "definition (%s) resolved to wrong uri: %s", def_type_cases[i].label,
            (uri && uri->kind == Json_String) ? uri->string : "(null result)");
      JsonValue* range = result ? json_obj_get(result, "range") : NULL;
      JsonValue* start  = range ? json_obj_get(range, "start") : NULL;
      JsonValue* dline    = start ? json_obj_get(start, "line") : NULL;
      JsonValue* dchar     = start ? json_obj_get(start, "character") : NULL;
      check(dline && (int)dline->number == 2,
            "definition (%s): expected line 2, got %d", def_type_cases[i].label, dline ? (int)dline->number : -1);
      check(dchar && (int)dchar->number == 0,
            "definition (%s): expected character 0, got %d", def_type_cases[i].label, dchar ? (int)dchar->number : -1);
      json_free(resp);
    }
  }
  free(def_type_uri);

  // goto-definition on the same `n` use site hover resolved above. A bare
  // local read resolves through the decl_token the checker records (3b.h),
  // landing on the enclosing `(let ...)` form: source line 4, col 3, so LSP
  // line 3, character 2.
  JsonValue* nodef_req = request(5, "textDocument/definition", position_params(hover_uri, 4, 4));
  send_msg(s.to_server, nodef_req);
  json_free(nodef_req);
  JsonValue* nodef_resp = read_msg(s.from_server);
  check(nodef_resp != NULL, "no response to definition (bare identifier)");
  if (nodef_resp) {
    JsonValue* result = json_obj_get(nodef_resp, "result");
    check(result != NULL && result->kind != Json_Null,
          "expected definition on a local variable to resolve to its enclosing `let`, got no result");
    if (result) {
      JsonValue* uri = json_obj_get(result, "uri");
      check(uri && uri->kind == Json_String && strcmp(uri->string, hover_uri) == 0,
            "local-variable definition resolved to wrong uri: %s", (uri && uri->kind == Json_String) ? uri->string : "?");
      JsonValue* range = json_obj_get(result, "range");
      JsonValue* start  = range ? json_obj_get(range, "start") : NULL;
      JsonValue* line    = start ? json_obj_get(start, "line") : NULL;
      JsonValue* character = start ? json_obj_get(start, "character") : NULL;
      check(line && (int)line->number == 3, "expected local-variable definition line 3, got %d", line ? (int)line->number : -1);
      check(character && (int)character->number == 2,
            "expected local-variable definition character 2, got %d", character ? (int)character->number : -1);
    }
    json_free(nodef_resp);
  }
  free(hover_uri);

  // completion in completion_pkg: prefix "frob" (line 2, char 8) must match
  // both top-level functions and nothing else.
  char comp_dir[4096]; snprintf(comp_dir, sizeof(comp_dir), "%s/completion_pkg", fixtures);
  char* comp_uri = uri_for(comp_dir, "completion_pkg.3b");

  JsonValue* open_comp = notification("textDocument/didOpen", did_open_params(comp_uri, COMPLETION_CONTENT));
  send_msg(s.to_server, open_comp);
  json_free(open_comp);
  JsonValue* comp_open_diag = read_msg(s.from_server); // discard -- expected clean
  if (comp_open_diag) json_free(comp_open_diag);

  JsonValue* comp_req = request(6, "textDocument/completion", position_params(comp_uri, 2, 8));
  send_msg(s.to_server, comp_req);
  json_free(comp_req);
  JsonValue* comp_resp = read_msg(s.from_server);
  check(comp_resp != NULL, "no response to completion");
  if (comp_resp) {
    JsonValue* result = json_obj_get(comp_resp, "result");
    JsonValue* items    = result ? json_obj_get(result, "items") : NULL;
    check(has_completion_label(items, "frobnicate"), "completion missing \"frobnicate\"");
    check(has_completion_label(items, "frobulate"), "completion missing \"frobulate\"");
    check(!has_completion_label(items, "double"), "completion leaked an unrelated package's symbol");
    json_free(comp_resp);
  }

  // didChange completion_pkg to a buffer with an unclosed paren, then complete
  // at the same position: both functions must still appear, served from the
  // cache the successful check above left behind.
  JsonValue* break_comp = notification("textDocument/didChange", did_change_params(comp_uri, COMPLETION_BROKEN_CONTENT));
  send_msg(s.to_server, break_comp);
  json_free(break_comp);
  JsonValue* comp_broken_diag = read_msg(s.from_server); // discard -- don't care about its content here
  if (comp_broken_diag) json_free(comp_broken_diag);

  JsonValue* comp_req2 = request(7, "textDocument/completion", position_params(comp_uri, 2, 8));
  send_msg(s.to_server, comp_req2);
  json_free(comp_req2);
  JsonValue* comp_resp2 = read_msg(s.from_server);
  check(comp_resp2 != NULL, "no response to completion (after buffer broke)");
  if (comp_resp2) {
    JsonValue* result = json_obj_get(comp_resp2, "result");
    JsonValue* items    = result ? json_obj_get(result, "items") : NULL;
    check(has_completion_label(items, "frobnicate"),
          "completion lost \"frobnicate\" after the live buffer broke -- cache fallback isn't working");
    check(has_completion_label(items, "frobulate"),
          "completion lost \"frobulate\" after the live buffer broke -- cache fallback isn't working");
    json_free(comp_resp2);
  }
  free(comp_uri);

  // Cross-package completion in importer_pkg. Prefix "lib", before any "/",
  // must offer "libpkg" itself as a Module; prefix "libpkg/" must offer the
  // qualified public member and must not leak the private one.
  char imp_dir[4096]; snprintf(imp_dir, sizeof(imp_dir), "%s/importer_pkg", fixtures);
  char* imp_uri = uri_for(imp_dir, "importer_pkg.3b");

  JsonValue* open_imp = notification("textDocument/didOpen", did_open_params(imp_uri, IMPORTER_CONTENT));
  send_msg(s.to_server, open_imp);
  json_free(open_imp);
  JsonValue* imp_open_diag = read_msg(s.from_server); // discard -- expected clean
  if (imp_open_diag) json_free(imp_open_diag);

  JsonValue* imp_req1 = request(8, "textDocument/completion", position_params(imp_uri, 2, 11));
  send_msg(s.to_server, imp_req1);
  json_free(imp_req1);
  JsonValue* imp_resp1 = read_msg(s.from_server);
  check(imp_resp1 != NULL, "no response to completion (import name, unqualified)");
  if (imp_resp1) {
    JsonValue* result = json_obj_get(imp_resp1, "result");
    JsonValue* items    = result ? json_obj_get(result, "items") : NULL;
    check(has_completion_label(items, "libpkg"), "completion missing imported package name \"libpkg\"");
    json_free(imp_resp1);
  }

  JsonValue* imp_req2 = request(9, "textDocument/completion", position_params(imp_uri, 5, 10));
  send_msg(s.to_server, imp_req2);
  json_free(imp_req2);
  JsonValue* imp_resp2 = read_msg(s.from_server);
  check(imp_resp2 != NULL, "no response to completion (qualified)");
  if (imp_resp2) {
    JsonValue* result = json_obj_get(imp_resp2, "result");
    JsonValue* items    = result ? json_obj_get(result, "items") : NULL;
    check(has_completion_label(items, "libpkg/pub-fn"), "qualified completion missing \"libpkg/pub-fn\"");
    check(!has_completion_label(items, "libpkg/priv-fn"), "qualified completion leaked private \"libpkg/priv-fn\"");
    json_free(imp_resp2);
  }
  free(imp_uri);

  // Richer hover: a call, a struct construction and an enum access each show
  // their full declaration, not just a resolved type.
  char hover2_dir[4096]; snprintf(hover2_dir, sizeof(hover2_dir), "%s/hover2_pkg", fixtures);
  char* hover2_uri = uri_for(hover2_dir, "hover2_pkg.3b");

  JsonValue* open_hover2 = notification("textDocument/didOpen", did_open_params(hover2_uri, HOVER2_CONTENT));
  send_msg(s.to_server, open_hover2);
  json_free(open_hover2);
  JsonValue* hover2_open_diag = read_msg(s.from_server); // discard -- expected clean
  if (hover2_open_diag) json_free(hover2_open_diag);

  struct { int line, character; const char* expect; const char* label; } hover2_cases[] = {
    {10, 5,  "function helper2\n\nfn helper2 [a i32 b i32] i32\n\nDefined in hover2_pkg.3b:7", "call"},
    {11, 20, "struct Vector2\n\nstruct Vector2 [x f32 y f32]\n\nDefined in hover2_pkg.3b:3",   "struct construction"},
    // An enum access is a pure, closed expression (hover_eval_is_pure in
    // lib3b.c), so unlike the two cases above, hover-eval appends its computed
    // value to the signature.
    {12, 17, "enum Color\n\nenum Color [Red Green Blue] = 0\n\nDefined in hover2_pkg.3b:5", "enum access"},
  };
  int next_id = 10;
  for (size_t i = 0; i < sizeof(hover2_cases) / sizeof(hover2_cases[0]); i += 1) {
    JsonValue* req = request(next_id, "textDocument/hover",
                              position_params(hover2_uri, hover2_cases[i].line, hover2_cases[i].character));
    next_id += 1;
    send_msg(s.to_server, req);
    json_free(req);
    JsonValue* resp = read_msg(s.from_server);
    check(resp != NULL, "no response to hover (%s)", hover2_cases[i].label);
    if (resp) {
      JsonValue* result   = json_obj_get(resp, "result");
      JsonValue* contents = result ? json_obj_get(result, "contents") : NULL;
      JsonValue* value     = contents ? json_obj_get(contents, "value") : NULL;
      check(value && value->kind == Json_String && strcmp(value->string, hover2_cases[i].expect) == 0,
            "hover (%s): expected \"%s\", got %s", hover2_cases[i].label, hover2_cases[i].expect,
            (value && value->kind == Json_String) ? value->string : "(null result)");
      json_free(resp);
    }
  }
  free(hover2_uri);

  // Hover-eval: a `val` with an arithmetic-only initializer computes its real
  // value, and an expression that traps in the VM (division by a runtime zero,
  // BcResult.trapped in bcvm.c) neither crashes the server nor appends a value.
  char hover_eval_dir[4096]; snprintf(hover_eval_dir, sizeof(hover_eval_dir), "%s/hover_eval_pkg", fixtures);
  char* hover_eval_uri = uri_for(hover_eval_dir, "hover_eval_pkg.3b");

  JsonValue* open_hover_eval = notification("textDocument/didOpen", did_open_params(hover_eval_uri, HOVER_EVAL_CONTENT));
  send_msg(s.to_server, open_hover_eval);
  json_free(open_hover_eval);
  JsonValue* hover_eval_open_diag = read_msg(s.from_server); // discard -- expected clean
  if (hover_eval_open_diag) json_free(hover_eval_open_diag);

  struct { int line, character; const char* expect; const char* label; } hover_eval_cases[] = {
    {5, 2, "variable HOUR_SECONDS\n\ni32 = 3600\n\nDefined in hover_eval_pkg.3b:3", "pure val reference"},
    // The outer `(/ ...)` form is an expression, not a reference to anything
    // declared, so it gets neither a heading nor a declaration site.
    {6, 2, "i32", "div-by-zero trap (no eval suffix)"},
  };
  for (size_t i = 0; i < sizeof(hover_eval_cases) / sizeof(hover_eval_cases[0]); i += 1) {
    JsonValue* req = request(next_id, "textDocument/hover",
                              position_params(hover_eval_uri, hover_eval_cases[i].line, hover_eval_cases[i].character));
    next_id += 1;
    send_msg(s.to_server, req);
    json_free(req);
    JsonValue* resp = read_msg(s.from_server);
    check(resp != NULL, "no response to hover (%s)", hover_eval_cases[i].label);
    if (resp) {
      JsonValue* result   = json_obj_get(resp, "result");
      JsonValue* contents = result ? json_obj_get(result, "contents") : NULL;
      JsonValue* value     = contents ? json_obj_get(contents, "value") : NULL;
      check(value && value->kind == Json_String && strcmp(value->string, hover_eval_cases[i].expect) == 0,
            "hover-eval (%s): expected \"%s\", got %s", hover_eval_cases[i].label, hover_eval_cases[i].expect,
            (value && value->kind == Json_String) ? value->string : "(null result)");
      json_free(resp);
    }
  }

  // One more ordinary hover, to prove the trap above did not take the server
  // down.
  JsonValue* alive_req = request(next_id, "textDocument/hover", position_params(hover_eval_uri, 5, 2));
  next_id += 1;
  send_msg(s.to_server, alive_req);
  json_free(alive_req);
  JsonValue* alive_resp = read_msg(s.from_server);
  check(alive_resp != NULL, "server did not respond after div-by-zero trap -- may have crashed");
  if (alive_resp) json_free(alive_resp);

  free(hover_eval_uri);

  // Builtin hover: `print`/`println` resolve to no declaration, so they show
  // their call shapes -- both the stdout form and the one taking a leading
  // `stream` -- and a call that actually passes a stream shows the same text.
  char hover_builtin_dir[4096];
  snprintf(hover_builtin_dir, sizeof(hover_builtin_dir), "%s/hover_builtin_pkg", fixtures);
  char* hover_builtin_uri = uri_for(hover_builtin_dir, "hover_builtin_pkg.3b");

  JsonValue* open_hover_builtin =
    notification("textDocument/didOpen", did_open_params(hover_builtin_uri, HOVER_BUILTIN_CONTENT));
  send_msg(s.to_server, open_hover_builtin);
  json_free(open_hover_builtin);
  JsonValue* hover_builtin_open_diag = read_msg(s.from_server); // discard -- expected clean
  if (hover_builtin_open_diag) json_free(hover_builtin_open_diag);

  struct { int line, character; const char* expect; const char* label; } hover_builtin_cases[] = {
    // Headed "builtin", and with no "Defined in" line: a builtin is dispatched
    // by name in checker.c and has no declaration anywhere to cite.
    {5, 3, "builtin println\n\n(println \"template {}\" values...) void\n"
           "(println stream \"template {}\" values...) void",
     "println, stdout form"},
    {7, 3, "builtin print\n\n(print \"template {}\" values...) void\n"
           "(print stream \"template {}\" values...) void",
     "print, stream form"},
  };
  for (size_t i = 0; i < sizeof(hover_builtin_cases) / sizeof(hover_builtin_cases[0]); i += 1) {
    JsonValue* req = request(next_id, "textDocument/hover",
                              position_params(hover_builtin_uri, hover_builtin_cases[i].line,
                                              hover_builtin_cases[i].character));
    next_id += 1;
    send_msg(s.to_server, req);
    json_free(req);
    JsonValue* resp = read_msg(s.from_server);
    check(resp != NULL, "no response to hover (%s)", hover_builtin_cases[i].label);
    if (resp) {
      JsonValue* result   = json_obj_get(resp, "result");
      JsonValue* contents = result ? json_obj_get(result, "contents") : NULL;
      JsonValue* value     = contents ? json_obj_get(contents, "value") : NULL;
      check(value && value->kind == Json_String && strcmp(value->string, hover_builtin_cases[i].expect) == 0,
            "builtin hover (%s): expected \"%s\", got %s", hover_builtin_cases[i].label,
            hover_builtin_cases[i].expect,
            (value && value->kind == Json_String) ? value->string : "(null result)");
      json_free(resp);
    }
  }

  // Doc comments -- see HOVER_DOC_CONTENT for what each position is meant to
  // prove. The two undocumented cases matter as much as the documented ones:
  // they are the fixture's trailing comment and blank-line-separated comment,
  // neither of which may be mistaken for a declaration's doc.
  char hover_doc_dir[4096]; snprintf(hover_doc_dir, sizeof(hover_doc_dir), "%s/hover_doc_pkg", fixtures);
  char* hover_doc_uri = uri_for(hover_doc_dir, "hover_doc_pkg.3b");

  JsonValue* open_hover_doc =
    notification("textDocument/didOpen", did_open_params(hover_doc_uri, HOVER_DOC_CONTENT));
  send_msg(s.to_server, open_hover_doc);
  json_free(open_hover_doc);
  JsonValue* hover_doc_open_diag = read_msg(s.from_server); // discard -- expected clean
  if (hover_doc_open_diag) json_free(hover_doc_open_diag);

  struct { int line, character; const char* expect; const char* label; } hover_doc_cases[] = {
    {21, 8,  "function documented\n\nfn documented [a i32 b i32] i32\n\n"
             "Adds two numbers.\n\nAnd explains itself at length.\n\n"
             "Defined in hover_doc_pkg.3b:6", "documented fn"},
    {21, 28, "function undocumented\n\nfn undocumented [] i32\n\n"
             "Defined in hover_doc_pkg.3b:10", "trailing comment is not a doc"},
    {21, 43, "function spaced\n\nfn spaced [] i32\n\n"
             "Defined in hover_doc_pkg.3b:15", "blank-line-separated comment is not a doc"},
    {20, 18, "struct Documented\n\nstruct Documented [x i32]\n\n"
             "A documented struct.\n\nDefined in hover_doc_pkg.3b:19", "doc through a type annotation"},
  };
  for (size_t i = 0; i < sizeof(hover_doc_cases) / sizeof(hover_doc_cases[0]); i += 1) {
    JsonValue* req = request(next_id, "textDocument/hover",
                              position_params(hover_doc_uri, hover_doc_cases[i].line,
                                              hover_doc_cases[i].character));
    next_id += 1;
    send_msg(s.to_server, req);
    json_free(req);
    JsonValue* resp = read_msg(s.from_server);
    check(resp != NULL, "no response to hover (%s)", hover_doc_cases[i].label);
    if (resp) {
      JsonValue* result   = json_obj_get(resp, "result");
      JsonValue* contents = result ? json_obj_get(result, "contents") : NULL;
      JsonValue* value     = contents ? json_obj_get(contents, "value") : NULL;
      check(value && value->kind == Json_String && strcmp(value->string, hover_doc_cases[i].expect) == 0,
            "doc hover (%s): expected \"%s\", got %s", hover_doc_cases[i].label, hover_doc_cases[i].expect,
            (value && value->kind == Json_String) ? value->string : "(null result)");
      json_free(resp);
    }
  }

  // The same signature and the same doc comment reach the completion popup,
  // not just the hover one: completing the already-written "documented" (line
  // 21, character 16 -- the end of that name) offers it with both attached.
  JsonValue* doc_comp_req = request(next_id, "textDocument/completion", position_params(hover_doc_uri, 21, 16));
  next_id += 1;
  send_msg(s.to_server, doc_comp_req);
  json_free(doc_comp_req);
  JsonValue* doc_comp_resp = read_msg(s.from_server);
  check(doc_comp_resp != NULL, "no response to completion (documented fn)");
  if (doc_comp_resp) {
    JsonValue*  result = json_obj_get(doc_comp_resp, "result");
    JsonValue*  items    = result ? json_obj_get(result, "items") : NULL;
    const char* detail   = completion_item_field(items, "documented", "detail");
    const char* doc      = completion_item_field(items, "documented", "documentation");
    check(has_completion_label(items, "documented"), "completion missing \"documented\"");
    check(detail && strcmp(detail, "fn documented [a i32 b i32] i32") == 0,
          "completion detail: expected the declaration header, got %s", detail ? detail : "(none)");
    check(doc && strcmp(doc, "Adds two numbers.\n\nAnd explains itself at length.") == 0,
          "completion documentation: expected the doc comment, got %s", doc ? doc : "(none)");
    json_free(doc_comp_resp);
  }
  free(hover_doc_uri);

  // Field references -- see HOVER_FIELD_CONTENT for what each position covers.
  // The name is qualified with the owning struct because the field type alone
  // ("i32") says nothing about which struct answered, and the two hops of one
  // `.` chain resolve against different ones.
  char hover_field_dir[4096];
  snprintf(hover_field_dir, sizeof(hover_field_dir), "%s/hover_field_pkg", fixtures);
  char* hover_field_uri = uri_for(hover_field_dir, "hover_field_pkg.3b");

  JsonValue* open_hover_field =
    notification("textDocument/didOpen", did_open_params(hover_field_uri, HOVER_FIELD_CONTENT));
  send_msg(s.to_server, open_hover_field);
  json_free(open_hover_field);
  JsonValue* hover_field_open_diag = read_msg(s.from_server); // discard -- expected clean
  if (hover_field_open_diag) json_free(hover_field_open_diag);

  struct { int line, character; const char* expect; const char* label; } hover_field_cases[] = {
    {12, 7,  "field Outer.inner\n\nInner*\n\nThe inner box this one wraps.\n\n"
             "Defined in hover_field_pkg.3b:8", "first hop of a `.` chain, through a pointer"},
    // The second hop resolves against Inner, not Outer, and its declaration
    // shares a line with `(struct Inner ...)` -- so the comment above that
    // line documents the struct and is deliberately not shown here.
    {12, 13, "field Inner.w\n\ni32\n\nDefined in hover_field_pkg.3b:3", "second hop of a `.` chain"},
    {15, 9,  "field Outer.label\n\nstring\n\nDefined in hover_field_pkg.3b:9", "`get` field name"},
    {18, 9,  "field Inner.w\n\ni32\n\nDefined in hover_field_pkg.3b:3", "destructuring pattern field"},
    // A Map hop is a key expression, never a field: it must still hover as the
    // string literal it is, evaluated by hover-eval like any other closed one.
    {22, 8,  "string = \"two\"", "`.` hop on a Map is a key, not a field"},
    // Written against Wrapper, but credited to the struct that declares it --
    // Inner, reached through Wrapper's anonymous member.
    {35, 11, "field Inner.w\n\ni32\n\nDefined in hover_field_pkg.3b:3", "field of an anonymous member"},
    // Declared by the inline `(struct [tag i32])` inside Payload, whose
    // synthesized name is no use to anyone, so Payload qualifies it instead.
    {35, 22, "field Payload.tag\n\ni32\n\nDefined in hover_field_pkg.3b:26",
     "field of an inline anonymous member"},
  };
  for (size_t i = 0; i < sizeof(hover_field_cases) / sizeof(hover_field_cases[0]); i += 1) {
    JsonValue* req = request(next_id, "textDocument/hover",
                              position_params(hover_field_uri, hover_field_cases[i].line,
                                              hover_field_cases[i].character));
    next_id += 1;
    send_msg(s.to_server, req);
    json_free(req);
    JsonValue* resp = read_msg(s.from_server);
    check(resp != NULL, "no response to hover (%s)", hover_field_cases[i].label);
    if (resp) {
      JsonValue* result   = json_obj_get(resp, "result");
      JsonValue* contents = result ? json_obj_get(result, "contents") : NULL;
      JsonValue* value     = contents ? json_obj_get(contents, "value") : NULL;
      check(value && value->kind == Json_String && strcmp(value->string, hover_field_cases[i].expect) == 0,
            "field hover (%s): expected \"%s\", got %s", hover_field_cases[i].label,
            hover_field_cases[i].expect,
            (value && value->kind == Json_String) ? value->string : "(null result)");
      json_free(resp);
    }
  }

  // goto-definition follows the same resolution: `inner` (line 12, character 7)
  // lands on the field's own line in `struct Outer` -- source line 8, col 3, so
  // LSP line 7, character 2 -- not on the struct's opening line.
  JsonValue* field_def_req = request(next_id, "textDocument/definition",
                                      position_params(hover_field_uri, 12, 7));
  next_id += 1;
  send_msg(s.to_server, field_def_req);
  json_free(field_def_req);
  JsonValue* field_def_resp = read_msg(s.from_server);
  check(field_def_resp != NULL, "no response to definition (field name)");
  if (field_def_resp) {
    JsonValue* result = json_obj_get(field_def_resp, "result");
    check(result != NULL && result->kind != Json_Null,
          "expected definition on a field name to resolve to its declaration, got no result");
    if (result && result->kind != Json_Null) {
      JsonValue* range     = json_obj_get(result, "range");
      JsonValue* start      = range ? json_obj_get(range, "start") : NULL;
      JsonValue* line       = start ? json_obj_get(start, "line") : NULL;
      JsonValue* character  = start ? json_obj_get(start, "character") : NULL;
      check(line && (int)line->number == 7, "expected field definition line 7, got %d",
            line ? (int)line->number : -1);
      check(character && (int)character->number == 2, "expected field definition character 2, got %d",
            character ? (int)character->number : -1);
    }
    json_free(field_def_resp);
  }
  free(hover_field_uri);

  // The same shapes reach completion: mid-typing "printl" (line 5, character 9
  // -- inside the existing "println") offers the builtin with its common shape
  // as `detail` and every shape, stream form included, as `documentation`.
  JsonValue* builtin_comp_req = request(next_id, "textDocument/completion",
                                         position_params(hover_builtin_uri, 5, 9));
  next_id += 1;
  send_msg(s.to_server, builtin_comp_req);
  json_free(builtin_comp_req);
  JsonValue* builtin_comp_resp = read_msg(s.from_server);
  check(builtin_comp_resp != NULL, "no response to builtin completion");
  if (builtin_comp_resp) {
    JsonValue*  result = json_obj_get(builtin_comp_resp, "result");
    JsonValue*  items    = result ? json_obj_get(result, "items") : NULL;
    const char* detail   = completion_item_field(items, "println", "detail");
    const char* doc      = completion_item_field(items, "println", "documentation");
    check(has_completion_label(items, "println"), "completion missing builtin \"println\"");
    check(detail && strcmp(detail, "(println \"template {}\" values...) void") == 0,
          "builtin completion detail: expected the one-line shape, got %s", detail ? detail : "(none)");
    check(doc && strstr(doc, "(println stream \"template {}\" values...) void") != NULL,
          "builtin completion documentation is missing the stream shape, got %s", doc ? doc : "(none)");
    json_free(builtin_comp_resp);
  }
  free(hover_builtin_uri);

  // Partial-parse completion: didOpen clean, seeding the cache with only
  // "existing-fn", then didChange to a buffer that adds "brand-new-fn" and
  // leaves the last form unclosed. Completing "brand" must still offer
  // "brand-new-fn", which only the tolerant recompile can supply -- the stale
  // cache has never seen that name.
  char partial_dir[4096]; snprintf(partial_dir, sizeof(partial_dir), "%s/partial_pkg", fixtures);
  char* partial_uri = uri_for(partial_dir, "partial_pkg.3b");

  JsonValue* open_partial = notification("textDocument/didOpen", did_open_params(partial_uri, PARTIAL_CONTENT));
  send_msg(s.to_server, open_partial);
  json_free(open_partial);
  JsonValue* partial_open_diag = read_msg(s.from_server); // discard -- expected clean
  if (partial_open_diag) json_free(partial_open_diag);

  JsonValue* break_partial = notification("textDocument/didChange", did_change_params(partial_uri, PARTIAL_BROKEN_CONTENT));
  send_msg(s.to_server, break_partial);
  json_free(break_partial);
  JsonValue* partial_broken_diag = read_msg(s.from_server); // discard -- don't care about its content here
  if (partial_broken_diag) json_free(partial_broken_diag);

  JsonValue* partial_req = request(15, "textDocument/completion", position_params(partial_uri, 9, 8));
  send_msg(s.to_server, partial_req);
  json_free(partial_req);
  JsonValue* partial_resp = read_msg(s.from_server);
  check(partial_resp != NULL, "no response to completion (partial-parse)");
  if (partial_resp) {
    JsonValue* result = json_obj_get(partial_resp, "result");
    JsonValue* items    = result ? json_obj_get(result, "items") : NULL;
    check(has_completion_label(items, "brand-new-fn"),
          "completion missing \"brand-new-fn\" -- partial-parse (patch + tolerate-errors) recompile didn't run");
    json_free(partial_resp);
  }
  free(partial_uri);

  // Local and parameter completion; see LOCALS_CONTENT above. didOpen clean
  // first to seed the cache, then one didChange plus completion round trip per
  // broken variant.
  char locals_dir[4096]; snprintf(locals_dir, sizeof(locals_dir), "%s/locals_pkg", fixtures);
  char* locals_uri = uri_for(locals_dir, "locals_pkg.3b");

  JsonValue* open_locals = notification("textDocument/didOpen", did_open_params(locals_uri, LOCALS_CONTENT));
  send_msg(s.to_server, open_locals);
  json_free(open_locals);
  JsonValue* locals_open_diag = read_msg(s.from_server); // discard -- expected clean
  if (locals_open_diag) json_free(locals_open_diag);

  JsonValue* break_locals_a = notification("textDocument/didChange", did_change_params(locals_uri, LOCALS_BROKEN_A));
  send_msg(s.to_server, break_locals_a);
  json_free(break_locals_a);
  JsonValue* locals_broken_a_diag = read_msg(s.from_server); // discard
  if (locals_broken_a_diag) json_free(locals_broken_a_diag);

  JsonValue* locals_req_a = request(16, "textDocument/completion", position_params(locals_uri, 10, 10));
  send_msg(s.to_server, locals_req_a);
  json_free(locals_req_a);
  JsonValue* locals_resp_a = read_msg(s.from_server);
  check(locals_resp_a != NULL, "no response to completion (locals -- let-binding)");
  if (locals_resp_a) {
    JsonValue* result = json_obj_get(locals_resp_a, "result");
    JsonValue* items    = result ? json_obj_get(result, "items") : NULL;
    check(has_completion_label(items, "total"),
          "completion missing \"total\" -- a `let`-bound local isn't being offered");
    json_free(locals_resp_a);
  }

  JsonValue* break_locals_b = notification("textDocument/didChange", did_change_params(locals_uri, LOCALS_BROKEN_B));
  send_msg(s.to_server, break_locals_b);
  json_free(break_locals_b);
  JsonValue* locals_broken_b_diag = read_msg(s.from_server); // discard
  if (locals_broken_b_diag) json_free(locals_broken_b_diag);

  JsonValue* locals_req_b = request(17, "textDocument/completion", position_params(locals_uri, 10, 15));
  send_msg(s.to_server, locals_req_b);
  json_free(locals_req_b);
  JsonValue* locals_resp_b = read_msg(s.from_server);
  check(locals_resp_b != NULL, "no response to completion (locals -- param, scope boundary)");
  if (locals_resp_b) {
    JsonValue* result = json_obj_get(locals_resp_b, "result");
    JsonValue* items    = result ? json_obj_get(result, "items") : NULL;
    check(has_completion_label(items, "count"),
          "completion missing \"count\" -- the CURRENT function's own param isn't being offered");
    check(!has_completion_label(items, "co-unrelated"),
          "completion leaked \"co-unrelated\" -- an earlier, already-checked sibling function's "
          "own param shouldn't still be in scope");
    json_free(locals_resp_b);
  }
  free(locals_uri);

  // workspace/symbol. "pub-fn" lives in importer_pkg/libpkg/, which is never
  // didOpen'd here and gets no SymbolCache entry of its own from importer_pkg's
  // check -- that caches the importer, with libpkg's surface nested under its
  // `imports` (symbol_cache_update). Finding it can only mean the lazy scan
  // walked into that directory and checked it. Also confirms substring rather
  // than prefix matching: "elper" against "helper2".
  JsonValue* ws_params1 = json_new_object();
  json_obj_set(ws_params1, "query", json_new_string("pub-fn"));
  JsonValue* ws_req1 = request(13, "workspace/symbol", ws_params1);
  send_msg(s.to_server, ws_req1);
  json_free(ws_req1);
  JsonValue* ws_resp1 = read_msg(s.from_server);
  check(ws_resp1 != NULL, "no response to workspace/symbol (pub-fn)");
  if (ws_resp1) {
    JsonValue* result = json_obj_get(ws_resp1, "result");
    bool       found   = false;
    for (JsonArrayItem* it = (result && result->kind == Json_Array) ? result->array_first : NULL; it; it = it->next) {
      JsonValue* name = json_obj_get(it->value, "name");
      if (name && name->kind == Json_String && strcmp(name->string, "pub-fn") == 0) found = true;
    }
    check(found, "workspace/symbol didn't find \"pub-fn\" in the never-opened libpkg/ directory");
    json_free(ws_resp1);
  }

  JsonValue* ws_params2 = json_new_object();
  json_obj_set(ws_params2, "query", json_new_string("elper"));
  JsonValue* ws_req2 = request(14, "workspace/symbol", ws_params2);
  send_msg(s.to_server, ws_req2);
  json_free(ws_req2);
  JsonValue* ws_resp2 = read_msg(s.from_server);
  check(ws_resp2 != NULL, "no response to workspace/symbol (elper substring)");
  if (ws_resp2) {
    JsonValue* result = json_obj_get(ws_resp2, "result");
    bool       found   = false;
    for (JsonArrayItem* it = (result && result->kind == Json_Array) ? result->array_first : NULL; it; it = it->next) {
      JsonValue* name = json_obj_get(it->value, "name");
      if (name && name->kind == Json_String && strcmp(name->string, "helper2") == 0) found = true;
    }
    check(found, "workspace/symbol substring query \"elper\" didn't find \"helper2\"");
    json_free(ws_resp2);
  }

  // textDocument/documentSymbol and textDocument/formatting, both against
  // format_pkg. Its file is deliberately laid out badly (see FORMAT_CONTENT).
  char format_dir[4096]; snprintf(format_dir, sizeof(format_dir), "%s/format_pkg", fixtures);
  char* format_uri = uri_for(format_dir, "format_pkg.3b");

  JsonValue* open_format = notification("textDocument/didOpen", did_open_params(format_uri, FORMAT_CONTENT));
  send_msg(s.to_server, open_format);
  json_free(open_format);
  JsonValue* format_open_diag = read_msg(s.from_server); // discard -- expected clean
  if (format_open_diag) json_free(format_open_diag);

  // Every top-level declaration in the file and nothing else. Two different
  // ways to get this wrong are covered: "sibling-fn" is in the SAME package,
  // and so in the very same per-directory cache entry these results are read
  // from -- it is excluded only by the per-file filter; "helper2" is in
  // another package entirely, which workspace/symbol above does return.
  JsonValue* ds_req = request(18, "textDocument/documentSymbol", text_document_params(format_uri));
  send_msg(s.to_server, ds_req);
  json_free(ds_req);
  JsonValue* ds_resp = read_msg(s.from_server);
  check(ds_resp != NULL, "no response to documentSymbol");
  if (ds_resp) {
    JsonValue* result = json_obj_get(ds_resp, "result");
    check(result && result->kind == Json_Array, "documentSymbol result should be an array");

    size_t count = 0;
    for (JsonArrayItem* it = (result && result->kind == Json_Array) ? result->array_first : NULL; it; it = it->next) {
      count += 1;
    }
    check(count == 3, "expected 3 document symbols in format_pkg.3b, got %zu", count);
    check(find_symbol(result, "sibling-fn") == NULL,
          "documentSymbol returned \"sibling-fn\" -- a symbol from the package's OTHER file");
    check(find_symbol(result, "helper2") == NULL,
          "documentSymbol returned \"helper2\" -- a symbol from another package's file");

    // Kinds are LSP's SymbolKind, which numbers differently from
    // CompletionItemKind: Struct 23, Constant 14, Function 12.
    struct { const char* name; int kind; int line; } expected[] = {
      { "Point", 23, 1 }, { "LIMIT", 14, 2 }, { "area", 12, 3 },
    };
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i += 1) {
      JsonValue* sym = find_symbol(result, expected[i].name);
      check(sym != NULL, "documentSymbol missing \"%s\"", expected[i].name);
      if (!sym) continue;
      JsonValue* kind = json_obj_get(sym, "kind");
      check(kind && (int)kind->number == expected[i].kind,
            "\"%s\": expected SymbolKind %d, got %d", expected[i].name, expected[i].kind,
            kind ? (int)kind->number : -1);
      JsonValue* location = json_obj_get(sym, "location");
      JsonValue* loc_uri  = location ? json_obj_get(location, "uri") : NULL;
      check(loc_uri && loc_uri->kind == Json_String && strcmp(loc_uri->string, format_uri) == 0,
            "\"%s\": location points at the wrong uri", expected[i].name);
      JsonValue* range = location ? json_obj_get(location, "range") : NULL;
      JsonValue* start = range ? json_obj_get(range, "start") : NULL;
      JsonValue* line  = start ? json_obj_get(start, "line") : NULL;
      check(line && (int)line->number == expected[i].line,
            "\"%s\": expected line %d, got %d", expected[i].name, expected[i].line,
            line ? (int)line->number : -1);
    }
    json_free(ds_resp);
  }

  // Formatting the badly laid out buffer -> exactly one TextEdit, spanning the
  // whole document and carrying format.c's rendering verbatim.
  JsonValue* fmt_req = request(19, "textDocument/formatting", text_document_params(format_uri));
  send_msg(s.to_server, fmt_req);
  json_free(fmt_req);
  JsonValue* fmt_resp = read_msg(s.from_server);
  check(fmt_resp != NULL, "no response to formatting");
  if (fmt_resp) {
    JsonValue* result = json_obj_get(fmt_resp, "result");
    size_t     count   = 0;
    for (JsonArrayItem* it = (result && result->kind == Json_Array) ? result->array_first : NULL; it; it = it->next) {
      count += 1;
    }
    check(count == 1, "expected exactly 1 formatting TextEdit, got %zu", count);
    if (count == 1) {
      JsonValue* edit     = result->array_first->value;
      JsonValue* new_text = json_obj_get(edit, "newText");
      check(new_text && new_text->kind == Json_String && strcmp(new_text->string, FORMAT_EXPECTED) == 0,
            "formatting newText did not match `3b format`'s rendering; got:\n%s",
            (new_text && new_text->kind == Json_String) ? new_text->string : "(not a string)");
      // The edit must replace the WHOLE document: start (0,0), end one past
      // the last byte. FORMAT_CONTENT is 5 lines each ending in "\n", so the
      // position past its last byte is line 5, character 0.
      JsonValue* range     = json_obj_get(edit, "range");
      JsonValue* start     = range ? json_obj_get(range, "start") : NULL;
      JsonValue* end       = range ? json_obj_get(range, "end") : NULL;
      JsonValue* start_ln  = start ? json_obj_get(start, "line") : NULL;
      JsonValue* start_ch  = start ? json_obj_get(start, "character") : NULL;
      JsonValue* end_ln    = end ? json_obj_get(end, "line") : NULL;
      JsonValue* end_ch    = end ? json_obj_get(end, "character") : NULL;
      check(start_ln && (int)start_ln->number == 0 && start_ch && (int)start_ch->number == 0,
            "formatting edit should start at (0,0), got (%d,%d)",
            start_ln ? (int)start_ln->number : -1, start_ch ? (int)start_ch->number : -1);
      check(end_ln && (int)end_ln->number == 5 && end_ch && (int)end_ch->number == 0,
            "formatting edit should end at (5,0), got (%d,%d)",
            end_ln ? (int)end_ln->number : -1, end_ch ? (int)end_ch->number : -1);
    }
    json_free(fmt_resp);
  }

  // Formatting an ALREADY formatted buffer -> an empty edit list, not a
  // no-op edit. This is also the idempotence check on the wire: the server is
  // being handed back its own output from the request above, and must agree
  // there is nothing left to change. (test/format_test.c proves the same
  // property over the whole committed corpus; this pins it for the one path a
  // format-on-save client takes, where a drifting formatter would mark every
  // buffer dirty on every save.)
  JsonValue* change_formatted =
      notification("textDocument/didChange", did_change_params(format_uri, FORMAT_EXPECTED));
  send_msg(s.to_server, change_formatted);
  json_free(change_formatted);
  JsonValue* format_change_diag = read_msg(s.from_server); // discard -- expected clean
  if (format_change_diag) json_free(format_change_diag);

  JsonValue* fmt_req2 = request(20, "textDocument/formatting", text_document_params(format_uri));
  send_msg(s.to_server, fmt_req2);
  json_free(fmt_req2);
  JsonValue* fmt_resp2 = read_msg(s.from_server);
  check(fmt_resp2 != NULL, "no response to formatting an already-formatted buffer");
  if (fmt_resp2) {
    JsonValue* result = json_obj_get(fmt_resp2, "result");
    check(result && result->kind == Json_Array && result->array_first == NULL,
          "formatting an already-formatted buffer should produce no edits");
    json_free(fmt_resp2);
  }

  // A buffer that doesn't parse -> a null result, never a partial rewrite.
  // The unbalanced text never reaches disk, so this is purely the overlay the
  // didChange installed.
  JsonValue* change_unparseable =
      notification("textDocument/didChange", did_change_params(format_uri, FORMAT_BROKEN_CONTENT));
  send_msg(s.to_server, change_unparseable);
  json_free(change_unparseable);
  JsonValue* format_broken_diag = read_msg(s.from_server); // discard -- a parse error is expected
  if (format_broken_diag) json_free(format_broken_diag);

  JsonValue* fmt_req3 = request(21, "textDocument/formatting", text_document_params(format_uri));
  send_msg(s.to_server, fmt_req3);
  json_free(fmt_req3);
  JsonValue* fmt_resp3 = read_msg(s.from_server);
  check(fmt_resp3 != NULL, "no response to formatting an unparseable buffer");
  if (fmt_resp3) {
    JsonValue* result = json_obj_get(fmt_resp3, "result");
    check(result && result->kind == Json_Null,
          "formatting an unparseable buffer should return null, leaving the document alone");
    json_free(fmt_resp3);
  }

  // The same unparseable buffer is still installed, so documentSymbol here is
  // being asked about a file that currently has no symbols at all. It must
  // answer with the last state that CHECKED -- the three declarations above --
  // rather than blanking the outline. That is the whole reason it reads the
  // SymbolCache instead of compiling per request.
  JsonValue* ds_req2 = request(23, "textDocument/documentSymbol", text_document_params(format_uri));
  send_msg(s.to_server, ds_req2);
  json_free(ds_req2);
  JsonValue* ds_resp2 = read_msg(s.from_server);
  check(ds_resp2 != NULL, "no response to documentSymbol on a broken buffer");
  if (ds_resp2) {
    JsonValue* result = json_obj_get(ds_resp2, "result");
    size_t     count   = 0;
    for (JsonArrayItem* it = (result && result->kind == Json_Array) ? result->array_first : NULL; it; it = it->next) {
      count += 1;
    }
    check(count == 3, "documentSymbol on a broken buffer should keep the last good outline (3), got %zu", count);
    json_free(ds_resp2);
  }

  // Formatting a document the client never opened -> null. A whole-document
  // edit against text the editor isn't showing would be worse than no reply.
  char* unopened_uri = uri_for(format_dir, "never-opened.3b");
  JsonValue* fmt_req4 = request(22, "textDocument/formatting", text_document_params(unopened_uri));
  send_msg(s.to_server, fmt_req4);
  json_free(fmt_req4);
  JsonValue* fmt_resp4 = read_msg(s.from_server);
  check(fmt_resp4 != NULL, "no response to formatting an unopened document");
  if (fmt_resp4) {
    JsonValue* result = json_obj_get(fmt_resp4, "result");
    check(result && result->kind == Json_Null, "formatting an unopened document should return null");
    json_free(fmt_resp4);
  }
  free(unopened_uri);
  free(format_uri);

  // The markdown popup. Every hover above went to a client declaring no
  // hover.contentFormat, which is the plaintext fallback; a real editor
  // declares markdown, and gets the same content laid out as a heading, a
  // fenced `3b` code block, prose and a cited declaration site, separated by
  // horizontal rules. That needs its own server, since the format is settled
  // once at initialize.
  {
    Server md = spawn_server(server_path);

    JsonValue* formats = json_new_array();
    json_array_push(formats, json_new_string("markdown"));
    json_array_push(formats, json_new_string("plaintext"));
    JsonValue* hover_caps = json_new_object();
    json_obj_set(hover_caps, "contentFormat", formats);
    JsonValue* td_caps = json_new_object();
    json_obj_set(td_caps, "hover", hover_caps);
    JsonValue* client_caps = json_new_object();
    json_obj_set(client_caps, "textDocument", td_caps);
    JsonValue* md_init_params = json_new_object();
    json_obj_set(md_init_params, "rootUri", json_new_string(root_uri));
    json_obj_set(md_init_params, "capabilities", client_caps);

    JsonValue* md_init = request(1, "initialize", md_init_params);
    send_msg(md.to_server, md_init);
    json_free(md_init);
    JsonValue* md_init_resp = read_msg(md.from_server);
    check(md_init_resp != NULL, "no response to initialize (markdown client)");
    if (md_init_resp) json_free(md_init_resp);

    JsonValue* md_initd = notification("initialized", json_new_object());
    send_msg(md.to_server, md_initd);
    json_free(md_initd);

    char* md_uri = uri_for(hover_doc_dir, "hover_doc_pkg.3b");
    JsonValue* md_open = notification("textDocument/didOpen", did_open_params(md_uri, HOVER_DOC_CONTENT));
    send_msg(md.to_server, md_open);
    json_free(md_open);
    JsonValue* md_open_diag = read_msg(md.from_server); // discard -- expected clean
    if (md_open_diag) json_free(md_open_diag);

    JsonValue* md_req = request(2, "textDocument/hover", position_params(md_uri, 21, 8));
    send_msg(md.to_server, md_req);
    json_free(md_req);
    JsonValue* md_resp = read_msg(md.from_server);
    check(md_resp != NULL, "no response to hover (markdown client)");
    if (md_resp) {
      JsonValue*  result   = json_obj_get(md_resp, "result");
      JsonValue*  contents = result ? json_obj_get(result, "contents") : NULL;
      JsonValue*  kind      = contents ? json_obj_get(contents, "kind") : NULL;
      JsonValue*  value     = contents ? json_obj_get(contents, "value") : NULL;
      const char* expect    = "### function `documented`\n\n---\n"
                               "```3b\nfn documented [a i32 b i32] i32\n```\n\n---\n"
                               "Adds two numbers.\n\nAnd explains itself at length.\n\n---\n"
                               "*Defined in `hover_doc_pkg.3b:6`*";
      check(kind && kind->kind == Json_String && strcmp(kind->string, "markdown") == 0,
            "expected contents.kind \"markdown\" for a markdown-capable client, got %s",
            (kind && kind->kind == Json_String) ? kind->string : "(missing)");
      check(value && value->kind == Json_String && strcmp(value->string, expect) == 0,
            "markdown hover: expected \"%s\", got %s", expect,
            (value && value->kind == Json_String) ? value->string : "(null result)");
      json_free(md_resp);
    }
    free(md_uri);

    JsonValue* md_shutdown = request(3, "shutdown", NULL);
    send_msg(md.to_server, md_shutdown);
    json_free(md_shutdown);
    JsonValue* md_shutdown_resp = read_msg(md.from_server);
    if (md_shutdown_resp) json_free(md_shutdown_resp);
    JsonValue* md_exit = notification("exit", json_new_object());
    send_msg(md.to_server, md_exit);
    json_free(md_exit);
    waitpid(md.pid, NULL, 0);
    fclose(md.to_server);
    fclose(md.from_server);
  }

  // A body that is valid JSON but nested far past the parser's depth limit.
  // It fails to parse, which is a message the server ignores -- it must still
  // answer the next well-formed request.
  {
    char*  deep     = nested_json(500000);
    size_t deep_len = strlen(deep);
    char   header[64];
    snprintf(header, sizeof(header), "Content-Length: %zu\r\n\r\n", deep_len);
    send_raw(s.to_server, header, deep, deep_len);
    free(deep);

    JsonValue* ping_params = json_new_object();
    json_obj_set(ping_params, "query", json_new_string("zzz-no-such-symbol"));
    JsonValue* ping = request(90, "workspace/symbol", ping_params);
    send_msg(s.to_server, ping);
    json_free(ping);
    JsonValue* ping_resp = read_msg(s.from_server);
    check(ping_resp != NULL, "server stopped responding after a deeply nested message");
    if (ping_resp) {
      JsonValue* id = json_obj_get(ping_resp, "id");
      check(id && (int)id->number == 90, "expected the response to request 90 after the deep message");
      json_free(ping_resp);
    }
  }

  // A Content-Length no allocator can satisfy, on a server of its own since
  // the frame is unrecoverable and ends the stream. The server must exit
  // rather than die on a signal from writing through a failed allocation.
  // The body bytes matter: they ride in on the same read as the header, so a
  // server that allocated first would copy them through a NULL pointer.
  {
    Server      huge = spawn_server(server_path);
    const char* body = "{\"jsonrpc\":\"2.0\"}";
    send_raw(huge.to_server, "Content-Length: 99999999999999\r\n\r\n", body, strlen(body));
    fclose(huge.to_server);

    int   status = 0;
    pid_t waited = waitpid(huge.pid, &status, 0);
    check(waited == huge.pid, "waitpid failed for the oversized-header server");
    check(WIFEXITED(status), "oversized Content-Length killed the server, status 0x%x", status);
    fclose(huge.from_server);
  }

  // shutdown -> a null-result response
  JsonValue* shutdown_req = request(2, "shutdown", NULL);
  send_msg(s.to_server, shutdown_req);
  json_free(shutdown_req);
  JsonValue* shutdown_resp = read_msg(s.from_server);
  check(shutdown_resp != NULL, "no response to shutdown");
  if (shutdown_resp) {
    JsonValue* result = json_obj_get(shutdown_resp, "result");
    check(result != NULL && result->kind == Json_Null, "expected shutdown's result to be null");
    json_free(shutdown_resp);
  }

  JsonValue* exit_notif = notification("exit", json_new_object());
  send_msg(s.to_server, exit_notif);
  json_free(exit_notif);

  int   status = 0;
  pid_t waited = waitpid(s.pid, &status, 0);
  check(waited == s.pid, "waitpid failed");
  check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
        "expected clean exit(0) after shutdown+exit, got status 0x%x", status);

  fclose(s.to_server);
  fclose(s.from_server);
  free(broken_uri);
  free(clean_uri);

  char rm_cmd[4200];
  snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf '%s'", fixtures); // mkdtemp'd, same pattern as build.c's build_clean
  if (system(rm_cmd) != 0) fprintf(stderr, "lsp_test: warning: failed to clean up %s\n", fixtures);
  free(fixtures);

  if (g_failures == 0) {
    printf("lsp_test: all checks passed\n");
    return 0;
  }
  printf("lsp_test: %d check(s) failed\n", g_failures);
  return 1;
}
