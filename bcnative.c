// bcnative.c -- the zero-marshaling host call: invokes an arbitrary existing
// native C function straight from the VM, with no hand-written
// i64*-unpacking trampoline, for the integer/pointer-only case (see
// BcHostImportKind_Direct in bytecode.h). VM register values ARE the calling
// convention's argument values rather than being converted into them.
//
// MECHANISM: cast the native function pointer to one fixed, maximal-arity
// prototype -- BC_NATIVE_DIRECT_MAX_ARGS i64 params returning i64 -- and call
// through that, even when the real function takes fewer or differently-typed
// (but integer/pointer-sized) parameters.
//
// This works because the x86-64 System V ABI places the first 6
// integer/pointer arguments in fixed registers (RDI, RSI, RDX, RCX, R8, R9)
// and the rest on the stack, in a fixed order, regardless of how many the
// CALLEE reads: a function taking fewer simply never touches the extra slots.
// Well-defined at the ABI level, even though retyping a function pointer this
// way is unspecified at the C language level. It is genuinely
// platform-specific, not portable C.
//
// Argument PLACEMENT stays the C compiler's job -- it lowers whatever N-ary
// prototype this file declares using its own ABI knowledge -- so this file
// never hand-manages a stack frame. That's why raising
// BC_NATIVE_DIRECT_MAX_ARGS past 6 needed only a wider prototype.
//
// FLOAT ARGUMENTS ARE NOT SUPPORTED, at all, however many int/pointer args
// accompany them: floats travel in a separate register class (XMM0-7) that no
// integer-prototype cast can route into. Handling a mixed signature would take
// either one hand-written prototype per int-vs-float PATTERN -- not merely per
// count -- or libffi. Structs passed or returned BY VALUE are out for the same
// reason: SysV's per-eightbyte INTEGER/SSE classification, plus a hidden
// pointer for large ones, is ABI complexity this won't replicate by hand.
//
// Both cases still work through BcHostImportKind_Trampoline, whose
// hand-written wrapper unpacks whatever the real signature needs. This is a
// permanent boundary rather than an unfinished one -- closing it means taking
// on libffi -- and it asserts rather than mis-calling.
//
// Verified empirically for both the 6-argument and the extended cases: GCC and
// Clang at -O2 both pass every argument correctly, register- and stack-passed
// alike, and leave the extra slots harmless when the real callee takes fewer.
#include "bcnative.h"
#include <stdint.h>

#if !defined(__x86_64__)
#error "bc_call_native_direct assumes the x86-64 System V calling convention -- see this file's own top-of-file note; no portable fallback exists here without a real FFI library like libffi"
#endif

// BC_NATIVE_DIRECT_MAX_ARGS lives in bcnative.h so bcgen.c's verification can
// reject an over-arity Direct registration up front rather than only via the
// assert below. It is NOT a load-bearing ABI constant, unlike the original 6,
// which really was SysV's integer-register count: SysV passes arbitrarily many
// stack arguments beyond those 6, so this is only how many i64 parameters this
// file bothers to declare. Raise it in bcnative.h if a host function needs more.
typedef i64 (*BcNativeFnMax)(i64, i64, i64, i64, i64, i64, i64, i64, i64, i64, i64, i64);

i64
bc_call_native_direct(void* fn_ptr, i64* args, u32 arg_count) {
  xassert(arg_count <= BC_NATIVE_DIRECT_MAX_ARGS);
  i64 a[BC_NATIVE_DIRECT_MAX_ARGS];
  foreach_index(i, BC_NATIVE_DIRECT_MAX_ARGS) a[i] = (i < arg_count) ? args[i] : 0;
  BcNativeFnMax fn = (BcNativeFnMax)(intptr_t)fn_ptr;
  return fn(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11]);
}
