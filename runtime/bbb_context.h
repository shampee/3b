////////////////////////////////
//~ Context (per-thread arenas, plus this thread's lane identity/barrier
//~ when running inside a `parallel` block -- see bbb_LaneCtx)

typedef struct bbb_Rng1u64 { u64 min; u64 max; } bbb_Rng1u64;

typedef struct bbb_LaneCtx {
  u32         lane_index;
  u32         lane_count;
  bbb_Barrier barrier;
} bbb_LaneCtx;

typedef struct bbb_Context {
  bbb_Arena   scratch;
  bbb_Arena   perm;
  bbb_LaneCtx lane; // zeroed outside of a `parallel` block's trampoline fn
} bbb_Context;

void        bbb_ctx_init(bbb_Context* ctx, u64 scratch_size);
void        bbb_ctx_free(void);
bbb_Arena*  bbb_ctx_scratch(void);
bbb_Arena*  bbb_ctx_perm(void);

// bbb_lane_idx/bbb_lane_sync/bbb_lane_range only mean something on an
// active lane thread, so they're only valid inside a `parallel` block's
// body -- enforced by the checker (Checker.in_parallel_block), not by
// these functions themselves. bbb_lane_count is different: it's a fixed,
// process-wide number, safe to call from anywhere (including before
// forking, e.g. to size an output array by the real lane count).
u32         bbb_lane_idx(void);
u32         bbb_lane_count(void);
void        bbb_lane_sync(void);
bbb_Rng1u64 bbb_lane_range(u64 work_count);
