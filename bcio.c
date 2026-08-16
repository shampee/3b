// bcio.c -- BcProgram serialization and disk caching; see bcio.h.
//
// LOADING IS A REAL MMAP, NOT A READ-THEN-COPY. bc_program_load maps the cache
// file via file_map (file.c) and hands back pointers DIRECTLY into those pages
// for every chunk's code/const arrays and every string constant's bytes. The
// on-disk layout of an array is a real `DynHdr` (base/base.h) immediately
// followed by the raw `BcInstr`/`i64` bytes -- bit for bit what a
// dyn_push-built array already looks like in memory -- so `chunk.code` is just
// `mapped_ptr + sizeof(DynHdr)`.
//
// Every DynHdr+array region is padded to `AlignOf(DynHdr)` in the FILE before
// writing. mmap's base address is page-aligned, a superset of 8-byte
// alignment, so an 8-aligned file offset stays 8-aligned once mapped, making
// the pointer cast onto mapped bytes defined rather than UB.
//
// The mapping is WRITABLE (copy-on-write), not read-only, because several
// things in a saved program can't be taken at face value and are PATCHED in
// place. The unifying problem is that a compiled BcProgram is full of values
// that are only meaningful to the PROCESS THAT COMPILED IT -- addresses, and
// indices into tables that process happened to build. Each one is saved as
// something a different process can rebuild it from (bytes, a name, a set of
// fields) and reconstituted here:
//
//  1. STRING CONSTANTS ARE LIVE POINTERS. bcgen.c's
//     bc_compile_string_literal/bc_fill_string_field put a real
//     compile-time-computed address into the const pool -- either a raw bytes
//     pointer or the address of a boxed {ptr,size} String8 header. A file
//     stores the byte CONTENT instead (BcStringFixup, bytecode.h). On load the
//     raw-bytes case points `consts[slot]` straight at the mapped bytes, no
//     allocation; the boxed-header case allocates one 16-byte String8 into
//     `arena`, since the header needs a stable address of its own, but its
//     `.str` still points into the mapping rather than a copy.
//
//  2. A MAP/SET CALL SITE'S SLOT DESCRIPTOR IS A LIVE POINTER TOO. bcgen.c
//     bakes a `BcHashSlotLayout*` into the const pool per call site
//     (bc_add_layout_const), which BcOp_MapSet/MapGet/MapRemove read through.
//     A file stores that descriptor's own FIELDS (BcLayoutFixup, bytecode.h),
//     and a load allocates a fresh one into `arena` and points the slot at
//     it. Unlike a string's bytes it can't just point into the mapping: the
//     fields are written individually, so there's no correctly-aligned struct
//     in the file to point at.
//
//  3. BcOp_CallHost's OPERAND IS AN ABSOLUTE INDEX into whatever
//     BcHostImportTable compiled the program. Fine within one process, but not
//     safe to bake into a file: a later run's table may register the same
//     functions in a different order, or a different set, and trusting the old
//     index would silently call the WRONG native function. So saving writes
//     the original table's NAME for that slot instead of the index, and
//     loading looks the name up in this run's table and patches the
//     instruction in place. Without PROT_WRITE that patch would force a full
//     copy of the code array.
//
//  4. BcProgram.module_table POINTS AT OTHER COMPILED PROGRAMS. A cache load
//     skips compilation, so nothing has compiled the imported module either
//     -- the file records each import's qualified NAME ("build/getenv") and
//     bc_program_load hands them to its caller's BcModuleResolver, which
//     compiles/looks up the module and hands back a live program+chunk. The
//     table is rebuilt in file order, which keeps every BcOp_CallModule's
//     baked-in operand pointing at the right entry (see bc_program_save's own
//     comment on why the CallHost reordering hazard can't arise here).
//
// Every translation lives entirely here; no opcode semantics change.
//
// BcChunk.string_fixups and .layout_fixups are both left empty on a loaded
// chunk: their whole job is done by the time that chunk exists, so there's
// nothing to reconstruct.
#include "bcio.h"
#include "bcvm.h" // bc_run_in_program -- bc_program_load runs the loaded `#init_globals` chunk
                     // itself, once, before returning (mirrors bc_compile_program's own identical
                     // step exactly -- see that function's doc comment in bcgen.h)
#include "file.h"
#include <string.h>
#include <stdint.h>

#define BC_CACHE_MAGIC   0x33425343u // "3BSC", arbitrary but fixed/recognizable

// BUMP THIS whenever the on-disk byte layout changes, the BcOp SET changes,
// OR an existing opcode's operand SEMANTICS change. All three are format
// changes, though only the first two are obvious ones:
//
//  - a layout change makes an older binary misread every following field;
//  - a new opcode value makes an older binary's bcvm.c fail on an unknown
//    opcode -- a clean failure, but still a failure;
//  - an operand-semantics change is the subtle case, and the reason the rule
//    is worded this way. When BcOp_Print* gained its optional stream-target
//    operand, neither the opcode set nor the byte layout changed, so an older
//    binary would have loaded the file happily and printed a redirected
//    `(print f "...")` to STDOUT. Wrong output, not a clean error.
//
// A version mismatch is never fatal: bc_program_load treats it as "no usable
// cache" and the caller recompiles. See git log for the per-version history.
// 18: BcOp_ShrU/BcOp_Narrow and the unsigned BcOp_DivU/ModU/LtU/LeU/GtU/GeU
//     inserted, renumbering every opcode past them -- and, separately,
//     BcOp_ParseNumberValue now yields 0 rather than a partial value on a
//     failed parse, the kind of same-opcode semantics change the rule above
//     is worded to catch.
#define BC_CACHE_VERSION 18u

// ~~ A growable byte buffer for serialization. Not built on dyn_push:
// dyn_push operates one ELEMENT at a time, a poor fit for "append N raw
// bytes at once" (a large chunk's code/consts would mean tens of
// thousands of individual push calls) -- this manages its own
// size/capacity directly instead.
typedef struct BcByteBuf {
  u8*    data;
  u64    size;
  u64    capacity;
  Arena* arena;
} BcByteBuf;

static void
bcio_buf_reserve(BcByteBuf* buf, u64 additional) {
  if (buf->size + additional <= buf->capacity) return;
  u64 new_cap = buf->capacity == 0 ? 4096 : buf->capacity;
  while (new_cap < buf->size + additional) new_cap *= 2;
  u8* new_data = push_array(buf->arena, u8, new_cap);
  if (buf->data) MemoryCopy(new_data, buf->data, buf->size);
  buf->data     = new_data;
  buf->capacity = new_cap;
}

static void
bcio_write(BcByteBuf* buf, const void* data, u64 size) {
  if (size == 0) return; // avoid passing a NULL `data` (a valid, empty dyn array's own pointer, e.g.
                            // a chunk with zero consts) into MemoryCopy/memmove -- UB per the C
                            // standard even at size 0, confirmed by UBSan flagging exactly this
  bcio_buf_reserve(buf, size);
  MemoryCopy(buf->data + buf->size, data, size);
  buf->size += size;
}

static void bcio_write_u32(BcByteBuf* buf, u32 v) { bcio_write(buf, &v, sizeof(v)); }
static void bcio_write_u64(BcByteBuf* buf, u64 v) { bcio_write(buf, &v, sizeof(v)); }
static void bcio_write_b32(BcByteBuf* buf, b32 v) { bcio_write(buf, &v, sizeof(v)); }

static void
bcio_write_string(BcByteBuf* buf, String8 s) {
  bcio_write_u32(buf, (u32)s.size);
  bcio_write(buf, s.str, s.size);
}

// Recursively serializes a TypeRef, only for HOST IMPORT SIGNATURES (see
// BcCachedHostImportSig in bcio.h) -- a bounded handful of types per file,
// never bulk data, so the zero-copy concern the code/const arrays have
// doesn't apply.
//
// Writes exactly the fields type_ref_equal examines: kind, name
// (Named/Handle), pointee (Pointer/Array/Vector/Set -- for Pointer, NULL is a
// meaningful `nil` wildcard, not merely absent), count (Array), map_key (Map),
// and fn_param_count/fn_params/fn_return (Fn). Skips alias_name and is_const,
// which type_ref_equal never consults -- they matter only to codegen and
// display, neither relevant to a signature comparison.
static void
bcio_write_type_ref(BcByteBuf* buf, TypeRef t) {
  bcio_write_u32(buf, (u32)t.kind);
  bcio_write_string(buf, t.name);
  bcio_write_u64(buf, t.count);
  b32 has_pointee = t.pointee != NULL;
  bcio_write_b32(buf, has_pointee);
  if (has_pointee) bcio_write_type_ref(buf, *t.pointee);
  b32 has_map_key = t.map_key != NULL;
  bcio_write_b32(buf, has_map_key);
  if (has_map_key) bcio_write_type_ref(buf, *t.map_key);
  bcio_write_u32(buf, t.fn_param_count);
  foreach_index(i, t.fn_param_count) bcio_write_type_ref(buf, t.fn_params[i]);
  b32 has_fn_return = t.fn_return != NULL;
  bcio_write_b32(buf, has_fn_return);
  if (has_fn_return) bcio_write_type_ref(buf, *t.fn_return);
}

// Pads `buf` with zero bytes up to the next `align`-byte boundary --
// see this file's own top-of-file note on why a DynHdr+array region needs
// to land at an ALIGNED file offset (mmap's page-aligned base means an
// aligned file offset is still aligned once mapped, making a direct
// pointer cast onto the mapped bytes safe).
static void
bcio_align(BcByteBuf* buf, u64 align) {
  u64 pad = (align - (buf->size % align)) % align;
  if (pad == 0) return;
  u8 zeros[16] = {0}; // covers every alignment this file actually uses (AlignOf(DynHdr) == 8)
  xassert(pad <= sizeof(zeros));
  bcio_write(buf, zeros, pad);
}

// Writes a REAL DynHdr immediately followed by a raw blit of `arr` --
// bit-for-bit the same layout dyn_push already builds in memory (see
// bcio_dyn_alloc's OLD counterpart, now unneeded on the load side since
// this makes the on-disk bytes directly usable as-is). `elem_size` lets
// one function serve both the BcInstr and i64 array cases below rather
// than duplicating this for each element type.
static void
bcio_write_dyn_array(BcByteBuf* buf, void* arr, u64 count, u64 elem_size) {
  bcio_align(buf, AlignOf(DynHdr));
  DynHdr hdr = {0};
  hdr.count    = count;
  hdr.capacity = count;
  bcio_write(buf, &hdr, sizeof(hdr));
  bcio_write(buf, arr, count * elem_size);
}

// Builds a dyn_count-compatible array in one shot: a real DynHdr immediately
// before the data, with count == capacity. Used ONLY for BcProgram.chunks,
// because bc_program_find_fn calls dyn_count() on it without knowing whether
// it came from compilation or a file. That array is metadata-scale --
// proportional to function count, not data size -- so allocating it costs
// nothing. Every BULK array goes through bcio_mmap_read_dyn_array instead,
// which points into the mapping and allocates nothing.
static void*
bcio_dyn_alloc(Arena* arena, u64 count, u64 elem_size, u64 elem_align) {
  u64     hdr_align = Max(elem_align, AlignOf(DynHdr));
  u64     total      = sizeof(DynHdr) + count * elem_size;
  DynHdr* hdr        = (DynHdr*)arena_push(arena, total, hdr_align);
  hdr->count    = count;
  hdr->capacity = count;
  return (u8*)hdr + sizeof(DynHdr);
}
#define bcio_dyn_alloc_t(arena, count, T) (T*)bcio_dyn_alloc((arena), (count), sizeof(T), AlignOf(T))

u64
bc_content_hash(String8 data) {
  // FNV-1a -- not cryptographic, doesn't need to be: this only ever has
  // to detect "did the source change" for a caching decision, not resist
  // a deliberate adversary.
  u64 hash = 0xcbf29ce484222325ULL;
  foreach_index(i, data.size) {
    hash ^= data.str[i];
    hash *= 0x100000001b3ULL;
  }
  return hash;
}

b32
bc_program_save(BcProgram* prog, BcHostImportTable* host_imports, String8 path, u64 content_hash) {
  ArenaTemp temp = arena_temp_begin(ctx_scratch());
  BcByteBuf buf  = {0};
  buf.arena      = temp.arena;

  bcio_write_u32(&buf, BC_CACHE_MAGIC);
  bcio_write_u32(&buf, BC_CACHE_VERSION);
  bcio_write_u64(&buf, content_hash);
  u64 chunk_count = dyn_count(prog->chunks);
  bcio_write_u32(&buf, (u32)chunk_count);

  // Module-level `var`/`val` globals -- ONLY the COUNT is saved, never the
  // current VALUES: a global's value is mutable RUNTIME state, not part of
  // the static bytecode, and the ALWAYS-PRESENT `#init_globals` chunk
  // (already just an ordinary saved/loaded BcChunk, ordinary code below)
  // re-derives every slot's real initial value at LOAD time anyway (see
  // bc_program_load's own comment) -- so there's nothing else here to do.
  bcio_write_u32(&buf, (u32)dyn_count(prog->globals));

  String8* referenced_host_names = NULL; // dyn array, deduped, PROGRAM-wide (not per-chunk) -- feeds
                                             // the signature table written after the chunk loop below

  foreach_index(ci, chunk_count) {
    BcChunk* chunk = &prog->chunks[ci];
    bcio_write_string(&buf, chunk->name);
    bcio_write_u32(&buf, chunk->num_registers);
    bcio_write_u32(&buf, chunk->param_count);

    u64 code_count = dyn_count(chunk->code);
    bcio_write_dyn_array(&buf, chunk->code, code_count, sizeof(BcInstr));

    u64 const_count = dyn_count(chunk->consts);
    bcio_write_dyn_array(&buf, chunk->consts, const_count, sizeof(i64));

    u64 fixup_count = dyn_count(chunk->string_fixups);
    bcio_write_u32(&buf, (u32)fixup_count);
    foreach_index(i, fixup_count) {
      BcStringFixup* fx = &chunk->string_fixups[i];
      bcio_write_u32(&buf, fx->const_slot);
      bcio_write_b32(&buf, fx->is_header);
      bcio_write_string(&buf, fx->bytes);
    }

    // The OTHER kind of compile-time pointer a const pool can hold (see
    // BcLayoutFixup in bytecode.h): one Map/Set slot descriptor per call
    // site. Written FIELD BY FIELD rather than blitted as a struct, so the
    // on-disk layout stays something this file spells out explicitly --
    // adding a BcHashSlotLayout field would otherwise silently change the
    // file format without touching a line of code here.
    u64 layout_fixup_count = dyn_count(chunk->layout_fixups);
    bcio_write_u32(&buf, (u32)layout_fixup_count);
    foreach_index(i, layout_fixup_count) {
      BcLayoutFixup* fx = &chunk->layout_fixups[i];
      bcio_write_u32(&buf, fx->const_slot);
      bcio_write_u32(&buf, (u32)fx->layout.key_kind);
      bcio_write_u64(&buf, fx->layout.key_size);
      bcio_write_u64(&buf, fx->layout.value_size);
      bcio_write_u64(&buf, fx->layout.slot_size);
      bcio_write_u64(&buf, fx->layout.slot_align);
      bcio_write_u64(&buf, fx->layout.key_offset);
      bcio_write_u64(&buf, fx->layout.value_offset);
      bcio_write_u64(&buf, fx->layout.state_offset);
      bcio_write_b32(&buf, fx->layout.is_set);
    }

    // See this file's own top-of-file note (#3): translate each
    // BcOp_CallHost's baked-in index into ITS OWN import's name, so a
    // future load can re-resolve against a differently-ordered (or
    // differently-populated) host_imports table.
    u32 host_call_count = 0;
    foreach_index(i, code_count) if (chunk->code[i].kind == BcOp_CallHost) host_call_count += 1;
    bcio_write_u32(&buf, host_call_count);
    foreach_index(i, code_count) {
      if (chunk->code[i].kind != BcOp_CallHost) continue;
      u32 host_idx = chunk->code[i].b;
      xassert(host_imports && host_idx < dyn_count(host_imports->entries)); // this program couldn't
                                                                                // have compiled a
                                                                                // CallHost without a
                                                                                // real table to resolve
                                                                                // against in the first place
      bcio_write_u32(&buf, (u32)i);
      String8 hname = host_imports->entries[host_idx].name;
      bcio_write_string(&buf, hname);

      b32 already_tracked = false;
      foreach_index(k, dyn_count(referenced_host_names)) {
        if (str8_match(referenced_host_names[k], hname, 0)) { already_tracked = true; break; }
      }
      if (!already_tracked) dyn_push(temp.arena, referenced_host_names, hname);
    }
  }

  // Program-level host-import SIGNATURE table -- one entry per unique
  // name referenced anywhere above (not per call site), recording the
  // COMPILING run's own registered kind/param_types/return_type so a
  // future load can cross-check the LOADING run's table against it (see
  // BcCachedHostImportSig/bc_verify_cached_host_imports in bcio.h).
  u64 sig_count = dyn_count(referenced_host_names);
  bcio_write_u32(&buf, (u32)sig_count);
  foreach_index(i, sig_count) {
    String8 hname    = referenced_host_names[i];
    u32     host_idx = 0;
    b32     found     = false;
    foreach_index(hi, dyn_count(host_imports->entries)) {
      if (str8_match(host_imports->entries[hi].name, hname, 0)) { host_idx = (u32)hi; found = true; break; }
    }
    xassert(found); // referenced_host_names only ever collected names already resolved against
                       // this exact table above -- this can't fail
    BcHostImport* imp = &host_imports->entries[host_idx];
    bcio_write_string(&buf, hname);
    bcio_write_u32(&buf, (u32)imp->kind);
    bcio_write_u32(&buf, imp->arg_count);
    foreach_index(pi, imp->arg_count) {
      bcio_write_type_ref(&buf, imp->param_types ? imp->param_types[pi] : (TypeRef){0});
    }
    bcio_write_type_ref(&buf, imp->return_type);
  }

  // Cross-package module imports (BcOp_CallModule's own BcModuleTable). Only
  // each entry's QUALIFIED NAME is storable: the rest of a BcModuleImport is
  // a live `BcProgram*` into a separately-compiled module, which is exactly
  // the kind of thing a file can't hold. bc_program_load re-resolves each
  // name through its caller's BcModuleResolver.
  //
  // Written in TABLE ORDER, and every entry is written whether or not any
  // BcOp_CallModule actually references it. That's what keeps each
  // instruction's baked-in operand valid: a load rebuilds the table from
  // this list, in this order, so index N still means entry N. The
  // BcOp_CallHost problem -- an index into a table the LOADING run built
  // independently, which may be ordered differently -- can't arise here,
  // because a loading run has no module table of its own to disagree with.
  u64 module_import_count = prog->module_table ? dyn_count(prog->module_table->entries) : 0;
  bcio_write_u32(&buf, (u32)module_import_count);
  foreach_index(i, module_import_count) {
    bcio_write_string(&buf, prog->module_table->entries[i].name);
  }

  File file = {0};
  file.view = const_view(buf.data, buf.size);
  file.path = path;
  b32 ok = file_store(&file, path);
  arena_temp_end(&temp);
  return ok;
}

// ~~ A bounds-checked cursor over MAPPED memory. Reads that hand back BULK
// data -- arrays, string bytes -- return a pointer into the mapping, never a
// copy. Small scalar fields still MemoryCopy into a local: cheap at 4 bytes,
// and it sidesteps alignment concerns at the many positions that aren't
// explicitly padded (only DynHdr+array regions are, see bcio_align).
//
// `error` is sticky: once a read runs past the end, every later read is a
// no-op rather than reading out of bounds.
typedef struct BcMmapReader {
  u8* data; // mutable -- the mapping is writable (copy-on-write), see file_map's own comment
  u64 size;
  u64 pos;
  b32 error;
} BcMmapReader;

// Returns a pointer to the next `size` bytes within the mapping (NO copy)
// and advances the cursor, or NULL (setting `error`) if that would run
// past the end.
static void*
bcio_mmap_ptr(BcMmapReader* r, u64 size) {
  if (r->error || r->pos + size > r->size) { r->error = true; return NULL; }
  void* p = r->data + r->pos;
  r->pos += size;
  return p;
}

static u32
bcio_mmap_read_u32(BcMmapReader* r) {
  void* p = bcio_mmap_ptr(r, sizeof(u32));
  u32   v = 0;
  if (p) MemoryCopy(&v, p, sizeof(v));
  return v;
}

// A COUNT read off disk, bounded by what the file could actually back.
//
// A plain bcio_mmap_read_u32 is fine for a count that is only ever compared
// against; it is not fine for one that sizes an ALLOCATION, because a corrupt
// file can name 2^32 elements and the arena push for them aborts the process
// -- which this loader must never do, its whole contract being that an
// unusable cache is non-fatal and the caller recompiles.
//
// Every element costs at least one byte in the file it was read from, so a
// count larger than the bytes REMAINING is one the file cannot possibly
// contain. Deliberately loose: this is the bound that needs no per-element
// knowledge, and the reader's own range checks reject the rest as it goes.
static u32
bcio_mmap_read_bounded_count(BcMmapReader* r) {
  u32 count = bcio_mmap_read_u32(r);
  if (r->error) return 0;
  if ((u64)count > r->size - r->pos) { r->error = true; return 0; }
  return count;
}

static b32
bcio_mmap_read_b32(BcMmapReader* r) {
  void* p = bcio_mmap_ptr(r, sizeof(b32));
  b32   v = false;
  if (p) MemoryCopy(&v, p, sizeof(v));
  return v;
}

static u64
bcio_mmap_read_u64(BcMmapReader* r) {
  void* p = bcio_mmap_ptr(r, sizeof(u64));
  u64   v = 0;
  if (p) MemoryCopy(&v, p, sizeof(v));
  return v;
}

// Reads a length-prefixed string WITHOUT copying its bytes -- `.str`
// points directly into the mapping. Safe as long as the caller keeps the
// mapping alive (see bc_program_load's own lifetime note) -- exactly the
// same requirement as every other pointer this loader hands back.
static String8
bcio_mmap_read_string(BcMmapReader* r) {
  u32 len = bcio_mmap_read_u32(r);
  if (r->error || len == 0) return (String8){0};
  void* p = bcio_mmap_ptr(r, len);
  if (!p) return (String8){0};
  return (String8){ (u8*)p, len };
}

// Reads a DynHdr+array region written by bcio_write_dyn_array, returning a
// pointer to the array's FIRST ELEMENT with no copy. That pointer already
// satisfies dyn_count's `dyn_hdr(p) == (DynHdr*)p - 1` contract: the aligned
// bytes immediately before it really are a valid DynHdr sitting in the mapped
// file. `*out_count` comes straight out of that same mapped header, so the
// stream needs no separate count field.
static void*
bcio_mmap_read_dyn_array(BcMmapReader* r, u64 elem_size, u64* out_count) {
  u64 pad = (AlignOf(DynHdr) - (r->pos % AlignOf(DynHdr))) % AlignOf(DynHdr);
  if (pad) bcio_mmap_ptr(r, pad); // consume the alignment padding bcio_align wrote
  void* hdr_ptr = bcio_mmap_ptr(r, sizeof(DynHdr));
  if (!hdr_ptr) { *out_count = 0; return NULL; }
  DynHdr* hdr = (DynHdr*)hdr_ptr; // a REAL, correctly-aligned DynHdr living in the mapping itself
  // `count` comes straight off disk, so `count * elem_size` is the one
  // multiplication here that a corrupt file can make WRAP -- and a wrapped
  // product is small, so bcio_mmap_ptr's bounds check passes and hands back a
  // pointer for an array whose count is still the astronomical original.
  // Checked before multiplying, not after.
  if (hdr->count > UINT64_MAX / (elem_size ? elem_size : 1)) {
    r->error   = true;
    *out_count = 0;
    return NULL;
  }
  *out_count  = hdr->count;
  void* elems = bcio_mmap_ptr(r, hdr->count * elem_size);
  if (!elems) *out_count = 0; // the file claimed more elements than actually fit
  return elems; // NULL (with r->error set) if the file claims more elements than actually fit
}

// The read-side counterpart of bcio_write_type_ref -- reconstructs a
// TypeRef from a host-import signature entry (see BcCachedHostImportSig
// in bcio.h). Small, fixed-size (proportional to type NESTING depth times
// the number of host imports, never bulk data) allocations into `arena`
// for any nested pointee/map_key/fn_params/fn_return -- `.name`'s bytes
// still come straight from the mapping (bcio_mmap_read_string), zero-copy.
static TypeRef
bcio_mmap_read_type_ref(BcMmapReader* r, Arena* arena) {
  TypeRef t = {0};
  t.kind    = (TypeKind)bcio_mmap_read_u32(r);
  t.name    = bcio_mmap_read_string(r);
  t.count   = bcio_mmap_read_u64(r);

  if (bcio_mmap_read_b32(r)) {
    TypeRef* p = push_one(arena, TypeRef);
    *p = bcio_mmap_read_type_ref(r, arena);
    t.pointee = p;
  }
  if (bcio_mmap_read_b32(r)) {
    TypeRef* p = push_one(arena, TypeRef);
    *p = bcio_mmap_read_type_ref(r, arena);
    t.map_key = p;
  }
  t.fn_param_count = bcio_mmap_read_u32(r);
  if (t.fn_param_count > 0) {
    t.fn_params = push_array(arena, TypeRef, t.fn_param_count);
    foreach_index(i, t.fn_param_count) t.fn_params[i] = bcio_mmap_read_type_ref(r, arena);
  }
  if (bcio_mmap_read_b32(r)) {
    TypeRef* p = push_one(arena, TypeRef);
    *p = bcio_mmap_read_type_ref(r, arena);
    t.fn_return = p;
  }
  return t;
}

b32
bc_verify_cached_host_imports(BcCachedHostImportSig* sigs, u64 sig_count,
                               BcHostImportTable* host_imports, Arena* arena,
                               BcHostSignatureMismatch** out_mismatches) {
  b32 ok = true;
  foreach_index(i, sig_count) {
    BcCachedHostImportSig* sig = &sigs[i];
    b32 found      = false;
    u32 host_index = 0;
    if (host_imports) {
      foreach_index(hi, dyn_count(host_imports->entries)) {
        if (str8_match(host_imports->entries[hi].name, sig->name, 0)) { host_index = (u32)hi; found = true; break; }
      }
    }
    if (!found) continue; // a MISSING registration is bc_program_load's OWN concern -- it already
                             // asserts on this while resolving per-chunk CallHost operands, above
                             // this function's own call site. This function's job is only
                             // signature MISMATCHES for names that DO resolve.
    BcHostImport* imp = &host_imports->entries[host_index];
    if (!bc_verify_host_import_signature(sig->name, sig->param_count, sig->param_types,
                                          sig->return_type, imp, arena, out_mismatches)) {
      ok = false;
    }
  }
  return ok;
}

// Structural validation of a chunk table that came off DISK, run before the
// program is allowed to execute (bc_program_load's `#init_globals` call is
// already execution).
//
// The BcMmapReader above keeps every read inside the mapping, but that only
// makes the FILE safe to parse -- it says nothing about whether the
// instructions it parsed are runnable. The VM indexes with these operands
// directly and, being fed only bcgen.c's own output until a file could
// supply them, assumed they were in range: a bad jump target walks `pc` off
// the end of the code array (a real SEGV, reproduced by patching a byte of a
// `.3bc`), and a bad `Call` operand indexes `prog->chunks` out of bounds.
//
// So the invariants the VM relies on are established ONCE, here, rather than
// re-checked on every dispatch:
//   - a chunk has at least one instruction, and the last one ENDS it
//     (Return/ReturnVoid/Jump) -- together with in-range jump targets, this
//     is what bounds `pc`: no instruction can fall off the end;
//   - every opcode is a real one (bcvm.c's dispatch also range-checks, as
//     defense in depth -- this reports it as a bad file instead);
//   - Jump/JumpIfFalse targets, Call chunk indices and LoadConst pool slots
//     are all in range for the chunk they appear in.
//
// Failing means "this is not a usable cache", the same non-fatal outcome as
// a stale version, so the caller recompiles from source.
static b32
bcio_verify_loaded_code(BcChunk* chunks, u32 chunk_count) {
  foreach_index(ci, chunk_count) {
    BcChunk* chunk       = &chunks[ci];
    u64      code_count  = dyn_count(chunk->code);
    u64      const_count = dyn_count(chunk->consts);
    if (code_count == 0) return false;

    // The register window the VM will hand this chunk. Both of these are
    // xassert-ed rather than checked once execution starts (bc_run_in_program's
    // entry, and BcOp_Call's frame push), so a corrupt value has to be caught
    // before then or it aborts the process.
    if (chunk->num_registers > BC_MAX_TOTAL_REGISTERS) return false;
    if (chunk->param_count > chunk->num_registers) return false;

    foreach_index(i, code_count) {
      BcInstr* in = &chunk->code[i];
      if ((u32)in->kind >= (u32)BcOp_Count) return false;
      switch (in->kind) {
        case BcOp_Jump:        if (in->a >= code_count)  return false; break;
        case BcOp_JumpIfFalse: if (in->b >= code_count)  return false; break;
        case BcOp_Call:        if (in->b >= chunk_count) return false; break;
        case BcOp_LoadConst:   if (in->b >= const_count) return false; break;
        default: break;
      }
    }

    BcOp last = chunk->code[code_count - 1].kind;
    if (last != BcOp_Return && last != BcOp_ReturnVoid && last != BcOp_Jump) return false;
  }

  // bc_program_load runs `#init_globals` on the way out and reaches it via
  // bc_program_find_fn, which ASSERTS when no chunk has the name -- correct
  // for a program compiled in this process (its absence would be a bcgen.c
  // bug) but not for one whose chunk names came off disk, where a single
  // corrupted byte takes the process down. Checked here so the file is
  // rejected as unusable instead, leaving that assert to mean what it says.
  // It is also called with zero arguments, against another xassert.
  foreach_index(ci, chunk_count) {
    if (str8_match(chunks[ci].name, str8_lit("#init_globals"), 0)) return chunks[ci].param_count == 0;
  }
  return false;
}

BcLoadResult
bc_program_load(String8 path, BcHostImportTable* host_imports, BcModuleResolver* module_resolver,
                 Arena* arena, Arena* heap) {
  BcLoadResult result = {0};

  File mapped = file_map(path);
  if (mapped.view.size == 0) return result; // file_map already reported the I/O error, if any

  BcMmapReader r = {0};
  r.data = (u8*)(intptr_t)mapped.view.data; // file_map's mapping is writable -- see its own comment
  r.size = mapped.view.size;

  u32 magic   = bcio_mmap_read_u32(&r);
  u32 version = bcio_mmap_read_u32(&r);
  if (r.error || magic != BC_CACHE_MAGIC || version != BC_CACHE_VERSION) {
    file_unmap(&mapped); // stale, foreign, or corrupt file -- caller's fallback is to recompile
    return result;          // from source, so nothing here needs to stay mapped
  }
  result.content_hash = bcio_mmap_read_u64(&r); // see bc_program_save's own `content_hash` param --
                                                    // caller compares this against the CURRENT
                                                    // source's own hash; a mismatch is exactly as
                                                    // "no usable cache" as a bad magic/version would
                                                    // be, just not detectable from THIS function alone

  u32      chunk_count = bcio_mmap_read_bounded_count(&r);
  BcChunk* chunks       = bcio_dyn_alloc_t(arena, chunk_count, BcChunk); // metadata-scale, NOT bulk
                                                                             // data -- see this file's
                                                                             // own top-of-file note on
                                                                             // what's still a real
                                                                             // allocation vs. mmap-backed

  // Module-level globals -- a fresh, zeroed slot array sized to the saved
  // COUNT only (see bc_program_save's own comment on why no VALUES are
  // stored) -- `#init_globals` (an ordinary chunk in the loop below, same
  // as any other) gets run once, right before this function returns, to
  // fill every slot with its real initial value, exactly mirroring what
  // bc_compile_program itself already does after a fresh compile.
  u32  global_count = bcio_mmap_read_bounded_count(&r);
  i64* globals        = bcio_dyn_alloc_t(arena, global_count, i64);
  foreach_index(gi, global_count) globals[gi] = 0; // #init_globals below overwrites every slot
                                                        // regardless, but zeroed-until-then is the
                                                        // more obviously-correct starting state,
                                                        // not just relied on implicitly

  foreach_index(ci, chunk_count) {
    BcChunk chunk = {0};
    chunk.name          = bcio_mmap_read_string(&r); // zero-copy -- points into the mapping
    chunk.num_registers = bcio_mmap_read_u32(&r);
    chunk.param_count   = bcio_mmap_read_u32(&r);

    u64      code_count = 0;
    BcInstr* code        = (BcInstr*)bcio_mmap_read_dyn_array(&r, sizeof(BcInstr), &code_count);
    chunk.code = code;

    u64  const_count = 0;
    i64* consts       = (i64*)bcio_mmap_read_dyn_array(&r, sizeof(i64), &const_count);
    chunk.consts = consts;

    u32 fixup_count = bcio_mmap_read_bounded_count(&r);
    foreach_index(i, fixup_count) {
      u32     const_slot = bcio_mmap_read_u32(&r);
      b32     is_header  = bcio_mmap_read_b32(&r);
      String8 bytes       = bcio_mmap_read_string(&r); // zero-copy -- points into the mapping
      if (r.error || !consts || const_slot >= const_count) continue;
      if (is_header) {
        // ONE small (16-byte) fixed-size allocation regardless of string
        // length -- the header struct itself needs a stable address of
        // its own, but `.str` still points into the mapping, not a copy.
        String8* header = push_one(arena, String8);
        header->str  = bytes.str;
        header->size = bytes.size;
        consts[const_slot] = (i64)(intptr_t)header;
      } else {
        consts[const_slot] = (i64)(intptr_t)bytes.str; // zero-copy, zero allocation
      }
    }

    // Map/Set slot descriptors (see the matching write loop's own comment).
    // Rebuilt as a real allocation rather than pointed at the mapping the
    // way string BYTES are: the fields are written individually, so there's
    // no correctly-aligned BcHashSlotLayout sitting in the file to point at
    // -- and at one small struct per Map/Set call site this is
    // metadata-scale, nothing that grows with program data.
    u32 layout_fixup_count = bcio_mmap_read_bounded_count(&r);
    foreach_index(i, layout_fixup_count) {
      u32              const_slot = bcio_mmap_read_u32(&r);
      BcHashSlotLayout layout      = {0};
      layout.key_kind     = (BcMapKeyKind)bcio_mmap_read_u32(&r);
      layout.key_size     = bcio_mmap_read_u64(&r);
      layout.value_size   = bcio_mmap_read_u64(&r);
      layout.slot_size    = bcio_mmap_read_u64(&r);
      layout.slot_align   = bcio_mmap_read_u64(&r);
      layout.key_offset   = bcio_mmap_read_u64(&r);
      layout.value_offset = bcio_mmap_read_u64(&r);
      layout.state_offset = bcio_mmap_read_u64(&r);
      layout.is_set       = bcio_mmap_read_b32(&r);
      if (r.error || !consts || const_slot >= const_count) continue;
      BcHashSlotLayout* boxed = push_one(arena, BcHashSlotLayout);
      *boxed             = layout;
      consts[const_slot] = (i64)(intptr_t)boxed;
    }

    u32 host_call_count = bcio_mmap_read_bounded_count(&r);
    foreach_index(i, host_call_count) {
      u32     instr_idx = bcio_mmap_read_u32(&r);
      String8 name       = bcio_mmap_read_string(&r); // zero-copy -- only needed transiently, for
                                                          // the str8_match below, never stored
      if (r.error || !code || instr_idx >= code_count) continue;
      b32 found = false;
      u32 resolved_idx = 0;
      if (host_imports) {
        foreach_index(hi, dyn_count(host_imports->entries)) {
          if (str8_match(host_imports->entries[hi].name, name, 0)) { resolved_idx = (u32)hi; found = true; break; }
        }
      }
      // An unresolved name is usually an embedding bug -- an incomplete
      // host_imports table -- but it is also what a STALE cache looks like: a
      // program that renamed or removed a registered callback since this file
      // was written. Asserting aborts the process on that, so return instead
      // and let the caller recompile from source, the same fallback a
      // magic/version mismatch takes. The caller cannot rule this out ahead of
      // the call by comparing content_hash: that is only read here, never
      // compared, so nothing judges staleness until this returns.
      if (!found) { file_unmap(&mapped); return result; }
      code[instr_idx].b = resolved_idx; // patched in place, in the writable mapping
    }

    chunks[ci] = chunk;
  }

  // The program-level host-import SIGNATURE table, written after the chunk
  // loop in bc_program_save: cross-checks this run's `host_imports` against
  // what the file recorded at save time. Nothing else re-verifies host import
  // TYPES for a cached program, since bc_verify_host_imports only runs against
  // a live TypedAst and a load exists precisely to skip building one.
  u32                    sig_count = bcio_mmap_read_bounded_count(&r);
  BcCachedHostImportSig* sigs       = push_array(arena, BcCachedHostImportSig, sig_count); // metadata-scale
  foreach_index(i, sig_count) {
    BcCachedHostImportSig sig = {0};
    sig.name         = bcio_mmap_read_string(&r); // zero-copy
    sig.kind         = (BcHostImportKind)bcio_mmap_read_u32(&r);
    sig.param_count  = bcio_mmap_read_bounded_count(&r);
    if (sig.param_count > 0) {
      sig.param_types = push_array(arena, TypeRef, sig.param_count);
      foreach_index(pi, sig.param_count) sig.param_types[pi] = bcio_mmap_read_type_ref(&r, arena);
    }
    sig.return_type = bcio_mmap_read_type_ref(&r, arena);
    sigs[i] = sig;
  }

  if (!r.error && sig_count > 0) {
    BcHostSignatureMismatch* mismatches = NULL;
    if (!bc_verify_cached_host_imports(sigs, sig_count, host_imports, arena, &mismatches)) {
      fprintf(stderr, "bc_program_load: host import signature mismatch(es) against the cache file:\n");
      foreach_index(i, dyn_count(mismatches)) {
        fprintf(stderr, "  %.*s: %.*s\n", str8_varg(mismatches[i].name), str8_varg(mismatches[i].reason));
      }
      xassert(!"bc_program_load: at least one host import signature mismatch (see stderr) -- this is "
               "an embedding-program bug: the LOADING run's host_imports registration disagrees with "
               "what the cache file was originally compiled against");
    }
  }

  // Cross-package module imports -- rebuilt into a real BcModuleTable by
  // handing each saved qualified name to `module_resolver` (see
  // BcModuleResolveFn in bytecode.h). Appended in FILE ORDER, which is what
  // makes each BcOp_CallModule's baked-in operand still name the right entry
  // (see the matching write loop's own comment).
  //
  // A name that won't resolve -- no resolver supplied at all, an unknown
  // module, a function that no longer exists in it -- rejects the whole file
  // as "no usable cache", the same outcome a bad magic/version gets, rather
  // than handing back a program whose calls would dispatch through a hole.
  // The caller's fallback is to compile from source, which then reports any
  // real problem with a proper diagnostic.
  u32            module_import_count = bcio_mmap_read_bounded_count(&r);
  BcModuleTable* module_table         = NULL;
  if (!r.error && module_import_count > 0) {
    module_table = push_one_zero(arena, BcModuleTable);
    foreach_index(i, module_import_count) {
      String8    name     = bcio_mmap_read_string(&r); // zero-copy; bc_module_table_add stores it, so
                                                            // it stays valid exactly as long as the mapping
      BcProgram* mod_prog = NULL;
      u32        mod_fn   = 0;
      if (r.error || !module_resolver || !module_resolver->fn ||
          !module_resolver->fn(name, module_resolver->userdata, &mod_prog, &mod_fn)) {
        fprintf(stderr, "bc_program_load: cache file references module import '%.*s', which this run "
                         "can't resolve -- ignoring the cache and recompiling\n", str8_varg(name));
        file_unmap(&mapped);
        return (BcLoadResult){0};
      }
      bc_module_table_add(module_table, arena, name, mod_prog, mod_fn);
    }
  }

  if (!r.error && !bcio_verify_loaded_code(chunks, chunk_count)) {
    file_unmap(&mapped);
    return (BcLoadResult){0}; // structurally invalid -- caller recompiles, same as a bad magic
  }

  result.ok             = !r.error;
  result.program.chunks  = chunks;
  result.program.globals = globals;
  result.program.ok      = result.ok;
  result.program.module_table = module_table; // BEFORE `#init_globals` runs below -- a global's own
                                                  // initializer is ordinary compiled code and can
                                                  // perfectly well contain a cross-module call
  result.mapped_file     = mapped;

  // Run `#init_globals` once here, mirroring bc_compile_program's own
  // post-compile step: a global must be initialized before anything, this
  // function's caller included, can observe it. Every loadable file was
  // written by bc_program_save from a bc_compile_program output, which always
  // includes that chunk even when empty, so this needs no does-it-exist check.
  if (result.ok) {
    u32 init_idx = bc_program_find_fn(&result.program, str8_lit("#init_globals"));
    bc_run_in_program(&result.program, init_idx, NULL, 0, heap, host_imports);
  }

  return result;
}

void
bc_program_unload(BcLoadResult* result) {
  if (result) file_unmap(&result->mapped_file);
}
