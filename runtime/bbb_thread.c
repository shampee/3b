////////////////////////////////
//~ OS Threading -- see the matching comment in bbb_thread.h. `entity`
//~ below is a tiny malloc'd wrapper around the real OS synchronization
//~ object; never freed -- every entity this runtime ever allocates lives
//~ exactly once, at bbb_async_threads_init time, for the rest of the
//~ process.

#if defined(BBB_OS_WINDOWS)

typedef struct OS_Entity {
  union {
    struct { HANDLE handle; bbb_ThreadFn* func; void* ptr; } thread;
    SRWLOCK            mutex_handle;
    CONDITION_VARIABLE cond_handle;
    // Hand-rolled, same sense-reversing-barrier algorithm as the POSIX
    // side below -- Windows has no native barrier primitive either
    // (nothing before the Win8 CreateBarrier family, which this doesn't
    // depend on). Built from SRWLOCK+CONDITION_VARIABLE instead.
    struct {
      SRWLOCK            mutex;
      CONDITION_VARIABLE cond;
      u32                count;
      u32                waiting;
      u32                generation;
    } barrier;
  };
} OS_Entity;

static OS_Entity*
os_entity_alloc(void) {
  OS_Entity* entity = (OS_Entity*)malloc(sizeof(OS_Entity));
  bbb_MemoryZeroStruct(entity);
  return entity;
}

static DWORD WINAPI
os_thread_entry_point(LPVOID ptr) {
  OS_Entity* entity = (OS_Entity*)ptr;
  entity->thread.func(entity->thread.ptr);
  return 0;
}

bbb_Thread
bbb_thread_launch(bbb_ThreadFn* func, void* ptr) {
  OS_Entity* entity   = os_entity_alloc();
  entity->thread.func = func;
  entity->thread.ptr  = ptr;
  // Unconditional, not bbb_xassert -- see the matching comment on the
  // POSIX side below.
  entity->thread.handle = CreateThread(NULL, 0, os_thread_entry_point, entity, 0, NULL);
  if (!entity->thread.handle) {
    fprintf(stderr, "3b: CreateThread failed -- out of threads or system resources\n");
    exit(1);
  }
  return (bbb_Thread){ (u64)entity };
}

bbb_Mutex
bbb_mutex_alloc(void) {
  OS_Entity* entity = os_entity_alloc();
  InitializeSRWLock(&entity->mutex_handle);
  return (bbb_Mutex){ (u64)entity };
}

void bbb_mutex_take(bbb_Mutex mutex) { AcquireSRWLockExclusive(&((OS_Entity*)mutex.v[0])->mutex_handle); }
void bbb_mutex_drop(bbb_Mutex mutex) { ReleaseSRWLockExclusive(&((OS_Entity*)mutex.v[0])->mutex_handle); }

bbb_CondVar
bbb_cond_var_alloc(void) {
  OS_Entity* entity = os_entity_alloc();
  InitializeConditionVariable(&entity->cond_handle);
  return (bbb_CondVar){ (u64)entity };
}

b32
bbb_cond_var_wait(bbb_CondVar cv, bbb_Mutex mutex, u64 duration_ns) {
  OS_Entity* cv_entity    = (OS_Entity*)cv.v[0];
  OS_Entity* mutex_entity = (OS_Entity*)mutex.v[0];
  DWORD      timeout_ms   = (DWORD)(duration_ns / 1000000ull);
  BOOL ok = SleepConditionVariableSRW(&cv_entity->cond_handle, &mutex_entity->mutex_handle, timeout_ms, 0);
  return ok != 0;
}

void
bbb_cond_var_broadcast(bbb_CondVar cv) {
  WakeAllConditionVariable(&((OS_Entity*)cv.v[0])->cond_handle);
}

bbb_Barrier
bbb_barrier_alloc(u32 count) {
  OS_Entity* entity = os_entity_alloc();
  InitializeSRWLock(&entity->barrier.mutex);
  InitializeConditionVariable(&entity->barrier.cond);
  entity->barrier.count = count;
  return (bbb_Barrier){ (u64)entity };
}

// Sense/generation-reversing barrier -- see this OS_Entity's own comment
// on why this isn't a native primitive. The LAST arrival resets `waiting`
// and bumps `generation`, waking every earlier arrival's condvar wait;
// each of those re-checks against the generation it originally observed,
// so a spurious wakeup (or a thread immediately reusing this same
// barrier for its NEXT wait) can never let it fall through early.
void
bbb_barrier_wait(bbb_Barrier barrier) {
  OS_Entity* entity = (OS_Entity*)barrier.v[0];
  AcquireSRWLockExclusive(&entity->barrier.mutex);
  u32 gen = entity->barrier.generation;
  entity->barrier.waiting += 1;
  if (entity->barrier.waiting == entity->barrier.count) {
    entity->barrier.waiting = 0;
    entity->barrier.generation += 1;
    WakeAllConditionVariable(&entity->barrier.cond);
  } else {
    while (entity->barrier.generation == gen) {
      SleepConditionVariableSRW(&entity->barrier.cond, &entity->barrier.mutex, INFINITE, 0);
    }
  }
  ReleaseSRWLockExclusive(&entity->barrier.mutex);
}

u32
bbb_os_get_core_count(void) {
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  return si.dwNumberOfProcessors > 0 ? (u32)si.dwNumberOfProcessors : 1;
}

#else // POSIX (Linux/Mac) -- pthread-backed

typedef struct OS_Entity {
  union {
    struct { pthread_t handle; bbb_ThreadFn* func; void* ptr; } thread;
    pthread_mutex_t   mutex_handle;
    pthread_cond_t    cond_handle;
    // Hand-rolled instead of pthread_barrier_t -- that type doesn't exist
    // on macOS (a real POSIX gap on Darwin). Built from mutex+condvar+a
    // generation counter instead, same sense-reversing-barrier pattern the
    // Lanes section below uses for g_3b_phase_generation/bbb_async_run_phase --
    // portable everywhere mutex+condvar are (i.e. everywhere).
    struct {
      pthread_mutex_t mutex;
      pthread_cond_t  cond;
      u32             count;
      u32             waiting;
      u32             generation;
    } barrier;
  };
} OS_Entity;

static OS_Entity*
os_entity_alloc(void) {
  OS_Entity* entity = (OS_Entity*)malloc(sizeof(OS_Entity));
  bbb_MemoryZeroStruct(entity);
  return entity;
}

static void*
os_thread_entry_point(void* ptr) {
  OS_Entity* entity = (OS_Entity*)ptr;
  entity->thread.func(entity->thread.ptr);
  return NULL;
}

bbb_Thread
bbb_thread_launch(bbb_ThreadFn* func, void* ptr) {
  OS_Entity* entity   = os_entity_alloc();
  entity->thread.func = func;
  entity->thread.ptr  = ptr;
  // Unconditional, not bbb_xassert -- a lost lane thread here would
  // otherwise leave bbb_async_phase_wait's done-count permanently short
  // with zero indication why, in EVERY build (bbb_xassert is a silent
  // no-op without -DXDEBUG, i.e. in --release builds).
  if (pthread_create(&entity->thread.handle, NULL, os_thread_entry_point, entity) != 0) {
    fprintf(stderr, "3b: pthread_create failed -- out of threads or system resources\n");
    exit(1);
  }
  return (bbb_Thread){ (u64)entity };
}

bbb_Mutex
bbb_mutex_alloc(void) {
  OS_Entity* entity = os_entity_alloc();
  pthread_mutex_init(&entity->mutex_handle, NULL);
  return (bbb_Mutex){ (u64)entity };
}

void bbb_mutex_take(bbb_Mutex mutex) { pthread_mutex_lock(&((OS_Entity*)mutex.v[0])->mutex_handle); }
void bbb_mutex_drop(bbb_Mutex mutex) { pthread_mutex_unlock(&((OS_Entity*)mutex.v[0])->mutex_handle); }

bbb_CondVar
bbb_cond_var_alloc(void) {
  OS_Entity* entity = os_entity_alloc();
  // Default attr (no pthread_condattr_setclock(CLOCK_MONOTONIC)) --
  // Apple's pthread has never implemented that function at all (another
  // real POSIX gap on Darwin, like pthread_barrier_t above), so
  // bbb_cond_var_wait below measures its deadline against CLOCK_REALTIME
  // (the default a plain pthread_cond_t times out against) instead.
  // Portable everywhere; the only real cost is a timeout that can drift
  // if the wall clock itself is stepped, which doesn't matter here --
  // every caller just retries on timeout (see async_threads_init/
  // lane_thread_entry below), it never needs a precise deadline.
  pthread_cond_init(&entity->cond_handle, NULL);
  return (bbb_CondVar){ (u64)entity };
}

b32
bbb_cond_var_wait(bbb_CondVar cv, bbb_Mutex mutex, u64 duration_ns) {
  OS_Entity* cv_entity    = (OS_Entity*)cv.v[0];
  OS_Entity* mutex_entity = (OS_Entity*)mutex.v[0];

  struct timespec endt;
  clock_gettime(CLOCK_REALTIME, &endt);
  endt.tv_sec  += (time_t)(duration_ns / bbb_Second);
  endt.tv_nsec += (long)(duration_ns % bbb_Second);
  if (endt.tv_nsec >= bbb_Second) { endt.tv_sec += 1; endt.tv_nsec -= bbb_Second; }

  i32 wait_result =
    pthread_cond_timedwait(&cv_entity->cond_handle, &mutex_entity->mutex_handle, &endt);
  return wait_result == 0;
}

void
bbb_cond_var_broadcast(bbb_CondVar cv) {
  pthread_cond_broadcast(&((OS_Entity*)cv.v[0])->cond_handle);
}

bbb_Barrier
bbb_barrier_alloc(u32 count) {
  OS_Entity* entity = os_entity_alloc();
  pthread_mutex_init(&entity->barrier.mutex, NULL);
  pthread_cond_init(&entity->barrier.cond, NULL);
  entity->barrier.count = count;
  return (bbb_Barrier){ (u64)entity };
}

// Sense/generation-reversing barrier -- see OS_Entity's own comment on why
// this isn't pthread_barrier_wait. The LAST arrival resets `waiting` and
// bumps `generation`, waking every earlier arrival's cond_wait; each of
// those re-checks against the generation it originally observed, so a
// spurious wakeup (or a thread immediately reusing this same barrier for
// its NEXT wait) can never let it fall through early.
void
bbb_barrier_wait(bbb_Barrier barrier) {
  OS_Entity* entity = (OS_Entity*)barrier.v[0];
  pthread_mutex_lock(&entity->barrier.mutex);
  u32 gen = entity->barrier.generation;
  entity->barrier.waiting += 1;
  if (entity->barrier.waiting == entity->barrier.count) {
    entity->barrier.waiting = 0;
    entity->barrier.generation += 1;
    pthread_cond_broadcast(&entity->barrier.cond);
  } else {
    while (entity->barrier.generation == gen) {
      pthread_cond_wait(&entity->barrier.cond, &entity->barrier.mutex);
    }
  }
  pthread_mutex_unlock(&entity->barrier.mutex);
}

u32
bbb_os_get_core_count(void) {
  long n = sysconf(_SC_NPROCESSORS_ONLN);
  return n > 0 ? (u32)n : 1;
}

#endif

////////////////////////////////
//~ Lanes -- fork-join over a fixed pool of `core_count - 1` OS threads,
//~ started once by bbb_async_threads_init (see cg_function_main) and reused
//~ for every `parallel` block in the program. Only one phase is ever
//~ active at a time -- bbb_async_run_phase asserts the previous one
//~ finished (the checker separately rejects nested `parallel` blocks so
//~ this never fires from valid generated code).

void* bbb_g_3b_lane_job = NULL;

// Guards lazy, run-exactly-once pool startup -- bbb_async_run_phase and
// bbb_lane_count (the two entry points that need the pool to exist, one
// starting a phase and one just reading its size -- see bbb_lane_count's
// own comment on why IT also needs this, not only bbb_async_run_phase)
// both call pthread_once on this before touching g_3b_lanes. pthread_once
// (not a hand-rolled atomic flag) specifically because it guarantees the
// init COMPLETES before any caller proceeds, not just that it started.
#if defined(BBB_OS_WINDOWS)
static INIT_ONCE g_3b_lanes_once = INIT_ONCE_STATIC_INIT;
#else
static pthread_once_t g_3b_lanes_once = PTHREAD_ONCE_INIT;
#endif

static bbb_CondVar g_3b_phase_start_cv;
static bbb_CondVar g_3b_phase_done_cv;
static bbb_Mutex   g_3b_phase_mutex;

static _Atomic b32                g_3b_async_exit          = 0;
static _Atomic(bbb_AsyncPhaseFn*) g_3b_phase_fn            = NULL;
static _Atomic u64                g_3b_phase_generation    = 0;
static _Atomic u64                g_3b_completed_generation = 0;
static _Atomic u32                g_3b_phase_done_count    = 0;

static struct {
  u32          lane_count;
  bbb_Barrier  barrier;
  bbb_Thread*  threads;
  bbb_LaneCtx* lane_ctxs;
} g_3b_lanes;

static void
lane_thread_entry(void* ptr) {
  bbb_LaneCtx lane = *(bbb_LaneCtx*)ptr;

  bbb_Context* ctx = (bbb_Context*)malloc(sizeof(bbb_Context));
  bbb_ctx_init(ctx, bbb_MB(4)); // also attaches ctx as this OS thread's bbb_tls_ctx
  ctx->lane = lane;

  u64 last_seen_generation = 0;
  for (;;) {
    bbb_mutex_take(g_3b_phase_mutex);
    while (atomic_load(&g_3b_phase_generation) == last_seen_generation
           && !atomic_load(&g_3b_async_exit)) {
      bbb_cond_var_wait(g_3b_phase_start_cv, g_3b_phase_mutex, bbb_Second);
    }
    bbb_mutex_drop(g_3b_phase_mutex);

    if (atomic_load(&g_3b_async_exit)) break;
    last_seen_generation = atomic_load(&g_3b_phase_generation);

    bbb_AsyncPhaseFn* func = atomic_load(&g_3b_phase_fn);
    if (func) func();

    u32 done = atomic_fetch_add(&g_3b_phase_done_count, 1) + 1;
    if (done == g_3b_lanes.lane_count) {
      bbb_mutex_take(g_3b_phase_mutex);
      atomic_store(&g_3b_completed_generation, last_seen_generation);
      bbb_cond_var_broadcast(g_3b_phase_done_cv);
      bbb_mutex_drop(g_3b_phase_mutex);
    }
  }
}

// `static` -- lazily triggered on first use by bbb_async_run_phase/
// bbb_lane_count below (see g_3b_lanes_once), never called directly by
// generated code.
static void
async_threads_init(void) {
  u32 cores = bbb_os_get_core_count();
  g_3b_lanes.lane_count = cores > 1 ? cores - 1 : 1;

  g_3b_phase_start_cv = bbb_cond_var_alloc();
  g_3b_phase_done_cv  = bbb_cond_var_alloc();
  g_3b_phase_mutex    = bbb_mutex_alloc();

  g_3b_lanes.barrier   = bbb_barrier_alloc(g_3b_lanes.lane_count);
  g_3b_lanes.threads   = (bbb_Thread*)malloc(sizeof(bbb_Thread) * g_3b_lanes.lane_count);
  g_3b_lanes.lane_ctxs = (bbb_LaneCtx*)malloc(sizeof(bbb_LaneCtx) * g_3b_lanes.lane_count);

  for (u32 i = 0; i < g_3b_lanes.lane_count; i += 1) {
    bbb_LaneCtx* lane = &g_3b_lanes.lane_ctxs[i];
    lane->lane_index  = i;
    lane->lane_count  = g_3b_lanes.lane_count;
    lane->barrier     = g_3b_lanes.barrier;
    g_3b_lanes.threads[i] = bbb_thread_launch(lane_thread_entry, lane);
  }
}

#if defined(BBB_OS_WINDOWS)
static BOOL CALLBACK
async_threads_init_once(PINIT_ONCE init_once, PVOID param, PVOID* context) {
  (void)init_once; (void)param; (void)context;
  async_threads_init();
  return TRUE;
}
#endif

u64
bbb_async_run_phase(bbb_AsyncPhaseFn* func) {
#if defined(BBB_OS_WINDOWS)
  InitOnceExecuteOnce(&g_3b_lanes_once, async_threads_init_once, NULL, NULL);
#else
  pthread_once(&g_3b_lanes_once, async_threads_init);
#endif
  u64 prev_gen = atomic_load(&g_3b_phase_generation);
  bbb_xassert(atomic_load(&g_3b_completed_generation) == prev_gen
          && "bbb_async_run_phase called before the previous phase completed");

  atomic_store(&g_3b_phase_fn, func);
  atomic_store(&g_3b_phase_done_count, 0);

  u64 new_gen = prev_gen + 1;
  atomic_store(&g_3b_phase_generation, new_gen);

  bbb_mutex_take(g_3b_phase_mutex);
  bbb_cond_var_broadcast(g_3b_phase_start_cv);
  bbb_mutex_drop(g_3b_phase_mutex);

  return new_gen;
}

void
bbb_async_phase_wait(u64 generation) {
  bbb_mutex_take(g_3b_phase_mutex);
  while (atomic_load(&g_3b_completed_generation) < generation) {
    bbb_cond_var_wait(g_3b_phase_done_cv, g_3b_phase_mutex, bbb_Second);
  }
  bbb_mutex_drop(g_3b_phase_mutex);
}

u32
bbb_lane_idx(void) {
  bbb_xassert(bbb_tls_ctx && bbb_tls_ctx->lane.lane_count > 0
          && "bbb_lane_idx() called outside of a `parallel` block");
  return bbb_tls_ctx->lane.lane_index;
}

u32
bbb_lane_count(void) {
  // Unlike bbb_lane_idx()/bbb_lane_sync(), this doesn't depend on running
  // ON a lane thread -- it's a fixed, process-wide number -- so it's safe
  // (and useful) to call from the main thread too, e.g. to size a
  // `push`-allocated output array BEFORE forking a `parallel` block. That
  // means it can be the very FIRST lane-related call a program makes, so
  // it triggers the same lazy pool startup bbb_async_run_phase does,
  // rather than assuming a `parallel` block already ran one first.
#if defined(BBB_OS_WINDOWS)
  InitOnceExecuteOnce(&g_3b_lanes_once, async_threads_init_once, NULL, NULL);
#else
  pthread_once(&g_3b_lanes_once, async_threads_init);
#endif
  return g_3b_lanes.lane_count;
}

void
bbb_lane_sync(void) {
  bbb_xassert(bbb_tls_ctx && bbb_tls_ctx->lane.lane_count > 0
          && "bbb_lane_sync() called outside of a `parallel` block");
  bbb_barrier_wait(bbb_tls_ctx->lane.barrier);
}

bbb_Rng1u64
bbb_lane_range(u64 work_count) {
  u64 lanes = bbb_lane_count();
  u64 idx   = bbb_lane_idx();

  u64 base = work_count / lanes;
  u64 rem  = work_count % lanes;

  u64 start = bbb_Min(idx * base + bbb_Min(idx, rem), work_count);
  u64 size  = base + (idx < rem);
  if (start + size > work_count) size = work_count - start;

  return (bbb_Rng1u64){ start, start + size };
}
