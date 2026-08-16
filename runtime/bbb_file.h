////////////////////////////////
//~ File I/O

// Whole-file read into `arena`. Returns `{0}` (str==NULL) on any failure.
bbb_String8 bbb_os_file_read(bbb_Arena arena, bbb_String8 path);

// Whole-buffer write to `path`, creating it if missing and overwriting it
// otherwise. Returns false on any failure (can't open, short write) --
// no arena needed, nothing here allocates.
b32 bbb_os_file_write(bbb_String8 path, bbb_String8 contents);

// Every entry's bare filename (not the full path, "." and ".." skipped)
// directly inside `dir`, arena-copied into `out` (already-allocated,
// capacity `max`) so nothing references the OS's own transient listing
// buffer. Order is whatever the OS returns, unspecified. Entries beyond
// `max` are silently dropped. Returns the actual count written -- 0 for
// a missing/unreadable/empty directory, same "0 means nothing"
// convention bbb_os_file_read's own empty-string failure result uses.
u32 bbb_os_list_dir(bbb_Arena arena, bbb_String8 dir, bbb_String8* out, u32 max);

// Whether `path` names an existing REGULAR file -- false for a directory,
// for a missing path, and for anything unreadable enough that `stat`
// itself fails. Two separate predicates rather than one returning a kind:
// callers know which of the two they want, and a kind enum would need its
// own type in the language surface.
b32 bbb_os_file_exists(bbb_String8 path);

// Whether `path` names an existing DIRECTORY -- the counterpart to
// bbb_os_file_exists above, false for a regular file.
b32 bbb_os_dir_exists(bbb_String8 path);

// `path`'s own last-modified time, seconds since the Unix epoch (`stat`'s
// st_mtime, portable to Windows via mingw's own <sys/stat.h> -- no #if
// needed, see codegen.c's cg_write_runtime_source for where this header
// gets included from). -1 for a missing/unstat-able path -- deliberately
// NOT 0 (0 is a real, if ancient, mtime; a caller comparing "did this
// change since last poll" would misread a missing-then-reappearing file
// as unchanged if the sentinel could collide with a real value).
i64 bbb_os_file_mtime(bbb_String8 path);

////////////////////////////////
//~ Streams
//
// The incremental counterpart to the whole-file read/write pair above --
// what `bbb_os_file_read` can't express: writing a 100MB video frame dump
// without first materializing all of it in an arena, or reading a file
// bigger than memory a chunk at a time.
//
// `bbb_Stream` backs the language's own `stream` PRIMITIVE type
// (TypeKind_Stream, 3b.h) -- like `arena`, it's copied by value and never
// appears as `stream*` at the language level. Unlike `bbb_Arena` (a
// two-pointer struct) this is a plain typedef'd POINTER, deliberately:
// generated code has to be able to say `!s` for a failed open, and 3b's
// `not`/`if`/`and`/`or` all lower to C truthiness on the operand
// (checker.c's TypedNodeKind_LogicalNot accepts any type and codegen
// emits a bare `!`) -- a struct-by-value wouldn't compile there. The
// pointee stays incomplete on purpose: `bbb_StreamImpl` is defined only
// in bbb_file.c, so neither generated code nor any FFI neighbor can
// reach past this handle at the FILE* inside.
typedef struct bbb_StreamImpl bbb_StreamImpl;
typedef bbb_StreamImpl*       bbb_Stream;

// `mode` for bbb_stream_open. Values are the language's own
// `os/mode-read`/`os/mode-write`/`os/mode-append`/`os/mode-update`
// constants (native_pkgs/os/os.3b) -- deliberately a small u32 enum
// rather than fopen's mode STRING, so a typo is a compile error in 3b
// instead of a NULL at runtime. Every one of these opens in BINARY mode
// ("rb"/"wb"/...): no platform ever silently munges the bytes going
// through, same reasoning bbb_os_file_write's own "wb" already has.
enum {
  bbb_StreamMode_Read   = 0, // "rb"  -- must already exist
  bbb_StreamMode_Write  = 1, // "wb"  -- created/truncated
  bbb_StreamMode_Append = 2, // "ab"  -- created if missing, writes go to the end
  bbb_StreamMode_Update = 3, // "r+b" -- read AND write, must already exist
};

// `origin` for bbb_stream_seek -- same three C stdio whence values, named
// so generated code never has to spell SEEK_SET/SEEK_CUR/SEEK_END.
enum {
  bbb_StreamOrigin_Start   = 0,
  bbb_StreamOrigin_Current = 1,
  bbb_StreamOrigin_End     = 2,
};

// Opens `path` in `mode`. Returns NULL on any failure (missing file,
// permission denied, bad mode) -- the "empty means failure" convention
// bbb_os_file_read's own `{0}` result already uses, in the shape a
// pointer can express. The returned stream owns a small heap allocation
// that bbb_stream_close frees; leaking one leaks that plus the FILE*.
bbb_Stream bbb_stream_open(bbb_String8 path, u32 mode);

// The process's own three standard streams. Each is a single shared,
// lazily-initialized static -- calling stdout() twice hands back the SAME
// stream, so a caller can't accidentally close one copy out from under
// another. bbb_stream_close on any of them is a no-op that returns true
// (see its own comment): closing the process's stdout because a helper
// function "owned" a stream is never what the caller meant.
bbb_Stream bbb_stream_stdout(void);
bbb_Stream bbb_stream_stderr(void);
bbb_Stream bbb_stream_stdin(void);

// Flushes and closes `s`, freeing it. Returns false if the final flush
// failed (a full disk shows up HERE, not at the write that filled the
// buffer) -- so a caller that cares about durability must check this, not
// just the writes. Safe on NULL (returns false) and on a std stream (a
// no-op returning true, see above). Using `s` after this is a
// use-after-free, exactly like any other freed pointer.
b32 bbb_stream_close(bbb_Stream s);

// Writes `size` raw bytes from `data`. Returns the byte count actually
// written -- equal to `size` on success, less (usually 0) on failure;
// unlike the b32 whole-file bbb_os_file_write, a partial write is real
// information to a caller streaming a frame out. 0 for a NULL stream.
u64 bbb_stream_write(bbb_Stream s, void* data, u64 size);

// Writes a counted string's bytes exactly (no nul, no newline appended) --
// `bbb_stream_write(s, text.str, text.size)` with the unpacking done
// here, since `string` is the type 3b code actually holds.
u64 bbb_stream_write_string(bbb_Stream s, bbb_String8 text);

// Reads up to `size` bytes into `out`. Returns the count actually read --
// short (including 0) at end-of-file, which is NOT an error; use
// bbb_stream_at_end/bbb_stream_had_error to tell a clean EOF from a real
// read failure.
u64 bbb_stream_read(bbb_Stream s, void* out, u64 size);

// Reads one line into `arena`, WITHOUT its trailing newline (a trailing
// \r is stripped too, so a CRLF file read on Linux doesn't leave a stray
// \r on every line). Returns `{0}` (str==NULL, size 0) at end-of-file or
// on error -- which a genuinely EMPTY line also produces, since both are
// zero bytes of content; a reader that must tell them apart drives the
// loop with bbb_stream_at_end rather than by testing the result. Lines of
// any length work (the buffer grows), but every line lands in `arena`
// with no way to reclaim just one -- read a big file inside a scratch
// scope, not into a permanent arena.
bbb_String8 bbb_stream_read_line(bbb_Arena arena, bbb_Stream s);

// Pushes buffered writes out to the OS. Returns false on failure. Note
// this is the OS's problem after that -- it does NOT force a physical
// disk sync (no fsync here).
b32 bbb_stream_flush(bbb_Stream s);

// Moves the read/write cursor to `offset` bytes from `origin` (one of the
// bbb_StreamOrigin_* values above). Returns false if the stream isn't
// seekable (a pipe, a terminal) or the target is invalid.
b32 bbb_stream_seek(bbb_Stream s, i64 offset, u32 origin);

// The cursor's current byte offset from the start, or -1 if the stream
// isn't seekable -- the same -1-not-0 sentinel choice bbb_os_file_mtime
// makes, and for the same reason (0 is a real, valid answer here).
i64 bbb_stream_tell(bbb_Stream s);

// Whether a previous read already hit end-of-file. C stdio semantics:
// this only goes true AFTER a read that came up short, never
// speculatively, so `while (!at_end) read()` runs one extra empty
// iteration at the end rather than stopping early.
b32 bbb_stream_at_end(bbb_Stream s);

// Whether any operation on this stream has failed since it was opened --
// the sticky error flag C stdio already keeps. Lets a caller write a
// whole batch of writes unchecked and ask ONCE at the end, instead of
// branching per write.
b32 bbb_stream_had_error(bbb_Stream s);

// Formatted write -- what backs `(print s "{} {}" a b)`/`(println s ...)`
// in generated code (codegen.c's cg_call print case, which synthesizes
// `fmt` from the `{}` template exactly as it does for the stdout-bound
// `printf` form). A real varargs function rather than a `#define` to
// fprintf specifically so the FILE* stays hidden behind bbb_StreamImpl,
// and so a future non-file stream kind (memory, socket) can dispatch here
// without every generated call site changing. Returns the byte count
// written, or a negative value on failure -- fprintf's own contract.
i32 bbb_stream_printf(bbb_Stream s, char* fmt, ...);
