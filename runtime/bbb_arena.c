////////////////////////////////
//~ Arena (VM backend)

// Running past an arena's reservation used to print "overflow" (no newline, no
// numbers) and return NULL, which no caller checked -- bbb_arena_push_zero
// memset through it, bbb_dyn_push wrote a count through `NULL - 16`, and
// bbb_os_file_read/bbb_str8f fread and sprintf'd into it. Every one of those
// became a segfault at a near-NULL address, with nothing on screen naming the
// arena that ran out or the allocation that asked for too much.
//
// So this is where allocation failure is handled, once, for all of them. There
// is no recoverable alternative to offer: an arena hands back a raw pointer,
// and 3b has no error value or unwinding mechanism a runtime function could use
// to say "this did not work" to the program being run. Reporting and stopping
// is the honest version of what already happened, and the numbers are the whole
// point -- "wanted 8 MB out of a 16 MB arena with 15 MB gone" is a size to
// change, where a segfault in memset is a debugging session.
static void
bbb_arena_exhausted(u64 size, u64 align, u64 used, u64 reserved) {
  fflush(stdout); // this program's own output first, so the message lands after it
  fprintf(stderr,
          "3b runtime: arena out of memory -- wanted %llu byte(s) (align %llu), "
          "%llu of %llu byte(s) already used\n",
          (unsigned long long)size, (unsigned long long)align,
          (unsigned long long)used, (unsigned long long)reserved);
  abort();
}

#if defined(BBB_OS_WINDOWS)

static void*
bbb_arena_vm_push_op(void* backend, u64 size, u64 align) {
  bbb_ArenaVMBackend* arena = (bbb_ArenaVMBackend*)backend;
  bbb_xassert(bbb_IsPow2(align) && "alignment must be power-of-two");

  u8* result = (u8*)bbb_AlignPow2((u64)arena->at, align);
  u8* next   = result + size;

  if (next > arena->base + arena->reserve_size) {
    bbb_arena_exhausted(size, align, (u64)(arena->at - arena->base), arena->reserve_size);
  }

  if (next > arena->commit) {
    u64 offset      = bbb_AlignPow2((u64)(next - arena->base), arena->page_size);
    u8* new_commit  = arena->base + offset;
    u64 commit_size = new_commit - arena->commit;
    bbb_xassert(new_commit <= arena->base + arena->reserve_size);
    bbb_xassert(VirtualAlloc(arena->commit, commit_size, MEM_COMMIT, PAGE_READWRITE) != NULL
           && "failed to commit pages");
    arena->commit = new_commit;
  }

  arena->at = next;
  return result;
}

static void
bbb_arena_vm_release_op(void* backend) {
  bbb_ArenaVMBackend* arena = (bbb_ArenaVMBackend*)backend;
  if (arena->commit > arena->base) {
    u64 size = arena->commit - arena->base;
    VirtualFree(arena->base, size, MEM_DECOMMIT);
  }
  arena->commit = arena->base;
  arena->at     = arena->base;
}

static void
bbb_arena_vm_reset_op(void* backend) {
  bbb_ArenaVMBackend* arena = (bbb_ArenaVMBackend*)backend;
  arena->at                 = arena->base;
}

static void
bbb_arena_vm_destroy_op(void* backend) {
  bbb_ArenaVMBackend* arena = (bbb_ArenaVMBackend*)backend;
  VirtualFree(arena->base, 0, MEM_RELEASE); // size MUST be 0 with MEM_RELEASE -- releases the whole reservation
  free(arena);
}

static u8*  bbb_arena_vm_get_at_op(void* backend)          { return ((bbb_ArenaVMBackend*)backend)->at; }
static void bbb_arena_vm_set_at_op(void* backend, u8* at) {
  bbb_ArenaVMBackend* arena = (bbb_ArenaVMBackend*)backend;
  bbb_xassert(at >= arena->base && at <= arena->commit);
  arena->at = at;
}

static const bbb_ArenaOps bbb_arena_vm_ops = {
  bbb_arena_vm_push_op,
  bbb_arena_vm_reset_op,
  bbb_arena_vm_release_op,
  bbb_arena_vm_destroy_op,
  bbb_arena_vm_get_at_op,
  bbb_arena_vm_set_at_op,
};

bbb_Arena
bbb_arena_create_vm(u64 reserve_size) {
  bbb_xassert(reserve_size > 0 && "reserve size is 0");
  bbb_ArenaVMBackend* backend = (bbb_ArenaVMBackend*)calloc(1, sizeof(bbb_ArenaVMBackend));

  SYSTEM_INFO si;
  GetSystemInfo(&si);
  backend->page_size = si.dwPageSize;
  bbb_xassert(bbb_IsPow2(backend->page_size) && "page size isn't power-of-two aligned");

  backend->reserve_size =
    bbb_AlignPow2(reserve_size, backend->page_size) + backend->page_size;
  // MEM_RESERVE alone -- no PAGE_NOACCESS pages actually committed yet,
  // matching mmap(PROT_NONE)'s "reserve address space, commit lazily" role.
  backend->base = (u8*)VirtualAlloc(NULL, backend->reserve_size, MEM_RESERVE, PAGE_NOACCESS);
  bbb_xassert(backend->base != NULL && "VirtualAlloc reserve failed");

  backend->at     = backend->base;
  backend->commit = backend->base;

  bbb_Arena arena = { .backend = backend, .ops = &bbb_arena_vm_ops };
  return arena;
}

#else // POSIX (Linux/Mac) -- mmap-backed

static void*
bbb_arena_vm_push_op(void* backend, u64 size, u64 align) {
  bbb_ArenaVMBackend* arena = (bbb_ArenaVMBackend*)backend;
  bbb_xassert(bbb_IsPow2(align) && "alignment must be power-of-two");

  u8* result = (u8*)bbb_AlignPow2((u64)arena->at, align);
  u8* next   = result + size;

  if (next > arena->base + arena->reserve_size) {
    bbb_arena_exhausted(size, align, (u64)(arena->at - arena->base), arena->reserve_size);
  }

  if (next > arena->commit) {
    u64 offset       = bbb_AlignPow2((u64)(next - arena->base), arena->page_size);
    u8* new_commit   = arena->base + offset;
    u64 commit_size  = new_commit - arena->commit;
    bbb_xassert(new_commit <= arena->base + arena->reserve_size);
    bbb_xassert(mprotect(arena->commit, commit_size, PROT_READ | PROT_WRITE) == 0
           && "failed to change protection to read-write");
    arena->commit = new_commit;
  }

  arena->at = next;
  return result;
}

static void
bbb_arena_vm_release_op(void* backend) {
  bbb_ArenaVMBackend* arena = (bbb_ArenaVMBackend*)backend;
  if (arena->commit > arena->base) {
    u64 size = arena->commit - arena->base;
    mprotect(arena->base, size, PROT_NONE);
  }
  arena->commit = arena->base;
  arena->at     = arena->base;
}

static void
bbb_arena_vm_reset_op(void* backend) {
  bbb_ArenaVMBackend* arena = (bbb_ArenaVMBackend*)backend;
  arena->at                 = arena->base;
}

static void
bbb_arena_vm_destroy_op(void* backend) {
  bbb_ArenaVMBackend* arena = (bbb_ArenaVMBackend*)backend;
  munmap(arena->base, arena->reserve_size);
  free(arena);
}

static u8*  bbb_arena_vm_get_at_op(void* backend)          { return ((bbb_ArenaVMBackend*)backend)->at; }
static void bbb_arena_vm_set_at_op(void* backend, u8* at) {
  bbb_ArenaVMBackend* arena = (bbb_ArenaVMBackend*)backend;
  bbb_xassert(at >= arena->base && at <= arena->commit);
  arena->at = at;
}

static const bbb_ArenaOps bbb_arena_vm_ops = {
  bbb_arena_vm_push_op,
  bbb_arena_vm_reset_op,
  bbb_arena_vm_release_op,
  bbb_arena_vm_destroy_op,
  bbb_arena_vm_get_at_op,
  bbb_arena_vm_set_at_op,
};

bbb_Arena
bbb_arena_create_vm(u64 reserve_size) {
  bbb_xassert(reserve_size > 0 && "reserve size is 0");
  bbb_ArenaVMBackend* backend = (bbb_ArenaVMBackend*)calloc(1, sizeof(bbb_ArenaVMBackend));

  backend->page_size = sysconf(_SC_PAGESIZE);
  bbb_xassert(bbb_IsPow2(backend->page_size) && "page size isn't power-of-two aligned");

  backend->reserve_size =
    bbb_AlignPow2(reserve_size, backend->page_size) + backend->page_size;
  backend->base = (u8*)mmap(NULL, backend->reserve_size,
                              PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  bbb_xassert(backend->base != MAP_FAILED && "mmap failed");

  backend->at     = backend->base;
  backend->commit = backend->base;

  bbb_Arena arena = { .backend = backend, .ops = &bbb_arena_vm_ops };
  return arena;
}

#endif
