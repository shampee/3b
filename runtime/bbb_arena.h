////////////////////////////////
//~ Arena (virtual-memory backend only -- the only backend codegen ever
//~ constructs, via bbb_arena_create_vm)

typedef struct bbb_ArenaOps {
  void *(*push)   (void* backend, u64 size, u64 align);
  void  (*reset)  (void* backend);
  void  (*release)(void* backend);
  void  (*destroy)(void* backend);
  u8   *(*get_at) (void* backend);
  void  (*set_at) (void* backend, u8* at);
} bbb_ArenaOps;

typedef struct bbb_ArenaVMBackend {
  u8* base;
  u8* commit;
  u8* at;
  u64 reserve_size;
  u64 page_size;
} bbb_ArenaVMBackend;

typedef struct bbb_ArenaMark {
  u8* at;
} bbb_ArenaMark;

typedef struct bbb_Arena {
  void*               backend;
  const bbb_ArenaOps* ops;
} bbb_Arena;

typedef struct bbb_ArenaTemp {
  bbb_Arena*    arena;
  bbb_ArenaMark mark;
} bbb_ArenaTemp;

static inline void* bbb_arena_push(bbb_Arena* arena, u64 size, u64 align) {
  return arena->ops->push(arena->backend, size, align);
}

static inline void bbb_arena_reset(bbb_Arena* arena) {
  arena->ops->reset(arena->backend);
}

static inline void bbb_arena_release(bbb_Arena* arena) {
  arena->ops->release(arena->backend);
}

// Cannot return NULL: the VM backend's push (bbb_arena_arena.c) reports and
// aborts on exhaustion rather than handing one back, precisely so that the
// dozens of sites like this one -- which write through the result immediately
// and have nowhere to report a failure to -- do not each need a check that
// would only turn a near-NULL crash into a silently empty value.
static inline void*
bbb_arena_push_zero(bbb_Arena* arena, u64 size, u64 align) {
  void* mem = arena->ops->push(arena->backend, size, align);
  bbb_MemoryZero(mem, size);
  return mem;
}

static inline void
bbb_arena_destroy(bbb_Arena* arena) {
  if (!arena || !arena->backend) return;
  bbb_xassert(arena->ops && "invalid arena");
  arena->ops->destroy(arena->backend);
  *arena = (bbb_Arena){0};
}

static inline bbb_ArenaMark
bbb_arena_mark(bbb_Arena* arena) {
  bbb_ArenaMark mark;
  mark.at = arena->ops->get_at(arena->backend);
  return mark;
}

static inline void
bbb_arena_pop(bbb_Arena* arena, bbb_ArenaMark mark) {
  arena->ops->set_at(arena->backend, mark.at);
}

bbb_Arena bbb_arena_create_vm(u64 reserve_size);

static inline void*
bbb_arena_realloc(bbb_Arena* arena, void* old, u64 old_size, u64 new_size, u64 align) {
  void* new_mem = bbb_arena_push(arena, new_size, align);
  if (!new_mem) return NULL;
  if (old && old_size) bbb_MemoryCopy(new_mem, old, bbb_Min(old_size, new_size));
  return new_mem;
}

#define bbb_push_one(a, T)            (T*)bbb_arena_push((a), sizeof(T), bbb_AlignOf(T))
#define bbb_push_one_zero(a, T)       (T*)bbb_arena_push_zero((a), sizeof(T), bbb_AlignOf(T))
#define bbb_push_array(a, T, c)       (T*)bbb_arena_push((a), sizeof(T) * (c), bbb_AlignOf(T))
#define bbb_push_array_zero(a, T, c)  (T*)bbb_arena_push_zero((a), sizeof(T) * (c), bbb_AlignOf(T))

static inline bbb_ArenaTemp
bbb_arena_temp_begin(bbb_Arena* arena) {
  bbb_xassert(arena);
  bbb_ArenaTemp temp;
  temp.arena = arena;
  temp.mark  = bbb_arena_mark(arena);
  return temp;
}

static inline void
bbb_arena_temp_end(bbb_ArenaTemp* temp) {
  if (!temp) return;
  bbb_arena_pop(temp->arena, temp->mark);
  temp->arena = NULL;
}

////////////////////////////////
//~ Dynamic Array (arena-backed)

typedef struct bbb_DynHdr {
  u64 count;
  u64 capacity;
} bbb_DynHdr;

#define bbb_dyn_hdr(p)      ((bbb_DynHdr*)((u8*)(p) - sizeof(bbb_DynHdr)))
#define bbb_dyn_count(p)    ((p) ? bbb_dyn_hdr(p)->count : 0)
#define bbb_dyn_capacity(p) ((p) ? bbb_dyn_hdr(p)->capacity : 0)

static inline void*
bbb_arena_dyn_grow(bbb_Arena* arena, void* ptr, u64 elem_size, u64 elem_align) {
  bbb_xassert(arena && "Arena is NULL");
  u64 old_cap = ptr ? bbb_dyn_capacity(ptr) : 0;
  u64 new_cap = old_cap ? old_cap * 2 : 8;

  u64         hdr_size  = sizeof(bbb_DynHdr);
  u64         old_bytes = hdr_size + old_cap * elem_size;
  u64         new_bytes = hdr_size + new_cap * elem_size;
  // The header sits in front of the elements, so the WHOLE allocation must
  // satisfy bbb_DynHdr's own alignment (8, for its two u64 fields)
  // regardless of what the elements themselves need -- passing bare
  // elem_align under-aligns whenever elem_align < bbb_AlignOf(bbb_DynHdr)
  // (e.g. a dyn array of u8/i8/bool/char), corrupting `new_hdr->capacity`'s
  // access below on any target that enforces alignment.
  u64         hdr_align = bbb_Max(elem_align, bbb_AlignOf(bbb_DynHdr));
  bbb_DynHdr* new_hdr =
    (bbb_DynHdr*)bbb_arena_realloc(arena, ptr ? bbb_dyn_hdr(ptr) : NULL, old_bytes, new_bytes, hdr_align);
  // Unreachable now that the VM backend's push aborts instead of returning NULL
  // (bbb_arena_exhausted). Kept because `push` is a function pointer in
  // bbb_ArenaOps and a backend that does return NULL would otherwise walk
  // straight into bbb_dyn_push writing a count through `NULL - sizeof(hdr)`.
  if (!new_hdr) return NULL;

  new_hdr->capacity = new_cap;
  return (u8*)new_hdr + hdr_size;
}

// NOTE: `arr` is parenthesized at its one bare, operator-adjacent use site
// below (`(arr)[_c]`) -- see base.h's own note on this same macro for why.
#define bbb_dyn_push(arena, arr, val)                                                              \
  do {                                                                                              \
    u64 _c   = bbb_dyn_count(arr);                                                                  \
    u64 _cap = bbb_dyn_capacity(arr);                                                                \
    if (_c >= _cap) {                                                                                \
      arr                     = bbb_arena_dyn_grow((arena), arr, sizeof(*(arr)), bbb_AlignOf(*(arr))); \
      bbb_dyn_hdr(arr)->count = _c;                                                                   \
    }                                                                                                 \
    (arr)[_c]               = (val);                                                                 \
    bbb_dyn_hdr(arr)->count = _c + 1;                                                                 \
  } while (0)
