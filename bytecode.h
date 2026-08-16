#ifndef BYTECODE_H
#define BYTECODE_H
#include "3b.h"
#include "bcmap.h" // BcHashSlotLayout -- a Map/Set call site bakes one into the const pool, so
                      // BcLayoutFixup below has to hold one BY VALUE (see its own comment)

// The register-machine instruction set bcgen.c compiles to and bcvm.c
// interprets. Registers are plain i64 slots; every other representation is
// layered on top of that:
//
//  - a float lives in a register as its own reinterpreted bit pattern (f32
//    in the low 4 bytes), so the const pool needs no separate float section;
//  - a struct, string or Arena value is EMBEDDED -- the register holds the
//    ADDRESS of its bytes, whether the source type is `T` or `T*`;
//  - any 8-byte scalar-shaped value (Handle, ArenaMark, Pointer, Vector)
//    lives directly in the register, unboxed.
//
// `a`/`b`/`c` mean different things per opcode, documented below. Several
// opcodes need more than three operands; those follow the convention
// BcOp_Call establishes -- one operand names the START of a contiguous
// register block holding the inputs, and the remaining operand is unused.
typedef enum BcOp {
  BcOp_LoadConst,    // regs[a] = consts[b]
  BcOp_Move,         // regs[a] = regs[b]
  BcOp_Add, BcOp_Sub, BcOp_Mul, BcOp_Div, BcOp_Mod, // regs[a] = regs[b] OP regs[c]  (SIGNED integer)
  BcOp_Eq, BcOp_Neq, BcOp_Lt, BcOp_Le, BcOp_Gt, BcOp_Ge, // regs[a] = (regs[b] OP regs[c]) ? 1 : 0  (SIGNED)
  // The unsigned halves of the ops above that have one, reading both
  // registers as u64. Same story as BcOp_ShrU below, and reached the same
  // way -- bcgen.c picks by the operands' static type -- but for a
  // different reason: ShrU is about which bits come IN, these are about how
  // the bits already there are READ. Only a u64 (or a Pointer, an address C
  // orders unsigned) needs them; every narrower unsigned type is held
  // zero-extended, hence never negative, hence already right under the
  // signed ops. Add/Sub/Mul need no pair -- bit-identical in two's
  // complement -- and neither does Eq/Neq.
  //
  // Without these, `(/ big 3u64)` on a u64 past 2^63 divides a NEGATIVE
  // number here while codegen.c's C divides a large positive one: U64_MAX/3
  // came back 0 instead of 6148914691236517205.
  BcOp_DivU, BcOp_ModU,
  BcOp_LtU, BcOp_LeU, BcOp_GtU, BcOp_GeU,
  BcOp_FAdd, BcOp_FSub, BcOp_FMul, BcOp_FDiv, // regs[a] = regs[b] OP regs[c] as f64 bit patterns.
                                                 // No BcOp_FMod: the checker requires integer
                                                 // operands for `%`.
  BcOp_FEq, BcOp_FNeq, BcOp_FLt, BcOp_FLe, BcOp_FGt, BcOp_FGe, // regs[a] = (f64 regs[b] OP f64 regs[c]) ? 1 : 0
  BcOp_F32Add, BcOp_F32Sub, BcOp_F32Mul, BcOp_F32Div, // as BcOp_FAdd etc, but on the LOW 4 bytes of
                                                          // each register as an f32 -- real f32
                                                          // precision, not widened to double
  BcOp_F32Eq, BcOp_F32Neq, BcOp_F32Lt, BcOp_F32Le, BcOp_F32Gt, BcOp_F32Ge,
  BcOp_Jump,         // pc = a (absolute instruction index)
  BcOp_JumpIfFalse,  // pc = (regs[a] == 0) ? b : pc+1
  BcOp_Call,         // regs[a] = result of calling program-chunk index b, with args already placed
                        // by bcgen.c into regs[c .. c+callee->param_count)
  BcOp_CallHost,     // regs[a] = result of calling host-import index b (see BcHostImportTable),
                        // args in regs[c .. c+that import's arg_count)
  BcOp_CallModule,   // regs[a] = result of calling module-import index b (see BcModuleTable), args
                        // in regs[c .. c+that module fn's param_count). Unlike BcOp_Call (which
                        // reuses the current bc_run_in_program invocation's own explicit frame
                        // stack), this recurses into a fresh bc_run_in_program on the OTHER
                        // program: the two programs have different chunk/register-count universes,
                        // so they can't share one frame stack. Safe because each invocation's
                        // frame/register arrays are its own locals, not shared state.
  BcOp_Return,       // returns regs[a] to the caller
  BcOp_ReturnVoid,   // returns no value

  // ~~ Struct field access. bcgen.c wires layout.c's byte offsets straight
  // into the `c`/`b` offset operands. Which field widths share which opcode:
  //
  //  - i64/u64/f64/handle/pointer/Vector: all BcOp_*FieldI64 (an 8-byte
  //    access fills the whole register, nothing to extend).
  //  - i32 and u32 share the STORE op (writing back 4 bytes doesn't care
  //    about signedness) but need separate LOADs, since sign- vs
  //    zero-extension into the wider register differ for a u32 with its top
  //    bit set. Same split for i8/u8 and i16/u16.
  //  - `char` is plain C `char`, signed on this codebase's only target
  //    (x86-64 Linux), so it reuses I8's sign-extending load.
  //  - a nested by-value struct, string, or array field is wider than one
  //    register and embedded inline: no opcode reads it as a unit, it's
  //    reached via BcOp_FieldAddr's pointer arithmetic instead.
  //
  // Array indexing (into an embedded array or a raw pointer) has no
  // dedicated opcode: plain BcOp_Mul/BcOp_Add compute `base + index *
  // compile-time stride`, then whichever load/store above matches the
  // element width.
  BcOp_Alloc,          // regs[a] = a fresh, zeroed, b-byte allocation aligned to c bytes
  BcOp_FieldAddr,      // regs[a] = regs[b] + c  (no memory access)
  BcOp_LoadFieldI32,   // regs[a] = sign-extend(*(i32*)(regs[b] + c))
  BcOp_LoadFieldU32,   // regs[a] = zero-extend(*(u32*)(regs[b] + c))
  BcOp_LoadFieldI64,   // regs[a] = *(i64*)(regs[b] + c)
  BcOp_LoadFieldF32,   // regs[a] = zero-extend(*(u32*)(regs[b] + c)) -- an f32 bit pattern in the
                          // low 4 bytes of the register
  BcOp_LoadFieldBool,  // regs[a] = *(u8*)(regs[b] + c) != 0
  BcOp_LoadFieldI8,    // regs[a] = sign-extend(*(i8*)(regs[b] + c))  -- also `char`
  BcOp_LoadFieldU8,    // regs[a] = zero-extend(*(u8*)(regs[b] + c))
  BcOp_LoadFieldI16,   // regs[a] = sign-extend(*(i16*)(regs[b] + c))
  BcOp_LoadFieldU16,   // regs[a] = zero-extend(*(u16*)(regs[b] + c))
  BcOp_StoreFieldI32,  // *(i32*)(regs[a] + b) = (i32)regs[c]  -- also u32
  BcOp_StoreFieldI64,  // *(i64*)(regs[a] + b) = regs[c]
  BcOp_StoreFieldF32,  // *(u32*)(regs[a] + b) = (u32)regs[c]  -- the low 4 bytes of regs[c]
  BcOp_StoreFieldBool, // *(u8*)(regs[a] + b) = regs[c] != 0
  BcOp_StoreFieldI8,   // *(i8*)(regs[a] + b) = (i8)regs[c]  -- also u8/char
  BcOp_StoreFieldI16,  // *(i16*)(regs[a] + b) = (i16)regs[c]  -- also u16

  // regs[a] = a 3-way byte comparison of the two String8 headers at regs[b]
  // and regs[c]: <0 if the first is lexicographically smaller, >0 if larger,
  // 0 if identical. Matches runtime/bbb_string.c's bbb_str8_compare (byte
  // diff up to the shorter length, then a shorter-is-less size tiebreak),
  // reimplemented inline in bcvm.c because that file is text embedded into
  // GENERATED programs, not linked into this binary.
  //
  // The only string/struct comparison opcode: !=, <, <=, > and >= are this
  // result fed through the existing integer Eq/Neq/Lt/Le/Gt/Ge, and struct
  // comparison is a plain BcOp_Call into a per-type synthesized comparator
  // chunk.
  BcOp_StrCmp,

  // regs[a] = str8_match(String8 at regs[b], String8 at regs[b+1],
  // (StringMatchFlags)regs[b+2]) -- backs `(string-match a b flags)`. Unlike
  // BcOp_StrCmp, this calls base.h's real str8_match, which IS linked into
  // this binary; reimplementing its flag handling would risk drift from the
  // one real implementation. `b` names a 3-register block, `c` unused.
  BcOp_StringMatch,

  // ~~ `Map<K,V>`/`Set<T>`. See bcmap.h/bcmap.c: one generic runtime
  // algorithm parameterized by a compile-time BcHashSlotLayout descriptor,
  // rather than the per-(K,V) monomorphization codegen.c's native backend
  // needs. All three take a register block (`b` = block start, `c` unused).
  //
  // The layout descriptor is always the LAST register in the block -- a real
  // pointer baked into the const pool at compile time, loaded with an
  // ordinary BcOp_LoadConst, exactly as a boxed string literal's header
  // already is. A Map/Set instance's address doubles as a `BcHashInstance*`
  // with no marshaling (bcmap.h's struct is shaped to match).
  //
  // regs[a] = bc_map_set(arena, map_addr, layout, key_ptr, value_ptr) --
  // backs both `map-set` and `set-add`; layout->is_set picks the behavior.
  // `b` names {map_addr, arena_addr, key_ptr, value_ptr, layout_ptr};
  // value_ptr is never read when layout->is_set.
  BcOp_MapSet,
  // regs[a] = bc_map_get(map_addr, layout, key_ptr) -- a nilable pointer to
  // the matching slot's VALUE, NULL if absent. Backs `map-get` directly, and
  // `map-contains?`/`set-contains?` as this plus a `!= 0` check at the
  // bcgen.c call site. `b` names {map_addr, key_ptr, layout_ptr}.
  BcOp_MapGet,
  // regs[a] = bc_map_remove(map_addr, layout, key_ptr) -- backs both
  // `map-remove` and `set-remove` (same algorithm either way, no is_set
  // branch). `b` names {map_addr, key_ptr, layout_ptr}.
  BcOp_MapRemove,

  // regs[a] = prog->globals[b] / prog->globals[a] = regs[b] -- a module-level
  // `var`/`val`'s storage, one i64 slot each (an embedded-typed global's slot
  // holds its backing storage's address, as everywhere else in this VM).
  // These read `prog->globals` rather than the register file: a global must
  // survive across separate bc_run_in_program calls, unlike a local. See
  // bcgen.c's top-of-file note for name resolution and init order.
  BcOp_LoadGlobal,
  BcOp_StoreGlobal,

  // regs[a] = regs[b] OP regs[c], a 64-bit bitwise op backing `bit-and`/
  // `bit-or`/`bit-xor`/`bit-shl`/`bit-shr`. No separate i32/i64 families, the
  // same simplification BcOp_Add makes. The shift amount needn't match the
  // shifted value's width, per checker.c's BinaryShl/Shr rule (only the left
  // operand must be bitwise-ok).
  //
  // Right shift is the one op here whose meaning depends on the SHIFTED
  // value's signedness, so it comes in two: BcOp_Shr is the arithmetic shift
  // C emits for a signed i32/i64, BcOp_ShrU the logical one C emits for an
  // unsigned type. Registers carry no type, so bcgen.c picks the opcode from
  // the operand's static type. It matters only at 64 bits -- a narrower
  // unsigned value is held zero-extended and therefore never negative -- but
  // that is exactly where PCG32 in native_pkgs/rng lives: `(bit-shr state
  // 59u64)` on a u64 past 2^63 filled with sign bits instead of zeros and
  // handed the generator a rotation of ~4 billion.
  //
  // type_ref_is_bitwise_ok also permits an F32/F64 operand (it only checks
  // type_kind_is_numeric). Masking a float's raw bit pattern is well-defined
  // here, unlike codegen.c where the C compiler rejects `float & float`; not
  // guarded against, same as `not`'s C-style truthiness on any type.
  BcOp_BitAnd,
  BcOp_BitOr,
  BcOp_BitXor,
  BcOp_Shl,
  BcOp_Shr,
  BcOp_ShrU,
  // regs[a] = ~regs[b] -- `bit-not`, the one unary bitwise op. Preserves the
  // operand's type, unlike `not`.
  BcOp_BitNot,

  // regs[a] = regs[b] truncated to `c`'s width and re-extended back to a full
  // 64-bit register -- sign-extended when c's BC_NARROW_SIGNED bit is set,
  // zero-extended otherwise. `c` is a width/sign CODE, not a register (see
  // BC_NARROW_WIDTH_MASK below). Writing regs[a] == regs[b] is fine: the
  // source is read before the destination is written.
  //
  // This is the whole of this backend's integer-width model. Registers are
  // untyped i64, so every value narrower than 64 bits is kept in its
  // canonically extended form, and this opcode is what re-establishes that
  // after an op that could leave the form behind: `(cast u8 x)`, and the
  // arithmetic wraparound that makes u32/i32/... math agree with the real C
  // types codegen.c emits (see bcgen.c's bc_narrow_arith_result).
  //
  // One opcode rather than the Shl/Shr or BitAnd+LoadConst pairs that used to
  // spell this out inline, because arithmetic is a hot path and it now
  // narrows: two instructions per `(+ i 1)` on an i32 instead of four.
  BcOp_Narrow,

  // ~~ `(cast Type value)` across an int<->float boundary: real numeric
  // conversion. Every other cast pairing is a no-op reinterpretation or a
  // BcOp_Narrow width adjustment on the same representation -- see
  // bc_compile_expr's BinaryCast case for the full matrix.
  //
  // regs[a] = (f64)regs[b] / (f32)regs[b], reading regs[b] as a SIGNED 64-bit
  // int. An unsigned source with its top bit set converts as if negative --
  // narrower than codegen.c's real C cast, which handles it correctly; fixing
  // it needs a separate unsigned-to-float instruction.
  BcOp_IntToF64,
  BcOp_IntToF32,
  // regs[a] = (i64)regs[b], reading regs[b] as f64/f32 -- truncates TOWARD
  // ZERO, per C's conversion rule. A narrower integer target (`(cast i32
  // someF64)`) still needs a follow-up width adjustment; this does only the
  // float-to-int part.
  BcOp_F64ToInt,
  BcOp_F32ToInt,
  // regs[a] = (f32)regs[b] / (f64)regs[b] -- real precision widen/narrow, not
  // a bit reinterpretation.
  BcOp_F32ToF64,
  BcOp_F64ToF32,

  // ~~ Arenas (`create`/`destroy`/`reset`/`release`/`mark`/`pop`, `push`/
  // `push0`, `scratch`). base.h's `Arena` (2 pointers, 16 bytes) is a real
  // primitive at the 3b level (TypeKind_Arena), copied by value, so it gets
  // the embedded treatment: an Arena-valued register is the address of a
  // 16-byte block holding a live `Arena{void* backend; ArenaOps* ops;}`.
  // `ArenaMark` is one pointer and lives in the register unboxed.
  //
  // These call base.h's own `arena_*` functions -- the ones this compiler
  // itself uses -- NOT the `bbb_arena_*` copy codegen.c emits as text into a
  // generated program, which has no access to base.h at all.
  //
  // regs[a] = a fresh, boxed Arena from arena_create_vm(regs[b] as a u64
  // reserve size).
  BcOp_ArenaCreate,
  // arena_destroy/reset/release(regs[a] as Arena*) -- void. regs[a] already
  // holds the address of the boxed Arena, which IS a valid `Arena*`.
  BcOp_ArenaDestroy,
  BcOp_ArenaReset,
  BcOp_ArenaRelease,
  // regs[a] = arena_mark(regs[b] as Arena*) -- unboxed, see above.
  BcOp_ArenaMark,
  // arena_pop(regs[a] as Arena*, regs[b] as ArenaMark) -- void.
  BcOp_ArenaPop,
  // regs[a] = arena_push(regs[b] as Arena*, regs[c] as a u64 byte size) /
  // arena_push_zero(...). ALIGNMENT IS FIXED AT 16 BYTES: a 3-operand
  // instruction has no room for (dst, arena, size, align), and 16
  // over-aligns every ordinary 3b type, whose natural alignment from
  // layout_of never exceeds 8. A type with an explicit `align-bytes` above
  // 16 would be UNDER-aligned here.
  //
  // `size` is a runtime register, not a baked literal: unlike BcOp_Alloc,
  // `(push arena Type Count)`'s Count can be any runtime expression, so
  // `stride * count` is computed by a preceding BcOp_Mul.
  BcOp_ArenaPush,
  BcOp_ArenaPushZero,
  // regs[a] = a fresh, boxed Arena copying `*ctx_scratch()`'s two pointers --
  // a by-value handle to the shared per-thread scratch arena. bcgen.c brackets
  // the `scratch` body with an ordinary BcOp_ArenaMark/BcOp_ArenaPop pair,
  // replicating arena_temp_begin/arena_temp_end with no extra machinery.
  BcOp_LoadScratchArena,

  // `(alloc Type)`/`(alloc Type Count)` + `(free p)` -- malloc-backed, the one
  // non-arena allocation family. regs[a] = malloc(regs[b] as a u64 byte
  // size); free(regs[a] as a real pointer).
  BcOp_Malloc,
  BcOp_Free,

  // ~~ `mem-set`/`mem-copy`/`mem-zero`/`mem-compare` -- the raw-byte escape
  // hatch, the memset/memmove/memcmp trio codegen.c emits as bbb_MemorySet
  // and friends. Every pointer operand is a register holding a real host
  // address (what BcOp_FieldAddr produces and the field loads dereference),
  // so these are thin wrappers with no address translation.
  BcOp_MemSet,   // memset((void*)regs[a], (int)regs[b], (size_t)regs[c]) -- void
  // memmove((void*)regs[a], (void*)regs[b], (size_t)regs[c]) -- void.
  // memMOVE, not memcpy, matching bbb_MemoryCopy's overlap-safe choice.
  BcOp_MemCopy,
  BcOp_MemZero,  // memset((void*)regs[a], 0, (size_t)regs[b]) -- void
  // regs[a] = memcmp((void*)regs[c+0], (void*)regs[c+1], (size_t)regs[c+2]) --
  // the one member of this family with three inputs plus a destination, so it
  // uses the register-block convention. `b` unused.
  BcOp_MemCompare,

  // ~~ `(sqrt-checked x)`/`(asin-checked x)`/`(acos-checked x)`/
  // `(pow-checked base exp)` -- the underlying libm call, ALWAYS computed in
  // f64 (regs[b]/regs[c] read and written as f64 bit patterns) even for an
  // f32 argument: bcgen.c widens via BcOp_F32ToF64 first and narrows the
  // result back via BcOp_F64ToF32. A real precision difference from
  // codegen.c, which calls `sqrtf`/`asinf` and stays in f32 throughout.
  BcOp_Sqrt,
  BcOp_Asin,
  BcOp_Acos,
  BcOp_Pow, // regs[a] = pow(regs[b], regs[c])
  // Unchecked trig -- `(sin x)`/`(cos x)`/`(atan2 y x)`, ordinary
  // TypedNodeKind_Call nodes rather than the CheckedMath family above (they
  // succeed for any finite input, so there's no `(bool T)` result). Same
  // f64-always convention as BcOp_Sqrt.
  BcOp_Sin,
  BcOp_Cos,
  BcOp_Atan2, // regs[a] = atan2(regs[b], regs[c])
  // regs[a] = isfinite(regs[b] as f64) ? 1 : 0 -- rejects both NaN (a genuine
  // domain error: negative sqrt, out-of-range asin/acos, a negative pow base
  // with a fractional exponent) and +-Inf, matching codegen.c's own
  // `isfinite(_3b_r)` check.
  BcOp_F64IsFinite,

  // ~~ Handle pools (`(handle Name)` + `handle-alloc`/`handle-deref`/
  // `handle-free`/`handle-valid?`). base.h's `DEFINE_HANDLE_POOL(T, Prefix)`
  // generates type-specific functions, but the underlying `HandlePool` struct
  // is already type-erased -- `stride` (== sizeof(T)) is a runtime field, not
  // baked into the struct's shape -- so a few opcodes over a real
  // `HandlePool*` cover every pooled type with no per-type codegen.
  //
  // A `(handle Name)` declaration gets a slot in `BcProgram.globals` (the
  // same array module-level `var`/`val` uses, with a separate compile-time
  // name table so a pooled struct name and a global can't collide);
  // `#init_globals` allocates and zeroes a live `sizeof(HandlePool)` block
  // for it, mirroring codegen.c's `static MeshPool g_Mesh_pool;`.
  //
  // pool->stride = regs[b] (a u32). Separate from BcOp_HandlePoolInit because
  // (pool, arena, capacity, stride) is four operands.
  BcOp_HandlePoolSetStride,
  // The REST of Prefix_pool_init(arena, pool, capacity): allocates data/
  // generation/free_list, fills the free list, seeds generation counters.
  // Reads pool->stride, already set by the opcode above. regs[a] = pool,
  // regs[b] = arena, regs[c] = capacity (u32).
  BcOp_HandlePoolInit,
  // regs[a] = Prefix_pool_alloc(regs[b] as HandlePool*, NULL) -- a fresh,
  // zeroed slot's Handle. `{u32 index; u32 generation;}` is 8 bytes, so it
  // lives in the register unboxed.
  BcOp_HandleAlloc,
  // regs[a] = Prefix_pool_get(regs[b] as HandlePool*, regs[c] as Handle) -- a
  // real T* if index and generation are both still live, NULL otherwise.
  // Backs `handle-deref` directly AND `handle-valid?` (this result compared
  // against 0). `handle-valid?` must NOT use base.h's `Prefix_handle_valid`,
  // which is only a zero-check.
  BcOp_HandleGet,
  // regs[a] = Prefix_pool_free(regs[b] as HandlePool*, regs[c] as Handle) ? 1 : 0.
  BcOp_HandleFree,

  // regs[a] = dyn_count(regs[b] as a Vector's T* pointer) -- reads the hidden
  // `DynHdr{count,capacity}` immediately BEFORE the data pointer, via base.h's
  // own `dyn_count`/`dyn_hdr` macros. This can't reuse BcOp_FieldAddr:
  // that opcode's offset is a `u32` zero-extended into pointer arithmetic,
  // with no way to express a hidden header's NEGATIVE displacement. Backs
  // collection `for` over a Vector; an Array's element count is already known
  // from its type.
  BcOp_DynCount,

  // ~~ `(string-to-i32 s)`/`string-to-i64`/`string-to-u32`/`string-to-u64`/
  // `string-to-f32`/`string-to-f64`. The parsing logic is replicated from
  // runtime/bbb_string.c's `bbb_string_to_*` family, which is text embedded
  // into generated programs and not callable from here; unlike the arena and
  // string-match cases there's no generic base.h equivalent to call instead.
  // Int: leading sign, digit run, overflow-checked against i64/u64's range.
  // Float: strtod/strtof through a bounded stack buffer, requiring the WHOLE
  // string to be consumed.
  //
  // One pair of opcodes covers all six target kinds: `ins.c` carries the
  // target TypeKind, which bcgen.c already has from
  // `TypedNodeKind_ParseNumber.target_kind`. The value and the ok flag are
  // separate opcodes that each re-parse the string -- the same split
  // BcOp_Sqrt+BcOp_F64IsFinite uses, for the same reason: no operand slot to
  // return a second result through. Both go through bcvm.c's single
  // `bc_parse_number`, which is where the I32/U32 narrow-range rule and the
  // zero-on-failure guarantee live; see its comment for why one shared
  // implementation and not two.
  //
  // I32/U32 additionally range-check the parsed i64/u64 magnitude against
  // their own narrower bounds, matching bbb_string_to_i32/_u32's parse-wide-
  // then-check-it-fits delegation. A value that parses as i64 but doesn't fit
  // in i32 is a FAILURE, not a silent truncation.
  //
  // regs[a] = the parsed value, or ZERO if parsing would fail -- not merely
  // "don't look at it": runtime/bbb_string.c zeroes the value on failure too,
  // and a program that reads it without checking BcOp_ParseNumberOk first has
  // to see the same number on both backends (see bcvm.c's bc_parse_number,
  // the single parse both this and BcOp_ParseNumberOk go through).
  BcOp_ParseNumberValue,
  // regs[a] = 1 if regs[b]'s string parses as target kind `c`, 0 otherwise.
  BcOp_ParseNumberOk,

  // ~~ `dyn-push`/`vector-push`/`commit` -- base.h's amortized-growth dynamic
  // arrays. base.h's `dyn_push` is a STATEMENT MACRO that reseats the
  // caller's own `arr` variable on growth (`arr = arena_dyn_grow(...)`), so
  // this needs real bytecode-level growth logic rather than the thin wrapper
  // `push`/`push0` could be.
  //
  // All four reuse the "operand `a` is BOTH the input and the output pointer"
  // trick to fit three operands, and take the compile-time element stride as
  // a literal immediate in `c` (not a register index), the same convention
  // BcOp_Alloc's size/align use. Alignment is again fixed at 16 bytes, same
  // no-operand-slot-left reason as BcOp_ArenaPush; DynHdr's natural alignment
  // is 8, so 16 over-aligns anything short of an explicit `align-bytes > 16`.

  // regs[a] = dyn_capacity(regs[b] as a Vector/dyn-push-grown T* pointer) --
  // the same hidden-header read as BcOp_DynCount, on the other field. Decides
  // whether `dyn-push` must grow before writing.
  BcOp_DynCapacity,

  // Grows a dyn-push-grown array in place, reimplementing base.h's
  // `arena_dyn_grow`: new_cap = old_cap ? old_cap*2 : 8, allocate
  // hdr_size+new_cap*elem_size, copy `Min(old_bytes, new_bytes)` bytes across
  // (which preserves the old `.count`, since it lives in the copied header;
  // zeroed instead when regs[a] started NULL, with no old header to copy).
  // regs[a] is both read as the old pointer and overwritten with the new one;
  // bcgen.c then re-stores it wherever the pushed-to local/global lives,
  // matching the C macro's reseat. regs[b] = the Arena. `c` = element stride.
  BcOp_DynGrow,

  // dyn_hdr(regs[a])->count = regs[c] -- the final step of `dyn-push`, after
  // growth and the element write. A separate opcode for the same reason as
  // BcOp_DynCount: the generic field stores can't express DynHdr's negative
  // displacement.
  BcOp_DynSetCount,

  // `(commit dst-arena src)` -- copies a dyn-push-grown array (regs[a], read
  // via its hidden DynHdr count) into regs[b] (an Arena) as a fresh,
  // exactly-right-sized allocation, with no growth-by-doubling slack.
  // Mirrors cg_commit_expr, including its empty-array case: regs[a] = NULL,
  // not a zero-length allocation, when nothing was pushed. regs[a] is both
  // the source pointer and the committed result -- safe because a `commit`
  // call site never needs the old pointer again. `c` = element stride.
  BcOp_DynCommit,

  // ~~ `print`/`println`. codegen.c's `cg_call` synthesizes one big C printf
  // FORMAT STRING out of the template and has to escape the user's literal
  // text so it can't be read as a format spec. This backend builds no
  // variadic call at all: bcgen.c walks the (checker-validated, see
  // `count_template_placeholders`) template at compile time and emits a flat
  // sequence -- one BcOp_PrintString per literal chunk, one type-specific
  // Print opcode per `{}` placeholder, plus a boxed newline for `println`.
  // Each opcode does one fixed-format fprintf/fwrite, so no runtime-built
  // format string exists for a user's `%`/`{`/`}` bytes to land in.
  //
  // Two integer opcodes cover all eight integer TypeKinds. codegen.c needs
  // per-width `(int)`/`(long long)` casts only to satisfy C's printf argument
  // promotion; here every integer already sits in a register as its
  // mathematically correct 64-bit value whatever its declared width (loads
  // sign/zero-extend, literals are stored correctly, arithmetic is
  // full-width), so only SIGNEDNESS affects rendering.
  //
  // OPTIONAL STREAM TARGET: each opcode takes the value in `a` and, when `c`
  // is nonzero, the `stream` (bcosprims.c's `FILE*`) to write to in `b` --
  // `(print s "count: {}" n)`, the form native codegen routes to
  // bbb_stream_printf. `c == 0` means stdout. Encoded as an operand rather
  // than a parallel seven-opcode set: the target is the only thing that
  // varies, `b`/`c` were already unused, and doubling the opcode set would
  // double the dispatch table, the disassembly, and the on-disk opcode space.

  // fprintf(out, "%lld", (long long)regs[a]) -- i8/i16/i32/i64.
  BcOp_PrintI64,
  // fprintf(out, "%llu", (unsigned long long)regs[a]) -- u8/u16/u32/u64.
  BcOp_PrintU64,
  // fprintf(out, "%g", bits-reinterpreted f64 in regs[a]) -- an f32 argument
  // is widened via BcOp_F32ToF64 first, so this is the only float opcode.
  BcOp_PrintF64,
  // fprintf(out, "%s", regs[a] ? "true" : "false").
  BcOp_PrintBool,
  // fprintf(out, "%c", (int)regs[a]) -- a genuinely different fixed format
  // from PrintI64's %lld, so its own opcode.
  BcOp_PrintChar,
  // fwrite(header.str, 1, header.size, out) -- regs[a] is the address of a
  // boxed {ptr,size} String8 header. A raw byte write, not `%s`: a `string`
  // is a COUNTED view with no nul-termination guarantee.
  BcOp_PrintString,
  // fprintf(out, "%s", (char*)regs[a]) -- the one pointer shape
  // `print`/`str` render (type_ref_is_printable's `char*` case): a genuine
  // nul-terminated C string from `cstring` or an extern binding, a bare
  // pointer rather than a boxed header, hence a separate opcode.
  BcOp_PrintCString,

  // Not an opcode -- the count of them, so a decoder can range-check an
  // opcode it did not itself produce. bcvm.c's computed-goto dispatch table
  // is sized by this and bounds-checks against it, which is what keeps a
  // corrupt/truncated `.3bc` (bcio.c loads one straight off disk) from
  // indexing that table out of range and jumping through whatever pointer
  // happened to follow it. Must stay LAST.
  BcOp_Count,
} BcOp;

// BcOp_Narrow's `c` operand: the target width in the low bits, plus one flag
// for sign- vs zero-extension. A width/sign pair rather than the TypeKind it
// comes from, because this value is serialized into a `.3bc` cache: widths
// and signedness are facts about the machine and never renumber, whereas
// TypeKind is a compiler-internal enum that would silently change meaning the
// day a kind is inserted into it.
#define BC_NARROW_WIDTH_MASK 0xFFu
#define BC_NARROW_SIGNED     0x100u

typedef struct BcInstr {
  BcOp kind;
  u32  a, b, c;
} BcInstr;

// One const-pool slot holding a compile-time-computed POINTER (into a string
// literal's bytes, or into a boxed {ptr,size} header pointing at them -- see
// bcgen.c's bc_compile_string_literal/bc_fill_string_field) rather than a
// scalar bit pattern. Inert for a freshly-compiled chunk run in-process,
// where consts[const_slot] is already a valid pointer. It exists for bcio.c:
// a raw pointer is meaningless once written to a file and read back in
// another process, so the serializer writes `bytes` instead, and the
// deserializer re-interns them into that run's arena and patches the slot.
typedef struct BcStringFixup {
  u32     const_slot;
  b32     is_header; // true: the slot becomes the address of a FRESH {ptr,size} header pointing at
                        // re-interned `bytes` (bc_compile_string_literal's shape); false: the
                        // address of the re-interned bytes directly (bc_fill_string_field's shape)
  String8 bytes;
} BcStringFixup;

// BcStringFixup's sibling, for the OTHER kind of compile-time pointer a const
// pool can hold: the BcHashSlotLayout descriptor bcgen.c bakes in per Map/Set
// call site (bc_add_layout_const), which BcOp_MapSet/MapGet/MapRemove read
// through at runtime. Inert in-process for exactly the same reason -- the
// slot already holds a live pointer -- and needed for exactly the same one:
// that pointer is meaningless in the process that reads the file back.
//
// Unlike a string's bytes, a descriptor is a small FIXED-SIZE value with no
// interior pointers, so it's just held by value here and serialized
// field-by-field; the loader allocates one and points the slot at it.
typedef struct BcLayoutFixup {
  u32              const_slot;
  BcHashSlotLayout layout;
} BcLayoutFixup;

// One compiled function. `num_registers` is the frame size the interpreter
// allocates per call; `param_count` is how many of the low-numbered
// registers the caller must pre-bind to incoming arguments.
typedef struct BcChunk {
  BcInstr*       code;   // dyn array
  i64*           consts; // dyn array -- constant pool
  u32            num_registers;
  u32            param_count;
  String8        name;   // the source fn's own name -- diagnostics AND bc_program_find_fn
  BcStringFixup* string_fixups; // dyn array; empty for a chunk with no string literals, and always
                                    // empty on a chunk bcio.c deserialized -- the fixups are
                                    // already applied by the time that chunk exists
  BcLayoutFixup* layout_fixups; // dyn array; same story, for Map/Set call sites
} BcChunk;

// A set of compiled functions that can call each other. bcgen.c's
// bc_compile_program resolves every BcOp_Call's callee to a fixed chunk index
// at compile time, by name, in a two-pass gather-then-compile sweep so
// forward and mutually-recursive calls resolve regardless of declaration
// order.
//
// Forward-declared here (the full definition, BcModuleImport included, is at
// the bottom of this file) so BcProgram can hold a pointer to one.
typedef struct BcModuleTable BcModuleTable;

typedef struct BcProgram {
  BcChunk* chunks;  // dyn array
  i64*     globals; // dyn array -- one i64 slot per module-level `var`/`val`, at the index bcgen.c's
                        // global name table resolved at compile time. Zeroed at allocation, then
                        // populated by running the always-present `#init_globals` chunk.
                        // bc_compile_program does this before returning; bc_program_load must do it
                        // too after a cache load, since this is runtime state a cache can't bake in.
  b32      ok;      // false only if bc_compile_program hit 3b syntax this backend doesn't support
                        // yet -- a diag_error was already printed at the source location (see
                        // bc_unsupported). `chunks` is then partial and must not be run.
  BcModuleTable* module_table; // caller-owned, may be NULL. Backs BcOp_CallModule's dispatch, which
                        // reads it off `prog` rather than taking a new bc_run_in_program parameter:
                        // only the rare cross-module-import case needs it, and threading it through
                        // would touch every one of that function's call sites.
} BcProgram;

// A native (host) function callable from compiled bytecode via BcOp_CallHost
// -- see bcgen.c's TypedNodeKind_Call handling (which tries BcFnTable first,
// then falls back to this table by name).
//
// Two ways to register one, a real tradeoff rather than one superseding the
// other:
//
//  - BcHostImportKind_Trampoline: `fn` conforms to one fixed C signature
//    (`BcHostFn`) and unpacks its own `args`, much as Lua's C API bridges
//    native functions. Handles ANY real signature (floats, structs by value,
//    more than 6 arguments) precisely because the wrapper does the unpacking
//    -- at the cost of one hand-written wrapper per host function.
//
//  - BcHostImportKind_Direct: zero-marshaling. `native_fn` is an existing
//    native function pointer called with no wrapper at all, via
//    bc_call_native_direct (bcnative.c) -- VM register values ARE the calling
//    convention's argument values. Limits (see bcnative.c for why): x86-64
//    System V only, integer/pointer arguments only, at most 6 of them, and a
//    scalar integer/pointer return (or don't-care, for void). Float args,
//    more than 6 arguments, or struct-by-value still need Trampoline.
//
// TYPED SIGNATURE VERIFICATION: `param_types`/`return_type` are cross-checked
// by bcgen.c's bc_verify_host_imports against the matching `(extern (fn name
// [params...] ReturnType))` declaration. It checks the EXTERN DECLARATION,
// not call sites: the checker already guarantees a call site matches its own
// extern, and the real gap is between that declaration and whatever the
// embedding program separately told this table -- two sources of truth that
// can drift. It also catches a Direct-kind import whose signature involves a
// float, which that calling convention can't route at all; without the check
// the argument would be read out of an integer register and silently corrupt.
//
// `userdata` is the embedding program's context pointer (see
// BcHostImportTable), passed unchanged to every Trampoline call so a host
// function can reach native state without a global. Meaningless for Direct:
// there's no wrapper, hence no room for a hidden parameter -- a Direct
// function that needs context takes it as one of its own parameters.
typedef i64 (*BcHostFn)(i64* args, u32 arg_count, Arena* heap, void* userdata);

typedef enum BcHostImportKind {
  BcHostImportKind_Trampoline,
  BcHostImportKind_Direct,
} BcHostImportKind;

typedef struct BcHostImport {
  String8          name;      // matches a 3bscript call site's callee name exactly. Conventionally
                                 // declared script-side as a body-less `(extern (fn name [params...]
                                 // ReturnType))`, the same syntax as a real C FFI binding, so the
                                 // checker validates the call site with no new syntax to support.
  BcHostImportKind kind;
  union {
    BcHostFn fn;         // when kind == BcHostImportKind_Trampoline
    void*    native_fn;  // when kind == BcHostImportKind_Direct -- the real function pointer, its
                            // actual C signature irrelevant to this table (see bc_call_native_direct)
  };
  u32              arg_count;  // how many i64 args to pass at RUNTIME; the only one of these three
                                  // fields bcvm.c reads. Redundant with param_count by construction,
                                  // kept separate so the interpreter never has to consult the
                                  // verification-only fields below.
  TypeRef*         param_types; // arena-owned array of `arg_count` TypeRefs, for
                                  // bc_verify_host_imports only. Caller-owned: must outlive this
                                  // table's use, as `name` must. NULL is fine when arg_count == 0.
  TypeRef          return_type;
} BcHostImport;

typedef struct BcHostImportTable {
  BcHostImport* entries; // dyn array
  void*         userdata; // one shared context pointer for the whole table, passed unchanged to
                             // every Trampoline-kind call. Not per-import: every host import a
                             // single embedding run registers typically wants the same context
                             // (e.g. the Config* one script run is building). NULL is fine.
} BcHostImportTable;

// Registers one TRAMPOLINE host function under `name`. Call this (or
// bc_host_import_table_add_direct) to build the table BEFORE compiling or
// running bytecode that references it. `param_types` (length `param_count`)
// and `return_type` describe this import's real signature, for
// bc_verify_host_imports to cross-check against the extern declaration. Pass
// `param_types=NULL, param_count=0` for a no-argument function.
static inline void
bc_host_import_table_add(BcHostImportTable* table, Arena* arena, String8 name, BcHostFn fn,
                          TypeRef* param_types, u32 param_count, TypeRef return_type) {
  BcHostImport entry = {0};
  entry.name        = name;
  entry.kind        = BcHostImportKind_Trampoline;
  entry.fn          = fn;
  entry.arg_count   = param_count;
  entry.param_types = param_types;
  entry.return_type = return_type;
  dyn_push(arena, table->entries, entry);
}

// Registers one DIRECT (zero-marshaling) host function under `name`.
// `native_fn` is an existing native function pointer -- integer/pointer-only,
// up to 6 parameters, see BcHostImportKind_Direct -- passed through a `void*`
// cast (POSIX guarantees function-pointer <-> object-pointer round-tripping,
// and this mechanism is already x86-64-SysV-specific).
// `param_types`/`return_type`: as bc_host_import_table_add, except
// bc_verify_host_imports also rejects an F32/F64 among them here, since
// Direct mode can never route a float correctly whatever the extern claims.
static inline void
bc_host_import_table_add_direct(BcHostImportTable* table, Arena* arena, String8 name, void* native_fn,
                                 TypeRef* param_types, u32 param_count, TypeRef return_type) {
  BcHostImport entry = {0};
  entry.name        = name;
  entry.kind        = BcHostImportKind_Direct;
  entry.native_fn   = native_fn;
  entry.arg_count   = param_count;
  entry.param_types = param_types;
  entry.return_type = return_type;
  dyn_push(arena, table->entries, entry);
}

// A real cross-package `.3bs` import: `(import build)`, then a qualified call
// site `(build/getenv "PATH")`. `name` is the FULL qualified name the call
// site writes verbatim ("build/getenv") -- lower_call's Identifier fallback
// stores callee text verbatim, slash and all, so this needs no parser or
// lower.c work, the same insight compiler.h documents for the native backend.
//
// `prog`/`fn_index` point at that other module's independently-compiled
// BcProgram and the chunk within it. The two programs are never merged into
// one chunk array: the native backend gets separate compilation free from the
// C linker, `.3bs` has no equivalent, so both BcPrograms stay live and
// separate (see BcOp_CallModule for how a call crosses the boundary). `prog`
// is caller-owned and must outlive this table's use.
typedef struct BcModuleImport {
  String8    name;
  BcProgram* prog;
  u32        fn_index;
} BcModuleImport;

typedef struct BcModuleTable {
  BcModuleImport* entries; // dyn array
} BcModuleTable;

// Rebuilds ONE entry of a CACHED program's module table at load time. A cache
// load skips compilation entirely (that's the point of it), so nothing has
// compiled the imported module -- bcio.c hands each qualified name the file
// recorded ("build/getenv") to this callback, and the embedding program
// (script.c) compiles or looks up whatever backs it, handing back that module's
// own BcProgram and the chunk index within it.
// `*out_prog` must outlive the loaded program, exactly as bc_module_table_add's
// own `prog` must.
//
// Returning false means "this name can't be resolved in this run" -- bcio.c
// then rejects the whole cache file as unusable rather than handing back a
// program whose cross-module calls would dispatch through a hole, and the
// caller falls back to compiling from source like any other cache miss.
typedef b32 (*BcModuleResolveFn)(String8 qualified_name, void* userdata,
                                  BcProgram** out_prog, u32* out_fn_index);

typedef struct BcModuleResolver {
  BcModuleResolveFn fn;
  void*             userdata; // the embedder's context, passed through unchanged (script.c's own
                                 // module registry + the tables a module compile needs)
} BcModuleResolver;

// Registers one cross-module import under an ALREADY-qualified `name`
// ("build/getenv") -- call this before compiling bytecode that references it.
// See script.c's module-resolution pipeline.
static inline void
bc_module_table_add(BcModuleTable* table, Arena* arena, String8 name, BcProgram* prog, u32 fn_index) {
  BcModuleImport entry = {0};
  entry.name     = name;
  entry.prog     = prog;
  entry.fn_index = fn_index;
  dyn_push(arena, table->entries, entry);
}

#endif
