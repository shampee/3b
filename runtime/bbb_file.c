////////////////////////////////
//~ File I/O

// Plain ANSI C stdio (fopen/fseek/ftell/fread), not a raw POSIX fd
// (open/fstat/read/close) -- identical on Linux/Mac/Windows with no
// platform branch at all. The one real cost: `ftell` returns a plain
// `long`, 32 bits under Windows' LLP64 model, capping a single
// `os_file_read` at ~2GB there (Linux/Mac's 64-bit `long` has no such
// cap) -- an acceptable trade for staying branch-free given what this
// actually reads in practice (shader source, small data files).
bbb_String8
bbb_os_file_read(bbb_Arena arena, bbb_String8 path) {
  char* cpath = bbb_cstring_str8(bbb_ctx_scratch(), path);
  FILE* f     = fopen(cpath, "rb");
  if (!f) return (bbb_String8){0};

  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return (bbb_String8){0}; }
  long file_size = ftell(f);
  if (file_size <= 0 || fseek(f, 0, SEEK_SET) != 0) {
    fclose(f);
    return (bbb_String8){0};
  }
  u64 size = (u64)file_size;

  u8* dst        = bbb_push_array(&arena, u8, size);
  u64 total_read = fread(dst, 1, size, f);
  fclose(f);
  if (total_read != size) return (bbb_String8){0};

  return (bbb_String8){ .str = dst, .size = size };
}

// Plain ANSI C stdio (fopen/fwrite/fclose), same portability rationale as
// bbb_os_file_read above -- "wb" so no platform ever silently munges the
// bytes going out (e.g. Windows' text-mode \n -> \r\n translation).
b32
bbb_os_file_write(bbb_String8 path, bbb_String8 contents) {
  char* cpath = bbb_cstring_str8(bbb_ctx_scratch(), path);
  FILE* f     = fopen(cpath, "wb");
  if (!f) return 0;

  u64 written = contents.size > 0 ? fwrite(contents.str, 1, contents.size, f) : 0;
  fclose(f);
  return written == contents.size;
}

// Win32 (FindFirstFileA/FindNextFileA) vs POSIX (opendir/readdir) --
// same platform split bbb_os_sleep (bbb_os.c) already has, dirent.h
// pulled in specifically for this (see codegen.c's cg_write_runtime_source,
// windows.h is already unconditionally included for _WIN32 by
// cg_write_runtime_header, ahead of this function's own concatenation).
u32
bbb_os_list_dir(bbb_Arena arena, bbb_String8 dir, bbb_String8* out, u32 max) {
  char* cpath = bbb_cstring_str8(bbb_ctx_scratch(), dir);
  u32   count = 0;
#if defined(_WIN32)
  char pattern[1024];
  snprintf(pattern, sizeof(pattern), "%s\\*", cpath);
  WIN32_FIND_DATAA find_data;
  HANDLE h = FindFirstFileA(pattern, &find_data);
  if (h == INVALID_HANDLE_VALUE) return 0;
  do {
    if (strcmp(find_data.cFileName, ".") == 0 || strcmp(find_data.cFileName, "..") == 0) continue;
    if (count >= max) break;
    out[count] = bbb_str8_copy(&arena, bbb_str8_cstring(find_data.cFileName));
    count += 1;
  } while (FindNextFileA(h, &find_data));
  FindClose(h);
#else
  DIR* d = opendir(cpath);
  if (!d) return 0;
  struct dirent* entry;
  while (count < max && (entry = readdir(d)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
    out[count] = bbb_str8_copy(&arena, bbb_str8_cstring(entry->d_name));
    count += 1;
  }
  closedir(d);
#endif
  return count;
}

// `stat` + S_ISREG/S_ISDIR everywhere, including Windows: mingw's own
// <sys/stat.h> provides both, which is why bbb_os_file_mtime just below
// needs no platform branch either. The S_IS* macros aren't in mingw's
// header under every configuration, so both are spelled out against
// S_IFMT rather than assumed.
b32
bbb_os_file_exists(bbb_String8 path) {
  char*       cpath = bbb_cstring_str8(bbb_ctx_scratch(), path);
  struct stat st;
  if (stat(cpath, &st) != 0) return 0;
  return (st.st_mode & S_IFMT) == S_IFREG;
}

b32
bbb_os_dir_exists(bbb_String8 path) {
  char*       cpath = bbb_cstring_str8(bbb_ctx_scratch(), path);
  struct stat st;
  if (stat(cpath, &st) != 0) return 0;
  return (st.st_mode & S_IFMT) == S_IFDIR;
}

i64
bbb_os_file_mtime(bbb_String8 path) {
  char*      cpath = bbb_cstring_str8(bbb_ctx_scratch(), path);
  struct stat st;
  if (stat(cpath, &st) != 0) return -1;
  return (i64)st.st_mtime;
}

////////////////////////////////
//~ Streams
//
// Same plain-ANSI-C-stdio choice the whole-file helpers above already
// made (see bbb_os_file_read's own comment for why: no platform branch at
// all). The struct stays private to this file -- bbb_file.h publishes
// only the incomplete `bbb_StreamImpl` type, so `FILE*` is not part of
// the language's own surface and a later non-file stream kind can be
// added by growing THIS struct instead of every call site.
//
// `owned` is the whole reason this is a struct rather than a bare FILE*
// typedef: the three std streams must survive a `close` (see
// bbb_stream_close), and a flag here is cheaper to reason about than
// three pointer comparisons at every entry point.
struct bbb_StreamImpl {
  FILE* file;
  b32   owned; // false for stdout/stderr/stdin -- close is a no-op on those
};

bbb_Stream
bbb_stream_open(bbb_String8 path, u32 mode) {
  char* cpath = bbb_cstring_str8(bbb_ctx_scratch(), path);
  char* cmode = NULL;
  switch (mode) {
    case bbb_StreamMode_Read:   cmode = "rb";  break;
    case bbb_StreamMode_Write:  cmode = "wb";  break;
    case bbb_StreamMode_Append: cmode = "ab";  break;
    case bbb_StreamMode_Update: cmode = "r+b"; break;
    default: return NULL; // unknown mode -- same NULL as a failed open, nothing to distinguish
  }

  FILE* f = fopen(cpath, cmode);
  if (!f) return NULL;

  // malloc, not an arena push: a stream's lifetime is the caller's
  // explicit open/close pair, which has nothing to do with any arena's
  // scope -- and an arena can't reclaim one slot anyway, so a program
  // opening and closing files in a loop would grow without bound. `free`
  // in bbb_stream_close is the matching half.
  bbb_StreamImpl* s = (bbb_StreamImpl*)malloc(sizeof(bbb_StreamImpl));
  if (!s) { fclose(f); return NULL; }
  s->file  = f;
  s->owned = 1;
  return s;
}

// One shared static per std stream, lazily filled -- see bbb_file.h on
// why these can't be handed out as fresh allocations. Not thread-safe to
// FIRST touch from two lanes at once (the writes race), but the race is
// benign in practice: both lanes write identical values to the same two
// fields. Anything stricter would need an atomic/once here, which the
// rest of this runtime deliberately doesn't reach for.
static bbb_StreamImpl bbb_stream_std[3];

static bbb_Stream
bbb_stream_std_get(u32 slot, FILE* file) {
  bbb_StreamImpl* s = &bbb_stream_std[slot];
  s->file  = file;
  s->owned = 0;
  return s;
}

bbb_Stream bbb_stream_stdout(void) { return bbb_stream_std_get(0, stdout); }
bbb_Stream bbb_stream_stderr(void) { return bbb_stream_std_get(1, stderr); }
bbb_Stream bbb_stream_stdin(void)  { return bbb_stream_std_get(2, stdin);  }

b32
bbb_stream_close(bbb_Stream s) {
  if (!s) return 0;
  if (!s->owned) return 1; // a std stream -- deliberately NOT closed, see bbb_file.h
  // fclose flushes on its own and reports a failed flush through its own
  // return value, so there's no separate fflush here -- one call, one
  // answer.
  b32 ok = (fclose(s->file) == 0);
  free(s);
  return ok;
}

u64
bbb_stream_write(bbb_Stream s, void* data, u64 size) {
  if (!s || size == 0) return 0;
  return (u64)fwrite(data, 1, size, s->file);
}

u64
bbb_stream_write_string(bbb_Stream s, bbb_String8 text) {
  return bbb_stream_write(s, text.str, text.size);
}

u64
bbb_stream_read(bbb_Stream s, void* out, u64 size) {
  if (!s || size == 0) return 0;
  return (u64)fread(out, 1, size, s->file);
}

bbb_String8
bbb_stream_read_line(bbb_Arena arena, bbb_Stream s) {
  if (!s) return (bbb_String8){0};

  // Grown with malloc/realloc rather than pushed into an arena as it
  // goes: a bump allocator can't resize its last allocation in place, so
  // an arena-grown line would leave every intermediate size behind as
  // garbage -- and the obvious fix (grow in ctx_scratch, copy out) breaks
  // outright when the CALLER passes ctx_scratch as `arena`. This owns its
  // buffer end to end and copies the finished line into `arena` once.
  u64   cap  = 128;
  u64   len  = 0;
  char* buf  = (char*)malloc(cap);
  if (!buf) return (bbb_String8){0};

  int c;
  while ((c = fgetc(s->file)) != EOF && c != '\n') {
    if (len + 1 > cap) {
      cap *= 2;
      char* grown = (char*)realloc(buf, cap);
      if (!grown) { free(buf); return (bbb_String8){0}; }
      buf = grown;
    }
    buf[len] = (char)c;
    len += 1;
  }

  if (len == 0 && c == EOF) { free(buf); return (bbb_String8){0}; }
  // A CRLF file read through a "rb" stream keeps its \r -- strip it here
  // so line content is the same on every platform (the whole point of
  // opening in binary mode is that nothing ELSE gets rewritten silently).
  if (len > 0 && buf[len - 1] == '\r') len -= 1;

  u8* dst = len > 0 ? bbb_push_array(&arena, u8, len) : NULL;
  if (len > 0) bbb_MemoryCopy(dst, buf, len);
  free(buf);
  return (bbb_String8){ .str = dst, .size = len };
}

b32
bbb_stream_flush(bbb_Stream s) {
  if (!s) return 0;
  return (fflush(s->file) == 0);
}

b32
bbb_stream_seek(bbb_Stream s, i64 offset, u32 origin) {
  if (!s) return 0;
  int whence;
  switch (origin) {
    case bbb_StreamOrigin_Start:   whence = SEEK_SET; break;
    case bbb_StreamOrigin_Current: whence = SEEK_CUR; break;
    case bbb_StreamOrigin_End:     whence = SEEK_END; break;
    default: return 0;
  }
  // `long`, so a seek past 2GB is unreachable under Windows' LLP64 model
  // -- the same cap bbb_os_file_read's own ftell already documents, and
  // the same trade (no platform branch) is being made here.
  return (fseek(s->file, (long)offset, whence) == 0);
}

i64
bbb_stream_tell(bbb_Stream s) {
  if (!s) return -1;
  long pos = ftell(s->file);
  return pos < 0 ? -1 : (i64)pos;
}

b32
bbb_stream_at_end(bbb_Stream s) {
  if (!s) return 1; // nothing to read from a null stream -- "at end" is the honest answer
  return (feof(s->file) != 0);
}

b32
bbb_stream_had_error(bbb_Stream s) {
  if (!s) return 1;
  return (ferror(s->file) != 0);
}

i32
bbb_stream_printf(bbb_Stream s, char* fmt, ...) {
  if (!s) return -1;
  va_list args;
  va_start(args, fmt);
  int written = vfprintf(s->file, fmt, args);
  va_end(args);
  return (i32)written;
}
