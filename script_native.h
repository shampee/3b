#ifndef SCRIPT_NATIVE_H
#define SCRIPT_NATIVE_H
#include "script.h"

// script_native.h -- a native-3b-facing wrapper over script.h's hot-
// reloadable script-embedding API, for a NATIVELY-COMPILED (`3b build`)
// game to load, call, and hot-reload `.3b`/`.3bs` gameplay scripts. See
// script.3b (a hand-written binding, NOT `3b translate` output) for how a
// native 3b program actually reaches this.
//
// OPAQUE BY DESIGN: every function here takes/returns only `String8`
// (byte-identical to native 3b's own `string`, i.e. `bbb_String8` -- see
// codegen.c's c_type_from_typeref, the "string" case), plain scalars, and
// one opaque `NativeScriptEngine*` (native 3b sees this as `any*`,
// matching how sdl.3b already represents e.g. `SDL_Window*`) -- NEVER a
// compiler-internal type (Arena/BcHostImportTable/BcProgram/ScriptTable)
// or a struct whose layout would need to match across two independently
// compiled translation units. A script handle crosses this boundary as a
// bare `u32` (ScriptHandle is itself just `{u32 index}`, so this is a
// direct, lossless re-representation, not a lossy simplification) --
// index 0 is always the null/invalid handle, matching ScriptHandle's own
// convention.
//
// SCOPE: call arguments and results are plain i64 SCALARS only (numbers, or
// a packed handle value -- exactly what a native game's own handle-taking
// mutator functions expect, per script.h's state model). A STRING argument
// or result crossing THIS boundary would need a bridge between the bytecode
// VM's boxed-string representation and native 3b's `bbb_String8`, which
// nothing here provides.
//
// Calling BACK into the host is covered, but only for the fixed integer
// signature shapes native_script_register_* declares below; the fully
// generic form would need a native-3b-facing TypeRef constructor, which
// does not exist.
//
// THREADING: mirrors base/base.h's own Context, which is thread-local --
// native_script_engine_create makes its own Context current on the
// CALLING thread (every native_script_* call re-asserts it as current
// too, in case some OTHER context is active on this thread when called),
// so every native_script_* call for one engine should happen from the
// same thread that created it.
typedef struct NativeScriptEngine NativeScriptEngine;

NativeScriptEngine* native_script_engine_create(void);
void                native_script_engine_destroy(NativeScriptEngine* engine);

// `*out_handle_index` is 0 (the null handle) whenever this returns false.
bool native_script_load(NativeScriptEngine* engine, String8 path, u32* out_handle_index);

// Fixed-arity call wrappers (0..4 plain i64/handle arguments) -- see this
// file's own top-of-file note on why arguments are scalar-only.
// `*out_has_value`/`*out_value` mirror BcResult's own two fields exactly,
// just as separate out-parameters rather than a shared struct (sidesteps
// any risk of a `bool`-vs-`b32` struct-layout mismatch between THIS TU
// and a 3b-generated caller -- native 3b's own `bool` compiles to a real
// C `_Bool`, not this codebase's internal 4-byte `b32` convention).
bool native_script_call0(NativeScriptEngine* engine, u32 handle_index, String8 fn_name,
                        bool* out_has_value, i64* out_value);
bool native_script_call1(NativeScriptEngine* engine, u32 handle_index, String8 fn_name,
                        i64 a0, bool* out_has_value, i64* out_value);
bool native_script_call2(NativeScriptEngine* engine, u32 handle_index, String8 fn_name,
                        i64 a0, i64 a1, bool* out_has_value, i64* out_value);
bool native_script_call3(NativeScriptEngine* engine, u32 handle_index, String8 fn_name,
                        i64 a0, i64 a1, i64 a2, bool* out_has_value, i64* out_value);
bool native_script_call4(NativeScriptEngine* engine, u32 handle_index, String8 fn_name,
                        i64 a0, i64 a1, i64 a2, i64 a3, bool* out_has_value, i64* out_value);

bool native_script_poll_reload(NativeScriptEngine* engine, u32 handle_index);
void native_script_unload(NativeScriptEngine* engine, u32 handle_index);

// ~~ Host-callback registration -- lets a native game expose a handful of
// its OWN functions for a script loaded through `engine` to call BACK
// into. Not the generic form (which would need a native-3b-facing TypeRef
// constructor): three FIXED, pure-integer signature shapes, registered via
// bc_host_import_table_add_direct. See bytecode.h's BcHostImportKind_Direct
// note for why integer/pointer-only args are the limit of that mechanism,
// not a limit invented here.
//
// `fn` is any ordinary native-3b top-level `fn` value with the matching
// REAL signature -- native 3b compiles a `(fn [idx u64] u32)` to a real C
// `u32 (*)(u64)`, so this needs no wrapper at all, same zero-marshaling
// deal bcgen_native_test.c already proves for Direct-kind imports in
// general. A float-typed value crossing this boundary (e.g. a Vec3
// field) goes through as raw bits (native 3b's own `(deref (cast u32*
// (addr v)))` pointer-reinterpret trick, no new language feature needed)
// -- the SAME convention on both the registering side and the script
// side calling back in, so a script never sees a real f32 arg/return
// here, only u32 bit patterns it round-trips through its own bits-to-f32.
//
// Registration must happen AFTER native_script_engine_create and BEFORE
// any native_script_load whose script declares a matching `(extern (fn
// ...))` -- the same ordering script.h's BcHostImportTable requires.
// Returns false if
// `name` was already registered (bc_host_import_table_add_direct has no
// such check itself, but a silent duplicate would be a confusing bug to
// chase, so this layer rejects it explicitly).
bool native_script_register_u64_to_u32(NativeScriptEngine* engine, String8 name, u32 (*fn)(u64));
bool native_script_register_u64_u32_to_void(NativeScriptEngine* engine, String8 name, void (*fn)(u64, u32));
bool native_script_register_u64_u32x3_to_void(NativeScriptEngine* engine, String8 name, void (*fn)(u64, u32, u32, u32));

#endif
