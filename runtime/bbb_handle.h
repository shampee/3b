////////////////////////////////
//~ Handles

typedef struct bbb_Handle {
  u32 index;
  u32 generation;
} bbb_Handle;

typedef struct bbb_HandlePool {
  void* data;        // raw T array
  u32*  generation;  // per-slot generation
  u32*  free_list;
  u32   free_count;
  u32   count;
  u32   capacity;
  u32   stride;      // sizeof(T)
} bbb_HandlePool;

#define bbb_DEFINE_HANDLE_POOL(T, Prefix)                                                          \
                                                                                                    \
  typedef bbb_Handle     T##Handle;                                                                \
  typedef bbb_HandlePool T##Pool;                                                                  \
                                                                                                    \
  static inline void Prefix##_pool_init(bbb_Arena* arena, T##Pool* pool, u32 capacity) {           \
    pool->data       = bbb_push_array_zero(arena, T, capacity);                                     \
    pool->generation = bbb_push_array_zero(arena, u32, capacity);                                   \
    pool->free_list  = bbb_push_array(arena, u32, capacity);                                        \
    pool->capacity   = capacity;                                                                    \
    pool->count      = 0;                                                                           \
    pool->stride     = sizeof(T);                                                                   \
    /* slot 0 reserved as null -- fill free list with 1..capacity-1 */                               \
    pool->free_count = capacity - 1;                                                                \
    for (u32 i = 0; i < capacity - 1; i++)                                                           \
      pool->free_list[i] = capacity - 1 - i; /* pop gives 1,2,3,... */                               \
    /* generation starts at 1 so first valid handle is never zero */                                 \
    for (u32 i = 1; i < capacity; i++) pool->generation[i] = 1;                                      \
  }                                                                                                  \
                                                                                                      \
  static inline T##Handle Prefix##_pool_alloc(T##Pool* pool, T* data) {                              \
    if (pool->free_count == 0) {                                                                     \
      fprintf(stderr, #Prefix "_pool_alloc: pool full\n");                                           \
      return (T##Handle){ 0 };                                                                       \
    }                                                                                                 \
    u32 idx = pool->free_list[--pool->free_count];                                                   \
    if (data) bbb_MemoryCopy((u8*)pool->data + idx * sizeof(T), data, sizeof(T));                    \
    pool->count++;                                                                                    \
    return (T##Handle){ idx, pool->generation[idx] };                                                 \
  }                                                                                                    \
                                                                                                        \
  static inline T* Prefix##_pool_get(T##Pool* pool, T##Handle handle) {                                \
    if (handle.index == 0 || handle.index >= pool->capacity) return NULL;                              \
    if (pool->generation[handle.index] != handle.generation) return NULL;                              \
    return (T*)((u8*)pool->data + handle.index * sizeof(T));                                          \
  }                                                                                                    \
                                                                                                        \
  static inline b32 Prefix##_pool_free(T##Pool* pool, T##Handle handle) {                              \
    if (handle.index == 0 || handle.index >= pool->capacity) return false;                             \
    if (pool->generation[handle.index] != handle.generation) return false;                             \
    bbb_MemoryZero((u8*)pool->data + handle.index * sizeof(T), sizeof(T));                             \
    pool->generation[handle.index]++; /* invalidate all existing handles */                            \
    pool->free_list[pool->free_count++] = handle.index;                                                \
    pool->count--;                                                                                     \
    return true;                                                                                       \
  }                                                                                                    \
                                                                                                        \
  static inline T##Handle Prefix##_handle_zero(void) { return (T##Handle){ 0 }; }                      \
                                                                                                        \
  /* NOT a liveness check -- only "is this not the all-zero sentinel                                  \
   * handle", regardless of whether its slot has since been freed. A copy                             \
   * of a handle taken before Prefix_pool_free() reports true here                                    \
   * forever, since this never touches the pool at all. 3b's own                                      \
   * `(handle-valid? h)` builtin does NOT call this -- it compiles to                                  \
   * Prefix_pool_get(pool, h) != NULL instead, the same real                                           \
   * index+generation check `handle-deref` uses. Kept only for raw C                                  \
   * callers that specifically want the cheap zero-check. */                                          \
  static inline b32 Prefix##_handle_valid(T##Handle handle) {                                          \
    return handle.index != 0 || handle.generation != 0;                                               \
  }
