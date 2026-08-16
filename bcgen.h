#ifndef BCGEN_H
#define BCGEN_H
#include "3b.h"
#include "bytecode.h"
#include "layout.h"

// name -> chunk-index table, built by bc_compile_program's first pass
// (gather every compilable top-level fn's name/index before compiling any
// body) so a Call node resolves its callee regardless of declaration
// order -- forward references and (mutual) recursion both just work.
// Populated once per program compile, read-only from every
// bc_compile_function call after that.
typedef struct BcFnEntry {
  String8 name;
  u32     index; // into the eventual BcProgram.chunks
} BcFnEntry;

typedef struct BcFnTable {
  BcFnEntry* entries; // dyn array
} BcFnTable;

// Compiles ONE FunctionDecl typed node into a BcChunk -- mirrors codegen.c's
// cg_expr/cg_function in SHAPE (same TypedNodeKind switch, same recursive
// descent, consuming the same Checker/resolved_types the native backend
// does), but emits register-machine bytecode instead of C text. See
// bcgen.c's own top-of-file note for this slice's scope. `arena` backs the
// returned chunk's code/consts arrays. `layout_cache` is the caller's own
// LayoutCache (see layout.h) -- share ONE across multiple
// bc_compile_function calls in the same compile so a struct's layout isn't
// recomputed per function. `fn_table` resolves any TypedNodeKind_Call
// this function's body makes to another compiled-3bscript function by
// name (bc_compile_program handles populating this automatically; calling
// this directly for an isolated function with no calls can pass an empty
// table). `global_table` resolves an Identifier/`set`-target this
// function's body references that ISN'T a local to a module-level `var`/
// `val`'s slot (bc_compile_program's own gather pass populates this the
// same way `fn_table` is populated -- reuses the identical BcFnEntry{name,
// index} shape, "index" just means a global slot instead of a chunk index
// here) -- may be NULL/empty if this function references no globals.
// `host_imports` resolves a call to anything NOT found in `fn_table` --
// conventionally a body-less `(extern (fn ...))` declaration on the
// 3bscript side (see BcHostImportTable's own comment in bytecode.h) --
// may be NULL if this function makes no host calls. `handle_pool_table`
// is the SAME BcFnEntry{name,index} shape again, resolving `(handle
// Name)`-declared pooled struct names to their own slot in
// BcProgram.globals -- may be NULL/empty if this function touches no
// handle pools. `module_table` resolves a QUALIFIED call name
// ("build/getenv") to a function living in a DIFFERENT, already-
// independently-compiled BcProgram -- a real cross-package `.3bs` import
// (see BcModuleTable's own bytecode.h comment) -- may be NULL/empty if
// this function makes no cross-module calls. `addr_taken_names` is a
// name -> non-NULL set, computed ONCE for the whole program (bc_compile_
// program's own bc_scan_address_taken_names call, not per-function) --
// may be NULL if nothing in the whole program ever takes `(addr x)`/`&x`
// of a plain identifier; consulted by bc_bind_local_typed to decide
// whether an otherwise-scalar local needs a real backing memory slot.
BcChunk bc_compile_function(Checker* ck, TypedAst* tast, TypedIndex func_idx, Arena* arena,
                             LayoutCache* layout_cache, BcFnTable* fn_table, BcFnTable* global_table,
                             BcFnTable* handle_pool_table, BcHostImportTable* host_imports,
                             BcModuleTable* module_table, HashTable* addr_taken_names);

// Compiles every top-level FunctionDecl with a body (skipping `extern`
// signatures -- nothing to compile, but see BcHostImportTable for why an
// `(extern (fn ...))` is still a meaningful call TARGET) reachable from `root`'s
// top-level Block into one BcProgram, resolving calls between them by
// index. Two passes: gather every fn's name -> (future) chunk index
// first, then compile each body -- see BcFnTable's own comment for why.
// `host_imports` may be NULL if this program makes no host calls at all;
// if non-NULL, this ALSO calls bc_verify_host_imports internally and
// asserts on the first mismatch found (see that function's own comment --
// a signature mismatch here is an embedding-program bug, same "trust the
// compile-time contract, assert on violation" stance bcio.c's own
// bc_program_load already takes for a missing host registration). Also
// gathers every top-level `var`/`val` into `BcProgram.globals` and
// synthesizes+compiles an ALWAYS-PRESENT `#init_globals` chunk (a `#`
// can't appear in a real 3b identifier, same collision-proofing a struct
// comparator chunk's own name already uses) that stores each global's own
// (possibly omitted -> zero-valued) initializer into its slot -- then RUNS
// that chunk once, right here, via bc_run_in_program, before returning: a
// global must already be initialized before ANYTHING else (including this
// function's own caller) can observe it.
//
// `heap` is that one-time run's own heap (an embedded-typed global's
// backing memory is allocated into it) and must NOT be `arena`, which stays
// scoped to compile-time-only data: chunks/consts/string literals. `arena`
// is conventionally a short-lived/scratch arena at most of this function's
// call sites (e.g. `ArenaTemp fn_temp = arena_temp_begin(ctx_scratch())`),
// and bc_run_in_program always opens its OWN nested ArenaTemp on
// ctx_scratch() for its register file/frame stack, popping it right before
// returning -- so passing `arena` as `heap` poisons every embedded-typed
// global's backing bytes the moment `#init_globals` allocates them. `heap`
// should be whatever LONG-LIVED arena (typically `ctx_perm()`) the caller
// already uses for its own later `bc_run_in_program` calls: a global's
// storage must outlive every one of those, the same lifetime requirement
// `heap` has at an ordinary call site.
// `module_table` (may be NULL) resolves every QUALIFIED call name
// ("build/getenv") this program's own body makes into a DIFFERENT,
// already-independently-compiled BcProgram -- see BcModuleTable's own
// bytecode.h comment for the full cross-package-import story. Copied
// straight onto the returned BcProgram.module_table (bcvm.c's
// BcOp_CallModule dispatch reads it from there, not from a new
// bc_run_in_program parameter -- deliberately, to avoid a signature
// change on that function's ~300 existing call sites for a field only the
// rare cross-module-import case ever needs).
BcProgram bc_compile_program(Checker* ck, TypedAst* tast, TypedIndex root, Arena* arena, Arena* heap,
                              LayoutCache* layout_cache, BcHostImportTable* host_imports,
                              BcModuleTable* module_table);

// Finds a compiled function's chunk index by its source name (BcChunk.name)
// -- for a caller (a test, an embedding program) that wants to run a
// specific function out of a BcProgram via bc_run_in_program without
// having tracked its index from bc_compile_program's own gather order.
u32 bc_program_find_fn(BcProgram* prog, String8 name);

typedef struct BcHostSignatureMismatch {
  String8 name;   // the host import name this mismatch is about
  String8 reason; // human-readable description -- arg count, a specific parameter's type, the
                     // return type, or (Direct-kind only) a float/double where one isn't allowed
} BcHostSignatureMismatch;

// Cross-checks every `(extern (fn name [params...] ReturnType))` declaration
// in `root` that has a MATCHING entry in `host_imports` (by name) against
// that entry's OWN registered param_types/arg_count/return_type -- NOT
// against individual call sites, since the checker already guarantees
// every call site matches its own extern declaration; the actual gap this
// closes is between the 3bscript-side declaration and whatever the
// EMBEDDING PROGRAM separately told bc_host_import_table_add(_direct),
// two independent sources of truth for "the same" signature that could
// otherwise silently drift apart. Also rejects a BcHostImportKind_Direct
// entry whose real signature involves an F32/F64 parameter or return type
// anywhere, regardless of whether it matches the extern declaration --
// Direct mode's calling convention can never route a float correctly (see
// bcnative.c), so this is a hard rejection, not a mismatch-detection.
//
// An extern declaration with NO matching host_imports entry is NOT an
// error here (it might be an ordinary C FFI extern unrelated to 3bscript
// hosting, or simply not called through this table at all) -- only a
// NAME MATCH with a REAL discrepancy counts.
//
// Returns true iff no mismatches were found. On a mismatch, appends a
// BcHostSignatureMismatch (arena-allocated) to `*out_mismatches` (dyn
// array) for EVERY mismatch found, not just the first -- callers that
// want "stop at the first" can just check `dyn_count(*out_mismatches) > 0`
// without reading further. Deliberately returns data instead of asserting
// directly (unlike bc_compile_program's own use of this), so it stays
// testable in isolation against a deliberately-wrong registration without
// aborting the calling process.
b32 bc_verify_host_imports(TypedAst* tast, TypedIndex root, BcHostImportTable* host_imports,
                           Arena* arena, BcHostSignatureMismatch** out_mismatches);

// The actual per-import signature comparison bc_verify_host_imports uses
// internally, exposed so bcio.c's load-time re-verification (comparing a
// signature STORED IN A CACHE FILE, not a live TypedAst's extern
// declaration, against the LOADING run's host_imports table) can run the
// EXACT SAME comparison logic rather than a second, potentially-drifting
// copy of it -- see bc_verify_host_imports's own comment for the full
// story on why this check exists and what it catches. `expected_param_types`
// is a plain array (length `expected_param_count`), not a `Param*`/TypedAst
// shape, since a cache file has no TypedAst to read from at load time --
// error messages reference a parameter by INDEX only, not name, for the
// same reason. Same append-every-mismatch, return-true-iff-none-found
// contract as bc_verify_host_imports.
b32 bc_verify_host_import_signature(String8 name, u32 expected_param_count, TypeRef* expected_param_types,
                                     TypeRef expected_return_type, BcHostImport* imp,
                                     Arena* arena, BcHostSignatureMismatch** out_mismatches);

#endif
