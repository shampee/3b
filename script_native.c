// script_native.c -- see script_native.h's own top-of-file note for the
// design (opaque engine, scalar-only call args, no host-fn registration
// yet).
#include "script_native.h"
#include <stdlib.h>
#include <stdint.h>

struct NativeScriptEngine {
  Context           ctx;          // this engine's own, thread-local Context -- see script_native.h's
                                     // THREADING note. Lives INSIDE this struct (a stable address for
                                     // the engine's whole lifetime) precisely because ctx_init/ctx_enter
                                     // stash a raw Context* in thread-local storage -- it must never move
                                     // or be freed out from under that pointer while the engine is alive.
  ScriptTable       table;
  BcHostImportTable host_imports; // always empty for now -- see script_native.h's own SCOPE note
};

NativeScriptEngine*
native_script_engine_create(void) {
  // Plain malloc, NOT ctx_perm() -- ctx_perm() doesn't exist until AFTER
  // ctx_init has run, and ctx_init itself needs a Context at a stable
  // address to hand to ctx_enter, so the engine (which embeds that
  // Context) has to exist before its own arenas do.
  NativeScriptEngine* engine = malloc(sizeof(NativeScriptEngine));
  xassert(engine && "native_script_engine_create: out of memory");
  MemoryZeroStruct(engine);
  ctx_init(&engine->ctx, MB(4)); // also makes this the CALLING thread's current context (ctx_enter)
  return engine;
}

void
native_script_engine_destroy(NativeScriptEngine* engine) {
  if (!engine) return;
  ctx_enter(&engine->ctx); // in case some OTHER context is current on this thread right now
  ctx_free();              // destroys ctx.scratch/ctx.perm (base.c's own ctx_free)
  free(engine);
}

bool
native_script_load(NativeScriptEngine* engine, String8 path, u32* out_handle_index) {
  ctx_enter(&engine->ctx);
  ScriptHandle h;
  b32 ok = script_load(&engine->table, path, &engine->host_imports, &h);
  *out_handle_index = ok ? h.index : 0;
  return ok;
}

// Shared by every native_script_callN wrapper below -- `args`/`argc` are
// exactly script_call's own parameters; NULL/0 for the zero-arg case.
// Uses a fresh, per-call scratch arena for script_call's own `heap`
// parameter, torn down before returning -- correct as long as a result is
// only ever read as a plain scalar (see script_native.h's own SCOPE note:
// a struct/string result's backing memory would NOT survive this).
static b32
call_n(NativeScriptEngine* engine, u32 handle_index, String8 fn_name, i64* args, u32 argc,
       bool* out_has_value, i64* out_value) {
  ctx_enter(&engine->ctx);
  ScriptHandle handle = { handle_index };
  BcResult     result;
  ArenaTemp    call_heap = arena_temp_begin(ctx_scratch());
  b32 ok = script_call(&engine->table, handle, fn_name, args, argc, call_heap.arena,
                        &engine->host_imports, &result);
  arena_temp_end(&call_heap);
  if (ok) {
    *out_has_value = result.has_value;
    *out_value     = result.value;
  }
  return ok;
}

bool
native_script_call0(NativeScriptEngine* engine, u32 handle_index, String8 fn_name,
                     bool* out_has_value, i64* out_value) {
  return call_n(engine, handle_index, fn_name, NULL, 0, out_has_value, out_value);
}

bool
native_script_call1(NativeScriptEngine* engine, u32 handle_index, String8 fn_name,
                     i64 a0, bool* out_has_value, i64* out_value) {
  i64 args[1] = { a0 };
  return call_n(engine, handle_index, fn_name, args, 1, out_has_value, out_value);
}

bool
native_script_call2(NativeScriptEngine* engine, u32 handle_index, String8 fn_name,
                     i64 a0, i64 a1, bool* out_has_value, i64* out_value) {
  i64 args[2] = { a0, a1 };
  return call_n(engine, handle_index, fn_name, args, 2, out_has_value, out_value);
}

bool
native_script_call3(NativeScriptEngine* engine, u32 handle_index, String8 fn_name,
                     i64 a0, i64 a1, i64 a2, bool* out_has_value, i64* out_value) {
  i64 args[3] = { a0, a1, a2 };
  return call_n(engine, handle_index, fn_name, args, 3, out_has_value, out_value);
}

bool
native_script_call4(NativeScriptEngine* engine, u32 handle_index, String8 fn_name,
                     i64 a0, i64 a1, i64 a2, i64 a3, bool* out_has_value, i64* out_value) {
  i64 args[4] = { a0, a1, a2, a3 };
  return call_n(engine, handle_index, fn_name, args, 4, out_has_value, out_value);
}

bool
native_script_poll_reload(NativeScriptEngine* engine, u32 handle_index) {
  ctx_enter(&engine->ctx);
  ScriptHandle handle = { handle_index };
  return script_poll_reload(&engine->table, handle, &engine->host_imports);
}

void
native_script_unload(NativeScriptEngine* engine, u32 handle_index) {
  ctx_enter(&engine->ctx);
  ScriptHandle handle = { handle_index };
  script_unload(&engine->table, handle);
}

// True iff `name` is already registered in engine->host_imports -- see
// native_script_register_u64_to_u32's own comment on why a duplicate is
// rejected rather than silently shadowed/duplicated.
static b32
host_import_already_registered(NativeScriptEngine* engine, String8 name) {
  foreach_index(i, dyn_count(engine->host_imports.entries)) {
    if (str8_match(engine->host_imports.entries[i].name, name, 0)) return true;
  }
  return false;
}

bool
native_script_register_u64_to_u32(NativeScriptEngine* engine, String8 name, u32 (*fn)(u64)) {
  ctx_enter(&engine->ctx);
  if (host_import_already_registered(engine, name)) return false;
  TypeRef u64_ty = {0}; u64_ty.kind = TypeKind_U64;
  TypeRef u32_ty = {0}; u32_ty.kind = TypeKind_U32;
  TypeRef* params = push_array(ctx_perm(), TypeRef, 1);
  params[0] = u64_ty;
  bc_host_import_table_add_direct(&engine->host_imports, ctx_perm(), name,
                                   (void*)(intptr_t)fn, params, 1, u32_ty);
  return true;
}

bool
native_script_register_u64_u32_to_void(NativeScriptEngine* engine, String8 name, void (*fn)(u64, u32)) {
  ctx_enter(&engine->ctx);
  if (host_import_already_registered(engine, name)) return false;
  TypeRef u64_ty  = {0}; u64_ty.kind  = TypeKind_U64;
  TypeRef u32_ty  = {0}; u32_ty.kind  = TypeKind_U32;
  TypeRef void_ty = {0}; void_ty.kind = TypeKind_Void;
  TypeRef* params = push_array(ctx_perm(), TypeRef, 2);
  params[0] = u64_ty;
  params[1] = u32_ty;
  bc_host_import_table_add_direct(&engine->host_imports, ctx_perm(), name,
                                   (void*)(intptr_t)fn, params, 2, void_ty);
  return true;
}

bool
native_script_register_u64_u32x3_to_void(NativeScriptEngine* engine, String8 name,
                                          void (*fn)(u64, u32, u32, u32)) {
  ctx_enter(&engine->ctx);
  if (host_import_already_registered(engine, name)) return false;
  TypeRef u64_ty  = {0}; u64_ty.kind  = TypeKind_U64;
  TypeRef u32_ty  = {0}; u32_ty.kind  = TypeKind_U32;
  TypeRef void_ty = {0}; void_ty.kind = TypeKind_Void;
  TypeRef* params = push_array(ctx_perm(), TypeRef, 4);
  params[0] = u64_ty;
  params[1] = u32_ty;
  params[2] = u32_ty;
  params[3] = u32_ty;
  bc_host_import_table_add_direct(&engine->host_imports, ctx_perm(), name,
                                   (void*)(intptr_t)fn, params, 4, void_ty);
  return true;
}
