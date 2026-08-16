// Diagnostics: "error: <msg>", then "--> path:line:col", then the offending
// source line quoted with a caret underline. Every Token carries line, column
// and file_id from the lexer, so any error function holding one gets all of
// this by calling diag_error rather than fprintf.
#include "3b.h"

static SourceFile* g_source_files = NULL; // dyn array; index 0 reserved, see SOURCE_FILE_UNKNOWN

static void
source_registry_ensure_init(void) {
  if (dyn_count(g_source_files) == 0) {
    SourceFile unknown = {0};
    unknown.path       = str8_lit("<unknown>");
    unknown.text        = str8_lit("");
    dyn_push(ctx_perm(), g_source_files, unknown);
  }
}

u32
source_file_register(String8 path, String8 text) {
  source_registry_ensure_init();
  SourceFile sf = {0};
  sf.path       = path;
  sf.text       = text;
  dyn_push(ctx_perm(), g_source_files, sf);
  return (u32)(dyn_count(g_source_files) - 1);
}

SourceFile*
source_file_get(u32 file_id) {
  source_registry_ensure_init();
  if (file_id >= dyn_count(g_source_files)) file_id = SOURCE_FILE_UNKNOWN;
  return &g_source_files[file_id];
}

// g_source_files is a global holding a ctx_perm() dyn array. That suits the
// CLI, which inits once and exits, but a long-lived host calling
// ctx_init/ctx_free repeatedly would leave it pointing at memory the arena has
// already torn down. Call this after ctx_init and before registering a file.
void
source_registry_reset(void) {
  g_source_files = NULL;
}

// The 1-indexed `line`'th line of `text`, without its newline. Empty when
// `text` has no such line, as for an unregistered file or a synthetic token
// whose line number matches no real source.
static String8
source_line_text(String8 text, u32 line) {
  u64 pos = 0;
  u32 cur = 1;
  while (cur < line && pos < text.size) {
    if (text.str[pos] == '\n') cur += 1;
    pos += 1;
  }
  if (cur != line) return str8_lit("");
  u64 start = pos;
  while (pos < text.size && text.str[pos] != '\n') pos += 1;
  return str8_range(text.str + start, text.str + pos);
}

static b32
stderr_supports_color(void) {
  static int cached = -1;
  if (cached < 0) cached = isatty(fileno(stderr)) ? 1 : 0;
  return cached;
}

////////////////////////////////
//~ Diagnostic capture
//
// Collects diagnostics as data for an embedder; see diag_capture_begin in
// 3b.h. Additive, piggybacking on the message diag_errorv already formats, so
// no call site changes.
//
// Process-wide globals rather than per-thread, with the same caveat as
// g_source_files above: one caller at a time is fine, two capturing
// concurrently is a data race.

static Diagnostic* g_captured           = NULL; // dyn array
static b32          g_capturing         = false;
static b32          g_capture_also_print = true;

void
diag_capture_begin(b32 also_print) {
  g_captured           = NULL;
  g_capturing           = true;
  g_capture_also_print = also_print;
}

Diagnostic*
diag_capture_end(u64* out_count) {
  g_capturing = false;
  if (out_count) *out_count = dyn_count(g_captured);
  return g_captured;
}

void
diag_errorv(Token tok, const char* fmt, va_list args) {
  SourceFile* sf  = source_file_get(tok.file_id);
  String8     msg = str8fv(ctx_perm(), (char*)fmt, args);

  if (g_capturing) {
    Diagnostic d = {0};
    d.message   = msg;
    d.file_path = sf->path;
    d.line      = tok.line;
    d.col       = tok.col;
    dyn_push(ctx_perm(), g_captured, d);
    if (!g_capture_also_print) return;
  }

  b32         color = stderr_supports_color();
  const char* red    = color ? "\x1b[1;31m" : "";
  const char* bold   = color ? "\x1b[1m"    : "";
  const char* blue   = color ? "\x1b[1;34m" : "";
  const char* reset  = color ? "\x1b[0m"    : "";

  fprintf(stderr, "%serror:%s%s %.*s%s\n", red, reset, bold, str8_varg(msg), reset);
  fprintf(stderr, "%s  --> %s%.*s:%u:%u\n", blue, reset, str8_varg(sf->path), tok.line, tok.col);

  String8 line_text = source_line_text(sf->text, tok.line);
  char     linebuf[16];
  int      linelen = snprintf(linebuf, sizeof(linebuf), "%u", tok.line);

  fprintf(stderr, "%s%*s |%s\n", blue, linelen, "", reset);
  fprintf(stderr, "%s%s |%s %.*s\n", blue, linebuf, reset, str8_varg(line_text));

  u32 underline_col = (tok.col >= 1) ? tok.col - 1 : 0;
  u32 underline_len = (u32)tok.text.size;
  if (underline_len == 0) underline_len = 1;
  // Clamped so the underline never runs past the quoted line; a synthetic
  // token's span does not always match real source text.
  if (underline_col > line_text.size) underline_col = (u32)line_text.size;
  if (underline_col + underline_len > line_text.size) {
    underline_len = (line_text.size > underline_col) ? (u32)(line_text.size - underline_col) : 1;
  }

  fprintf(stderr, "%s%*s |%s ", blue, linelen, "", reset);
  foreach_index(i, underline_col) { fputc(' ', stderr); }
  fprintf(stderr, "%s", red);
  foreach_index(i, underline_len) { fputc('^', stderr); }
  fprintf(stderr, "%s\n", reset);
}

void
diag_error(Token tok, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  diag_errorv(tok, fmt, args);
  va_end(args);
}
