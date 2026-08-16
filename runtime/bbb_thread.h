////////////////////////////////
//~ OS Threading (pthread-backed) -- the minimum needed to run `parallel`
//~ blocks: no thread_join/detach or mutex/cond_var/barrier release, since
//~ the lane pool is started once (lazily, on first use -- see
//~ bbb_async_threads_init below) and lives for the rest of the process.

typedef void bbb_ThreadFn(void* p);
typedef void bbb_AsyncPhaseFn(void);

typedef struct bbb_Mutex   { u64 v[1]; } bbb_Mutex;
typedef struct bbb_CondVar { u64 v[1]; } bbb_CondVar;
typedef struct bbb_Barrier { u64 v[1]; } bbb_Barrier;
typedef struct bbb_Thread  { u64 v[1]; } bbb_Thread;

bbb_Thread  bbb_thread_launch(bbb_ThreadFn* func, void* ptr);
bbb_Mutex   bbb_mutex_alloc(void);
void        bbb_mutex_take(bbb_Mutex mutex);
void        bbb_mutex_drop(bbb_Mutex mutex);
bbb_CondVar bbb_cond_var_alloc(void);
b32         bbb_cond_var_wait(bbb_CondVar cv, bbb_Mutex mutex, u64 duration_ns);
void        bbb_cond_var_broadcast(bbb_CondVar cv);
bbb_Barrier bbb_barrier_alloc(u32 count);
void        bbb_barrier_wait(bbb_Barrier barrier);
u32         bbb_os_get_core_count(void);

#define bbb_Second (1000000000LL) // nanoseconds -- bbb_cond_var_wait's duration_ns unit

// bbb_async_threads_init itself is `static` (internal to the concatenated
// 3b_runtime.c) -- it's lazily triggered by bbb_async_run_phase/
// bbb_lane_count, never called directly by generated code, so it has no
// business being in this header.
u64         bbb_async_run_phase(bbb_AsyncPhaseFn* func);
void        bbb_async_phase_wait(u64 generation);

// Set by codegen just before bbb_async_run_phase, read back by the
// generated trampoline function every `parallel` block compiles down to --
// see cg_parallel_expr's comment in codegen.c. Only one phase is ever
// active at a time (bbb_async_run_phase asserts this), so one shared slot
// suffices.
extern void* bbb_g_3b_lane_job;
