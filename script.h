#ifndef SCRIPT_H
#define SCRIPT_H
#include "3b.h"
#include "bytecode.h"
#include "bcvm.h"
#include "bcio.h" // BcLoadResult -- see ScriptSlot's own comment on cache_mapping

// The `.3bs` driver: parse -> lower -> check -> bc_compile_program ->
// bc_run_in_program for a script file, splicing in known embedded modules
// (g_known_modules in script.c) for any `(import name)` form found. `main` is
// the entry point, in either shape a
// native 3b program's own may have -- zero parameters, or `[argc i32 argv
// string*]`, which gets a one-entry argv holding the program's own path.
//
// A `.3b` file names a PACKAGE, not just itself: its same-package siblings in
// the same directory are compiled with it, exactly as `3b <package-dir>`
// compiles them (see script.c's script_compile_unit). A `.3bs` script is
// always the one file named.
//
// `host_imports` may be NULL for a script that makes no host calls at all,
// which is rare: `(import build)` and `(import os)` both resolve to registered
// host imports.
//
// Returns false (with a diagnostic already printed, via diag_error for a
// parse/lower/check failure or a plain fprintf for anything else) on
// failure; `out_result` is only meaningful when this returns true.
//
// A script that RAN but hit a runtime trap (BcResult.trapped -- a division
// by zero, say) also counts as a failure here, and prints what happened
// before returning: it stopped partway through whatever it was doing, which
// is not something a caller should be able to mistake for success.
b32 script_run_file(const char* path_cstr, BcHostImportTable* host_imports, BcResult* out_result);

////////////////////////////////
//~ Hot-reloadable script embedding
//
// The above (script_run_file) is one-shot: compile, call `main`, done. This
// second API is for a HOST PROGRAM (typically a game's own main loop) that
// wants to keep a `.3b`/`.3bs` file loaded across many frames, call
// arbitrary named functions in it (not just `main`), and pick up edits to the
// file without restarting. That serves both iterative gameplay-script
// development and -- same mechanism, polled less often or not at all --
// shippable, moddable gameplay scripting.
//
// STATE MODEL: the host owns ALL persistent game state. A script never
// holds a struct/pointer to that state directly -- it receives HANDLES as
// arguments (the same generation-checked bbb_HandlePool convention this
// project already uses for e.g. `Mesh^`) and mutates host state only by
// calling host-registered functions (BcHostImportTable, already how OS
// primitives get exposed to scripts) that take those handles. A handle
// value is a plain packed i64, so it crosses the BcHostFn args boundary
// with zero new plumbing -- see BcHostFn's own bytecode.h comment. This
// sidesteps the hard part of hot-reloading (preserving arbitrary script-
// owned global state across a reload that may have changed its shape)
// entirely: reloading a script just re-runs its OWN `#init_globals` (its
// own constants), never anything belonging to the host.
//
// A ScriptHandle here is a SEPARATE concept from that: it identifies a
// loaded SCRIPT (a `ScriptTable` slot holding its compiled BcProgram), not
// a piece of game data. Index 0 is permanently reserved/dead, so a zero-
// initialized `ScriptHandle` is always the null handle, matching this
// project's usual handle convention.
//
// SHIPPING PRECOMPILED BYTECODE: script_load/script_poll_reload both try a
// cached, mmap-loaded BcProgram (bcio.c's bc_program_load) BEFORE ever
// parsing/checking/compiling from source -- see load_or_compile_script in
// script.c. A source file has exactly ONE cache file, its own path with the
// extension replaced by `.3bc` (cache_path_for); staleness is decided by the
// content hash stored inside it, not by the name, and a recompile OVERWRITES
// it rather than accumulating a file per edit.
//
// That is what makes a shipped build fast to boot: a build step precompiles
// every script once (by running script_load against the shipped source) and
// ships the resulting `.3bc` files alongside it, and the game's own
// script_load calls pick them up with no code changes, recompiling and
// caching anew for anything precompilation missed -- a modder's added script,
// say. A cache MISS (missing file, I/O error, bad magic, or version mismatch,
// which bcio.h's doc says to treat identically) falls back to compiling from
// source, then best-effort writes a fresh cache so the next load hits.

typedef struct ScriptHandle { u32 index; } ScriptHandle;

typedef struct ScriptSlot {
  b32          alive;        // false for slot 0 (the permanent null-handle slot); a real script's
                                // slot never goes back to false -- see script_unload's own comment
  String8      path;
  u64          content_hash; // bc_content_hash of the source last successfully compiled into `prog`
                                // -- what script_poll_reload compares a fresh read against. It does NOT
                                // name the cache file: that name comes from `path` alone (see above), and
                                // this is instead what the hash stored INSIDE a `.3bc` is checked against
                                // to decide whether that file is stale
  BcProgram    prog;
  BcLoadResult cache_mapping; // {0} (mapped_file.view.data == NULL, ok == false) when `prog` was
                                // compiled fresh rather than mmap-loaded from a cache file --
                                // bc_program_unload is a documented-safe no-op on that zero value,
                                // so script_poll_reload/script_unload can call it UNCONDITIONALLY
                                // on the outgoing slot contents without needing to track a separate
                                // "was this from cache" flag.
} ScriptSlot;

// Explicit, caller-owned table of loaded scripts -- same "no hidden global
// state" convention BcHostImportTable/BcProgram already establish elsewhere
// in this codebase. Zero-initialize before first use (`slots` is a dyn
// array and starts NULL, same as e.g. script.c's own `CompiledModule**
// registry`).
typedef struct ScriptTable {
  ScriptSlot* slots; // dyn array; index 0 reserved as the permanent null-handle slot, see above
} ScriptTable;

// Compiles `path` into a fresh slot of `table`, returning a handle to it.
// Unlike script_run_file, this does NOT require (or call) a `main` function
// -- see script_call for how a loaded script's functions get invoked
// instead, by name, whichever ones the host wants to call. `host_imports`
// is threaded through exactly like script_run_file's own parameter (every
// host-registered function a script calls, including any handle-taking
// state mutators, must already be registered here) -- may be NULL only if
// this script (and everything it imports) makes no host calls at all.
// Returns false (diagnostic already printed) on any read/parse/lower/
// check/compile failure; `*out_handle` is only meaningful when this
// returns true.
b32 script_load(ScriptTable* table, String8 path, BcHostImportTable* host_imports, ScriptHandle* out_handle);

// Calls the function named `fn_name` inside the script at `handle`, passing
// `args`/`argc` exactly as bc_run_in_program's own parameters (`heap` is
// that same call's heap parameter -- see bcvm.h's doc comment on its
// lifetime: anything the call returns/stores must not outlive `heap`).
// Returns false (diagnostic printed, `*out_result` untouched) if `handle`
// is stale/invalid, `fn_name` doesn't exist in this script, or its param
// count doesn't match `argc` -- deliberately never reaches
// bc_program_find_fn/bc_run_in_program's own assert-on-mismatch path (a
// mismatch here is a normal "the script doesn't define that" outcome, not
// an embedding-program bug those asserts are meant to catch).
//
// Also false, with the reason printed, if the call TRAPPED (BcResult.trapped)
// -- it delivered no value, and a host that polls a script every frame must
// not read that as the number 0. `*out_result` IS written in that one case,
// so a host that wants to react (unload the script, fall back to a default)
// can inspect trap_message/trap_fn; see bcvm.h on how long those stay valid.
b32 script_call(ScriptTable* table, ScriptHandle handle, String8 fn_name, i64* args, u32 argc,
                 Arena* heap, BcHostImportTable* host_imports, BcResult* out_result);

// Re-reads `handle`'s own source file from disk and, ONLY if its content
// hash differs from what's currently compiled (bc_content_hash, same
// mechanism bcio.c's own disk cache uses to detect staleness), recompiles
// it and swaps the new BcProgram into `handle`'s slot IN PLACE -- so any
// copy of this ScriptHandle held elsewhere (the host's own game-object
// table, say) keeps calling script_call exactly as before and transparently
// starts running the new code, with no re-fetch required anywhere.
//
// On a recompile FAILURE (a real parse/check/compile error in the edited
// file), the OLD, still-working BcProgram is left running untouched
// (diagnostic printed to stderr) -- a syntax error mid-keystroke must never
// tear down an otherwise-live game session. Returns true iff a reload
// actually HAPPENED; false covers both "source unchanged" and "source
// changed but failed to recompile" (check stderr to tell those apart).
// Cheap to call every frame for every loaded script (one file read + one
// hash, skipping recompilation entirely on the overwhelmingly common
// "nothing changed" case) -- that's the intended usage for iterative dev;
// a shipped, non-moddable build simply never calls this at all.
b32 script_poll_reload(ScriptTable* table, ScriptHandle handle, BcHostImportTable* host_imports);

// Marks `handle`'s slot dead, invalidating it (and every other outstanding
// copy of this same ScriptHandle) for script_call/script_poll_reload,
// which both fail cleanly (false, diagnostic) rather than run stale state
// afterward. Does NOT reclaim `handle`'s slot for reuse by a future
// script_load -- table->slots only ever grows, matching this codebase's
// existing dyn-array convention (nothing here calls a "pop" this project
// has no equivalent of) -- fine in practice since a game loads a small,
// fixed set of mod/gameplay scripts, not a high-churn stream of them.
void script_unload(ScriptTable* table, ScriptHandle handle);

#endif
