#ifndef BCOSPRIMS_H
#define BCOSPRIMS_H
#include "3b.h"
#include "bytecode.h"

// One GENERIC OS-facing primitive's full signature -- shared by
// bc_register_os_primitives' own registration below AND script.c's
// `(import os)` splicing (`os` is a NATIVE pseudo-module: no `.3bs`
// source of its own, its members are these very host imports, spliced as
// bodyless externs directly from this table) -- a single source of truth
// so the two can never independently drift on what `os` actually offers.
// Arbitrary arity: `param_types` is a static array of `param_count`
// entries (NULL/0 for a no-argument primitive like `os/stdout`), which is
// what the stream verbs need -- `os/seek` alone takes three.
typedef struct BcOsPrimitiveDecl {
  String8  qualified_name; // e.g. "os/getenv" -- the slash is baked directly into the REGISTERED
                              // name (not derived at import time), since bcgen.c's host-import
                              // fallback lookup already matches a call site's callee text VERBATIM
  BcHostFn fn;
  TypeRef* param_types;    // `param_count` entries, arena-owned by bc_os_primitive_decls
  u32      param_count;
  TypeRef  return_type;
} BcOsPrimitiveDecl;

// One `os/*` CONSTANT -- `os/mode-write`, `os/seek-end`, ... Streams are
// the first `os` members that need named integer constants rather than
// functions, and a native pseudo-module has no source file to declare
// them in, so they get the same "one fixed table, two consumers" shape
// BcOsPrimitiveDecl already has: script.c splices each of these as a real
// top-level ConstDecl with a synthesized integer-literal initializer, so
// a script writes `os/mode-write` exactly as native 3b code does (see
// native_pkgs/os/os.3b's own `(val mode-write ...)`) rather than a bare
// magic number that would make the same source unportable between the
// two backends.
typedef struct BcOsConstantDecl {
  String8 qualified_name;
  TypeRef type;
  i64     value;
} BcOsConstantDecl;

// The full, fixed set of `os/*` primitives, arena-allocated. Exposed (not
// just an internal bcosprims.c detail) so script.c's `(import os)`
// splicing can iterate the EXACT SAME table bc_register_os_primitives
// itself registers against.
//
// Every entry mirrors a function in native_pkgs/os/os.3b by name AND
// signature, which is what makes one `(import os)` source text work on
// either backend; that file's header spells out the guarantee and its
// limits. Adding an entry here without adding it there (or vice versa) is
// the drift examples/os-portable exists to catch.
BcOsPrimitiveDecl* bc_os_primitive_decls(Arena* arena, u32* out_count);

// The full, fixed set of `os/*` CONSTANTS (the stream `mode-*`/`seek-*`
// values). Exposed for exactly one consumer -- script.c's `(import os)`
// splicing -- since unlike the primitives above there's nothing to
// "register" for a constant: it becomes a real ConstDecl in the importing
// program, not a host import.
BcOsConstantDecl* bc_os_constant_decls(Arena* arena, u32* out_count);

// Registers every `bc_os_primitive_decls` entry -- the whole `os` module:
// os/read-file, os/write-file, os/getenv, os/get-time, os/sleep,
// os/list-dir, os/file-mtime, os/file-exists, os/dir-exists,
// os/exec-capture, and the full set of `stream` verbs (os/open,
// os/read-line, os/seek, ...) -- into `table`
// (arena-owned, same convention bc_host_import_table_add already has).
// NOT translate-specific: this is what any `.3bs` script gets via
// `(import os)` (see script.c's own splicing) or `(import build)`
// (translate/build.3bs itself does `(import os)` and wraps each in a
// nicer name -- see that file's own comment). See bcosprims.c's own
// top-of-file note for the full design/motivation -- the pkg-config-on-
// Windows use case that originally motivated this.
void bc_register_os_primitives(BcHostImportTable* table, Arena* arena);

#endif
