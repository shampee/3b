// bcgen.c -- the bytecode compiler for 3bscript: typed AST in, BcProgram
// (bytecode.h) out. Mirrors codegen.c's cg_expr/cg_function in shape --
// same TypedNodeKind switch, same recursive descent, consuming the same
// Checker/resolved_types the native backend does, rather than re-deriving
// anything. What codegen.c has no equivalent of is jump backpatching: C
// text gets `if`/`while` from C's own syntax, a flat instruction stream
// doesn't.
//
// NOT SUPPORTED (checker-valid 3b this backend can't compile yet):
//  - `str` (Clojure-style stringify+concat) -- needs `print`'s per-type
//    rendering, but building a String8 instead of writing to a stream.
//  - the unchecked libm group other than sin/cos/atan2: `sqrt`, `pow`,
//    `mod`, `tan`, `asin`, `acos`, `atan`, `floor`, `ceil`, `round`,
//    `cbrt`, `sinh`, `cosh`, `tanh`. (The CHECKED forms are a separate
//    path and DO work: `sqrt-checked`/`asin-checked`/`acos-checked`/
//    `pow-checked`, see TypedNodeKind_CheckedMath below.)
//  - the `string-*` library other than `string-match`: `string-cat`,
//    `-chop`, `-copy`, `-ends-with`, `-find`, `-find-reverse`,
//    `-postfix`, `-prefix`, `-skip`, `-starts-with`, `-substr`. Plus
//    `cstring-copy`.
//  - `pow2?`/`pow2-or-zero?` (the `align-*-pow2` trio IS supported).
//  - array elements that aren't inline literals.
//  - a function used as a VALUE rather than called: passing one as an
//    argument, binding it to a `(fn ...)`-typed local, or storing it in a
//    struct field. The checker resolves these (its Identifier case falls
//    back to fn_table_lookup), and codegen.c compiles them to plain C
//    function pointers, so this is a genuine backend asymmetry -- it needs
//    a function-value representation plus an indirect-call opcode, not just
//    another case in a switch.
//
// Anything outside the supported set gets a real diag_error at its source
// location (see bc_unsupported) rather than silently miscompiling or
// aborting; bc_compile_program then returns a BcProgram with `.ok ==
// false`. To re-derive this list, diff the Call names checker.c
// special-cases against the ones this file matches on.
//
// LANES -- A DELIBERATE, NAMED ASYMMETRY. `parallel`/`parallel-for`/
// `lane-index`/`lane-count`/`lane-sync`/`lane-arena` all compile here, but
// they compile to ONE LANE, running serially on the interpreter's only
// thread: `lane-count` is 1, `lane-index` is 0, `lane-sync` emits nothing,
// `lane-arena` is this thread's scratch arena, `parallel` evaluates its
// captures and runs its body once, and `parallel-for` walks the whole
// `[0, count)` range. Scripts using lanes RUN and produce correct answers;
// they do not go faster.
//
// This is a legal execution of the feature, not an approximation of one.
// The lane contract is fork/join over INDEPENDENT work, and the native
// lane count is already a runtime-varying `Max(1, core_count - 1)`
// (runtime/bbb_thread.c's async_threads_init) -- so a correct lane program
// must already tolerate a count of 1, which is precisely what a
// single-core machine gives it natively. `lane_range(n)` with one lane
// returns `[0, n)` by its own arithmetic, so the partition needs no
// special case either.
//
// What it is NOT is real parallelism, and that gap is here rather than in
// the VM because this interpreter's whole execution state is
// process-global and single-threaded (g_bc_stack, script.c's
// g_module_registry, diag.c's g_source_files). Multi-lane execution would
// need a per-thread register stack, a per-thread arena context, a
// thread-safe global/heap story and fork/join opcodes -- a VM redesign,
// for a backend whose purpose is fast iteration on code the native
// backend already runs on real threads.
//
// LOOPS: `while` and range `for` share IfExpr's jump-backpatch mechanism,
// plus a backward jump to the condition check -- whose target is already
// known at emit time, unlike IfExpr's forward jumps. No new opcodes:
// BcOp_Jump takes an absolute target in either direction. Both are
// void-typed (checker-enforced) and return a fresh, never-written dead
// register, the convention ReturnExpr established. Range `for` shares
// BinaryAdd/comparison's int-vs-f64-vs-f32 operand dispatch for its
// `+=`/`<`, since a float-typed range is valid 3b. `break` and `continue`
// are forward jumps backpatched by the enclosing loop -- see BcLoopCtx.
//
// STRUCT REPRESENTATION: a struct or string value is a register holding
// the address of its bytes (BcOp_Alloc'd, sized/aligned per layout_of; a
// string's {ptr,size} header is boxed the same way for a standalone
// literal, or filled inline as a struct field -- see
// bc_fill_string_field). Always by reference, never inlined for small
// structs: correct but not maximally compact. Two things fall out of it
// for free -- param and let-local bindings need no special code, since
// such a local is just a register holding a pointer like any scalar; and
// reading a nested by-value struct or string field needs no memory access
// at all, just base-plus-offset (BcOp_FieldAddr).
//
// FIELD WIDTHS: see bytecode.h's note above BcOp_Alloc for which types
// share which load/store opcode. A width with no opcode asserts rather
// than borrowing a wrong-width one, which would silently over- or
// under-read real memory.
//
// INTEGER WIDTHS: registers are all 64-bit, but a u32/i16/... value is
// kept in the canonically extended form of its own type at all times, and
// arithmetic on it wraps at that width via a BcOp_Narrow emitted right
// after the op -- see bc_narrow_arith_result for why eagerly rather than
// at the points where the value is read. Without it the two backends
// disagree on the same source: native codegen emits real C types, which
// wrap, so `(* x 3u32)` printed 12000000000 here and 3410065408 there.
// Right shift, whose meaning depends on the operand's signedness rather
// than its width, is handled instead by picking BcOp_Shr vs BcOp_ShrU.
//
// VALUE-COPY SEMANTICS: a call argument, or a `let`/`var`/`val` binding,
// of struct/string/array type gets a real independent copy (see
// bc_compile_value_copy) rather than the usual alias-the-address
// shortcut. This matches codegen.c: struct and string are C by-value
// copies at both a call site and a binding; a fixed-size array is copied
// only at a binding (codegen.c emits an explicit memcpy), and as a
// function PARAMETER it decays and aliases, like any C `T name[N]`
// parameter. `set` on an identifier still never copies -- once
// binding-time and call-time copies close the aliasing gap, rebinding the
// register and copying into existing storage are observably identical.
//
// MUTATION: `set` on an identifier is a Move into whatever register
// bc_local_try_lookup resolves the name to; params, `var` locals and
// `let` locals are all just "a name bound to a register". `set` through
// `deref`/a field/an array element reuses bc_resolve_field_access/
// bc_resolve_index_access -- factored out of the read-path FieldAccess/
// IndexAccess cases so read and write can never disagree about where a
// target lives -- plus bc_store_value, which mirrors
// bc_load_op_for_type's scalar-vs-embedded split for writes.
// `ck->resolved_types[idx]` on the SetExpr node decides the store's
// width/kind uniformly across all four target kinds, as codegen.c's own
// SetExpr case does.
//
// Register allocation is a monotonic bump allocator: every new local or
// temporary gets the next unused register, never reused. Correct but not
// compact; a proper lexical-lifetime scan would be the optimization.
#include "bcgen.h"
#include "bcnative.h" // BC_NATIVE_DIRECT_MAX_ARGS -- bc_verify_host_import_signature's own
                         // Direct-mode arity check
#include "bcmap.h" // Map/Set support -- BcHashSlotLayout/bc_hash_slot_layout
#include "bcvm.h" // bc_run_in_program -- bc_compile_program runs the synthesized `#init_globals`
                     // chunk once before returning, the one place the compiler half depends on the
                     // interpreter half (bcio.c has the mirror-image dependency on bcgen.h)
#include <stdio.h>
#include <setjmp.h>

typedef struct BcLocal {
  String8 name;
  u32     reg;
  // True iff `reg` holds this local's ADDRESS -- a real backing memory slot
  // Alloc'd by bc_bind_local_typed -- rather than its value directly. Only
  // possible for an otherwise-scalar local whose name appears somewhere in
  // the program as `(addr name)`/`&name` (see
  // bc_scan_address_taken_names); always false for an embedded-typed local,
  // whose value already IS its address.
  b32     is_addr_taken;
} BcLocal;

// SCRIPT-AUTHOR DIAGNOSTICS vs. INTERNAL INVARIANTS. The `xassert(!"...")`
// calls throughout this file guard conditions the checker (or an earlier
// pass here) already guarantees for well-typed 3b -- those stay hard
// asserts. But a script author can write checker-valid 3b this backend
// doesn't compile yet (see the not-supported list at the top of this file),
// and that needs a real file:line:col diagnostic, not a process abort.
//
// bc_unsupported is that path: it reports through diag_error, then longjmps
// out of the entire bc_compile_program call -- the only clean way to
// abandon a bc_compile_expr recursion from arbitrary depth. Plain
// (non-thread-local) jmp_buf, the same one-caller-at-a-time convention
// diag.c's g_source_files/g_captured already assume for this
// single-threaded CLI.
static jmp_buf g_bc_compile_err;

static void
bc_unsupported(Token tok, const char* what) {
  diag_error(tok, "'.3bs' scripts do not yet support %s", what);
  longjmp(g_bc_compile_err, 1);
}

// Human name for a TypedNodeKind bc_compile_expr's switch doesn't handle.
// Every remaining entry on the not-supported list above is identified by a
// CALL NAME rather than a node kind, and each of those reports at its own
// site, so there is nothing left for this to special-case -- it exists for
// the switch's `default`, which by construction is a kind nobody has named.
// Add a case here if a whole node kind ever goes unsupported again.
static const char*
bc_unsupported_kind_label(TypedNodeKind kind) {
  (void)kind;
  return "this construct";
}

// One `scratch` scope open around the expression being compiled: the register
// holding the scratch arena and the register holding its mark, i.e. exactly
// the pair BcOp_ArenaPop needs. bc_unwind_scratch_scopes replays them for a
// jump that leaves the block early -- see TypedNodeKind_ScratchExpr.
typedef struct BcScratchScope {
  u32 arena_reg;
  u32 mark_reg;
} BcScratchScope;

typedef struct BcFnCtx {
  Checker*           ck;
  TypedAst*          tast;
  Arena*             arena;
  LayoutCache*       layout_cache;
  BcFnTable*         fn_table;
  BcFnTable*         global_table;      // may be NULL/empty -- see bcgen.h
  BcFnTable*         handle_pool_table; // may be NULL/empty -- pooled struct name ("Mesh", for
                                            // `(handle Mesh)`) -> its slot in BcProgram.globals. A
                                            // separate table from global_table, over the same globals
                                            // array, so a pooled struct name and a `var`/`val` name
                                            // spelled the same can't collide.
  BcHostImportTable* host_imports; // may be NULL -- see bcgen.h
  HashTable*         addr_taken_names; // may be NULL (bc_compile_struct_cmp_chunk's synthesized
                                           // comparators have no source-level `&` to care about) --
                                           // name -> non-NULL. Computed once for the whole program by
                                           // bc_scan_address_taken_names; consulted by
                                           // bc_bind_local_typed to decide whether a scalar local
                                           // needs a real backing slot rather than just a register.
  BcModuleTable*     module_table; // may be NULL -- resolves a QUALIFIED call name ("build/getenv") to
                                       // a separately-compiled BcProgram (see bytecode.h).
                                       // TypedNodeKind_Call tries this after fn_table and before
                                       // host_imports.
  BcInstr*           code;   // dyn array
  i64*               consts; // dyn array
  u32                next_reg;
  BcLocal*           locals; // dyn array -- looked up by backward scan, so an inner binding shadows
                                // an outer one, matching the checker's Scope chain
  BcStringFixup*     string_fixups; // dyn array -- see BcStringFixup in bytecode.h; metadata for
                                       // bcio.c, inert for a chunk run in-process
  BcLayoutFixup*     layout_fixups; // dyn array -- the same, for Map/Set call sites
  u32*               break_fixups;    // dyn arrays of code indices: BcOp_Jump instructions emitted by
  u32*               continue_fixups; // `(break)`/`(continue)` whose target isn't known yet. See
                                          // BcLoopCtx.
  BcScratchScope*    scratch_scopes; // dyn array used as a stack: the `scratch` blocks enclosing
                                        // the expression being compiled, outermost first
  u64                loop_scratch_mark; // how many of those the innermost enclosing loop began
                                          // with, and so the floor a `break`/`continue` rewinds to
                                          // -- see BcLoopCtx.scratch_mark. Zero outside any loop,
                                          // where the checker rejects both anyway.
} BcFnCtx;

// `break` and `continue` are forward jumps to points the loop being compiled
// has not emitted yet, so each records its instruction index and gets
// backpatched -- IfExpr's mechanism, with two differences that make it worth a
// helper. There can be MANY of either per loop, at any nesting depth inside
// the body; and loops nest, so a fixup must reach only its own loop. Both fall
// out of treating fc's two fixup arrays as stacks: a loop remembers their
// heights on entry, and every entry pushed above that mark is its own.
//
// The two targets differ, which is the whole reason `continue` is not just
// `break`: `break` lands past the loop, `continue` lands on the step (a range
// `for`'s `+=`, a foreach's index bump) so the counter still advances. Jumping
// to the condition instead would spin forever.
//
// `scratch_mark` is the same idea applied to fc->scratch_scopes, and is what a
// jump out of the loop rewinds DOWN TO rather than past: a `scratch` opened
// inside the body has to be given back, while one wrapping the whole loop is
// still live once the jump lands. `return` ignores it and unwinds everything,
// since it leaves the function outright.
typedef struct BcLoopCtx {
  u64 break_mark;
  u64 continue_mark;
  u64 scratch_mark;
  u64 saved_loop_scratch_mark; // the enclosing loop's, restored by bc_loop_end
} BcLoopCtx;

static BcLoopCtx
bc_loop_begin(BcFnCtx* fc) {
  BcLoopCtx loop = { dyn_count(fc->break_fixups), dyn_count(fc->continue_fixups),
                     dyn_count(fc->scratch_scopes), fc->loop_scratch_mark };
  fc->loop_scratch_mark = loop.scratch_mark;
  return loop;
}

// Every loop calls this once its fixups are patched. Restoring matters for
// SEQUENTIAL loops as much as nested ones: leave an inner loop's mark in place
// and a later `break` in the outer loop stops unwinding too early, missing a
// `scratch` it should have given back.
static void
bc_loop_end(BcFnCtx* fc, BcLoopCtx loop) {
  fc->loop_scratch_mark = loop.saved_loop_scratch_mark;
}

// Patches every fixup this loop pushed to `target` and pops them, leaving any
// belonging to an enclosing loop untouched.
static void
bc_loop_patch(BcFnCtx* fc, u32** fixups, u64 mark, u32 target) {
  u64 count = dyn_count(*fixups);
  for (u64 i = mark; i < count; i += 1) fc->code[(*fixups)[i]].a = target;
  if (*fixups) dyn_hdr(*fixups)->count = mark;
}

static u32
bc_alloc_reg(BcFnCtx* fc) {
  u32 r = fc->next_reg;
  fc->next_reg += 1;
  return r;
}

static u32
bc_emit(BcFnCtx* fc, BcOp kind, u32 a, u32 b, u32 c) {
  u32      idx = (u32)dyn_count(fc->code);
  BcInstr  ins = { kind, a, b, c };
  dyn_push(fc->arena, fc->code, ins);
  return idx;
}

static u32
bc_add_const(BcFnCtx* fc, i64 value) {
  u32 idx = (u32)dyn_count(fc->consts);
  dyn_push(fc->arena, fc->consts, value);
  return idx;
}

static void
bc_bind_local(BcFnCtx* fc, String8 name, u32 reg) {
  BcLocal loc = { name, reg, false };
  dyn_push(fc->arena, fc->locals, loc);
}

// Rewind the `scratch` scopes open at this point down to `floor`, innermost
// first, for a jump that is about to leave them. The stack is left alone: the
// jump is one path out of those blocks, and whatever follows it is still
// inside them. codegen.c's cg_unwind_scratch_scopes is the native counterpart,
// `floor` included -- `return` passes 0, `break`/`continue` pass the enclosing
// loop's mark, since a `scratch` around the whole loop outlives them.
static void
bc_unwind_scratch_scopes(BcFnCtx* fc, u64 floor) {
  for (u64 i = dyn_count(fc->scratch_scopes); i > floor; i -= 1) {
    BcScratchScope* scope = &fc->scratch_scopes[i - 1];
    bc_emit(fc, BcOp_ArenaPop, scope->arena_reg, scope->mark_reg, 0);
  }
}

// Non-asserting backward scan, so an inner binding shadows an outer one.
// Identifier and SetTargetKind_Identifier both try this before falling back
// to fc->global_table, so a local shadows a same-named module-level global,
// matching the checker's resolution order.
//
// Returns a pointer INTO fc->locals (not a copy) so a caller can read
// `.is_addr_taken`; NULL if not found. Only valid until the next
// `dyn_push` onto fc->locals -- every caller reads it and is done before
// binding anything new.
static BcLocal*
bc_local_try_lookup(BcFnCtx* fc, String8 name) {
  u64 count = dyn_count(fc->locals);
  for (u64 i = count; i > 0; i -= 1) {
    if (str8_match(fc->locals[i - 1].name, name, 0)) return &fc->locals[i - 1];
  }
  return NULL;
}

// One-pass, whole-program scan for every `(addr name)`/`&name`. No
// recursive tree walk is needed: `tast->nodes` is a flat array of every
// typed node in the program, so a linear scan picking out each UnaryAddr
// over an Identifier finds them all without understanding any other node
// kind's child-index shape.
//
// NAME-based, not scoped per declaration site: a same-named local in an
// unrelated function that takes `&` there will conservatively mark this
// one too. That costs an unneeded backing slot, and is far simpler than
// threading declaration-site identity through the scan.
static void
bc_scan_address_taken_names(TypedAst* tast, Arena* arena, HashTable* out) {
  foreach_index(i, dyn_count(tast->nodes)) {
    TypedNode* n = &tast->nodes[i];
    if (n->kind != TypedNodeKind_UnaryAddr) continue;
    if (n->unary.expr == TYPED_NIL) continue;
    TypedNode* operand = &tast->nodes[n->unary.expr];
    if (operand->kind != TypedNodeKind_Identifier) continue;
    hashtable_insert(arena, out, operand->ident.name, (void*)1, /*overwrite*/ true);
  }
}

// Non-asserting -- TypedNodeKind_Call tries this FIRST (a compiled-
// 3bscript function), then falls back to bc_host_import_try_lookup (a
// registered native function, conventionally declared `(extern (fn ...))` on the
// 3bscript side) before giving up.
static b32
bc_fn_table_try_lookup(BcFnTable* table, String8 name, u32* out_index) {
  foreach_index(i, dyn_count(table->entries)) {
    if (str8_match(table->entries[i].name, name, 0)) { *out_index = table->entries[i].index; return true; }
  }
  return false;
}

static b32
bc_host_import_try_lookup(BcHostImportTable* table, String8 name, u32* out_index) {
  if (!table) return false;
  foreach_index(i, dyn_count(table->entries)) {
    if (str8_match(table->entries[i].name, name, 0)) { *out_index = (u32)i; return true; }
  }
  return false;
}

// Non-asserting -- TypedNodeKind_Call tries this AFTER fn_table (this
// program's own functions) and BEFORE host_imports (in practice the two
// never collide: a module-table name is always QUALIFIED, "build/getenv",
// while a host import's own name is conventionally unqualified, "os-
// getenv" -- but fn_table is still checked first regardless, so a local
// function can never accidentally be shadowed by an identically-named
// cross-module import either way).
static b32
bc_module_table_try_lookup(BcModuleTable* table, String8 name, u32* out_index) {
  if (!table) return false;
  foreach_index(i, dyn_count(table->entries)) {
    if (str8_match(table->entries[i].name, name, 0)) { *out_index = (u32)i; return true; }
  }
  return false;
}

static u32 bc_compile_expr(BcFnCtx* fc, TypedIndex idx);
static void bc_compile_struct_fields_into(BcFnCtx* fc, StructEntry* se, TypedIndex lit_idx, u32 base_reg, u64 base_offset);
static void bc_compile_array_elements_into(BcFnCtx* fc, TypeRef elem_ty, TypedIndex lit_idx, u32 base_reg, u64 base_offset);
static u32 bc_compile_value_cmp(BcFnCtx* fc, Token tok, TypeRef ty, u32 addr_a, u32 addr_b);

// True iff `ty` is an enum/flags type. TypeKind_Named covers both structs
// and enums (see layout_of_named in layout.c), but only a struct is
// embedded/address-shaped in this backend: an enum is an ordinary scalar,
// its value a plain LoadConst (see TypedNodeKind_EnumAccess), so it needs
// the same treatment as i32 everywhere a TypeKind_Named check would
// otherwise assume "struct".
static b32
bc_type_is_enum(Checker* ck, TypeRef ty) {
  return ty.kind == TypeKind_Named && enum_table_lookup(ck, ty.name) != NULL;
}

// The scalar load/store opcode for a field of type `t`. Nested by-value
// struct/string/array fields never reach here -- bc_field_is_embedded
// routes them through BcOp_FieldAddr. Anything with no matching opcode
// raises a diagnostic rather than borrowing a wrong-width one.
static BcOp
bc_load_op_for_type(Token tok, TypeRef t) {
  switch (t.kind) {
    case TypeKind_I32:  return BcOp_LoadFieldI32;
    case TypeKind_U32:  return BcOp_LoadFieldU32; // zero-extend, unlike I32's sign-extend
    case TypeKind_F32:  return BcOp_LoadFieldF32;
    case TypeKind_I64:
    case TypeKind_U64: // an 8-byte load fills the whole register, so u64 reuses the i64 op with
                          // nothing left to extend -- unlike u32
    case TypeKind_F64:
    case TypeKind_Handle:
    case TypeKind_Pointer:
    case TypeKind_Stream: // a typedef'd POINTER at the language level, see TypeKind_Stream in 3b.h
    case TypeKind_Vector:
    case TypeKind_ArenaMark: // base.h's `{u8* at;}` -- the one pointer IS the whole value, and
                                // BcOp_ArenaMark/ArenaPop already keep it unboxed in a register, so
                                // an 8-byte load off the field offset reproduces it exactly
      return BcOp_LoadFieldI64;
    case TypeKind_Bool: return BcOp_LoadFieldBool;
    case TypeKind_I8:
    case TypeKind_Char: // plain C `char`, signed on this codebase's only target, so it reuses I8's
                           // sign-extending load
      return BcOp_LoadFieldI8;
    case TypeKind_U8:   return BcOp_LoadFieldU8;
    case TypeKind_I16:  return BcOp_LoadFieldI16;
    case TypeKind_U16:  return BcOp_LoadFieldU16;
    case TypeKind_Named: // only an enum reaches here -- see bc_field_is_embedded
      return BcOp_LoadFieldI32;
    default:
      bc_unsupported(tok, "a field/element of this type (only i8/u8/i16/u16/i32/u32/i64/u64/f32/"
                            "f64/bool/char/enum/handle/pointer/stream/ArenaMark/Vector are readable "
                            "by this bytecode compiler slice)");
      return BcOp_LoadFieldI64;
  }
}

static BcOp
bc_store_op_for_type(Token tok, TypeRef t) {
  switch (t.kind) {
    case TypeKind_I32:
    case TypeKind_U32: // storing back 4 bytes needs no sign/zero distinction, unlike a load, which
                          // has to decide how to extend into a wider register
      return BcOp_StoreFieldI32;
    case TypeKind_F32:  return BcOp_StoreFieldF32;
    case TypeKind_I64:
    case TypeKind_U64:
    case TypeKind_F64:
    case TypeKind_Handle:
    case TypeKind_Pointer:
    case TypeKind_Stream: // see bc_load_op_for_type's own note
    case TypeKind_Vector:
    case TypeKind_ArenaMark: // the same, see bc_load_op_for_type
      return BcOp_StoreFieldI64;
    case TypeKind_Bool: return BcOp_StoreFieldBool;
    case TypeKind_I8:
    case TypeKind_U8:
    case TypeKind_Char: // same no-distinction-on-a-store reasoning as I32/U32 above
      return BcOp_StoreFieldI8;
    case TypeKind_I16:
    case TypeKind_U16:
      return BcOp_StoreFieldI16;
    case TypeKind_Named: // only an enum reaches here, as in bc_load_op_for_type
      return BcOp_StoreFieldI32;
    default:
      bc_unsupported(tok, "a field/element of this type (only i8/u8/i16/u16/i32/u32/i64/u64/f32/"
                            "f64/bool/char/enum/handle/pointer/stream/ArenaMark/Vector are writable "
                            "by this bytecode compiler slice)");
      return BcOp_StoreFieldI64;
  }
}

// True iff a value of type `ty` is wider than one register and so lives
// embedded INLINE -- its bytes directly at base+offset, not behind a
// pointer stored there. Reading or filling one is pure pointer arithmetic,
// never a scalar load/store.
static b32
bc_field_is_embedded(Checker* ck, TypeRef ty) {
  // Arena is here for the same reason String is: base.h's `Arena` is a real
  // 16-byte by-value primitive (2 pointers). ArenaMark is NOT: at 8 bytes it
  // fits in a register, like Handle/Pointer/Vector. Map/Set are 32-byte
  // by-value instance structs (see layout_of's TypeKind_Map/Set case), NOT
  // bare pointers the way Vector is -- a Vector compiles straight to
  // `ElementType*`, while a Map's storage sits behind a `slots` pointer
  // that's only the first of four fields (bcmap.h's BcHashInstance). Named
  // is embedded only for a struct; an enum is a scalar.
  TypeKind k = ty.kind;
  if (k == TypeKind_Named) return !bc_type_is_enum(ck, ty);
  return k == TypeKind_String || k == TypeKind_Array || k == TypeKind_Arena
      || k == TypeKind_Map    || k == TypeKind_Set;
}

// Binds `name` -> `value_reg`, exactly like bc_bind_local -- unless `name`
// is address-taken somewhere in the program (fc->addr_taken_names) AND `ty`
// isn't already embedded (an embedded local's value is its address already,
// so it's addressable for free). In that case the local gets a real backing
// slot: Alloc, store `value_reg` into it, and bind the SLOT'S ADDRESS with
// `is_addr_taken = true`.
//
// This is the address-taken half of the same scalar/embedded duality
// bc_field_is_embedded models, decided at bind time from the pre-scan
// rather than purely by type. Every read or write of a local must check
// `.is_addr_taken` to know which representation it has.
static u32
bc_bind_local_typed(BcFnCtx* fc, Token tok, String8 name, TypeRef ty, u32 value_reg) {
  b32 addr_taken = fc->addr_taken_names && hashtable_lookup(fc->addr_taken_names, name) != NULL;
  if (addr_taken && !bc_field_is_embedded(fc->ck, ty)) {
    Layout layout   = layout_of(fc->layout_cache, fc->ck, ty);
    u32    slot_reg = bc_alloc_reg(fc);
    bc_emit(fc, BcOp_Alloc, slot_reg, (u32)layout.size, (u32)layout.align);
    bc_emit(fc, bc_store_op_for_type(tok, ty), slot_reg, 0, value_reg);
    BcLocal loc = { name, slot_reg, true };
    dyn_push(fc->arena, fc->locals, loc);
    return slot_reg;
  }
  bc_bind_local(fc, name, value_reg);
  return value_reg;
}

// A new register holding the all-bytes-zero value of type `ty`, the
// equivalent of codegen.c's `(T){0}`. For an embedded type BcOp_Alloc
// already zeroes the memory, so a fresh allocation with nothing filled in
// IS the zero value; for a scalar it's the all-zero-bits constant. Shared by
// `(zero T)` and a module-level `var`/`val` with no initializer.
static u32
bc_compile_zero_value(BcFnCtx* fc, TypeRef ty) {
  if (bc_field_is_embedded(fc->ck, ty)) {
    Layout layout = layout_of(fc->layout_cache, fc->ck, ty);
    u32     dst    = bc_alloc_reg(fc);
    bc_emit(fc, BcOp_Alloc, dst, (u32)layout.size, (u32)layout.align);
    return dst;
  }
  u32 dst = bc_alloc_reg(fc);
  bc_emit(fc, BcOp_LoadConst, dst, bc_add_const(fc, 0), 0);
  return dst;
}

// Builds a synthesized `(bool T)` result struct -- the shape
// lower_positional_struct_from_types interns for `string-to-*`,
// `vector-index-of` and `sqrt-checked` alike: a real top-level StructDecl
// with fields named "_0" (the bool) and "_1" (the value), resolved through
// struct_table_lookup/layout_field_offset like any other struct.
static u32
bc_compile_bool_t_result(BcFnCtx* fc, Token tok, String8 result_struct_name, u32 ok_reg, u32 value_reg) {
  StructEntry* se = struct_table_lookup(fc->ck, result_struct_name);
  xassert(se);
  TypeRef result_ty = {0};
  result_ty.kind = TypeKind_Named;
  result_ty.name = result_struct_name;
  Layout layout = layout_of(fc->layout_cache, fc->ck, result_ty);
  u32    dst      = bc_alloc_reg(fc);
  bc_emit(fc, BcOp_Alloc, dst, (u32)layout.size, (u32)layout.align);

  FieldLayout fl0 = layout_field_offset(fc->layout_cache, fc->ck, se, str8_lit("_0"));
  xassert(fl0.found);
  bc_emit(fc, bc_store_op_for_type(tok, fl0.type), dst, (u32)fl0.offset, ok_reg);
  FieldLayout fl1 = layout_field_offset(fc->layout_cache, fc->ck, se, str8_lit("_1"));
  xassert(fl1.found);
  bc_emit(fc, bc_store_op_for_type(tok, fl1.type), dst, (u32)fl1.offset, value_reg);
  return dst;
}

// The byte stride of one `element_ty` element within an array. Goes through
// layout_of's TypeKind_Array case, wrapping element_ty in a count-of-1
// array, rather than re-deriving the tail-padding round-up -- so this can
// never disagree with how layout_of sizes a real array of that element.
static u64
bc_element_stride(BcFnCtx* fc, TypeRef element_ty) {
  TypeRef one_elem = {0};
  one_elem.kind    = TypeKind_Array;
  one_elem.pointee = &element_ty;
  one_elem.count   = 1;
  return layout_of(fc->layout_cache, fc->ck, one_elem).size;
}

// ~~ String/struct/array CONTENT comparison (bc_compile_value_cmp and the
// helpers below). A `string`/struct/array operand's register holds the
// ADDRESS of its bytes, so plain integer register comparison there would
// compare addresses, not content -- and two freshly-boxed values with
// identical content never share an address.
//
// The semantics mirror codegen.c's cg_emit_field_eq_expr/
// cg_emit_field_cmp_expr/cg_emit_struct_comparators, and BcOp_StrCmp
// (bcvm.c) reimplements runtime/bbb_string.c's bbb_str8_compare byte for
// byte. One simplification versus codegen.c, which generates a separate
// `<Name>_eq` (bool, `&&`-chain) and `<Name>_cmp` (i32,
// first-differing-field-wins) per struct: this backend synthesizes only the
// `_cmp` chunk (bc_compile_struct_cmp_chunk) and derives equality as
// `cmp(a,b) == 0` for all six comparison kinds. That's equivalent at every
// level of the recursion -- BcOp_StrCmp's 0 case is exactly
// bbb_str8_match's true case, a scalar's `gt - lt` is 0 iff `a == b`, and a
// struct's _cmp is 0 iff every field's _cmp is 0.
//
// SCOPE, matching codegen.c's: a BinaryEq/etc node's top-level operand type
// is only ever String or Named. codegen.c never special-cases a top-level
// Array operand either -- array handling is reached only through a
// String/Named comparison's recursion -- and diverging here would drift the
// two backends apart. `type_ref_is_deep_comparable` is never called in this
// file: the checker already guarantees these operands are comparable, and
// that every struct reached directly or as a nested field passed the same
// check.

// The chunk name a struct's synthesized _cmp comparator is registered under
// in bc_compile_program's BcFnTable. It shares the ordinary BcFnTable/
// BcOp_Call/bc_fn_table_try_lookup machinery, so structs needed no new call
// dispatch -- only the byte-comparison primitive itself was new. `#` can't
// appear in a 3b identifier, so this can't collide with a real function.
static String8
bc_struct_cmp_chunk_name(Arena* arena, String8 struct_name) {
  return str8f(arena, "%.*s#cmp", str8_varg(struct_name));
}

// Compares two runs of `count` `elem_ty` elements at addresses `arr_a`/
// `arr_b`, returning a new register with a 3-way i32 result. Mirrors
// codegen.c's inline statement-expression `for` loop for an array field
// comparison, expressed as jump-backpatched bytecode since this VM has no
// statement-expression equivalent. Short-circuits on the first differing
// element, as codegen.c's loop does with an early `break`.
static u32
bc_compile_array_cmp(BcFnCtx* fc, Token tok, TypeRef elem_ty, u32 arr_a, u32 arr_b, u64 count) {
  u64 stride = bc_element_stride(fc, elem_ty);

  u32 zero_reg = bc_alloc_reg(fc);
  bc_emit(fc, BcOp_LoadConst, zero_reg, bc_add_const(fc, 0), 0);
  u32 one_reg = bc_alloc_reg(fc);
  bc_emit(fc, BcOp_LoadConst, one_reg, bc_add_const(fc, 1), 0);
  u32 count_reg = bc_alloc_reg(fc);
  bc_emit(fc, BcOp_LoadConst, count_reg, bc_add_const(fc, (i64)count), 0);
  u32 stride_reg = bc_alloc_reg(fc);
  bc_emit(fc, BcOp_LoadConst, stride_reg, bc_add_const(fc, (i64)stride), 0);

  u32 result = bc_alloc_reg(fc);
  bc_emit(fc, BcOp_Move, result, zero_reg, 0); // "equal" (0) until proven otherwise
  u32 i_reg = bc_alloc_reg(fc);
  bc_emit(fc, BcOp_Move, i_reg, zero_reg, 0);

  u32 loop_start = (u32)dyn_count(fc->code);
  u32 cond = bc_alloc_reg(fc);
  bc_emit(fc, BcOp_Lt, cond, i_reg, count_reg); // i < count
  u32 jf_done = bc_emit(fc, BcOp_JumpIfFalse, cond, 0, 0); // patched below -- all elements equal

  u32 byte_off = bc_alloc_reg(fc);
  bc_emit(fc, BcOp_Mul, byte_off, i_reg, stride_reg);
  u32 elem_a = bc_alloc_reg(fc);
  bc_emit(fc, BcOp_Add, elem_a, arr_a, byte_off);
  u32 elem_b = bc_alloc_reg(fc);
  bc_emit(fc, BcOp_Add, elem_b, arr_b, byte_off);

  u32 elem_cmp = bc_compile_value_cmp(fc, tok, elem_ty, elem_a, elem_b); // recursive -- an array of
                                                                        // arrays or of comparable
                                                                        // structs re-enters the same
                                                                        // Array/Named branches
  u32 elems_equal = bc_alloc_reg(fc);
  bc_emit(fc, BcOp_Eq, elems_equal, elem_cmp, zero_reg);
  u32 jf_diff = bc_emit(fc, BcOp_JumpIfFalse, elems_equal, 0, 0); // patched below -- a difference
                                                                      // was found at this index

  // Elements at this index are equal -- advance and loop back.
  bc_emit(fc, BcOp_Add, i_reg, i_reg, one_reg);
  bc_emit(fc, BcOp_Jump, loop_start, 0, 0);

  // Found a difference -- record it (the loop's own overall result) and
  // fall through to loop_end (no need to keep scanning).
  fc->code[jf_diff].b = (u32)dyn_count(fc->code);
  bc_emit(fc, BcOp_Move, result, elem_cmp, 0);

  fc->code[jf_done].b = (u32)dyn_count(fc->code);
  return result;
}

// A new register holding a 3-way i32 comparison of the `ty`-typed values at
// `addr_a` and `addr_b`. Used both at the top level (BinaryEq/etc below,
// where a String/Named operand's own value already IS its address, so
// addr_a/addr_b are just that expression's compiled registers) and for a
// struct field or array element (bc_compile_struct_cmp_chunk/
// bc_compile_array_cmp, where the addresses come from FieldAddr or
// index*stride arithmetic first) -- bc_field_is_embedded's always-an-address
// convention, extended from load/store to comparison.
static u32
bc_compile_value_cmp(BcFnCtx* fc, Token tok, TypeRef ty, u32 addr_a, u32 addr_b) {
  if (ty.kind == TypeKind_String) {
    u32 dst = bc_alloc_reg(fc);
    bc_emit(fc, BcOp_StrCmp, dst, addr_a, addr_b);
    return dst;
  }
  if (ty.kind == TypeKind_Named && !bc_type_is_enum(fc->ck, ty)) {
    String8 chunk_name = bc_struct_cmp_chunk_name(fc->arena, ty.name);
    u32     cmp_idx;
    b32     found = bc_fn_table_try_lookup(fc->fn_table, chunk_name, &cmp_idx);
    xassert(found); // bc_compile_program's gather pass reserves a comparator chunk for every struct
                       // that passed type_ref_is_deep_comparable, and the checker guarantees a
                       // comparison never reaches a struct that didn't
    u32 arg_first = fc->next_reg;
    bc_alloc_reg(fc); bc_alloc_reg(fc); // reserve 2 contiguous regs, TypedNodeKind_Call's convention
    bc_emit(fc, BcOp_Move, arg_first + 0, addr_a, 0);
    bc_emit(fc, BcOp_Move, arg_first + 1, addr_b, 0);
    u32 dst = bc_alloc_reg(fc);
    bc_emit(fc, BcOp_Call, dst, cmp_idx, arg_first);
    return dst;
  }
  if (ty.kind == TypeKind_Array) {
    return bc_compile_array_cmp(fc, tok, *ty.pointee, addr_a, addr_b, ty.count);
  }
  // Scalar (int/f32/f64/bool/handle/pointer/Vector -- see bc_load_op_for_type's
  // own scope note for exactly which primitive kinds this covers; u8/u16/
  // u32/u64/i8/i16/char fields hit that function's OWN pre-existing assert,
  // same as every other field access in this file, not a new gap this
  // comparison support introduces) -- load both values from their
  // addresses and compute a 3-way result as `gt - lt`: exactly one of
  // `gt`/`lt` can be 1 (or neither, if equal), so this branch-free
  // subtraction gives -1/0/1 directly from the EXISTING comparison
  // opcodes, no new arithmetic primitive needed.
  BcOp load_op = bc_load_op_for_type(tok, ty);
  u32  val_a    = bc_alloc_reg(fc); bc_emit(fc, load_op, val_a, addr_a, 0);
  u32  val_b    = bc_alloc_reg(fc); bc_emit(fc, load_op, val_b, addr_b, 0);
  BcOp lt_op, gt_op;
  if      (ty.kind == TypeKind_F64) { lt_op = BcOp_FLt;   gt_op = BcOp_FGt; }
  else if (ty.kind == TypeKind_F32) { lt_op = BcOp_F32Lt; gt_op = BcOp_F32Gt; }
  else                                { lt_op = BcOp_Lt;    gt_op = BcOp_Gt; }
  u32 lt = bc_alloc_reg(fc); bc_emit(fc, lt_op, lt, val_a, val_b);
  u32 gt = bc_alloc_reg(fc); bc_emit(fc, gt_op, gt, val_a, val_b);
  u32 dst = bc_alloc_reg(fc);
  bc_emit(fc, BcOp_Sub, dst, gt, lt);
  return dst;
}

// Synthesizes a `<StructName>#cmp` chunk for one deeply-comparable struct;
// bc_compile_program's gather pass calls this once per such struct. There's
// no TypedAst FunctionDecl behind it -- like codegen.c's
// cg_emit_struct_comparators, this emits a function with no source
// declaration, driven directly by `se`'s field list, the same field-order
// source type_ref_is_deep_comparable walks.
//
// Per field, in declaration order: compute `field_cmp` via
// bc_compile_value_cmp (so a nested struct/string/array field composes
// through the same recursion array elements use); if nonzero, return it
// immediately (first-differing-field-wins, codegen.c's `<Name>_cmp`
// semantics); after every field compares equal, return 0. Two params, both
// struct addresses, as bc_compile_value_cmp's Named branch assumes.
static BcChunk
bc_compile_struct_cmp_chunk(Checker* ck, TypedAst* tast, StructEntry* se, Arena* arena,
                             LayoutCache* layout_cache, BcFnTable* fn_table, String8 chunk_name) {
  BcFnCtx fc = {0};
  fc.ck           = ck;
  fc.tast         = tast;
  fc.arena        = arena;
  fc.layout_cache = layout_cache;
  fc.fn_table     = fn_table;
  fc.host_imports = NULL; // a synthesized comparator never makes a host call

  u32 reg_a = bc_alloc_reg(&fc);
  u32 reg_b = bc_alloc_reg(&fc);
  xassert(reg_a == 0 && reg_b == 1); // the params MUST land at registers 0/1: BcOp_Call copies
                                         // caller-supplied args starting at regs[0] of the callee's
                                         // frame, as every compiled function's param binding assumes

  TypedNode* decl = &tast->nodes[se->decl];
  xassert(decl->kind == TypedNodeKind_StructDecl);

  foreach_index(i, decl->struct_decl.field_count) {
    Param*      f  = &tast->params[decl->struct_decl.field_first + i];
    FieldLayout fl = layout_field_offset(layout_cache, ck, se, f->name);
    xassert(fl.found);

    u32 addr_a = bc_alloc_reg(&fc);
    bc_emit(&fc, BcOp_FieldAddr, addr_a, reg_a, (u32)fl.offset);
    u32 addr_b = bc_alloc_reg(&fc);
    bc_emit(&fc, BcOp_FieldAddr, addr_b, reg_b, (u32)fl.offset);

    u32 field_cmp = bc_compile_value_cmp(&fc, decl->token, fl.type, addr_a, addr_b);

    u32 zero_reg = bc_alloc_reg(&fc);
    bc_emit(&fc, BcOp_LoadConst, zero_reg, bc_add_const(&fc, 0), 0);
    u32 is_different = bc_alloc_reg(&fc);
    bc_emit(&fc, BcOp_Neq, is_different, field_cmp, zero_reg);
    u32 jf = bc_emit(&fc, BcOp_JumpIfFalse, is_different, 0, 0); // patched below -- fields EQUAL,
                                                                     // skip the early return
    bc_emit(&fc, BcOp_Return, field_cmp, 0, 0); // fields DIFFER -- return immediately
    fc.code[jf].b = (u32)dyn_count(fc.code);
  }

  u32 zero_final = bc_alloc_reg(&fc);
  bc_emit(&fc, BcOp_LoadConst, zero_final, bc_add_const(&fc, 0), 0);
  bc_emit(&fc, BcOp_Return, zero_final, 0, 0); // every field was equal

  BcChunk chunk = {0};
  chunk.code          = fc.code;
  chunk.consts        = fc.consts;
  chunk.num_registers = fc.next_reg;
  chunk.param_count   = 2;
  chunk.name          = chunk_name;
  chunk.string_fixups = fc.string_fixups; // NULL -- a comparator has no string literals
  chunk.layout_fixups = fc.layout_fixups; // NULL too, for the same reason: no Map/Set call sites
  return chunk;
}

// Resolves a FieldAccess node to (base_reg, field layout) WITHOUT emitting
// a load -- shared by the read path (TypedNodeKind_FieldAccess below) and
// the write path (SetTargetKind_Field in TypedNodeKind_SetExpr), so the
// two can never disagree about where a field actually lives.
static void
bc_resolve_field_access(BcFnCtx* fc, TypedIndex field_access_idx, u32* out_base_reg, FieldLayout* out_fl) {
  TypedNode* n = &fc->tast->nodes[field_access_idx];
  xassert(n->kind == TypedNodeKind_FieldAccess);
  TypeRef base_ty  = fc->ck->resolved_types[n->field_access.base];
  u32     base_reg = bc_compile_expr(fc, n->field_access.base);
  // A by-value `T` and a real `T*` have the same representation here (a
  // register holding the address), so auto-deref changes nothing: base_reg
  // holds the struct's address either way.
  TypeRef struct_ty = base_ty.kind == TypeKind_Pointer ? *base_ty.pointee : base_ty;
  xassert(struct_ty.kind == TypeKind_Named);
  StructEntry* se = struct_table_lookup(fc->ck, struct_ty.name);
  xassert(se);
  FieldLayout fl = layout_field_offset(fc->layout_cache, fc->ck, se, n->field_access.field);
  xassert(fl.found);
  *out_base_reg = base_reg;
  *out_fl       = fl;
}

// Resolves `(nth base index)` to an element's address WITHOUT emitting a
// load -- shared by the read path (IndexAccess) and the write path
// (SetTargetKind_Index), same reasoning as bc_resolve_field_access.
//
// Valid on an Array-, Pointer- or Vector-typed base, with no branching on
// which: all three carry their element type in the same `.pointee` field,
// and a Vector's runtime representation is already a bare T*, so one
// base-register-plus-index*stride computation is correct for all three.
static void
bc_resolve_index_access(BcFnCtx* fc, TypedIndex base_idx, TypedIndex index_idx,
                         u32* out_elem_addr_reg, TypeRef* out_elem_ty) {
  TypeRef base_ty = fc->ck->resolved_types[base_idx];
  xassert(base_ty.kind == TypeKind_Array || base_ty.kind == TypeKind_Pointer || base_ty.kind == TypeKind_Vector);
  TypeRef elem_ty = *base_ty.pointee;
  u32     base_reg  = bc_compile_expr(fc, base_idx);
  u32     index_reg = bc_compile_expr(fc, index_idx);
  u64     stride     = bc_element_stride(fc, elem_ty);
  u32     stride_reg = bc_alloc_reg(fc);
  bc_emit(fc, BcOp_LoadConst, stride_reg, bc_add_const(fc, (i64)stride), 0);
  u32 byte_offset_reg = bc_alloc_reg(fc);
  bc_emit(fc, BcOp_Mul, byte_offset_reg, index_reg, stride_reg);
  u32 elem_addr_reg = bc_alloc_reg(fc);
  bc_emit(fc, BcOp_Add, elem_addr_reg, base_reg, byte_offset_reg); // integer add doubles as pointer
                                                                      // arithmetic -- both are just
                                                                      // 8-byte register values
  *out_elem_addr_reg = elem_addr_reg;
  *out_elem_ty        = elem_ty;
}

// Stores `val_reg` into (base_reg + offset), honoring the embedded-vs-scalar
// distinction every field/element/deref write needs: a scalar gets an
// ordinary typed store; a struct/string gets a real byte copy, since
// val_reg is only the ADDRESS of a value living elsewhere and can't be
// stored as one 8-byte unit. An array target isn't supported yet.
// Forward-declared because it calls bc_copy_struct_bytes/
// bc_copy_boxed_string below.
static void bc_store_value(BcFnCtx* fc, Token tok, u32 base_reg, u64 offset, TypeRef target_ty, u32 val_reg);

// Copies `size` bytes from the struct-valued register `src_reg` (holding the
// source struct's address) into (base_reg + base_offset). For filling a
// nested by-value struct field or element from an ARBITRARY expression; an
// inline StructLiteral instead recurses through
// bc_compile_struct_fields_into with no copy at all.
//
// Unrolled at compile time into 8-byte-word copies -- `size` is always known
// from layout_of -- then the 1-to-7-byte tail in descending 4/2/1 steps, which
// is every remainder a `packed` struct can produce. Unsigned loads throughout:
// this moves BYTES, and sign-extending one into the register before the store
// truncates it back only happens to be harmless.
//
// A remainder other than 4 used to `xassert(!"...")`, which had two problems.
// It fired on legal 3b -- `(packed (struct P [a i32 b i16]))` is 6 bytes, and
// binding one by value aborted `3b run` with a raw C assertion naming a line of
// bcgen.c. And xassert compiles to `(void)` without XDEBUG (base.h), so the
// same program built for release would copy the whole words and leave the tail
// bytes holding whatever the fresh allocation had. Handling every width is a
// smaller change than reporting the gap well.
static void
bc_copy_struct_bytes(BcFnCtx* fc, u32 base_reg, u64 base_offset, u32 src_reg, u64 size) {
  u64 words = size / 8;
  foreach_index(i, words) {
    u32 tmp = bc_alloc_reg(fc);
    bc_emit(fc, BcOp_LoadFieldI64, tmp, src_reg, (u32)(i * 8));
    bc_emit(fc, BcOp_StoreFieldI64, base_reg, (u32)(base_offset + i * 8), tmp);
  }
  for (u64 done = words * 8; done < size; /* advanced below */) {
    u64  left  = size - done;
    u64  chunk = (left >= 4) ? 4 : (left >= 2) ? 2 : 1;
    BcOp load  = (chunk == 4) ? BcOp_LoadFieldU32  : (chunk == 2) ? BcOp_LoadFieldU16  : BcOp_LoadFieldU8;
    BcOp store = (chunk == 4) ? BcOp_StoreFieldI32 : (chunk == 2) ? BcOp_StoreFieldI16 : BcOp_StoreFieldI8;
    u32  tmp   = bc_alloc_reg(fc);
    bc_emit(fc, load, tmp, src_reg, (u32)done);
    bc_emit(fc, store, base_reg, (u32)(base_offset + done), tmp);
    done += chunk;
  }
}

// Copies an already-boxed string's {ptr,size} header (two words) from
// `src_reg` (the header's own address) into (base_reg + offset) -- for the
// arbitrary-expression cases of struct-field and array-element fill. See
// bc_fill_string_field for the compile-time-known-bytes counterpart.
static void
bc_copy_boxed_string(BcFnCtx* fc, u32 base_reg, u64 offset, u32 src_reg) {
  u32 word0 = bc_alloc_reg(fc);
  u32 word1 = bc_alloc_reg(fc);
  bc_emit(fc, BcOp_LoadFieldI64, word0, src_reg, 0);
  bc_emit(fc, BcOp_LoadFieldI64, word1, src_reg, 8);
  bc_emit(fc, BcOp_StoreFieldI64, base_reg, (u32)offset, word0);
  bc_emit(fc, BcOp_StoreFieldI64, base_reg, (u32)(offset + 8), word1);
}

// An INDEPENDENT copy of the struct/string/array value at `src_reg` (itself
// the value's address). A fresh BcOp_Alloc sized via layout_of, filled by
// bc_copy_struct_bytes -- which despite the name is a plain byte-range
// copier, equally correct for a String8's 16-byte {ptr,size} HEADER (a
// header copy, not a deep copy of the character bytes, matching C's own
// String8 assignment) or a whole array's byte range.
//
// Called wherever 3b's by-value semantics need a real copy instead of the
// usual pass-the-address shortcut. See VALUE-COPY SEMANTICS at the top of
// this file for which sites those are -- in particular, an array-typed
// ARGUMENT must NOT come through here, since a C array parameter decays and
// aliases.
static u32
bc_compile_value_copy(BcFnCtx* fc, TypeRef ty, u32 src_reg) {
  Layout layout = layout_of(fc->layout_cache, fc->ck, ty);
  u32    dst    = bc_alloc_reg(fc);
  bc_emit(fc, BcOp_Alloc, dst, (u32)layout.size, (u32)layout.align);
  bc_copy_struct_bytes(fc, dst, 0, src_reg, layout.size);
  return dst;
}

static void
bc_store_value(BcFnCtx* fc, Token tok, u32 base_reg, u64 offset, TypeRef target_ty, u32 val_reg) {
  if ((target_ty.kind == TypeKind_Named && !bc_type_is_enum(fc->ck, target_ty)) || target_ty.kind == TypeKind_Array) {
    // bc_copy_struct_bytes has no struct-specific logic, only a
    // compile-time-known size, so a whole-array `set` through deref/field/
    // index is the same machinery as a struct field copy.
    Layout layout = layout_of(fc->layout_cache, fc->ck, target_ty);
    bc_copy_struct_bytes(fc, base_reg, offset, val_reg, layout.size);
  } else if (target_ty.kind == TypeKind_String) {
    bc_copy_boxed_string(fc, base_reg, offset, val_reg);
  } else {
    bc_emit(fc, bc_store_op_for_type(tok, target_ty), base_reg, (u32)offset, val_reg);
  }
}

// Writes `val_reg` into whatever SCALAR lvalue `target_idx` names --
// Identifier (local Move / global StoreGlobal), FieldAccess, IndexAccess or
// UnaryDeref, the four shapes checker.c's `swap_target_is_valid` allows for
// `swap`'s operands. `swap` is the one caller.
//
// Scalar-only: an embedded-typed swap needs a real byte-content exchange,
// not a register rebind -- see `swap`'s TypedNodeKind_Call case for why the
// shortcut SetTargetKind_Identifier safely uses elsewhere is wrong here.
static void
bc_compile_lvalue_write(BcFnCtx* fc, Token tok, TypedIndex target_idx, TypeRef target_ty, u32 val_reg) {
  TypedNode* target = &fc->tast->nodes[target_idx];
  switch (target->kind) {
    case TypedNodeKind_Identifier: {
      BcLocal* loc = bc_local_try_lookup(fc, target->ident.name);
      if (loc) {
        // An address-taken scalar local's register holds an ADDRESS, not the
        // value; a bare Move would not reach whoever holds `&this-local`.
        if (loc->is_addr_taken) bc_emit(fc, bc_store_op_for_type(tok, target_ty), loc->reg, 0, val_reg);
        else                     bc_emit(fc, BcOp_Move, loc->reg, val_reg, 0);
        return;
      }
      u32 slot;
      if (fc->global_table && bc_fn_table_try_lookup(fc->global_table, target->ident.name, &slot)) {
        bc_emit(fc, BcOp_StoreGlobal, slot, val_reg, 0);
        return;
      }
      xassert(!"bc_compile_lvalue_write: identifier unresolved as either a local or a global -- "
               "the checker should already have caught this");
      return;
    }
    case TypedNodeKind_FieldAccess: {
      u32         base_reg;
      FieldLayout fl;
      bc_resolve_field_access(fc, target_idx, &base_reg, &fl);
      bc_store_value(fc, tok, base_reg, fl.offset, target_ty, val_reg);
      return;
    }
    case TypedNodeKind_IndexAccess: {
      u32     elem_addr_reg;
      TypeRef elem_ty;
      bc_resolve_index_access(fc, target->index_access.base, target->index_access.index, &elem_addr_reg, &elem_ty);
      bc_store_value(fc, tok, elem_addr_reg, 0, target_ty, val_reg);
      return;
    }
    case TypedNodeKind_UnaryDeref: {
      u32 addr_reg = bc_compile_expr(fc, target->unary.expr);
      bc_store_value(fc, tok, addr_reg, 0, target_ty, val_reg);
      return;
    }
    default:
      xassert(!"bc_compile_lvalue_write: target kind isn't one swap_target_is_valid allows, "
               "the checker should already have caught this");
  }
}

// Copies `s`'s bytes into this chunk's compile-time arena -- stable for the
// chunk's lifetime, like its code/consts arrays -- and returns the copy: the
// backing storage for a string literal's `.str`, computed once since a
// literal's content never changes at runtime.
static u8*
bc_intern_string_bytes(BcFnCtx* fc, String8 s) {
  u8* copy = push_array(fc->arena, u8, s.size);
  MemoryCopy(copy, s.str, s.size);
  return copy;
}

// Records that consts[const_slot] holds a real pointer bcio.c must know how
// to reconstruct (see BcStringFixup in bytecode.h). Descriptive only: it
// changes nothing about how a freshly-compiled chunk uses that slot.
static void
bc_add_string_fixup(BcFnCtx* fc, u32 const_slot, b32 is_header, String8 bytes) {
  BcStringFixup fx = { const_slot, is_header, bytes };
  dyn_push(fc->arena, fc->string_fixups, fx);
}

// Gives `layout` a stable compile-time address, puts THAT address in the
// const pool for BcOp_MapSet/MapGet/MapRemove to read through, and records
// the matching BcLayoutFixup so bcio.c can rebuild the slot from the
// descriptor's own FIELDS after a save/load (see BcLayoutFixup in
// bytecode.h). The only way a Map/Set call site should ever reach the const
// pool: a bare bc_add_const of a layout pointer compiles and runs perfectly
// in-process, and only falls apart once the program is cached and reloaded
// somewhere else -- which is why this pairs the two steps in one call
// rather than leaving the fixup as a second thing to remember.
static u32
bc_add_layout_const(BcFnCtx* fc, BcHashSlotLayout layout) {
  BcHashSlotLayout* boxed = push_one(fc->arena, BcHashSlotLayout);
  *boxed = layout;
  u32           c  = bc_add_const(fc, (i64)(intptr_t)boxed);
  BcLayoutFixup fx = { c, layout };
  dyn_push(fc->arena, fc->layout_fixups, fx);
  return c;
}

// Loads a BOXED string value: a register holding the address of a
// compile-time-built 16-byte {ptr, size} header. For a standalone string
// literal; a string FIELD inside a struct uses bc_fill_string_field instead.
static u32
bc_compile_string_literal(BcFnCtx* fc, String8 value) {
  u8*      bytes  = bc_intern_string_bytes(fc, value);
  String8* header = push_one(fc->arena, String8);
  header->str  = bytes;
  header->size = value.size;
  u32 dst = bc_alloc_reg(fc);
  u32 c   = bc_add_const(fc, (i64)(intptr_t)header);
  bc_add_string_fixup(fc, c, /*is_header=*/true, value);
  bc_emit(fc, BcOp_LoadConst, dst, c, 0);
  return dst;
}

// Fills a string FIELD embedded inline in a struct, writing String8's two
// 8-byte halves ({u8* str; u64 size;}, in that order) at field_offset and
// +8. There's no user-declared StructEntry for the `string` primitive to run
// layout_field_offset against, so the field order is hardcoded here just as
// layout.c hardcodes TypeKind_String's own {16, 8}.
static void
bc_fill_string_field(BcFnCtx* fc, u32 base_reg, u64 field_offset, String8 value) {
  u8* bytes = bc_intern_string_bytes(fc, value);
  u32 ptr_const = bc_add_const(fc, (i64)(intptr_t)bytes);
  bc_add_string_fixup(fc, ptr_const, /*is_header=*/false, value);
  u32 len_const = bc_add_const(fc, (i64)value.size);
  u32 ptr_reg   = bc_alloc_reg(fc);
  u32 len_reg   = bc_alloc_reg(fc);
  bc_emit(fc, BcOp_LoadConst, ptr_reg, ptr_const, 0);
  bc_emit(fc, BcOp_LoadConst, len_reg, len_const, 0);
  bc_emit(fc, BcOp_StoreFieldI64, base_reg, (u32)field_offset, ptr_reg);
  bc_emit(fc, BcOp_StoreFieldI64, base_reg, (u32)(field_offset + 8), len_reg);
}

// Whether an operand type needs the UNSIGNED half of an op pair that has one
// (divide, remainder, the four orderings -- see BcOp_DivU). Deliberately
// narrower than bc_type_kind_is_unsigned_int below, which answers a different
// question for `bit-shr`: u8/u16/u32 are held zero-extended and so are never
// negative in their registers, leaving the signed opcodes already correct for
// them. Only a u64 has no spare bits to be canonical in -- and a Pointer,
// which C orders as the unsigned address it is.
static b32
bc_type_needs_unsigned_ops(TypeRef ty) {
  return ty.kind == TypeKind_U64 || ty.kind == TypeKind_Pointer;
}

// Which family of opcode a `BinaryXyz` node needs -- decided by the
// OPERAND type (both operands are already guaranteed the same type by the
// checker), not the node's own result type (comparisons resolve to Bool
// overall regardless of whether the operands were int or float).
//
// `is_unsigned` (bc_type_needs_unsigned_ops of that same operand type) picks
// the unsigned half of the ops that come in pairs. Add/Sub/Mul are
// bit-identical either way in two's complement, and equality can't care about
// signedness at all, so those have no pair to choose from.
static BcOp
bc_int_op_for_kind(TypedNodeKind kind, b32 is_unsigned) {
  switch (kind) {
    case TypedNodeKind_BinaryAdd: return BcOp_Add;
    case TypedNodeKind_BinarySub: return BcOp_Sub;
    case TypedNodeKind_BinaryMul: return BcOp_Mul;
    case TypedNodeKind_BinaryDiv: return is_unsigned ? BcOp_DivU : BcOp_Div;
    case TypedNodeKind_BinaryMod: return is_unsigned ? BcOp_ModU : BcOp_Mod;
    case TypedNodeKind_BinaryEq:  return BcOp_Eq;
    case TypedNodeKind_BinaryNeq: return BcOp_Neq;
    case TypedNodeKind_BinaryLt:  return is_unsigned ? BcOp_LtU : BcOp_Lt;
    case TypedNodeKind_BinaryLe:  return is_unsigned ? BcOp_LeU : BcOp_Le;
    case TypedNodeKind_BinaryGt:  return is_unsigned ? BcOp_GtU : BcOp_Gt;
    default:                      return is_unsigned ? BcOp_GeU : BcOp_Ge; // TypedNodeKind_BinaryGe
  }
}

static BcOp
bc_f64_op_for_kind(TypedNodeKind kind) {
  switch (kind) {
    case TypedNodeKind_BinaryAdd: return BcOp_FAdd;
    case TypedNodeKind_BinarySub: return BcOp_FSub;
    case TypedNodeKind_BinaryMul: return BcOp_FMul;
    case TypedNodeKind_BinaryDiv: return BcOp_FDiv;
    case TypedNodeKind_BinaryEq:  return BcOp_FEq;
    case TypedNodeKind_BinaryNeq: return BcOp_FNeq;
    case TypedNodeKind_BinaryLt:  return BcOp_FLt;
    case TypedNodeKind_BinaryLe:  return BcOp_FLe;
    case TypedNodeKind_BinaryGt:  return BcOp_FGt;
    default:                      return BcOp_FGe; // TypedNodeKind_BinaryGe
                                                       // (BinaryMod on floats is unreachable --
                                                       // the checker requires integer operands)
  }
}

static BcOp
bc_f32_op_for_kind(TypedNodeKind kind) {
  switch (kind) {
    case TypedNodeKind_BinaryAdd: return BcOp_F32Add;
    case TypedNodeKind_BinarySub: return BcOp_F32Sub;
    case TypedNodeKind_BinaryMul: return BcOp_F32Mul;
    case TypedNodeKind_BinaryDiv: return BcOp_F32Div;
    case TypedNodeKind_BinaryEq:  return BcOp_F32Eq;
    case TypedNodeKind_BinaryNeq: return BcOp_F32Neq;
    case TypedNodeKind_BinaryLt:  return BcOp_F32Lt;
    case TypedNodeKind_BinaryLe:  return BcOp_F32Le;
    case TypedNodeKind_BinaryGt:  return BcOp_F32Gt;
    default:                      return BcOp_F32Ge; // TypedNodeKind_BinaryGe (BinaryMod
                                                          // unreachable, same reason as f64)
  }
}

// Picks bc_int_op_for_kind/bc_f64_op_for_kind/bc_f32_op_for_kind by an
// OPERAND's type -- the 3-way dispatch BinaryAdd's own case does inline,
// factored out for the abs/min/max/clamp family below, which needs it from
// outside that switch and has no BinaryLt/BinarySub node to read a resolved
// type off. `kind` here just names which arithmetic family to pick.
static BcOp
bc_typed_op_for_kind(TypeRef ty, TypedNodeKind kind) {
  if (ty.kind == TypeKind_F64) return bc_f64_op_for_kind(kind);
  if (ty.kind == TypeKind_F32) return bc_f32_op_for_kind(kind);
  return bc_int_op_for_kind(kind, bc_type_needs_unsigned_ops(ty));
}

// dst = cond_reg ? true_reg : false_reg, over ALREADY-COMPILED registers --
// neither arm is re-evaluated. The jump-and-Move shape IfExpr uses, factored
// out for the abs/min/max/clamp family, which needs to pick between two
// known values repeatedly with no `if` node to compile through.
static u32
bc_compile_select(BcFnCtx* fc, u32 cond_reg, u32 true_reg, u32 false_reg) {
  u32 dst  = bc_alloc_reg(fc);
  u32 jf   = bc_emit(fc, BcOp_JumpIfFalse, cond_reg, 0, 0); // patched below
  bc_emit(fc, BcOp_Move, dst, true_reg, 0);
  u32 jend = bc_emit(fc, BcOp_Jump, 0, 0, 0); // patched below
  fc->code[jf].b = (u32)dyn_count(fc->code);
  bc_emit(fc, BcOp_Move, dst, false_reg, 0);
  fc->code[jend].a = (u32)dyn_count(fc->code);
  return dst;
}

// An ADDRESS holding `val_reg`'s value: `val_reg` unchanged if `ty` is
// already embedded, otherwise a fresh allocation sized to `ty` with the
// scalar stored into it. For anywhere a byte-range pointer is needed whether
// the type is scalar or embedded -- currently only the Map/Set builtins,
// since bcmap.c's generic algorithm reads and writes raw bytes through a
// `void*` key/value pointer with no already-in-a-register fast path.
static u32
bc_compile_addr_of(BcFnCtx* fc, Token tok, TypeRef ty, u32 val_reg) {
  if (bc_field_is_embedded(fc->ck, ty)) return val_reg;
  Layout layout = layout_of(fc->layout_cache, fc->ck, ty);
  u32    buf_reg = bc_alloc_reg(fc);
  bc_emit(fc, BcOp_Alloc, buf_reg, (u32)layout.size, (u32)layout.align);
  bc_emit(fc, bc_store_op_for_type(tok, ty), buf_reg, 0, val_reg);
  return buf_reg;
}

// Compiles an argument consumed as a raw INTEGER -- a byte count or byte
// value for the mem-* family -- converting a float first. The checker admits
// any numeric type in those positions, which costs codegen.c nothing (C's
// implicit conversion turns `memset(p, 7, 8.0)`'s double into a size_t) but
// would be catastrophic here: a register holding an f64 BIT PATTERN read as
// a size is an enormous garbage length. Widens f32 first, as the libm
// opcodes do.
static u32
bc_compile_size_arg(BcFnCtx* fc, TypedIndex idx) {
  TypeRef ty  = fc->ck->resolved_types[idx];
  u32     reg = bc_compile_expr(fc, idx);
  if (ty.kind != TypeKind_F32 && ty.kind != TypeKind_F64) return reg;
  u32 as_f64 = reg;
  if (ty.kind == TypeKind_F32) {
    as_f64 = bc_alloc_reg(fc);
    bc_emit(fc, BcOp_F32ToF64, as_f64, reg, 0);
  }
  u32 result = bc_alloc_reg(fc);
  bc_emit(fc, BcOp_F64ToInt, result, as_f64, 0);
  return result;
}

// Builds the {coll_addr, key_ptr, layout_ptr} register block BcOp_MapGet and
// BcOp_MapRemove both expect. Shared by map-get/map-contains?/map-remove/
// set-contains?/set-remove, which all read a Map/Set by key and differ only
// in what they do with the opcode's result: a nilable value-pointer for
// map-get, a `!= 0` check for *-contains?, the removal opcode's own bool for
// *-remove. `coll_idx` must resolve to TypeKind_Map or TypeKind_Set, which
// the checker guarantees.
//
// The BcHashSlotLayout descriptor is baked into the const pool as a real
// pointer, the same compile-time-address convention a boxed string literal's
// header uses. That means a FRESH descriptor per call site rather than one
// cached per (key_ty, value_ty) -- dedupable, but a compile-time-only cost.
static u32
bc_compile_map_key_args(BcFnCtx* fc, TypedIndex coll_idx, TypedIndex key_idx) {
  TypeRef coll_ty = fc->ck->resolved_types[coll_idx];
  b32     is_map   = coll_ty.kind == TypeKind_Map;
  TypeRef key_ty   = is_map ? *coll_ty.map_key : *coll_ty.pointee;

  BcHashSlotLayout layout;
  if (is_map) {
    TypeRef value_ty = *coll_ty.pointee;
    layout = bc_hash_slot_layout(fc->layout_cache, fc->ck, key_ty, &value_ty);
  } else {
    layout = bc_hash_slot_layout(fc->layout_cache, fc->ck, key_ty, NULL);
  }

  u32 coll_reg = bc_compile_expr(fc, coll_idx);
  u32 key_val   = bc_compile_expr(fc, key_idx);
  u32 key_ptr   = bc_compile_addr_of(fc, fc->tast->nodes[key_idx].token, key_ty, key_val);
  u32 layout_reg = bc_alloc_reg(fc);
  bc_emit(fc, BcOp_LoadConst, layout_reg, bc_add_layout_const(fc, layout), 0);

  u32 arg_first = fc->next_reg;
  bc_alloc_reg(fc); bc_alloc_reg(fc); bc_alloc_reg(fc); // reserve 3 contiguous regs
  bc_emit(fc, BcOp_Move, arg_first + 0, coll_reg,   0);
  bc_emit(fc, BcOp_Move, arg_first + 1, key_ptr,    0);
  bc_emit(fc, BcOp_Move, arg_first + 2, layout_reg, 0);
  return arg_first;
}

static b32
bc_type_kind_is_unsigned_int(TypeKind k) {
  return k == TypeKind_U8 || k == TypeKind_U16 || k == TypeKind_U32 || k == TypeKind_U64;
}

// `operand_kind` is the SHIFTED value's type, and picks between the two
// right shifts (see BcOp_ShrU in bytecode.h); every other op here is
// signedness-blind. A float operand, which the checker permits for bitwise
// ops, takes the arithmetic BcOp_Shr -- shifting a raw bit pattern has no
// signed/unsigned reading to get right.
static BcOp
bc_bitwise_op_for_kind(TypedNodeKind kind, TypeKind operand_kind) {
  switch (kind) {
    case TypedNodeKind_BinaryBitAnd: return BcOp_BitAnd;
    case TypedNodeKind_BinaryBitOr:  return BcOp_BitOr;
    case TypedNodeKind_BinaryShl:    return BcOp_Shl;
    case TypedNodeKind_BinaryShr:    return bc_type_kind_is_unsigned_int(operand_kind) ? BcOp_ShrU : BcOp_Shr;
    default:                          return BcOp_BitXor; // TypedNodeKind_BinaryBitXor
  }
}

// BcOp_Narrow's width/sign code for `kind`, or 0 for a type that already
// fills a 64-bit register and needs no fixup at all: I64/U64/Pointer/Any, a
// named enum's backing width, and every non-integer. Bool is one of those --
// its callers treat it as a truthiness test rather than a width adjustment.
//
// Char lands on the SIGNED side for the same reason bc_load_op_for_type
// reads a `char` field with the sign-extending BcOp_LoadFieldI8: plain C
// `char` is signed on this codebase's only target, so codegen.c's `(char)`
// cast sign-extends and this backend has to agree. `(cast char -1)` widens
// back to -1 on both.
static u32
bc_narrow_code_for_kind(TypeKind kind) {
  switch (kind) {
    case TypeKind_I8:   return 8  | BC_NARROW_SIGNED;
    case TypeKind_I16:  return 16 | BC_NARROW_SIGNED;
    case TypeKind_I32:  return 32 | BC_NARROW_SIGNED;
    case TypeKind_U8:   return 8;
    case TypeKind_U16:  return 16;
    case TypeKind_U32:  return 32;
    case TypeKind_Char: return 8  | BC_NARROW_SIGNED; // signed `char`, see above
    default:            return 0;
  }
}

// Truncates `src_reg` to `target_kind`'s real width in a FRESH register,
// leaving `src_reg` alone -- what `(cast u8 x)` needs, since `x` may be a
// variable's own register.
static u32
bc_compile_int_narrow(BcFnCtx* fc, TypeKind target_kind, u32 src_reg) {
  u32 code = bc_narrow_code_for_kind(target_kind);
  if (!code) return src_reg;
  u32 dst = bc_alloc_reg(fc);
  bc_emit(fc, BcOp_Narrow, dst, src_reg, code);
  return dst;
}

// Wraps an ARITHMETIC result back into `result_ty`'s real width, IN PLACE --
// `reg` is always a register the op just wrote and nothing else aliases, so
// this costs one instruction and no new register. Registers here are all
// i64, so `(* x 3u32)` on a u32 otherwise computes at 64 bits and keeps
// every bit a real C `u32` would have dropped -- native codegen wraps
// because it emits real C types, and the two backends then print different
// numbers for the same source (PCG32 in native_pkgs/rng depends on exactly
// this wraparound).
//
// Applied EAGERLY, right after the op, rather than lazily at the points
// where a value is finally observed. The register file carries no type, so
// everything downstream -- signed Lt/Le/Gt/Ge, Div/Mod, the StoreField
// widths, BcOp_PrintU64, bc_compile_int_narrow itself -- already assumes a
// narrow-typed register holds the canonically extended form of its value.
// Keeping that invariant true at all times is what makes those consumers
// correct, and it is one decision per op; the lazy version would have to
// track which registers are still dirty across branch joins and would still
// need a narrow at every compare, store, call argument, return and print --
// MORE sites, not fewer. What made eager affordable is BcOp_Narrow being a
// single instruction: `(+ i 1)` on an i32 goes from one instruction to two,
// not to four.
//
// Only the ops that can actually carry a value out of the type's range go
// through here. BitAnd/BitOr/BitXor and Shr are deliberately NOT narrowed:
// on canonically-extended operands they preserve the form on their own --
// bitwise ops see identical high bits in both operands (all zeros, or all
// copies of a sign that bit 31/15/7 agrees with), and a right shift only
// ever moves a value toward zero.
static void
bc_narrow_arith_result(BcFnCtx* fc, TypeRef result_ty, u32 reg) {
  u32 code = bc_narrow_code_for_kind(result_ty.kind);
  if (code) bc_emit(fc, BcOp_Narrow, reg, reg, code);
}

// An enum/flags variant's final integer value -- a compile-time constant,
// since `Name/Variant` always resolves to a fixed number. Replicates
// codegen.c's cg_enum_decl auto-assignment rather than deriving a second
// version that could drift: `enum` assigns sequential values from 0
// (next_auto = value + 1); `flags` assigns bit POSITIONS, storing
// `1 << next_auto`. An explicit `value` overrides auto-assignment for that
// variant, and later variants continue from it.
//
// Must walk every variant from the start, threading `next_auto`, even though
// only one variant's value is wanted -- an earlier variant's assignment
// affects every later one's default.
static i64
bc_enum_variant_value(TypedAst* tast, TypedNode* decl, String8 variant_name) {
  i64 next_auto = 0;
  foreach_index(i, decl->enum_decl.variant_count) {
    EnumVariant* v = &tast->enum_variants[decl->enum_decl.variant_first + i];
    i64 value;
    if      (v->has_explicit_value)    value = v->value;
    else if (decl->enum_decl.is_flags) value = ((i64)1) << next_auto;
    else                                 value = next_auto;
    if (str8_match(v->name, variant_name, 0)) return value;
    next_auto = decl->enum_decl.is_flags ? (next_auto + 1) : (value + 1);
  }
  xassert(!"bc_enum_variant_value: variant not found -- the checker should already have caught this");
  return 0;
}

// A new register holding 1 if `idx`'s value is C-style truthy, 0 otherwise.
// "Truthy" is the same `regs[x] == 0` test BcOp_JumpIfFalse performs for
// `if`/`while`, materialized as a 0/1 value instead of driving a branch --
// checker.c ties `not`/`and`/`or`'s operand typing to that same rule. An
// embedded operand's value is its address, never null once allocated, so it
// is always truthy, matching what `if` on such a value already does.
//
// Used by `and`/`or`'s short-circuit compilation for the non-short-circuited
// operand's contribution. `not` doesn't need it: Eq-against-zero is the
// negation directly, one op fewer.
static u32
bc_compile_truthy(BcFnCtx* fc, TypedIndex idx) {
  u32 v        = bc_compile_expr(fc, idx);
  u32 zero_reg = bc_alloc_reg(fc);
  bc_emit(fc, BcOp_LoadConst, zero_reg, bc_add_const(fc, 0), 0);
  u32 dst = bc_alloc_reg(fc);
  bc_emit(fc, BcOp_Neq, dst, v, zero_reg);
  return dst;
}

// Compiles every statement of `block_idx` in sequence, returning the
// register holding the LAST statement's value. codegen.c's cg_block_as_expr
// gets this "last expression is the value" behavior for free from C's own
// GNU statement-expression semantics; a flat instruction stream has no such
// implicit trailing value, so this tracks it explicitly instead.
static u32
bc_compile_block(BcFnCtx* fc, TypedIndex block_idx) {
  TypedNode* n = &fc->tast->nodes[block_idx];
  xassert(n->kind == TypedNodeKind_Block);
  u64 locals_mark = dyn_count(fc->locals);
  u32 last_reg     = 0;
  foreach_index(i, n->block.stmt_count) {
    TypedIndex stmt = fc->tast->extra[n->block.stmt_first + i];
    last_reg = bc_compile_expr(fc, stmt);
  }
  // Pop the bindings THIS block introduced back off fc->locals. Because
  // bc_local_try_lookup scans backward, a name left in the array after its
  // block closes would stay the most recent binding for that name forever,
  // so an outer reference to the same name used after this block would keep
  // resolving to the shadowed inner register.
  //
  // Every block -- function body, `do`, an `if` branch, a loop body, a
  // `let`/`scratch` body -- funnels through here, so this one truncation
  // covers them all. No register is freed by it (the bump allocator never
  // reclaims), only the name lookup entry, so an already-compiled reference
  // to the shadowed local still reads its own register.
  if (fc->locals) dyn_hdr(fc->locals)->count = locals_mark;
  return last_reg;
}

static u32
bc_compile_expr(BcFnCtx* fc, TypedIndex idx) {
  TypedNode* n = &fc->tast->nodes[idx];
  switch (n->kind) {
    case TypedNodeKind_IntLiteral: {
      u32 dst = bc_alloc_reg(fc);
      u32 c   = bc_add_const(fc, n->int_lit.value);
      bc_emit(fc, BcOp_LoadConst, dst, c, 0);
      return dst;
    }
    case TypedNodeKind_FloatLiteral: {
      TypeKind float_kind = fc->ck->resolved_types[idx].kind;
      u32      dst        = bc_alloc_reg(fc);
      i64      bits        = 0;
      if (float_kind == TypeKind_F64) {
        MemoryCopy(&bits, &n->float_lit.value, sizeof(f64)); // bit-reinterpret f64 -> i64 so it
                                                                  // can live in the shared const pool
      } else if (float_kind == TypeKind_F32) {
        f32 narrowed = (f32)n->float_lit.value;
        u32 bits32; MemoryCopy(&bits32, &narrowed, sizeof(bits32));
        bits = (i64)(u64)bits32; // zero-extended -- an f32 register only ever looks at the low 4 bytes
      } else {
        xassert(!"bc_compile_expr: FloatLiteral resolved to neither f32 nor f64 -- unexpected");
      }
      u32 c = bc_add_const(fc, bits);
      bc_emit(fc, BcOp_LoadConst, dst, c, 0);
      return dst;
    }
    case TypedNodeKind_BoolLiteral: {
      u32 dst = bc_alloc_reg(fc);
      u32 c   = bc_add_const(fc, n->bool_lit.value ? 1 : 0);
      bc_emit(fc, BcOp_LoadConst, dst, c, 0);
      return dst;
    }
    case TypedNodeKind_NilLiteral: {
      // Always a nil POINTER, never a handle -- a zero register, matching the
      // `NULL` codegen.c emits.
      u32 dst = bc_alloc_reg(fc);
      bc_emit(fc, BcOp_LoadConst, dst, bc_add_const(fc, 0), 0);
      return dst;
    }
    case TypedNodeKind_StringLiteral: {
      return bc_compile_string_literal(fc, n->string_lit.value);
    }
    case TypedNodeKind_EnumAccess: {
      // `Name/Variant` -- a compile-time constant; see bc_enum_variant_value
      // for the auto-assignment algorithm. Neither lookup can fail for a
      // well-typed program: checker.c's EnumAccess case already validated
      // that both the enum and the variant name resolve.
      EnumEntry* ee = enum_table_lookup(fc->ck, n->enum_access.enum_name);
      xassert(ee);
      TypedNode* decl  = &fc->tast->nodes[ee->decl];
      i64        value = bc_enum_variant_value(fc->tast, decl, n->enum_access.variant_name);
      u32        dst    = bc_alloc_reg(fc);
      bc_emit(fc, BcOp_LoadConst, dst, bc_add_const(fc, value), 0);
      return dst;
    }
    case TypedNodeKind_Identifier: {
      BcLocal* loc = bc_local_try_lookup(fc, n->ident.name);
      if (loc) {
        if (!loc->is_addr_taken) return loc->reg;
        // Address-taken scalar local: `loc->reg` holds its ADDRESS, so a read
        // needs a real load through it.
        TypeRef ty  = fc->ck->resolved_types[idx];
        u32     dst = bc_alloc_reg(fc);
        bc_emit(fc, bc_load_op_for_type(n->token, ty), dst, loc->reg, 0);
        return dst;
      }
      u32 slot;
      if (fc->global_table && bc_fn_table_try_lookup(fc->global_table, n->ident.name, &slot)) {
        u32 dst = bc_alloc_reg(fc);
        bc_emit(fc, BcOp_LoadGlobal, dst, slot, 0);
        return dst;
      }
      // A top-level `fn` named as a VALUE rather than called. The checker
      // resolves that deliberately -- see its own Identifier case, which falls
      // back to fn_table_lookup and types the reference `Fn` -- so it is
      // well-typed 3b this backend can't compile rather than a checker bug:
      // there's no function-value representation in a register and no
      // indirect-call opcode. Script-author reachable, so a diagnostic, not
      // the assert below.
      if (fc->ck->resolved_types[idx].kind == TypeKind_Fn) {
        bc_unsupported(n->token, "a function used as a VALUE rather than called -- this bytecode "
                                   "compiler slice has no function-value representation and no "
                                   "indirect-call opcode");
      }
      xassert(!"bc_compile_expr: Identifier -- unresolved as either a local or a global -- "
               "the checker should already have caught this");
      return bc_alloc_reg(fc);
    }
    case TypedNodeKind_BinaryAdd: case TypedNodeKind_BinarySub:
    case TypedNodeKind_BinaryMul: case TypedNodeKind_BinaryDiv: case TypedNodeKind_BinaryMod:
    case TypedNodeKind_BinaryEq:  case TypedNodeKind_BinaryNeq:
    case TypedNodeKind_BinaryLt:  case TypedNodeKind_BinaryLe:
    case TypedNodeKind_BinaryGt:  case TypedNodeKind_BinaryGe: {
      TypeRef operand_ty = fc->ck->resolved_types[n->binary.lhs];
      u32     lhs        = bc_compile_expr(fc, n->binary.lhs);
      u32     rhs        = bc_compile_expr(fc, n->binary.rhs);
      u32     dst        = bc_alloc_reg(fc);
      // String and Named(struct) operands reach this switch only via a
      // comparison kind -- the checker never allows them for arithmetic -- so
      // branching on operand_ty.kind alone, with no separate is-this-a-
      // comparison check, can't misfire. `lhs`/`rhs` are already the
      // addresses to compare, so bc_compile_value_cmp needs no FieldAddr step.
      //
      // Named(ENUM) is excluded: an enum's value here is a genuine VALUE out
      // of bc_compile_expr, not an address, so it must fall through to the
      // scalar path below, which operates on lhs/rhs directly.
      if (operand_ty.kind == TypeKind_String || (operand_ty.kind == TypeKind_Named && !bc_type_is_enum(fc->ck, operand_ty))) {
        u32 cmp      = bc_compile_value_cmp(fc, n->token, operand_ty, lhs, rhs);
        u32 zero_reg = bc_alloc_reg(fc);
        bc_emit(fc, BcOp_LoadConst, zero_reg, bc_add_const(fc, 0), 0);
        bc_emit(fc, bc_int_op_for_kind(n->kind, false), dst, cmp, zero_reg); // cmp is a signed -1/0/1
        return dst;
      }
      BcOp op;
      if      (operand_ty.kind == TypeKind_F64) op = bc_f64_op_for_kind(n->kind);
      else if (operand_ty.kind == TypeKind_F32) op = bc_f32_op_for_kind(n->kind);
      else                                        op = bc_int_op_for_kind(n->kind, bc_type_needs_unsigned_ops(operand_ty));
      bc_emit(fc, op, dst, lhs, rhs);
      // A comparison kind yields bool, whose result type isn't a narrow
      // integer, so this only ever fires for the five arithmetic kinds.
      bc_narrow_arith_result(fc, fc->ck->resolved_types[idx], dst);
      return dst;
    }
    case TypedNodeKind_BinaryBitAnd: case TypedNodeKind_BinaryBitOr: case TypedNodeKind_BinaryBitXor:
    case TypedNodeKind_BinaryShl:    case TypedNodeKind_BinaryShr: {
      // Always a plain integer op, with no operand-type dispatch (unlike
      // BinaryAdd above). See BcOp_BitAnd in bytecode.h for why a stray
      // float operand, which the checker permits, needs no guard here.
      TypeRef shifted_ty = fc->ck->resolved_types[n->binary.lhs];
      u32 lhs = bc_compile_expr(fc, n->binary.lhs);
      u32 rhs = bc_compile_expr(fc, n->binary.rhs);
      u32 dst = bc_alloc_reg(fc);
      bc_emit(fc, bc_bitwise_op_for_kind(n->kind, shifted_ty.kind), dst, lhs, rhs);
      // Only Shl can push bits past the type's width -- see
      // bc_narrow_arith_result for why the other four need nothing.
      if (n->kind == TypedNodeKind_BinaryShl) bc_narrow_arith_result(fc, fc->ck->resolved_types[idx], dst);
      return dst;
    }
    case TypedNodeKind_UnaryPos: {
      return bc_compile_expr(fc, n->unary.expr); // true no-op, same as codegen.c's own case
    }
    case TypedNodeKind_UnaryNeg: {
      // `-x` compiled as `0 - x`, reusing BinarySub's op-family dispatch --
      // the bc_*_op_for_kind tables are keyed by TypedNodeKind, so passing
      // the literal TypedNodeKind_BinarySub (not n->kind, which is UnaryNeg)
      // reaches the right ops. Zero's all-zero-bits pattern is a correct LHS
      // for int, f32 and f64 alike.
      TypeRef ty       = fc->ck->resolved_types[n->unary.expr];
      u32     x        = bc_compile_expr(fc, n->unary.expr);
      u32     zero_reg = bc_alloc_reg(fc);
      bc_emit(fc, BcOp_LoadConst, zero_reg, bc_add_const(fc, 0), 0);
      BcOp op;
      if      (ty.kind == TypeKind_F64) op = bc_f64_op_for_kind(TypedNodeKind_BinarySub);
      else if (ty.kind == TypeKind_F32) op = bc_f32_op_for_kind(TypedNodeKind_BinarySub);
      else                                 op = bc_int_op_for_kind(TypedNodeKind_BinarySub, false); // Sub has no unsigned half
      u32 dst = bc_alloc_reg(fc);
      bc_emit(fc, op, dst, zero_reg, x);
      bc_narrow_arith_result(fc, fc->ck->resolved_types[idx], dst); // `-x` overflows like any subtraction
      return dst;
    }
    case TypedNodeKind_UnaryBitNot: {
      // `~x` preserves the operand's type, and on a narrow one it flips the
      // extension bits above that width too -- `(bit-not 0u8)` is 255, not
      // -1 -- so the result goes back through the width fixup.
      u32 x   = bc_compile_expr(fc, n->unary.expr);
      u32 dst = bc_alloc_reg(fc);
      bc_emit(fc, BcOp_BitNot, dst, x, 0);
      bc_narrow_arith_result(fc, fc->ck->resolved_types[idx], dst);
      return dst;
    }
    case TypedNodeKind_BinaryCast: {
      // `(cast Type value)`. The checker allows numeric<->numeric,
      // pointer<->pointer/any and enum<->numeric -- never structs, strings,
      // arrays, arenas or handles -- plus `void` as an always-legal target,
      // the `(void expr)` discard idiom.
      //
      // `n->binary.lhs` is an Identifier node holding the TYPE NAME, not a
      // value to compile; the target type comes from
      // `fc->ck->resolved_types[idx]` instead.
      TypeRef dst_ty = fc->ck->resolved_types[idx];
      TypeRef src_ty = fc->ck->resolved_types[n->binary.rhs];
      u32     val    = bc_compile_expr(fc, n->binary.rhs);

      if (dst_ty.kind == TypeKind_Void) {
        // `val`'s side effects already ran above, and the checker enforces
        // that a void context never consumes a value, so hand the register
        // back unchanged -- the dead-register convention WhileExpr uses.
        return val;
      }
      if (type_ref_equal(src_ty, dst_ty)) {
        return val; // a same-type cast is always a no-op
      }

      b32 src_float = src_ty.kind == TypeKind_F32 || src_ty.kind == TypeKind_F64;
      b32 dst_float = dst_ty.kind == TypeKind_F32 || dst_ty.kind == TypeKind_F64;

      if (!src_float && !dst_float) {
        // int-like <-> int-like, bool/char/pointer/any/enum included: a WIDTH
        // adjustment on the same representation, never a numeric conversion.
        if (dst_ty.kind == TypeKind_Bool) {
          // `(bool)x` is C's `!= 0` truthiness rule, not a bit copy.
          u32 zero_reg = bc_alloc_reg(fc);
          bc_emit(fc, BcOp_LoadConst, zero_reg, bc_add_const(fc, 0), 0);
          u32 dst = bc_alloc_reg(fc);
          bc_emit(fc, BcOp_Neq, dst, val, zero_reg);
          return dst;
        }
        return bc_compile_int_narrow(fc, dst_ty.kind, val);
      }
      if (src_float && dst_float) {
        u32 dst = bc_alloc_reg(fc);
        bc_emit(fc, (dst_ty.kind == TypeKind_F64) ? BcOp_F32ToF64 : BcOp_F64ToF32, dst, val, 0);
        return dst;
      }
      if (dst_float) {
        // int-like -> float: a real numeric conversion. See BcOp_IntToF64 for
        // the unsigned-source caveat.
        u32 dst = bc_alloc_reg(fc);
        bc_emit(fc, (dst_ty.kind == TypeKind_F64) ? BcOp_IntToF64 : BcOp_IntToF32, dst, val, 0);
        return dst;
      }
      // float -> int-like: convert, then narrow if the target is narrower than
      // i64, or apply the truthiness rule if it's Bool.
      u32 converted = bc_alloc_reg(fc);
      bc_emit(fc, (src_ty.kind == TypeKind_F64) ? BcOp_F64ToInt : BcOp_F32ToInt, converted, val, 0);
      if (dst_ty.kind == TypeKind_Bool) {
        u32 zero_reg = bc_alloc_reg(fc);
        bc_emit(fc, BcOp_LoadConst, zero_reg, bc_add_const(fc, 0), 0);
        u32 dst = bc_alloc_reg(fc);
        bc_emit(fc, BcOp_Neq, dst, converted, zero_reg);
        return dst;
      }
      return bc_compile_int_narrow(fc, dst_ty.kind, converted);
    }
    case TypedNodeKind_BinaryReinterpret: {
      // `(reinterpret Type value)` -- never a numeric conversion, unlike
      // BinaryCast: the checker requires src and dst to be the same byte
      // width, and every scalar already lives in an i64 register as its raw
      // bit pattern, so there's no instruction to emit for the reinterpret
      // itself.
      //
      // bc_compile_int_narrow handles the one wrinkle: a narrower INTEGER
      // target needs its extension normalized into the register's upper bits
      // -- reinterpreting an f32's zero-extended pattern as an i32 must
      // sign-extend those upper 32 bits, or later ops on the register
      // misbehave. It's a no-op for every other target kind, so calling it
      // unconditionally is safe.
      TypeRef dst_ty = fc->ck->resolved_types[idx];
      u32     val    = bc_compile_expr(fc, n->binary.rhs);
      return bc_compile_int_narrow(fc, dst_ty.kind, val);
    }
    case TypedNodeKind_LogicalNot: {
      // `!x` -- Eq-against-zero is the negation of bc_compile_truthy's
      // `!= 0` test directly, one op fewer than composing through it.
      u32 x        = bc_compile_expr(fc, n->unary.expr);
      u32 zero_reg = bc_alloc_reg(fc);
      bc_emit(fc, BcOp_LoadConst, zero_reg, bc_add_const(fc, 0), 0);
      u32 dst = bc_alloc_reg(fc);
      bc_emit(fc, BcOp_Eq, dst, x, zero_reg);
      return dst;
    }
    case TypedNodeKind_LogicalAnd: {
      // Real short-circuit, matching C's `&&`: the rhs is never compiled when
      // the lhs is falsy. IfExpr's jump-backpatch shape, no new opcodes.
      u32 dst = bc_alloc_reg(fc);
      u32 lhs = bc_compile_expr(fc, n->binary.lhs);
      u32 jf  = bc_emit(fc, BcOp_JumpIfFalse, lhs, 0, 0); // patched below -- lhs falsy, short-circuit
      u32 rhs_truthy = bc_compile_truthy(fc, n->binary.rhs);
      bc_emit(fc, BcOp_Move, dst, rhs_truthy, 0);
      u32 jend = bc_emit(fc, BcOp_Jump, 0, 0, 0); // patched below
      fc->code[jf].b = (u32)dyn_count(fc->code); // lhs falsy -- result is 0, rhs never compiled
      u32 zero_reg = bc_alloc_reg(fc);
      bc_emit(fc, BcOp_LoadConst, zero_reg, bc_add_const(fc, 0), 0);
      bc_emit(fc, BcOp_Move, dst, zero_reg, 0);
      fc->code[jend].a = (u32)dyn_count(fc->code);
      return dst;
    }
    case TypedNodeKind_LogicalOr: {
      // Mirror image of `and`: short-circuits to 1 as soon as the lhs is
      // truthy, leaving the rhs uncompiled.
      u32 dst = bc_alloc_reg(fc);
      u32 lhs = bc_compile_expr(fc, n->binary.lhs);
      u32 jf  = bc_emit(fc, BcOp_JumpIfFalse, lhs, 0, 0); // patched below -- lhs falsy, fall through to rhs
      u32 one_reg = bc_alloc_reg(fc);
      bc_emit(fc, BcOp_LoadConst, one_reg, bc_add_const(fc, 1), 0);
      bc_emit(fc, BcOp_Move, dst, one_reg, 0);
      u32 jend = bc_emit(fc, BcOp_Jump, 0, 0, 0); // patched below
      fc->code[jf].b = (u32)dyn_count(fc->code); // lhs falsy -- result is rhs's own truthiness
      u32 rhs_truthy = bc_compile_truthy(fc, n->binary.rhs);
      bc_emit(fc, BcOp_Move, dst, rhs_truthy, 0);
      fc->code[jend].a = (u32)dyn_count(fc->code);
      return dst;
    }
    case TypedNodeKind_StringLenExpr: {
      // `(string-len s)` is `(s).size`. String8 is `{u8* str; u64 size;}`, so
      // `.size` is at offset 8: a plain LoadFieldI64 off `s`'s address.
      u32 s   = bc_compile_expr(fc, n->unary.expr);
      u32 dst = bc_alloc_reg(fc);
      bc_emit(fc, BcOp_LoadFieldI64, dst, s, 8);
      return dst;
    }
    case TypedNodeKind_CstrExpr: {
      // `(cstring s)` is `(char*)(s).str`, and `.str` is at offset 0. No
      // string-literal special case like codegen.c's (which emits a raw C
      // string constant rather than a str8_lit call): a literal's header is
      // already boxed at compile time here, so reading `.str` off it is
      // uniformly correct and just as cheap.
      u32 s   = bc_compile_expr(fc, n->unary.expr);
      u32 dst = bc_alloc_reg(fc);
      bc_emit(fc, BcOp_LoadFieldI64, dst, s, 0);
      return dst;
    }
    case TypedNodeKind_SizeofExpr: {
      // A compile-time constant; layout_of computes what C's `sizeof` would.
      // `result_type` -- the optional `(sizeof T Type)` override of the
      // default u64 result -- doesn't change which bits get loaded, since
      // every register here is i64-shaped whatever the declared width.
      Layout layout = layout_of(fc->layout_cache, fc->ck, n->type_query.type);
      u32     dst    = bc_alloc_reg(fc);
      bc_emit(fc, BcOp_LoadConst, dst, bc_add_const(fc, (i64)layout.size), 0);
      return dst;
    }
    case TypedNodeKind_AlignofExpr: {
      Layout layout = layout_of(fc->layout_cache, fc->ck, n->type_query.type);
      u32     dst    = bc_alloc_reg(fc);
      bc_emit(fc, BcOp_LoadConst, dst, bc_add_const(fc, (i64)layout.align), 0);
      return dst;
    }
    case TypedNodeKind_MemberOffsetExpr: {
      // `(member-offset StructName field)` -- a compile-time constant via
      // layout_field_offset, the helper every struct field access here uses.
      // The checker already validated that the type resolves to a real struct
      // and the field exists on it, so neither lookup below can fail.
      StructEntry* se = struct_table_lookup(fc->ck, n->member_offset.type.name);
      xassert(se);
      FieldLayout fl = layout_field_offset(fc->layout_cache, fc->ck, se, n->member_offset.field);
      xassert(fl.found);
      u32 dst = bc_alloc_reg(fc);
      bc_emit(fc, BcOp_LoadConst, dst, bc_add_const(fc, (i64)fl.offset), 0);
      return dst;
    }
    case TypedNodeKind_ZeroExpr: {
      return bc_compile_zero_value(fc, n->type_query.type);
    }
    case TypedNodeKind_IfExpr: {
      // `dst` is allocated even for a no-else (`when`-lowered) if, where it
      // goes unwritten when the condition is false. A valid program never
      // reads it in that case: codegen.c's C ternary has `(void)0` as its
      // false arm, so a context needing a real value would be a type error.
      // Everything compiles for a value here rather than splitting out a
      // for-effect path, at the cost of an occasional dead register.
      u32 dst      = bc_alloc_reg(fc);
      u32 cond_reg = bc_compile_expr(fc, n->if_expr.cond);
      u32 jf       = bc_emit(fc, BcOp_JumpIfFalse, cond_reg, 0, 0); // patched below
      u32 then_reg = bc_compile_expr(fc, n->if_expr.then_branch);
      bc_emit(fc, BcOp_Move, dst, then_reg, 0);
      u32 jend = bc_emit(fc, BcOp_Jump, 0, 0, 0); // patched below
      fc->code[jf].b = (u32)dyn_count(fc->code); // else branch (or "past the if" with no else)
      if (n->if_expr.else_branch != TYPED_NIL) {
        u32 else_reg = bc_compile_expr(fc, n->if_expr.else_branch);
        bc_emit(fc, BcOp_Move, dst, else_reg, 0);
      }
      fc->code[jend].a = (u32)dyn_count(fc->code);
      return dst;
    }
    case TypedNodeKind_BreakExpr:
    case TypedNodeKind_ContinueExpr: {
      // An unconditional jump whose target the enclosing loop patches in (see
      // BcLoopCtx). The checker has already rejected these outside a loop, so
      // there is always one to record the fixup against. Dead register out,
      // WhileExpr's convention -- control leaves, nothing reads it.
      //
      // Any `scratch` opened inside the loop body is rewound BEFORE the jump,
      // down to the loop's own mark -- one wrapping the whole loop is still
      // live where this lands.
      bc_unwind_scratch_scopes(fc, fc->loop_scratch_mark);
      u32 jmp = bc_emit(fc, BcOp_Jump, 0, 0, 0);
      if (n->kind == TypedNodeKind_BreakExpr) dyn_push(fc->arena, fc->break_fixups, jmp);
      else                                    dyn_push(fc->arena, fc->continue_fixups, jmp);
      return bc_alloc_reg(fc);
    }
    case TypedNodeKind_WhileExpr: {
      // `(while cond body...)` -- always void, so it returns a dead register,
      // ReturnExpr's convention. The one new piece versus IfExpr's
      // forward-only jumps is the BACKWARD jump to the condition check, whose
      // target is already known at emit time and needs no backpatching.
      //
      // `continue` targets loop_start too: re-testing the condition IS this
      // loop's next iteration, there being no separate step.
      BcLoopCtx loop = bc_loop_begin(fc);
      u32 loop_start = (u32)dyn_count(fc->code);
      u32 cond_reg    = bc_compile_expr(fc, n->while_expr.cond);
      u32 jf          = bc_emit(fc, BcOp_JumpIfFalse, cond_reg, 0, 0); // patched below
      bc_compile_block(fc, n->while_expr.body); // for effect -- the checker requires a Void body
      bc_loop_patch(fc, &fc->continue_fixups, loop.continue_mark, loop_start);
      bc_emit(fc, BcOp_Jump, loop_start, 0, 0);
      fc->code[jf].b = (u32)dyn_count(fc->code);
      bc_loop_patch(fc, &fc->break_fixups, loop.break_mark, (u32)dyn_count(fc->code));
      bc_loop_end(fc, loop);
      return bc_alloc_reg(fc);
    }
    case TypedNodeKind_ForCExpr: {
      // (for [name init cond expr] body ...)
      u64 locals_mark = dyn_count(fc->locals);
      u32 var_reg     = bc_compile_expr(fc, n->for_c.init);
      bc_bind_local(fc, n->for_c.var_name, var_reg);
      TypeRef var_ty   = fc->ck->resolved_types[n->for_c.init];

      BcLoopCtx loop       = bc_loop_begin(fc);
      u32       loop_start = (u32)dyn_count(fc->code);
      u32       cond_reg   = bc_compile_expr(fc, n->for_c.cond);

      u32 jf = bc_emit(fc, BcOp_JumpIfFalse, cond_reg, 0, 0); // patched below
      bc_compile_block(fc, n->for_c.body); // for effect -- the checker requires a Void body
      bc_compile_expr(fc, n->for_c.expr); // compile expression after body
      bc_loop_patch(fc, &fc->continue_fixups, loop.continue_mark, loop_start);
      bc_emit(fc, BcOp_Jump, loop_start, 0, 0);
      fc->code[jf].b = (u32)dyn_count(fc->code);
      bc_loop_patch(fc, &fc->break_fixups, loop.break_mark, (u32)dyn_count(fc->code));
      bc_loop_end(fc, loop);
      if (fc->locals) dyn_hdr(fc->locals)->count = locals_mark; // pop the loop var
      return bc_alloc_reg(fc); // void, same convention as WhileExpr above
    }
    case TypedNodeKind_ForRangeExpr: {
      // `(for [name begin end (step)] body...)`. begin/end/step share
      // BinaryAdd's int-vs-f64-vs-f32 operand dispatch, since a float-typed
      // range is valid 3b.
      //
      // `end` and `step` are emitted INSIDE the loop body's instruction
      // range, so the backward jump re-executes them every iteration -- which
      // is exactly C's `for (i = begin; i < end; i += step)` semantics, where
      // both are ordinary expressions re-evaluated each time, side effects
      // included, not hoisted. It costs nothing at compile time:
      // bc_compile_expr still runs once per expression.
      u64     locals_mark = dyn_count(fc->locals); // popped after the loop -- the loop var is bound
                                                        // outside any Block node, so bc_compile_block's
                                                        // own truncation never covers it
      u32     var_reg = bc_compile_expr(fc, n->for_range.begin);
      bc_bind_local(fc, n->for_range.var_name, var_reg); // begin/end/step can't reference var_name:
                                                             // the checker resolves them in the outer
                                                             // scope, before var_name exists
      TypeRef var_ty = fc->ck->resolved_types[n->for_range.begin];
      BcOp    add_op, lt_op;
      if      (var_ty.kind == TypeKind_F64) { add_op = BcOp_FAdd;   lt_op = BcOp_FLt; }
      else if (var_ty.kind == TypeKind_F32) { add_op = BcOp_F32Add; lt_op = BcOp_F32Lt; }
      // The bound test reads the counter's signedness like any other `<`, so
      // a u64 counter past 2^63 doesn't compare as a negative number and end
      // the loop before it starts (see BcOp_DivU).
      else { add_op = BcOp_Add; lt_op = bc_type_needs_unsigned_ops(var_ty) ? BcOp_LtU : BcOp_Lt; }

      BcLoopCtx loop = bc_loop_begin(fc);
      u32 loop_start = (u32)dyn_count(fc->code);
      u32 end_reg     = bc_compile_expr(fc, n->for_range.end);
      u32 cond_reg    = bc_alloc_reg(fc);
      bc_emit(fc, lt_op, cond_reg, var_reg, end_reg);
      u32 jf = bc_emit(fc, BcOp_JumpIfFalse, cond_reg, 0, 0); // patched below
      bc_compile_block(fc, n->for_range.body); // for effect
      // `continue` lands HERE, on the step -- not on loop_start, which would
      // re-test the same counter value and never terminate.
      bc_loop_patch(fc, &fc->continue_fixups, loop.continue_mark, (u32)dyn_count(fc->code));
      u32 step_reg = bc_compile_expr(fc, n->for_range.step);
      bc_emit(fc, add_op, var_reg, var_reg, step_reg); // in-place accumulate: dst == lhs is fine,
                                                            // operands are read before the write
      bc_narrow_arith_result(fc, var_ty, var_reg); // the counter wraps at its own width like any
                                                        // other `+`; BcOp_Narrow is in-place too
      bc_emit(fc, BcOp_Jump, loop_start, 0, 0);
      fc->code[jf].b = (u32)dyn_count(fc->code);
      bc_loop_patch(fc, &fc->break_fixups, loop.break_mark, (u32)dyn_count(fc->code));
      bc_loop_end(fc, loop);
      if (fc->locals) dyn_hdr(fc->locals)->count = locals_mark; // pop the loop var
      return bc_alloc_reg(fc); // void, same convention as WhileExpr above
    }
    case TypedNodeKind_ForEachExpr: {
      // `[item coll]`/`[[i item] coll]` over Array/Vector, or `[[k v] m]`/
      // `[x s]`/`[[i x] s]` over Map/Set -- the Map/Set branch is just below,
      // everything else falls through to the Array/Vector logic.
      //
      // An Array's element count is known from its type; a Vector's needs
      // BcOp_DynCount to read the hidden DynHdr. Either way the collection's
      // compiled value is already the right base address: an Array's value is
      // its own address (matching C's array-to-pointer decay), and a Vector's
      // is already a bare T*.
      TypeRef coll_ty = fc->ck->resolved_types[n->for_each.collection];
      if (coll_ty.kind == TypeKind_Map || coll_ty.kind == TypeKind_Set) {
        // Walk the slot array directly, skipping empty and tombstone slots,
        // mirroring codegen.c's cg_foreach_expr. For a Set, elem_name is
        // slot.key and index_name (if given) is the raw slot POSITION; for a
        // Map, index_name is slot.key and elem_name is slot.value -- the
        // checker requires the two-binding `[[k v] m]` form for a Map, since a
        // key/value pair has no single natural element.
        //
        // The instance's compiled value is its address, and `slots`/`capacity`
        // are its first two 8-byte fields (bcmap.h's BcHashInstance), so the
        // generic field-load op reads them with no new opcode.
        b32     is_map = coll_ty.kind == TypeKind_Map;
        TypeRef key_ty  = is_map ? *coll_ty.map_key : *coll_ty.pointee;

        BcHashSlotLayout* layout = push_one(fc->arena, BcHashSlotLayout);
        TypeRef value_ty = {0};
        if (is_map) {
          value_ty = *coll_ty.pointee;
          *layout   = bc_hash_slot_layout(fc->layout_cache, fc->ck, key_ty, &value_ty);
        } else {
          *layout = bc_hash_slot_layout(fc->layout_cache, fc->ck, key_ty, NULL);
        }

        u64 locals_mark = dyn_count(fc->locals); // see ForRangeExpr's own comment
        u32 coll_reg     = bc_compile_expr(fc, n->for_each.collection);
        u32 slots_reg    = bc_alloc_reg(fc);
        bc_emit(fc, BcOp_LoadFieldI64, slots_reg, coll_reg, 0); // BcHashInstance.slots, offset 0
        u32 capacity_reg = bc_alloc_reg(fc);
        bc_emit(fc, BcOp_LoadFieldI64, capacity_reg, coll_reg, 8); // .capacity, offset 8

        u32 idx_reg = bc_alloc_reg(fc);
        bc_emit(fc, BcOp_LoadConst, idx_reg, bc_add_const(fc, 0), 0);

        BcLoopCtx loop = bc_loop_begin(fc);
        u32 loop_start = (u32)dyn_count(fc->code);
        u32 cond_reg    = bc_alloc_reg(fc);
        bc_emit(fc, BcOp_Lt, cond_reg, idx_reg, capacity_reg);
        u32 jf_done = bc_emit(fc, BcOp_JumpIfFalse, cond_reg, 0, 0); // patched below -- exits the whole loop

        u32 stride_reg = bc_alloc_reg(fc);
        bc_emit(fc, BcOp_LoadConst, stride_reg, bc_add_const(fc, (i64)layout->slot_size), 0);
        u32 byte_off_reg = bc_alloc_reg(fc);
        bc_emit(fc, BcOp_Mul, byte_off_reg, idx_reg, stride_reg);
        u32 slot_addr_reg = bc_alloc_reg(fc);
        bc_emit(fc, BcOp_Add, slot_addr_reg, slots_reg, byte_off_reg);

        u32 state_reg = bc_alloc_reg(fc);
        bc_emit(fc, BcOp_LoadFieldI32, state_reg, slot_addr_reg, (u32)layout->state_offset);
        u32 occupied_reg = bc_alloc_reg(fc);
        bc_emit(fc, BcOp_LoadConst, occupied_reg, bc_add_const(fc, (i64)BcHashSlotState_Occupied), 0);
        u32 is_occupied_reg = bc_alloc_reg(fc);
        bc_emit(fc, BcOp_Eq, is_occupied_reg, state_reg, occupied_reg);
        u32 jf_skip = bc_emit(fc, BcOp_JumpIfFalse, is_occupied_reg, 0, 0); // patched below -- skip
                                                                                // to the increment

        if (n->for_each.has_index) {
          u32 index_val_reg;
          if (is_map) {
            index_val_reg = slot_addr_reg; // the key sits at slot offset 0
            if (!bc_field_is_embedded(fc->ck, key_ty)) {
              index_val_reg = bc_alloc_reg(fc);
              bc_emit(fc, bc_load_op_for_type(n->token, key_ty), index_val_reg, slot_addr_reg, 0);
            }
          } else {
            index_val_reg = idx_reg; // Set: the raw slot POSITION, not the key
          }
          bc_bind_local(fc, n->for_each.index_name, index_val_reg);
        }

        u32 elem_reg;
        if (is_map) {
          elem_reg = bc_alloc_reg(fc);
          if (!bc_field_is_embedded(fc->ck, value_ty)) {
            bc_emit(fc, bc_load_op_for_type(n->token, value_ty), elem_reg, slot_addr_reg, (u32)layout->value_offset);
          } else {
            bc_emit(fc, BcOp_FieldAddr, elem_reg, slot_addr_reg, (u32)layout->value_offset);
          }
        } else {
          elem_reg = slot_addr_reg; // a Set's element IS its key, at slot offset 0
          if (!bc_field_is_embedded(fc->ck, key_ty)) {
            elem_reg = bc_alloc_reg(fc);
            bc_emit(fc, bc_load_op_for_type(n->token, key_ty), elem_reg, slot_addr_reg, 0);
          }
        }
        bc_bind_local(fc, n->for_each.elem_name, elem_reg);

        bc_compile_block(fc, n->for_each.body); // for effect

        // `continue` shares the skip-an-empty-slot landing site: both mean
        // "this slot is done, move on", so both want the index bump next.
        fc->code[jf_skip].b = (u32)dyn_count(fc->code);
        bc_loop_patch(fc, &fc->continue_fixups, loop.continue_mark, (u32)dyn_count(fc->code));
        u32 one_reg = bc_alloc_reg(fc);
        bc_emit(fc, BcOp_LoadConst, one_reg, bc_add_const(fc, 1), 0);
        bc_emit(fc, BcOp_Add, idx_reg, idx_reg, one_reg);
        bc_emit(fc, BcOp_Jump, loop_start, 0, 0);
        fc->code[jf_done].b = (u32)dyn_count(fc->code);
        bc_loop_patch(fc, &fc->break_fixups, loop.break_mark, (u32)dyn_count(fc->code));
        bc_loop_end(fc, loop);
        if (fc->locals) dyn_hdr(fc->locals)->count = locals_mark; // pop index/elem names
        return bc_alloc_reg(fc); // void, as the Array/Vector case below
      }
      // Array/Vector are all that's left: Map/Set returned just above, and the
      // checker rejects every other collection type outright ("`for` can't
      // iterate ..."), so this is an internal invariant rather than a
      // script-author-facing gap -- see the bc_unsupported note above.
      xassert((coll_ty.kind == TypeKind_Array || coll_ty.kind == TypeKind_Vector)
              && "bc_compile_expr: collection `for` over a type the checker should already have "
                 "rejected");
      TypeRef elem_ty = *coll_ty.pointee;
      u64     locals_mark = dyn_count(fc->locals); // popped after the loop -- the index/elem names are
                                                        // bound outside a Block, as ForRangeExpr's
                                                        // loop var is
      u32     coll_reg = bc_compile_expr(fc, n->for_each.collection);

      u32 idx_reg = bc_alloc_reg(fc);
      bc_emit(fc, BcOp_LoadConst, idx_reg, bc_add_const(fc, 0), 0);
      if (n->for_each.has_index) bc_bind_local(fc, n->for_each.index_name, idx_reg);

      u32 count_reg = bc_alloc_reg(fc);
      if (coll_ty.kind == TypeKind_Array) {
        bc_emit(fc, BcOp_LoadConst, count_reg, bc_add_const(fc, (i64)coll_ty.count), 0);
      } else {
        bc_emit(fc, BcOp_DynCount, count_reg, coll_reg, 0);
      }

      BcLoopCtx loop = bc_loop_begin(fc);
      u32 loop_start = (u32)dyn_count(fc->code);
      u32 cond_reg    = bc_alloc_reg(fc);
      bc_emit(fc, BcOp_Lt, cond_reg, idx_reg, count_reg);
      u32 jf = bc_emit(fc, BcOp_JumpIfFalse, cond_reg, 0, 0); // patched below

      u64 stride     = bc_element_stride(fc, elem_ty);
      u32 stride_reg = bc_alloc_reg(fc);
      bc_emit(fc, BcOp_LoadConst, stride_reg, bc_add_const(fc, (i64)stride), 0);
      u32 byte_off_reg = bc_alloc_reg(fc);
      bc_emit(fc, BcOp_Mul, byte_off_reg, idx_reg, stride_reg);
      u32 elem_addr_reg = bc_alloc_reg(fc);
      bc_emit(fc, BcOp_Add, elem_addr_reg, coll_reg, byte_off_reg);
      u32 elem_reg = elem_addr_reg;
      if (!bc_field_is_embedded(fc->ck, elem_ty)) {
        elem_reg = bc_alloc_reg(fc);
        bc_emit(fc, bc_load_op_for_type(n->token, elem_ty), elem_reg, elem_addr_reg, 0);
      }
      bc_bind_local(fc, n->for_each.elem_name, elem_reg);

      bc_compile_block(fc, n->for_each.body); // for effect

      bc_loop_patch(fc, &fc->continue_fixups, loop.continue_mark, (u32)dyn_count(fc->code));
      u32 one_reg = bc_alloc_reg(fc);
      bc_emit(fc, BcOp_LoadConst, one_reg, bc_add_const(fc, 1), 0);
      bc_emit(fc, BcOp_Add, idx_reg, idx_reg, one_reg); // in-place accumulate, as in ForRangeExpr
      bc_emit(fc, BcOp_Jump, loop_start, 0, 0);
      fc->code[jf].b = (u32)dyn_count(fc->code);
      bc_loop_patch(fc, &fc->break_fixups, loop.break_mark, (u32)dyn_count(fc->code));
      bc_loop_end(fc, loop);
      if (fc->locals) dyn_hdr(fc->locals)->count = locals_mark; // pop index/elem names
      return bc_alloc_reg(fc); // void, as WhileExpr/ForRangeExpr
    }
    case TypedNodeKind_LetExpr: {
      u64 locals_mark = dyn_count(fc->locals); // a `let`'s own bindings need the same popping
                                                    // bc_compile_block does, and can't rely on the
                                                    // body's truncation to cover them: an `if` branch
                                                    // that's a bare LetExpr compiles through
                                                    // bc_compile_expr, never reaching
                                                    // bc_compile_block at this level
      foreach_index(i, n->let_expr.binding_count) {
        Binding* b   = &fc->tast->bindings[n->let_expr.binding_first + i];
        u32      reg = bc_compile_expr(fc, b->init);
        // A binding copies arrays too, unlike a by-value call argument:
        // codegen.c emits an explicit memcpy at a `let`/`var`/`val`, so this
        // uses bc_field_is_embedded's full set rather than the narrower
        // call-site check.
        TypeRef init_ty = fc->ck->resolved_types[b->init];
        if (bc_field_is_embedded(fc->ck, init_ty)) reg = bc_compile_value_copy(fc, init_ty, reg);
        bc_bind_local_typed(fc, n->token, b->name, init_ty, reg);
      }
      u32 result = bc_compile_block(fc, n->let_expr.body);
      if (fc->locals) dyn_hdr(fc->locals)->count = locals_mark; // pop the `let`'s own bindings; the
                                                                      // body's were already popped by
                                                                      // bc_compile_block
      return result;
    }
    case TypedNodeKind_ParallelExpr: {
      // The one-lane serial fallback (see LANES at the top of this file):
      // fork/join over one lane is just "evaluate the captures, run the body
      // once". Structurally identical to LetExpr above -- captures ARE
      // bindings, out of the same TypedAst.bindings array -- because that is
      // exactly what the native backend does too, only into a capture struct
      // handed to a trampoline instead of into local registers.
      //
      // Nothing here hides the enclosing function's locals the way
      // cg_parallel_expr swaps `cg->scope` to NULL. It does not need to: the
      // checker resolves the body against the captures and globals alone
      // (Scope.hidden_lo), so a body that reached here cannot name one.
      u64 locals_mark = dyn_count(fc->locals);
      foreach_index(i, n->parallel_expr.capture_count) {
        Binding* b   = &fc->tast->bindings[n->parallel_expr.capture_first + i];
        u32      reg = bc_compile_expr(fc, b->init);
        // Captures are copied by value into every lane natively; an embedded
        // type needs the same real copy here, for the same reason LetExpr does.
        TypeRef init_ty = fc->ck->resolved_types[b->init];
        if (bc_field_is_embedded(fc->ck, init_ty)) reg = bc_compile_value_copy(fc, init_ty, reg);
        bc_bind_local_typed(fc, n->token, b->name, init_ty, reg);
      }
      bc_compile_block(fc, n->parallel_expr.body); // for effect; checker requires Void
      if (fc->locals) dyn_hdr(fc->locals)->count = locals_mark; // pop the captures
      return bc_alloc_reg(fc); // void, same convention as WhileExpr
    }
    case TypedNodeKind_ParallelForExpr: {
      // `(parallel-for [i count])` partitions `[0, count)` across lanes with
      // lane_range. With one lane that partition is the whole range, so this
      // is an ascending `for i = 0; i < count; i += 1` -- ForRangeExpr's shape
      // with a synthesized begin and step, and no float case to dispatch on:
      // lane_range's argument is a work COUNT, so the checker has already
      // required an integer.
      //
      // Unlike ForRangeExpr, `count` is emitted BEFORE loop_start, so the
      // backward jump does not re-run it. That is not an optimization: it is
      // the semantics. Natively `count` is an argument to a single
      // `lane_range(...)` call evaluated once outside the loop, whereas a
      // range `for`'s `end` is an ordinary C loop condition re-evaluated every
      // iteration. Emitting this one inside the loop would make a `count` with
      // side effects run N times here and once there.
      u64     locals_mark = dyn_count(fc->locals); // the loop var is bound outside any Block node
      TypeRef var_ty      = fc->ck->resolved_types[n->parallel_for.count];
      u32     count_reg   = bc_compile_expr(fc, n->parallel_for.count);
      u32     var_reg     = bc_alloc_reg(fc);
      bc_emit(fc, BcOp_LoadConst, var_reg, bc_add_const(fc, 0), 0);
      bc_bind_local(fc, n->parallel_for.var_name, var_reg); // after `count`, which the checker
                                                                 // resolves in the OUTER scope
      BcOp lt_op = bc_type_needs_unsigned_ops(var_ty) ? BcOp_LtU : BcOp_Lt;

      BcLoopCtx loop = bc_loop_begin(fc);
      u32 loop_start = (u32)dyn_count(fc->code);
      u32 cond_reg   = bc_alloc_reg(fc);
      bc_emit(fc, lt_op, cond_reg, var_reg, count_reg);
      u32 jf = bc_emit(fc, BcOp_JumpIfFalse, cond_reg, 0, 0); // patched below
      bc_compile_block(fc, n->parallel_for.body); // for effect
      bc_loop_patch(fc, &fc->continue_fixups, loop.continue_mark, (u32)dyn_count(fc->code));
      u32 one_reg = bc_alloc_reg(fc);
      bc_emit(fc, BcOp_LoadConst, one_reg, bc_add_const(fc, 1), 0);
      bc_emit(fc, BcOp_Add, var_reg, var_reg, one_reg);
      bc_narrow_arith_result(fc, var_ty, var_reg);
      bc_emit(fc, BcOp_Jump, loop_start, 0, 0);
      fc->code[jf].b = (u32)dyn_count(fc->code);
      bc_loop_patch(fc, &fc->break_fixups, loop.break_mark, (u32)dyn_count(fc->code));
      bc_loop_end(fc, loop);
      if (fc->locals) dyn_hdr(fc->locals)->count = locals_mark; // pop the loop var
      return bc_alloc_reg(fc); // void, same convention as WhileExpr
    }
    case TypedNodeKind_ScratchExpr: {
      // `(scratch [t] body...)` binds `t` to a handle on the shared
      // per-thread scratch arena, marks it, runs the body, then rewinds --
      // arena_temp_begin/arena_temp_end's shape, mirroring cg_scratch_expr.
      //
      // If the body's VALUE was itself allocated out of this scratch arena,
      // using it after the block is a use-after-free. That's inherent to
      // `scratch` -- anything worth keeping must be copied out first -- and
      // codegen.c's `_3b_scratch_result` has the same property.
      // A `return` inside the body would jump straight past the ArenaPop
      // below, leaving the scratch arena at its high-water mark, so the scope
      // is pushed on fc->scratch_scopes for bc_unwind_scratch_scopes to replay
      // ahead of the BcOp_Return. cg_scratch_expr does the same thing with
      // inline bbb_arena_temp_end calls.
      u64 locals_mark = dyn_count(fc->locals); // the arena's name is bound outside a Block, so it
                                                    // needs popping here, as ForRangeExpr's var does
      u32 arena_reg = bc_alloc_reg(fc);
      bc_emit(fc, BcOp_LoadScratchArena, arena_reg, 0, 0);
      u32 mark_reg = bc_alloc_reg(fc);
      bc_emit(fc, BcOp_ArenaMark, mark_reg, arena_reg, 0);
      bc_bind_local(fc, n->scratch_expr.var_name, arena_reg);
      BcScratchScope scope = { arena_reg, mark_reg };
      dyn_push(fc->arena, fc->scratch_scopes, scope);
      u32 result = bc_compile_block(fc, n->scratch_expr.body);
      dyn_hdr(fc->scratch_scopes)->count -= 1;
      bc_emit(fc, BcOp_ArenaPop, arena_reg, mark_reg, 0);
      if (fc->locals) dyn_hdr(fc->locals)->count = locals_mark; // pop the scratch arena's own name
      return result;
    }
    case TypedNodeKind_PushAlloc: {
      // `(push arena Type)` / `(push arena Type Count)` / `(push0 ...)` ->
      // arena_push(_zero)(arena, stride * count). `count` is a runtime
      // expression, not always a literal, so the byte size needs a BcOp_Mul --
      // unlike BcOp_Alloc, whose size is always compile-time-known.
      u32 arena_reg  = bc_compile_expr(fc, n->push_alloc.arena);
      u64 stride     = bc_element_stride(fc, n->push_alloc.elem_type);
      u32 count_reg  = bc_compile_expr(fc, n->push_alloc.count);
      u32 stride_reg = bc_alloc_reg(fc);
      bc_emit(fc, BcOp_LoadConst, stride_reg, bc_add_const(fc, (i64)stride), 0);
      u32 size_reg = bc_alloc_reg(fc);
      bc_emit(fc, BcOp_Mul, size_reg, stride_reg, count_reg);
      u32 dst = bc_alloc_reg(fc);
      bc_emit(fc, n->push_alloc.zeroed ? BcOp_ArenaPushZero : BcOp_ArenaPush, dst, arena_reg, size_reg);
      return dst;
    }
    case TypedNodeKind_PushCopy: {
      // `(push arena value)` -- allocates one element sized to `value`'s type
      // and copies it in. bc_store_value already covers both the embedded
      // (real byte copy) and scalar (typed store) cases.
      u32     arena_reg = bc_compile_expr(fc, n->push_copy.arena);
      TypeRef value_ty   = fc->ck->resolved_types[n->push_copy.value];
      Layout  layout      = layout_of(fc->layout_cache, fc->ck, value_ty);
      u32     size_reg    = bc_alloc_reg(fc);
      bc_emit(fc, BcOp_LoadConst, size_reg, bc_add_const(fc, (i64)layout.size), 0);
      u32 dst = bc_alloc_reg(fc);
      bc_emit(fc, BcOp_ArenaPush, dst, arena_reg, size_reg);
      u32 val_reg = bc_compile_expr(fc, n->push_copy.value);
      bc_store_value(fc, n->token, dst, 0, value_ty, val_reg);
      return dst;
    }
    case TypedNodeKind_AllocExpr: {
      // `(alloc Type)` / `(alloc Type Count)` -- malloc-backed, cast to
      // `Type*`. The same size computation as PushAlloc with BcOp_Malloc
      // instead of BcOp_ArenaPush, and no zeroed variant. `free` is the other
      // half of the pair, a special-cased Call below.
      u64 stride     = bc_element_stride(fc, n->alloc_expr.elem_type);
      u32 count_reg  = bc_compile_expr(fc, n->alloc_expr.count);
      u32 stride_reg = bc_alloc_reg(fc);
      bc_emit(fc, BcOp_LoadConst, stride_reg, bc_add_const(fc, (i64)stride), 0);
      u32 size_reg = bc_alloc_reg(fc);
      bc_emit(fc, BcOp_Mul, size_reg, stride_reg, count_reg);
      u32 dst = bc_alloc_reg(fc);
      bc_emit(fc, BcOp_Malloc, dst, size_reg, 0);
      return dst;
    }
    case TypedNodeKind_CheckedMath: {
      // `(sqrt-checked x)`/`(asin-checked x)`/`(acos-checked x)`/
      // `(pow-checked base exp)`: call libm (always in f64 -- see BcOp_Sqrt
      // for the f32-precision tradeoff), report `ok = isfinite(result)`, and
      // build the `(bool T)` struct lower_checked_math already interned.
      TypeRef arg_ty = fc->ck->resolved_types[n->checked_math.arg];
      b32     is_f32 = arg_ty.kind == TypeKind_F32;
      String8 result_struct_name = is_f32 ? n->checked_math.f32_struct_name : n->checked_math.f64_struct_name;

      u32 arg_reg = bc_compile_expr(fc, n->checked_math.arg);
      u32 arg_f64 = arg_reg;
      if (is_f32) {
        arg_f64 = bc_alloc_reg(fc);
        bc_emit(fc, BcOp_F32ToF64, arg_f64, arg_reg, 0);
      }

      u32 raw = bc_alloc_reg(fc);
      if (str8_match_lit("pow", n->checked_math.libm_name, 0)) {
        u32 arg2_reg = bc_compile_expr(fc, n->checked_math.arg2);
        u32 arg2_f64 = arg2_reg;
        if (is_f32) {
          arg2_f64 = bc_alloc_reg(fc);
          bc_emit(fc, BcOp_F32ToF64, arg2_f64, arg2_reg, 0);
        }
        bc_emit(fc, BcOp_Pow, raw, arg_f64, arg2_f64);
      } else {
        BcOp op = str8_match_lit("sqrt", n->checked_math.libm_name, 0) ? BcOp_Sqrt
                : str8_match_lit("asin", n->checked_math.libm_name, 0) ? BcOp_Asin
                                                                          : BcOp_Acos;
        bc_emit(fc, op, raw, arg_f64, 0);
      }

      u32 ok_reg = bc_alloc_reg(fc);
      bc_emit(fc, BcOp_F64IsFinite, ok_reg, raw, 0);

      u32 value_reg = raw;
      if (is_f32) {
        value_reg = bc_alloc_reg(fc);
        bc_emit(fc, BcOp_F64ToF32, value_reg, raw, 0);
      }

      return bc_compile_bool_t_result(fc, n->token, result_struct_name, ok_reg, value_reg);
    }
    case TypedNodeKind_ParseNumber: {
      // `(string-to-i32 s)` and friends -- see BcOp_ParseNumberValue for the
      // parsing rules. `target_kind` passes straight through as the opcode's
      // `c` operand; both opcodes dispatch on it with no translation table.
      u32 str_reg = bc_compile_expr(fc, n->parse_number.arg);
      u32 value_reg = bc_alloc_reg(fc);
      bc_emit(fc, BcOp_ParseNumberValue, value_reg, str_reg, (u32)n->parse_number.target_kind);
      u32 ok_reg = bc_alloc_reg(fc);
      bc_emit(fc, BcOp_ParseNumberOk, ok_reg, str_reg, (u32)n->parse_number.target_kind);
      return bc_compile_bool_t_result(fc, n->token, n->parse_number.result_struct_name, ok_reg, value_reg);
    }
    case TypedNodeKind_IndexOf: {
      // `(vector-index-of v x)` -- a linear search, the loop `cg_call`'s
      // `vector-contains?` case uses plus index tracking. The comparison
      // dispatches on `elem_ty` exactly as BinaryEq does: embedded ->
      // bc_compile_value_cmp on ADDRESSES, scalar -> the int/f32/f64 Eq family
      // on VALUES. `needle`'s compiled register is already the right shape for
      // whichever path runs, so neither needs unboxing.
      TypeRef elem_ty  = *fc->ck->resolved_types[n->index_of.vec].pointee;
      u32     vec_reg   = bc_compile_expr(fc, n->index_of.vec);
      u32     needle_reg = bc_compile_expr(fc, n->index_of.needle);
      u32     count_reg  = bc_alloc_reg(fc);
      bc_emit(fc, BcOp_DynCount, count_reg, vec_reg, 0);

      u32 idx_reg = bc_alloc_reg(fc);
      bc_emit(fc, BcOp_LoadConst, idx_reg, bc_add_const(fc, 0), 0);
      u32 found_reg = bc_alloc_reg(fc);
      bc_emit(fc, BcOp_LoadConst, found_reg, bc_add_const(fc, 0), 0);
      u32 result_idx_reg = bc_alloc_reg(fc);
      bc_emit(fc, BcOp_Move, result_idx_reg, count_reg, 0); // not-found default: the count itself

      u32 loop_start = (u32)dyn_count(fc->code);
      u32 cond_reg    = bc_alloc_reg(fc);
      bc_emit(fc, BcOp_Lt, cond_reg, idx_reg, count_reg);
      u32 jf_done = bc_emit(fc, BcOp_JumpIfFalse, cond_reg, 0, 0); // patched below

      // `one_reg` must be loaded HERE, unconditionally, before the match/
      // no-match branch -- never inside the found branch. The no-match branch
      // needs it too, and is reached by jumping PAST that code, so a load
      // placed on the fallthrough side would leave the register at its
      // initial zero there: "advance by 1" becomes "advance by 0", an
      // infinite loop rather than a crash.
      u32 one_reg = bc_alloc_reg(fc);
      bc_emit(fc, BcOp_LoadConst, one_reg, bc_add_const(fc, 1), 0);

      u64 stride     = bc_element_stride(fc, elem_ty);
      u32 stride_reg = bc_alloc_reg(fc);
      bc_emit(fc, BcOp_LoadConst, stride_reg, bc_add_const(fc, (i64)stride), 0);
      u32 byte_off_reg = bc_alloc_reg(fc);
      bc_emit(fc, BcOp_Mul, byte_off_reg, idx_reg, stride_reg);
      u32 elem_addr_reg = bc_alloc_reg(fc);
      bc_emit(fc, BcOp_Add, elem_addr_reg, vec_reg, byte_off_reg);

      u32 eq_reg;
      if (bc_field_is_embedded(fc->ck, elem_ty)) {
        u32 cmp = bc_compile_value_cmp(fc, n->token, elem_ty, elem_addr_reg, needle_reg);
        u32 zero_reg = bc_alloc_reg(fc);
        bc_emit(fc, BcOp_LoadConst, zero_reg, bc_add_const(fc, 0), 0);
        eq_reg = bc_alloc_reg(fc);
        bc_emit(fc, BcOp_Eq, eq_reg, cmp, zero_reg);
      } else {
        u32 elem_val = bc_alloc_reg(fc);
        bc_emit(fc, bc_load_op_for_type(n->token, elem_ty), elem_val, elem_addr_reg, 0);
        BcOp eq_op;
        if      (elem_ty.kind == TypeKind_F64) eq_op = BcOp_FEq;
        else if (elem_ty.kind == TypeKind_F32) eq_op = BcOp_F32Eq;
        else                                     eq_op = BcOp_Eq;
        eq_reg = bc_alloc_reg(fc);
        bc_emit(fc, eq_op, eq_reg, elem_val, needle_reg);
      }
      u32 jf_no_match = bc_emit(fc, BcOp_JumpIfFalse, eq_reg, 0, 0); // patched below

      bc_emit(fc, BcOp_Move, found_reg, one_reg, 0);
      bc_emit(fc, BcOp_Move, result_idx_reg, idx_reg, 0);
      u32 jend = bc_emit(fc, BcOp_Jump, 0, 0, 0); // found -- skip straight to done, patched below

      fc->code[jf_no_match].b = (u32)dyn_count(fc->code); // no match at this index -- advance and loop
      u32 next_idx_reg = bc_alloc_reg(fc);
      bc_emit(fc, BcOp_Add, next_idx_reg, idx_reg, one_reg);
      bc_emit(fc, BcOp_Move, idx_reg, next_idx_reg, 0);
      bc_emit(fc, BcOp_Jump, loop_start, 0, 0);

      fc->code[jf_done].b = (u32)dyn_count(fc->code); // loop exhausted, nothing matched
      fc->code[jend].a     = (u32)dyn_count(fc->code); // found -- lands at the same done point
      return bc_compile_bool_t_result(fc, n->token, n->index_of.result_struct_name, found_reg, result_idx_reg);
    }
    case TypedNodeKind_DynPush: {
      // `(dyn-push arena arr value)` -- `arr` is a bare mutable local or
      // module-level global name, never a general lvalue. Mirrors
      // cg_dyn_push, which also falls back to a global when the scope lookup
      // misses.
      u32 arena_reg = bc_compile_expr(fc, n->dyn_push.arena);

      b32 is_local  = false;
      u32 local_reg = 0;
      u32 global_slot = 0;
      BcLocal* loc = bc_local_try_lookup(fc, n->dyn_push.arr_name);
      if (loc) {
        // `arr_name` is always Vector-typed, and Vector is always embedded, so
        // bc_bind_local_typed never gives it the address-taken treatment:
        // `loc->reg` is its plain T* value.
        xassert(!loc->is_addr_taken);
        is_local  = true;
        local_reg = loc->reg;
      } else {
        b32 found = fc->global_table && bc_fn_table_try_lookup(fc->global_table, n->dyn_push.arr_name, &global_slot);
        xassert(found && "bc_compile_expr: dyn-push target -- unresolved as either a local or a "
                          "global -- the checker should already have caught this");
      }

      // The register `dyn-push` operates on. For a local this IS the local's
      // own register, which BcOp_DynGrow updates in place, matching the C
      // macro's `arr = arena_dyn_grow(...)` reseat. For a global, LoadGlobal
      // hands back a COPY, so growth's new pointer must be StoreGlobal'd back
      // or it is lost when this expression ends.
      u32 ptr_reg;
      if (is_local) {
        ptr_reg = local_reg;
      } else {
        ptr_reg = bc_alloc_reg(fc);
        bc_emit(fc, BcOp_LoadGlobal, ptr_reg, global_slot, 0);
      }

      TypeRef elem_ty     = fc->ck->resolved_types[n->dyn_push.value];
      u64     elem_stride = bc_element_stride(fc, elem_ty);

      u32 count_reg = bc_alloc_reg(fc);
      bc_emit(fc, BcOp_DynCount, count_reg, ptr_reg, 0);
      u32 cap_reg = bc_alloc_reg(fc);
      bc_emit(fc, BcOp_DynCapacity, cap_reg, ptr_reg, 0);
      u32 need_grow_reg = bc_alloc_reg(fc);
      bc_emit(fc, BcOp_Ge, need_grow_reg, count_reg, cap_reg);
      u32 jf_skip_grow = bc_emit(fc, BcOp_JumpIfFalse, need_grow_reg, 0, 0); // patched below
      bc_emit(fc, BcOp_DynGrow, ptr_reg, arena_reg, (u32)elem_stride); // updates ptr_reg in place
      if (!is_local) bc_emit(fc, BcOp_StoreGlobal, global_slot, ptr_reg, 0);
      fc->code[jf_skip_grow].b = (u32)dyn_count(fc->code);

      // Compiled after the possible grow, matching the order the C macro's
      // body evaluates in.
      u32 value_reg = bc_compile_expr(fc, n->dyn_push.value);

      u32 stride_reg = bc_alloc_reg(fc);
      bc_emit(fc, BcOp_LoadConst, stride_reg, bc_add_const(fc, (i64)elem_stride), 0);
      u32 byte_off_reg = bc_alloc_reg(fc);
      bc_emit(fc, BcOp_Mul, byte_off_reg, count_reg, stride_reg);
      u32 elem_addr_reg = bc_alloc_reg(fc);
      bc_emit(fc, BcOp_Add, elem_addr_reg, ptr_reg, byte_off_reg);
      bc_store_value(fc, n->token, elem_addr_reg, 0, elem_ty, value_reg);

      u32 one_reg = bc_alloc_reg(fc);
      bc_emit(fc, BcOp_LoadConst, one_reg, bc_add_const(fc, 1), 0);
      u32 new_count_reg = bc_alloc_reg(fc);
      bc_emit(fc, BcOp_Add, new_count_reg, count_reg, one_reg);
      bc_emit(fc, BcOp_DynSetCount, ptr_reg, 0, new_count_reg);

      return bc_alloc_reg(fc); // void, the dead-register convention while/for use
    }
    case TypedNodeKind_CommitExpr: {
      // `(commit dst-arena src)` -- BcOp_DynCommit does the work; see it for
      // why `src_reg` is both the opcode's input and its output.
      TypeRef src_ty = fc->ck->resolved_types[n->commit_expr.src];
      xassert(src_ty.kind == TypeKind_Pointer && src_ty.pointee);
      u64 elem_stride = bc_element_stride(fc, *src_ty.pointee);

      u32 dst_arena_reg = bc_compile_expr(fc, n->commit_expr.dst_arena);
      u32 src_reg         = bc_compile_expr(fc, n->commit_expr.src);
      bc_emit(fc, BcOp_DynCommit, src_reg, dst_arena_reg, (u32)elem_stride);
      return src_reg;
    }
    case TypedNodeKind_HandlePoolInit: {
      // `(handle-pool-init Name capacity arena)` -- resolves Name's slot in
      // `handle_pool_table`, stores the pooled type's stride (a separate step
      // for operand-count reasons, see BcOp_HandlePoolSetStride), then runs
      // the rest of pool_init.
      u32 pool_slot;
      b32 found = bc_fn_table_try_lookup(fc->handle_pool_table, n->handle_pool_init.type.name, &pool_slot);
      xassert(found); // the checker already required a matching `(handle Name)` to exist
      u32 pool_reg = bc_alloc_reg(fc);
      bc_emit(fc, BcOp_LoadGlobal, pool_reg, pool_slot, 0);

      Layout  elem_layout = layout_of(fc->layout_cache, fc->ck, n->handle_pool_init.type);
      u32     stride_reg   = bc_alloc_reg(fc);
      bc_emit(fc, BcOp_LoadConst, stride_reg, bc_add_const(fc, (i64)elem_layout.size), 0);
      bc_emit(fc, BcOp_HandlePoolSetStride, pool_reg, stride_reg, 0);

      u32 arena_reg    = bc_compile_expr(fc, n->handle_pool_init.arena);
      u32 capacity_reg = bc_compile_expr(fc, n->handle_pool_init.capacity);
      bc_emit(fc, BcOp_HandlePoolInit, pool_reg, arena_reg, capacity_reg);
      return bc_alloc_reg(fc); // void, the dead-register convention
    }
    case TypedNodeKind_HandleAlloc: {
      // `(handle-alloc Name)` -- there's no `(handle-alloc Mesh value)` form,
      // so `data` is always NULL and the new slot comes back zeroed rather
      // than copy-initialized (the pool's `data` array is already zeroed).
      u32 pool_slot;
      b32 found = bc_fn_table_try_lookup(fc->handle_pool_table, n->type_query.type.name, &pool_slot);
      xassert(found);
      u32 pool_reg = bc_alloc_reg(fc);
      bc_emit(fc, BcOp_LoadGlobal, pool_reg, pool_slot, 0);
      u32 dst = bc_alloc_reg(fc);
      bc_emit(fc, BcOp_HandleAlloc, dst, pool_reg, 0);
      return dst;
    }
    case TypedNodeKind_ReturnExpr: {
      if (n->unary.expr == TYPED_NIL) {
        bc_unwind_scratch_scopes(fc, 0);
        bc_emit(fc, BcOp_ReturnVoid, 0, 0, 0);
      } else {
        // The value is computed BEFORE the enclosing `scratch` scopes rewind:
        // it may be exactly what the scratch arena was allocated for.
        u32 val = bc_compile_expr(fc, n->unary.expr);
        bc_unwind_scratch_scopes(fc, 0);
        bc_emit(fc, BcOp_Return, val, 0, 0);
      }
      // Provably dead: the Return already exited the frame, so nothing
      // downstream reads this. Only meaningful if ReturnExpr appears as a
      // sub-expression rather than a statement.
      return bc_alloc_reg(fc);
    }
    case TypedNodeKind_Block: {
      return bc_compile_block(fc, idx);
    }
    case TypedNodeKind_StructLiteral: {
      StructEntry* se = struct_table_lookup(fc->ck, n->struct_lit.type_name);
      xassert(se); // the checker already required this to resolve
      TypeRef struct_ty = {0};
      struct_ty.kind = TypeKind_Named;
      struct_ty.name = n->struct_lit.type_name;
      Layout layout = layout_of(fc->layout_cache, fc->ck, struct_ty);
      u32    dst    = bc_alloc_reg(fc);
      bc_emit(fc, BcOp_Alloc, dst, (u32)layout.size, (u32)layout.align);
      bc_compile_struct_fields_into(fc, se, idx, dst, 0);
      return dst;
    }
    case TypedNodeKind_ArrayLiteral: {
      TypeRef arr_ty = fc->ck->resolved_types[idx];
      xassert(arr_ty.kind == TypeKind_Array);
      Layout layout = layout_of(fc->layout_cache, fc->ck, arr_ty);
      u32    dst    = bc_alloc_reg(fc);
      bc_emit(fc, BcOp_Alloc, dst, (u32)layout.size, (u32)layout.align);
      bc_compile_array_elements_into(fc, *arr_ty.pointee, idx, dst, 0);
      return dst;
    }
    case TypedNodeKind_FieldAccess: {
      u32         base_reg;
      FieldLayout fl;
      bc_resolve_field_access(fc, idx, &base_reg, &fl);
      u32 dst = bc_alloc_reg(fc);
      if (bc_field_is_embedded(fc->ck, fl.type)) {
        // A nested by-value struct/string/array field's value is just
        // base+offset, with no memory access.
        bc_emit(fc, BcOp_FieldAddr, dst, base_reg, (u32)fl.offset);
      } else {
        bc_emit(fc, bc_load_op_for_type(n->token, fl.type), dst, base_reg, (u32)fl.offset);
      }
      return dst;
    }
    case TypedNodeKind_IndexAccess: {
      u32     elem_addr_reg;
      TypeRef elem_ty;
      bc_resolve_index_access(fc, n->index_access.base, n->index_access.index, &elem_addr_reg, &elem_ty);
      if (bc_field_is_embedded(fc->ck, elem_ty)) {
        return elem_addr_reg; // an embedded element's value is its address, as for a field
      }
      u32 dst = bc_alloc_reg(fc);
      bc_emit(fc, bc_load_op_for_type(n->token, elem_ty), dst, elem_addr_reg, 0);
      return dst;
    }
    case TypedNodeKind_UnaryDeref: {
      // `(deref p)`. For an embedded pointee, dereferencing is a NO-OP here --
      // the address already IS the value -- otherwise it's a scalar load at
      // offset 0.
      TypeRef ptr_ty = fc->ck->resolved_types[n->unary.expr];
      xassert(ptr_ty.kind == TypeKind_Pointer);
      TypeRef pointee_ty = *ptr_ty.pointee;
      u32     addr_reg   = bc_compile_expr(fc, n->unary.expr);
      if (bc_field_is_embedded(fc->ck, pointee_ty)) return addr_reg;
      u32 dst = bc_alloc_reg(fc);
      bc_emit(fc, bc_load_op_for_type(n->token, pointee_ty), dst, addr_reg, 0);
      return dst;
    }
    case TypedNodeKind_UnaryAddr: {
      // `(addr x)`/`&x` -- checker.c's `addr_operand_is_valid` restricts the
      // operand to an Identifier, FieldAccess, IndexAccess or UnaryDeref. The
      // latter three reuse the resolve-to-an-address-without-loading helpers
      // the read and write paths already share.
      TypedNode* operand = &fc->tast->nodes[n->unary.expr];
      switch (operand->kind) {
        case TypedNodeKind_FieldAccess: {
          u32         base_reg;
          FieldLayout fl;
          bc_resolve_field_access(fc, n->unary.expr, &base_reg, &fl);
          u32 dst = bc_alloc_reg(fc);
          bc_emit(fc, BcOp_FieldAddr, dst, base_reg, (u32)fl.offset);
          return dst;
        }
        case TypedNodeKind_IndexAccess: {
          u32     elem_addr_reg;
          TypeRef elem_ty;
          bc_resolve_index_access(fc, operand->index_access.base, operand->index_access.index,
                                   &elem_addr_reg, &elem_ty);
          return elem_addr_reg; // bc_resolve_index_access already hands back an address, no load
        }
        case TypedNodeKind_UnaryDeref:
          // `&*p == p`: compiling `p` itself gives the address, with no need
          // to load through it at all.
          return bc_compile_expr(fc, operand->unary.expr);
        case TypedNodeKind_Identifier: {
          // An embedded-typed identifier's value already IS its address, so
          // this is the identity. A scalar-typed one normally has no address
          // -- its value lives in a register, not memory -- unless
          // bc_scan_address_taken_names flagged the name and
          // bc_bind_local_typed therefore gave it a real backing slot, making
          // its register that slot's address: the same identity, decided at
          // bind time rather than by type.
          //
          // A module-level global has no such mechanism and is a real gap. A
          // spill-a-snapshot fix would NOT alias -- a later `set` on the
          // pointee wouldn't reach the global -- so this reports the gap
          // rather than shipping something that only looks like it works.
          TypeRef ident_ty = fc->ck->resolved_types[n->unary.expr];
          if (bc_field_is_embedded(fc->ck, ident_ty)) return bc_compile_expr(fc, n->unary.expr);
          BcLocal* loc = bc_local_try_lookup(fc, operand->ident.name);
          if (loc) {
            xassert(loc->is_addr_taken && "bc_compile_expr: UnaryAddr on a scalar Identifier local "
                     "that bc_scan_address_taken_names should already have flagged -- a real scan "
                     "bug, not a script-author-facing gap, if reached");
            return loc->reg;
          }
          bc_unsupported(n->token, "`addr`/`&` on a scalar-typed module-level global (this backend "
                                     "keeps a scalar LOCAL addressable now, but a global still has no "
                                     "backing mechanism -- take the address of a field/element/"
                                     "already-dereferenced pointer instead)");
          return bc_alloc_reg(fc);
        }
        default:
          xassert(!"bc_compile_expr: UnaryAddr -- operand kind isn't one addr_operand_is_valid "
                   "allows, the checker should already have caught this");
          return bc_alloc_reg(fc);
      }
    }
    case TypedNodeKind_SetExpr: {
      // `(set target value)` always returns the assigned VALUE, not void.
      // `ck->resolved_types[idx]` on the SetExpr node is whichever target
      // kind's declared type applies -- deref'd pointee, element, field or
      // identifier -- which is exactly what bc_store_value needs across all
      // four kinds, so nothing is re-derived here.
      TypeRef target_ty = fc->ck->resolved_types[idx];
      switch (n->set_expr.target_kind) {
        case SetTargetKind_Identifier: {
          u32 val_reg = bc_compile_expr(fc, n->set_expr.value);
          BcLocal* loc = bc_local_try_lookup(fc, n->set_expr.target_name);
          if (loc) {
            // Address-taken scalar local: `loc->reg` holds its ADDRESS, so
            // this needs a real store through it -- a bare Move would not
            // reach whoever holds `&this-local`.
            if (loc->is_addr_taken) bc_emit(fc, bc_store_op_for_type(n->token, target_ty), loc->reg, 0, val_reg);
            else                     bc_emit(fc, BcOp_Move, loc->reg, val_reg, 0);
            return val_reg; // the assigned value, as the other three target kinds also return
          }
          u32 slot;
          if (fc->global_table && bc_fn_table_try_lookup(fc->global_table, n->set_expr.target_name, &slot)) {
            bc_emit(fc, BcOp_StoreGlobal, slot, val_reg, 0);
            return val_reg;
          }
          xassert(!"bc_compile_expr: set target identifier -- unresolved as either a local or a "
                   "global -- the checker should already have caught this");
          return val_reg;
        }
        case SetTargetKind_Deref: {
          TypeRef ptr_ty  = fc->ck->resolved_types[n->set_expr.target_expr];
          xassert(ptr_ty.kind == TypeKind_Pointer);
          u32 addr_reg = bc_compile_expr(fc, n->set_expr.target_expr);
          u32 val_reg  = bc_compile_expr(fc, n->set_expr.value);
          bc_store_value(fc, n->token, addr_reg, 0, target_ty, val_reg);
          return val_reg;
        }
        case SetTargetKind_Field: {
          u32         base_reg;
          FieldLayout fl;
          bc_resolve_field_access(fc, n->set_expr.target_expr, &base_reg, &fl);
          u32 val_reg = bc_compile_expr(fc, n->set_expr.value);
          bc_store_value(fc, n->token, base_reg, fl.offset, target_ty, val_reg);
          return val_reg;
        }
        case SetTargetKind_Index: {
          u32     elem_addr_reg;
          TypeRef elem_ty;
          bc_resolve_index_access(fc, n->set_expr.index_base, n->set_expr.index_index, &elem_addr_reg, &elem_ty);
          u32 val_reg = bc_compile_expr(fc, n->set_expr.value);
          bc_store_value(fc, n->token, elem_addr_reg, 0, target_ty, val_reg);
          return val_reg;
        }
        default: {
          xassert(!"bc_compile_expr: unknown SetTargetKind");
          return bc_alloc_reg(fc);
        }
      }
    }
    case TypedNodeKind_VarDecl: {
      // An omitted initializer is checker-legal only for an
      // array/Vector/Map/Set-typed `var` -- e.g. `(var vec [i32])`, the usual
      // way to start a growable local. With no init expression to compile,
      // this falls back to the type's zero value, the same path #init_globals
      // takes for a module-level global.
      u32     reg;
      if (n->var_decl.init == TYPED_NIL) {
        reg = bc_compile_zero_value(fc, n->var_decl.type);
      } else {
        reg = bc_compile_expr(fc, n->var_decl.init);
        TypeRef init_ty = fc->ck->resolved_types[n->var_decl.init];
        // Real copy on bind, as in LetExpr -- see VALUE-COPY SEMANTICS.
        if (bc_field_is_embedded(fc->ck, init_ty)) reg = bc_compile_value_copy(fc, init_ty, reg);
      }
      bc_bind_local_typed(fc, n->token, n->var_decl.name, n->var_decl.type, reg);
      return reg; // never read as a value: a decl is only ever a statement
    }
    case TypedNodeKind_ConstDecl: {
      // `val` allows an omitted initializer under the same checker rule as
      // VarDecl above, handled the same way.
      u32 reg;
      if (n->const_decl.init == TYPED_NIL) {
        reg = bc_compile_zero_value(fc, n->const_decl.type);
      } else {
        reg = bc_compile_expr(fc, n->const_decl.init);
        TypeRef init_ty = fc->ck->resolved_types[n->const_decl.init];
        if (bc_field_is_embedded(fc->ck, init_ty)) reg = bc_compile_value_copy(fc, init_ty, reg);
      }
      bc_bind_local_typed(fc, n->token, n->const_decl.name, n->const_decl.type, reg);
      return reg;
    }
    case TypedNodeKind_Call: {
      // The arena/malloc lifecycle builtins, which checker.c also
      // special-cases: never resolved as an ordinary user function or host
      // import even if one shares the name, matching checker.c's own
      // cascading-if shape.
      //
      // Checked before the ordinary arg-compile loop below because several
      // want a register that already holds an ADDRESS, which this backend's
      // embedded-value convention gives for free on an Arena-typed argument --
      // codegen.c needs an explicit `&(...)` there, since a C `Arena` local
      // isn't address-shaped the way a VM register is.
      String8 callee = n->call.callee;
      if (str8_match_lit("create", callee, 0)) {
        // `(create)` / `(create reserve-size)` -- a VM-backed arena, by value.
        // The no-arg default matches codegen.c's 64 MB reserve, which is cheap
        // because a VM arena only pays for pages actually touched.
        u32 size_reg;
        if (n->call.arg_count == 1) {
          size_reg = bc_compile_expr(fc, fc->tast->extra[n->call.arg_first + 0]);
        } else {
          size_reg = bc_alloc_reg(fc);
          bc_emit(fc, BcOp_LoadConst, size_reg, bc_add_const(fc, 64 * 1024 * 1024), 0);
        }
        u32 dst = bc_alloc_reg(fc);
        bc_emit(fc, BcOp_ArenaCreate, dst, size_reg, 0);
        return dst;
      }
      if (str8_match_lit("destroy", callee, 0) || str8_match_lit("reset", callee, 0)
          || str8_match_lit("release", callee, 0)) {
        u32 arena_reg = bc_compile_expr(fc, fc->tast->extra[n->call.arg_first + 0]);
        BcOp op = str8_match_lit("destroy", callee, 0) ? BcOp_ArenaDestroy
                : str8_match_lit("reset", callee, 0)     ? BcOp_ArenaReset
                                                            : BcOp_ArenaRelease;
        bc_emit(fc, op, arena_reg, 0, 0);
        return bc_alloc_reg(fc); // void
      }
      if (str8_match_lit("mark", callee, 0)) {
        u32 arena_reg = bc_compile_expr(fc, fc->tast->extra[n->call.arg_first + 0]);
        u32 dst        = bc_alloc_reg(fc);
        bc_emit(fc, BcOp_ArenaMark, dst, arena_reg, 0);
        return dst;
      }
      if (str8_match_lit("pop", callee, 0)) {
        u32 arena_reg = bc_compile_expr(fc, fc->tast->extra[n->call.arg_first + 0]);
        u32 mark_reg   = bc_compile_expr(fc, fc->tast->extra[n->call.arg_first + 1]);
        bc_emit(fc, BcOp_ArenaPop, arena_reg, mark_reg, 0);
        return bc_alloc_reg(fc); // void
      }
      if (str8_match_lit("free", callee, 0)) {
        u32 ptr_reg = bc_compile_expr(fc, fc->tast->extra[n->call.arg_first + 0]);
        bc_emit(fc, BcOp_Free, ptr_reg, 0, 0);
        return bc_alloc_reg(fc); // void
      }
      if (str8_match_lit("handle-deref", callee, 0) || str8_match_lit("handle-free", callee, 0)
          || str8_match_lit("handle-valid?", callee, 0)) {
        // Unlike `handle-alloc`, which names the pooled type explicitly
        // because there's no value to infer it from, these three take the
        // pool from the HANDLE ARGUMENT's resolved type: TypeKind_Handle
        // carries `.name`, the backing struct's name (`Mesh^` -> "Mesh"),
        // matching codegen.c's own lookup.
        TypedIndex arg_idx     = fc->tast->extra[n->call.arg_first + 0];
        String8    struct_name = fc->ck->resolved_types[arg_idx].name;
        u32        pool_slot;
        b32        found = bc_fn_table_try_lookup(fc->handle_pool_table, struct_name, &pool_slot);
        xassert(found);
        u32 pool_reg = bc_alloc_reg(fc);
        bc_emit(fc, BcOp_LoadGlobal, pool_reg, pool_slot, 0);
        u32 handle_reg = bc_compile_expr(fc, arg_idx);

        if (str8_match_lit("handle-free", callee, 0)) {
          u32 dst = bc_alloc_reg(fc);
          bc_emit(fc, BcOp_HandleFree, dst, pool_reg, handle_reg);
          return dst;
        }
        u32 ptr_reg = bc_alloc_reg(fc);
        bc_emit(fc, BcOp_HandleGet, ptr_reg, pool_reg, handle_reg);
        if (str8_match_lit("handle-deref", callee, 0)) return ptr_reg;
        // `handle-valid?` uses the SAME real index+generation liveness check
        // `handle-deref` does (BcOp_HandleGet != NULL), never a cheaper
        // is-this-the-zero-handle test -- base.h's DEFINE_HANDLE_POOL warns
        // about that distinction, and the native backend once got it wrong.
        u32 zero_reg = bc_alloc_reg(fc);
        bc_emit(fc, BcOp_LoadConst, zero_reg, bc_add_const(fc, 0), 0);
        u32 dst = bc_alloc_reg(fc);
        bc_emit(fc, BcOp_Neq, dst, ptr_reg, zero_reg);
        return dst;
      }

      // The lane builtins, under the one-lane serial fallback (see the LANES
      // section at the top of this file). Zero-argument, checker-validated, so
      // there is nothing here but the constant each one collapses to.
      if (str8_match_lit("lane-index", callee, 0) || str8_match_lit("lane-count", callee, 0)) {
        u32 dst = bc_alloc_reg(fc);
        bc_emit(fc, BcOp_LoadConst, dst,
                bc_add_const(fc, str8_match_lit("lane-index", callee, 0) ? 0 : 1), 0);
        return dst;
      }
      if (str8_match_lit("lane-sync", callee, 0)) {
        // A barrier across one participant is reached and cleared by that
        // participant alone -- no instruction to emit at all.
        return bc_alloc_reg(fc); // void
      }
      if (str8_match_lit("lane-arena", callee, 0)) {
        // Natively this is the LANE THREAD's own ctx_scratch(). With one lane
        // and no threads, this thread's scratch arena is that same arena.
        u32 dst = bc_alloc_reg(fc);
        bc_emit(fc, BcOp_LoadScratchArena, dst, 0, 0);
        return dst;
      }

      if (str8_match_lit("dyn-count", callee, 0)) {
        // `(dyn-count v)` -- the user-facing spelling of the Vector element
        // count that collection `for`, `vector-index-of` and `dyn-push`
        // already read via BcOp_DynCount internally.
        u32 ptr_reg = bc_compile_expr(fc, fc->tast->extra[n->call.arg_first + 0]);
        u32 dst      = bc_alloc_reg(fc);
        bc_emit(fc, BcOp_DynCount, dst, ptr_reg, 0);
        return dst;
      }

      if (str8_match_lit("swap", callee, 0)) {
        // `(swap a b)` is base.h's `Swap(T,a,b)`: `T tmp=a; a=b; b=tmp;`, a
        // REAL byte-content exchange for a struct/string/array, not "make a
        // and b point at each other's storage".
        //
        // That distinction is why `set`'s register-rebind shortcut is WRONG
        // here: anything that took `(addr a)` before the swap must observe the
        // exchanged content afterward, as in C, and a rebind would leave that
        // old address's bytes untouched. So an embedded type gets a real
        // 3-copy byte exchange through the existing copy helpers, while a
        // scalar -- which has no storage identity a register could diverge
        // from -- writes through bc_compile_lvalue_write.
        TypedIndex a_idx = fc->tast->extra[n->call.arg_first + 0];
        TypedIndex b_idx = fc->tast->extra[n->call.arg_first + 1];
        TypeRef    ty    = fc->ck->resolved_types[a_idx]; // same as b's, checker-guaranteed

        if (bc_field_is_embedded(fc->ck, ty)) {
          Layout layout  = layout_of(fc->layout_cache, fc->ck, ty);
          u32    a_addr   = bc_compile_expr(fc, a_idx); // a's OWN address -- never changes below
          u32    b_addr   = bc_compile_expr(fc, b_idx); // b's OWN address -- never changes below
          u32    tmp_addr = bc_compile_value_copy(fc, ty, a_addr); // independent snapshot of a's bytes
          bc_copy_struct_bytes(fc, a_addr, 0, b_addr, layout.size);   // a's storage <- b's current bytes
          bc_copy_struct_bytes(fc, b_addr, 0, tmp_addr, layout.size); // b's storage <- a's original bytes
        } else {
          // Each read MUST be forced into a fresh register before either
          // write. bc_compile_expr on a plain scalar Identifier hands back
          // that local's own storage register rather than a copy, so without
          // these Moves `a_val`/`b_val` would ALIAS a and b. The first
          // bc_compile_lvalue_write then overwrites a's register in place,
          // retroactively changing what `a_val` reads before the second write
          // runs, and the swap sets both to b's original value.
          //
          // An address-taken local's read already lands in a fresh register,
          // so the extra Move only costs anything in the aliasing case.
          u32 a_read = bc_compile_expr(fc, a_idx);
          u32 a_val  = bc_alloc_reg(fc);
          bc_emit(fc, BcOp_Move, a_val, a_read, 0);
          u32 b_read = bc_compile_expr(fc, b_idx);
          u32 b_val  = bc_alloc_reg(fc);
          bc_emit(fc, BcOp_Move, b_val, b_read, 0);
          bc_compile_lvalue_write(fc, n->token, a_idx, ty, b_val);
          bc_compile_lvalue_write(fc, n->token, b_idx, ty, a_val);
        }
        return bc_alloc_reg(fc); // void
      }

      if (str8_match_lit("len", callee, 0)) {
        // `(len x)` -- generic container length (u64) over three shapes, as
        // checker.c defines them. An Array's count is a compile-time constant
        // from the STATIC type, correct even where the value decayed to a bare
        // pointer, since the language type still carries the count; a Vector
        // reads BcOp_DynCount; a string reads `.size` off the boxed header.
        // `x` is always compiled for its side effects, matching codegen.c's
        // `((void)(x), (u64)N)` in the Array case.
        TypedIndex x_idx = fc->tast->extra[n->call.arg_first + 0];
        TypeRef    x_ty  = fc->ck->resolved_types[x_idx];
        u32        x_reg = bc_compile_expr(fc, x_idx);
        u32        dst    = bc_alloc_reg(fc);
        if (x_ty.kind == TypeKind_Array) {
          bc_emit(fc, BcOp_LoadConst, dst, bc_add_const(fc, (i64)x_ty.count), 0);
        } else if (x_ty.kind == TypeKind_Vector) {
          bc_emit(fc, BcOp_DynCount, dst, x_reg, 0);
        } else { // String -- the same `.size` read as TypedNodeKind_StringLenExpr
          bc_emit(fc, BcOp_LoadFieldI64, dst, x_reg, 8);
        }
        return dst;
      }

      if (str8_match_lit("nth-checked", callee, 0)) {
        // `(nth-checked base index)` -- like `nth`, but returns a
        // checked-nilable `T*`, NULL when out of range, instead of trusting
        // the index the way `nth`'s bare pointer arithmetic does. An Array's
        // bound is its compile-time `.count`, a Vector's is BcOp_DynCount.
        //
        // `result_reg` is allocated once and written on BOTH branches -- Add
        // in range, LoadConst-0 out of range -- before they reconverge, so
        // there's no uninitialized read. See TypedNodeKind_IndexOf for what
        // goes wrong when a register is written on only one branch.
        TypedIndex base_idx = fc->tast->extra[n->call.arg_first + 0];
        TypedIndex idx_idx  = fc->tast->extra[n->call.arg_first + 1];
        TypeRef    base_ty  = fc->ck->resolved_types[base_idx];
        TypeRef    elem_ty  = *base_ty.pointee;
        u64        stride   = bc_element_stride(fc, elem_ty);

        u32 base_reg = bc_compile_expr(fc, base_idx);
        u32 idx_reg   = bc_compile_expr(fc, idx_idx);

        u32 count_reg = bc_alloc_reg(fc);
        if (base_ty.kind == TypeKind_Array) {
          bc_emit(fc, BcOp_LoadConst, count_reg, bc_add_const(fc, (i64)base_ty.count), 0);
        } else { // Vector
          bc_emit(fc, BcOp_DynCount, count_reg, base_reg, 0);
        }

        u32 in_range_reg = bc_alloc_reg(fc);
        bc_emit(fc, BcOp_Lt, in_range_reg, idx_reg, count_reg);
        u32 jf = bc_emit(fc, BcOp_JumpIfFalse, in_range_reg, 0, 0); // patched below

        u32 result_reg  = bc_alloc_reg(fc);
        u32 stride_reg   = bc_alloc_reg(fc);
        bc_emit(fc, BcOp_LoadConst, stride_reg, bc_add_const(fc, (i64)stride), 0);
        u32 byte_off_reg = bc_alloc_reg(fc);
        bc_emit(fc, BcOp_Mul, byte_off_reg, idx_reg, stride_reg);
        bc_emit(fc, BcOp_Add, result_reg, base_reg, byte_off_reg);
        u32 jend = bc_emit(fc, BcOp_Jump, 0, 0, 0); // patched below

        fc->code[jf].b = (u32)dyn_count(fc->code); // out of range -- nil
        bc_emit(fc, BcOp_LoadConst, result_reg, bc_add_const(fc, 0), 0);

        fc->code[jend].a = (u32)dyn_count(fc->code);
        return result_reg;
      }

      if (str8_match_lit("vector-clear", callee, 0) && n->call.arg_count == 1) {
        // `(vector-clear v)` truncates the hidden DynHdr's count to 0 via
        // BcOp_DynSetCount. NULL-guarded, matching codegen.c's `if (_3b_v)`:
        // a never-pushed Vector is a NULL pointer, and `dyn_hdr(NULL)` would
        // be invalid. A register's value doubles as its truthiness for
        // BcOp_JumpIfFalse, so no null-check opcode is needed.
        u32 vec_reg = bc_compile_expr(fc, fc->tast->extra[n->call.arg_first + 0]);
        u32 jf = bc_emit(fc, BcOp_JumpIfFalse, vec_reg, 0, 0); // patched below
        u32 zero_reg = bc_alloc_reg(fc);
        bc_emit(fc, BcOp_LoadConst, zero_reg, bc_add_const(fc, 0), 0);
        bc_emit(fc, BcOp_DynSetCount, vec_reg, 0, zero_reg);
        fc->code[jf].b = (u32)dyn_count(fc->code);
        return bc_alloc_reg(fc); // void
      }

      if ((str8_match_lit("vector-swap-remove", callee, 0) || str8_match_lit("vector-remove-at", callee, 0))
          && n->call.arg_count == 2) {
        // `(vector-swap-remove v index)` is O(1): overwrite `index` with the
        // last element and shrink the count. `(vector-remove-at v index)` is
        // order-preserving: shift every following element down instead. Both
        // are checked -- a bool result, no mutation if `index` is out of range
        // -- matching codegen.c.
        //
        // Both the single move and the shift loop's per-step move use the
        // embedded-vs-scalar dispatch already established: an embedded
        // element's value is its address (no load), a scalar gets a typed
        // load, and bc_store_value handles either write.
        b32     is_swap = str8_match_lit("vector-swap-remove", callee, 0);
        TypeRef elem_ty  = *fc->ck->resolved_types[fc->tast->extra[n->call.arg_first + 0]].pointee;
        u64     stride    = bc_element_stride(fc, elem_ty);
        u32     v          = bc_compile_expr(fc, fc->tast->extra[n->call.arg_first + 0]);
        u32     index_reg  = bc_compile_expr(fc, fc->tast->extra[n->call.arg_first + 1]);
        u32     count_reg  = bc_alloc_reg(fc);
        bc_emit(fc, BcOp_DynCount, count_reg, v, 0);
        u32 ok_reg = bc_alloc_reg(fc);
        bc_emit(fc, BcOp_Lt, ok_reg, index_reg, count_reg);
        u32 jf = bc_emit(fc, BcOp_JumpIfFalse, ok_reg, 0, 0); // patched below -- out of range, no mutation

        u32 stride_reg = bc_alloc_reg(fc);
        bc_emit(fc, BcOp_LoadConst, stride_reg, bc_add_const(fc, (i64)stride), 0);
        u32 one_reg = bc_alloc_reg(fc);
        bc_emit(fc, BcOp_LoadConst, one_reg, bc_add_const(fc, 1), 0);
        u32 last_idx_reg = bc_alloc_reg(fc);
        bc_emit(fc, BcOp_Sub, last_idx_reg, count_reg, one_reg);

        if (is_swap) {
          // v[index] = v[count - 1]
          u32 dst_off  = bc_alloc_reg(fc); bc_emit(fc, BcOp_Mul, dst_off, index_reg, stride_reg);
          u32 dst_addr = bc_alloc_reg(fc); bc_emit(fc, BcOp_Add, dst_addr, v, dst_off);
          u32 src_off  = bc_alloc_reg(fc); bc_emit(fc, BcOp_Mul, src_off, last_idx_reg, stride_reg);
          u32 src_addr = bc_alloc_reg(fc); bc_emit(fc, BcOp_Add, src_addr, v, src_off);
          u32 src_val  = src_addr;
          if (!bc_field_is_embedded(fc->ck, elem_ty)) {
            src_val = bc_alloc_reg(fc);
            bc_emit(fc, bc_load_op_for_type(n->token, elem_ty), src_val, src_addr, 0);
          }
          bc_store_value(fc, n->token, dst_addr, 0, elem_ty, src_val);
        } else {
          // for j = index; j+1 < count; j += 1: v[j] = v[j+1]
          u32 j_reg = bc_alloc_reg(fc);
          bc_emit(fc, BcOp_Move, j_reg, index_reg, 0);
          u32 loop_start = (u32)dyn_count(fc->code);
          u32 j_plus_1 = bc_alloc_reg(fc);
          bc_emit(fc, BcOp_Add, j_plus_1, j_reg, one_reg);
          u32 loop_cond = bc_alloc_reg(fc);
          bc_emit(fc, BcOp_Lt, loop_cond, j_plus_1, count_reg);
          u32 loop_jf = bc_emit(fc, BcOp_JumpIfFalse, loop_cond, 0, 0); // patched below

          u32 dst_off  = bc_alloc_reg(fc); bc_emit(fc, BcOp_Mul, dst_off, j_reg, stride_reg);
          u32 dst_addr = bc_alloc_reg(fc); bc_emit(fc, BcOp_Add, dst_addr, v, dst_off);
          u32 src_off  = bc_alloc_reg(fc); bc_emit(fc, BcOp_Mul, src_off, j_plus_1, stride_reg);
          u32 src_addr = bc_alloc_reg(fc); bc_emit(fc, BcOp_Add, src_addr, v, src_off);
          u32 src_val  = src_addr;
          if (!bc_field_is_embedded(fc->ck, elem_ty)) {
            src_val = bc_alloc_reg(fc);
            bc_emit(fc, bc_load_op_for_type(n->token, elem_ty), src_val, src_addr, 0);
          }
          bc_store_value(fc, n->token, dst_addr, 0, elem_ty, src_val);

          bc_emit(fc, BcOp_Move, j_reg, j_plus_1, 0);
          bc_emit(fc, BcOp_Jump, loop_start, 0, 0);
          fc->code[loop_jf].b = (u32)dyn_count(fc->code);
        }

        bc_emit(fc, BcOp_DynSetCount, v, 0, last_idx_reg);
        fc->code[jf].b = (u32)dyn_count(fc->code);
        return ok_reg;
      }

      if (str8_match_lit("vector-contains?", callee, 0) && n->call.arg_count == 2) {
        // `(vector-contains? v x)` -- a linear search returning bool. The same
        // loop shape as TypedNodeKind_IndexOf, but not shared with it:
        // `vector-index-of` also needs the matching INDEX, and returns it
        // through the `(bool u64)` result-struct machinery that doesn't apply
        // to a plain bool.
        TypeRef elem_ty   = *fc->ck->resolved_types[fc->tast->extra[n->call.arg_first + 0]].pointee;
        u32     vec_reg    = bc_compile_expr(fc, fc->tast->extra[n->call.arg_first + 0]);
        u32     needle_reg = bc_compile_expr(fc, fc->tast->extra[n->call.arg_first + 1]);
        u32     count_reg  = bc_alloc_reg(fc);
        bc_emit(fc, BcOp_DynCount, count_reg, vec_reg, 0);

        u32 idx_reg = bc_alloc_reg(fc);
        bc_emit(fc, BcOp_LoadConst, idx_reg, bc_add_const(fc, 0), 0);
        u32 found_reg = bc_alloc_reg(fc);
        bc_emit(fc, BcOp_LoadConst, found_reg, bc_add_const(fc, 0), 0);

        u32 loop_start = (u32)dyn_count(fc->code);
        u32 cond_reg    = bc_alloc_reg(fc);
        bc_emit(fc, BcOp_Lt, cond_reg, idx_reg, count_reg);
        u32 jf_done = bc_emit(fc, BcOp_JumpIfFalse, cond_reg, 0, 0); // patched below

        // Loaded unconditionally before the match/no-match branch -- see
        // TypedNodeKind_IndexOf for why this can't move inside a branch.
        u32 one_reg = bc_alloc_reg(fc);
        bc_emit(fc, BcOp_LoadConst, one_reg, bc_add_const(fc, 1), 0);

        u64 stride     = bc_element_stride(fc, elem_ty);
        u32 stride_reg = bc_alloc_reg(fc);
        bc_emit(fc, BcOp_LoadConst, stride_reg, bc_add_const(fc, (i64)stride), 0);
        u32 byte_off_reg = bc_alloc_reg(fc);
        bc_emit(fc, BcOp_Mul, byte_off_reg, idx_reg, stride_reg);
        u32 elem_addr_reg = bc_alloc_reg(fc);
        bc_emit(fc, BcOp_Add, elem_addr_reg, vec_reg, byte_off_reg);

        u32 eq_reg;
        if (bc_field_is_embedded(fc->ck, elem_ty)) {
          u32 cmp = bc_compile_value_cmp(fc, n->token, elem_ty, elem_addr_reg, needle_reg);
          u32 zero_reg = bc_alloc_reg(fc);
          bc_emit(fc, BcOp_LoadConst, zero_reg, bc_add_const(fc, 0), 0);
          eq_reg = bc_alloc_reg(fc);
          bc_emit(fc, BcOp_Eq, eq_reg, cmp, zero_reg);
        } else {
          u32 elem_val = bc_alloc_reg(fc);
          bc_emit(fc, bc_load_op_for_type(n->token, elem_ty), elem_val, elem_addr_reg, 0);
          BcOp eq_op;
          if      (elem_ty.kind == TypeKind_F64) eq_op = BcOp_FEq;
          else if (elem_ty.kind == TypeKind_F32) eq_op = BcOp_F32Eq;
          else                                     eq_op = BcOp_Eq;
          eq_reg = bc_alloc_reg(fc);
          bc_emit(fc, eq_op, eq_reg, elem_val, needle_reg);
        }
        u32 jf_no_match = bc_emit(fc, BcOp_JumpIfFalse, eq_reg, 0, 0); // patched below

        bc_emit(fc, BcOp_Move, found_reg, one_reg, 0);
        u32 jend = bc_emit(fc, BcOp_Jump, 0, 0, 0); // found -- skip straight to done, patched below

        fc->code[jf_no_match].b = (u32)dyn_count(fc->code); // no match at this index -- advance and loop
        u32 next_idx_reg = bc_alloc_reg(fc);
        bc_emit(fc, BcOp_Add, next_idx_reg, idx_reg, one_reg);
        bc_emit(fc, BcOp_Move, idx_reg, next_idx_reg, 0);
        bc_emit(fc, BcOp_Jump, loop_start, 0, 0);

        fc->code[jf_done].b = (u32)dyn_count(fc->code); // loop exhausted, nothing matched
        fc->code[jend].a     = (u32)dyn_count(fc->code); // found -- lands at the same done point
        return found_reg;
      }

      if ((str8_match_lit("map-set", callee, 0) && n->call.arg_count == 4)
          || (str8_match_lit("set-add", callee, 0) && n->call.arg_count == 3)) {
        // `(map-set arena m key value)` / `(set-add arena s value)` --
        // BcOp_MapSet backs both, with layout->is_set picking the behavior.
        // The argument count differs (a Set has no value), so these don't go
        // through bc_compile_map_key_args, whose fixed 3-arg shape serves the
        // read-by-key builtins below.
        b32        is_map    = str8_match_lit("map-set", callee, 0);
        TypedIndex arena_idx  = fc->tast->extra[n->call.arg_first + 0];
        TypedIndex coll_idx    = fc->tast->extra[n->call.arg_first + 1];
        TypedIndex key_idx      = fc->tast->extra[n->call.arg_first + 2]; // the key for `map-set`, and
                                                                             // for `set-add` the element
                                                                             // being added, which acts
                                                                             // as its own key
        TypeRef    coll_ty     = fc->ck->resolved_types[coll_idx];
        TypeRef    key_ty       = is_map ? *coll_ty.map_key : *coll_ty.pointee;

        BcHashSlotLayout layout;
        TypeRef value_ty = {0};
        if (is_map) {
          value_ty = *coll_ty.pointee;
          layout    = bc_hash_slot_layout(fc->layout_cache, fc->ck, key_ty, &value_ty);
        } else {
          layout = bc_hash_slot_layout(fc->layout_cache, fc->ck, key_ty, NULL);
        }

        u32 coll_reg   = bc_compile_expr(fc, coll_idx);
        u32 arena_reg  = bc_compile_expr(fc, arena_idx);
        u32 key_val    = bc_compile_expr(fc, key_idx);
        u32 key_ptr    = bc_compile_addr_of(fc, n->token, key_ty, key_val);
        u32 value_ptr;
        if (is_map) {
          TypedIndex value_idx = fc->tast->extra[n->call.arg_first + 3];
          u32        value_val  = bc_compile_expr(fc, value_idx);
          value_ptr              = bc_compile_addr_of(fc, n->token, value_ty, value_val);
        } else {
          value_ptr = key_ptr; // never read when layout->is_set, so any register will do
        }
        u32 layout_reg = bc_alloc_reg(fc);
        bc_emit(fc, BcOp_LoadConst, layout_reg, bc_add_layout_const(fc, layout), 0);

        u32 arg_first = fc->next_reg;
        bc_alloc_reg(fc); bc_alloc_reg(fc); bc_alloc_reg(fc); bc_alloc_reg(fc); bc_alloc_reg(fc);
        bc_emit(fc, BcOp_Move, arg_first + 0, coll_reg,   0);
        bc_emit(fc, BcOp_Move, arg_first + 1, arena_reg,  0);
        bc_emit(fc, BcOp_Move, arg_first + 2, key_ptr,    0);
        bc_emit(fc, BcOp_Move, arg_first + 3, value_ptr,  0);
        bc_emit(fc, BcOp_Move, arg_first + 4, layout_reg, 0);
        u32 dst = bc_alloc_reg(fc);
        bc_emit(fc, BcOp_MapSet, dst, arg_first, 0);
        return dst; // checker-declared void for map-set, a real bool for set-add
      }

      if (str8_match_lit("map-get", callee, 0) && n->call.arg_count == 2) {
        // `(map-get m key)` -- a nilable pointer to the matching slot's
        // value, matching BcOp_MapGet's own return directly.
        u32 arg_first = bc_compile_map_key_args(fc, fc->tast->extra[n->call.arg_first + 0],
                                                       fc->tast->extra[n->call.arg_first + 1]);
        u32 dst = bc_alloc_reg(fc);
        bc_emit(fc, BcOp_MapGet, dst, arg_first, 0);
        return dst;
      }

      if ((str8_match_lit("map-contains?", callee, 0) || str8_match_lit("set-contains?", callee, 0))
          && n->call.arg_count == 2) {
        // BcOp_MapGet plus a `!= 0` check -- no separate contains opcode.
        u32 arg_first = bc_compile_map_key_args(fc, fc->tast->extra[n->call.arg_first + 0],
                                                       fc->tast->extra[n->call.arg_first + 1]);
        u32 ptr_reg = bc_alloc_reg(fc);
        bc_emit(fc, BcOp_MapGet, ptr_reg, arg_first, 0);
        u32 zero_reg = bc_alloc_reg(fc);
        bc_emit(fc, BcOp_LoadConst, zero_reg, bc_add_const(fc, 0), 0);
        u32 dst = bc_alloc_reg(fc);
        bc_emit(fc, BcOp_Neq, dst, ptr_reg, zero_reg);
        return dst;
      }

      if ((str8_match_lit("map-remove", callee, 0) || str8_match_lit("set-remove", callee, 0))
          && n->call.arg_count == 2) {
        u32 arg_first = bc_compile_map_key_args(fc, fc->tast->extra[n->call.arg_first + 0],
                                                       fc->tast->extra[n->call.arg_first + 1]);
        u32 dst = bc_alloc_reg(fc);
        bc_emit(fc, BcOp_MapRemove, dst, arg_first, 0);
        return dst;
      }

      if (str8_match_lit("string-match", callee, 0) && n->call.arg_count == 3) {
        // `(string-match a b flags)` -- the escape hatch for fuzzy string
        // comparison via base.h's StringMatchFlags, matching codegen.c's
        // `bbb_string_match` call. `a`/`b`'s compiled values are already their
        // own addresses, so this just fills BcOp_StringMatch's 3-register
        // block (str_a_addr, str_b_addr, flags) the way a Call's argument
        // block is filled.
        u32 a_reg     = bc_compile_expr(fc, fc->tast->extra[n->call.arg_first + 0]);
        u32 b_reg     = bc_compile_expr(fc, fc->tast->extra[n->call.arg_first + 1]);
        u32 flags_reg = bc_compile_expr(fc, fc->tast->extra[n->call.arg_first + 2]);
        u32 arg_first = fc->next_reg;
        bc_alloc_reg(fc); bc_alloc_reg(fc); bc_alloc_reg(fc); // reserve 3 contiguous regs
        bc_emit(fc, BcOp_Move, arg_first + 0, a_reg,     0);
        bc_emit(fc, BcOp_Move, arg_first + 1, b_reg,     0);
        bc_emit(fc, BcOp_Move, arg_first + 2, flags_reg, 0);
        u32 dst = bc_alloc_reg(fc);
        bc_emit(fc, BcOp_StringMatch, dst, arg_first, 0);
        return dst;
      }

      if ((str8_match_lit("abs", callee, 0) && n->call.arg_count == 1)
          || ((str8_match_lit("min", callee, 0) || str8_match_lit("max", callee, 0)) && n->call.arg_count == 2)
          || ((str8_match_lit("clamp-top", callee, 0) || str8_match_lit("clamp-bot", callee, 0)) && n->call.arg_count == 2)
          || (str8_match_lit("clamp", callee, 0) && n->call.arg_count == 3)) {
        // `abs`/`min`/`max`/`clamp-top`/`clamp-bot`/`clamp`. base.h's
        // Abs/Min/Max/ClampTop/ClampBot/Clamp are plain `</>` ternaries with
        // no libm function underneath, unlike sqrt/sin, which need real
        // opcodes. A C ternary evaluates only one arm, matched here by
        // bc_compile_select's jump-and-Move over already-compiled registers.
        //
        // Each argument is therefore evaluated exactly ONCE, unlike the raw
        // macro text codegen.c emits, which double-evaluates a side-effecting
        // argument. int/f32/f64 dispatch goes through bc_typed_op_for_kind.
        TypedIndex x_idx = fc->tast->extra[n->call.arg_first + 0];
        TypeRef    ty    = fc->ck->resolved_types[x_idx];
        u32        x     = bc_compile_expr(fc, x_idx);
        if (str8_match_lit("abs", callee, 0)) {
          u32 zero_reg = bc_alloc_reg(fc);
          bc_emit(fc, BcOp_LoadConst, zero_reg, bc_add_const(fc, 0), 0);
          u32 lt_reg = bc_alloc_reg(fc);
          bc_emit(fc, bc_typed_op_for_kind(ty, TypedNodeKind_BinaryLt), lt_reg, x, zero_reg);
          u32 neg_reg = bc_alloc_reg(fc);
          bc_emit(fc, bc_typed_op_for_kind(ty, TypedNodeKind_BinarySub), neg_reg, zero_reg, x);
          return bc_compile_select(fc, lt_reg, neg_reg, x);
        }
        u32 b = bc_compile_expr(fc, fc->tast->extra[n->call.arg_first + 1]);
        if (str8_match_lit("clamp", callee, 0)) {
          // `(clamp x lo hi)` puts the VALUE first, unlike base.h's own
          // Clamp(A,X,B), so lo is `b` and hi is arg2 here:
          // `x < lo ? lo : (x > hi ? hi : x)`.
          u32 hi = bc_compile_expr(fc, fc->tast->extra[n->call.arg_first + 2]);
          u32 gt_reg = bc_alloc_reg(fc);
          bc_emit(fc, bc_typed_op_for_kind(ty, TypedNodeKind_BinaryGt), gt_reg, x, hi);
          u32 inner = bc_compile_select(fc, gt_reg, hi, x);
          u32 lt_reg = bc_alloc_reg(fc);
          bc_emit(fc, bc_typed_op_for_kind(ty, TypedNodeKind_BinaryLt), lt_reg, x, b); // b == lo here
          return bc_compile_select(fc, lt_reg, b, inner);
        }
        // min/max/clamp-top/clamp-bot each reduce to one comparison plus a
        // select: min(a,b)=a<b?a:b, max(a,b)=a>b?a:b,
        // clamp-top(x,top)=min(x,top), clamp-bot(x,bot)=max(x,bot).
        b32  want_lt = str8_match_lit("min", callee, 0) || str8_match_lit("clamp-top", callee, 0);
        BcOp cmp_op   = bc_typed_op_for_kind(ty, want_lt ? TypedNodeKind_BinaryLt : TypedNodeKind_BinaryGt);
        u32  cmp_reg  = bc_alloc_reg(fc);
        bc_emit(fc, cmp_op, cmp_reg, x, b);
        return bc_compile_select(fc, cmp_reg, x, b);
      }

      if ((str8_match_lit("mem-set", callee, 0) && n->call.arg_count == 3)
          || (str8_match_lit("mem-copy", callee, 0) && n->call.arg_count == 3)
          || (str8_match_lit("mem-zero", callee, 0) && n->call.arg_count == 2)
          || (str8_match_lit("mem-compare", callee, 0) && n->call.arg_count == 3)) {
        // The raw-byte escape hatch, matching codegen.c's bbb_MemorySet/
        // MemoryCopy/MemoryZero/MemoryCompare. A pointer argument is already a
        // register holding a real host address; the byte and size arguments go
        // through bc_compile_size_arg, since the checker admits any numeric
        // type there, floats included.
        if (str8_match_lit("mem-compare", callee, 0)) {
          u32 a_ptr = bc_compile_expr(fc, fc->tast->extra[n->call.arg_first + 0]);
          u32 b_ptr = bc_compile_expr(fc, fc->tast->extra[n->call.arg_first + 1]);
          u32 size  = bc_compile_size_arg(fc, fc->tast->extra[n->call.arg_first + 2]);
          // A destination plus 3 inputs doesn't fit one instruction's a/b/c, so
          // the inputs go in a contiguous block, as for BcOp_Call.
          u32 arg_first = fc->next_reg;
          bc_alloc_reg(fc); bc_alloc_reg(fc); bc_alloc_reg(fc);
          bc_emit(fc, BcOp_Move, arg_first + 0, a_ptr, 0);
          bc_emit(fc, BcOp_Move, arg_first + 1, b_ptr, 0);
          bc_emit(fc, BcOp_Move, arg_first + 2, size, 0);
          u32 dst = bc_alloc_reg(fc);
          bc_emit(fc, BcOp_MemCompare, dst, 0, arg_first);
          return dst;
        }
        u32 ptr = bc_compile_expr(fc, fc->tast->extra[n->call.arg_first + 0]);
        if (str8_match_lit("mem-zero", callee, 0)) {
          u32 size = bc_compile_size_arg(fc, fc->tast->extra[n->call.arg_first + 1]);
          bc_emit(fc, BcOp_MemZero, ptr, size, 0);
          return bc_alloc_reg(fc); // void
        }
        // mem-set's 2nd argument is a byte VALUE, mem-copy's is a source
        // POINTER -- only the former needs the numeric normalization.
        b32 is_set = str8_match_lit("mem-set", callee, 0);
        u32 second = is_set ? bc_compile_size_arg(fc, fc->tast->extra[n->call.arg_first + 1])
                            : bc_compile_expr(fc, fc->tast->extra[n->call.arg_first + 1]);
        u32 size   = bc_compile_size_arg(fc, fc->tast->extra[n->call.arg_first + 2]);
        bc_emit(fc, is_set ? BcOp_MemSet : BcOp_MemCopy, ptr, second, size);
        return bc_alloc_reg(fc); // void
      }

      if (((str8_match_lit("sin", callee, 0) || str8_match_lit("cos", callee, 0)) && n->call.arg_count == 1)
          || (str8_match_lit("atan2", callee, 0) && n->call.arg_count == 2)) {
        // `(sin x)`/`(cos x)`/`(atan2 y x)` -- unchecked trig, ordinary
        // TypedNodeKind_Call nodes rather than the CheckedMath family, so they
        // land here instead of a dedicated node kind. Same f64-always
        // convention as BcOp_Sqrt: widen an f32 argument, narrow the result
        // back afterward.
        TypedIndex arg1_idx = fc->tast->extra[n->call.arg_first + 0];
        TypeRef    ty       = fc->ck->resolved_types[arg1_idx];
        b32        is_f32   = ty.kind == TypeKind_F32;
        u32        arg1     = bc_compile_expr(fc, arg1_idx);
        u32        arg1_64  = arg1;
        if (is_f32) {
          arg1_64 = bc_alloc_reg(fc);
          bc_emit(fc, BcOp_F32ToF64, arg1_64, arg1, 0);
        }
        u32 raw = bc_alloc_reg(fc);
        if (str8_match_lit("atan2", callee, 0)) {
          TypedIndex arg2_idx = fc->tast->extra[n->call.arg_first + 1];
          u32        arg2     = bc_compile_expr(fc, arg2_idx);
          u32        arg2_64  = arg2;
          if (is_f32) {
            arg2_64 = bc_alloc_reg(fc);
            bc_emit(fc, BcOp_F32ToF64, arg2_64, arg2, 0);
          }
          bc_emit(fc, BcOp_Atan2, raw, arg1_64, arg2_64);
        } else {
          bc_emit(fc, str8_match_lit("sin", callee, 0) ? BcOp_Sin : BcOp_Cos, raw, arg1_64, 0);
        }
        if (!is_f32) return raw;
        u32 result = bc_alloc_reg(fc);
        bc_emit(fc, BcOp_F64ToF32, result, raw, 0);
        return result;
      }

      if ((str8_match_lit("align-pow2", callee, 0) || str8_match_lit("align-down-pow2", callee, 0)
           || str8_match_lit("align-pad-pow2", callee, 0)) && n->call.arg_count == 2) {
        // base.h's AlignPow2/AlignDownPow2/AlignPadPow2 macros: pure bitwise
        // arithmetic, always integer ops whatever the operand type -- the same
        // stance BinaryBitAnd takes on a checker-permitted float operand.
        u32 x = bc_compile_expr(fc, fc->tast->extra[n->call.arg_first + 0]);
        u32 b = bc_compile_expr(fc, fc->tast->extra[n->call.arg_first + 1]);
        u32 one_reg = bc_alloc_reg(fc);
        bc_emit(fc, BcOp_LoadConst, one_reg, bc_add_const(fc, 1), 0);
        u32 b_minus_1 = bc_alloc_reg(fc);
        bc_emit(fc, BcOp_Sub, b_minus_1, b, one_reg);
        if (str8_match_lit("align-pow2", callee, 0)) {
          u32 not_b_minus_1 = bc_alloc_reg(fc);
          bc_emit(fc, BcOp_BitNot, not_b_minus_1, b_minus_1, 0);
          u32 x_plus = bc_alloc_reg(fc);
          bc_emit(fc, BcOp_Add, x_plus, x, b_minus_1);
          u32 dst = bc_alloc_reg(fc);
          bc_emit(fc, BcOp_BitAnd, dst, x_plus, not_b_minus_1);
          return dst;
        }
        if (str8_match_lit("align-down-pow2", callee, 0)) {
          u32 not_b_minus_1 = bc_alloc_reg(fc);
          bc_emit(fc, BcOp_BitNot, not_b_minus_1, b_minus_1, 0);
          u32 dst = bc_alloc_reg(fc);
          bc_emit(fc, BcOp_BitAnd, dst, x, not_b_minus_1);
          return dst;
        }
        // align-pad-pow2: (0 - x) & (b - 1)
        u32 zero_reg = bc_alloc_reg(fc);
        bc_emit(fc, BcOp_LoadConst, zero_reg, bc_add_const(fc, 0), 0);
        u32 neg_x = bc_alloc_reg(fc);
        bc_emit(fc, BcOp_Sub, neg_x, zero_reg, x);
        u32 dst = bc_alloc_reg(fc);
        bc_emit(fc, BcOp_BitAnd, dst, neg_x, b_minus_1);
        return dst;
      }

      if (str8_match_lit("print", callee, 0) || str8_match_lit("println", callee, 0)) {
        // `(print "template with {} placeholders" v1 v2 ...)` -- the
        // checker already validated arg0 is a string-LITERAL template
        // with exactly as many `{}` placeholders as trailing arguments,
        // each a printable type (`type_ref_is_printable`). Walk the
        // (already-validated) template ONCE, emitting a flat sequence of
        // Print* opcodes -- see BcOp_PrintI64's own bytecode.h comment for
        // why this needs no escaping/format-string synthesis the way
        // codegen.c's native `printf` call does.
        //
        // TWO PASSES, and that is the whole point: every value argument is
        // evaluated first, left to right, and only then does any Print*
        // opcode run. That is 3b's rule for these builtins on both
        // backends -- see the ARGUMENT EVALUATION ORDER note above
        // cg_print_hoist_args in codegen.c for the rule and why C's own
        // unspecified printf argument order can't just be inherited.
        // Emitting the template as it goes and evaluating each `{}` in
        // between is the obvious single-pass shape, and it is wrong twice
        // over: an argument that PRINTS interleaves its output into the
        // middle of this line (native, being one printf, cannot), and an
        // argument that TRAPS leaves the literal text before it already
        // written to a half-finished line.
        //
        // An optional leading `stream` argument redirects the whole
        // sequence: `(print s "count: {}" n)` emits the SAME opcodes,
        // each carrying the stream's register (see BcOp_PrintI64's own
        // bytecode.h comment on the `b`/`c` operands). The checker has
        // already discriminated the two forms on arg0's TYPE -- the
        // no-stream form requires a string LITERAL there -- so this just
        // shifts every argument index by `tmpl_arg`.
        b32     is_println = str8_match_lit("println", callee, 0);
        u32     tmpl_arg   = 0;
        u32     stream_reg = 0;
        u32     has_stream = 0;
        if (n->call.arg_count > 0
            && fc->ck->resolved_types[fc->tast->extra[n->call.arg_first + 0]].kind == TypeKind_Stream) {
          // Compiled ONCE, before any output happens -- a stream
          // expression can be a real call (`(os/stderr)`), and re-
          // evaluating it per placeholder would both repeat its side
          // effects and interleave them with the output it's the target
          // of.
          stream_reg = bc_compile_expr(fc, fc->tast->extra[n->call.arg_first + 0]);
          has_stream = 1;
          tmpl_arg   = 1;
        }
        String8 tmpl        = fc->tast->nodes[fc->tast->extra[n->call.arg_first + tmpl_arg]].string_lit.value;
        u32     first_value = n->call.arg_first + tmpl_arg + 1; // tast->extra index of value argument 0
        u32     value_count = n->call.arg_count - 1 - tmpl_arg;
        u32     value_i     = 0; // index into the VALUE arguments (the ones after the template)

        // PASS 1 -- evaluate, in source order, before a single byte goes out.
        u32* value_regs = NULL; // dyn array, one entry per `{}`, parallel to the value arguments
        foreach_index(vi, value_count) {
          TypedIndex arg_idx = fc->tast->extra[first_value + vi];
          TypeRef    arg_ty  = fc->ck->resolved_types[arg_idx];
          u32        reg     = bc_compile_expr(fc, arg_idx);
          if (arg_ty.kind == TypeKind_F32) {
            // BcOp_PrintF64 is the only float opcode, so an f32 widens here
            // rather than in pass 2 -- which also lands it in a register of
            // its own, doing the same job as the snapshots below.
            u32 widened = bc_alloc_reg(fc);
            bc_emit(fc, BcOp_F32ToF64, widened, reg, 0);
            reg = widened;
          } else if (arg_ty.kind == TypeKind_String) {
            // A `string` register is the ADDRESS of a boxed {ptr,size}
            // header, so a register snapshot alone would still see a later
            // argument overwrite that box -- `(println "{} / {}" (. p name)
            // (set (. p name) "two"))` writes THROUGH the field's address.
            // A header copy is what native's own hoisting does (a by-value
            // `String8` binding), and is the same by-value copy this
            // backend already makes for a `string` passed to a function --
            // see the value-copy at the general call path below.
            reg = bc_compile_value_copy(fc, arg_ty, reg);
          } else {
            // Snapshot into a register nothing else writes. bc_compile_expr
            // hands back a local's HOME register for a bare identifier, and a
            // later argument can assign to that local -- `(println "{} {}" i
            // (set i 5))` must print the old `i`, the value the argument had
            // when its turn came, not whatever the register holds by the time
            // the line is emitted.
            u32 snapshot = bc_alloc_reg(fc);
            bc_emit(fc, BcOp_Move, snapshot, reg, 0);
            reg = snapshot;
          }
          dyn_push(fc->arena, value_regs, reg);
        }

        // PASS 2 -- emit the line: literal chunks and the already-computed
        // values, in template order, with nothing left to evaluate.
        u8* chunk = NULL; // the literal text accumulated since the last flush
        for (u64 i = 0; i < tmpl.size; i += 1) {
          u8 c = tmpl.str[i];
          if (c == '{' && i + 1 < tmpl.size && tmpl.str[i + 1] == '{') { dyn_push(fc->arena, chunk, (u8)'{'); i += 1; continue; }
          if (c == '}' && i + 1 < tmpl.size && tmpl.str[i + 1] == '}') { dyn_push(fc->arena, chunk, (u8)'}'); i += 1; continue; }
          if (c == '{') {
            // A real placeholder -- flush whatever literal text came
            // before it (if any), then print the next value argument (pass
            // 1 already evaluated it) per its own static type.
            if (dyn_count(chunk) > 0) {
              String8 piece = { chunk, dyn_count(chunk) };
              bc_emit(fc, BcOp_PrintString, bc_compile_string_literal(fc, piece), stream_reg, has_stream);
              chunk = NULL;
            }
            TypedIndex arg_idx = fc->tast->extra[first_value + value_i];
            TypeRef    arg_ty  = fc->ck->resolved_types[arg_idx];
            u32        val_reg = value_regs[value_i];
            if (arg_ty.kind == TypeKind_Pointer) {
              bc_emit(fc, BcOp_PrintCString, val_reg, stream_reg, has_stream);
            } else {
              switch (arg_ty.kind) {
                case TypeKind_I8: case TypeKind_I16: case TypeKind_I32: case TypeKind_I64:
                  bc_emit(fc, BcOp_PrintI64, val_reg, stream_reg, has_stream); break;
                case TypeKind_U8: case TypeKind_U16: case TypeKind_U32: case TypeKind_U64:
                  bc_emit(fc, BcOp_PrintU64, val_reg, stream_reg, has_stream); break;
                // F32 too: pass 1 widened it to an f64 in `val_reg`.
                case TypeKind_F32: case TypeKind_F64:
                  bc_emit(fc, BcOp_PrintF64, val_reg, stream_reg, has_stream); break;
                case TypeKind_Bool:
                  bc_emit(fc, BcOp_PrintBool, val_reg, stream_reg, has_stream); break;
                case TypeKind_Char:
                  bc_emit(fc, BcOp_PrintChar, val_reg, stream_reg, has_stream); break;
                case TypeKind_String:
                  bc_emit(fc, BcOp_PrintString, val_reg, stream_reg, has_stream); break;
                default:
                  xassert(!"bc_compile_expr: print/println -- unprintable argument type, "
                           "the checker should already have caught this");
              }
            }
            value_i += 1;
            i += 1; // skip the matching `}` -- checker already guaranteed it's there
            continue;
          }
          // A bare `}` not part of `}}` can't reach here -- the checker
          // already rejected that as malformed -- so every `}` byte is either
          // taken by the `}}` escape check or consumed by the skip above.
          dyn_push(fc->arena, chunk, c); // an ordinary literal byte, needing no escaping
        }
        if (dyn_count(chunk) > 0) {
          String8 piece = { chunk, dyn_count(chunk) };
          bc_emit(fc, BcOp_PrintString, bc_compile_string_literal(fc, piece), stream_reg, has_stream);
        }
        if (is_println) {
          u8 nl = '\n';
          String8 nl_str = { &nl, 1 };
          bc_emit(fc, BcOp_PrintString, bc_compile_string_literal(fc, nl_str), stream_reg, has_stream);
        }
        return bc_alloc_reg(fc); // void
      }

      u32 arg_first = fc->next_reg;
      foreach_index(i, n->call.arg_count) bc_alloc_reg(fc); // reserve the contiguous block up front.
                                                                // Compiling an argument can allocate
                                                                // scratch registers past the block, so
                                                                // it's the Moves below, not this loop,
                                                                // that guarantee contiguity.
      foreach_index(i, n->call.arg_count) {
        TypedIndex arg_idx = fc->tast->extra[n->call.arg_first + i];
        u32        arg_reg = bc_compile_expr(fc, arg_idx);
        TypeRef    arg_ty  = fc->ck->resolved_types[arg_idx];
        // Struct and string arguments get a real by-value copy, matching a C
        // by-value parameter's struct copy at the call site. ARRAY arguments
        // are excluded: they alias, matching C's array-parameter decay.
        if ((arg_ty.kind == TypeKind_Named && !bc_type_is_enum(fc->ck, arg_ty)) || arg_ty.kind == TypeKind_String) {
          arg_reg = bc_compile_value_copy(fc, arg_ty, arg_reg);
        }
        bc_emit(fc, BcOp_Move, arg_first + (u32)i, arg_reg, 0);
      }
      u32 dst = bc_alloc_reg(fc);
      u32 fn_index, host_index, module_index;
      if (bc_fn_table_try_lookup(fc->fn_table, n->call.callee, &fn_index)) {
        bc_emit(fc, BcOp_Call, dst, fn_index, arg_first);
      } else if (bc_module_table_try_lookup(fc->module_table, n->call.callee, &module_index)) {
        bc_emit(fc, BcOp_CallModule, dst, module_index, arg_first);
      } else if (bc_host_import_try_lookup(fc->host_imports, n->call.callee, &host_index)) {
        bc_emit(fc, BcOp_CallHost, dst, host_index, arg_first);
      } else if (bc_local_try_lookup(fc, n->call.callee)) {
        // The callee names a LOCAL or PARAMETER in scope, so this is an
        // INDIRECT call through a function value -- the consuming half of the
        // gap the Identifier case reports. Worth separating from the message
        // below, which would otherwise claim no such function exists when one
        // is bound right here and codegen.c calls it through a plain C
        // function pointer.
        bc_unsupported(n->call.callee_token, "an indirect call through a function-valued local or "
                                               "parameter -- this bytecode compiler slice has no "
                                               "indirect-call opcode");
      } else {
        // A checker-valid call the checker resolved to SOME extern, just not
        // one this run's host_imports registered -- e.g. `(import config)`
        // without also wiring bc_register_config_primitives. Script-author
        // reachable, so a diagnostic rather than an assert.
        diag_error(n->call.callee_token, "call to `%.*s` -- this program has no compiled function or "
                                          "registered host import by that name", str8_varg(n->call.callee));
        longjmp(g_bc_compile_err, 1);
      }
      return dst;
    }
    default: {
      bc_unsupported(n->token, bc_unsupported_kind_label(n->kind));
      return bc_alloc_reg(fc);
    }
  }
}

// Fills `se`'s field list into memory already allocated at (base_reg +
// base_offset). Shared by the top-level case -- a StructLiteral allocating
// its own memory, base_offset == 0 -- and the nested by-value case, where a
// FieldInit whose value is itself a StructLiteral is filled directly into
// the outer allocation's embedded slot rather than getting its own, matching
// real struct-in-struct layout.
//
// A Named, String or array field whose value is NOT an inline literal still
// fills correctly, via a runtime byte copy. Only an array ELEMENT is restricted
// to literals -- see bc_compile_array_elements_into.
static void
bc_compile_struct_fields_into(BcFnCtx* fc, StructEntry* se, TypedIndex lit_idx, u32 base_reg, u64 base_offset) {
  TypedNode* lit = &fc->tast->nodes[lit_idx];
  xassert(lit->kind == TypedNodeKind_StructLiteral);
  foreach_index(i, lit->struct_lit.field_count) {
    FieldInit*  fi = &fc->tast->field_inits[lit->struct_lit.field_first + i];
    FieldLayout fl = layout_field_offset(fc->layout_cache, fc->ck, se, fi->name);
    xassert(fl.found);
    u64        field_offset = base_offset + fl.offset;
    TypedNode* value_node   = &fc->tast->nodes[fi->value];
    if (fl.type.kind == TypeKind_Named && !bc_type_is_enum(fc->ck, fl.type)) {
      if (value_node->kind == TypedNodeKind_StructLiteral) {
        StructEntry* nested_se = struct_table_lookup(fc->ck, fl.type.name);
        xassert(nested_se);
        bc_compile_struct_fields_into(fc, nested_se, fi->value, base_reg, field_offset);
      } else {
        Layout nested_layout = layout_of(fc->layout_cache, fc->ck, fl.type);
        u32    src_reg        = bc_compile_expr(fc, fi->value); // the existing struct's address
        bc_copy_struct_bytes(fc, base_reg, field_offset, src_reg, nested_layout.size);
      }
      continue;
    }
    if (fl.type.kind == TypeKind_String) {
      if (value_node->kind == TypedNodeKind_StringLiteral) {
        bc_fill_string_field(fc, base_reg, field_offset, value_node->string_lit.value);
      } else {
        u32 src_reg = bc_compile_expr(fc, fi->value); // an already-boxed header's address
        bc_copy_boxed_string(fc, base_reg, field_offset, src_reg);
      }
      continue;
    }
    if (fl.type.kind == TypeKind_Array) {
      if (value_node->kind == TypedNodeKind_ArrayLiteral) {
        bc_compile_array_elements_into(fc, *fl.type.pointee, fi->value, base_reg, field_offset);
      } else {
        // Any other array-typed value -- another array in scope, a field of
        // one, a call result -- is the same runtime byte copy the Named case
        // above does, an array being a flat byte range like a struct.
        Layout arr_layout = layout_of(fc->layout_cache, fc->ck, fl.type);
        u32    src_reg    = bc_compile_expr(fc, fi->value); // the source array's address
        bc_copy_struct_bytes(fc, base_reg, field_offset, src_reg, arr_layout.size);
      }
      continue;
    }
    u32 val_reg = bc_compile_expr(fc, fi->value);
    bc_emit(fc, bc_store_op_for_type(lit->token, fl.type), base_reg, (u32)field_offset, val_reg);
  }
}

// Fills a fixed-size array's elements into memory already allocated at
// (base_reg + base_offset) -- shared by a standalone ArrayLiteral and by a
// field whose value is an inline ArrayLiteral, filled into the embedded slot.
//
// Unlike a struct or string field, an embedded ELEMENT initialized from
// anything other than an inline literal is unsupported: it raises a
// diagnostic rather than silently doing nothing.
static void
bc_compile_array_elements_into(BcFnCtx* fc, TypeRef elem_ty, TypedIndex lit_idx, u32 base_reg, u64 base_offset) {
  TypedNode* lit = &fc->tast->nodes[lit_idx];
  xassert(lit->kind == TypedNodeKind_ArrayLiteral);
  u64 stride = bc_element_stride(fc, elem_ty);
  foreach_index(i, lit->array_lit.element_count) {
    TypedIndex elem_idx    = fc->tast->array_elements[lit->array_lit.element_first + i];
    u64        elem_offset = base_offset + i * stride;
    TypedNode* value_node  = &fc->tast->nodes[elem_idx];
    if (elem_ty.kind == TypeKind_Named && value_node->kind == TypedNodeKind_StructLiteral) {
      StructEntry* nested_se = struct_table_lookup(fc->ck, elem_ty.name);
      xassert(nested_se);
      bc_compile_struct_fields_into(fc, nested_se, elem_idx, base_reg, elem_offset);
      continue;
    }
    if (elem_ty.kind == TypeKind_String && value_node->kind == TypedNodeKind_StringLiteral) {
      bc_fill_string_field(fc, base_reg, elem_offset, value_node->string_lit.value);
      continue;
    }
    if (elem_ty.kind == TypeKind_Array && value_node->kind == TypedNodeKind_ArrayLiteral) {
      bc_compile_array_elements_into(fc, *elem_ty.pointee, elem_idx, base_reg, elem_offset);
      continue;
    }
    if (bc_field_is_embedded(fc->ck, elem_ty)) {
      bc_unsupported(value_node->token, "an array element of an embedded type (struct/string/array/"
                                         "arena/Map/Set) initialized from anything other than an "
                                         "inline literal");
      continue;
    }
    u32 val_reg = bc_compile_expr(fc, elem_idx);
    bc_emit(fc, bc_store_op_for_type(lit->token, elem_ty), base_reg, (u32)elem_offset, val_reg);
  }
}

BcChunk
bc_compile_function(Checker* ck, TypedAst* tast, TypedIndex func_idx, Arena* arena,
                     LayoutCache* layout_cache, BcFnTable* fn_table, BcFnTable* global_table,
                     BcFnTable* handle_pool_table, BcHostImportTable* host_imports,
                     BcModuleTable* module_table, HashTable* addr_taken_names) {
  TypedNode* n = &tast->nodes[func_idx];
  xassert(n->kind == TypedNodeKind_FunctionDecl);
  xassert(n->func.body != TYPED_NIL); // an `extern` signature has no body to compile

  BcFnCtx fc = {0};
  fc.ck                = ck;
  fc.tast              = tast;
  fc.arena             = arena;
  fc.layout_cache      = layout_cache;
  fc.fn_table          = fn_table;
  fc.global_table      = global_table;
  fc.handle_pool_table = handle_pool_table;
  fc.host_imports      = host_imports;
  fc.addr_taken_names  = addr_taken_names;
  fc.module_table      = module_table;

  // Reserve the WHOLE incoming-argument block before binding any of it. The
  // calling convention puts param i's value in register i, so those registers
  // must be exactly [0, param_count). Reserving inside the binding loop would
  // break that: binding an ADDRESS-TAKEN param allocates a second register
  // for its backing slot, so the next param would be handed a register the
  // caller never wrote -- reading the previous param's slot address as its
  // own value.
  foreach_index(i, n->func.param_count) bc_alloc_reg(&fc);
  foreach_index(i, n->func.param_count) {
    Param* p = &tast->params[n->func.param_first + i];
    bc_bind_local_typed(&fc, n->token, p->name, p->type, (u32)i); // a Param carries no token of its
                                                                      // own, so the function decl's is
                                                                      // the closest fallback
  }

  b32        is_void = n->func.return_type.kind == TypeKind_Void;
  TypedNode* body     = &tast->nodes[n->func.body];
  xassert(body->kind == TypedNodeKind_Block);
  foreach_index(i, body->block.stmt_count) {
    b32        is_last = (i + 1 == body->block.stmt_count);
    TypedIndex stmt     = tast->extra[body->block.stmt_first + i];
    u32        reg      = bc_compile_expr(&fc, stmt);
    if (is_last && !is_void) {
      // The signal cg_function_body_stmts uses to skip a redundant trailing
      // `return` when the last statement already diverges on every path -- an
      // unconditional `(return ...)` nested inside if/when, with no
      // fallthrough. Unresolved means exactly that; see checker.c's ReturnExpr.
      b32 is_diverging = ck->resolved_types[stmt].kind == TypeKind_Unresolved;
      if (!is_diverging) bc_emit(&fc, BcOp_Return, reg, 0, 0);
    }
  }
  if (is_void) bc_emit(&fc, BcOp_ReturnVoid, 0, 0, 0);

  BcChunk chunk = {0};
  chunk.code          = fc.code;
  chunk.consts        = fc.consts;
  chunk.num_registers = fc.next_reg;
  chunk.param_count   = n->func.param_count;
  chunk.name          = n->func.name;
  chunk.string_fixups = fc.string_fixups;
  chunk.layout_fixups = fc.layout_fixups;
  return chunk;
}

// Compiles the always-present `#init_globals` chunk: one BcOp_StoreGlobal per
// module-level `var`/`val` in `global_decls`, in declaration order, which
// matches global_table's gather order 1:1 so
// `global_table->entries[i].index` is that global's slot. Each stores either
// its initializer's value or, if omitted, its zero value.
//
// The initializers are genuinely EXECUTED once via bc_run_in_program rather
// than constant-folded, so one can be arbitrarily complex -- a function call
// or even a host import -- which C's static-initializer-only globals can't
// do. `#init_globals` takes no parameters and returns void, like any other
// void top-level fn, and `#` can't appear in a 3b identifier so the name
// can't collide.
//
// INITIALIZATION ORDER is declaration order. An initializer reading a global
// declared LATER sees that global's zero value, since its own init hasn't run
// -- well-defined, if occasionally surprising, and no worse than C's
// cross-translation-unit static init order.
static BcChunk
bc_compile_global_init_chunk(Checker* ck, TypedAst* tast, Arena* arena, LayoutCache* layout_cache,
                              BcFnTable* fn_table, BcFnTable* global_table, TypedIndex* global_decls,
                              BcFnTable* handle_pool_table, BcHostImportTable* host_imports,
                              BcModuleTable* module_table) {
  BcFnCtx fc = {0};
  fc.ck                = ck;
  fc.tast              = tast;
  fc.arena             = arena;
  fc.layout_cache      = layout_cache;
  fc.fn_table          = fn_table;
  fc.global_table      = global_table;
  fc.handle_pool_table = handle_pool_table;
  fc.host_imports      = host_imports;
  fc.module_table      = module_table;

  foreach_index(i, dyn_count(global_decls)) {
    TypedNode* n = &tast->nodes[global_decls[i]];
    u32        slot = global_table->entries[i].index;
    TypeRef    ty;
    TypedIndex init;
    if (n->kind == TypedNodeKind_VarDecl) { ty = n->var_decl.type;   init = n->var_decl.init; }
    else                                    { ty = n->const_decl.type; init = n->const_decl.init; }
    u32 val = (init == TYPED_NIL) ? bc_compile_zero_value(&fc, ty) : bc_compile_expr(&fc, init);
    bc_emit(&fc, BcOp_StoreGlobal, slot, val, 0);
  }
  // Every `(handle Name)`-declared pooled struct also gets a fresh, zeroed
  // `sizeof(HandlePool)` block, mirroring codegen.c's `static MeshPool
  // g_Mesh_pool;`. HandlePool is a fixed base.h struct, so plain C
  // sizeof/AlignOf apply with no layout.c involvement.
  foreach_index(i, dyn_count(handle_pool_table->entries)) {
    u32 slot = handle_pool_table->entries[i].index;
    u32 dst  = bc_alloc_reg(&fc);
    bc_emit(&fc, BcOp_Alloc, dst, (u32)sizeof(HandlePool), (u32)AlignOf(HandlePool));
    bc_emit(&fc, BcOp_StoreGlobal, slot, dst, 0);
  }
  bc_emit(&fc, BcOp_ReturnVoid, 0, 0, 0);

  BcChunk chunk = {0};
  chunk.code          = fc.code;
  chunk.consts        = fc.consts;
  chunk.num_registers = fc.next_reg;
  chunk.param_count   = 0;
  chunk.name          = str8_lit("#init_globals");
  chunk.string_fixups = fc.string_fixups;
  chunk.layout_fixups = fc.layout_fixups;
  return chunk;
}

static void
bc_append_mismatch(BcHostSignatureMismatch** mismatches, Arena* arena, String8 name, String8 reason) {
  BcHostSignatureMismatch m = { name, reason };
  dyn_push(arena, *mismatches, m);
}

// The signature comparison itself, factored out so it can run against either
// "expected" source with identical logic: an extern declaration's own types
// (bc_verify_host_imports below, checking a live TypedAst against the
// host_imports table it's about to compile against), or a signature stored in
// a cache file at save time (bcio.c's load-time re-verification, checking the
// LOADING run's table against what the file was compiled against).
//
// Takes plain `TypeRef` arrays rather than a Param*/TypedAst source, since a
// cache file has no TypedAst at load time. For the same reason errors name a
// parameter by INDEX, not name: a cache file's signature table stores only
// types. That's sufficient, because type_ref_equal ignores names except for
// TypeKind_Named/Handle's `.name`, which IS compared and IS serialized.
//
// Non-static: declared in bcgen.h, since bcio.c is the second caller.
b32
bc_verify_host_import_signature(String8 name, u32 expected_param_count, TypeRef* expected_param_types,
                                 TypeRef expected_return_type, BcHostImport* imp,
                                 Arena* arena, BcHostSignatureMismatch** out_mismatches) {
  b32 ok = true;

  if (expected_param_count != imp->arg_count) {
    ok = false;
    String8 reason = str8f(arena, "arg count mismatch: expected %u param(s), host import registered %u",
                               expected_param_count, imp->arg_count);
    bc_append_mismatch(out_mismatches, arena, name, reason);
    return ok; // mismatched counts make the per-parameter comparison meaningless
  }

  // Rejected here rather than left to bc_call_native_direct's runtime assert,
  // so it surfaces as a real diagnostic instead of an assert deep in a native
  // call -- the same treatment Direct+float gets below.
  if (imp->kind == BcHostImportKind_Direct && imp->arg_count > BC_NATIVE_DIRECT_MAX_ARGS) {
    ok = false;
    String8 reason = str8f(arena, "%u arg(s) -- BcHostImportKind_Direct supports at most %u (see bcnative.c)",
                               imp->arg_count, (u32)BC_NATIVE_DIRECT_MAX_ARGS);
    bc_append_mismatch(out_mismatches, arena, name, reason);
  }

  foreach_index(pi, expected_param_count) {
    TypeRef expected_ty   = expected_param_types ? expected_param_types[pi] : (TypeRef){0};
    TypeRef registered_ty = imp->param_types ? imp->param_types[pi] : (TypeRef){0};
    if (!type_ref_equal(expected_ty, registered_ty)) {
      ok = false;
      String8 reason = str8f(arena, "param %u type mismatch: expected %.*s, host import registered %.*s",
                                 (u32)pi, str8_varg(type_ref_display(ctx_scratch(), expected_ty)), str8_varg(type_ref_display(ctx_scratch(), registered_ty)));
      bc_append_mismatch(out_mismatches, arena, name, reason);
    }
    if (imp->kind == BcHostImportKind_Direct && (registered_ty.kind == TypeKind_F32 || registered_ty.kind == TypeKind_F64)) {
      ok = false;
      String8 reason = str8f(arena, "param %u is %.*s -- BcHostImportKind_Direct can't pass float/double arguments (see bcnative.c)",
                                 (u32)pi, str8_varg(type_ref_display(ctx_scratch(), registered_ty)));
      bc_append_mismatch(out_mismatches, arena, name, reason);
    }
  }

  if (!type_ref_equal(expected_return_type, imp->return_type)) {
    ok = false;
    String8 reason = str8f(arena, "return type mismatch: expected %.*s, host import registered %.*s",
                               str8_varg(type_ref_display(ctx_scratch(), expected_return_type)), str8_varg(type_ref_display(ctx_scratch(), imp->return_type)));
    bc_append_mismatch(out_mismatches, arena, name, reason);
  }
  if (imp->kind == BcHostImportKind_Direct && (imp->return_type.kind == TypeKind_F32 || imp->return_type.kind == TypeKind_F64)) {
    ok = false;
    String8 reason = str8f(arena, "return type is %.*s -- BcHostImportKind_Direct can't return float/double (see bcnative.c)",
                               str8_varg(type_ref_display(ctx_scratch(), imp->return_type)));
    bc_append_mismatch(out_mismatches, arena, name, reason);
  }

  return ok;
}

b32
bc_verify_host_imports(TypedAst* tast, TypedIndex root, BcHostImportTable* host_imports,
                        Arena* arena, BcHostSignatureMismatch** out_mismatches) {
  TypedNode* root_n = &tast->nodes[root];
  xassert(root_n->kind == TypedNodeKind_Block);
  b32 ok = true;

  foreach_index(i, root_n->block.stmt_count) {
    TypedIndex stmt = tast->extra[root_n->block.stmt_first + i];
    TypedNode* n    = &tast->nodes[stmt];
    if (n->kind != TypedNodeKind_FunctionDecl || n->func.body != TYPED_NIL) continue; // externs only

    u32 host_index;
    if (!bc_host_import_try_lookup(host_imports, n->func.name, &host_index)) continue; // not hosted
                                                                                           // through this
                                                                                           // table, which
                                                                                           // is fine
    BcHostImport* imp = &host_imports->entries[host_index];

    // Small and transient, but allocated out of the caller's `arena` rather
    // than a scratch temp: that avoids nesting two ArenaTemps on one arena,
    // a pitfall base.h documents.
    TypeRef* expected_param_types = push_array(arena, TypeRef, n->func.param_count);
    foreach_index(pi, n->func.param_count) expected_param_types[pi] = tast->params[n->func.param_first + pi].type;

    if (!bc_verify_host_import_signature(n->func.name, n->func.param_count, expected_param_types,
                                          n->func.return_type, imp, arena, out_mismatches)) {
      ok = false;
    }
  }

  return ok;
}

BcProgram
bc_compile_program(Checker* ck, TypedAst* tast, TypedIndex root, Arena* arena, Arena* heap,
                    LayoutCache* layout_cache, BcHostImportTable* host_imports,
                    BcModuleTable* module_table) {
  // Catches any bc_unsupported() from arbitrarily deep inside the
  // bc_compile_expr recursion below. diag_error already printed at the source
  // location by the time control lands here, so this only has to hand back an
  // unusable program. None of the locals below are read on this branch, so
  // none needs `volatile` for setjmp's usual caveat.
  if (setjmp(g_bc_compile_err)) {
    BcProgram failed = {0};
    return failed;
  }

  TypedNode* root_n = &tast->nodes[root];
  xassert(root_n->kind == TypedNodeKind_Block);

  // One whole-program pass, not per-function -- see
  // bc_scan_address_taken_names.
  HashTable addr_taken_names = {0};
  hashtable_init(arena, &addr_taken_names, 64);
  bc_scan_address_taken_names(tast, arena, &addr_taken_names);

  if (host_imports) {
    BcHostSignatureMismatch* mismatches = NULL;
    if (!bc_verify_host_imports(tast, root, host_imports, arena, &mismatches)) {
      fprintf(stderr, "bc_compile_program: host import signature mismatch(es):\n");
      foreach_index(i, dyn_count(mismatches)) {
        fprintf(stderr, "  %.*s: %.*s\n", str8_varg(mismatches[i].name), str8_varg(mismatches[i].reason));
      }
      xassert(!"bc_compile_program: at least one host import signature mismatch (see stderr) -- "
               "this is an embedding-program bug: the extern declaration and the host_imports "
               "registration disagree about a function's real signature");
    }
  }

  // Pass 0: give every top-level `var`/`val` a slot, before structs and
  // functions, since an initializer or any function body may reference any
  // global regardless of declaration order -- the same gather-names-first
  // reasoning as passes 1a/1b. Reuses BcFnEntry{name, index}, where "index"
  // means a global slot rather than a chunk index.
  //
  // `globals` gets one zeroed placeholder per global, in the same order;
  // `#init_globals` overwrites each with its real value. Nothing reads a
  // global before that chunk runs, since bc_compile_program runs it itself
  // before returning.
  BcFnTable   global_table = {0};
  i64*        globals       = NULL; // dyn array, index i <-> global_table.entries[i].index
  TypedIndex* global_decls  = NULL; // dyn array, parallel to global_table.entries, DECLARATION order
  foreach_index(i, root_n->block.stmt_count) {
    TypedIndex stmt = tast->extra[root_n->block.stmt_first + i];
    TypedNode* n    = &tast->nodes[stmt];
    if (n->kind != TypedNodeKind_VarDecl && n->kind != TypedNodeKind_ConstDecl) continue;
    String8 name = (n->kind == TypedNodeKind_VarDecl) ? n->var_decl.name : n->const_decl.name;
    BcFnEntry entry = { name, (u32)dyn_count(globals) };
    dyn_push(arena, global_table.entries, entry);
    dyn_push(arena, globals, (i64)0);
    dyn_push(arena, global_decls, stmt);
  }

  // Same idea, separate table: every top-level `(handle Name)` gets a slot in
  // the same `globals` array, keyed by the pooled struct's name. A separate
  // namespace from global_table, so a pooled struct name and a `var`/`val`
  // name spelled the same can't collide.
  BcFnTable handle_pool_table = {0};
  foreach_index(i, root_n->block.stmt_count) {
    TypedIndex stmt = tast->extra[root_n->block.stmt_first + i];
    TypedNode* n    = &tast->nodes[stmt];
    if (n->kind != TypedNodeKind_HandlePoolDecl) continue;
    BcFnEntry entry = { n->handle_pool_decl.type_name, (u32)dyn_count(globals) };
    dyn_push(arena, handle_pool_table.entries, entry);
    dyn_push(arena, globals, (i64)0);
  }

  // Pass 1a: reserve one chunk slot per deeply-comparable struct -- see the
  // CONTENT comparison note above bc_struct_cmp_chunk_name. Reserved before
  // ordinary functions but into the SAME fn_table, since a comparator is
  // resolved by name through the ordinary BcFnTable/BcOp_Call machinery.
  // Gathering names up front, as pass 1b does, means mutually-referencing
  // comparable structs resolve whichever comparator is synthesized first.
  BcFnTable      fn_table    = {0};
  StructEntry**  cmp_structs = NULL; // dyn array, index i <-> fn_table.entries[i] for i < dyn_count(cmp_structs)
  foreach_index(i, dyn_count(ck->structs)) {
    StructEntry* se = &ck->structs[i];
    TypeRef      named_ty = {0};
    named_ty.kind = TypeKind_Named;
    named_ty.name = se->name;
    if (!type_ref_is_deep_comparable(tast, ck->structs, dyn_count(ck->structs), named_ty)) continue;
    BcFnEntry entry = { bc_struct_cmp_chunk_name(arena, se->name), (u32)dyn_count(fn_table.entries) };
    dyn_push(arena, fn_table.entries, entry);
    dyn_push(arena, cmp_structs, se);
  }

  // Pass 1b: reserve an index for every top-level FunctionDecl with a body, so
  // a call resolves regardless of order -- forward, backward or recursive --
  // the same two-pass shape checker.c's fn gathering uses.
  TypedIndex* fn_decls = NULL; // dyn array, index i <-> fn_table.entries[dyn_count(cmp_structs) + i]
  foreach_index(i, root_n->block.stmt_count) {
    TypedIndex stmt = tast->extra[root_n->block.stmt_first + i];
    TypedNode* n    = &tast->nodes[stmt];
    if (n->kind != TypedNodeKind_FunctionDecl || n->func.body == TYPED_NIL) continue;
    BcFnEntry entry = { n->func.name, (u32)dyn_count(fn_table.entries) };
    dyn_push(arena, fn_table.entries, entry);
    dyn_push(arena, fn_decls, stmt);
  }

  // Pass 2: compile every chunk body now that the name table is complete.
  // Struct comparators first, then ordinary functions, so dyn_push order into
  // prog.chunks matches the index space pass 1 committed to.
  BcProgram prog = {0};
  prog.globals      = globals;
  prog.module_table = module_table;
  foreach_index(i, dyn_count(cmp_structs)) {
    String8 chunk_name = fn_table.entries[i].name;
    BcChunk chunk = bc_compile_struct_cmp_chunk(ck, tast, cmp_structs[i], arena, layout_cache, &fn_table, chunk_name);
    dyn_push(arena, prog.chunks, chunk);
  }
  foreach_index(i, dyn_count(fn_decls)) {
    BcChunk chunk = bc_compile_function(ck, tast, fn_decls[i], arena, layout_cache, &fn_table, &global_table,
                                          &handle_pool_table, host_imports, module_table, &addr_taken_names);
    dyn_push(arena, prog.chunks, chunk);
  }

  // `#init_globals` is always synthesized and compiled -- a trivially empty
  // chunk with zero globals -- then run once here, before returning. See
  // bcgen.h for the reasoning, including why the run uses `heap` and not
  // `arena`.
  BcChunk init_chunk = bc_compile_global_init_chunk(ck, tast, arena, layout_cache, &fn_table,
                                                      &global_table, global_decls, &handle_pool_table,
                                                      host_imports, module_table);
  dyn_push(arena, prog.chunks, init_chunk);
  prog.ok = true;
  u32 init_idx = bc_program_find_fn(&prog, str8_lit("#init_globals"));
  bc_run_in_program(&prog, init_idx, NULL, 0, heap, host_imports);

  return prog;
}

u32
bc_program_find_fn(BcProgram* prog, String8 name) {
  foreach_index(i, dyn_count(prog->chunks)) {
    if (str8_match(prog->chunks[i].name, name, 0)) return (u32)i;
  }
  xassert(!"bc_program_find_fn: no compiled function with this name");
  return 0;
}
