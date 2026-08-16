#ifndef BCIO_H
#define BCIO_H
#include "3b.h"
#include "bytecode.h"
#include "bcgen.h" // BcHostSignatureMismatch/bc_verify_host_import_signature -- see
                     // bc_verify_cached_host_imports's own comment below
#include "file.h"

// Serializes/deserializes a BcProgram to a flat binary file -- the point 8
// half of the 3bscript-bytecode-vm-design project memory ("compile once,
// cache to disk, skip recompilation on subsequent boots unless source
// changed -- the actual lever for incredibly fast boot"). See bcio.c's own
// top-of-file note for the two real correctness problems this had to
// solve (persisting string-literal pointers, and host-import indices that
// aren't safe to bake in as absolute numbers across a save/reload).
//
// Native endianness/pointer width only, by design (per the project
// memory) -- a saved file only ever gets read back by the SAME native
// program that wrote it, so there's no cross-arch portability to support.

// Writes `prog` to `path`. `host_imports` must be the EXACT table `prog`
// was compiled against (bc_compile_program's own parameter) -- used to
// translate each BcOp_CallHost's baked-in index back into that import's
// own NAME, so a future load can re-resolve it against a DIFFERENT run's
// host_imports table (different registration order, or even a different
// set of registered functions) instead of trusting the original index to
// still mean the same thing. ALSO records each REFERENCED host import's
// own full signature (kind/param_types/return_type) -- see
// BcCachedHostImportSig/bc_verify_cached_host_imports below for why.
// Pass NULL only if `prog` makes no host calls at all. `content_hash` is
// an opaque caller-chosen value (script.c passes bc_content_hash of the
// SOURCE this program was compiled from) stored verbatim in the file and
// handed back by bc_program_load's own BcLoadResult -- lets a caller use
// a FIXED cache filename (overwritten on every save) while still being
// able to tell "is this cache still valid for the CURRENT source" without
// re-deriving that from the filename itself. Returns false on any I/O
// failure (see file_store).
b32 bc_program_save(BcProgram* prog, BcHostImportTable* host_imports, String8 path, u64 content_hash);

typedef struct BcLoadResult {
  b32       ok; // false for a missing file, I/O error, bad magic, or version mismatch -- ALWAYS
                  // treat any of these as "no usable cache", never as a fatal error: the caller's
                  // fallback is just to recompile from source (see BC_CACHE_MAGIC/VERSION in bcio.c)
  u64       content_hash; // verbatim from bc_program_save's own `content_hash` param -- ONLY
                             // meaningful when `ok` is true. A caller using a fixed cache filename
                             // (rather than encoding a hash in the filename itself) compares this
                             // against the CURRENT source's own hash to decide whether this load is
                             // actually usable, or just a stale cache from a prior edit that happens
                             // to still pass the magic/version check.
  BcProgram program;
  File      mapped_file; // the underlying mmap -- see bc_program_load's own top-of-file note.
                            // `program`'s code/const arrays and every string-literal constant
                            // point DIRECTLY into these mapped pages (zero-copy), so this mapping
                            // must stay alive for as long as `program` is used. Only meaningful
                            // when `ok` is true. Pass to bc_program_unload once `program` is no
                            // longer needed -- NOT plain `file_unmap` (which only unmaps; see that
                            // function's own comment for why more than that is needed here).
} BcLoadResult;

// Loads a program previously written by bc_program_save from `path` via a
// WRITABLE (copy-on-write) mmap of the file (file_map -- see that
// function's own comment) -- NOT a read-then-copy the way this used to
// work. Every chunk's code/const arrays, and every string-literal
// constant's bytes, are LIVE POINTERS directly into the mapped file --
// genuinely zero-copy for anything that scales with program/data size.
// The only things still freshly allocated (into `arena`, small, fixed-size
// per call) are: the top-level BcProgram.chunks array itself (metadata,
// proportional to function count, not data size), and one 16-byte String8
// header per BOXED string-literal constant (see BcStringFixup's own
// `is_header` case in bytecode.h) -- its `.str` field points into the
// mapping too, only the header struct itself needs a stable address of
// its own. Two in-place patches are applied directly to the mapped
// (writable, copy-on-write) memory: a boxed string header's `.str`
// pointer, and each BcOp_CallHost operand re-resolved against
// `host_imports` -- see bc_program_save's own note on why that can't be
// baked in as a fixed index. `host_imports` need not match the order (or
// even the exact contents, beyond having every name the file actually
// references) of whatever table originally compiled this file; pass NULL
// only if the file makes no host calls (asserts if it turns out to need
// one anyway -- a missing registration is an embedding-program bug, not a
// malformed-file concern, which is what `ok` already distinguishes).
// ALSO cross-checks every referenced host import's REGISTERED signature
// in `host_imports` against what the file recorded at save time (see
// bc_verify_cached_host_imports below) -- asserts (with mismatches
// printed to stderr first) on any real discrepancy, same "trust the
// compile-time contract" stance a missing registration already gets.
//
// LIFETIME: the returned `BcLoadResult.mapped_file` must stay alive (i.e.
// not be passed to bc_program_unload) for as long as `program` is used --
// see BcLoadResult's own comment.
//
// ALSO allocates `program.globals` (into `arena`, metadata-scale -- see
// above) and runs the loaded `#init_globals` chunk once, right here, via
// `heap` -- mirrors bc_compile_program's own identical step exactly (see
// its bcgen.h doc comment for the full reasoning; in short, `heap` must be
// genuinely long-lived, e.g. `ctx_perm()`, NOT the same scratch arena a
// caller might otherwise reuse for `arena` here).
// `module_resolver` (nullable) rebuilds the program's BcModuleTable, which
// is the one thing in a BcProgram a file genuinely cannot hold on its own:
// its entries point at OTHER separately-compiled BcPrograms. The file records
// each import's qualified name, and this callback turns each one back into a
// live program+chunk (see BcModuleResolveFn in bytecode.h). Pass NULL only
// when the cached program has no cross-package imports -- a file that turns
// out to need one is then reported as an unusable cache (`ok` false, caller
// recompiles), never a crash and never a silently-broken dispatch.
BcLoadResult bc_program_load(String8 path, BcHostImportTable* host_imports,
                              BcModuleResolver* module_resolver, Arena* arena, Arena* heap);

// Unmaps the file backing a loaded program (via file_unmap) -- call once
// `result.program` is no longer needed. Just a thin, clearly-named wrapper
// (rather than requiring every caller to know to reach into `mapped_file`
// and call plain file_unmap itself), matching this API's own "the
// mmap-ness of loading is an implementation detail, not something callers
// should have to think about beyond this one cleanup call" intent. Safe to
// call on a `!result.ok` result (file_unmap itself is a no-op on an
// already-zeroed File).
void bc_program_unload(BcLoadResult* result);

// A simple (non-cryptographic -- FNV-1a) content hash, just strong enough
// to detect "did the source change" for a caching decision. Meant to be
// combined across every source file a package's compile depends on (fold
// each file's own hash together, e.g. via bc_content_hash on the
// concatenation, or by hashing each file and combining with XOR/mix at
// the call site) and stored/compared by the CALLER, alongside whatever
// cache file path convention it uses -- bcio.c deliberately doesn't bake
// a specific cache-invalidation POLICY (mtime vs. hash, one cache file
// per package vs. one per function, ...) into bc_program_save/load
// themselves, only the save/load mechanics those policies build on.
u64 bc_content_hash(String8 data);

// One host import's signature as RECORDED IN A CACHE FILE at save time
// (from whatever host_imports table originally compiled it) -- what
// bc_verify_cached_host_imports below cross-checks against the LOADING
// run's own table. Exposed (not just an internal bcio.c detail) so that
// re-verification is independently testable, same reasoning
// bc_verify_host_imports/BcHostSignatureMismatch already established.
typedef struct BcCachedHostImportSig {
  String8           name;
  BcHostImportKind  kind;
  TypeRef*          param_types; // arena-owned, length param_count; NULL when param_count == 0
  u32               param_count;
  TypeRef           return_type;
} BcCachedHostImportSig;

// Cross-checks a cache file's own recorded host-import signatures (see
// BcCachedHostImportSig above -- bc_program_load reads these out of the
// file and calls this internally, asserting on any mismatch, same
// "trust the compile-time contract, assert on violation" stance
// bc_compile_program's own use of bc_verify_host_imports already takes)
// against `host_imports` -- the CURRENT run's table. It is the load-time
// counterpart to bc_verify_host_imports, which only ever runs against a LIVE
// TypedAst at COMPILE time and so never sees a program loaded from cache.
// Without it, a cache file reloaded against a host_imports table that
// resolves the right NAMES but to WRONGLY-TYPED registrations would silently
// corrupt results (e.g. a float argument read out of an integer register)
// instead of erroring. Reuses bc_verify_host_import_signature (bcgen.h) --
// the EXACT SAME per-import comparison bc_verify_host_imports itself
// uses, not a second, potentially-drifting copy of that logic. Same
// append-every-mismatch, return-true-iff-none-found, testable-in-isolation
// contract as bc_verify_host_imports.
b32 bc_verify_cached_host_imports(BcCachedHostImportSig* sigs, u64 sig_count,
                                   BcHostImportTable* host_imports, Arena* arena,
                                   BcHostSignatureMismatch** out_mismatches);

#endif
