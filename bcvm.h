#ifndef BCVM_H
#define BCVM_H
#include "3b.h"
#include "bytecode.h"

// The VM's two fixed capacities. Fixed, not growable: growing these with
// dyn_push could move them out from under a raw pointer held across the
// growth -- `r`, the cached window into the register file, is exactly such a
// pointer. A fixed capacity trades that hazard for an explicit, asserted-on
// ceiling.
//
// In the header rather than bcvm.c because they are a CONTRACT a chunk has to
// satisfy, not just an internal size: bcio.c has to reject a chunk loaded off
// disk whose `num_registers` exceeds this, since the VM only xasserts it, and
// an assert on a corrupt file would abort a process that is supposed to just
// recompile.
#define BC_MAX_CALL_DEPTH      1024
#define BC_MAX_TOTAL_REGISTERS 65536

typedef struct BcResult {
  b32 has_value; // false for a void function's BcOp_ReturnVoid
  i64 value;
  b32 trapped; // true if this call bailed out early instead of completing (currently only
               // BcOp_Div/BcOp_Mod by a zero divisor) -- has_value/value are meaningless when
               // this is set. A narrow escape hatch (see bcvm.c's own note at the Div/Mod
               // cases): it unwinds straight out of bc_run_in_program with no attempt at a
               // proper per-frame unwind, which is only safe because a trapped call is expected
               // to have no still-live nested BcOp_Call frames worth preserving.
               //
               // EVERY caller must check this before believing has_value/value. Reporting it as
               // a plain zero is how a script that stopped a third of the way through its work
               // looks exactly like one that ran to completion.
  String8 trap_message; // what went wrong ("division by zero"), a static literal; empty unless
                        // `trapped`. Not a source location: this backend has no pc-to-line map.
  String8 trap_fn;      // name of the function that was executing when it trapped -- the only
                        // locator available, so worth reporting. BORROWED from the trapping
                        // BcProgram's own BcChunk, and no more valid than that program is; a
                        // caller that outlives the program must copy it, not hold it. On a trap
                        // propagated out of BcOp_CallModule this names a function in the
                        // IMPORTED module, whose program the module table owns.
} BcResult;

// Runs function `fn_index` of `prog` to completion. `args` supplies its
// chunk's first `chunk->param_count` registers (arg_count must match
// exactly); every other register starts zeroed.
//
// `heap` backs every BcOp_Alloc (struct/string storage) any call in this
// program performs -- deliberately separate from the interpreter's own
// register file/frame stack (both freed together when this whole
// bc_run_in_program call returns): a struct pointer returned or passed
// onward has to stay valid AFTER the call that produced it returns, so
// its backing memory can't come from something freed on the way out. The
// caller owns `heap`'s lifetime -- same explicit-arena convention as the
// rest of this language (e.g. `str`'s own arena parameter), no hidden
// lifetime here either. The SAME `heap` is threaded through every nested
// call this program makes.
//
// BcOp_Call/Return/ReturnVoid push/pop an explicit BcFrame (bcvm.c) --
// NOT C recursion. A fixed-capacity frame stack + one shared flat
// register file (both sized once per bc_run_in_program call, see
// BC_MAX_CALL_DEPTH/BC_MAX_TOTAL_REGISTERS in bcvm.c) means 3bscript-level
// recursion depth is a VM-detected, asserted-on limit rather than a
// dependency on however much OS stack this thread happens to have -- see
// bcvm.c's own top-of-file note for why fixed-not-growable was the right
// tradeoff (growing either array with dyn_push would risk moving it out
// from under a raw pointer cached across the growth).
//
// Computed-goto threaded dispatch (GCC/Clang's `&&label` extension) when
// available, falling back to a plain switch otherwise -- see bcvm.c's own
// note for the mechanics and why `-pedantic` needs a pragma around it.
//
// `host_imports` (may be NULL if this program makes no host calls) backs
// every BcOp_CallHost -- see BcHostImportTable's own comment in
// bytecode.h for the fixed-trampoline-signature scope note.
BcResult bc_run_in_program(BcProgram* prog, u32 fn_index, i64* args, u32 arg_count,
                            Arena* heap, BcHostImportTable* host_imports);

#endif
