#ifndef SCRIPT_NATIVE_ABI_H
#define SCRIPT_NATIVE_ABI_H
#include "3b_runtime.h"

// script_native_abi.h -- the MINIMAL, self-contained mirror of
// script_native.h's own declarations, meant to be `-include`d directly
// into a NATIVE 3b PROJECT's generated code (via that project's own
// `build.cfg.3b` `include-first` -- see scriptvm.3b's own top-of-file
// note). A native 3b project has no reach into this compiler's own
// internal header maze (script.h/bytecode.h/base/base.h/...) the way
// script_native.c itself does, so this file re-declares each function's
// signature using ONLY the generated runtime prelude's own types
// (`3b_runtime.h`, already `#include`d by every generated package, hence
// already on this project's own include path) -- `bbb_String8`/`u32`/
// `i64`/`bool` here are the EXACT SAME types a native 3b `string`/`u32`/
// `i64`/`bool` value already compiles to, not merely layout-compatible
// stand-ins. Redeclaring a same-layout struct under a different name here
// instead would not do: gcc rejects that as an incompatible type for a
// by-value struct argument once both are visible in one translation unit.
// A raw scalar or pointer would survive it; a struct passed by value needs
// true type identity, not just matching layout.
//
// NEVER included by script_native.c itself (which uses the real
// script_native.h, with this COMPILER's OWN internal `String8`/`b32`/...
// -- distinct types from the runtime prelude's `bbb_String8`/`u32`/...
// even though every one of them matches byte-for-byte, which is all the
// FUNCTION SIGNATURES below actually need: script_native.c's real
// definitions and this header's declarations only have to agree on
// layout/calling-convention across their two SEPARATE translation units,
// never on literal type names).

typedef struct NativeScriptEngine NativeScriptEngine;

NativeScriptEngine* native_script_engine_create(void);
void                native_script_engine_destroy(NativeScriptEngine* engine);

bool native_script_load(NativeScriptEngine* engine, bbb_String8 path, u32* out_handle_index);

bool native_script_call0(NativeScriptEngine* engine, u32 handle_index, bbb_String8 fn_name,
                         bool* out_has_value, i64* out_value);
bool native_script_call1(NativeScriptEngine* engine, u32 handle_index, bbb_String8 fn_name,
                         i64 a0, bool* out_has_value, i64* out_value);
bool native_script_call2(NativeScriptEngine* engine, u32 handle_index, bbb_String8 fn_name,
                         i64 a0, i64 a1, bool* out_has_value, i64* out_value);
bool native_script_call3(NativeScriptEngine* engine, u32 handle_index, bbb_String8 fn_name,
                         i64 a0, i64 a1, i64 a2, bool* out_has_value, i64* out_value);
bool native_script_call4(NativeScriptEngine* engine, u32 handle_index, bbb_String8 fn_name,
                         i64 a0, i64 a1, i64 a2, i64 a3, bool* out_has_value, i64* out_value);

bool native_script_poll_reload(NativeScriptEngine* engine, u32 handle_index);
void native_script_unload(NativeScriptEngine* engine, u32 handle_index);

// `u32` params/returns here are the exact same type a native 3b `u32`
// value compiles to (see this file's own top-of-file note) -- `cb`'s
// pointee signature matches vm.3b's Getter/Setter1/Setter3 aliases
// exactly, so a native 3b `(fn [idx u64] u32)`-typed value passed as `cb`
// needs no cast on the 3b side.
bool native_script_register_u64_to_u32(NativeScriptEngine* engine, bbb_String8 name, u32 (*cb)(u64));
bool native_script_register_u64_u32_to_void(NativeScriptEngine* engine, bbb_String8 name, void (*cb)(u64, u32));
bool native_script_register_u64_u32x3_to_void(NativeScriptEngine* engine, bbb_String8 name,
                                               void (*cb)(u64, u32, u32, u32));

#endif
