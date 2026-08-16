////////////////////////////////
//~ OS-facing primitives (environment, wall-clock time) -- bbb_file.h's own
//~ File I/O section (bbb_os_file_read) is this family's other member,
//~ kept in its own file since it predates this one.

// Returns the named environment variable's value, copied into `arena`.
// `{0}` (str==NULL, size==0) for an unset variable -- same "empty means
// unset" convention bcosprims.c's bytecode-side `os/getenv` already uses;
// no second return value needed to distinguish "unset" from "set empty".
bbb_String8 bbb_os_getenv(bbb_Arena arena, bbb_String8 name);

// Runs `cmd` through the platform shell and returns everything it wrote
// to STDOUT, copied into `arena`, with a single trailing newline (or
// \r\n) trimmed -- matching what a POSIX shell's own `$(cmd)` command
// substitution already strips, since that's the convention a caller
// reaching for this is expecting.
//
// STDERR is NOT captured: it passes straight through to this process's
// own stderr, so a failing command still reports itself somewhere a user
// can see. Neither is the exit status -- a command that fails and prints
// nothing is indistinguishable from one that succeeds silently, both
// `{0}`. Both are real limits: widening the return to carry them would
// have to widen the bytecode-VM side (bcosprims.c) in lockstep. Check for
// an empty result if that distinction matters.
//
// `cmd` is SHELL-INTERPRETED. Never build one out of untrusted input.
bbb_String8 bbb_os_exec_capture(bbb_Arena arena, bbb_String8 cmd);

// Seconds since the Unix epoch (plain libc `time(NULL)`) -- wall-clock,
// NOT monotonic; don't use this to measure an interval that must never
// go backward across an NTP adjustment/DST change.
i64 bbb_os_time_unix(void);

// Blocks the CALLING thread for `seconds` (POSIX `sleep`/Windows `Sleep`
// under the hood -- see bbb_os.c). Whole seconds only, no sub-second
// precision -- a finer-grained sleep would need its own primitive.
void bbb_os_sleep(u32 seconds);
