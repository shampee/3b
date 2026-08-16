// bcosprims.c -- the bytecode VM's half of the `os` module (see
// bcosprims.h). A script can only reach native capability the embedding
// program explicitly registered; this table is what `(import os)` registers.
//
// This started as the minimal set the `build` module (translate/build.3bs)
// needed for a portable pkg-config-equivalent, and grew opportunistically
// from there -- which left it a strict subset of what natively-compiled code
// got from native_pkgs/os/os.3b, differing on nothing more principled than
// which side had happened to need a verb first. It is now a full MIRROR of
// that file: same names, same signatures, same semantics, so one `(import
// os)` source text runs on either backend. See os.3b's own header for the
// short list of what genuinely can't be made identical, and keep the two in
// step -- examples/os-portable is the standing check that they are.
//
// STRING CONVENTION: every `string`-typed Trampoline argument and return is
// the ADDRESS of a boxed {ptr,size} String8 header, like every string value in
// this VM, and bc_os_unbox_string/bc_os_box_string are the glue.
//
// ARENA CONVENTION: every primitive that RETURNS freshly-allocated memory --
// a string, a Vector -- takes a leading `arena` parameter and allocates into
// THAT, not into the BcHostFn `heap` parameter. A VM `arena` register holds a
// real base.h `Arena*`, so this is genuinely honored rather than accepted and
// ignored, and it is what makes the signatures here match os.3b's, which have
// always been arena-taking. It also means a value returned from inside a
// `(scratch [temp] ...)` block is reclaimed with that block on both backends
// alike. `heap` consequently goes unused throughout this file.
//
// The corollary, and the one real trap in here: `arena` is very often
// ctx_scratch() ITSELF, so a primitive must never stage its result through an
// ArenaTemp taken on ctx_scratch and then copy it out -- arena_temp_end would
// pop the returned bytes back off. Where a primitive needs scratch anyway (a
// NUL-terminated path for the libc call), the temp is opened and CLOSED again
// before the first allocation into `arena`, which is safe even when they are
// the same arena.
//
// All Trampoline, no Direct: the primitives taking or returning a `string`
// need that boxing glue, and Direct mode has nowhere to put it -- it calls the
// native function with no wrapper. Direct's integer-register convention could
// otherwise carry everything here, since a string, a `stream` and an `arena`
// are all pointers. The pure-pointer stream verbs could go Direct, but they're
// all wrappers written for this table rather than pre-existing functions, so
// splitting the set across two mechanisms would gain nothing.
#include "bcosprims.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>     // bc_os_get_time
#include <sys/stat.h> // bc_os_file_exists/dir_exists/file_mtime -- mingw has this too
#if !defined(_WIN32)
# include <dirent.h>  // bc_os_list_dir
# include <unistd.h>  // bc_os_sleep_fn
#endif
// windows.h itself comes in transitively (3b.h -> base/base.h's own
// _WIN32 branch), same convention file.c already established.

// Most entries os/list-dir will collect from one directory before it
// silently stops. Must equal native_pkgs/os/os.3b's own `list-dir-max`,
// which bc_os_constant_decls below republishes to scripts by that name --
// the two backends truncating a huge directory at different points would
// be exactly the kind of quiet divergence this module is meant not to
// have.
#define BC_OS_LIST_DIR_MAX 512

static String8
bc_os_unbox_string(i64 arg) {
  String8* header = (String8*)(intptr_t)arg;
  return *header;
}

// `dst` is the CALLER'S arena, never the BcHostFn `heap` parameter -- see
// the arena convention in this file's header. Copies the bytes as well as
// the header, so `s` may point at anything, including a libc buffer.
static i64
bc_os_box_string(Arena* dst, String8 s) {
  u8* bytes = s.size > 0 ? push_array(dst, u8, s.size) : NULL;
  if (s.size > 0) MemoryCopy(bytes, s.str, s.size);
  String8* header = push_one(dst, String8);
  header->str  = bytes;
  header->size = s.size;
  return (i64)(intptr_t)header;
}

// A NUL-terminated C string copy of a String8 -- most OS calls (getenv,
// popen, stat) want `const char*`, not a length-prefixed buffer. Always
// allocated into a CALLER-supplied (typically transient/scratch) arena,
// never `heap` -- this is throwaway conversion scratch, not a value that
// needs to outlive the primitive's own call.
static char*
bc_os_cstr(Arena* arena, String8 s) {
  char* buf = push_array(arena, char, s.size + 1);
  if (s.size > 0) MemoryCopy(buf, s.str, s.size);
  buf[s.size] = 0;
  return buf;
}

// Runs `body` with `name` bound to a NUL-terminated copy of the boxed
// string in `arg`, then RELEASES the scratch that copy lives in.
//
// A macro wrapping the body, rather than a function handing a `char*`
// back, precisely so the release can't be forgotten or reordered: every
// caller here needs the C string only for the one fopen/opendir/stat that
// consumes it, and must have released the scratch again before allocating
// anything into its own `arena` -- see this file's header for why (the
// two can be the same arena). `body` must contain no preprocessor
// directives, since it is a macro argument.
#define BC_OS_WITH_CSTR(arg, name, body)                              \
  do {                                                                 \
    ArenaTemp _temp = arena_temp_begin(ctx_scratch());                 \
    char*     name  = bc_os_cstr(_temp.arena, bc_os_unbox_string(arg));\
    body                                                               \
    arena_temp_end(&_temp);                                            \
  } while (0)

// Allocates into the CALLER'S arena (`args[0]`), like every string- or
// Vector-returning primitive in this file -- see the arena convention in
// this file's own header, and native_pkgs/os/os.3b's `getenv`, whose
// signature this now matches exactly.
static i64
bc_os_getenv(i64* args, u32 arg_count, Arena* heap, void* userdata) {
  (void)arg_count; (void)heap; (void)userdata;
  Arena*      dst   = (Arena*)(intptr_t)args[0];
  const char* value = NULL;
  // `getenv` hands back a pointer into the process environment, not into
  // the scratch arena, so it stays valid after the temp is released.
  BC_OS_WITH_CSTR(args[1], cname, { value = getenv(cname); });
  // Missing/unset -> empty string, not an error -- the same "empty means
  // absence" convention read-file uses. Distinguishing "unset" from "set
  // to empty" would need a real `(bool string)` two-return host import,
  // and os.3b's native getenv doesn't distinguish them either.
  return bc_os_box_string(dst, value ? str8_cstring((char*)value) : (String8){0});
}

// `stat` + S_IFMT on every platform, including Windows (mingw's own
// <sys/stat.h>) -- the same test runtime/bbb_file.c's native
// bbb_os_file_exists/bbb_os_dir_exists make, so the two backends can't
// disagree about what counts as a file.
static i64
bc_os_file_exists(i64* args, u32 arg_count, Arena* heap, void* userdata) {
  (void)arg_count; (void)heap; (void)userdata;
  b32 exists = false;
  BC_OS_WITH_CSTR(args[0], cpath, {
    struct stat st;
    exists = stat(cpath, &st) == 0 && (st.st_mode & S_IFMT) == S_IFREG;
  });
  return exists ? 1 : 0;
}

static i64
bc_os_dir_exists(i64* args, u32 arg_count, Arena* heap, void* userdata) {
  (void)arg_count; (void)heap; (void)userdata;
  b32 exists = false;
  BC_OS_WITH_CSTR(args[0], cpath, {
    struct stat st;
    exists = stat(cpath, &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR;
  });
  return exists ? 1 : 0;
}

// Runs `cmd` through the platform shell (popen/_popen -- always shell-
// interpreted, same as every other embedding of this technique) and
// captures its STDOUT, with a single trailing \n (or \r\n) trimmed if
// present -- matches how a POSIX shell's own `$(cmd)` command
// substitution already strips it, the convention a script author calling
// this is most likely expecting. The motivating use case (see this file's
// own top-of-file note): `(exec-capture temp "pkg-config --cflags sdl2")`.
static i64
bc_os_exec_capture(i64* args, u32 arg_count, Arena* heap, void* userdata) {
  (void)arg_count; (void)heap; (void)userdata;
  Arena* dst = (Arena*)(intptr_t)args[0];
  // Spelled out rather than run through BC_OS_WITH_CSTR: the platform
  // split needs an `#if` where that macro would want its body argument,
  // and preprocessor directives inside a macro invocation are undefined.
  ArenaTemp temp = arena_temp_begin(ctx_scratch());
  char*     ccmd = bc_os_cstr(temp.arena, bc_os_unbox_string(args[1]));
#if defined(_WIN32)
  FILE* p = _popen(ccmd, "r");
#else
  FILE* p = popen(ccmd, "r");
#endif
  arena_temp_end(&temp); // before the first `dst` allocation, see this file's header
  if (!p) return bc_os_box_string(dst, (String8){0});

  // Grown DIRECTLY in `dst`, with no scratch staging step -- `dst` may be
  // ctx_scratch itself, which is exactly the case staging would corrupt.
  u8* out = NULL; // dyn array, in `dst` -- its buffer IS the returned string's bytes
  u8  chunk[4096];
  size_t n;
  while ((n = fread(chunk, 1, sizeof(chunk), p)) > 0) {
    foreach_index(i, n) dyn_push(dst, out, chunk[i]);
  }
#if defined(_WIN32)
  _pclose(p);
#else
  pclose(p);
#endif

  u64 len = dyn_count(out);
  if (len > 0 && out[len - 1] == '\n') len -= 1;
  if (len > 0 && out[len - 1] == '\r') len -= 1;

  String8* header = push_one(dst, String8);
  header->str  = out;
  header->size = len;
  return (i64)(intptr_t)header;
}

// The whole-file counterpart to the incremental stream verbs below, and
// the same fopen/fseek/ftell/fread shape runtime/bbb_file.c's native
// bbb_os_file_read uses -- including its ~2GB cap on Windows, where
// `ftell` returns a 32-bit long. Reads into the CALLER'S arena.
static i64
bc_os_read_file(i64* args, u32 arg_count, Arena* heap, void* userdata) {
  (void)arg_count; (void)heap; (void)userdata;
  Arena* dst = (Arena*)(intptr_t)args[0];
  FILE*  f   = NULL;
  BC_OS_WITH_CSTR(args[1], cpath, { f = fopen(cpath, "rb"); });
  if (!f) return bc_os_box_string(dst, (String8){0});

  String8 contents = {0};
  long    file_size;
  if (fseek(f, 0, SEEK_END) == 0 && (file_size = ftell(f)) > 0 && fseek(f, 0, SEEK_SET) == 0) {
    u64 size  = (u64)file_size;
    u8* bytes = push_array(dst, u8, size);
    if (fread(bytes, 1, size, f) == size) contents = (String8){ bytes, size };
    // A short read leaves `contents` empty but the bytes allocated in
    // `dst`: a failed whole-file read is rare enough that reclaiming them
    // isn't worth an arena mark, and `dst` is the caller's to reset.
  }
  fclose(f);

  String8* header = push_one(dst, String8);
  *header = contents;
  return (i64)(intptr_t)header;
}

// Creates `path` or truncates it, then writes `contents` whole. "wb", so
// no platform silently rewrites the bytes going out -- the same reasoning
// (and the same libc calls) as native bbb_os_file_write.
static i64
bc_os_write_file(i64* args, u32 arg_count, Arena* heap, void* userdata) {
  (void)arg_count; (void)heap; (void)userdata;
  String8 contents = bc_os_unbox_string(args[1]);
  FILE*   f        = NULL;
  BC_OS_WITH_CSTR(args[0], cpath, { f = fopen(cpath, "wb"); });
  if (!f) return 0;

  u64 written = contents.size > 0 ? (u64)fwrite(contents.str, 1, contents.size, f) : 0;
  fclose(f);
  return written == contents.size ? 1 : 0;
}

// Every entry's bare filename directly inside `dir`, as a real `[string]`
// Vector in the CALLER'S arena.
//
// The Vector is a plain base.h dyn array -- the identical representation
// bytecode-compiled 3b code uses for one (a T* with a hidden DynHdr
// immediately before it; see BcOp_DynCount/BcOp_DynGrow), with `string`
// elements laid out as 16-byte {ptr,size} headers inline, which is what
// layout_of gives a `[string]` and therefore what bc_element_stride makes
// the compiled `(nth entries i)` expect. Returning one straight from a
// host import needs no new machinery for exactly that reason, and an
// empty listing correctly comes back as the NULL pointer a freshly
// declared `(var v [string])` already holds.
//
// Native os.3b builds its own Vector in 3b source, over a fixed
// bbb_os_list_dir buffer; the cap it uses (os/list-dir-max, published as
// a constant on both sides) is honored here too so the two truncate
// identically rather than merely similarly.
static i64
bc_os_list_dir(i64* args, u32 arg_count, Arena* heap, void* userdata) {
  (void)arg_count; (void)heap; (void)userdata;
  Arena*   dst     = (Arena*)(intptr_t)args[0];
  String8* entries = NULL; // dyn array, in `dst` -- IS the returned Vector
  u32      count   = 0;

#if defined(_WIN32)
  // FindFirstFileA wants a `dir\*` pattern, unlike opendir's plain path,
  // so the scratch C string is built one level up from BC_OS_WITH_CSTR's
  // straight copy. Same platform split bbb_os_list_dir already has.
  ArenaTemp temp = arena_temp_begin(ctx_scratch());
  String8   dir  = bc_os_unbox_string(args[1]);
  char*     cdir = bc_os_cstr(temp.arena, dir);
  char*     pattern = push_array(temp.arena, char, dir.size + 3);
  snprintf(pattern, dir.size + 3, "%s\\*", cdir);
  WIN32_FIND_DATAA find_data;
  HANDLE           h = FindFirstFileA(pattern, &find_data);
  arena_temp_end(&temp); // released before the first `dst` allocation, see this file's header
  if (h != INVALID_HANDLE_VALUE) {
    do {
      if (strcmp(find_data.cFileName, ".") == 0 || strcmp(find_data.cFileName, "..") == 0) continue;
      if (count >= BC_OS_LIST_DIR_MAX) break;
      dyn_push(dst, entries, str8_copy(dst, str8_cstring(find_data.cFileName)));
      count += 1;
    } while (FindNextFileA(h, &find_data));
    FindClose(h);
  }
#else
  DIR* d = NULL;
  BC_OS_WITH_CSTR(args[1], cdir, { d = opendir(cdir); });
  if (d) {
    struct dirent* entry;
    while (count < BC_OS_LIST_DIR_MAX && (entry = readdir(d)) != NULL) {
      if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
      dyn_push(dst, entries, str8_copy(dst, str8_cstring(entry->d_name)));
      count += 1;
    }
    closedir(d);
  }
#endif

  return (i64)(intptr_t)entries; // NULL for an empty/missing dir -- an empty Vector
}

// -1, not 0, for a missing path: 0 is a real (if ancient) mtime, and the
// whole point of this verb is "did it change since I last looked". Same
// sentinel bbb_os_file_mtime picks, for the same reason.
static i64
bc_os_file_mtime(i64* args, u32 arg_count, Arena* heap, void* userdata) {
  (void)arg_count; (void)heap; (void)userdata;
  i64 mtime = -1;
  BC_OS_WITH_CSTR(args[0], cpath, {
    struct stat st;
    if (stat(cpath, &st) == 0) mtime = (i64)st.st_mtime;
  });
  return mtime;
}

static i64
bc_os_get_time(i64* args, u32 arg_count, Arena* heap, void* userdata) {
  (void)args; (void)arg_count; (void)heap; (void)userdata;
  return (i64)time(NULL);
}

// Blocks the VM's own thread -- there is no scheduler here to yield to,
// so this is exactly as blocking as native os/sleep is, which is what
// makes the two interchangeable.
static i64
bc_os_sleep_fn(i64* args, u32 arg_count, Arena* heap, void* userdata) {
  (void)arg_count; (void)heap; (void)userdata;
  u32 seconds = (u32)args[0];
#if defined(_WIN32)
  Sleep(seconds * 1000);
#else
  sleep(seconds);
#endif
  return 0; // `void` at the language level -- bcgen.c's CallHost writes every
              // result to a register, and the caller's is simply never read
}

//~ Streams
//
// The VM half of the `stream` primitive type (TypeKind_Stream, 3b.h) -- the
// same verbs native code gets from native_pkgs/os/os.3b, so one source text
// works on either backend.
//
// A VM `stream` register holds a bare `FILE*`, not native codegen's
// `bbb_Stream` (runtime/bbb_file.h): that runtime is embedded TEXT emitted
// into a generated project, never compiled into the `3b` binary, so there's no
// bbb_stream_open to call here. Both are a typedef'd pointer that `(not f)`
// tests directly, which is all the language cares about. The one behavioral
// seam is `error?` -- native keeps a sticky had_error flag, and C stdio's
// `ferror` is sticky the same way, so the observable contract matches.
//
// Every verb matches its os.3b counterpart's SIGNATURE, not just its name,
// `read-line`'s leading `arena` included -- genuinely honored here, since a VM
// `arena` register holds a real base.h `Arena*`, rather than accepted and
// ignored. That's what lets examples/streams/main.3b run unmodified through
// either backend. It's a stricter bar than `os/getenv` met: native takes an
// arena there and the VM's doesn't, a pre-existing divergence left alone
// because translate/build.3bs already wraps that contract.

static FILE*
bc_os_stream(i64 arg) {
  return (FILE*)(intptr_t)arg;
}

// Whether `f` is one of the process's own three standard streams -- those
// are handed out by os/stdout|stderr|stdin below and must survive a
// `close` (closing the real stdout because some helper "owned" it is
// never what a caller meant), matching bbb_stream_close's own no-op.
static b32
bc_os_stream_is_std(FILE* f) {
  return f == stdout || f == stderr || f == stdin;
}

// mode-read/write/append/update, in the order native_pkgs/os/os.3b's own
// `(val mode-* ...)` constants declare them -- and bc_os_constant_decls
// below is what actually publishes those names to a script, so the two
// can't drift. Every one is BINARY, same reasoning os.3b documents: no
// platform silently rewrites the bytes going through.
static const char*
bc_os_stream_mode_string(i64 mode) {
  switch (mode) {
    case 0:  return "rb";
    case 1:  return "wb";
    case 2:  return "ab";
    case 3:  return "r+b";
    default: return NULL; // an out-of-range mode is a failed open, not UB -- see bc_os_open
  }
}

static i64
bc_os_open(i64* args, u32 arg_count, Arena* heap, void* userdata) {
  (void)arg_count; (void)heap; (void)userdata;
  const char* cmode = bc_os_stream_mode_string(args[1]);
  if (!cmode) return 0;
  ArenaTemp temp  = arena_temp_begin(ctx_scratch());
  char*     cpath = bc_os_cstr(temp.arena, bc_os_unbox_string(args[0]));
  FILE*     f     = fopen(cpath, cmode);
  arena_temp_end(&temp);
  return (i64)(intptr_t)f; // NULL on failure -- the null stream `(not f)` tests for
}

static i64
bc_os_stdout_fn(i64* args, u32 arg_count, Arena* heap, void* userdata) {
  (void)args; (void)arg_count; (void)heap; (void)userdata;
  return (i64)(intptr_t)stdout;
}

static i64
bc_os_stderr_fn(i64* args, u32 arg_count, Arena* heap, void* userdata) {
  (void)args; (void)arg_count; (void)heap; (void)userdata;
  return (i64)(intptr_t)stderr;
}

static i64
bc_os_stdin_fn(i64* args, u32 arg_count, Arena* heap, void* userdata) {
  (void)args; (void)arg_count; (void)heap; (void)userdata;
  return (i64)(intptr_t)stdin;
}

static i64
bc_os_close(i64* args, u32 arg_count, Arena* heap, void* userdata) {
  (void)arg_count; (void)heap; (void)userdata;
  FILE* f = bc_os_stream(args[0]);
  if (!f) return 0;
  if (bc_os_stream_is_std(f)) return fflush(f) == 0 ? 1 : 0; // deliberate no-op close, see above
  return fclose(f) == 0 ? 1 : 0;
}

static i64
bc_os_write_string(i64* args, u32 arg_count, Arena* heap, void* userdata) {
  (void)arg_count; (void)heap; (void)userdata;
  FILE*   f    = bc_os_stream(args[0]);
  String8 text = bc_os_unbox_string(args[1]);
  if (!f || text.size == 0) return 0;
  return (i64)fwrite(text.str, 1, text.size, f);
}

// Reads `size` raw bytes from `data` (an `any` -- a bare pointer register,
// typically `(cast any (addr some-array))`), returning how many actually
// got written.
static i64
bc_os_write(i64* args, u32 arg_count, Arena* heap, void* userdata) {
  (void)arg_count; (void)heap; (void)userdata;
  FILE* f    = bc_os_stream(args[0]);
  void* data = (void*)(intptr_t)args[1];
  u64   size = (u64)args[2];
  if (!f || !data || size == 0) return 0;
  return (i64)fwrite(data, 1, size, f);
}

// Reads up to `size` bytes into `out`. A short read (including 0) is
// end-of-file, NOT an error -- at-end?/error? are what tell those apart,
// same contract os.3b's own `read` documents.
static i64
bc_os_read(i64* args, u32 arg_count, Arena* heap, void* userdata) {
  (void)arg_count; (void)heap; (void)userdata;
  FILE* f    = bc_os_stream(args[0]);
  void* out  = (void*)(intptr_t)args[1];
  u64   size = (u64)args[2];
  if (!f || !out || size == 0) return 0;
  return (i64)fread(out, 1, size, f);
}

// One line, WITHOUT its trailing newline (or the \r of a CRLF file).
// Allocates into the CALLER'S arena (`args[0]`, a real `Arena*` -- see
// this section's own note above), not the VM's `heap`, so a read loop
// inside a `(scratch [temp] ...)` block reclaims each line exactly the
// way the identical native code does.
static i64
bc_os_read_line(i64* args, u32 arg_count, Arena* heap, void* userdata) {
  (void)arg_count; (void)heap; (void)userdata;
  Arena* dst = (Arena*)(intptr_t)args[0];
  FILE*  f   = bc_os_stream(args[1]);
  if (!f) return bc_os_box_string(dst, (String8){0});

  // Accumulated DIRECTLY in `dst`, with no ctx_scratch() staging step and
  // no bc_os_box_string copy -- unlike every other primitive in this file.
  // `dst` is very often ctx_scratch() ITSELF (the caller is typically
  // inside a `(scratch [temp] ...)` block, whose arena bcvm.c's
  // BcOp_LoadScratchArena boxes straight off ctx_scratch), so staging
  // through an ArenaTemp on that same arena would pop the returned
  // string's own bytes back off on the way out -- a use-after-free
  // returning freed memory, not merely wasted work.
  u8* line = NULL; // dyn array, in `dst` -- its buffer IS the returned string's bytes
  int c;
  while ((c = fgetc(f)) != EOF && c != '\n') dyn_push(dst, line, (u8)c);
  u64 len = dyn_count(line);
  if (len > 0 && line[len - 1] == '\r') len -= 1;

  String8* header = push_one(dst, String8);
  header->str  = line;
  header->size = len;
  return (i64)(intptr_t)header;
}

static i64
bc_os_flush(i64* args, u32 arg_count, Arena* heap, void* userdata) {
  (void)arg_count; (void)heap; (void)userdata;
  FILE* f = bc_os_stream(args[0]);
  return f && fflush(f) == 0 ? 1 : 0;
}

static i64
bc_os_seek(i64* args, u32 arg_count, Arena* heap, void* userdata) {
  (void)arg_count; (void)heap; (void)userdata;
  FILE* f = bc_os_stream(args[0]);
  if (!f) return 0;
  int whence;
  switch (args[2]) { // seek-start/seek-current/seek-end, see bc_os_constant_decls
    case 0:  whence = SEEK_SET; break;
    case 1:  whence = SEEK_CUR; break;
    case 2:  whence = SEEK_END; break;
    default: return 0;
  }
  return fseek(f, (long)args[1], whence) == 0 ? 1 : 0;
}

static i64
bc_os_tell(i64* args, u32 arg_count, Arena* heap, void* userdata) {
  (void)arg_count; (void)heap; (void)userdata;
  FILE* f = bc_os_stream(args[0]);
  return f ? (i64)ftell(f) : -1; // -1, not 0, for an unseekable stream -- 0 is a real answer here
}

static i64
bc_os_at_end(i64* args, u32 arg_count, Arena* heap, void* userdata) {
  (void)arg_count; (void)heap; (void)userdata;
  FILE* f = bc_os_stream(args[0]);
  return f && feof(f) ? 1 : 0;
}

static i64
bc_os_error(i64* args, u32 arg_count, Arena* heap, void* userdata) {
  (void)arg_count; (void)heap; (void)userdata;
  FILE* f = bc_os_stream(args[0]);
  return !f || ferror(f) ? 1 : 0; // a null stream is itself an error, not a silently-fine one
}

// An arena-owned COPY of `types` -- the BC_OS_PARAMS macro below builds
// each parameter list as a compound literal (automatic storage, dead the
// moment bc_os_primitive_decls returns), and both consumers of that list
// keep the pointer rather than copying it themselves.
static TypeRef*
bc_os_param_list(Arena* arena, TypeRef* types, u32 count) {
  TypeRef* copy = push_array(arena, TypeRef, count);
  foreach_index(i, count) copy[i] = types[i];
  return copy;
}

BcOsPrimitiveDecl*
bc_os_primitive_decls(Arena* arena, u32* out_count) {
  TypeRef string_ty = {0}; string_ty.kind = TypeKind_String;
  TypeRef bool_ty   = {0}; bool_ty.kind   = TypeKind_Bool;
  TypeRef stream_ty = {0}; stream_ty.kind = TypeKind_Stream;
  TypeRef u32_ty    = {0}; u32_ty.kind    = TypeKind_U32;
  TypeRef u64_ty    = {0}; u64_ty.kind    = TypeKind_U64;
  TypeRef i64_ty    = {0}; i64_ty.kind    = TypeKind_I64;
  TypeRef any_ty    = {0}; any_ty.kind    = TypeKind_Any;
  TypeRef arena_ty  = {0}; arena_ty.kind  = TypeKind_Arena;
  TypeRef void_ty   = {0}; void_ty.kind   = TypeKind_Void;

  // `[string]`, os/list-dir's return. The element TypeRef has to be
  // arena-allocated rather than a stack local for the same reason the
  // parameter lists below are: `pointee` is a borrowed pointer that both
  // consumers of this table keep well past this function's return.
  TypeRef* string_elem_ty = push_one(arena, TypeRef);
  *string_elem_ty         = string_ty;
  TypeRef string_vec_ty   = {0};
  string_vec_ty.kind      = TypeKind_Vector;
  string_vec_ty.pointee   = string_elem_ty;

  // Every parameter list below is arena-allocated, NOT a stack local:
  // both consumers (bc_register_os_primitives' table entries, script.c's
  // spliced externs) keep the pointer rather than copying it.
  #define BC_OS_PARAMS(...) \
    bc_os_param_list(arena, (TypeRef[]){ __VA_ARGS__ }, (u32)(sizeof((TypeRef[]){ __VA_ARGS__ }) / sizeof(TypeRef)))

  // Order and signatures mirror native_pkgs/os/os.3b top to bottom; that
  // file is the readable specification of what each of these means.
  BcOsPrimitiveDecl* decls = push_array(arena, BcOsPrimitiveDecl, 25);
  u32                n     = 0;
  decls[n++] = (BcOsPrimitiveDecl){ str8_lit("os/read-file"),    bc_os_read_file,    BC_OS_PARAMS(arena_ty, string_ty), 2, string_ty };
  decls[n++] = (BcOsPrimitiveDecl){ str8_lit("os/write-file"),   bc_os_write_file,   BC_OS_PARAMS(string_ty, string_ty), 2, bool_ty };
  decls[n++] = (BcOsPrimitiveDecl){ str8_lit("os/getenv"),       bc_os_getenv,       BC_OS_PARAMS(arena_ty, string_ty), 2, string_ty };
  decls[n++] = (BcOsPrimitiveDecl){ str8_lit("os/get-time"),     bc_os_get_time,     NULL, 0, i64_ty };
  decls[n++] = (BcOsPrimitiveDecl){ str8_lit("os/sleep"),        bc_os_sleep_fn,     BC_OS_PARAMS(u32_ty), 1, void_ty };
  decls[n++] = (BcOsPrimitiveDecl){ str8_lit("os/list-dir"),     bc_os_list_dir,     BC_OS_PARAMS(arena_ty, string_ty), 2, string_vec_ty };
  decls[n++] = (BcOsPrimitiveDecl){ str8_lit("os/file-mtime"),   bc_os_file_mtime,   BC_OS_PARAMS(string_ty), 1, i64_ty };
  decls[n++] = (BcOsPrimitiveDecl){ str8_lit("os/file-exists"),  bc_os_file_exists,  BC_OS_PARAMS(string_ty), 1, bool_ty   };
  decls[n++] = (BcOsPrimitiveDecl){ str8_lit("os/dir-exists"),   bc_os_dir_exists,   BC_OS_PARAMS(string_ty), 1, bool_ty   };
  decls[n++] = (BcOsPrimitiveDecl){ str8_lit("os/exec-capture"), bc_os_exec_capture, BC_OS_PARAMS(arena_ty, string_ty), 2, string_ty };

  decls[n++] = (BcOsPrimitiveDecl){ str8_lit("os/open"),         bc_os_open,         BC_OS_PARAMS(string_ty, u32_ty), 2, stream_ty };
  decls[n++] = (BcOsPrimitiveDecl){ str8_lit("os/stdout"),       bc_os_stdout_fn,    NULL, 0, stream_ty };
  decls[n++] = (BcOsPrimitiveDecl){ str8_lit("os/stderr"),       bc_os_stderr_fn,    NULL, 0, stream_ty };
  decls[n++] = (BcOsPrimitiveDecl){ str8_lit("os/stdin"),        bc_os_stdin_fn,     NULL, 0, stream_ty };
  decls[n++] = (BcOsPrimitiveDecl){ str8_lit("os/close"),        bc_os_close,        BC_OS_PARAMS(stream_ty), 1, bool_ty   };
  decls[n++] = (BcOsPrimitiveDecl){ str8_lit("os/write"),        bc_os_write,        BC_OS_PARAMS(stream_ty, any_ty, u64_ty), 3, u64_ty };
  decls[n++] = (BcOsPrimitiveDecl){ str8_lit("os/write-string"), bc_os_write_string, BC_OS_PARAMS(stream_ty, string_ty), 2, u64_ty };
  decls[n++] = (BcOsPrimitiveDecl){ str8_lit("os/read"),         bc_os_read,         BC_OS_PARAMS(stream_ty, any_ty, u64_ty), 3, u64_ty };
  decls[n++] = (BcOsPrimitiveDecl){ str8_lit("os/read-line"),    bc_os_read_line,    BC_OS_PARAMS(arena_ty, stream_ty), 2, string_ty };
  decls[n++] = (BcOsPrimitiveDecl){ str8_lit("os/flush"),        bc_os_flush,        BC_OS_PARAMS(stream_ty), 1, bool_ty   };
  decls[n++] = (BcOsPrimitiveDecl){ str8_lit("os/seek"),         bc_os_seek,         BC_OS_PARAMS(stream_ty, i64_ty, u32_ty), 3, bool_ty };
  decls[n++] = (BcOsPrimitiveDecl){ str8_lit("os/tell"),         bc_os_tell,         BC_OS_PARAMS(stream_ty), 1, i64_ty    };
  decls[n++] = (BcOsPrimitiveDecl){ str8_lit("os/at-end?"),      bc_os_at_end,       BC_OS_PARAMS(stream_ty), 1, bool_ty   };
  decls[n++] = (BcOsPrimitiveDecl){ str8_lit("os/error?"),       bc_os_error,        BC_OS_PARAMS(stream_ty), 1, bool_ty   };
  #undef BC_OS_PARAMS

  *out_count = n;
  return decls;
}

BcOsConstantDecl*
bc_os_constant_decls(Arena* arena, u32* out_count) {
  TypeRef u32_ty = {0}; u32_ty.kind = TypeKind_U32;

  // Values (and names) must match native_pkgs/os/os.3b's own `(val
  // mode-* ...)`/`(val seek-* ...)`/`(val list-dir-max ...)` exactly --
  // that's the whole point of publishing them by name rather than letting
  // a script write `1u32`. bc_os_stream_mode_string/bc_os_seek/
  // BC_OS_LIST_DIR_MAX above are what these mean on THIS side, so keep all
  // three in step.
  BcOsConstantDecl* decls = push_array(arena, BcOsConstantDecl, 8);
  decls[0] = (BcOsConstantDecl){ str8_lit("os/list-dir-max"), u32_ty, BC_OS_LIST_DIR_MAX };
  decls[1] = (BcOsConstantDecl){ str8_lit("os/mode-read"),   u32_ty, 0 };
  decls[2] = (BcOsConstantDecl){ str8_lit("os/mode-write"),  u32_ty, 1 };
  decls[3] = (BcOsConstantDecl){ str8_lit("os/mode-append"), u32_ty, 2 };
  decls[4] = (BcOsConstantDecl){ str8_lit("os/mode-update"), u32_ty, 3 };
  decls[5] = (BcOsConstantDecl){ str8_lit("os/seek-start"),   u32_ty, 0 };
  decls[6] = (BcOsConstantDecl){ str8_lit("os/seek-current"), u32_ty, 1 };
  decls[7] = (BcOsConstantDecl){ str8_lit("os/seek-end"),     u32_ty, 2 };
  *out_count = 8;
  return decls;
}

void
bc_register_os_primitives(BcHostImportTable* table, Arena* arena) {
  u32                 count;
  BcOsPrimitiveDecl* decls = bc_os_primitive_decls(arena, &count);
  foreach_index(i, count) {
    bc_host_import_table_add(table, arena, decls[i].qualified_name, decls[i].fn,
                              decls[i].param_types, decls[i].param_count, decls[i].return_type);
  }
}
