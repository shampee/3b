// bcvm.c -- the interpreter half of the 3bscript bytecode backend; bcgen.c
// is the compiler half, and bytecode.h defines the instruction set and the
// register representation both assume.
//
// CALL STACK: an explicit BcFrame stack plus one shared flat register file,
// not C recursion. BcOp_Call/Return/ReturnVoid push and pop a frame and keep
// looping inside this same bc_run_in_program invocation. So interpreted
// recursion is bounded by BC_MAX_CALL_DEPTH, which the VM detects and
// asserts on, rather than by however much OS stack the thread happens to
// have.
//
// The register file is likewise one fixed-capacity array
// (BC_MAX_TOTAL_REGISTERS), with each frame owning a WINDOW into it via a
// `reg_base` offset -- the design register VMs like Lua's use -- rather than
// an allocation per call.
#include "bcvm.h"
#include "bcnative.h"
#include "bcmap.h"
#include <string.h>
#include <stdint.h>

typedef struct BcFrame {
  BcChunk* chunk;
  u32      reg_base; // this frame's own registers are regs[reg_base .. reg_base+chunk->num_registers)
  u32      pc;        // saved only while this frame is NOT the one currently executing
  u32      dst_reg;   // absolute index into regs[] (i.e. already relative to the CALLER's own
                        // reg_base) where this frame's return value gets written on Return/ReturnVoid
                        // -- meaningless for frame 0, which returns from bc_run_in_program itself
} BcFrame;

// Computed-goto threaded dispatch via GCC/Clang's `&&label`/`goto *expr`
// extension where available, falling back to a plain switch: fewer branch
// mispredictions than a switch's bounds-check-then-jump-table indirection,
// the technique CPython's ceval.c uses. Not a new toolchain dependency --
// the native backend already requires GCC or Clang downstream.
//
// `-pedantic`, already in this project's CFLAGS, flags both constructs as
// non-standard even under GCC/Clang, hence the #pragma push/pop bracketing
// bc_run_in_program's body below.
#if defined(__GNUC__) || defined(__clang__)
#define BC_HAVE_COMPUTED_GOTO 1
#endif

#ifdef BC_HAVE_COMPUTED_GOTO
#define BC_TARGET(name) L_##name
// The range check is NOT redundant with "bcgen.c only ever emits real
// opcodes": a program can also arrive from bcio.c's loader, which maps a
// `.3bc` off disk, and a corrupt or truncated one supplies `ins.kind`
// directly. Unchecked, that indexes past the dispatch table and `goto *`
// jumps through whatever pointer follows it. The NULL check covers the same
// hole from the inside -- the table is built with designated initializers,
// so any value inside the range that no target claims is a NULL entry.
// Switch-mode dispatch below already had this, as its `default:` case.
#define BC_DISPATCH()                                                     \
  do {                                                                    \
    ins = chunk->code[pc];                                                \
    if ((u32)ins.kind >= (u32)BcOp_Count || !bc_dispatch_table[ins.kind]) \
      BC_TRAP("invalid opcode (corrupt bytecode)");                       \
    goto *bc_dispatch_table[ins.kind];                                    \
  } while (0)
#else
#define BC_TARGET(name) case BcOp_##name
#define BC_DISPATCH()   goto bc_dispatch_top
#endif

// Abandons the whole bc_run_in_program call, releasing its register file and
// frame stack, and reports WHY plus which function was mid-execution. Reads
// bc_run_in_program's own `temp`/`chunk` locals, so it only makes sense inside
// that function's dispatch loop. See BcResult.trapped for the contract, and
// bcvm.c's Div/Mod cases for the only two things that raise one today.
#define BC_TRAP(msg_lit)                    \
  do {                                      \
    arena_temp_end(&temp);                  \
    return (BcResult){                      \
      .trapped      = 1,                    \
      .trap_message = str8_lit(msg_lit),    \
      .trap_fn      = chunk->name,          \
    };                                      \
  } while (0)

// Shared arithmetic for the f64/f32 opcode groups, as ordinary functions
// rather than inlined per opcode. Computed goto can't share one label the way
// a switch shares a case body, so each of the ~10 variants per group needs
// its own label; these functions plus the one-line BC_F64_*_CASE/
// BC_F32_*_CASE macros keep the extract-bits/operate/pack-bits boilerplate
// from being duplicated ten times over.
static i64
bc_exec_f64_arith(BcOp kind, f64 a, f64 b) {
  f64 r;
  switch (kind) {
    case BcOp_FSub: r = a - b; break;
    case BcOp_FMul: r = a * b; break;
    case BcOp_FDiv: r = a / b; break;
    default:        r = a + b; break; // BcOp_FAdd
  }
  i64 bits; MemoryCopy(&bits, &r, sizeof(bits));
  return bits;
}

static i64
bc_exec_f64_compare(BcOp kind, f64 a, f64 b) {
  switch (kind) {
    case BcOp_FNeq: return (a != b) ? 1 : 0;
    case BcOp_FLt:  return (a <  b) ? 1 : 0;
    case BcOp_FLe:  return (a <= b) ? 1 : 0;
    case BcOp_FGt:  return (a >  b) ? 1 : 0;
    case BcOp_FGe:  return (a >= b) ? 1 : 0;
    default:        return (a == b) ? 1 : 0; // BcOp_FEq
  }
}

static i64
bc_exec_f32_arith(BcOp kind, f32 a, f32 b) {
  f32 r;
  switch (kind) {
    case BcOp_F32Sub: r = a - b; break;
    case BcOp_F32Mul: r = a * b; break;
    case BcOp_F32Div: r = a / b; break;
    default:          r = a + b; break; // BcOp_F32Add
  }
  u32 bits; memcpy(&bits, &r, sizeof(bits));
  return (i64)(u64)bits;
}

static i64
bc_exec_f32_compare(BcOp kind, f32 a, f32 b) {
  switch (kind) {
    case BcOp_F32Neq: return (a != b) ? 1 : 0;
    case BcOp_F32Lt:  return (a <  b) ? 1 : 0;
    case BcOp_F32Le:  return (a <= b) ? 1 : 0;
    case BcOp_F32Gt:  return (a >  b) ? 1 : 0;
    case BcOp_F32Ge:  return (a >= b) ? 1 : 0;
    default:          return (a == b) ? 1 : 0; // BcOp_F32Eq
  }
}

////////////////////////////////
//~ `string-to-*` parsing, replicating runtime/bbb_string.c's
// `bbb_string_to_i64`/`_u64`/`_f64`/`_f32`. That family is text embedded into
// GENERATED programs and isn't callable from here. `_i32`/`_u32` have no
// separate functions, matching the reference's parse-wide-then-range-check
// delegation -- see bc_parse_number's TypeKind_I32/U32 arms.
static b32
bc_parse_i64(u8* str, u64 size, i64* out) {
  u64 i = 0; b32 neg = 0;
  if (i < size && (str[i] == '-' || str[i] == '+')) { neg = (str[i] == '-'); i += 1; }
  if (i >= size) return 0; // empty, or just a lone sign
  u64 mag = 0, digit_count = 0;
  for (; i < size; i += 1) {
    u8 c = str[i];
    if (c < '0' || c > '9') return 0; // a non-digit before the string ends -- no partial parse
    // Bound the accumulator BEFORE multiplying. `mag` is a u64, so without
    // this a 20-digit input could wrap past U64_MAX and land back UNDER
    // I64_MAX, sailing through the range check below and reporting ok=true
    // for a magnitude the string never named. 2^63 is the largest either
    // polarity can hold (I64_MIN's), so it is the ceiling to accumulate
    // against; the polarity split is the check after the loop.
    u64 digit = (u64)(c - '0');
    if (mag > (9223372036854775808ull - digit) / 10) return 0;
    mag = mag * 10 + digit;
    digit_count += 1;
    if (digit_count > 20) return 0; // already more digits than any i64 magnitude could need
  }
  if (digit_count == 0) return 0;
  if (!neg && mag > 9223372036854775807ull) return 0; // > I64_MAX; `neg` may use the full 2^63
  // Negated THROUGH u64, not as `-(i64)mag`: the one magnitude only `neg` can
  // hold is 2^63, and `-(i64)2^63` is I64_MIN negated -- undefined behavior,
  // for the single input this branch exists to accept ("-9223372036854775808").
  // Unsigned negation is defined for every value and yields the same bits.
  *out = neg ? (i64)(0ull - mag) : (i64)mag;
  return 1;
}

static b32
bc_parse_u64(u8* str, u64 size, u64* out) {
  u64 i = 0;
  if (i < size && str[i] == '+') i += 1; // no `-` at all -- unsigned
  if (i >= size) return 0;
  u64 mag = 0, digit_count = 0;
  for (; i < size; i += 1) {
    u8 c = str[i];
    if (c < '0' || c > '9') return 0;
    u64 next = mag * 10 + (u64)(c - '0');
    if (next < mag) return 0; // wrapped past U64_MAX
    mag = next;
    digit_count += 1;
  }
  if (digit_count == 0) return 0;
  *out = mag;
  return 1;
}

// `s` is copied into a small, guaranteed-null-terminated stack buffer so
// strtod/strtof never reads past what `s` actually points at (a String8 is
// a fat-pointer VIEW, not guaranteed null-terminated at `s[size]`).
// Anything too long to plausibly be a real numeric literal (>= 64 bytes)
// is rejected outright rather than truncated.
static b32
bc_parse_f64(u8* str, u64 size, f64* out) {
  if (size == 0 || size >= 64) return 0;
  char buf[64];
  MemoryCopy(buf, str, size);
  buf[size] = 0;
  char* endptr = 0;
  f64   v      = strtod(buf, &endptr);
  if (endptr != buf + size) return 0; // trailing garbage, or nothing consumed at all
  *out = v;
  return 1;
}

static b32
bc_parse_f32(u8* str, u64 size, f32* out) {
  if (size == 0 || size >= 64) return 0;
  char buf[64];
  MemoryCopy(buf, str, size);
  buf[size] = 0;
  char* endptr = 0;
  f32   v      = strtof(buf, &endptr);
  if (endptr != buf + size) return 0;
  *out = v;
  return 1;
}

// The one parse behind BOTH BcOp_ParseNumberValue and BcOp_ParseNumberOk.
// Those two opcodes each re-parse the same string (there's no operand slot to
// return a second result through -- see bytecode.h), so the decision about
// whether a string parses, and the value it parses to, have to come from one
// place or the pair can disagree with itself. `*out` comes back in REGISTER
// form: an integer as-is, an f32 as its 32-bit pattern, an f64 as its bits.
//
// A failed parse leaves `*out` ZERO rather than whatever fell out of the
// attempt. runtime/bbb_string.c's bbb_string_to_* all return a zeroed result
// struct on failure, and a program that reads the value without checking the
// ok flag has to see the same thing on both backends. "9999999999999" as an
// i32 is the case that made this concrete: it parses perfectly well as an
// i64 and fails only the narrower range check, so handing back the parsed
// number left the VM printing a truncated 1316134911 where native printed 0.
static b32
bc_parse_number(u8* str, u64 size, TypeKind kind, i64* out) {
  *out = 0;
  switch (kind) {
    case TypeKind_I32: {
      i64 v;
      if (!bc_parse_i64(str, size, &v) || v < -2147483648ll || v > 2147483647ll) return 0;
      *out = v;
    } break;
    case TypeKind_I64: {
      i64 v;
      if (!bc_parse_i64(str, size, &v)) return 0;
      *out = v;
    } break;
    case TypeKind_U32: {
      u64 v;
      if (!bc_parse_u64(str, size, &v) || v > 4294967295ull) return 0;
      *out = (i64)v;
    } break;
    case TypeKind_U64: {
      u64 v;
      if (!bc_parse_u64(str, size, &v)) return 0;
      *out = (i64)v;
    } break;
    case TypeKind_F32: {
      f32 v;
      if (!bc_parse_f32(str, size, &v)) return 0;
      u32 bits; MemoryCopy(&bits, &v, sizeof(bits));
      *out = (i64)(u64)bits;
    } break;
    default: { // TypeKind_F64
      f64 v;
      if (!bc_parse_f64(str, size, &v)) return 0;
      MemoryCopy(out, &v, sizeof(*out));
    } break;
  }
  return 1;
}

// The arena bc_run_in_program takes its register file and frame stack from.
//
// Deliberately NOT ctx_scratch(): a RUNNING program allocates into that same
// arena -- every `(scratch [temp] ...)` block does (BcOp_LoadScratchArena
// boxes ctx_scratch itself), and so does every host import handed one of
// those arenas to put its result in. This allocation has to bracket the whole
// run, so releasing it on the way out would take those allocations with it.
//
// Harmless for the OUTERMOST run, whose results nobody reads afterwards, and
// wrong for a nested one: BcOp_CallModule runs an imported module's function
// through a nested bc_run_in_program, and `(build/getenv temp "PATH")` --
// where `temp` is the CALLER's scratch arena and the callee allocates the
// returned string into it -- came back as freed memory, since the nested
// run's own arena_temp_end popped it.
//
// One arena for the process, temp-marked per invocation: runs nest strictly
// (a nested run always returns before its caller resumes), so mark/release is
// all the discipline the nesting needs. Reserve-backed, so the 544KB a run
// needs is address space until it's touched. File-static like the rest of
// this pipeline's process-wide state (bcgen.c's g_bc_compile_err, script.c's
// g_module_registry): one program at a time, on one thread.
static Arena g_bc_stack;
static b32   g_bc_stack_ready;

static Arena*
bc_stack_arena(void) {
  if (!g_bc_stack_ready) {
    g_bc_stack       = arena_create_vm(MB(256)); // ~470 nested runs before it reports exhaustion
    g_bc_stack_ready = true;
  }
  return &g_bc_stack;
}

#ifdef BC_HAVE_COMPUTED_GOTO
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif

BcResult
bc_run_in_program(BcProgram* prog, u32 fn_index, i64* args, u32 arg_count,
                   Arena* heap, BcHostImportTable* host_imports) {
  BcChunk* entry_chunk = &prog->chunks[fn_index];
  xassert(arg_count == entry_chunk->param_count);

  ArenaTemp temp   = arena_temp_begin(bc_stack_arena());
  i64*      regs   = push_array(temp.arena, i64, BC_MAX_TOTAL_REGISTERS); // uninitialized -- each
                                                                              // frame zeroes only its
                                                                              // OWN window when pushed
  BcFrame*  frames = push_array(temp.arena, BcFrame, BC_MAX_CALL_DEPTH);     // uninitialized -- every
                                                                              // field is set explicitly
                                                                              // whenever a frame is pushed

  xassert(entry_chunk->num_registers <= BC_MAX_TOTAL_REGISTERS); // same ceiling BcOp_Call
                                                                  // enforces for a callee frame
  MemoryZero(regs, entry_chunk->num_registers * sizeof(i64));
  foreach_index(i, arg_count) regs[i] = args[i];

  frames[0].chunk    = entry_chunk;
  frames[0].reg_base = 0;
  frames[0].pc       = 0;
  u32 frame_top     = 0;
  u32 next_free_reg = entry_chunk->num_registers; // one past the highest register any active frame owns

  BcChunk* chunk    = entry_chunk; // the CURRENTLY EXECUTING frame's state, kept in locals for
  u32      reg_base = 0;           // speed and synced into frames[frame_top] only across a push/pop --
  i64*     r        = &regs[0];    // r[i] == regs[reg_base + i], this frame's own registers
  u32      pc       = 0;
  BcInstr  ins;

#ifdef BC_HAVE_COMPUTED_GOTO
  // `static`: label addresses are fixed for every invocation of this same
  // function body, so there's nothing to rebuild per call or per frame.
  // Keyed by designated initializer against the BcOp enum itself rather
  // than positional order, so this can never silently drift out of sync
  // with a reordered/renumbered enum.
  static const void* const bc_dispatch_table[BcOp_Count] = {
    [BcOp_LoadConst]      = &&L_LoadConst,
    [BcOp_Move]           = &&L_Move,
    [BcOp_Add]            = &&L_Add,
    [BcOp_Sub]            = &&L_Sub,
    [BcOp_Mul]            = &&L_Mul,
    [BcOp_Div]            = &&L_Div,
    [BcOp_Mod]            = &&L_Mod,
    [BcOp_Eq]             = &&L_Eq,
    [BcOp_Neq]            = &&L_Neq,
    [BcOp_Lt]             = &&L_Lt,
    [BcOp_Le]             = &&L_Le,
    [BcOp_Gt]             = &&L_Gt,
    [BcOp_Ge]             = &&L_Ge,
    [BcOp_DivU]           = &&L_DivU,
    [BcOp_ModU]           = &&L_ModU,
    [BcOp_LtU]            = &&L_LtU,
    [BcOp_LeU]            = &&L_LeU,
    [BcOp_GtU]            = &&L_GtU,
    [BcOp_GeU]            = &&L_GeU,
    [BcOp_FAdd]           = &&L_FAdd,
    [BcOp_FSub]           = &&L_FSub,
    [BcOp_FMul]           = &&L_FMul,
    [BcOp_FDiv]           = &&L_FDiv,
    [BcOp_FEq]            = &&L_FEq,
    [BcOp_FNeq]           = &&L_FNeq,
    [BcOp_FLt]            = &&L_FLt,
    [BcOp_FLe]            = &&L_FLe,
    [BcOp_FGt]            = &&L_FGt,
    [BcOp_FGe]            = &&L_FGe,
    [BcOp_F32Add]         = &&L_F32Add,
    [BcOp_F32Sub]         = &&L_F32Sub,
    [BcOp_F32Mul]         = &&L_F32Mul,
    [BcOp_F32Div]         = &&L_F32Div,
    [BcOp_F32Eq]          = &&L_F32Eq,
    [BcOp_F32Neq]         = &&L_F32Neq,
    [BcOp_F32Lt]          = &&L_F32Lt,
    [BcOp_F32Le]          = &&L_F32Le,
    [BcOp_F32Gt]          = &&L_F32Gt,
    [BcOp_F32Ge]          = &&L_F32Ge,
    [BcOp_Jump]           = &&L_Jump,
    [BcOp_JumpIfFalse]    = &&L_JumpIfFalse,
    [BcOp_Call]           = &&L_Call,
    [BcOp_CallHost]       = &&L_CallHost,
    [BcOp_CallModule]     = &&L_CallModule,
    [BcOp_Return]         = &&L_Return,
    [BcOp_ReturnVoid]     = &&L_ReturnVoid,
    [BcOp_Alloc]          = &&L_Alloc,
    [BcOp_FieldAddr]      = &&L_FieldAddr,
    [BcOp_LoadFieldI32]   = &&L_LoadFieldI32,
    [BcOp_LoadFieldU32]   = &&L_LoadFieldU32,
    [BcOp_LoadFieldI64]   = &&L_LoadFieldI64,
    [BcOp_LoadFieldF32]   = &&L_LoadFieldF32,
    [BcOp_LoadFieldBool]  = &&L_LoadFieldBool,
    [BcOp_LoadFieldI8]    = &&L_LoadFieldI8,
    [BcOp_LoadFieldU8]    = &&L_LoadFieldU8,
    [BcOp_LoadFieldI16]   = &&L_LoadFieldI16,
    [BcOp_LoadFieldU16]   = &&L_LoadFieldU16,
    [BcOp_StoreFieldI32]  = &&L_StoreFieldI32,
    [BcOp_StoreFieldI64]  = &&L_StoreFieldI64,
    [BcOp_StoreFieldF32]  = &&L_StoreFieldF32,
    [BcOp_StoreFieldBool] = &&L_StoreFieldBool,
    [BcOp_StoreFieldI8]   = &&L_StoreFieldI8,
    [BcOp_StoreFieldI16]  = &&L_StoreFieldI16,
    [BcOp_StrCmp]         = &&L_StrCmp,
    [BcOp_StringMatch]    = &&L_StringMatch,
    [BcOp_MapSet]         = &&L_MapSet,
    [BcOp_MapGet]         = &&L_MapGet,
    [BcOp_MapRemove]      = &&L_MapRemove,
    [BcOp_LoadGlobal]     = &&L_LoadGlobal,
    [BcOp_StoreGlobal]    = &&L_StoreGlobal,
    [BcOp_BitAnd]         = &&L_BitAnd,
    [BcOp_BitOr]          = &&L_BitOr,
    [BcOp_BitXor]         = &&L_BitXor,
    [BcOp_Shl]            = &&L_Shl,
    [BcOp_Shr]            = &&L_Shr,
    [BcOp_ShrU]           = &&L_ShrU,
    [BcOp_Narrow]         = &&L_Narrow,
    [BcOp_BitNot]         = &&L_BitNot,
    [BcOp_IntToF64]       = &&L_IntToF64,
    [BcOp_IntToF32]       = &&L_IntToF32,
    [BcOp_F64ToInt]       = &&L_F64ToInt,
    [BcOp_F32ToInt]       = &&L_F32ToInt,
    [BcOp_F32ToF64]       = &&L_F32ToF64,
    [BcOp_F64ToF32]       = &&L_F64ToF32,
    [BcOp_ArenaCreate]     = &&L_ArenaCreate,
    [BcOp_ArenaDestroy]    = &&L_ArenaDestroy,
    [BcOp_ArenaReset]      = &&L_ArenaReset,
    [BcOp_ArenaRelease]    = &&L_ArenaRelease,
    [BcOp_ArenaMark]       = &&L_ArenaMark,
    [BcOp_ArenaPop]        = &&L_ArenaPop,
    [BcOp_ArenaPush]       = &&L_ArenaPush,
    [BcOp_ArenaPushZero]   = &&L_ArenaPushZero,
    [BcOp_LoadScratchArena] = &&L_LoadScratchArena,
    [BcOp_Malloc]           = &&L_Malloc,
    [BcOp_Free]             = &&L_Free,
    [BcOp_MemSet]           = &&L_MemSet,
    [BcOp_MemCopy]          = &&L_MemCopy,
    [BcOp_MemZero]          = &&L_MemZero,
    [BcOp_MemCompare]       = &&L_MemCompare,
    [BcOp_Sqrt]             = &&L_Sqrt,
    [BcOp_Asin]             = &&L_Asin,
    [BcOp_Acos]             = &&L_Acos,
    [BcOp_Pow]              = &&L_Pow,
    [BcOp_Sin]              = &&L_Sin,
    [BcOp_Cos]              = &&L_Cos,
    [BcOp_Atan2]            = &&L_Atan2,
    [BcOp_F64IsFinite]      = &&L_F64IsFinite,
    [BcOp_HandlePoolSetStride] = &&L_HandlePoolSetStride,
    [BcOp_HandlePoolInit]       = &&L_HandlePoolInit,
    [BcOp_HandleAlloc]           = &&L_HandleAlloc,
    [BcOp_HandleGet]              = &&L_HandleGet,
    [BcOp_HandleFree]             = &&L_HandleFree,
    [BcOp_DynCount]                = &&L_DynCount,
    [BcOp_ParseNumberValue]        = &&L_ParseNumberValue,
    [BcOp_ParseNumberOk]           = &&L_ParseNumberOk,
    [BcOp_DynCapacity]             = &&L_DynCapacity,
    [BcOp_DynGrow]                 = &&L_DynGrow,
    [BcOp_DynSetCount]             = &&L_DynSetCount,
    [BcOp_DynCommit]               = &&L_DynCommit,
    [BcOp_PrintI64]                = &&L_PrintI64,
    [BcOp_PrintU64]                = &&L_PrintU64,
    [BcOp_PrintF64]                = &&L_PrintF64,
    [BcOp_PrintBool]               = &&L_PrintBool,
    [BcOp_PrintChar]               = &&L_PrintChar,
    [BcOp_PrintString]             = &&L_PrintString,
    [BcOp_PrintCString]            = &&L_PrintCString,
  };
#endif

  BC_DISPATCH();

#ifndef BC_HAVE_COMPUTED_GOTO
  bc_dispatch_top:
  ins = chunk->code[pc]; // in switch mode every dispatch funnels through this one fetch, and
                            // BC_DISPATCH() is just the jump back here. Computed-goto mode has to
                            // fetch BEFORE the jump instead, since the target depends on ins.kind.
  switch (ins.kind) {
#endif

    BC_TARGET(LoadConst): { r[ins.a] = chunk->consts[ins.b]; pc += 1; BC_DISPATCH(); }
    BC_TARGET(Move):      { r[ins.a] = r[ins.b];             pc += 1; BC_DISPATCH(); }
    // Arithmetic THROUGH u64, for the same reason Shl/Narrow below already go
    // through it: the VM's integer model is that math WRAPS at the type's
    // width (BcOp_Narrow re-applies the width afterwards), but signed overflow
    // is undefined behavior in C, not wrapping. On i64 registers that is not a
    // corner case -- `(import rng)`'s PCG32 step multiplies by
    // 6364136223846793005 and overflows on its very first call. Unsigned
    // arithmetic is defined to wrap and produces identical bits.
    BC_TARGET(Add):       { r[ins.a] = (i64)((u64)r[ins.b] + (u64)r[ins.c]);  pc += 1; BC_DISPATCH(); }
    BC_TARGET(Sub):       { r[ins.a] = (i64)((u64)r[ins.b] - (u64)r[ins.c]);  pc += 1; BC_DISPATCH(); }
    BC_TARGET(Mul):       { r[ins.a] = (i64)((u64)r[ins.b] * (u64)r[ins.c]);  pc += 1; BC_DISPATCH(); }
    // Zero-divisor check: unlike every other opcode here, a raw C `/` or `%`
    // on i64 traps with SIGFPE instead of producing a value. That would be
    // acceptable on its own -- native codegen crashes there too -- but a QUERY
    // caller such as lib3b.c's hover-eval runs expressions it can't vouch for
    // and must not take the process down. This bails out of bc_run_in_program
    // entirely rather than unwinding per frame; see BcResult.trapped.
    //
    // `chunk` is the frame that was executing, so BC_TRAP names the function
    // the divisor actually went to zero in, not this call's entry point.
    //
    // I64_MIN / -1 is the second divisor that can't just be divided: the true
    // quotient (2^63) has no i64 representation, so C calls it undefined and
    // x86's idiv raises the SAME SIGFPE a zero divisor does. It gets the same
    // treatment for the same reason -- a QUERY caller must not be taken down
    // by an expression it couldn't vouch for.
    BC_TARGET(Div): {
      if (r[ins.c] == 0) BC_TRAP("division by zero");
      if (r[ins.b] == INT64_MIN && r[ins.c] == -1) BC_TRAP("division overflow (I64_MIN / -1)");
      r[ins.a] = r[ins.b] / r[ins.c];
      pc += 1;
      BC_DISPATCH();
    }
    BC_TARGET(Mod): {
      if (r[ins.c] == 0) BC_TRAP("remainder by zero");
      // Mathematically 0, but computed by the same trapping instruction.
      if (r[ins.b] == INT64_MIN && r[ins.c] == -1) { r[ins.a] = 0; pc += 1; BC_DISPATCH(); }
      r[ins.a] = r[ins.b] % r[ins.c];
      pc += 1;
      BC_DISPATCH();
    }
    BC_TARGET(BitAnd):    { r[ins.a] = r[ins.b] & r[ins.c];  pc += 1; BC_DISPATCH(); }
    BC_TARGET(BitOr):     { r[ins.a] = r[ins.b] | r[ins.c];  pc += 1; BC_DISPATCH(); }
    BC_TARGET(BitXor):    { r[ins.a] = r[ins.b] ^ r[ins.c];  pc += 1; BC_DISPATCH(); }
    // Shifts THROUGH u64, not i64: left-shifting a signed value so that a 1
    // bit reaches or crosses the sign-bit position is undefined behavior, and
    // UBSan catches it -- a script's own `(bit-shl 1u64 63u64)` does exactly
    // that, even though the resulting bit pattern is what it legitimately
    // wants. Shifting the u64 reinterpretation is defined for any pattern and yields
    // identical bits once cast back.
    //
    // BC_SHIFT_AMT is the other half of the same problem: a shift COUNT of 64
    // or more is equally undefined, and needs no corrupt input to reach --
    // `(bit-shl 1u64 100u64)` is an ordinary expression a script can write.
    // Masking to 63 is what x86's shift instructions do on their own, so this
    // PINS the behavior both backends already had rather than choosing a new
    // one (native codegen emits a plain C `<<` and inherits the hardware's).
    // What an oversized shift OUGHT to mean is a language question, still open
    // -- masking, yielding 0, and rejecting it are all defensible; native has
    // the same UB until it is answered.
#define BC_SHIFT_AMT(v) ((u64)(v) & 63u)
    BC_TARGET(Shl): { r[ins.a] = (i64)((u64)r[ins.b] << BC_SHIFT_AMT(r[ins.c])); pc += 1; BC_DISPATCH(); }
    BC_TARGET(Shr):       { r[ins.a] = r[ins.b] >> BC_SHIFT_AMT(r[ins.c]); pc += 1; BC_DISPATCH(); }
    // The unsigned counterpart: shifted THROUGH u64 so the vacated high bits
    // come in as zeros, which is what C's `>>` on an unsigned type does.
    BC_TARGET(ShrU): { r[ins.a] = (i64)((u64)r[ins.b] >> BC_SHIFT_AMT(r[ins.c])); pc += 1; BC_DISPATCH(); }
    BC_TARGET(BitNot):    { r[ins.a] = ~r[ins.b];            pc += 1; BC_DISPATCH(); }
    // Truncate to `c`'s width, then re-extend -- all through u64, since
    // shifting a 1 bit into or past an i64's sign position is undefined
    // behavior even when the resulting bit pattern is exactly what's wanted.
    // bcgen.c never emits a width of 64 or 0 (there would be nothing to do),
    // so `mask` is always a real partial mask.
    BC_TARGET(Narrow): {
      u32 width = ins.c & BC_NARROW_WIDTH_MASK;
      u64 mask  = (((u64)1) << width) - 1;
      u64 v     = ((u64)r[ins.b]) & mask;
      if ((ins.c & BC_NARROW_SIGNED) && (v & (((u64)1) << (width - 1)))) v |= ~mask;
      r[ins.a] = (i64)v;
      pc += 1;
      BC_DISPATCH();
    }
    BC_TARGET(Eq):  { r[ins.a] = (r[ins.b] == r[ins.c]) ? 1 : 0; pc += 1; BC_DISPATCH(); }
    BC_TARGET(Neq): { r[ins.a] = (r[ins.b] != r[ins.c]) ? 1 : 0; pc += 1; BC_DISPATCH(); }
    BC_TARGET(Lt):  { r[ins.a] = (r[ins.b] <  r[ins.c]) ? 1 : 0; pc += 1; BC_DISPATCH(); }
    BC_TARGET(Le):  { r[ins.a] = (r[ins.b] <= r[ins.c]) ? 1 : 0; pc += 1; BC_DISPATCH(); }
    BC_TARGET(Gt):  { r[ins.a] = (r[ins.b] >  r[ins.c]) ? 1 : 0; pc += 1; BC_DISPATCH(); }
    BC_TARGET(Ge):  { r[ins.a] = (r[ins.b] >= r[ins.c]) ? 1 : 0; pc += 1; BC_DISPATCH(); }

    // The unsigned halves -- see BcOp_DivU in bytecode.h. DivU/ModU trap on a
    // zero divisor exactly as Div/Mod do; nothing else differs but the cast.
    BC_TARGET(DivU): {
      if (r[ins.c] == 0) BC_TRAP("division by zero");
      r[ins.a] = (i64)((u64)r[ins.b] / (u64)r[ins.c]);
      pc += 1;
      BC_DISPATCH();
    }
    BC_TARGET(ModU): {
      if (r[ins.c] == 0) BC_TRAP("remainder by zero");
      r[ins.a] = (i64)((u64)r[ins.b] % (u64)r[ins.c]);
      pc += 1;
      BC_DISPATCH();
    }
    BC_TARGET(LtU): { r[ins.a] = ((u64)r[ins.b] <  (u64)r[ins.c]) ? 1 : 0; pc += 1; BC_DISPATCH(); }
    BC_TARGET(LeU): { r[ins.a] = ((u64)r[ins.b] <= (u64)r[ins.c]) ? 1 : 0; pc += 1; BC_DISPATCH(); }
    BC_TARGET(GtU): { r[ins.a] = ((u64)r[ins.b] >  (u64)r[ins.c]) ? 1 : 0; pc += 1; BC_DISPATCH(); }
    BC_TARGET(GeU): { r[ins.a] = ((u64)r[ins.b] >= (u64)r[ins.c]) ? 1 : 0; pc += 1; BC_DISPATCH(); }

#define BC_F64_ARITH_CASE(name) \
    BC_TARGET(name): { \
      f64 a, b; \
      MemoryCopy(&a, &r[ins.b], sizeof(a)); \
      MemoryCopy(&b, &r[ins.c], sizeof(b)); \
      r[ins.a] = bc_exec_f64_arith(ins.kind, a, b); \
      pc += 1; \
      BC_DISPATCH(); \
    }
#define BC_F64_COMPARE_CASE(name) \
    BC_TARGET(name): { \
      f64 a, b; \
      MemoryCopy(&a, &r[ins.b], sizeof(a)); \
      MemoryCopy(&b, &r[ins.c], sizeof(b)); \
      r[ins.a] = bc_exec_f64_compare(ins.kind, a, b); \
      pc += 1; \
      BC_DISPATCH(); \
    }
    BC_F64_ARITH_CASE(FAdd) BC_F64_ARITH_CASE(FSub) BC_F64_ARITH_CASE(FMul) BC_F64_ARITH_CASE(FDiv)
    BC_F64_COMPARE_CASE(FEq) BC_F64_COMPARE_CASE(FNeq) BC_F64_COMPARE_CASE(FLt)
    BC_F64_COMPARE_CASE(FLe) BC_F64_COMPARE_CASE(FGt) BC_F64_COMPARE_CASE(FGe)
#undef BC_F64_ARITH_CASE
#undef BC_F64_COMPARE_CASE

#define BC_F32_ARITH_CASE(name) \
    BC_TARGET(name): { \
      u32 abits = (u32)r[ins.b], bbits = (u32)r[ins.c]; \
      f32 a, b; memcpy(&a, &abits, sizeof(a)); memcpy(&b, &bbits, sizeof(b)); \
      r[ins.a] = bc_exec_f32_arith(ins.kind, a, b); \
      pc += 1; \
      BC_DISPATCH(); \
    }
#define BC_F32_COMPARE_CASE(name) \
    BC_TARGET(name): { \
      u32 abits = (u32)r[ins.b], bbits = (u32)r[ins.c]; \
      f32 a, b; memcpy(&a, &abits, sizeof(a)); memcpy(&b, &bbits, sizeof(b)); \
      r[ins.a] = bc_exec_f32_compare(ins.kind, a, b); \
      pc += 1; \
      BC_DISPATCH(); \
    }
    BC_F32_ARITH_CASE(F32Add) BC_F32_ARITH_CASE(F32Sub) BC_F32_ARITH_CASE(F32Mul) BC_F32_ARITH_CASE(F32Div)
    BC_F32_COMPARE_CASE(F32Eq) BC_F32_COMPARE_CASE(F32Neq) BC_F32_COMPARE_CASE(F32Lt)
    BC_F32_COMPARE_CASE(F32Le) BC_F32_COMPARE_CASE(F32Gt) BC_F32_COMPARE_CASE(F32Ge)
#undef BC_F32_ARITH_CASE
#undef BC_F32_COMPARE_CASE

    BC_TARGET(IntToF64): {
      f64 f = (f64)r[ins.b]; // r[ins.b] read as a SIGNED i64 -- see this opcode's own bytecode.h comment
      MemoryCopy(&r[ins.a], &f, sizeof(f));
      pc += 1;
      BC_DISPATCH();
    }
    BC_TARGET(IntToF32): {
      f32 f = (f32)r[ins.b];
      u32 bits; memcpy(&bits, &f, sizeof(bits));
      r[ins.a] = (i64)(u64)bits;
      pc += 1;
      BC_DISPATCH();
    }
    BC_TARGET(F64ToInt): {
      f64 f; MemoryCopy(&f, &r[ins.b], sizeof(f));
      r[ins.a] = (i64)f; // truncates toward zero, matching C's own int-conversion rule
      pc += 1;
      BC_DISPATCH();
    }
    BC_TARGET(F32ToInt): {
      u32 bits = (u32)r[ins.b];
      f32 f; memcpy(&f, &bits, sizeof(f));
      r[ins.a] = (i64)f;
      pc += 1;
      BC_DISPATCH();
    }
    BC_TARGET(F32ToF64): {
      u32 bits = (u32)r[ins.b];
      f32 f32v; memcpy(&f32v, &bits, sizeof(f32v));
      f64 f64v = (f64)f32v;
      MemoryCopy(&r[ins.a], &f64v, sizeof(f64v));
      pc += 1;
      BC_DISPATCH();
    }
    BC_TARGET(F64ToF32): {
      f64 f64v; MemoryCopy(&f64v, &r[ins.b], sizeof(f64v));
      f32 f32v = (f32)f64v;
      u32 bits; memcpy(&bits, &f32v, sizeof(bits));
      r[ins.a] = (i64)(u64)bits;
      pc += 1;
      BC_DISPATCH();
    }

    BC_TARGET(ArenaCreate): {
      Arena a = arena_create_vm((u64)r[ins.b]);
      void* box = arena_push(heap, sizeof(Arena), AlignOf(Arena));
      MemoryCopy(box, &a, sizeof(a));
      r[ins.a] = (i64)(intptr_t)box;
      pc += 1;
      BC_DISPATCH();
    }
    BC_TARGET(ArenaDestroy): { arena_destroy((Arena*)(intptr_t)r[ins.a]); pc += 1; BC_DISPATCH(); }
    BC_TARGET(ArenaReset):   { arena_reset((Arena*)(intptr_t)r[ins.a]);   pc += 1; BC_DISPATCH(); }
    BC_TARGET(ArenaRelease): { arena_release((Arena*)(intptr_t)r[ins.a]); pc += 1; BC_DISPATCH(); }
    BC_TARGET(ArenaMark): {
      ArenaMark m = arena_mark((Arena*)(intptr_t)r[ins.b]);
      r[ins.a] = (i64)(intptr_t)m.at;
      pc += 1;
      BC_DISPATCH();
    }
    BC_TARGET(ArenaPop): {
      ArenaMark m; m.at = (void*)(intptr_t)r[ins.b];
      arena_pop((Arena*)(intptr_t)r[ins.a], m);
      pc += 1;
      BC_DISPATCH();
    }
    BC_TARGET(ArenaPush): {
      // Fixed 16-byte alignment -- see this opcode's own bytecode.h comment.
      void* p = arena_push((Arena*)(intptr_t)r[ins.b], (u64)r[ins.c], 16);
      r[ins.a] = (i64)(intptr_t)p;
      pc += 1;
      BC_DISPATCH();
    }
    BC_TARGET(ArenaPushZero): {
      void* p = arena_push_zero((Arena*)(intptr_t)r[ins.b], (u64)r[ins.c], 16);
      r[ins.a] = (i64)(intptr_t)p;
      pc += 1;
      BC_DISPATCH();
    }
    BC_TARGET(LoadScratchArena): {
      Arena* scratch = ctx_scratch();
      void*  box      = arena_push(heap, sizeof(Arena), AlignOf(Arena));
      MemoryCopy(box, scratch, sizeof(Arena));
      r[ins.a] = (i64)(intptr_t)box;
      pc += 1;
      BC_DISPATCH();
    }
    BC_TARGET(Malloc): { r[ins.a] = (i64)(intptr_t)malloc((size_t)r[ins.b]); pc += 1; BC_DISPATCH(); }
    BC_TARGET(Free):   { free((void*)(intptr_t)r[ins.a]);                    pc += 1; BC_DISPATCH(); }

    BC_TARGET(MemSet):  { memset((void*)(intptr_t)r[ins.a], (int)r[ins.b], (size_t)r[ins.c]);            pc += 1; BC_DISPATCH(); }
    BC_TARGET(MemCopy): { memmove((void*)(intptr_t)r[ins.a], (void*)(intptr_t)r[ins.b], (size_t)r[ins.c]); pc += 1; BC_DISPATCH(); }
    BC_TARGET(MemZero): { memset((void*)(intptr_t)r[ins.a], 0, (size_t)r[ins.b]);                        pc += 1; BC_DISPATCH(); }
    BC_TARGET(MemCompare): {
      // Three inputs + a destination don't fit an instruction's a/b/c, so
      // the inputs live in the contiguous block at `c` -- see this opcode's
      // own bytecode.h comment.
      r[ins.a] = (i64)memcmp((void*)(intptr_t)r[ins.c + 0], (void*)(intptr_t)r[ins.c + 1],
                             (size_t)r[ins.c + 2]);
      pc += 1;
      BC_DISPATCH();
    }

    BC_TARGET(Sqrt): { f64 x; MemoryCopy(&x, &r[ins.b], sizeof(x)); f64 y = sqrt(x);  MemoryCopy(&r[ins.a], &y, sizeof(y)); pc += 1; BC_DISPATCH(); }
    BC_TARGET(Asin): { f64 x; MemoryCopy(&x, &r[ins.b], sizeof(x)); f64 y = asin(x);  MemoryCopy(&r[ins.a], &y, sizeof(y)); pc += 1; BC_DISPATCH(); }
    BC_TARGET(Acos): { f64 x; MemoryCopy(&x, &r[ins.b], sizeof(x)); f64 y = acos(x);  MemoryCopy(&r[ins.a], &y, sizeof(y)); pc += 1; BC_DISPATCH(); }
    BC_TARGET(Pow): {
      f64 base, exp;
      MemoryCopy(&base, &r[ins.b], sizeof(base));
      MemoryCopy(&exp, &r[ins.c], sizeof(exp));
      f64 y = pow(base, exp);
      MemoryCopy(&r[ins.a], &y, sizeof(y));
      pc += 1;
      BC_DISPATCH();
    }
    BC_TARGET(F64IsFinite): {
      f64 x; MemoryCopy(&x, &r[ins.b], sizeof(x));
      r[ins.a] = isfinite(x) ? 1 : 0;
      pc += 1;
      BC_DISPATCH();
    }
    BC_TARGET(Sin): { f64 x; MemoryCopy(&x, &r[ins.b], sizeof(x)); f64 y = sin(x); MemoryCopy(&r[ins.a], &y, sizeof(y)); pc += 1; BC_DISPATCH(); }
    BC_TARGET(Cos): { f64 x; MemoryCopy(&x, &r[ins.b], sizeof(x)); f64 y = cos(x); MemoryCopy(&r[ins.a], &y, sizeof(y)); pc += 1; BC_DISPATCH(); }
    BC_TARGET(Atan2): {
      f64 y, x;
      MemoryCopy(&y, &r[ins.b], sizeof(y));
      MemoryCopy(&x, &r[ins.c], sizeof(x));
      f64 result = atan2(y, x);
      MemoryCopy(&r[ins.a], &result, sizeof(result));
      pc += 1;
      BC_DISPATCH();
    }

    BC_TARGET(HandlePoolSetStride): {
      HandlePool* pool = (HandlePool*)(intptr_t)r[ins.a];
      pool->stride       = (u32)r[ins.b];
      pc += 1;
      BC_DISPATCH();
    }
    BC_TARGET(HandlePoolInit): {
      // DEFINE_HANDLE_POOL's Prefix_pool_init, generically: `pool->stride`
      // already holds sizeof(T), set by BcOp_HandlePoolSetStride just before
      // this, so no per-type information is needed. `pool->data`'s alignment
      // is fixed at 16 bytes, the same simplification BcOp_ArenaPush makes.
      HandlePool* pool     = (HandlePool*)(intptr_t)r[ins.a];
      Arena*      pool_arena = (Arena*)(intptr_t)r[ins.b];
      u32         capacity   = (u32)r[ins.c];
      pool->data       = arena_push_zero(pool_arena, (u64)pool->stride * capacity, 16);
      pool->generation = (u32*)arena_push_zero(pool_arena, sizeof(u32) * (u64)capacity, AlignOf(u32));
      pool->free_list  = (u32*)arena_push(pool_arena, sizeof(u32) * (u64)capacity, AlignOf(u32));
      pool->capacity   = capacity;
      pool->count      = 0;
      // slot 0 reserved as null -- fill free list with 1..capacity-1
      pool->free_count = capacity - 1;
      for (u32 i = 0; i < capacity - 1; i += 1) pool->free_list[i] = capacity - 1 - i; // pop gives 1,2,3,...
      // generation starts at 1 so the first valid handle is never zero
      for (u32 i = 1; i < capacity; i += 1) pool->generation[i] = 1;
      pc += 1;
      BC_DISPATCH();
    }
    BC_TARGET(HandleAlloc): {
      HandlePool* pool = (HandlePool*)(intptr_t)r[ins.b];
      Handle h = {0};
      if (pool->free_count == 0) {
        fprintf(stderr, "handle-alloc: pool full\n");
      } else {
        u32 idx = pool->free_list[--pool->free_count];
        pool->count += 1;
        h.index      = idx;
        h.generation = pool->generation[idx];
      }
      MemoryCopy(&r[ins.a], &h, sizeof(h));
      pc += 1;
      BC_DISPATCH();
    }
    BC_TARGET(HandleGet): {
      HandlePool* pool = (HandlePool*)(intptr_t)r[ins.b];
      Handle h; MemoryCopy(&h, &r[ins.c], sizeof(h));
      void* result = NULL;
      if (h.index != 0 && h.index < pool->capacity && pool->generation[h.index] == h.generation) {
        result = (u8*)pool->data + (u64)h.index * pool->stride;
      }
      r[ins.a] = (i64)(intptr_t)result;
      pc += 1;
      BC_DISPATCH();
    }
    BC_TARGET(HandleFree): {
      HandlePool* pool = (HandlePool*)(intptr_t)r[ins.b];
      Handle h; MemoryCopy(&h, &r[ins.c], sizeof(h));
      i64 ok = 0;
      if (h.index != 0 && h.index < pool->capacity && pool->generation[h.index] == h.generation) {
        MemoryZero((u8*)pool->data + (u64)h.index * pool->stride, pool->stride);
        pool->generation[h.index] += 1; // invalidate all existing handles to this slot
        pool->free_list[pool->free_count] = h.index;
        pool->free_count += 1;
        pool->count -= 1;
        ok = 1;
      }
      r[ins.a] = ok;
      pc += 1;
      BC_DISPATCH();
    }
    BC_TARGET(DynCount): {
      void* p = (void*)(intptr_t)r[ins.b];
      r[ins.a] = (i64)dyn_count(p);
      pc += 1;
      BC_DISPATCH();
    }

    BC_TARGET(ParseNumberValue): {
      u8* hdr = (u8*)(intptr_t)r[ins.b];
      u8* str;  MemoryCopy(&str,  hdr + 0, sizeof(str));
      u64 size; MemoryCopy(&size, hdr + 8, sizeof(size));
      i64 result;
      bc_parse_number(str, size, (TypeKind)ins.c, &result); // 0 on failure, see bc_parse_number
      r[ins.a] = result;
      pc += 1;
      BC_DISPATCH();
    }
    BC_TARGET(ParseNumberOk): {
      u8* hdr = (u8*)(intptr_t)r[ins.b];
      u8* str;  MemoryCopy(&str,  hdr + 0, sizeof(str));
      u64 size; MemoryCopy(&size, hdr + 8, sizeof(size));
      i64 ignored;
      r[ins.a] = bc_parse_number(str, size, (TypeKind)ins.c, &ignored) ? 1 : 0;
      pc += 1;
      BC_DISPATCH();
    }

    BC_TARGET(DynCapacity): {
      void* p = (void*)(intptr_t)r[ins.b];
      r[ins.a] = (i64)dyn_capacity(p);
      pc += 1;
      BC_DISPATCH();
    }
    BC_TARGET(DynGrow): {
      // Reimplements base.h's own `arena_dyn_grow` exactly -- see that
      // function and this opcode's own bytecode.h comment.
      Arena* arena     = (Arena*)(intptr_t)r[ins.b];
      void*  old_ptr   = (void*)(intptr_t)r[ins.a];
      u64    elem_size = (u64)ins.c;
      u64    old_cap   = old_ptr ? dyn_capacity(old_ptr) : 0;
      u64    new_cap   = old_cap ? old_cap * 2 : 8;
      u64    hdr_size  = sizeof(DynHdr);
      u64    old_bytes = hdr_size + old_cap * elem_size;
      u64    new_bytes = hdr_size + new_cap * elem_size;
      // Fixed 16-byte alignment -- see this opcode's own bytecode.h comment.
      void* new_mem = arena_push(arena, new_bytes, 16);
      DynHdr* new_hdr = (DynHdr*)new_mem;
      if (old_ptr) MemoryCopy(new_mem, dyn_hdr(old_ptr), Min(old_bytes, new_bytes));
      else         new_hdr->count = 0;
      new_hdr->capacity = new_cap;
      r[ins.a] = (i64)(intptr_t)((u8*)new_mem + hdr_size);
      pc += 1;
      BC_DISPATCH();
    }
    BC_TARGET(DynSetCount): {
      void* p = (void*)(intptr_t)r[ins.a];
      dyn_hdr(p)->count = (u64)r[ins.c];
      pc += 1;
      BC_DISPATCH();
    }
    BC_TARGET(DynCommit): {
      Arena* dst_arena = (Arena*)(intptr_t)r[ins.b];
      void*  src        = (void*)(intptr_t)r[ins.a];
      u64    elem_size   = (u64)ins.c;
      u64    cnt         = src ? dyn_count(src) : 0;
      void*  result      = NULL;
      if (cnt != 0) {
        u64     hdr_size    = sizeof(DynHdr);
        u64     total_bytes = hdr_size + cnt * elem_size;
        void*   new_mem      = arena_push(dst_arena, total_bytes, 16); // fixed align, same as DynGrow
        DynHdr* new_hdr      = (DynHdr*)new_mem;
        new_hdr->capacity    = cnt;
        new_hdr->count       = cnt;
        void* new_data       = (u8*)new_mem + hdr_size;
        MemoryCopy(new_data, src, cnt * elem_size);
        result = new_data;
      }
      r[ins.a] = (i64)(intptr_t)result;
      pc += 1;
      BC_DISPATCH();
    }

    // Every Print* writes to BC_PRINT_OUT rather than stdout directly: `c` set
    // means `b` holds a `stream` register (a `FILE*`, see bcosprims.c) and
    // this is a `(print s "..." ...)` redirected to it. A NULL stream -- one
    // whose `open` failed unchecked -- falls back to stdout rather than
    // handing fprintf a NULL FILE*.
#define BC_PRINT_OUT (ins.c && (FILE*)(intptr_t)r[ins.b] ? (FILE*)(intptr_t)r[ins.b] : stdout)
    BC_TARGET(PrintI64):  { fprintf(BC_PRINT_OUT, "%lld", (long long)r[ins.a]);          pc += 1; BC_DISPATCH(); }
    BC_TARGET(PrintU64):  { fprintf(BC_PRINT_OUT, "%llu", (unsigned long long)r[ins.a]); pc += 1; BC_DISPATCH(); }
    BC_TARGET(PrintF64):  {
      f64 v; MemoryCopy(&v, &r[ins.a], sizeof(v));
      fprintf(BC_PRINT_OUT, "%g", v);
      pc += 1;
      BC_DISPATCH();
    }
    BC_TARGET(PrintBool): { fprintf(BC_PRINT_OUT, "%s", r[ins.a] ? "true" : "false"); pc += 1; BC_DISPATCH(); }
    BC_TARGET(PrintChar): { fprintf(BC_PRINT_OUT, "%c", (int)r[ins.a]);               pc += 1; BC_DISPATCH(); }
    BC_TARGET(PrintString): {
      u8* hdr = (u8*)(intptr_t)r[ins.a];
      u8* str;  MemoryCopy(&str,  hdr + 0, sizeof(str));
      u64 size; MemoryCopy(&size, hdr + 8, sizeof(size));
      fwrite(str, 1, size, BC_PRINT_OUT);
      pc += 1;
      BC_DISPATCH();
    }
    BC_TARGET(PrintCString): { fprintf(BC_PRINT_OUT, "%s", (char*)(intptr_t)r[ins.a]); pc += 1; BC_DISPATCH(); }
#undef BC_PRINT_OUT

    BC_TARGET(Jump):        { pc = ins.a; BC_DISPATCH(); }
    BC_TARGET(JumpIfFalse): { pc = (r[ins.a] == 0) ? ins.b : pc + 1; BC_DISPATCH(); }

    BC_TARGET(Call): {
      BcChunk* callee = &prog->chunks[ins.b];
      xassert(frame_top + 1 < BC_MAX_CALL_DEPTH); // a VM-level recursion limit, never a native
                                                      // C stack overflow
      xassert((u64)next_free_reg + callee->num_registers <= BC_MAX_TOTAL_REGISTERS);

      // Save THIS (the caller's) live pc back into its own frame slot --
      // reg_base/chunk never change for an already-pushed frame, so only
      // pc needs saving/restoring across a push/pop.
      frames[frame_top].pc = pc;

      u32 callee_reg_base = next_free_reg;
      MemoryZero(&regs[callee_reg_base], callee->num_registers * sizeof(i64));
      // Copy args -- bcgen.c already placed them contiguously at
      // r[ins.c .. ins.c+callee->param_count) before emitting this Call.
      foreach_index(i, callee->param_count) regs[callee_reg_base + i] = r[ins.c + i];

      frame_top += 1;
      frames[frame_top].chunk    = callee;
      frames[frame_top].reg_base = callee_reg_base;
      frames[frame_top].pc       = 0;
      frames[frame_top].dst_reg  = reg_base + ins.a; // absolute -- already relative to the CALLER's
                                                         // own reg_base, exactly what regs[] expects
      next_free_reg += callee->num_registers;

      // Switch every "current frame" local over to the callee.
      chunk    = callee;
      reg_base = callee_reg_base;
      r        = &regs[reg_base];
      pc       = 0;
      BC_DISPATCH();
    }
    BC_TARGET(CallHost): {
      BcHostImport* imp = &host_imports->entries[ins.b];
      r[ins.a] = (imp->kind == BcHostImportKind_Direct)
        ? bc_call_native_direct(imp->native_fn, &r[ins.c], imp->arg_count)
        : imp->fn(&r[ins.c], imp->arg_count, heap, host_imports->userdata);
      pc += 1;
      BC_DISPATCH();
    }
    BC_TARGET(CallModule): {
      // A cross-package `.3bs` call into a separately-compiled BcProgram. This
      // genuinely recurses into a fresh bc_run_in_program, the one exception to
      // this function's explicit-frame-stack design -- see BcOp_CallModule in
      // bytecode.h for why that's safe. Args are already placed contiguously
      // at r[ins.c ...], as for BcOp_Call.
      //
      // `host_imports` passes through UNCHANGED: the imported module's own
      // BcOp_CallHost operands were baked in against this same table at its
      // compile time, and script.c's module resolution requires every module
      // and the importing script to share one table. Passing anything else
      // resolves those operands against the wrong table.
      BcModuleImport* imp = &prog->module_table->entries[ins.b];
      BcChunk*        target_chunk = &imp->prog->chunks[imp->fn_index];
      BcResult        result = bc_run_in_program(imp->prog, imp->fn_index, &r[ins.c],
                                                  target_chunk->param_count, heap, host_imports);
      // A trap in the callee has to keep unwinding. `result.value` is
      // meaningless when it's set (BcResult.trapped), so storing it would
      // resume this frame with a plain 0 in r[ins.a] -- the divide-by-zero
      // deep inside an imported module reaching its caller as a legitimate
      // "the answer is 0". Returned as-is rather than re-raised, so
      // trap_message/trap_fn keep naming the module function that trapped
      // instead of this call site. That borrows from `imp->prog`, which the
      // module table owns and which outlives this program by construction.
      if (result.trapped) { arena_temp_end(&temp); return result; }
      r[ins.a] = result.value;
      pc += 1;
      BC_DISPATCH();
    }

    BC_TARGET(Alloc): {
      void* ptr = arena_push_zero(heap, (u64)ins.b, (u64)ins.c);
      r[ins.a] = (i64)(intptr_t)ptr;
      pc += 1;
      BC_DISPATCH();
    }
    BC_TARGET(FieldAddr): {
      u8* base = (u8*)(intptr_t)r[ins.b];
      r[ins.a] = (i64)(intptr_t)(base + ins.c);
      pc += 1;
      BC_DISPATCH();
    }
    BC_TARGET(LoadFieldI32): {
      u8* base = (u8*)(intptr_t)r[ins.b];
      i32 v; memcpy(&v, base + ins.c, sizeof(v));
      r[ins.a] = (i64)v;
      pc += 1;
      BC_DISPATCH();
    }
    BC_TARGET(LoadFieldU32): {
      u8* base = (u8*)(intptr_t)r[ins.b];
      u32 v; memcpy(&v, base + ins.c, sizeof(v));
      r[ins.a] = (i64)(u64)v; // zero-extended, unlike LoadFieldI32's sign-extension
      pc += 1;
      BC_DISPATCH();
    }
    BC_TARGET(LoadFieldI64): {
      u8* base = (u8*)(intptr_t)r[ins.b];
      i64 v; memcpy(&v, base + ins.c, sizeof(v));
      r[ins.a] = v;
      pc += 1;
      BC_DISPATCH();
    }
    BC_TARGET(LoadFieldF32): {
      u8* base = (u8*)(intptr_t)r[ins.b];
      u32 bits; memcpy(&bits, base + ins.c, sizeof(bits));
      r[ins.a] = (i64)(u64)bits; // zero-extended -- only the low 4 bytes are ever read back
      pc += 1;
      BC_DISPATCH();
    }
    BC_TARGET(LoadFieldBool): {
      u8* base = (u8*)(intptr_t)r[ins.b];
      r[ins.a] = base[ins.c] != 0 ? 1 : 0;
      pc += 1;
      BC_DISPATCH();
    }
    BC_TARGET(LoadFieldI8): {
      u8* base = (u8*)(intptr_t)r[ins.b];
      i8  v; memcpy(&v, base + ins.c, sizeof(v));
      r[ins.a] = (i64)v; // sign-extended -- also used for `char` fields, see bytecode.h's own note
      pc += 1;
      BC_DISPATCH();
    }
    BC_TARGET(LoadFieldU8): {
      u8* base = (u8*)(intptr_t)r[ins.b];
      u8  v; memcpy(&v, base + ins.c, sizeof(v));
      r[ins.a] = (i64)(u64)v; // zero-extended
      pc += 1;
      BC_DISPATCH();
    }
    BC_TARGET(LoadFieldI16): {
      u8* base = (u8*)(intptr_t)r[ins.b];
      i16 v; memcpy(&v, base + ins.c, sizeof(v));
      r[ins.a] = (i64)v; // sign-extended
      pc += 1;
      BC_DISPATCH();
    }
    BC_TARGET(LoadFieldU16): {
      u8* base = (u8*)(intptr_t)r[ins.b];
      u16 v; memcpy(&v, base + ins.c, sizeof(v));
      r[ins.a] = (i64)(u64)v; // zero-extended
      pc += 1;
      BC_DISPATCH();
    }
    BC_TARGET(StoreFieldI32): {
      u8* base = (u8*)(intptr_t)r[ins.a];
      i32 v = (i32)r[ins.c];
      memcpy(base + ins.b, &v, sizeof(v));
      pc += 1;
      BC_DISPATCH();
    }
    BC_TARGET(StoreFieldI64): {
      u8* base = (u8*)(intptr_t)r[ins.a];
      i64 v = r[ins.c];
      memcpy(base + ins.b, &v, sizeof(v));
      pc += 1;
      BC_DISPATCH();
    }
    BC_TARGET(StoreFieldF32): {
      u8* base = (u8*)(intptr_t)r[ins.a];
      u32 bits = (u32)r[ins.c]; // low 4 bytes of the register hold the f32 bit pattern
      memcpy(base + ins.b, &bits, sizeof(bits));
      pc += 1;
      BC_DISPATCH();
    }
    BC_TARGET(StoreFieldBool): {
      u8* base = (u8*)(intptr_t)r[ins.a];
      base[ins.b] = (r[ins.c] != 0) ? 1 : 0;
      pc += 1;
      BC_DISPATCH();
    }
    BC_TARGET(StoreFieldI8): {
      u8* base = (u8*)(intptr_t)r[ins.a];
      i8  v = (i8)r[ins.c]; // also used for u8/char fields -- no sign/zero distinction on a store
      memcpy(base + ins.b, &v, sizeof(v));
      pc += 1;
      BC_DISPATCH();
    }
    BC_TARGET(StoreFieldI16): {
      u8* base = (u8*)(intptr_t)r[ins.a];
      i16 v = (i16)r[ins.c]; // also used for u16 fields
      memcpy(base + ins.b, &v, sizeof(v));
      pc += 1;
      BC_DISPATCH();
    }
    BC_TARGET(StrCmp): {
      // Inline rather than calling runtime/bbb_string.c's bbb_str8_compare:
      // that file is embedded into GENERATED programs, not linked into the
      // compiler/interpreter itself. Matches its exact semantics -- a
      // byte-by-byte `(i32)a[i] - (i32)b[i]` diff up to the shorter length,
      // then a shorter-is-less size tiebreak when one is a prefix of the other.
      u8* hdr_a = (u8*)(intptr_t)r[ins.b];
      u8* hdr_b = (u8*)(intptr_t)r[ins.c];
      u8* str_a;  u64 size_a; memcpy(&str_a, hdr_a + 0, sizeof(str_a)); memcpy(&size_a, hdr_a + 8, sizeof(size_a));
      u8* str_b;  u64 size_b; memcpy(&str_b, hdr_b + 0, sizeof(str_b)); memcpy(&size_b, hdr_b + 8, sizeof(size_b));
      u64 min_size = size_a < size_b ? size_a : size_b;
      i32 result = 0;
      foreach_index(i, min_size) {
        i32 diff = (i32)str_a[i] - (i32)str_b[i];
        if (diff != 0) { result = diff; break; }
      }
      if (result == 0 && size_a != size_b) result = (size_a < size_b) ? -1 : 1;
      r[ins.a] = (i64)result;
      pc += 1;
      BC_DISPATCH();
    }
    BC_TARGET(StringMatch): {
      // Unlike StrCmp, this calls base.h's real str8_match: that one IS
      // compiler-side infrastructure, not a generated-program-only runtime
      // file. `ins.b` names a 3-register block (str_a_addr, str_b_addr, flags).
      u8* hdr_a = (u8*)(intptr_t)r[ins.b + 0];
      u8* hdr_b = (u8*)(intptr_t)r[ins.b + 1];
      String8 a; memcpy(&a.str, hdr_a + 0, sizeof(a.str)); memcpy(&a.size, hdr_a + 8, sizeof(a.size));
      String8 b; memcpy(&b.str, hdr_b + 0, sizeof(b.str)); memcpy(&b.size, hdr_b + 8, sizeof(b.size));
      StringMatchFlags flags = (StringMatchFlags)r[ins.b + 2];
      r[ins.a] = (i64)str8_match(a, b, flags);
      pc += 1;
      BC_DISPATCH();
    }
    BC_TARGET(MapSet): {
      // `ins.b` names a 5-register block: {map_addr, arena_addr, key_ptr,
      // value_ptr, layout_ptr}. The instance's address doubles as a
      // BcHashInstance*, and the layout descriptor is a real pointer baked
      // into the const pool at compile time.
      BcHashInstance*         m      = (BcHashInstance*)(intptr_t)r[ins.b + 0];
      Arena*                   arena  = (Arena*)(intptr_t)r[ins.b + 1];
      void*                    key_p  = (void*)(intptr_t)r[ins.b + 2];
      void*                    val_p  = (void*)(intptr_t)r[ins.b + 3];
      const BcHashSlotLayout* layout = (const BcHashSlotLayout*)(intptr_t)r[ins.b + 4];
      r[ins.a] = (i64)bc_map_set(arena, m, layout, key_p, val_p);
      pc += 1;
      BC_DISPATCH();
    }
    BC_TARGET(MapGet): {
      // `ins.b` names a 3-register block: {map_addr, key_ptr, layout_ptr}.
      BcHashInstance*         m      = (BcHashInstance*)(intptr_t)r[ins.b + 0];
      void*                    key_p  = (void*)(intptr_t)r[ins.b + 1];
      const BcHashSlotLayout* layout = (const BcHashSlotLayout*)(intptr_t)r[ins.b + 2];
      r[ins.a] = (i64)(intptr_t)bc_map_get(m, layout, key_p);
      pc += 1;
      BC_DISPATCH();
    }
    BC_TARGET(MapRemove): {
      // Same 3-register block shape as MapGet.
      BcHashInstance*         m      = (BcHashInstance*)(intptr_t)r[ins.b + 0];
      void*                    key_p  = (void*)(intptr_t)r[ins.b + 1];
      const BcHashSlotLayout* layout = (const BcHashSlotLayout*)(intptr_t)r[ins.b + 2];
      r[ins.a] = (i64)bc_map_remove(m, layout, key_p);
      pc += 1;
      BC_DISPATCH();
    }
    BC_TARGET(LoadGlobal): { r[ins.a] = prog->globals[ins.b]; pc += 1; BC_DISPATCH(); }
    BC_TARGET(StoreGlobal): { prog->globals[ins.a] = r[ins.b]; pc += 1; BC_DISPATCH(); }

    BC_TARGET(Return):
    BC_TARGET(ReturnVoid): {
      b32 has_value = (ins.kind == BcOp_Return);
      i64 result    = has_value ? r[ins.a] : 0;

      if (frame_top == 0) { // the outermost frame returned -- this call is done
        arena_temp_end(&temp);
        return (BcResult){ .has_value = has_value, .value = result };
      }

      // Pop back to the caller: release this frame's register window, write the
      // result into the caller's already-absolute destination register, then
      // restore the caller's chunk/reg_base/pc from its frame slot.
      next_free_reg = reg_base;
      u32 dst_abs   = frames[frame_top].dst_reg;
      frame_top    -= 1;
      regs[dst_abs] = result;

      chunk    = frames[frame_top].chunk;
      reg_base = frames[frame_top].reg_base;
      r        = &regs[reg_base];
      pc       = frames[frame_top].pc + 1; // resume just PAST the Call instruction
      BC_DISPATCH();
    }

#ifndef BC_HAVE_COMPUTED_GOTO
    default: {
      xassert(!"bc_run_in_program: unknown opcode");
      arena_temp_end(&temp);
      return (BcResult){0};
    }
  }
#endif
}

#ifdef BC_HAVE_COMPUTED_GOTO
#pragma GCC diagnostic pop
#endif
