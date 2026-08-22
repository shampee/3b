// `3b-lsp`, a Language Server over lib3b: publishes diagnostics on open/
// change/save, and serves hover, goto-definition, completion,
// workspace/symbol, documentSymbol and whole-document formatting. See json.h
// for the wire format and lib3b.h for the compiler entry points this wraps.
//
// Plain malloc/free throughout, never ctx_init/ctx_perm. Each lib3b entry
// point owns a Context for the duration of its own call and hands back
// malloc'd results, and ctx_free clears tls_ctx rather than restoring
// whatever was active before it. A caller holding its own arena across a
// lib3b call would find it silently detached, so this file never
// establishes a Context at all.
#include "json.h"
#include "lib3b.h"
#include <ctype.h>
#include <dirent.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static FILE* g_out;
static bool  g_got_shutdown = false;
static bool  g_hover_markdown = false; // client declared markdown support -- see handle_initialize

////////////////////////////////
//~ Wire framing

// Ceiling on one message body. The largest real ones are whole-file didChange
// bodies, orders of magnitude under this. The header is the only place a peer
// picks an allocation size, so it is bounded before reaching malloc rather
// than trusted the way the rest of this tree trusts its own allocations.
#define MAX_CONTENT_LENGTH (16 * 1024 * 1024)

// Reads one `Content-Length: N\r\n\r\n<N bytes>` message from `in`. Returns a
// malloc'd buffer holding the body plus a NUL, `*out_len` bytes not counting
// that NUL, or NULL on EOF or a malformed frame.
static char*
read_message(FILE* in, size_t* out_len) {
  long    content_length = -1;
  char*   line     = NULL;
  size_t  line_cap = 0;
  for (;;) {
    ssize_t n = getline(&line, &line_cap, in);
    if (n < 0) { free(line); return NULL; } // EOF
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) { line[n - 1] = 0; n -= 1; }
    if (n == 0) break; // blank line ends the headers
    if (strncmp(line, "Content-Length:", 15) == 0) {
      content_length = strtol(line + 15, NULL, 10);
    }
    // any other header (Content-Type, ...) is read and ignored
  }
  free(line);
  // A length outside the range is a frame that cannot be resynced -- the body
  // boundary is unknown -- so it ends the stream rather than being skipped.
  if (content_length < 0 || content_length > MAX_CONTENT_LENGTH) return NULL;

  char* body = malloc((size_t)content_length + 1);
  if (!body) return NULL;
  size_t got = fread(body, 1, (size_t)content_length, in);
  if (got != (size_t)content_length) { free(body); return NULL; }
  body[content_length] = 0;
  if (out_len) *out_len = (size_t)content_length;
  return body;
}

static void
send_message(FILE* out, JsonValue* msg) {
  JsonBuf buf = {0};
  json_write(&buf, msg);
  fprintf(out, "Content-Length: %zu\r\n\r\n", buf.len);
  fwrite(buf.data, 1, buf.len, out);
  fflush(out);
  json_buf_free(&buf);
}

////////////////////////////////
//~ file:// URI <-> filesystem path

static char*
uri_to_path(const char* uri) {
  const char* prefix = "file://";
  size_t      plen   = strlen(prefix);
  if (strncmp(uri, prefix, plen) != 0) return NULL; // untitled: and friends -- caller no-ops
  const char* rest = uri + plen;
  size_t      n    = strlen(rest);
  char*       out  = malloc(n + 1);
  size_t      w    = 0;
  for (size_t i = 0; i < n; i += 1) {
    if (rest[i] == '%' && i + 2 < n && isxdigit((unsigned char)rest[i + 1]) && isxdigit((unsigned char)rest[i + 2])) {
      char hex[3] = {rest[i + 1], rest[i + 2], 0};
      out[w] = (char)strtol(hex, NULL, 16);
      w += 1;
      i += 2;
    } else {
      out[w] = rest[i];
      w += 1;
    }
  }
  out[w] = 0;
  return out;
}

static char*
path_to_uri(const char* path) {
  size_t n   = strlen(path);
  char*  out = malloc(7 + n * 3 + 1); // "file://" + worst case of every byte escaped
  memcpy(out, "file://", 7);
  char* w = out + 7;
  for (size_t i = 0; i < n; i += 1) {
    unsigned char c    = (unsigned char)path[i];
    bool          safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
                       || c == '/' || c == '-' || c == '_' || c == '.' || c == '~';
    if (safe) { *w = (char)c; w += 1; }
    else      { sprintf(w, "%%%02X", c); w += 3; }
  }
  *w = 0;
  return out;
}

// 3b's package model is one directory per package (compiler.h), so a file's
// containing directory is the package to check. No upward search is needed,
// unlike main.c's find_project_root for the CLI.
static char*
path_dirname(const char* path) {
  char* tmp    = strdup(path);
  char* d      = dirname(tmp); // may alias tmp or return static storage
  char* result = strdup(d);
  free(tmp);
  return result;
}

////////////////////////////////
//~ Open documents: path -> current in-memory buffer text, kept between
// didOpen and didClose. Feeds SourceOverlay (3b.h) so a check or query sees
// unsaved edits instead of stale disk content. Keyed by filesystem path, the
// same convention PublishedForDir uses.

typedef struct OpenDoc {
  char*            path; // owned, absolute
  char*            text; // owned
  struct OpenDoc* next;
} OpenDoc;

static OpenDoc* g_open_docs = NULL;

static void
open_doc_set(const char* path, const char* text) {
  for (OpenDoc* d = g_open_docs; d; d = d->next) {
    if (strcmp(d->path, path) == 0) {
      free(d->text);
      d->text = strdup(text);
      return;
    }
  }
  OpenDoc* d  = calloc(1, sizeof(OpenDoc));
  d->path      = strdup(path);
  d->text      = strdup(text);
  d->next      = g_open_docs;
  g_open_docs = d;
}

static void
open_doc_remove(const char* path) {
  OpenDoc** link = &g_open_docs;
  while (*link) {
    if (strcmp((*link)->path, path) == 0) {
      OpenDoc* dead = *link;
      *link          = dead->next;
      free(dead->path);
      free(dead->text);
      free(dead);
      return;
    }
    link = &(*link)->next;
  }
}

// Every open document whose containing directory is `dir_path`. Usually just
// the file that triggered the check, but two unsaved files of one package is a
// real case, so this is not narrowed to a single path.
//
// The returned entries alias OpenDoc's strings rather than copying them, so
// they are valid only until the next open_doc_set/remove -- long enough for the
// one lib3b call they feed. The caller frees the array, not the strings.
static SourceOverlay*
build_overlays_for_dir(const char* dir_path, u64* out_count) {
  size_t total = 0;
  for (OpenDoc* d = g_open_docs; d; d = d->next) total += 1;
  SourceOverlay* overlays = total ? malloc(total * sizeof(SourceOverlay)) : NULL;
  u64 n = 0;
  for (OpenDoc* d = g_open_docs; d; d = d->next) {
    char* dir = path_dirname(d->path);
    if (strcmp(dir, dir_path) == 0) {
      overlays[n].path    = str8_cstring(d->path);
      overlays[n].content = str8_cstring(d->text);
      n += 1;
    }
    free(dir);
  }
  *out_count = n;
  return overlays;
}

////////////////////////////////
//~ JSON-RPC envelope helpers

static JsonValue*
clone_id(JsonValue* id_v) {
  if (!id_v) return json_new_null();
  if (id_v->kind == Json_Number) return json_new_number(id_v->number);
  if (id_v->kind == Json_String) return json_new_string(id_v->string);
  return json_new_null();
}

static void
send_response(JsonValue* id_v, JsonValue* result) {
  JsonValue* resp = json_new_object();
  json_obj_set(resp, "jsonrpc", json_new_string("2.0"));
  json_obj_set(resp, "id", clone_id(id_v));
  json_obj_set(resp, "result", result);
  send_message(g_out, resp);
  json_free(resp);
}

static void
send_error_response(JsonValue* id_v, int code, const char* message) {
  JsonValue* err = json_new_object();
  json_obj_set(err, "code", json_new_number(code));
  json_obj_set(err, "message", json_new_string(message));
  JsonValue* resp = json_new_object();
  json_obj_set(resp, "jsonrpc", json_new_string("2.0"));
  json_obj_set(resp, "id", clone_id(id_v));
  json_obj_set(resp, "error", err);
  send_message(g_out, resp);
  json_free(resp);
}

static void
send_notification(const char* method, JsonValue* params) {
  JsonValue* notif = json_new_object();
  json_obj_set(notif, "jsonrpc", json_new_string("2.0"));
  json_obj_set(notif, "method", json_new_string(method));
  json_obj_set(notif, "params", params);
  send_message(g_out, notif);
  json_free(notif);
}

////////////////////////////////
//~ Diagnostics: check -> group by file -> publish, with stale-URI clearing

// Diagnostic.line/col are 1-indexed, matching diag.c's path:line:col output;
// LSP Position is 0-indexed.
static JsonValue*
make_lsp_diagnostic(Diagnostic* d) {
  int line = (d->line >= 1) ? (int)d->line - 1 : 0;
  int col  = (d->col >= 1) ? (int)d->col - 1 : 0;

  JsonValue* start = json_new_object();
  json_obj_set(start, "line", json_new_number(line));
  json_obj_set(start, "character", json_new_number(col));
  JsonValue* end = json_new_object();
  json_obj_set(end, "line", json_new_number(line));
  // Diagnostic tracks no end column, so span one character rather than zero,
  // which would render invisibly.
  json_obj_set(end, "character", json_new_number(col + 1));
  JsonValue* range = json_new_object();
  json_obj_set(range, "start", start);
  json_obj_set(range, "end", end);

  JsonValue* diag = json_new_object();
  json_obj_set(diag, "range", range);
  json_obj_set(diag, "severity", json_new_number(1)); // Error; 3b has no warnings
  json_obj_set(diag, "source", json_new_string("3b"));
  json_obj_set(diag, "message", json_new_string((const char*)d->message.str));
  return diag;
}

typedef struct FileGroup {
  char*              file_path; // owned
  Diagnostic**       items;     // borrowed pointers into the Lib3bCheckResult
  size_t             count, cap;
  struct FileGroup* next;
} FileGroup;

static FileGroup*
group_by_file(Lib3bCheckResult* r) {
  FileGroup* head = NULL;
  for (u64 i = 0; i < r->diagnostic_count; i += 1) {
    Diagnostic* d  = &r->diagnostics[i];
    const char* fp = (const char*)d->file_path.str;
    FileGroup*  g  = head;
    while (g && strcmp(g->file_path, fp) != 0) g = g->next;
    if (!g) {
      g            = calloc(1, sizeof(FileGroup));
      g->file_path = strdup(fp);
      g->next      = head;
      head         = g;
    }
    if (g->count == g->cap) {
      g->cap   = g->cap ? g->cap * 2 : 4;
      g->items = realloc(g->items, g->cap * sizeof(Diagnostic*));
    }
    g->items[g->count] = d;
    g->count += 1;
  }
  return head;
}

static void
free_groups(FileGroup* head) {
  while (head) {
    FileGroup* next = head->next;
    free(head->file_path);
    free(head->items);
    free(head);
    head = next;
  }
}

static void
send_publish(const char* uri, FileGroup* g) {
  JsonValue* arr = json_new_array();
  if (g) for (size_t i = 0; i < g->count; i += 1) json_array_push(arr, make_lsp_diagnostic(g->items[i]));
  JsonValue* params = json_new_object();
  json_obj_set(params, "uri", json_new_string(uri));
  json_obj_set(params, "diagnostics", arr);
  send_notification("textDocument/publishDiagnostics", params);
}

// One entry per package directory ever checked, rather than one global set:
// diagnostics are diffed and cleared only against the previous check of the
// same directory. Checking package C must not clear diagnostics still standing
// from an unrelated check of package A, which a single flat "last published"
// set would do. Tracked by path; URIs are built only when a notification is
// actually sent.
typedef struct PublishedForDir {
  char*                    dir_path; // owned
  char**                   paths;    // owned array of owned strings
  size_t                   count;
  struct PublishedForDir* next;
} PublishedForDir;

static PublishedForDir* g_published = NULL;

static PublishedForDir*
published_find_or_create(const char* dir_path) {
  for (PublishedForDir* e = g_published; e; e = e->next) {
    if (strcmp(e->dir_path, dir_path) == 0) return e;
  }
  PublishedForDir* e = calloc(1, sizeof(PublishedForDir));
  e->dir_path = strdup(dir_path);
  e->next     = g_published;
  g_published = e;
  return e;
}

////////////////////////////////
//~ Completion's symbol cache: the last successful check of each package
// directory, kept warm by check_and_publish below. Completion prefers a fresh
// tolerant recompile of the live buffer (lib3b_completion_context) and falls
// back to this cache when even that fails to parse, so a package always has
// some symbol list to offer mid-edit. One entry per directory, for the same
// reason as PublishedForDir: a failed check of package C must not disturb
// package A's still-good symbols.
typedef struct CachedSymbol {
  char*                 name;      // owned
  Lib3bSymbolKind        kind;
  char*                 file_path; // owned; for workspace/symbol, completion ignores it
  u32                    line, col; // 1-indexed
  char*                 detail;    // owned, may be NULL -- the declaration header, and the comment
  char*                 doc;       // owned, may be NULL -- above it. See Lib3bSymbol in lib3b.h
  struct CachedSymbol* next;
} CachedSymbol;

// One direct import's cached public surface. Transitive imports are not
// followed; see Lib3bImportedPackage in lib3b.h.
typedef struct CachedImport {
  char*                 name;    // owned -- the import's own package name
  CachedSymbol*         symbols; // owned list -- its PUBLIC top-level names only
  struct CachedImport* next;
} CachedImport;

typedef struct SymbolCache {
  char*                dir_path; // owned
  CachedSymbol*        symbols;  // owned; replaced wholesale on every successful
  CachedImport*        imports;  // check, never patched incrementally
  struct SymbolCache* next;
} SymbolCache;

static SymbolCache* g_symbol_cache = NULL;

static void
symbol_cache_free_list(CachedSymbol* head) {
  while (head) {
    CachedSymbol* next = head->next;
    free(head->name);
    free(head->file_path);
    free(head->detail);
    free(head->doc);
    free(head);
    head = next;
  }
}

static void
import_cache_free_list(CachedImport* head) {
  while (head) {
    CachedImport* next = head->next;
    free(head->name);
    symbol_cache_free_list(head->symbols);
    free(head);
    head = next;
  }
}

static CachedSymbol*
build_cached_symbol_list(Lib3bSymbol* symbols, u64 symbol_count) {
  CachedSymbol* list = NULL;
  for (u64 i = 0; i < symbol_count; i += 1) {
    CachedSymbol* node = calloc(1, sizeof(CachedSymbol));
    node->name      = strdup(symbols[i].name);
    node->kind      = symbols[i].kind;
    node->file_path = symbols[i].file_path ? strdup(symbols[i].file_path) : NULL;
    node->line       = symbols[i].line;
    node->col        = symbols[i].col;
    node->detail    = symbols[i].detail ? strdup(symbols[i].detail) : NULL;
    node->doc       = symbols[i].doc ? strdup(symbols[i].doc) : NULL;
    node->next       = list;
    list             = node;
  }
  return list;
}

// Replaces dir_path's cached symbol and import lists with a check result's
// arrays. Deep-copied: the caller frees the Lib3bCheckResult right after.
static void
symbol_cache_update(const char* dir_path, Lib3bSymbol* symbols, u64 symbol_count,
                     Lib3bImportedPackage* lib_imports, u64 import_count) {
  SymbolCache* entry = NULL;
  for (SymbolCache* e = g_symbol_cache; e; e = e->next) {
    if (strcmp(e->dir_path, dir_path) == 0) { entry = e; break; }
  }
  if (!entry) {
    entry           = calloc(1, sizeof(SymbolCache));
    entry->dir_path = strdup(dir_path);
    entry->next     = g_symbol_cache;
    g_symbol_cache  = entry;
  }
  symbol_cache_free_list(entry->symbols);
  entry->symbols = build_cached_symbol_list(symbols, symbol_count);

  import_cache_free_list(entry->imports);
  entry->imports = NULL;
  for (u64 i = 0; i < import_count; i += 1) {
    CachedImport* node = calloc(1, sizeof(CachedImport));
    node->name          = strdup(lib_imports[i].name);
    node->symbols        = build_cached_symbol_list(lib_imports[i].public_symbols, lib_imports[i].public_symbol_count);
    node->next           = entry->imports;
    entry->imports       = node;
  }
}

static CachedSymbol*
symbol_cache_lookup(const char* dir_path) {
  for (SymbolCache* e = g_symbol_cache; e; e = e->next) {
    if (strcmp(e->dir_path, dir_path) == 0) return e->symbols;
  }
  return NULL;
}

// Every direct import cached for dir_path. Serves both import names as
// unqualified candidates (typing "g" suggests "gl") and, through each entry's
// `symbols`, `pkgname/member` completion.
static CachedImport*
symbol_cache_lookup_imports(const char* dir_path) {
  for (SymbolCache* e = g_symbol_cache; e; e = e->next) {
    if (strcmp(e->dir_path, dir_path) == 0) return e->imports;
  }
  return NULL;
}

////////////////////////////////
//~ Workspace discovery, for workspace/symbol. The root is captured from
// initialize's params; the scan itself runs lazily on the first
// workspace/symbol request. See handle_workspace_symbol.

static char* g_workspace_root = NULL;

// Whether `dir_path` directly contains a real (non-`*.cfg.3b`) `.3b` file.
// Mirrors main.c's dir_has_3b_source; small helpers are duplicated rather than
// shared across the CLI/LSP boundary.
static bool
dir_has_3b_source(const char* dir_path) {
  DIR* dir = opendir(dir_path);
  if (!dir) return false;
  bool           found = false;
  struct dirent* entry;
  while ((entry = readdir(dir)) != NULL) {
    size_t len = strlen(entry->d_name);
    if (len > 3 && strcmp(entry->d_name + len - 3, ".3b") == 0
        && !(len > 7 && strcmp(entry->d_name + len - 7, ".cfg.3b") == 0)) {
      found = true;
      break;
    }
  }
  closedir(dir);
  return found;
}

// Recursively collects every directory at or under `dir` that directly
// contains `.3b` source: one entry per package. Skips dot-directories and
// "output", spelled as a literal here since this file does not include
// compiler.h, which owns OUTPUT_DIR.
static void
discover_packages(const char* dir, char*** out_dirs, size_t* out_count, size_t* out_cap) {
  if (dir_has_3b_source(dir)) {
    if (*out_count == *out_cap) {
      *out_cap  = *out_cap ? *out_cap * 2 : 16;
      *out_dirs = realloc(*out_dirs, *out_cap * sizeof(char*));
    }
    (*out_dirs)[*out_count] = strdup(dir);
    *out_count += 1;
  }

  DIR* d = opendir(dir);
  if (!d) return;
  struct dirent* entry;
  while ((entry = readdir(d)) != NULL) {
    if (entry->d_name[0] == '.') continue; // hidden dirs, "." and ".."
    if (strcmp(entry->d_name, "output") == 0) continue;
    char path[4096];
    snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name);
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
      discover_packages(path, out_dirs, out_count, out_cap);
    }
  }
  closedir(d);
}

// `opened_path` is the file that triggered this check. It always gets a
// publish, even an empty one: silence is spec-legal for a clean file, but an
// explicit empty array is more useful and far easier to test against.
static void
check_and_publish(const char* dir_path, const char* opened_path) {
  u64            overlay_count;
  SourceOverlay* overlays = build_overlays_for_dir(dir_path, &overlay_count);
  Lib3bCheckResult r = lib3b_check_package_with_overlays(dir_path, overlays, overlay_count);
  free(overlays);
  FileGroup* groups = group_by_file(&r);

  bool opened_seen = false;
  size_t count = 0;
  for (FileGroup* g = groups; g; g = g->next) {
    count += 1;
    if (strcmp(g->file_path, opened_path) == 0) opened_seen = true;
  }
  if (!opened_seen) count += 1;

  char** paths = malloc(count * sizeof(char*));
  size_t i = 0;
  for (FileGroup* g = groups; g; g = g->next) { paths[i] = strdup(g->file_path); i += 1; }
  if (!opened_seen) { paths[i] = strdup(opened_path); i += 1; }

  PublishedForDir* entry = published_find_or_create(dir_path);

  // Clear anything that dropped out since the last check of THIS dir.
  for (size_t k = 0; k < entry->count; k += 1) {
    bool still_present = false;
    for (size_t j = 0; j < count; j += 1) {
      if (strcmp(entry->paths[k], paths[j]) == 0) { still_present = true; break; }
    }
    if (!still_present) {
      char* u = path_to_uri(entry->paths[k]);
      send_publish(u, NULL);
      free(u);
    }
    free(entry->paths[k]);
  }
  free(entry->paths);

  // Publish the current set, including files whose diagnostics are unchanged.
  // publishDiagnostics fully replaces the client's set for a URI, so republishing
  // is redundant rather than wrong, and cheaper than diffing contents.
  for (size_t k = 0; k < count; k += 1) {
    FileGroup* g = groups;
    while (g && strcmp(g->file_path, paths[k]) != 0) g = g->next;
    char* u = path_to_uri(paths[k]);
    send_publish(u, g);
    free(u);
  }

  entry->paths = paths;
  entry->count = count;

  if (r.ok) symbol_cache_update(dir_path, r.symbols, r.symbol_count, r.imports, r.import_count);

  free_groups(groups);
  lib3b_free_result(&r);
}

static void
handle_close(const char* uri) {
  char* path = uri_to_path(uri);
  if (!path) return;
  open_doc_remove(path);
  for (PublishedForDir* e = g_published; e; e = e->next) {
    for (size_t i = 0; i < e->count; i += 1) {
      if (strcmp(e->paths[i], path) != 0) continue;
      send_publish(uri, NULL);
      free(e->paths[i]);
      e->paths[i] = e->paths[e->count - 1];
      e->count -= 1;
      free(path);
      return;
    }
  }
  free(path);
}

////////////////////////////////
//~ Method dispatch

// Shared by didOpen/didSave/didChange: resolve uri to path and dir, re-check.
static void
handle_check_notification(JsonValue* params) {
  JsonValue* td    = json_obj_get(params, "textDocument");
  JsonValue* uri_v = td ? json_obj_get(td, "uri") : NULL;
  if (!uri_v || uri_v->kind != Json_String) return;
  char* path = uri_to_path(uri_v->string);
  if (!path) return;
  char* dir = path_dirname(path);
  check_and_publish(dir, path);
  free(dir);
  free(path);
}

static void
track_open_doc_text(JsonValue* uri_v, const char* text) {
  if (!uri_v || uri_v->kind != Json_String || !text) return;
  char* path = uri_to_path(uri_v->string);
  if (!path) return;
  open_doc_set(path, text);
  free(path);
}

static void
handle_did_open(JsonValue* params) {
  JsonValue* td     = json_obj_get(params, "textDocument");
  JsonValue* uri_v   = td ? json_obj_get(td, "uri") : NULL;
  JsonValue* text_v = td ? json_obj_get(td, "text") : NULL;
  if (text_v && text_v->kind == Json_String) track_open_doc_text(uri_v, text_v->string);
  handle_check_notification(params);
}

// Full-document sync only (`change: 1`, set in handle_initialize): one
// contentChanges entry whose `text` replaces the whole document. No
// incremental range patching.
static void
handle_did_change(JsonValue* params) {
  JsonValue* td      = json_obj_get(params, "textDocument");
  JsonValue* uri_v    = td ? json_obj_get(td, "uri") : NULL;
  JsonValue* changes = json_obj_get(params, "contentChanges");
  JsonValue* first    = (changes && changes->kind == Json_Array && changes->array_first)
                          ? changes->array_first->value : NULL;
  JsonValue* text_v = first ? json_obj_get(first, "text") : NULL;
  if (text_v && text_v->kind == Json_String) track_open_doc_text(uri_v, text_v->string);
  handle_check_notification(params);
}

// True when the client listed "markdown" in
// capabilities.textDocument.hover.contentFormat. A hover response must use a
// format the client actually renders: sending markdown to a plaintext-only
// client shows it the backticks and hashes literally, so the popup falls back
// to an unmarked-up layout (render_hover below) rather than assuming.
static bool
client_supports_markdown_hover(JsonValue* params) {
  JsonValue* caps  = params ? json_obj_get(params, "capabilities") : NULL;
  JsonValue* td    = caps ? json_obj_get(caps, "textDocument") : NULL;
  JsonValue* hover = td ? json_obj_get(td, "hover") : NULL;
  JsonValue* fmts  = hover ? json_obj_get(hover, "contentFormat") : NULL;
  if (!fmts || fmts->kind != Json_Array) return false;
  for (JsonArrayItem* it = fmts->array_first; it; it = it->next) {
    if (it->value && it->value->kind == Json_String && strcmp(it->value->string, "markdown") == 0) return true;
  }
  return false;
}

// Captures the workspace root from workspaceFolders, falling back to the
// deprecated rootUri. Only workspace/symbol's directory walk reads it.
static void
handle_initialize(JsonValue* id_v, JsonValue* params) {
  g_hover_markdown        = client_supports_markdown_hover(params);
  JsonValue* folders      = json_obj_get(params, "workspaceFolders");
  JsonValue* first_folder = (folders && folders->kind == Json_Array && folders->array_first)
                              ? folders->array_first->value : NULL;
  JsonValue* folder_uri = first_folder ? json_obj_get(first_folder, "uri") : NULL;
  JsonValue* root_uri_v = json_obj_get(params, "rootUri");
  JsonValue* uri_v      = (folder_uri && folder_uri->kind == Json_String) ? folder_uri
                            : (root_uri_v && root_uri_v->kind == Json_String) ? root_uri_v
                            : NULL;
  if (uri_v) g_workspace_root = uri_to_path(uri_v->string);

  JsonValue* sync = json_new_object();
  json_obj_set(sync, "openClose", json_new_bool(true));
  json_obj_set(sync, "change", json_new_number(1)); // TextDocumentSyncKind.Full
  JsonValue* save = json_new_object();
  json_obj_set(save, "includeText", json_new_bool(false));
  json_obj_set(sync, "save", save);

  JsonValue* caps = json_new_object();
  json_obj_set(caps, "textDocumentSync", sync);
  json_obj_set(caps, "hoverProvider", json_new_bool(true));
  json_obj_set(caps, "definitionProvider", json_new_bool(true));
  json_obj_set(caps, "completionProvider", json_new_object()); // no trigger characters:
                                                                 // plain identifier typing
  json_obj_set(caps, "workspaceSymbolProvider", json_new_bool(true));
  json_obj_set(caps, "documentSymbolProvider", json_new_bool(true));
  json_obj_set(caps, "documentFormattingProvider", json_new_bool(true));
  // No documentRangeFormattingProvider: format.c renders a whole parsed
  // program, and a range in an s-expression language is usually a fragment of
  // one form, which has no standalone parse. A client that wants
  // format-on-save gets it from the whole-document provider above; one that
  // asks for a range gets "method not found" rather than a subtly wrong edit.

  JsonValue* info = json_new_object();
  json_obj_set(info, "name", json_new_string("3b-lsp"));
  json_obj_set(info, "version", json_new_string("0.1.0"));

  JsonValue* result = json_new_object();
  json_obj_set(result, "capabilities", caps);
  json_obj_set(result, "serverInfo", info);
  send_response(id_v, result);
}

////////////////////////////////
//~ hover / definition -- shared request-parameter resolution

// Extracts textDocument.uri and position from a hover or definition request,
// resolves them to an absolute path and dir, and builds that dir's overlay set.
// Returns false with nothing populated on malformed params; the caller then
// responds with null. `*out_line`/`*out_col` come back 1-indexed, matching
// lib3b_hover/lib3b_definition, where LSP's Position is 0-indexed.
static bool
resolve_query_request(JsonValue* params, char** out_path, char** out_dir,
                       SourceOverlay** out_overlays, u64* out_overlay_count,
                       u32* out_line, u32* out_col) {
  JsonValue* td      = json_obj_get(params, "textDocument");
  JsonValue* uri_v    = td ? json_obj_get(td, "uri") : NULL;
  JsonValue* pos      = json_obj_get(params, "position");
  JsonValue* line_v = pos ? json_obj_get(pos, "line") : NULL;
  JsonValue* char_v = pos ? json_obj_get(pos, "character") : NULL;
  if (!uri_v || uri_v->kind != Json_String || !line_v || !char_v) return false;
  char* path = uri_to_path(uri_v->string);
  if (!path) return false;
  *out_path          = path;
  *out_dir            = path_dirname(path);
  *out_overlays       = build_overlays_for_dir(*out_dir, out_overlay_count);
  *out_line           = (u32)line_v->number + 1;
  *out_col            = (u32)char_v->number + 1;
  return true;
}

////////////////////////////////
//~ Hover popup rendering -- laid out after clangd's: a "### kind `name`"
// heading, the declaration in a fenced code block, the doc comment written
// above it as prose, and last the file and line it was declared on, each
// section split by a horizontal rule. Every part below the code block is
// conditional, so an expression with nothing but a resolved type still renders
// as just that type.

// A grow-on-append char buffer, only ever built up and handed to
// json_new_string. Small enough that the alternative -- two passes to measure
// then fill -- would cost more in duplicated format strings than it saves.
typedef struct StrBuf {
  char*  data;
  size_t len, cap;
} StrBuf;

static void
sb_append(StrBuf* sb, const char* s) {
  size_t n = strlen(s);
  if (sb->len + n + 1 > sb->cap) {
    sb->cap  = (sb->len + n + 1) * 2;
    sb->data = realloc(sb->data, sb->cap);
  }
  memcpy(sb->data + sb->len, s, n + 1);
  sb->len += n;
}

static const char*
hover_kind_label(Lib3bHoverKind kind) {
  switch (kind) {
    case Lib3bHover_Function: return "function";
    case Lib3bHover_Struct:   return "struct";
    case Lib3bHover_Enum:     return "enum";
    case Lib3bHover_Builtin:  return "builtin";
    case Lib3bHover_Binding:  return "variable";
    case Lib3bHover_Field:    return "field";
    default:                  return NULL; // Lib3bHover_Expression -- no name to head the popup with
  }
}

// Everything after the last '/', so the popup cites "scene.3b:42" rather than
// an absolute path that would wrap the whole popup on a deep directory.
static const char*
path_basename_ptr(const char* path) {
  const char* slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

// The hover popup as one string, marked up when `markdown`, plain when not.
// Caller frees.
static char*
render_hover(const Lib3bHoverResult* hr, bool markdown) {
  StrBuf      sb    = {0};
  const char* label = hover_kind_label(hr->kind);
  const char* rule  = markdown ? "\n\n---\n" : "\n\n";

  if (label && hr->name) {
    if (markdown) { sb_append(&sb, "### "); sb_append(&sb, label); sb_append(&sb, " `");
                     sb_append(&sb, hr->name); sb_append(&sb, "`"); }
    else          { sb_append(&sb, label); sb_append(&sb, " "); sb_append(&sb, hr->name); }
    sb_append(&sb, rule);
  }

  if (markdown) sb_append(&sb, "```3b\n");
  sb_append(&sb, hr->type_text);
  if (markdown) sb_append(&sb, "\n```");

  if (hr->doc) {
    sb_append(&sb, rule);
    sb_append(&sb, hr->doc);
  }

  if (hr->decl_file_path) {
    char cite[512];
    snprintf(cite, sizeof(cite), markdown ? "*Defined in `%s:%u`*" : "Defined in %s:%u",
             path_basename_ptr(hr->decl_file_path), hr->decl_line);
    sb_append(&sb, rule);
    sb_append(&sb, cite);
  }
  return sb.data ? sb.data : strdup(""); // nothing appended at all -- an empty type display on a
                                          // nameless expression, the one input that reaches here
}

static void
handle_hover(JsonValue* id_v, JsonValue* params) {
  char *path, *dir;
  SourceOverlay* overlays;
  u64 overlay_count;
  u32 line, col;
  if (!resolve_query_request(params, &path, &dir, &overlays, &overlay_count, &line, &col)) {
    send_response(id_v, json_new_null());
    return;
  }
  Lib3bHoverResult hr = lib3b_hover(dir, path, line, col, overlays, overlay_count);
  free(overlays);
  free(dir);
  free(path);
  if (!hr.found) {
    send_response(id_v, json_new_null());
  } else {
    char*      value    = render_hover(&hr, g_hover_markdown);
    JsonValue* contents = json_new_object();
    json_obj_set(contents, "kind", json_new_string(g_hover_markdown ? "markdown" : "plaintext"));
    json_obj_set(contents, "value", json_new_string(value));
    free(value);
    JsonValue* result = json_new_object();
    json_obj_set(result, "contents", contents);
    send_response(id_v, result);
  }
  lib3b_free_hover(&hr);
}

static void
handle_definition(JsonValue* id_v, JsonValue* params) {
  char *path, *dir;
  SourceOverlay* overlays;
  u64 overlay_count;
  u32 line, col;
  if (!resolve_query_request(params, &path, &dir, &overlays, &overlay_count, &line, &col)) {
    send_response(id_v, json_new_null());
    return;
  }
  Lib3bLocation loc = lib3b_definition(dir, path, line, col, overlays, overlay_count);
  free(overlays);
  free(dir);
  free(path);
  if (!loc.found) {
    send_response(id_v, json_new_null());
  } else {
    char*      target_uri = path_to_uri(loc.file_path);
    JsonValue* start        = json_new_object();
    json_obj_set(start, "line", json_new_number((int)loc.line - 1));
    json_obj_set(start, "character", json_new_number((int)loc.col - 1));
    JsonValue* end = json_new_object();
    json_obj_set(end, "line", json_new_number((int)loc.line - 1));
    json_obj_set(end, "character", json_new_number((int)loc.col)); // 1-char span, as in make_lsp_diagnostic
    JsonValue* range = json_new_object();
    json_obj_set(range, "start", start);
    json_obj_set(range, "end", end);
    JsonValue* result = json_new_object();
    json_obj_set(result, "uri", json_new_string(target_uri));
    json_obj_set(result, "range", range);
    free(target_uri);
    send_response(id_v, result);
  }
  lib3b_free_location(&loc);
}

////////////////////////////////
//~ completion. Candidates come from three places: a package's own symbols
// (fresh recompile, falling back to the SymbolCache above), its imports, and
// the static lists below.
//
// The static lists are hand-curated from lower.c's and checker.c's
// str8_match_lit dispatch chains, not generated, so they can drift as those
// grow. They exist because special forms and builtins are not user-declared
// symbols that any top-level walk could find. NATIVE_PKGS, below, applies the
// same idea to compiler.c's embedded native packages: `(import os)` resolves to
// source baked into the binary, with no directory on disk to walk.

static const char* KEYWORDS[] = {
  "fn", "let", "var", "val", "if", "do", "else", "for", "while", "when", "continue", "break",
  "struct", "union", "enum", "flags", "alias", "extern", "private",
  "parallel", "parallel-for", "lane-fn", "match", "return", "break", "continue", "set",
  "cast", "reinterpret", "deref", "addr", "and", "or", "not", "true", "false", "nil",
  "void", "const", "import", "package", "push", "push0", "push-zero",
  "alloc", "scratch", "commit", "dyn-push", "handle", "handle-alloc",
  "handle-pool-init", "sizeof", "alignof", "type-name", "zero",
  "member-offset", "member-type", "cstring", "get", "get-in", "nth",
  "stream",
  "bit-and", "bit-or", "bit-xor", "bit-not", "bit-shl", "bit-shr",
  "Vector", "Map", "Set", "packed", "align",
};

static const char* BUILTINS[] = {
  "print", "println", "str", "create", "destroy", "mark", "pop", "free",
  "len", "dyn-count", "mem-copy", "mem-set", "mem-zero", "mem-compare",
  "cstring-copy", "min", "max", "clamp", "clamp-top", "clamp-bot",
  "align-pow2", "align-down-pow2", "align-pad-pow2", "abs",
  "sin", "cos", "tan", "asin", "acos", "atan", "sinh", "cosh", "tanh",
  "sqrt", "cbrt", "ceil", "floor", "round", "atan2", "pow", "mod",
  "lane-index", "lane-count", "lane-sync", "lane-arena",
  "handle-deref", "handle-free", "handle-valid?",
  "map-set", "map-get", "map-remove", "map-contains?",
  "set-add", "set-contains?", "set-remove",
  "vector-push", "vector-clear", "vector-contains?", "vector-remove-at",
  "vector-swap-remove", "vector-index-of",
  "string-to-i32", "string-to-i64", "string-to-u32", "string-to-u64",
  "string-to-f32", "string-to-f64",
  "string-match", "string-prefix", "string-skip", "string-postfix",
  "string-chop", "string-substr", "string-find", "string-find-reverse",
  "string-starts-with", "string-ends-with", "string-cat", "string-copy",
  "nth-checked", "sqrt-checked", "asin-checked", "acos-checked", "pow-checked",
  "pow2?", "pow2-or-zero?", "release", "reset", "swap",
};

// The public surface of compiler.c's embedded native packages (os, vm, rng --
// see known_embedded_native_package_source), hand-curated from
// native_pkgs/*/*.3b. Used as a fallback for both import-name and qualified
// `pkg/member` completion: unlike a sibling package on disk, no recompile or
// cache can ever discover these.
typedef struct {
  const char*        name;
  const char* const* members; // NULL-terminated
} NativePkg;

static const char* const NATIVE_PKG_OS_MEMBERS[]  = {
  "read-file", "write-file", "getenv", "get-time", "sleep", "list-dir", "file-mtime",
  "open", "close", "stdout", "stderr", "stdin",
  "write", "write-string", "read", "read-line", "flush", "seek", "tell",
  "at-end?", "error?",
  "mode-read", "mode-write", "mode-append", "mode-update",
  "seek-start", "seek-current", "seek-end", NULL,
};
static const char* const NATIVE_PKG_VM_MEMBERS[]  = {
  "create", "destroy", "load",
  "call0", "call1", "call2", "call3", "call4",
  "poll-reload", "unload",
  "register-getter", "register-setter1", "register-setter3", NULL,
};
static const char* const NATIVE_PKG_RNG_MEMBERS[] = {
  "create", "next-u32", "next-f32", "range-i32", "range-f32", "chance", NULL,
};

static const char* const NATIVE_PKG_SORT_MEMBERS[] = {
  "bubble-sort-i8", "bubble-sort-i16", "bubble-sort-i32", "bubble-sort-i64", "bubble-sort-u8", "bubble-sort-u16", "bubble-sort-u32", "bubble-sort-u64",
  "quick-sort-i8", "quick-sort-i16", "quick-sort-i32", "quick-sort-i64", "quick-sort-u8", "quick-sort-u16", "quick-sort-u32", "quick-sort-u64",
  "merge-sort-i8", "merge-sort-i16", "merge-sort-i32", "merge-sort-i64", "merge-sort-u8", "merge-sort-u16", "merge-sort-u32", "merge-sort-u64",
  "heap-sort-i8", "heap-sort-i16", "heap-sort-i32", "heap-sort-i64", "heap-sort-u8", "heap-sort-u16", "heap-sort-u32", "heap-sort-u64",
  "insertion-sort-i8", "insertion-sort-i16", "insertion-sort-i32", "insertion-sort-i64", "insertion-sort-u8", "insertion-sort-u16", "insertion-sort-u32", "insertion-sort-u64",
  NULL,
};

static const NativePkg NATIVE_PKGS[] = {
  { "os",   NATIVE_PKG_OS_MEMBERS },
  { "vm",   NATIVE_PKG_VM_MEMBERS },
  { "rng",  NATIVE_PKG_RNG_MEMBERS },
  { "sort", NATIVE_PKG_SORT_MEMBERS },
};

// Mirrors lexer.c's char_is_delim: an atom runs up to whitespace or one of
// `()[]{}";`. Character scanning only, just enough to find where the identifier
// being typed starts.
static bool
is_delim_char(char c) {
  return c == 0 || c == ' ' || c == '\t' || c == '\n' || c == '\r'
      || c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}'
      || c == '"' || c == ';';
}

// Returns the partial identifier being typed: everything from `character` back
// to the nearest delimiter on 0-indexed `line`. `character` is treated as a
// byte offset, as everywhere in this server, since 3b identifiers are ASCII.
// Returns an empty string, never NULL, when the cursor follows a delimiter.
static char*
extract_prefix(const char* text, int line, int character) {
  const char* p = text;
  for (int l = 0; l < line && *p; l += 1) {
    while (*p && *p != '\n') p += 1;
    if (*p == '\n') p += 1;
  }
  const char* line_start = p;
  const char* line_end   = p;
  while (*line_end && *line_end != '\n') line_end += 1;
  const char* cursor = line_start + character;
  if (cursor > line_end) cursor = line_end;
  const char* start = cursor;
  while (start > line_start && !is_delim_char(start[-1])) start -= 1;
  size_t len = (size_t)(cursor - start);
  char*  out = malloc(len + 1);
  memcpy(out, start, len);
  out[len] = 0;
  return out;
}

// Converts a 0-indexed LSP Position within `text` to a byte offset, which is
// what lib3b_completion_context takes. Same line walk as extract_prefix,
// without the backward scan.
static u64
line_character_to_offset(const char* text, int line, int character) {
  const char* p = text;
  for (int l = 0; l < line && *p; l += 1) {
    while (*p && *p != '\n') p += 1;
    if (*p == '\n') p += 1;
  }
  const char* line_end = p;
  while (*line_end && *line_end != '\n') line_end += 1;
  const char* cursor = p + character;
  if (cursor > line_end) cursor = line_end;
  return (u64)(cursor - text);
}

static bool
has_prefix(const char* name, const char* prefix) {
  return strncmp(name, prefix, strlen(prefix)) == 0;
}

static int
lib3b_symbol_kind_to_lsp(Lib3bSymbolKind kind) {
  switch (kind) {
    case Lib3bSymbol_Function: return 3;  // Function
    case Lib3bSymbol_Struct:   return 22; // Struct
    case Lib3bSymbol_Enum:     return 13; // Enum
    case Lib3bSymbol_Const:    return 21; // Constant
    case Lib3bSymbol_Var:      return 6;  // Variable
    case Lib3bSymbol_Alias:    return 25; // TypeParameter, the closest fit
    default:                   return 1;  // Text
  }
}

static void
push_completion_item(JsonValue* items, const char* label, int kind) {
  JsonValue* item = json_new_object();
  json_obj_set(item, "label", json_new_string(label));
  json_obj_set(item, "kind", json_new_number(kind));
  json_array_push(items, item);
}

// A completion item for a real declaration, carrying the same signature line
// and comment block its hover popup would show (Lib3bSymbol.detail/doc), so
// picking a name out of the list doesn't require hovering it first. Both are
// optional -- a const has no signature, most declarations have no comment --
// and an absent one is simply left off the item.
static void
push_symbol_completion_item(JsonValue* items, const char* label, int kind,
                             const char* detail, const char* doc) {
  JsonValue* item = json_new_object();
  json_obj_set(item, "label", json_new_string(label));
  json_obj_set(item, "kind", json_new_number(kind));
  if (detail) json_obj_set(item, "detail", json_new_string(detail));
  if (doc) json_obj_set(item, "documentation", json_new_string(doc));
  json_array_push(items, item);
}

// A builtin whose call shapes lib3b knows (`print`, `println`) is offered with
// its first shape as `detail` and the full list as `documentation`, so the
// optional leading `stream` is visible from the completion popup rather than
// only after the call is written and hovered. Builtins with no listed shapes
// push a plain item, exactly as before.
static void
push_builtin_completion_item(JsonValue* items, const char* label) {
  const char* shapes = lib3b_builtin_shapes(label);
  if (!shapes) { push_completion_item(items, label, 3); return; } // Function

  JsonValue* item = json_new_object();
  json_obj_set(item, "label", json_new_string(label));
  json_obj_set(item, "kind", json_new_number(3)); // Function
  const char* nl = strchr(shapes, '\n');
  if (nl) {
    char first[256];
    size_t len = (size_t)(nl - shapes);
    if (len >= sizeof(first)) len = sizeof(first) - 1;
    memcpy(first, shapes, len);
    first[len] = 0;
    json_obj_set(item, "detail", json_new_string(first));
  } else {
    json_obj_set(item, "detail", json_new_string(shapes));
  }
  json_obj_set(item, "documentation", json_new_string(shapes));
  json_array_push(items, item);
}

static void
handle_completion(JsonValue* id_v, JsonValue* params) {
  JsonValue* td      = json_obj_get(params, "textDocument");
  JsonValue* uri_v    = td ? json_obj_get(td, "uri") : NULL;
  JsonValue* pos      = json_obj_get(params, "position");
  JsonValue* line_v = pos ? json_obj_get(pos, "line") : NULL;
  JsonValue* char_v = pos ? json_obj_get(pos, "character") : NULL;
  if (!uri_v || uri_v->kind != Json_String || !line_v || !char_v) {
    send_response(id_v, json_new_null());
    return;
  }
  char* path = uri_to_path(uri_v->string);
  if (!path) {
    send_response(id_v, json_new_null());
    return;
  }
  char* dir = path_dirname(path);

  const char* text = NULL;
  for (OpenDoc* d = g_open_docs; d; d = d->next) {
    if (strcmp(d->path, path) == 0) { text = d->text; break; }
  }
  char* prefix = text ? extract_prefix(text, (int)line_v->number, (int)char_v->number) : strdup("");

  JsonValue* items = json_new_array();

  // `/` is not a delimiter to the lexer, so `gl/gen` arrives from
  // extract_prefix as one atom. Splitting at the last `/` only picks which
  // import's members to offer; the filter below still matches the full
  // prefix, keeping prefix semantics uniform.
  const char* slash = strrchr(prefix, '/');
  if (slash) {
    size_t pkg_len   = (size_t)(slash - prefix);
    char   pkg_name[256];
    size_t copy_len = pkg_len < sizeof(pkg_name) - 1 ? pkg_len : sizeof(pkg_name) - 1;
    memcpy(pkg_name, prefix, copy_len);
    pkg_name[copy_len] = 0;

    bool live_import_found = false;
    for (CachedImport* imp = symbol_cache_lookup_imports(dir); imp; imp = imp->next) {
      if (strcmp(imp->name, pkg_name) != 0) continue;
      live_import_found = true;
      for (CachedSymbol* s = imp->symbols; s; s = s->next) {
        char qualified[512];
        snprintf(qualified, sizeof(qualified), "%s/%s", imp->name, s->name);
        if (has_prefix(qualified, prefix)) {
          push_symbol_completion_item(items, qualified, lib3b_symbol_kind_to_lsp(s->kind), s->detail, s->doc);
        }
      }
      break;
    }
    // No cached import matched, so fall back to the NATIVE_PKGS surface: an
    // embedded package has no directory a real compile could have found.
    if (!live_import_found) {
      for (size_t i = 0; i < sizeof(NATIVE_PKGS) / sizeof(NATIVE_PKGS[0]); i += 1) {
        if (strcmp(NATIVE_PKGS[i].name, pkg_name) != 0) continue;
        for (const char* const* m = NATIVE_PKGS[i].members; *m; m += 1) {
          char qualified[512];
          snprintf(qualified, sizeof(qualified), "%s/%s", NATIVE_PKGS[i].name, *m);
          if (has_prefix(qualified, prefix)) push_completion_item(items, qualified, 3); // Function
        }
        break;
      }
    }
  } else {
    // A fresh tolerant recompile reflects the buffer as of this keystroke, so
    // a function added two lines up completes immediately instead of after the
    // next successful check. Purely additive: if even the patched buffer fails
    // to parse, this falls through to the cache below.
    bool used_fresh = false;
    if (text) {
      u64            overlay_count;
      SourceOverlay* overlays = build_overlays_for_dir(dir, &overlay_count);
      u64            offset    = line_character_to_offset(text, (int)line_v->number, (int)char_v->number);
      Lib3bCompletionContext cc = lib3b_completion_context(dir, path, text, offset, overlays, overlay_count);
      free(overlays);
      if (cc.ok) {
        used_fresh = true;
        foreach_index(i, cc.symbol_count) {
          if (has_prefix(cc.symbols[i].name, prefix)) {
            push_symbol_completion_item(items, cc.symbols[i].name, lib3b_symbol_kind_to_lsp(cc.symbols[i].kind),
                                         cc.symbols[i].detail, cc.symbols[i].doc);
          }
        }
        // Params, let-locals and loop vars in scope at the cursor, already
        // deduped against cc.symbols by lib3b.c.
        foreach_index(i, cc.local_count) {
          if (has_prefix(cc.locals[i].name, prefix)) {
            push_completion_item(items, cc.locals[i].name, lib3b_symbol_kind_to_lsp(cc.locals[i].kind));
          }
        }
      }
      lib3b_free_completion_context(&cc);
    }
    if (!used_fresh) {
      for (CachedSymbol* s = symbol_cache_lookup(dir); s; s = s->next) {
        if (has_prefix(s->name, prefix)) {
          push_symbol_completion_item(items, s->name, lib3b_symbol_kind_to_lsp(s->kind), s->detail, s->doc);
        }
      }
    }
    // Import names, unqualified: typing "g" surfaces "gl" before the "/".
    CachedImport* live_imports = symbol_cache_lookup_imports(dir);
    for (CachedImport* imp = live_imports; imp; imp = imp->next) {
      if (has_prefix(imp->name, prefix)) push_completion_item(items, imp->name, 9); // Module
    }
    // NATIVE_PKGS names too, skipping any a cached import already offered so
    // an embedded package that is also imported does not appear twice.
    for (size_t i = 0; i < sizeof(NATIVE_PKGS) / sizeof(NATIVE_PKGS[0]); i += 1) {
      bool already_live = false;
      for (CachedImport* imp = live_imports; imp; imp = imp->next) {
        if (strcmp(imp->name, NATIVE_PKGS[i].name) == 0) { already_live = true; break; }
      }
      if (!already_live && has_prefix(NATIVE_PKGS[i].name, prefix)) {
        push_completion_item(items, NATIVE_PKGS[i].name, 9); // Module
      }
    }
    for (size_t i = 0; i < sizeof(KEYWORDS) / sizeof(KEYWORDS[0]); i += 1) {
      if (has_prefix(KEYWORDS[i], prefix)) push_completion_item(items, KEYWORDS[i], 14); // Keyword
    }
    for (size_t i = 0; i < sizeof(BUILTINS) / sizeof(BUILTINS[0]); i += 1) {
      if (has_prefix(BUILTINS[i], prefix)) push_builtin_completion_item(items, BUILTINS[i]);
    }
  }

  JsonValue* result = json_new_object();
  json_obj_set(result, "isIncomplete", json_new_bool(false));
  json_obj_set(result, "items", items);
  send_response(id_v, result);

  free(prefix);
  free(dir);
  free(path);
}

////////////////////////////////
//~ workspace/symbol. Scans the whole workspace once, on first use rather than
// at startup, and caches every discovered package's symbols in the same
// SymbolCache completion maintains; queries only read that cache. A package
// created after the scan does not appear until the server restarts.

static bool g_workspace_scanned = false;

static void
scan_workspace_symbols(void) {
  g_workspace_scanned = true;
  if (!g_workspace_root) return;
  char**  dirs  = NULL;
  size_t  count = 0, cap = 0;
  discover_packages(g_workspace_root, &dirs, &count, &cap);
  for (size_t i = 0; i < count; i += 1) {
    Lib3bCheckResult r = lib3b_check_package_with_overlays(dirs[i], NULL, 0);
    if (r.ok) symbol_cache_update(dirs[i], r.symbols, r.symbol_count, r.imports, r.import_count);
    lib3b_free_result(&r);
    free(dirs[i]);
  }
  free(dirs);
}

// LSP's SymbolKind enum, which numbers differently from the CompletionItemKind
// of lib3b_symbol_kind_to_lsp above.
static int
lib3b_symbol_kind_to_lsp_symbolkind(Lib3bSymbolKind kind) {
  switch (kind) {
    case Lib3bSymbol_Function: return 12; // Function
    case Lib3bSymbol_Struct:   return 23; // Struct
    case Lib3bSymbol_Enum:     return 10; // Enum
    case Lib3bSymbol_Const:    return 14; // Constant
    case Lib3bSymbol_Var:      return 13; // Variable
    case Lib3bSymbol_Alias:    return 26; // TypeParameter
    default:                   return 1;  // File; unreachable placeholder
  }
}

static void
handle_workspace_symbol(JsonValue* id_v, JsonValue* params) {
  if (!g_workspace_scanned) scan_workspace_symbols();

  JsonValue*  query_v = json_obj_get(params, "query");
  const char* query   = (query_v && query_v->kind == Json_String) ? query_v->string : "";

  JsonValue* results = json_new_array();
  for (SymbolCache* e = g_symbol_cache; e; e = e->next) {
    const char* container = e->dir_path; // full path: unambiguous, if verbose
    for (CachedSymbol* s = e->symbols; s; s = s->next) {
      if (query[0] != '\0' && !strcasestr(s->name, query)) continue;
      if (!s->file_path) continue;
      char* uri = path_to_uri(s->file_path);
      JsonValue* start = json_new_object();
      json_obj_set(start, "line", json_new_number((int)s->line - 1));
      json_obj_set(start, "character", json_new_number((int)s->col - 1));
      JsonValue* end = json_new_object();
      json_obj_set(end, "line", json_new_number((int)s->line - 1));
      json_obj_set(end, "character", json_new_number((int)s->col));
      JsonValue* range = json_new_object();
      json_obj_set(range, "start", start);
      json_obj_set(range, "end", end);
      JsonValue* location = json_new_object();
      json_obj_set(location, "uri", json_new_string(uri));
      json_obj_set(location, "range", range);
      free(uri);

      JsonValue* item = json_new_object();
      json_obj_set(item, "name", json_new_string(s->name));
      json_obj_set(item, "kind", json_new_number(lib3b_symbol_kind_to_lsp_symbolkind(s->kind)));
      json_obj_set(item, "location", location);
      json_obj_set(item, "containerName", json_new_string(container));
      json_array_push(results, item);
    }
  }
  send_response(id_v, results);
}

////////////////////////////////
//~ textDocument/documentSymbol -- workspace/symbol's data narrowed to one
// file, which is what drives an editor's outline and breadcrumb bar.
//
// Answered purely from the SymbolCache, with no compile of its own.
// check_and_publish refreshes that cache on every didOpen/didChange/didSave,
// so by the time a client can ask about a document it has already been
// checked; an outline view re-requests on every edit, often enough that the
// fresh compile per query hover and definition can afford, this cannot.
//
// Reading the cache also gives the right behaviour mid-edit for free: the
// cache holds the last state that CHECKED, so breaking the file leaves the
// outline showing what was there rather than blanking it on every keystroke.
// A package that has never checked cleanly has no entry and yields an empty
// array -- unknown, not empty, but a quiet outline is the only useful way to
// render that either way.
//
// Replies with SymbolInformation[], the flat shape, not the nested
// DocumentSymbol[]. DocumentSymbol requires a `range` spanning the whole
// declaration, and Lib3bSymbol carries only the declaration's start position;
// filling the rest in with a guess would put a wrong highlight in the
// breadcrumb bar. 3b also has no nesting to represent here -- these are top
// level fn/struct/enum/const/var/alias only, with no members hung underneath.
static void
handle_document_symbol(JsonValue* id_v, JsonValue* params) {
  JsonValue* td    = json_obj_get(params, "textDocument");
  JsonValue* uri_v = td ? json_obj_get(td, "uri") : NULL;
  if (!uri_v || uri_v->kind != Json_String) {
    send_response(id_v, json_new_null());
    return;
  }
  char* path = uri_to_path(uri_v->string);
  if (!path) {
    send_response(id_v, json_new_null());
    return;
  }
  char* dir = path_dirname(path);

  JsonValue* results = json_new_array();
  for (CachedSymbol* s = symbol_cache_lookup(dir); s; s = s->next) {
    if (!s->file_path || strcmp(s->file_path, path) != 0) continue;
    JsonValue* start = json_new_object();
    json_obj_set(start, "line", json_new_number((int)s->line - 1));
    json_obj_set(start, "character", json_new_number((int)s->col - 1));
    JsonValue* end = json_new_object();
    json_obj_set(end, "line", json_new_number((int)s->line - 1));
    json_obj_set(end, "character", json_new_number((int)s->col)); // 1-char span, as in make_lsp_diagnostic
    JsonValue* range = json_new_object();
    json_obj_set(range, "start", start);
    json_obj_set(range, "end", end);
    JsonValue* location = json_new_object();
    json_obj_set(location, "uri", json_new_string(uri_v->string));
    json_obj_set(location, "range", range);

    JsonValue* item = json_new_object();
    json_obj_set(item, "name", json_new_string(s->name));
    json_obj_set(item, "kind", json_new_number(lib3b_symbol_kind_to_lsp_symbolkind(s->kind)));
    json_obj_set(item, "location", location);
    json_array_push(results, item);
  }
  send_response(id_v, results);

  free(dir);
  free(path);
}

////////////////////////////////
//~ textDocument/formatting -- format.c (`3b format`) as a whole-document
// TextEdit, which is what turns format-on-save on in every editor.
//
// Whole-document is the honest granularity: format.c re-renders a parsed
// program top to bottom, so there is no smaller edit to describe. A client
// applying it keeps its own cursor and folding state; that is its job, not the
// server's.

// The 0-indexed LSP Position one past the last byte of `text`.
static void
end_position(const char* text, int* out_line, int* out_character) {
  int         line       = 0;
  const char* line_start = text;
  for (const char* p = text; *p; p += 1) {
    if (*p == '\n') { line += 1; line_start = p + 1; }
  }
  *out_line      = line;
  *out_character = (int)strlen(line_start); // bytes, the unit used throughout this server
}

static void
handle_formatting(JsonValue* id_v, JsonValue* params) {
  JsonValue* td    = json_obj_get(params, "textDocument");
  JsonValue* uri_v = td ? json_obj_get(td, "uri") : NULL;
  if (!uri_v || uri_v->kind != Json_String) {
    send_response(id_v, json_new_null());
    return;
  }
  char* path = uri_to_path(uri_v->string);
  if (!path) {
    send_response(id_v, json_new_null());
    return;
  }

  // Only an open document can be formatted. A client is required to have sent
  // didOpen before requesting this, and formatting whatever happens to be on
  // disk instead would hand the editor an edit against text it isn't showing.
  const char* text = NULL;
  for (OpenDoc* d = g_open_docs; d; d = d->next) {
    if (strcmp(d->path, path) == 0) { text = d->text; break; }
  }
  if (!text) {
    send_response(id_v, json_new_null());
    free(path);
    return;
  }

  char* formatted = lib3b_format(path, text);
  free(path);
  // A buffer that doesn't parse has no formatting: null leaves the document
  // untouched, which is what a save in the middle of an unbalanced edit wants.
  if (!formatted) {
    send_response(id_v, json_new_null());
    return;
  }

  JsonValue* edits = json_new_array();
  if (strcmp(formatted, text) != 0) { // already-formatted file: no edit at all, so the
                                       // editor doesn't mark the buffer dirty on every save
    int end_line, end_char;
    end_position(text, &end_line, &end_char);
    JsonValue* start = json_new_object();
    json_obj_set(start, "line", json_new_number(0));
    json_obj_set(start, "character", json_new_number(0));
    JsonValue* end = json_new_object();
    json_obj_set(end, "line", json_new_number(end_line));
    json_obj_set(end, "character", json_new_number(end_char));
    JsonValue* range = json_new_object();
    json_obj_set(range, "start", start);
    json_obj_set(range, "end", end);
    JsonValue* edit = json_new_object();
    json_obj_set(edit, "range", range);
    json_obj_set(edit, "newText", json_new_string(formatted));
    json_array_push(edits, edit);
  }
  free(formatted);
  send_response(id_v, edits);
}

static void
handle_message(JsonValue* msg) {
  JsonValue*  method_v = json_obj_get(msg, "method");
  JsonValue*  id_v     = json_obj_get(msg, "id");
  JsonValue*  params_v = json_obj_get(msg, "params");
  const char* method   = (method_v && method_v->kind == Json_String) ? method_v->string : NULL;
  if (!method) return; // a response to a request; this server sends none

  if (strcmp(method, "initialize") == 0) {
    handle_initialize(id_v, params_v);
  } else if (strcmp(method, "initialized") == 0) {
    // no-op
  } else if (strcmp(method, "textDocument/didOpen") == 0) {
    handle_did_open(params_v);
  } else if (strcmp(method, "textDocument/didChange") == 0) {
    handle_did_change(params_v);
  } else if (strcmp(method, "textDocument/didSave") == 0) {
    handle_check_notification(params_v);
  } else if (strcmp(method, "textDocument/didClose") == 0) {
    JsonValue* td    = json_obj_get(params_v, "textDocument");
    JsonValue* uri_v = td ? json_obj_get(td, "uri") : NULL;
    if (uri_v && uri_v->kind == Json_String) handle_close(uri_v->string);
  } else if (strcmp(method, "textDocument/hover") == 0) {
    handle_hover(id_v, params_v);
  } else if (strcmp(method, "textDocument/definition") == 0) {
    handle_definition(id_v, params_v);
  } else if (strcmp(method, "textDocument/completion") == 0) {
    handle_completion(id_v, params_v);
  } else if (strcmp(method, "workspace/symbol") == 0) {
    handle_workspace_symbol(id_v, params_v);
  } else if (strcmp(method, "textDocument/documentSymbol") == 0) {
    handle_document_symbol(id_v, params_v);
  } else if (strcmp(method, "textDocument/formatting") == 0) {
    handle_formatting(id_v, params_v);
  } else if (strcmp(method, "shutdown") == 0) {
    g_got_shutdown = true;
    send_response(id_v, json_new_null());
  } else if (strcmp(method, "exit") == 0) {
    exit(g_got_shutdown ? 0 : 1); // per spec: exit 1 if the client skipped shutdown
  } else if (id_v) {
    send_error_response(id_v, -32601, "method not found"); // a request must never hang
  }
  // an unhandled notification (no id) is ignored, as the spec requires
}

int
main(void) {
  g_out = stdout;
  for (;;) {
    size_t len;
    char*  body = read_message(stdin, &len);
    if (!body) break; // EOF without a shutdown/exit handshake
    JsonValue* msg = json_parse(body, len);
    free(body);
    if (msg) {
      handle_message(msg);
      json_free(msg);
    }
  }
  return g_got_shutdown ? 0 : 1;
}
