#ifndef BCNATIVE_H
#define BCNATIVE_H
#include "3b.h"

// See bcnative.c's own comment on bc_call_native_direct for why this is a
// generous, chosen cap, not a hard ABI ceiling. Exposed here (not just a
// bcnative.c-internal `#define`) so bcgen.c's bc_verify_host_import_signature
// can reject a Direct-kind registration whose arg_count exceeds it at
// VERIFY time, the same proactive-rejection treatment Direct+float
// already gets, rather than only failing much later via
// bc_call_native_direct's own runtime assert.
#define BC_NATIVE_DIRECT_MAX_ARGS 12

// Calls an ARBITRARY native function pointer directly, with no per-
// function C wrapper/trampoline needed -- see bcnative.c's own top-of-
// file note for the mechanism and its real, documented limits (x86-64
// System V ABI only; integer/pointer arguments only, no float/double --
// PERMANENTLY, not just "not yet done", see that file's own note on why;
// at most BC_NATIVE_DIRECT_MAX_ARGS arguments (bcnative.c), comfortably
// past SysV's own 6-register threshold since anything beyond that is
// ordinary stack-passed arguments this mechanism handles the same way; a
// scalar integer/pointer return only, or a meaningless-but-harmless value
// for a void-returning function; no struct-by-value passing or
// returning).
i64 bc_call_native_direct(void* fn_ptr, i64* args, u32 arg_count);

#endif
