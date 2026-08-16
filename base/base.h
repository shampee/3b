#ifndef BASE_H
#define BASE_H

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#if defined(_WIN32)
# define WIN32_LEAN_AND_MEAN
# include <windows.h>
#else
# include <sys/mman.h>
# include <unistd.h>
#endif


////////////////////////////////
//~ Compiler / OS / Architecture Detection

#if defined(__clang__)

# define COMPILER_CLANG 1

# if defined(_WIN32)
#  define OS_WINDOWS 1
# elif defined(__gnu_linux__) || defined(__linux__)
#  define OS_LINUX 1
# elif defined(__APPLE__) && defined(__MACH__)
#  define OS_MAC 1
# else
#  error This compiler/OS combo is not supported.
# endif

# if defined(__amd64__) || defined(__amd64) || defined(__x86_64__) || defined(__x86_64)
#  define ARCH_X64 1
# elif defined(i386) || defined(__i386) || defined(__i386__)
#  define ARCH_X86 1
# elif defined(__aarch64__)
#  define ARCH_ARM64 1
# elif defined(__arm__)
#  define ARCH_ARM32 1
# else
#  error Architecture not supported.
# endif

#elif defined(_MSC_VER)

# define COMPILER_MSVC 1

# if _MSC_VER >= 1920
#  define COMPILER_MSVC_YEAR 2019
# elif _MSC_VER >= 1910
#  define COMPILER_MSVC_YEAR 2017
# elif _MSC_VER >= 1900
#  define COMPILER_MSVC_YEAR 2015
# elif _MSC_VER >= 1800
#  define COMPILER_MSVC_YEAR 2013
# elif _MSC_VER >= 1700
#  define COMPILER_MSVC_YEAR 2012
# elif _MSC_VER >= 1600
#  define COMPILER_MSVC_YEAR 2010
# elif _MSC_VER >= 1500
#  define COMPILER_MSVC_YEAR 2008
# elif _MSC_VER >= 1400
#  define COMPILER_MSVC_YEAR 2005
# else
#  define COMPILER_MSVC_YEAR 0
# endif

# if defined(_WIN32)
#  define OS_WINDOWS 1
# else
#  error This compiler/OS combo is not supported.
# endif

# if defined(_M_AMD64)
#  define ARCH_X64 1
# elif defined(_M_IX86)
#  define ARCH_X86 1
# elif defined(_M_ARM64)
#  define ARCH_ARM64 1
# elif defined(_M_ARM)
#  define ARCH_ARM32 1
# else
#  error Architecture not supported.
# endif

#elif defined(__TINYC__)

# define COMPILER_TCC 1

# if defined(__gnu_linux__) || defined(__linux__)
#  define OS_LINUX 1
# elif defined(_WIN32)
#  define OS_WINDOWS 1
# else
#  error This compiler/OS combo is not supported.
# endif

# if defined(__amd64__) || defined(__amd64) || defined(__x86_64__) || defined(__x86_64)
#  define ARCH_X64 1
# elif defined(i386) || defined(__i386) || defined(__i386__)
#  define ARCH_X86 1
# elif defined(__aarch64__)
#  define ARCH_ARM64 1
# elif defined(__arm__)
#  define ARCH_ARM32 1
# else
#  error Architecture not supported.
# endif

#elif defined(__GNUC__) || defined(__GNUG__)

# define COMPILER_GCC 1

# if defined(__gnu_linux__) || defined(__linux__)
#  define OS_LINUX 1
# elif defined(__APPLE__) && defined(__MACH__)
#  define OS_MAC 1
# elif defined(_WIN32)
#  define OS_WINDOWS 1 // MinGW-w64 GCC -- defines __GNUC__ AND _WIN32
# else
#  error This compiler/OS combo is not supported.
# endif

# if defined(__amd64__) || defined(__amd64) || defined(__x86_64__) || defined(__x86_64)
#  define ARCH_X64 1
# elif defined(i386) || defined(__i386) || defined(__i386__)
#  define ARCH_X86 1
# elif defined(__aarch64__)
#  define ARCH_ARM64 1
# elif defined(__arm__)
#  define ARCH_ARM32 1
# else
#  error Architecture not supported.
# endif

#else
# error Compiler not supported.
#endif


#if defined(ARCH_X64)
# define ARCH_64BIT 1
#elif defined(ARCH_X86)
# define ARCH_32BIT 1
#endif

#if ARCH_ARM32 || ARCH_ARM64 || ARCH_X64 || ARCH_X86
# define ARCH_LITTLE_ENDIAN 1
#else
# error Endianness of this architecture not understood by context cracker.
#endif


////////////////////////////////
//~ Basic Types

typedef unsigned char    b8;
typedef unsigned short   b16;
typedef unsigned int     b32;
typedef unsigned long long b64;

typedef   signed char    i8;
typedef   signed short   i16;
typedef   signed int     i32;
typedef   signed long long i64;

typedef unsigned char    u8;
typedef unsigned short   u16;
typedef unsigned int     u32;
typedef unsigned long long u64;

// `long long`, not bare `long` -- `long` is only 32 bits under Windows'
// LLP64 data model (both MSVC and MinGW), unlike LP64 Linux/Mac where
// it's 64. `long long` is a full 64 bits on all three.
typedef   signed long long iptr;
typedef unsigned long long uptr;

typedef float            f32;
typedef double           f64;

typedef ptrdiff_t        isize;
typedef u64              usize;


////////////////////////////////
//~ Global Constants

extern u8  integer_symbol_reverse[128];
extern u8  integer_symbols[16];
extern u8  base64[64];
extern u8  base64_reverse[128];

extern u32 sign32;
extern u32 exponent32;
extern u32 mantissa32;
extern f32 big_golden32;
extern f32 small_golden32;
extern f32 pi32;
extern f64 machine_epsilon64;

extern u64 max_u64;
extern u32 max_u32;
extern u16 max_u16;
extern u8  max_u8;

extern i64 max_i64;
extern i32 max_i32;
extern i16 max_i16;
extern i8  max_i8;

extern i64 min_i64;
extern i32 min_i32;
extern i16 min_i16;
extern i8  min_i8;


////////////////////////////////
//~ Size Helpers

#define KB(n)  (((u64)(n)) << 10)
#define MB(n)  (((u64)(n)) << 20)
#define GB(n)  (((u64)(n)) << 30)
#define TB(n)  (((u64)(n)) << 40)


////////////////////////////////
//~ Memory Operations

// memmove/memcmp declare both pointers `nonnull` -- unconditionally, so a
// count of 0 does NOT excuse a null one, and UBSan's `nonnull-attribute`
// check fires on it. An empty String8 is `{0,0}`, so the copy/compare of one
// is exactly that call, and it is reached constantly (str8_list_join with
// no prefix, str8_match on two empty strings). These wrappers make a
// zero-length operation a no-op rather than UB; every Memory* macro below
// funnels through them.
static inline void*
memory_copy_(void* dst, const void* src, u64 size) {
  if (size == 0) return dst;
  return memmove(dst, src, size);
}

static inline int
memory_compare_(const void* a, const void* b, u64 size) {
  if (size == 0) return 0;
  return memcmp(a, b, size);
}

#define MemoryCopy(dst, src, size)   memory_copy_((dst), (src), (size))
#define MemorySet(dst, byte, size)   memset((dst), (byte), (size))
#define MemoryCompare(a, b, size)    memory_compare_((a), (b), (size))
#define MemoryStrlen(ptr)            strlen(ptr)

#define MemoryCopyStruct(d, s)       MemoryCopy((d), (s), sizeof(*(d)))
#define MemoryCopyArray(d, s)        MemoryCopy((d), (s), sizeof(d))
#define MemoryCopyTyped(d, s, c)     MemoryCopy((d), (s), sizeof(*(d)) * (c))
#define MemoryCopyStr8(dst, s)       MemoryCopy(dst, (s).str, (s).size)

#define MemoryZero(s, z)             memset((s), 0, (z))
#define MemoryZeroStruct(s)          MemoryZero((s), sizeof(*(s)))
#define MemoryZeroArray(a)           MemoryZero((a), sizeof(a))
#define MemoryZeroTyped(m, c)        MemoryZero((m), sizeof(*(m)) * (c))

#define MemoryMatch(a, b, z)         (MemoryCompare((a), (b), (z)) == 0)
#define MemoryMatchStruct(a, b)      MemoryMatch((a), (b), sizeof(*(a)))
#define MemoryMatchArray(a, b)       MemoryMatch((a), (b), sizeof(a))


////////////////////////////////
//~ Compiler Hints

#if defined(__clang__) || defined(__GNUC__) || defined(__GNUG__)
# define likely(x)   __builtin_expect(!!(x), 1)
# define unlikely(x) __builtin_expect(!!(x), 0)
#else
# define likely(x)   (x)
# define unlikely(x) (x)
#endif


////////////////////////////////
//~ Debug / Xassert

#if defined(_MSC_VER)
# define Trap() __debugbreak()
#elif defined(__clang__) || defined(__GNUC__) || defined(__GNUG__)
# define Trap() __builtin_trap()
#else
# include <signal.h>
# define Trap() raise(SIGTRAP)
#endif

#ifdef XDEBUG
# define xassert assert
#else
# define xassert (void)
#endif

#define AssertAlways(x)      do { if (!(x)) { Trap(); } } while (0)
#define StaticAssert(C, ID)  static u8 Glue(ID, __LINE__)[(C) ? 1 : -1]


////////////////////////////////
//~ Utility Macros

#define Min(A, B)         (((A) < (B)) ? (A) : (B))
#define Max(A, B)         (((A) > (B)) ? (A) : (B))
#define ClampTop(A, X)    Min(A, X)
#define ClampBot(X, B)    Max(X, B)
#define Clamp(A, X, B)    (((X) < (A)) ? (A) : ((X) > (B)) ? (B) : (X))
#define Clamp01(X)        Clamp(0, (X), 1)
#define Clamp01f(X)       Clamp(0.f, (X), 1.f)

#define Stringify_(S)     #S
#define Stringify(S)      Stringify_(S)

#define Glue_(A, B)       A##B
#define Glue(A, B)        Glue_(A, B)

#define ArrayCount(a)     (sizeof(a) / sizeof((a)[0]))

#define CeilIntegerDiv(a, b)    (((a) + (b) - 1) / (b))
#define Swap(T, a, b)           do { T t__ = a; a = b; b = t__; } while (0)

#if defined(_MSC_VER) || defined(__clang__)
# define AlignOf(T) __alignof(T)
#elif defined(__GNUC__) || defined(__GNUG__) || defined(__TINYC__)
# define AlignOf(T) __alignof__(T)
#else
# error AlignOf not defined for this compiler.
#endif

#define IntFromPtr(ptr)    ((u64)(ptr))
#define PtrFromInt(i)      ((void*)(i))

#define AlignPow2(x, b)      (((x) + (b) - 1) & (~((b) - 1)))
#define AlignDownPow2(x, b)  ((x) & (~((b) - 1)))
#define AlignPadPow2(x, b)   ((0 - (x)) & ((b) - 1))
#define IsPow2(x)            ((x) != 0 && ((x) & ((x) - 1)) == 0)
#define IsPow2OrZero(x)      ((((x) - 1) & (x)) == 0)

#define Member(T, m)                  (((T*)0)->m)
#define OffsetOf(T, m)                IntFromPtr(&Member(T, m))
#define MemberFromOffset(T, ptr, off) (T)((((u8*)(ptr)) + (off)))
#define CastFromMember(T, m, ptr)     (T*)(((u8*)(ptr)) - OffsetOf(T, m))
#define TypeOf(T)                     __typeof__(T)

////////////////////////////////
//~ Control Flow Helpers

#define defer(begin, end) \
  for (i32 _i_ = ((begin), 0); !_i_; _i_ += 1, (end))

#define defer_checked(begin, end) \
  for (i32 _i_ = 2 * !(begin); (_i_ == 2 ? ((end), 0) : !_i_); _i_ += 1, (end))


////////////////////////////////
//~ Iteration Macros

#define map_into(dst, src, n, fn)                                                                  \
  do {                                                                                             \
    for (u64 _i = 0; _i < n; _i++) (dst)[_i] = fn((src)[_i]);                                      \
  } while (0)

#define filter_inplace(T, arr, n_ptr, pred)                                                        \
  do {                                                                                             \
    u64 _w = 0;                                                                                    \
    for (u64 _i = 0; _i < *(n_ptr); _i++)                                                          \
      if (pred((arr)[_i])) (arr)[_w++] = (arr)[_i];                                                \
    *(n_ptr) = _w;                                                                                 \
  } while (0)

#define any(arr, n, pred)                                                                          \
  ({                                                                                               \
    i32 _r = 0;                                                                                    \
    for (u64 _i = 0; _i < (n) && !_r; _i++) _r = pred((arr)[_i]);                                  \
    _r;                                                                                            \
  })

#define all(arr, n, pred)                                                                          \
  ({                                                                                               \
    i32 _r = 1;                                                                                    \
    for (u64 _i = 0; _i < (n) && _r; _i++) _r = pred((arr)[_i]);                                   \
    _r;                                                                                            \
  })

#define find(T, arr, n, pred)                                                                      \
  ({                                                                                               \
    T* _r = NULL;                                                                                  \
    for (u64 _i = 0; _i < (n); _i++)                                                               \
      if (pred((arr)[_i])) {                                                                       \
        _r = &(arr)[_i];                                                                           \
        break;                                                                                     \
      }                                                                                            \
    _r;                                                                                            \
  })

#define foreach(T, v, arr, n)\
  for (T* v = (arr), *_end_ = (arr)+(n); v < _end_; v++)

#define foreach_index(it, count) \
  for (u64 it = 0; it < (count); it += 1)

#define foreach_nonzero_index(it, count) \
  for (u64 it = 1; it < (count); it += 1)

#define foreach_index_reverse(it, count) \
  for (u64 it = (count) - 1; it != (u64)-1; it -= 1)

#define foreach_element(it, array) \
  for (u64 it = 0; it < ArrayCount(array); it += 1)

#define foreach_enum(type, it) \
  for (type it = (type)0; it < type##_Count; it = (type)(it + 1))

#define foreach_nonzero_enum(type, it) \
  for (type it = (type)1; it < type##_Count; it = (type)(it + 1))

#define foreach_in_range(it, range) \
  for (i64 it = (range).min; it < (range).max; it += 1)

#define foreach_in_list(T, it, list) \
  for (T(it) = (list).first; (it); (it) = (it)->next)

#define foreach_in_list_reverse(T, it, list) \
  for (T(it) = (list).last; (it); (it) = (it)->prev)

#define foreach_in_list_ptr(T, it, list) \
  for (T(it) = (list)->first; (it); (it) = (it)->next)

#define foreach_in_list_ptr_reverse(T, it, list) \
  for (T(it) = (list)->last; (it); (it) = (it)->prev)

#define foreach_in_vector(it, vector) \
  for (u64 it = 0; it < (vector).count; it += 1)


////////////////////////////////
//~ Linked List Building Macros

//- Nil helpers
#define CheckNil(nil, p) ((p) == 0 || (p) == nil)
#define SetNil(nil, p)   ((p) = nil)

//- Doubly-linked lists (nil-terminated, parameterized)
#define DLLInsert_NPZ(nil, f, l, p, n, next, prev)                                                 \
  (CheckNil(nil, f)   ? ((f) = (l) = (n), SetNil(nil, (n)->next), SetNil(nil, (n)->prev))          \
   : CheckNil(nil, p) ? ((n)->next = (f), (f)->prev = (n), (f) = (n), SetNil(nil, (n)->prev))      \
   : ((p) == (l))                                                                                  \
     ? ((l)->next = (n), (n)->prev = (l), (l) = (n), SetNil(nil, (n)->next))                       \
     : (((!CheckNil(nil, p) && CheckNil(nil, (p)->next)) ? (0) : ((p)->next->prev = (n))),         \
        ((n)->next = (p)->next), ((p)->next = (n)), ((n)->prev = (p))))
#define DLLPushBack_NPZ(nil, f, l, n, next, prev)  DLLInsert_NPZ(nil, f, l, l, n, next, prev)
#define DLLPushFront_NPZ(nil, f, l, n, next, prev) DLLInsert_NPZ(nil, l, f, f, n, prev, next)
#define DLLRemove_NPZ(nil, f, l, n, next, prev)                                                    \
  (((n) == (f) ? (f) = (n)->next : (0)), ((n) == (l) ? (l) = (l)->prev : (0)),                     \
   (CheckNil(nil, (n)->prev) ? (0) : ((n)->prev->next = (n)->next)),                               \
   (CheckNil(nil, (n)->next) ? (0) : ((n)->next->prev = (n)->prev)))

//- Singly-linked, doubly-headed (queues)
#define SLLQueuePush_NZ(nil, f, l, n, next)                                                        \
  (CheckNil(nil, f) ? ((f) = (l) = (n), SetNil(nil, (n)->next))                                    \
                    : ((l)->next = (n), (l) = (n), SetNil(nil, (n)->next)))
#define SLLQueuePushFront_NZ(nil, f, l, n, next)                                                   \
  (CheckNil(nil, f) ? ((f) = (l) = (n), SetNil(nil, (n)->next)) : ((n)->next = (f), (f) = (n)))
#define SLLQueuePop_NZ(nil, f, l, next)                                                            \
  ((f) == (l) ? (SetNil(nil, f), SetNil(nil, l)) : ((f) = (f)->next))

//- Singly-linked, singly-headed (stacks)
#define SLLStackPush_N(f, n, next)  ((n)->next = (f), (f) = (n))
#define SLLStackPop_N(f, next)      ((f) = (f)->next)
#define SLLStackPush(f, n)  SLLStackPush_N(f, n, next)
#define SLLStackPop(f)      SLLStackPop_N(f, next)

//- Doubly-linked-list helpers (zero-nil)
#define DLLInsert_NP(f, l, p, n, next, prev)  DLLInsert_NPZ(0, f, l, p, n, next, prev)
#define DLLPushBack_NP(f, l, n, next, prev)   DLLPushBack_NPZ(0, f, l, n, next, prev)
#define DLLPushFront_NP(f, l, n, next, prev)  DLLPushFront_NPZ(0, f, l, n, next, prev)
#define DLLRemove_NP(f, l, n, next, prev)     DLLRemove_NPZ(0, f, l, n, next, prev)
#define DLLInsert(f, l, p, n)                 DLLInsert_NPZ(0, f, l, p, n, next, prev)
#define DLLPushBack(f, l, n)                  DLLPushBack_NPZ(0, f, l, n, next, prev)
#define DLLPushFront(f, l, n)                 DLLPushFront_NPZ(0, f, l, n, next, prev)
#define DLLRemove(f, l, n)                    DLLRemove_NPZ(0, f, l, n, next, prev)

//- Singly-linked, doubly-headed helpers (zero-nil)
#define SLLQueuePush_N(f, l, n, next)          SLLQueuePush_NZ(0, f, l, n, next)
#define SLLQueuePushFront_N(f, l, n, next)     SLLQueuePushFront_NZ(0, f, l, n, next)
#define SLLQueuePop_N(f, l, next)              SLLQueuePop_NZ(0, f, l, next)
#define SLLQueuePush(f, l, n)                  SLLQueuePush_NZ(0, f, l, n, next)
#define SLLQueuePushFront(f, l, n)             SLLQueuePushFront_NZ(0, f, l, n, next)
#define SLLQueuePop(f, l)                      SLLQueuePop_NZ(0, f, l, next)


////////////////////////////////
//~ Safe Casts

u16 safe_cast_u16(u32 x);
u32 safe_cast_u32(u64 x);
i32 safe_cast_i32(i64 x);


////////////////////////////////
//~ Arena

// A unified arena interface with two backends: "heap" (malloc-backed) and
// "virtual" (mmap-backed).

#ifndef ARENA_MINIMUM_BLOCK_SIZE
# define ARENA_MINIMUM_BLOCK_SIZE (1 << 10) // 1 KB
#endif

// Debug-only use-after-reset/use-after-pop aid: this language has no GC and
// no borrow checker by design, so a stale pointer into memory a `reset`/
// `pop` just invalidated is otherwise free to keep "working" -- it silently
// reads whatever bytes happen to still be sitting there, which can look
// perfectly plausible for a long time before it doesn't. Filling the
// about-to-be-invalidated range with a distinctive byte pattern before the
// cursor actually moves back turns that silent-stale-read into an obviously
// wrong value instead (not a hardware fault -- this is deliberately just a
// memset, so it stays cheap enough to leave on for every `reset` in a
// per-frame arena; VM-backed `release`, just below, already gets a MUCH
// stronger guarantee for free via mprotect(PROT_NONE)/VirtualFree(DECOMMIT),
// so it isn't poisoned here too). Compiles away to nothing outside XDEBUG
// builds, same convention as xassert -- a --release binary pays zero cost
// and gets zero help from this, on purpose.
#ifndef ARENA_POISON_BYTE
# define ARENA_POISON_BYTE 0xDD
#endif
#ifdef XDEBUG
# define ArenaPoison(ptr, size) (((size) > 0) ? (void)memset((ptr), ARENA_POISON_BYTE, (size)) : (void)0)
#else
# define ArenaPoison(ptr, size) ((void)0)
#endif

#ifndef ARENA_DEFAULT_MAX_FREE
# define ARENA_DEFAULT_MAX_FREE (4 << 20)   // 4 MB
#endif

typedef struct ArenaOps {
  void *(*push)   (void* backend, u64 size, u64 align);
  void  (*reset)  (void* backend);
  void  (*release)(void* backend);
  void  (*destroy)(void* backend);
  u8   *(*get_at) (void* backend);
  void  (*set_at) (void* backend, u8* at);
} ArenaOps;

typedef struct ArenaBlock ArenaBlock;
struct ArenaBlock {
  ArenaBlock* next;
  u64         size;
  u64         used;
  b32         is_owned;
  u8*         data;
};

typedef struct ArenaHeapBackend {
  ArenaBlock* first;
  ArenaBlock* current;
  u64         default_block_size;
  u64         max_free_bytes;
  b64         is_owned;
} ArenaHeapBackend;

typedef struct ArenaVMBackend {
  u8* base;
  u8* commit;
  u8* at;
  u64 reserve_size;
  u64 page_size;
} ArenaVMBackend;

typedef struct ArenaMark {
  u8* at;
} ArenaMark;

typedef struct Arena {
  void*           backend;
  const ArenaOps* ops;
} Arena;

typedef struct ArenaTemp {
  Arena*    arena;
  ArenaMark mark;
} ArenaTemp;

// PITFALL when nesting two ArenaTemps on the SAME arena (this is exactly
// what `ctx_scratch()` is -- one single shared per-thread arena, not a
// rotating conflict-avoiding pool -- so this comes up constantly): if you
// open temp A, then open temp B with NOTHING allocated in between, A and B's
// marks land at (or near) the exact same position. Ending B pops the arena
// back to that shared position -- which invalidates ANYTHING allocated on
// the arena since then, including data you pushed into A's own scratch
// array while B was open. The arena has no notion of "whose" data a byte
// range belongs to; it only knows positions. This bit `lower_fn` for real
// (2026-07-28): two scratch arrays with different lifetimes (one needed
// much further down the function, one only until its own flush a few lines
// later) were built via two separate ArenaTemps opened back-to-back --
// ending the shorter-lived one silently poisoned the longer-lived one's
// data too. Caught by ARENA_POISON_BYTE turning the corruption into an
// obviously-wrong value instead of a plausible-looking stale read.
//
// Safe patterns: (a) ONE ArenaTemp per function holding every scratch array
// that function itself needs (share it, don't open a second one for "a
// different array" with the same lifetime need); (b) a nested temp that
// flushes its own data to a PERMANENT arena and closes BEFORE the outer
// temp's own array is appended to again (this is the ordinary "scratch-
// then-flush" pattern used throughout this codebase, and it's fine because
// the outer array's memory sits entirely BELOW the inner temp's mark, so
// popping the inner temp can never touch it).

//- Arena interface
static inline void* arena_push(Arena* arena, u64 size, u64 align) {
  return arena->ops->push(arena->backend, size, align);
}

static inline void arena_reset(Arena* arena) {
  arena->ops->reset(arena->backend);
}

static inline void arena_release(Arena* arena) {
  arena->ops->release(arena->backend);
}

// Cannot return NULL: the VM backend's push reports and aborts on exhaustion
// rather than handing one back -- see arena_exhausted below for why that is the
// only honest option here.
static inline void* 
arena_push_zero(Arena* arena, u64 size, u64 align) {
  void* mem = arena->ops->push(arena->backend, size, align);
  MemoryZero(mem, size);
  return mem;
}

static inline void
arena_destroy(Arena* arena) {
  if (!arena || !arena->backend) return;
  xassert(arena->ops && "invalid arena");
  arena->ops->destroy(arena->backend);
  *arena = (Arena){0};
}

static inline ArenaMark
arena_mark(Arena* arena) {
  ArenaMark mark;
  mark.at = arena->ops->get_at(arena->backend);
  return mark;
}

static inline void
arena_pop(Arena* arena, ArenaMark mark) {
  arena->ops->set_at(arena->backend, mark.at);
}

// Running past an arena's reservation used to print "overflow" (no newline, no
// numbers) and return NULL, which no caller checked -- arena_push_zero memset
// through it, dyn_push wrote a count through `NULL - 16`, str8fv vsnprintf'd
// into it. Each became a segfault at a near-NULL address with nothing on screen
// naming the arena that ran out.
//
// So allocation failure is handled here, once, for all of them. There is no
// recoverable alternative worth offering: an arena hands back a raw pointer,
// and every caller in this codebase writes through it immediately. The numbers
// are the point -- "wanted 8 MB out of a 16 MB arena with 15 MB gone" names a
// size to change, where a crash inside memset is a debugging session.
//
// The runtime the compiler GENERATES has its own copy of this, deliberately
// (runtime/bbb_arena.c); the two are mirrors and should change together.
static void
arena_exhausted(u64 size, u64 align, u64 used, u64 reserved) {
  fflush(stdout);
  fprintf(stderr,
          "3b: arena out of memory -- wanted %llu byte(s) (align %llu), "
          "%llu of %llu byte(s) already used\n",
          (unsigned long long)size, (unsigned long long)align,
          (unsigned long long)used, (unsigned long long)reserved);
  abort();
}

//- VM backend ops
#if defined(_WIN32)

static void*
arena_vm_push_op(void* backend, u64 size, u64 align) {
  ArenaVMBackend* arena = (ArenaVMBackend*)backend;
  xassert(IsPow2(align) && "alignment must be power-of-two");

  u8* result = (u8*)AlignPow2((u64)arena->at, align);
  u8* next   = result + size;

  if (next > arena->base + arena->reserve_size) {
    arena_exhausted(size, align, (u64)(arena->at - arena->base), arena->reserve_size);
  }

  if (next > arena->commit) {
    u64 offset      = AlignPow2((u64)(next - arena->base), arena->page_size);
    u8* new_commit  = arena->base + offset;
    u64 commit_size = new_commit - arena->commit;
    xassert(new_commit <= arena->base + arena->reserve_size);
    xassert(VirtualAlloc(arena->commit, commit_size, MEM_COMMIT, PAGE_READWRITE) != NULL
           && "failed to commit pages");
    arena->commit = new_commit;
  }

  arena->at = next;
  return result;
}

static void
arena_vm_release_op(void* backend) {
  ArenaVMBackend* arena = (ArenaVMBackend*)backend;
  if (arena->commit > arena->base) {
    u64 size = arena->commit - arena->base;
    VirtualFree(arena->base, size, MEM_DECOMMIT);
  }
  arena->commit = arena->base;
  arena->at     = arena->base;
}

static void
arena_vm_reset_op(void* backend) {
  ArenaVMBackend* arena = (ArenaVMBackend*)backend;
  ArenaPoison(arena->base, (u64)(arena->at - arena->base));
  arena->at             = arena->base;
}

static void
arena_vm_destroy_op(void* backend) {
  ArenaVMBackend* arena = (ArenaVMBackend*)backend;
  VirtualFree(arena->base, 0, MEM_RELEASE); // size MUST be 0 with MEM_RELEASE -- releases the whole reservation
  free(arena);
}

static u8* arena_vm_get_at_op(void* backend)           { return ((ArenaVMBackend*)backend)->at; }
static void arena_vm_set_at_op(void* backend, u8* at) {
  ArenaVMBackend* arena = (ArenaVMBackend*)backend;
  xassert(at >= arena->base && at <= arena->commit);
  if (at < arena->at) ArenaPoison(at, (u64)(arena->at - at)); // popping BACKWARD invalidates [at, old at)
  arena->at = at;
}

static const ArenaOps arena_vm_ops = {
  arena_vm_push_op,
  arena_vm_reset_op,
  arena_vm_release_op,
  arena_vm_destroy_op,
  arena_vm_get_at_op,
  arena_vm_set_at_op,
};

static inline Arena
arena_create_vm(u64 reserve_size) {
  xassert(reserve_size > 0 && "reserve size is 0");
  ArenaVMBackend* backend = (ArenaVMBackend*)calloc(1, sizeof(ArenaVMBackend));

  SYSTEM_INFO si;
  GetSystemInfo(&si);
  backend->page_size = si.dwPageSize;
  xassert(IsPow2(backend->page_size) && "page size isn't power-of-two aligned");

  backend->reserve_size =
    AlignPow2(reserve_size, backend->page_size) + backend->page_size;
  backend->base = (u8*)VirtualAlloc(NULL, backend->reserve_size, MEM_RESERVE, PAGE_NOACCESS);
  xassert(backend->base != NULL && "VirtualAlloc reserve failed");

  backend->at     = backend->base;
  backend->commit = backend->base;

  Arena arena = { .backend = backend, .ops = &arena_vm_ops };
  return arena;
}

#else // POSIX (Linux/Mac) -- mmap-backed

static void*
arena_vm_push_op(void* backend, u64 size, u64 align) {
  ArenaVMBackend* arena = (ArenaVMBackend*)backend;
  xassert(IsPow2(align) && "alignment must be power-of-two");

  u8* result = (u8*)AlignPow2((u64)arena->at, align);
  u8* next   = result + size;

  if (next > arena->base + arena->reserve_size) {
    arena_exhausted(size, align, (u64)(arena->at - arena->base), arena->reserve_size);
  }

  if (next > arena->commit) {
    u64 offset     = AlignPow2((u64)(next - arena->base), arena->page_size);
    u8* new_commit = arena->base + offset;
    u64 size       = new_commit - arena->commit;
    xassert(new_commit <= arena->base + arena->reserve_size);
    xassert(mprotect(arena->commit, size, PROT_READ | PROT_WRITE) == 0
           && "failed to change protection to read-write");
    arena->commit = new_commit;
  }

  arena->at = next;
  return result;
}

static void
arena_vm_release_op(void* backend) {
  ArenaVMBackend* arena = (ArenaVMBackend*)backend;
  if (arena->commit > arena->base) {
    u64 size = arena->commit - arena->base;
    mprotect(arena->base, size, PROT_NONE);
  }
  arena->commit = arena->base;
  arena->at     = arena->base;
}

static void
arena_vm_reset_op(void* backend) {
  ArenaVMBackend* arena = (ArenaVMBackend*)backend;
  ArenaPoison(arena->base, (u64)(arena->at - arena->base));
  arena->at             = arena->base;
}

static void
arena_vm_destroy_op(void* backend) {
  ArenaVMBackend* arena = (ArenaVMBackend*)backend;
  munmap(arena->base, arena->reserve_size);
  free(arena);
}

static u8* arena_vm_get_at_op(void* backend)           { return ((ArenaVMBackend*)backend)->at; }
static void arena_vm_set_at_op(void* backend, u8* at) {
  ArenaVMBackend* arena = (ArenaVMBackend*)backend;
  xassert(at >= arena->base && at <= arena->commit);
  if (at < arena->at) ArenaPoison(at, (u64)(arena->at - at)); // popping BACKWARD invalidates [at, old at)
  arena->at = at;
}

static const ArenaOps arena_vm_ops = {
  arena_vm_push_op,
  arena_vm_reset_op,
  arena_vm_release_op,
  arena_vm_destroy_op,
  arena_vm_get_at_op,
  arena_vm_set_at_op,
};

static inline Arena
arena_create_vm(u64 reserve_size) {
  xassert(reserve_size > 0 && "reserve size is 0");
  ArenaVMBackend* backend = (ArenaVMBackend*)calloc(1, sizeof(ArenaVMBackend));

  backend->page_size = sysconf(_SC_PAGESIZE);
  xassert(IsPow2(backend->page_size) && "page size isn't power-of-two aligned");

  backend->reserve_size =
    AlignPow2(reserve_size, backend->page_size) + backend->page_size;
  backend->base = (u8*)mmap(NULL, backend->reserve_size,
                              PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  xassert(backend->base != MAP_FAILED && "mmap failed");

  backend->at     = backend->base;
  backend->commit = backend->base;

  Arena arena = { .backend = backend, .ops = &arena_vm_ops };
  return arena;
}

#endif

//- Heap backend ops
static ArenaBlock* 
arena_new_block(u64 min_size) {
  u64 block_size = Max(min_size, ARENA_MINIMUM_BLOCK_SIZE);
  u64 total_size = sizeof(ArenaBlock) + block_size + 15;

  ArenaBlock* block = (ArenaBlock*)malloc(total_size);
  if (!block) return NULL;

  u8* raw = (u8*)(block + 1);
  block->data     = (u8*)AlignPow2((uptr)raw, 16);
  block->next     = NULL;
  block->size     = block_size;
  block->used     = 0;
  block->is_owned = true;
  return block;
}

static void* 
arena_heap_push_op(void* backend, u64 size, u64 align) {
  xassert(backend && "Arena backend is NULL");
  xassert(IsPow2(align) && "alignment must be power-of-two");
  ArenaHeapBackend* arena = (ArenaHeapBackend*)backend;

  if (!arena->current) {
    arena->current = arena_new_block(arena->default_block_size);
    arena->first   = arena->current;
  }

  u64 aligned_used = AlignPow2(arena->current->used, align);
  u64 end          = aligned_used + size;

  if (end > arena->current->size) {
    u64         needed = Max(size + align, arena->default_block_size);
    ArenaBlock* block  = arena_new_block(needed);
    if (!block) return NULL;

    if (arena->current) arena->current->next = block;
    else                arena->first         = block;
    arena->current = block;

    aligned_used = 0;
    end          = size;
  }

  void* ptr            = arena->current->data + aligned_used;
  arena->current->used = end;
  xassert(((uptr)ptr & (align - 1)) == 0);
  return ptr;
}

static void
arena_heap_reset_op(void* backend) {
  xassert(backend && "Arena backend is NULL");
  ArenaHeapBackend* arena = (ArenaHeapBackend*)backend;
  for (ArenaBlock* block = arena->first; block; block = block->next) {
    ArenaPoison(block->data, block->used);
    block->used = 0;
  }
  arena->current = arena->first;
}

static void arena_heap_release_op(void* backend) { (void)backend; }

static void
arena_heap_destroy_op(void* backend) {
  xassert(backend && "Arena backend is NULL");
  ArenaHeapBackend* arena = (ArenaHeapBackend*)backend;
  ArenaBlock*       block = arena->first;
  while (block) {
    ArenaBlock* next = block->next;
    if (block->is_owned) free(block);
    block = next;
  }
  arena->first = arena->current = NULL;
  if (arena->is_owned) free(arena);
}

static u8* 
arena_heap_get_at_op(void* backend) {
  ArenaHeapBackend* arena = (ArenaHeapBackend*)backend;
  if (!arena->current) return NULL;
  return arena->current->data + arena->current->used;
}

// O(n) in number of blocks.
static void
arena_heap_set_at_op(void* backend, u8* at) {
  ArenaHeapBackend* arena = (ArenaHeapBackend*)backend;
  if (!at) { arena_heap_reset_op(arena); return; }

  for (ArenaBlock* block = arena->first; block; block = block->next) {
    u8* start = block->data;
    u8* end   = block->data + block->size;
    if (at >= start && at <= end) {
      u64 offset = (u64)(at - start);
      if (offset <= block->used) ArenaPoison(at, block->used - offset); // invalidate this block's rolled-back tail
      for (ArenaBlock* later = block->next; later; later = later->next) {
        ArenaPoison(later->data, later->used); // every block allocated entirely after the mark
        later->used = 0;
      }
      block->used    = offset;
      arena->current = block;
      return;
    }
  }
  xassert(0 && "invalid mark");
}

static const ArenaOps arena_heap_ops = {
  arena_heap_push_op,
  arena_heap_reset_op,
  arena_heap_release_op,
  arena_heap_destroy_op,
  arena_heap_get_at_op,
  arena_heap_set_at_op,
};

static inline Arena
arena_create_heap(u64 default_block_size) {
  ArenaHeapBackend* backend = (ArenaHeapBackend*)calloc(1, sizeof(ArenaHeapBackend));
  backend->first              = NULL;
  backend->current            = NULL;
  backend->default_block_size = Max(default_block_size, ARENA_MINIMUM_BLOCK_SIZE);
  backend->max_free_bytes     = ARENA_DEFAULT_MAX_FREE;
  backend->is_owned           = true;
  Arena arena = { .backend = backend, .ops = &arena_heap_ops };
  return arena;
}

static inline Arena
arena_create_heap_with_buffer(void* buffer, u64 size) {
  xassert(buffer);
  xassert(size > sizeof(ArenaHeapBackend) + sizeof(ArenaBlock) && "buffer too small");

  u8* ptr = (u8*)buffer;

  ArenaHeapBackend* backend = (ArenaHeapBackend*)ptr;
  ptr += sizeof(ArenaHeapBackend);
  ptr  = (u8*)AlignPow2((uptr)ptr, 16);

  ArenaBlock* block = (ArenaBlock*)ptr;
  ptr += sizeof(ArenaBlock);
  ptr  = (u8*)AlignPow2((uptr)ptr, 16);

  u8* data_start = ptr;
  u64 remaining = size - (u64)(data_start - (u8*)buffer);

  backend->default_block_size = Max(size, ARENA_MINIMUM_BLOCK_SIZE);
  backend->max_free_bytes     = ARENA_DEFAULT_MAX_FREE;
  backend->is_owned           = false;

  block->next     = NULL;
  block->size     = remaining;
  block->used     = 0;
  block->is_owned = false;
  block->data     = ptr;

  backend->first   = block;
  backend->current = block;

  Arena arena = { .backend = backend, .ops = &arena_heap_ops };
  return arena;
}

//- Arena allocation helpers
// NOTE: allocates new memory and copies; old memory is never freed. May break marks.
static inline void* 
arena_realloc(Arena* arena, void* old, u64 old_size, u64 new_size, u64 align) {
  void* new_mem = arena_push(arena, new_size, align);
  if (!new_mem) return NULL;
  if (old && old_size) MemoryCopy(new_mem, old, Min(old_size, new_size));
  return new_mem;
}

#define push_one(a, T)            (T*)arena_push((a), sizeof(T), AlignOf(T))
#define push_one_zero(a, T)       (T*)arena_push_zero((a), sizeof(T), AlignOf(T))
#define push_array(a, T, c)       (T*)arena_push((a), sizeof(T) * (c), AlignOf(T))
#define push_array_zero(a, T, c)  (T*)arena_push_zero((a), sizeof(T) * (c), AlignOf(T))
#define resize_array(a, old, oc, nc, T) \
  (T*)arena_realloc((a), (old), (oc) * sizeof(T), (nc) * sizeof(T), AlignOf(T))


////////////////////////////////
//~ Temporary Arenas

static inline ArenaTemp
arena_temp_begin(Arena* arena) {
  xassert(arena);
  ArenaTemp temp;
  temp.arena = arena;
  temp.mark  = arena_mark(arena);
  return temp;
}

////////////////////////////////
//~ Sub-arenas

static inline Arena
subarena(Arena* parent, u64 size) {
  xassert(parent && "parent arena is NULL");
  xassert(size > sizeof(ArenaHeapBackend) + sizeof(ArenaBlock) + 64);
  void* buffer = arena_push(parent, size, 16);
  xassert(buffer && "parent arena OOM");
  return arena_create_heap_with_buffer(buffer, size);
}

static inline void
arena_temp_end(ArenaTemp* temp) {
  if (!temp) return;
  arena_pop(temp->arena, temp->mark);
  temp->arena = NULL;
}

#define temp_scope(it, a) \
  for (ArenaTemp(it) = arena_temp_begin((a)); it.arena; arena_temp_end(&it))


////////////////////////////////
//~ Dynamic Array (arena-backed)

typedef struct DynHdr {
  u64 count;
  u64 capacity;
} DynHdr;

#define dyn_hdr(p)      ((DynHdr*)((u8*)(p) - sizeof(DynHdr)))
#define dyn_count(p)    ((p) ? dyn_hdr(p)->count : 0)
#define dyn_capacity(p) ((p) ? dyn_hdr(p)->capacity : 0)

static inline void*
arena_dyn_grow(Arena* arena, void* ptr, u64 elem_size, u64 elem_align) {
  xassert(arena && "Arena is NULL");
  u64 old_cap = ptr ? dyn_capacity(ptr) : 0;
  u64 new_cap = old_cap ? old_cap * 2 : 8;

  u64     hdr_size  = sizeof(DynHdr);
  u64     old_bytes = hdr_size + old_cap * elem_size;
  u64     new_bytes = hdr_size + new_cap * elem_size;
  // The header sits in front of the elements, so the WHOLE allocation must
  // satisfy DynHdr's own alignment (8, for its two u64 fields) regardless
  // of what the elements themselves need -- passing bare `elem_align`
  // under-aligns whenever elem_align < AlignOf(DynHdr) (e.g. a dyn array of
  // u8/i8/bool/char), corrupting `new_hdr->capacity`'s access below on any
  // target that enforces alignment (caught by UBSAN's misaligned-access
  // check even on x86, which merely tolerates it silently otherwise).
  u64     hdr_align = Max(elem_align, AlignOf(DynHdr));
  DynHdr* new_hdr =
    (DynHdr*)arena_realloc(arena, ptr ? dyn_hdr(ptr) : NULL, old_bytes, new_bytes, hdr_align);
  // Unreachable now that the VM backend's push aborts instead of returning NULL
  // (arena_exhausted). Kept because `push` is a function pointer in ArenaOps and
  // a backend that does return NULL would otherwise walk straight into
  // dyn_push writing a count through `NULL - sizeof(DynHdr)`.
  if (!new_hdr) return NULL;

  new_hdr->capacity = new_cap;
  return (u8*)new_hdr + hdr_size;
}

// NOTE: `arr` is parenthesized at its one bare, operator-adjacent use site
// below (`(arr)[_c]`) -- every OTHER use here is already safe, either
// because it's a macro-call argument (`dyn_count(arr)`, `dyn_hdr(arr)`,
// plain assignment target `arr = ...`) or already explicitly wrapped
// (`sizeof(*(arr))`). Without this, a caller passing a dereference
// expression for `arr` (e.g. `dyn_push(a, *some_double_ptr, val)`) gets
// silently miscompiled: `*some_double_ptr[_c]` parses as
// `*(some_double_ptr[_c])` (`[]` binds tighter than unary `*`), not the
// intended `(*some_double_ptr)[_c]` -- corrupting unrelated memory instead
// of writing into the array.
#define dyn_push(arena, arr, val)                                                                  \
  do {                                                                                             \
    u64 _c   = dyn_count(arr);                                                                     \
    u64 _cap = dyn_capacity(arr);                                                                  \
    if (_c >= _cap) {                                                                              \
      arr                 = arena_dyn_grow((arena), arr, sizeof(*(arr)), AlignOf(*(arr)));         \
      dyn_hdr(arr)->count = _c;                                                                    \
    }                                                                                              \
    (arr)[_c]           = (val);                                                                   \
    dyn_hdr(arr)->count = _c + 1;                                                                  \
  } while (0)

#define dyn_clear(arr)                                                                             \
  do {                                                                                             \
    dyn_hdr(arr)->count = 0;                                                                       \
  } while (0)

// Commit a temp-arena dynamic array to a permanent arena.
// Usage:
//   ArenaTemp t = arena_temp_begin(scratch);
//   i32* tmp = NULL;
//   dyn_push(t.arena, tmp, 2);
//   i32* result = dyn_commit_from_temp(main, &t, tmp, sizeof(i32), AlignOf(i32));
//   arena_temp_end(&t);
static inline void*
dyn_commit_from_temp(Arena* main, ArenaTemp* temp, void* arr, u64 elem_size, u64 elem_align) {
  if (!arr) {
    arena_temp_end(temp);
    return NULL;
  }

  u64     cnt     = dyn_count(arr);
  u64     hdr_sz  = sizeof(DynHdr);
  u64     total   = hdr_sz + cnt * elem_size;
  DynHdr* dst_hdr = (DynHdr*)arena_push(main, total, elem_align);
  if (!dst_hdr) {
    arena_temp_end(temp);
    return NULL;
  }

  dst_hdr->capacity = cnt;
  dst_hdr->count    = cnt;
  void* dst_data    = (u8*)dst_hdr + hdr_sz;
  MemoryCopy(dst_data, arr, cnt * elem_size);
  return dst_data;
}

#define dyn_commit_t(a, temp, arr, T) \
  (T*)dyn_commit_from_temp((a), (temp), (arr), sizeof(T), AlignOf(T))

#define dyn_commit_to_perm(temp, arr, sz) \
  dyn_commit_from_temp(ctx_perm(), (temp), (arr), (sz))

Arena* ctx_scratch(void);

////////////////////////////////
//~ Small Vector (inline buffer + arena spill)

#define DEFINE_SMALL_VECTOR(T, Prefix, InlineCap)                                                  \
  typedef struct SV##T {                                                                           \
    T*  items;                                                                                     \
    u64 count;                                                                                     \
    u64 capacity;                                                                                  \
    T   inline_buf[InlineCap];                                                                     \
  } SV##T;                                                                                         \
                                                                                                   \
  static inline void sv_##Prefix##_init(SV##T* v) {                                                \
    v->items    = v->inline_buf;                                                                   \
    v->count    = 0;                                                                               \
    v->capacity = InlineCap;                                                                       \
  }                                                                                                \
                                                                                                   \
  static inline i32 sv_##Prefix##_using_inline(SV##T* v) { return v->items == v->inline_buf; }     \
                                                                                                   \
  static inline T sv_##Prefix##_get(SV##T* v, u64 index) {                                         \
    if (index >= v->count) {                                                                       \
      return (T){ 0 };                                                                             \
    }                                                                                              \
    return v->items[index];                                                                        \
  }                                                                                                \
                                                                                                   \
  static inline void sv_##Prefix##_set(SV##T* v, u64 index, T val) {                               \
    if (index >= v->count) {                                                                       \
      return;                                                                                      \
    }                                                                                              \
    v->items[index] = val;                                                                         \
  }                                                                                                \
                                                                                                   \
  static inline void sv_##Prefix##_reserve(Arena* arena, SV##T* v, u64 needed) {                \
    if (v->capacity >= needed) return;                                                             \
    u64 new_cap = v->capacity * 2;                                                                 \
    if (new_cap < needed) new_cap = needed;                                                        \
    T* new_items = push_array(arena, T, new_cap);                                                  \
    for (u64 i = 0; i < v->count; i++) new_items[i] = v->items[i];                                 \
    v->items    = new_items;                                                                       \
    v->capacity = new_cap;                                                                         \
  }                                                                                                \
                                                                                                   \
  static inline void sv_##Prefix##_push(Arena* arena, SV##T* v, T value) {                      \
    if (v->count >= v->capacity) sv_##Prefix##_reserve(arena, v, v->count + 1);                 \
    v->items[v->count++] = value;                                                                  \
  }                                                                                                \
                                                                                                   \
  static inline T* sv_##Prefix##_push_ptr(Arena* arena, SV##T* v) {                             \
    if (v->count >= v->capacity) sv_##Prefix##_reserve(arena, v, v->count + 1);                 \
    return &v->items[v->count++];                                                                  \
  }                                                                                                \
                                                                                                   \
  static inline void sv_##Prefix##_reset(SV##T* v) { v->count = 0; }                               \
                                                                                                   \
  static inline void sv_##Prefix##_free(SV##T* v) {                                                \
    v->count    = 0;                                                                               \
    v->items    = v->inline_buf;                                                                   \
    v->capacity = InlineCap;                                                                       \
  }

////////////////////////////////
//~ View / ConstView

typedef struct View View;
typedef struct ConstView ConstView;
typedef struct ViewNode ViewNode;

struct View {
  void* data;
  u64 size;
};
struct ConstView {
  const void* data;
  u64 size;
};
struct ViewNode {
  ViewNode* prev;
  ViewNode* next;
  View view;
};

typedef struct ViewList {
  ViewNode* first;
  ViewNode* last;
  u64 node_count;
  u64 total_size;
} ViewList;

//- Conversion macros
#define str8_from_view(v)         str8(((u8*)(v).data), (v).size)
#define view_into_str8            str8_from_view
#define view_from_str8(s)         view((s).str, (s).size)
#define str8_into_view            view_from_str8

#define view_as(T, v)             ((T*)((v).data))
#define view_from_array(a)        view((a), (u64)sizeof(a))
#define const_view_from_array(a)  const_view((a), (u64)sizeof(a))
#define view_from_struct(s)       view(&(s), (u64)sizeof(s))
#define const_view_from_struct(s) const_view(&(s), (u64)sizeof(s))

//- Constructors
static inline View      view(void* data, u64 size)             { View v = { data, size }; return v; }
static inline ConstView const_view(const void* data, u64 size) { ConstView v = { data, size }; return v; }

//- End pointers
static inline void*       view_end(View v)            { return (u8*)v.data + v.size; }
static inline const void* const_view_end(ConstView v) { return (const u8*)v.data + v.size; }

//- Sub-views
static inline View
view_sub(View v, u64 offset, u64 size) {
  if (offset >= v.size) return view(view_end(v), 0);
  if (offset + size > v.size) size = v.size - offset;
  return view((u8*)v.data + offset, size);
}

static inline ConstView
const_view_sub(ConstView v, u64 offset, u64 size) {
  if (offset >= v.size) return const_view(const_view_end(v), 0);
  if (offset + size > v.size) size = v.size - offset;
  return const_view((const u8*)v.data + offset, size);
}

#define view_rng(v, r) view_sub((v), (r).min, (r).max - (r).min)

//- Prefix / postfix
static inline View      view_prefix(View v, u64 size)            { v.size = ClampTop(size, v.size); return v; }
static inline ConstView const_view_prefix(ConstView v, u64 size) { v.size = ClampTop(size, v.size); return v; }

static inline View
view_postfix(View v, u64 size) {
  size = ClampTop(size, v.size);
  v.data = ((u8*)v.data + v.size) - size;
  v.size = size;
  return v;
}

static inline ConstView
const_view_postfix(ConstView v, u64 size) {
  size = ClampTop(size, v.size);
  v.data = ((const u8*)v.data + v.size) - size;
  v.size = size;
  return v;
}

//- Fill / zero
static inline void
view_zero(View v) {
  if (v.size) MemoryZero(v.data, v.size);
}

static inline void
view_fill(View v, u8 val) {
  if (v.size) MemorySet(v.data, val, v.size);
}

//- Equality
static inline b32
view_equal(View a, View b) {
  if (a.size != b.size) return false;
  if (a.size == 0)      return true;
  return MemoryMatch(a.data, b.data, a.size);
}

static inline b32
const_view_equal(ConstView a, ConstView b) {
  if (a.size != b.size) return false;
  if (a.size == 0)      return true;
  return MemoryMatch(a.data, b.data, a.size);
}

//- Prefix / suffix checks
static inline b32
view_starts_with(View v, View beg) {
  return view_equal(beg, view_prefix(v, beg.size));
}

static inline b32
const_view_starts_with(ConstView v, ConstView beg) {
  return const_view_equal(beg, const_view_prefix(v, beg.size));
}

static inline b32
view_ends_with(View v, View end) {
  return view_equal(end, view_postfix(v, end.size));
}

static inline b32
const_view_ends_with(ConstView v, ConstView end) {
  return const_view_equal(end, const_view_postfix(v, end.size));
}

//- Overlap
static inline b32
view_overlap(View a, View b) {
  const u8* a0 = (const u8*)a.data,* a1 = a0 + a.size;
  const u8* b0 = (const u8*)b.data,* b1 = b0 + b.size;
  return (a0 < b1) && (b0 < a1);
}

static inline b32
const_view_overlap(ConstView a, ConstView b) {
  const u8* a0 = (const u8*)a.data,* a1 = a0 + a.size;
  const u8* b0 = (const u8*)b.data,* b1 = b0 + b.size;
  return (a0 < b1) && (b0 < a1);
}

//- Advance
static inline View
view_advance(View v, u64 amount) {
  if (amount >= v.size) return view(view_end(v), 0);
  return view((u8*)v.data + amount, v.size - amount);
}

static inline void
view_advance_in_place(View* v, u64 amount) {
  if (amount >= v->size) { v->data = (u8*)v->data + v->size; v->size = 0; }
  else                   { v->data = (u8*)v->data + amount;  v->size -= amount; }
}

static inline ConstView
const_view_advance(ConstView v, u64 amount) {
  if (amount >= v.size) return const_view(const_view_end(v), 0);
  return const_view((const u8*)v.data + amount, v.size - amount);
}

//- Debug dump
static inline void
view_dump_hex(View v) {
  const u8* p = (const u8*)v.data;
  foreach_index(i, v.size) {
    printf("%02X", p[i]);
    if ((i & 1)  == 1)  putchar(' ');
    if ((i & 15) == 15) putchar('\n');
  }
  if (v.size % 16) putchar('\n');
}

static inline void
const_view_dump_hex(ConstView v) {
  const u8* p = (const u8*)v.data;
  foreach_index(i, v.size) {
    printf("%02X", p[i]);
    if ((i & 1)  == 1)  putchar(' ');
    if ((i & 15) == 15) putchar('\n');
  }
  if (v.size % 16) putchar('\n');
}

//- Arena helpers
static inline View
view_alloc(Arena* arena, u64 size) {
  return view(arena_push(arena, size, 8), size);
}

static inline View
view_cat(Arena* arena, View v1, View v2) {
  View out; out.size = v1.size + v2.size; out.data = push_array(arena, u8, out.size);
  MemoryCopy(out.data, v1.data, v1.size);
  MemoryCopy((u8*)out.data + v1.size, v2.data, v2.size);
  return out;
}

static inline View
view_copy(Arena* arena, View v) {
  View out; out.size = v.size; out.data = push_array(arena, u8, out.size);
  MemoryCopy(out.data, v.data, v.size);
  return out;
}


////////////////////////////////
//~ ViewList

static inline ViewNode* 
view_list_push_node(ViewList* list, ViewNode* node) {
  DLLPushBack(list->first, list->last, node);
  list->node_count += 1;
  list->total_size += node->view.size;
  return node;
}

static inline ViewNode* 
view_list_push_node_set(ViewList* list, ViewNode* node, View v) {
  DLLPushBack(list->first, list->last, node);
  node->view = v;
  list->node_count += 1;
  list->total_size += v.size;
  return node;
}

static inline ViewNode* 
view_list_push_node_front(ViewList* list, ViewNode* node) {
  DLLPushFront(list->first, list->last, node);
  list->node_count += 1;
  list->total_size += node->view.size;
  return node;
}

static inline ViewNode* 
view_list_push_node_front_set(ViewList* list, ViewNode* node, View v) {
  DLLPushFront(list->first, list->last, node);
  node->view = v;
  list->node_count += 1;
  list->total_size += v.size;
  return node;
}

static inline ViewNode* 
view_list_push(Arena* arena, ViewList* list, View v) {
  ViewNode* node = push_one(arena, ViewNode);
  view_list_push_node_set(list, node, v);
  return node;
}

static inline ViewNode* 
view_list_push_front(Arena* arena, ViewList* list, View v) {
  ViewNode* node = push_one(arena, ViewNode);
  view_list_push_node_front_set(list, node, v);
  return node;
}

// NOTE: does not NULLify node
static inline void
view_list_remove(ViewList* list, ViewNode* node) {
  DLLRemove(list->first, list->last, node);
  list->node_count -= 1;
  list->total_size -= node->view.size;
}

static inline void
view_list_concat_in_place(ViewList* list, ViewList* to_push) {
  if (to_push->node_count == 0) return;
  if (list->last) {
    list->node_count += to_push->node_count;
    list->total_size += to_push->total_size;
    list->last->next  = to_push->first;
    list->last        = to_push->last;
  } else {
    *list =* to_push;
  }
  MemoryZeroStruct(to_push);
}

static inline ViewList
view_list_copy(Arena* arena, ViewList* list) {
  ViewList result = {0};
  foreach_in_list_ptr(ViewNode* , node, list) {
    ViewNode* new_node = push_one(arena, ViewNode);
    View      new_view = view_copy(arena, node->view);
    view_list_push_node_set(&result, new_node, new_view);
  }
  return result;
}


#define DEFINE_VIEW_LIST(T, Prefix)                                                                \
  typedef ViewList T##List;                                                                        \
  typedef struct T##Node {                                                                         \
    ViewNode base;                                                                                 \
  } T##Node;                                                                                       \
                                                                                                   \
  static inline T##Node* Prefix##_push(Arena* a, T##List* list, T* val) {                          \
    T##Node* n   = push_one(a, T##Node);                                                           \
    n->base.view = view(val, (u64)sizeof(T));                                                      \
    view_list_push_node((ViewList*)list, &n->base);                                                \
    return n;                                                                                      \
  }                                                                                                \
  static inline T##Node* Prefix##_push_front(Arena* a, T##List* list, T* val) {                    \
    T##Node* n   = push_one(a, T##Node);                                                           \
    n->base.view = view(val, (u64)sizeof(T));                                                      \
    view_list_push_node_front((ViewList*)list, &n->base);                                          \
    return n;                                                                                      \
  }                                                                                                \
  static inline void Prefix##_remove(T##List* list, T##Node* node) {                               \
    view_list_remove((ViewList*)list, &node->base);                                                \
  }                                                                                                \
  static inline T* Prefix##_first(T##List* list) {                                                 \
    return list->first ? (T*)list->first->view.data : NULL;                                        \
  }

#define DEFINE_INTRUSIVE_VIEW_LIST(T, Prefix, NodeField)                                           \
  typedef ViewList T##List;                                                                        \
                                                                                                   \
  static inline void Prefix##_push(T##List* list, T* obj) {                                        \
    obj->NodeField.view = view(obj, (u64)sizeof(T));                                               \
    DLLPushBack(list->first, list->last, &obj->NodeField);                                         \
    list->node_count += 1;                                                                         \
    list->total_size += sizeof(T);                                                                 \
  }                                                                                                \
  static inline void Prefix##_push_front(T##List* list, T* obj) {                                  \
    obj->NodeField.view = view(obj, (u64)sizeof(T));                                               \
    DLLPushFront(list->first, list->last, &obj->NodeField);                                        \
    list->node_count += 1;                                                                         \
    list->total_size += sizeof(T);                                                                 \
  }                                                                                                \
  static inline void Prefix##_remove(T##List* list, T* obj) {                                      \
    DLLRemove(list->first, list->last, &obj->NodeField);                                           \
    list->node_count -= 1;                                                                         \
    list->total_size -= sizeof(T);                                                                 \
  }                                                                                                \
  static inline T* Prefix##_first(T##List* list) {                                                 \
    return list->first ? CastFromMember(T, NodeField, list->first) : NULL;                         \
  }

////////////////////////////////
//~ Slice (typed strided view)

typedef struct Slice Slice;
struct Slice { View view; u64 stride; u64 count; };

static inline Slice slice_make(View v, u64 stride) {
  return (Slice){ .view = v, .stride = stride, .count = v.size / stride };
}

static inline void* slice_at(Slice s, u64 idx) { return (u8*)s.view.data + idx * s.stride; }

#define slice_at_safe(s, idx)       (xassert((idx) < (s).count), slice_at((s), (idx)))
#define slice_get(T, s, idx)        ((T*)slice_at((s), (idx)))
#define slice_get_safe(T, s, idx)   (xassert((idx) < (s).count), slice_get(T, (s), (idx)))

static inline Slice
slice_sub(Slice s, u64 start, u64 n) {
  if (start > s.count)        start = s.count;
  if (start + n > s.count)    n = s.count - start;
  return slice_make(view_sub(s.view, start * s.stride, n * s.stride), s.stride);
}

static inline Slice slice_front(Slice s, u64 n)         { return slice_sub(s, 0, n); }
static inline Slice slice_back(Slice s, u64 n)          { return slice_sub(s, s.count - n, n); }
static inline b32   slice_empty(Slice s)                { return s.count == 0; }
static inline void  slice_zero(Slice s)                 { view_zero(s.view); }
static inline Slice slice_cast(Slice s, u64 new_stride) { return slice_make(s.view, new_stride); }

#define slice_from_ptr(ptr, count) slice_make(view((ptr), sizeof(*(ptr)) * (count)), sizeof(*(ptr)))
#define slice_from_array(arr)      slice_make(view((arr), sizeof(arr)), sizeof((arr)[0]))
#define slice_from_struct(s)       slice_make(view_from_struct(s), sizeof(s))

#define foreach_in_slice(T, it, s)                                                                 \
  for (u64 _i = 0; _i < (s).count && ((it = (T*)slice_at(s, _i)), 1); ++_i)

static inline Slice
slice_alloc(Arena* arena, u64 size, u64 stride) {
  return slice_make(view_alloc(arena, size), stride);
}

// Flatten a ViewList into a contiguous arena-owned array.
static inline Slice
view_list_flatten(Arena* arena, ViewList* list, u64 stride) {
  u64  count = list->node_count;
  View v     = view_alloc(arena, count* stride);
  u8*  out   = (u8*)v.data;
  foreach_in_list_ptr(ViewNode* , node, list) {
    MemoryCopy(out, node->view.data, stride);
    out += stride;
  }
  return slice_make(v, stride);
}


////////////////////////////////
//~ Generic List (erased type, arena-backed)

typedef struct ListNode ListNode;
struct ListNode { ListNode* prev; ListNode* next; void* data; u64 size; };

typedef struct List {
  Arena*    arena;
  ListNode* first;
  ListNode* last;
  u64       node_count;
  u64       total_size;
} List;

#define DEFINE_LIST(T)                                                                             \
  typedef ListNode T##Node;                                                                        \
  typedef List     T##List;

static inline ConstView
list_node_view(ListNode* node) {
  return const_view(node->data, node->size);
}

static inline ListNode* 
list_push_node(List* list, ListNode* node) {
  if (!list || !node) return NULL;
  DLLPushBack(list->first, list->last, node);
  list->node_count += 1;
  list->total_size  += sizeof(ListNode) + node->size;
  return node;
}

static inline ListNode* 
list_push_node_set(List* list, ListNode* node, void* data, u64 size) {
  if (!list || !node || !data) return NULL;
  MemoryCopy(node->data, data, size);
  node->size = size;
  return list_push_node(list, node);
}

static inline ListNode* 
list_push_node_front(List* list, ListNode* node) {
  if (!list || !node) return NULL;
  DLLPushFront(list->first, list->last, node);
  list->node_count += 1;
  list->total_size  += sizeof(ListNode) + node->size;
  return node;
}

static inline ListNode* 
list_push_node_front_set(List* list, ListNode* node, void* data, u64 size) {
  if (!list || !node || !data) return NULL;
  MemoryCopy(node->data, data, size);
  node->size = size;
  return list_push_node_front(list, node);
}

static inline ListNode* 
list_push(List* list, void* data, u64 size) {
  if (!list || !data) return NULL;
  ListNode* node = push_one_zero(list->arena, ListNode);
  if (!node) return NULL;
  node->data = arena_push_zero(list->arena, size, 8);
  if (!node->data) return NULL;
  return list_push_node_set(list, node, data, size);
}

static inline ListNode* 
list_push_front(List* list, void* data, u64 size) {
  if (!list || !data) return NULL;
  ListNode* node = push_one_zero(list->arena, ListNode);
  if (!node) return NULL;
  node->data = arena_push_zero(list->arena, size, 8);
  if (!node->data) return NULL;
  return list_push_node_front_set(list, node, data, size);
}

static inline void
list_remove(List* list, ListNode* node) {
  if (!list || !node) return;
  DLLRemove(list->first, list->last, node);
  u64 total = sizeof(ListNode) + node->size;
  if (list->node_count)              list->node_count -= 1;
  if (list->total_size >= total)     list->total_size -= total;
  else                               list->total_size  = 0;
}

#define list_remove_if(list, node, cond)                                                           \
  do {                                                                                             \
    ListNode* node = (list)->first;                                                                \
    while (node) {                                                                                 \
      ListNode* next = node->next;                                                                 \
      if (cond) list_remove(list, node);                                                           \
      node = next;                                                                                 \
    }                                                                                              \
  } while (0)

static inline void
list_pop(List* list) {
  list_remove(list, list->last);
}

static inline void list_pop_front(List* list) { list_remove(list, list->first); }

static inline void
list_unique(List* list) {
  if (!list) return;
  ListNode* curr = list->first;
  while (curr && curr->next) {
    ListNode* next = curr->next;
    if (curr->size == next->size && MemoryMatch(curr->data, next->data, curr->size))
      list_remove(list, next);
    else
      curr = next;
  }
}

static inline void
list_concat_in_place(List* list, List* to_push) {
  if (to_push->node_count == 0) return;
  if (list->last) {
    list->last->next  = to_push->first;
    if (to_push->first) to_push->first->prev = list->last;
    list->last        = to_push->last;
    list->node_count += to_push->node_count;
    list->total_size += to_push->total_size;
  } else {
    list->first       = to_push->first;
    list->last        = to_push->last;
    list->node_count  = to_push->node_count;
    list->total_size  = to_push->total_size;
  }
  to_push->first = to_push->last = NULL;
  to_push->node_count = 0;
  to_push->total_size  = 0;
}

static inline void list_clear(List* list) { while (list->last) list_pop(list); }

static inline List
list_copy(Arena* arena, List* list) {
  List result = {0};
  if (!arena || !list) return result;
  result.arena = arena;
  foreach_in_list_ptr(ListNode* , node, list) {
    if (!list_push(&result, node->data, node->size)) {
      list_clear(&result);
      return result;
    }
  }
  return result;
}


////////////////////////////////
//~ Math Types

//- Vec2
typedef union Vec2f32 Vec2f32;
union Vec2f32 {
  struct { f32 x, y; };
  struct { f32 width, height; };
  f32 v[2];
};
typedef Vec2f32 Vec2;

typedef union Vec2i64 Vec2i64;
union Vec2i64 { struct { i64 x, y; }; i64 v[2]; };

typedef union Vec2i32 Vec2i32;
union Vec2i32 { struct { i32 x, y; }; i32 v[2]; };

typedef union Vec2i16 Vec2i16;
union Vec2i16 { struct { i16 x, y; }; i16 v[2]; };

typedef union Vec2u32 Vec2u32;
union Vec2u32 { struct { u32 x, y; }; u32 v[2]; };

//- Vec3
typedef union Vec3f32 Vec3f32;
union Vec3f32 {
  struct { f32 x, y, z; };
  struct { f32 r, g, b; };
  struct { Vec2f32 xy; f32 _z0; };
  struct { f32 _x0; Vec2f32 yz; };
  f32 v[3];
};
typedef Vec3f32 Vec3;

typedef union Vec3i32 Vec3i32;
union Vec3i32 {
  struct { i32 x, y, z; };
  struct { Vec2i32 xy; i32 _z0; };
  struct { i32 _x0; Vec2i32 yz; };
  i32 v[3];
};

typedef union Vec3u32 Vec3u32;
union Vec3u32 {
  struct { u32 x, y, z; };
  struct { Vec2u32 xy; u32 _z0; };
  struct { u32 _x0; Vec2u32 yz; };
  u32 v[3];
};

//- Vec4
typedef union Vec4f32 Vec4f32;
union Vec4f32 {
  struct {
    f32 x, y;
    union { struct { f32 z, w; }; struct { f32 width, height; }; };
  };
  struct { union { Vec3f32 rgb; struct { f32 r, g, b; }; }; f32 a; };
  struct { Vec2f32 xy; Vec2f32 zw; };
  struct { Vec3f32 xyz; f32 _z0; };
  struct { f32 _x0; Vec3f32 yzw; };
  f32 v[4];
};
typedef Vec4f32 Quatf32;

typedef union Vec4i32 Vec4i32;
union Vec4i32 {
  struct { i32 x, y, z, w; };
  struct { Vec2i32 xy; Vec2i32 zw; };
  struct { Vec3i32 xyz; i32 _z0; };
  struct { i32 _x0; Vec3i32 yzw; };
  i32 v[4];
};

typedef union Vec4u32 Vec4u32;
union Vec4u32 {
  struct { u32 x, y, z, w; };
  struct { Vec2u32 xy; Vec2u32 zw; };
  struct { Vec3u32 xyz; u32 _z0; };
  struct { u32 _x0; Vec3u32 yzw; };
  u32 v[4];
};

//- Matrices
typedef union Mat3x3f32 Mat3x3f32;
union Mat3x3f32 {
  struct { f32 m0, m3, m6; f32 m1, m4, m7; f32 m2, m5, m8; };
  struct { Vec3f32 x, y, z; };
  f32 v[3][3];
};

typedef union Mat4x4f32 Mat4x4f32;
union Mat4x4f32 {
  struct {
    f32 m0,  m4,  m8,  m12;
    f32 m1,  m5,  m9,  m13;
    f32 m2,  m6,  m10, m14;
    f32 m3,  m7,  m11, m15;
  };
  struct { Vec4f32 x, y, z, w; };
  f32 v[4][4];
};
typedef Mat4x4f32 Mat4;


////////////////////////////////
//~ Range Types (Rng1 / Rng2)

//- 1D ranges
typedef union Rng1u32 Rng1u32;
union Rng1u32 { struct { u32 min, max; }; u32 v[2]; };

typedef union Rng1i32 Rng1i32;
union Rng1i32 { struct { i32 min, max; }; i32 v[2]; };

typedef union Rng1u64 Rng1u64;
union Rng1u64 { struct { u64 min, max; }; u64 v[2]; };

typedef union Rng1i64 Rng1i64;
union Rng1i64 { struct { i64 min, max; }; i64 v[2]; };

typedef union Rng1f32 Rng1f32;
union Rng1f32 { struct { f32 min, max; }; f32 v[2]; };

//- 2D ranges
typedef union Rng2i16 Rng2i16;
union Rng2i16 {
  struct { Vec2i16 min, max; };
  struct { Vec2i16 p0, p1; };
  struct { i16 x0, y0, x1, y1; };
  Vec2i16 v[2];
};

typedef union Rng2i32 Rng2i32;
union Rng2i32 {
  struct { Vec2i32 min, max; };
  struct { Vec2i32 p0, p1; };
  struct { i32 x0, y0, x1, y1; };
  Vec2i32 v[2];
};

typedef union Rng2f32 Rng2f32;
union Rng2f32 {
  struct { Vec2f32 min, max; };
  struct { Vec2f32 p0, p1; };
  struct { f32 x0, y0, x1, y1; };
  Vec2f32 v[2];
};

typedef union Rng2i64 Rng2i64;
union Rng2i64 {
  struct { Vec2i64 min, max; };
  struct { Vec2i64 p0, p1; };
  struct { i64 x0, y0, x1, y1; };
  Vec2i64 v[2];
};

//- Range list/array helpers
typedef struct Rng1u64Node  Rng1u64Node;
struct Rng1u64Node { Rng1u64Node* next; Rng1u64 v; };

typedef struct Rng1u64List  Rng1u64List;
struct Rng1u64List { u64 count; Rng1u64Node* first; Rng1u64Node* last; };

typedef struct Rng1u64Array Rng1u64Array;
struct Rng1u64Array { Rng1u64* v; u64 count; };

typedef struct Rng1i64Node  Rng1i64Node;
struct Rng1i64Node { Rng1i64Node* next; Rng1i64 v; };

typedef struct Rng1i64List  Rng1i64List;
struct Rng1i64List { Rng1i64Node* first; Rng1i64Node* last; u64 count; };

typedef struct Rng1i64Array Rng1i64Array;
struct Rng1i64Array { Rng1i64* v; u64 count; };

////////////////////////////////
//~ Scalar Math
#define abs_i64(v)    (i64)llabs(v)

#define sqrt_f32(v)   sqrtf(v)
#define cbrt_f32(v)   cbrtf(v)
#define mod_f32(a, b) fmodf((a), (b))
#define pow_f32(b, e) powf((b), (e))
#define ceil_f32(v)   ceilf(v)
#define floor_f32(v)  floorf(v)
#define round_f32(v)  roundf(v)
#define abs_f32(v)    fabsf(v)

#define sqrt_f64(v)   sqrt(v)
#define cbrt_f64(v)   cbrt(v)
#define mod_f64(a, b) fmod((a), (b))
#define pow_f64(b, e) pow((b), (e))
#define ceil_f64(v)   ceil(v)
#define floor_f64(v)  floor(v)
#define round_f64(v)  round(v)
#define abs_f64(v)    fabs(v)

//- Angle conversions (turns as canonical unit)
#define radians_from_turns_f32(v)    ((v) * (2 * 3.1415926535897f))
#define turns_from_radians_f32(v)    ((v) / (2 * 3.1415926535897f))
#define degrees_from_turns_f32(v)    ((v) * 360.f)
#define turns_from_degrees_f32(v)    ((v) / 360.f)
#define degrees_from_radians_f32(v)  (degrees_from_turns_f32(turns_from_radians_f32(v)))
#define radians_from_degrees_f32(v)  (radians_from_turns_f32(turns_from_degrees_f32(v)))

#define radians_from_turns_f64(v)    ((v) * (2 * 3.1415926535897))
#define turns_from_radians_f64(v)    ((v) / (2 * 3.1415926535897))
#define degrees_from_turns_f64(v)    ((v) * 360.0)
#define turns_from_degrees_f64(v)    ((v) / 360.0)
#define degrees_from_radians_f64(v)  (degrees_from_turns_f64(turns_from_radians_f64(v)))
#define radians_from_degrees_f64(v)  (radians_from_turns_f64(turns_from_degrees_f64(v)))

//- Trig (input in turns)
#define sin_f32(v)  sinf(radians_from_turns_f32(v))
#define cos_f32(v)  cosf(radians_from_turns_f32(v))
#define tan_f32(v)  tanf(radians_from_turns_f32(v))
#define sin_f64(v)  sin(radians_from_turns_f64(v))
#define cos_f64(v)  cos(radians_from_turns_f64(v))
#define tan_f64(v)  tan(radians_from_turns_f64(v))

//- Interpolation
#define lerp(a, b, t)  ((a) + (t) * ((b) - (a)))

f32 mix_1f32(f32 a, f32 b, f32 t);
f64 mix_1f64(f64 a, f64 b, f64 t);


////////////////////////////////
//~ Vector Math

//- Vec2f32
#define v2f32(x, y)  vec_2f32((x), (y))
Vec2f32 vec_2f32(f32 x, f32 y);
Vec2f32 add_2f32(Vec2f32 a, Vec2f32 b);
Vec2f32 sub_2f32(Vec2f32 a, Vec2f32 b);
Vec2f32 mul_2f32(Vec2f32 a, Vec2f32 b);
Vec2f32 div_2f32(Vec2f32 a, Vec2f32 b);
Vec2f32 scale_2f32(Vec2f32 v, f32 s);
f32     dot_2f32(Vec2f32 a, Vec2f32 b);
f32     length_squared_2f32(Vec2f32 v);
f32     length_2f32(Vec2f32 v);
Vec2f32 normalize_2f32(Vec2f32 v);
Vec2f32 mix_2f32(Vec2f32 a, Vec2f32 b, f32 t);
Vec2f32 lerp_2f32(Vec2f32 a, Vec2f32 b, f32 t);
Vec2f32 rotate_2f32(Vec2f32 v, f32 angle);

//- Vec2i64
#define v2i64(x, y)  vec_2i64((x), (y))
Vec2i64 vec_2i64(i64 x, i64 y);
Vec2i64 add_2i64(Vec2i64 a, Vec2i64 b);
Vec2i64 sub_2i64(Vec2i64 a, Vec2i64 b);
Vec2i64 mul_2i64(Vec2i64 a, Vec2i64 b);
Vec2i64 div_2i64(Vec2i64 a, Vec2i64 b);
Vec2i64 scale_2i64(Vec2i64 v, i64 s);
i64     dot_2i64(Vec2i64 a, Vec2i64 b);
i64     length_squared_2i64(Vec2i64 v);
i64     length_2i64(Vec2i64 v);
Vec2i64 normalize_2i64(Vec2i64 v);
Vec2i64 mix_2i64(Vec2i64 a, Vec2i64 b, f32 t);

//- Vec2i32
#define v2i32(x, y)  vec_2i32((x), (y))
Vec2i32 vec_2i32(i32 x, i32 y);
Vec2i32 add_2i32(Vec2i32 a, Vec2i32 b);
Vec2i32 sub_2i32(Vec2i32 a, Vec2i32 b);
Vec2i32 mul_2i32(Vec2i32 a, Vec2i32 b);
Vec2i32 div_2i32(Vec2i32 a, Vec2i32 b);
Vec2i32 scale_2i32(Vec2i32 v, i32 s);
i32     dot_2i32(Vec2i32 a, Vec2i32 b);
i32     length_squared_2i32(Vec2i32 v);
i32     length_2i32(Vec2i32 v);
Vec2i32 normalize_2i32(Vec2i32 v);
Vec2i32 mix_2i32(Vec2i32 a, Vec2i32 b, f32 t);

//- Vec2i16
#define v2i16(x, y)  vec_2i16((x), (y))
Vec2i16 vec_2i16(i16 x, i16 y);
Vec2i16 add_2i16(Vec2i16 a, Vec2i16 b);
Vec2i16 sub_2i16(Vec2i16 a, Vec2i16 b);
Vec2i16 mul_2i16(Vec2i16 a, Vec2i16 b);
Vec2i16 div_2i16(Vec2i16 a, Vec2i16 b);
Vec2i16 scale_2i16(Vec2i16 v, i16 s);
i16     dot_2i16(Vec2i16 a, Vec2i16 b);
i16     length_squared_2i16(Vec2i16 v);
i16     length_2i16(Vec2i16 v);
Vec2i16 normalize_2i16(Vec2i16 v);
Vec2i16 mix_2i16(Vec2i16 a, Vec2i16 b, f32 t);

//- Vec3f32
#define v3f32(x, y, z)  vec_3f32((x), (y), (z))
Vec3f32 vec_3f32(f32 x, f32 y, f32 z);
Vec3f32 add_3f32(Vec3f32 a, Vec3f32 b);
Vec3f32 sub_3f32(Vec3f32 a, Vec3f32 b);
Vec3f32 mul_3f32(Vec3f32 a, Vec3f32 b);
Vec3f32 div_3f32(Vec3f32 a, Vec3f32 b);
Vec3f32 scale_3f32(Vec3f32 v, f32 s);
Vec3f32 negate_3f32(Vec3f32 v);
Vec3f32 transform_3f32(Vec3f32 v, f32 last, Mat4x4f32 m);
f32     dot_3f32(Vec3f32 a, Vec3f32 b);
f32     length_squared_3f32(Vec3f32 v);
f32     length_3f32(Vec3f32 v);
f32     dist_3f32(Vec3f32 a, Vec3f32 b);
Vec3f32 normalize_3f32(Vec3f32 v);
Vec3f32 mix_3f32(Vec3f32 a, Vec3f32 b, f32 t);
Vec3f32 lerp_3f32(Vec3f32 a, Vec3f32 b, f32 t);
Vec3f32 lerpc_3f32(Vec3f32 a, Vec3f32 b, f32 t);
Vec3f32 cross_3f32(Vec3f32 a, Vec3f32 b);
Vec3f32 perp_3f32(Vec3f32 v);
Vec3f32 bary_3f32(Vec3f32 p, Vec3f32 a, Vec3f32 b, Vec3f32 c);
Vec3f32 xform_3f32(Vec3f32 v, Mat3x3f32 m);

//- Vec3i32
#define v3i32(x, y, z)  vec_3i32((x), (y), (z))
Vec3i32 vec_3i32(i32 x, i32 y, i32 z);
Vec3i32 add_3i32(Vec3i32 a, Vec3i32 b);
Vec3i32 sub_3i32(Vec3i32 a, Vec3i32 b);
Vec3i32 mul_3i32(Vec3i32 a, Vec3i32 b);
Vec3i32 div_3i32(Vec3i32 a, Vec3i32 b);
Vec3i32 scale_3i32(Vec3i32 v, i32 s);
i32     dot_3i32(Vec3i32 a, Vec3i32 b);
i32     length_squared_3i32(Vec3i32 v);
i32     length_3i32(Vec3i32 v);
Vec3i32 normalize_3i32(Vec3i32 v);
Vec3i32 mix_3i32(Vec3i32 a, Vec3i32 b, f32 t);
Vec3i32 cross_3i32(Vec3i32 a, Vec3i32 b);

//- Vec4f32
#define v4f32(x, y, z, w)  vec_4f32((x), (y), (z), (w))
Vec4f32 vec_4f32(f32 x, f32 y, f32 z, f32 w);
Vec4f32 add_4f32(Vec4f32 a, Vec4f32 b);
Vec4f32 sub_4f32(Vec4f32 a, Vec4f32 b);
Vec4f32 mul_4f32(Vec4f32 a, Vec4f32 b);
Vec4f32 div_4f32(Vec4f32 a, Vec4f32 b);
Vec4f32 scale_4f32(Vec4f32 v, f32 s);
f32     dot_4f32(Vec4f32 a, Vec4f32 b);
f32     length_squared_4f32(Vec4f32 v);
f32     length_4f32(Vec4f32 v);
Vec4f32 normalize_4f32(Vec4f32 v);
Vec4f32 mix_4f32(Vec4f32 a, Vec4f32 b, f32 t);

//- Vec4i32
#define v4i32(x, y, z, w)  vec_4i32((x), (y), (z), (w))
Vec4i32 vec_4i32(i32 x, i32 y, i32 z, i32 w);
Vec4i32 add_4i32(Vec4i32 a, Vec4i32 b);
Vec4i32 sub_4i32(Vec4i32 a, Vec4i32 b);
Vec4i32 mul_4i32(Vec4i32 a, Vec4i32 b);
Vec4i32 div_4i32(Vec4i32 a, Vec4i32 b);
Vec4i32 scale_4i32(Vec4i32 v, i32 s);
i32     dot_4i32(Vec4i32 a, Vec4i32 b);
i32     length_squared_4i32(Vec4i32 v);
i32     length_4i32(Vec4i32 v);
Vec4i32 normalize_4i32(Vec4i32 v);
Vec4i32 mix_4i32(Vec4i32 a, Vec4i32 b, f32 t);


////////////////////////////////
//~ Matrix Math

//- Mat3x3f32
Mat3x3f32 mat_3x3f32(f32 diagonal);
Mat3x3f32 make_translate_3x3f32(Vec2f32 delta);
Mat3x3f32 make_scale_3x3f32(Vec2f32 scale);
Mat3x3f32 mul_3x3f32(Mat3x3f32 a, Mat3x3f32 b);

//- Mat4x4f32
#define m4x4f32(d)  mat_4x4f32((d))
Mat4x4f32 mat_4x4f32(f32 diagonal);
Mat4x4f32 make_translate_4x4f32(Vec3f32 delta);
Mat4x4f32 make_scale_4x4f32(Vec3f32 scale);
Mat4x4f32 make_perspective_4x4f32(f32 fov, f32 aspect_ratio, f32 near_z, f32 far_z);
Mat4x4f32 make_perspective_invert_4x4f32(f32 fov, f32 aspect_ratio, f32 near_z, f32 far_z);
Mat4x4f32 make_orthographic_4x4f32(f32 left, f32 right, f32 bottom, f32 top, f32 near_z, f32 far_z);
Mat4x4f32 make_look_at_4x4f32(Vec3f32 eye, Vec3f32 center, Vec3f32 up);
Mat4x4f32 make_look_at_invert_4x4f32(Vec3f32 eye, Vec3f32 center, Vec3f32 up);
Mat4x4f32 make_rotate_4x4f32(Vec3f32 axis, f32 angle);
Mat4x4f32 make_rotate_x_4x4f32(f32 angle);
Mat4x4f32 make_rotate_y_4x4f32(f32 angle);
Mat4x4f32 make_rotate_z_4x4f32(f32 angle);
Mat4x4f32 mul_4x4f32(Mat4x4f32 a, Mat4x4f32 b);
Mat4x4f32 scale_4x4f32(Mat4x4f32 m, f32 scale);
Mat4x4f32 inverse_4x4f32(Mat4x4f32 m);
Mat4x4f32 derotate_4x4f32(Mat4x4f32 mat);
Mat4x4f32 transpose_4x4f32(Mat4x4f32 mat);


////////////////////////////////
//~ Range Math

#define r1_lerp(r, t)  lerp((r).min, (r).max, (t))

//- Rng1u32
#define r1u32(min, max)  rng_1u32((min), (max))
Rng1u32 rng_1u32(u32 min, u32 max);
Rng1u32 shift_1u32(Rng1u32 r, u32 x);
Rng1u32 pad_1u32(Rng1u32 r, u32 x);
u32     center_1u32(Rng1u32 r);
b32     contains_1u32(Rng1u32 r, u32 x);
u32     dim_1u32(Rng1u32 r);
Rng1u32 union_1u32(Rng1u32 a, Rng1u32 b);
Rng1u32 intersect_1u32(Rng1u32 a, Rng1u32 b);
u32     clamp_1u32(Rng1u32 r, u32 v);

//- Rng1i32
#define r1i32(min, max)  rng_1i32((min), (max))
Rng1i32 rng_1i32(i32 min, i32 max);
Rng1i32 shift_1i32(Rng1i32 r, i32 x);
Rng1i32 pad_1i32(Rng1i32 r, i32 x);
i32     center_1i32(Rng1i32 r);
b32     contains_1i32(Rng1i32 r, i32 x);
i32     dim_1i32(Rng1i32 r);
Rng1i32 union_1i32(Rng1i32 a, Rng1i32 b);
Rng1i32 intersect_1i32(Rng1i32 a, Rng1i32 b);
i32     clamp_1i32(Rng1i32 r, i32 v);

//- Rng1u64
#define r1u64(min, max)  rng_1u64((min), (max))
Rng1u64 rng_1u64(u64 min, u64 max);
Rng1u64 shift_1u64(Rng1u64 r, u64 x);
Rng1u64 pad_1u64(Rng1u64 r, u64 x);
u64     center_1u64(Rng1u64 r);
b32     contains_1u64(Rng1u64 r, u64 x);
u64     dim_1u64(Rng1u64 r);
Rng1u64 union_1u64(Rng1u64 a, Rng1u64 b);
Rng1u64 intersect_1u64(Rng1u64 a, Rng1u64 b);
u64     clamp_1u64(Rng1u64 r, u64 v);

//- Rng1i64
#define r1i64(min, max)  rng_1i64((min), (max))
Rng1i64 rng_1i64(i64 min, i64 max);
Rng1i64 shift_1i64(Rng1i64 r, i64 x);
Rng1i64 pad_1i64(Rng1i64 r, i64 x);
i64     center_1i64(Rng1i64 r);
b32     contains_1i64(Rng1i64 r, i64 x);
i64     dim_1i64(Rng1i64 r);
Rng1i64 union_1i64(Rng1i64 a, Rng1i64 b);
Rng1i64 intersect_1i64(Rng1i64 a, Rng1i64 b);
i64     clamp_1i64(Rng1i64 r, i64 v);

//- Rng1f32
#define r1f32(min, max)  rng_1f32((min), (max))
Rng1f32 rng_1f32(f32 min, f32 max);
Rng1f32 shift_1f32(Rng1f32 r, f32 x);
Rng1f32 pad_1f32(Rng1f32 r, f32 x);
f32     center_1f32(Rng1f32 r);
b32     contains_1f32(Rng1f32 r, f32 x);
f32     dim_1f32(Rng1f32 r);
Rng1f32 union_1f32(Rng1f32 a, Rng1f32 b);
Rng1f32 intersect_1f32(Rng1f32 a, Rng1f32 b);
f32     clamp_1f32(Rng1f32 r, f32 v);

//- Rng2i16
#define r2i16(min, max)          rng_2i16((min), (max))
#define r2i16p(x, y, z, w)       r2i16(v2i16((x), (y)), v2i16((z), (w)))
Rng2i16 rng_2i16(Vec2i16 min, Vec2i16 max);
Rng2i16 shift_2i16(Rng2i16 r, Vec2i16 x);
Rng2i16 pad_2i16(Rng2i16 r, i16 x);
Vec2i16 center_2i16(Rng2i16 r);
b32     contains_2i16(Rng2i16 r, Vec2i16 x);
Vec2i16 dim_2i16(Rng2i16 r);
Rng2i16 union_2i16(Rng2i16 a, Rng2i16 b);
Rng2i16 intersect_2i16(Rng2i16 a, Rng2i16 b);
Vec2i16 clamp_2i16(Rng2i16 r, Vec2i16 v);

//- Rng2i32
#define r2i32(min, max)          rng_2i32((min), (max))
#define r2i32p(x, y, z, w)       r2i32(v2i32((x), (y)), v2i32((z), (w)))
Rng2i32 rng_2i32(Vec2i32 min, Vec2i32 max);
Rng2i32 shift_2i32(Rng2i32 r, Vec2i32 x);
Rng2i32 pad_2i32(Rng2i32 r, i32 x);
Vec2i32 center_2i32(Rng2i32 r);
b32     contains_2i32(Rng2i32 r, Vec2i32 x);
Vec2i32 dim_2i32(Rng2i32 r);
Rng2i32 union_2i32(Rng2i32 a, Rng2i32 b);
Rng2i32 intersect_2i32(Rng2i32 a, Rng2i32 b);
Vec2i32 clamp_2i32(Rng2i32 r, Vec2i32 v);

//- Rng2i64
#define r2i64(min, max)          rng_2i64((min), (max))
#define r2i64p(x, y, z, w)       r2i64(v2i64((x), (y)), v2i64((z), (w)))
Rng2i64 rng_2i64(Vec2i64 min, Vec2i64 max);
Rng2i64 shift_2i64(Rng2i64 r, Vec2i64 x);
Rng2i64 pad_2i64(Rng2i64 r, i64 x);
Vec2i64 center_2i64(Rng2i64 r);
b32     contains_2i64(Rng2i64 r, Vec2i64 x);
Vec2i64 dim_2i64(Rng2i64 r);
Rng2i64 union_2i64(Rng2i64 a, Rng2i64 b);
Rng2i64 intersect_2i64(Rng2i64 a, Rng2i64 b);
Vec2i64 clamp_2i64(Rng2i64 r, Vec2i64 v);

//- Rng2f32
#define r2f32(min, max)          rng_2f32((min), (max))
#define r2f32p(x, y, z, w)       r2f32(v2f32((x), (y)), v2f32((z), (w)))
Rng2f32 rng_2f32(Vec2f32 min, Vec2f32 max);
Rng2f32 shift_2f32(Rng2f32 r, Vec2f32 x);
Rng2f32 pad_2f32(Rng2f32 r, f32 x);
Vec2f32 center_2f32(Rng2f32 r);
b32     contains_2f32(Rng2f32 r, Vec2f32 x);
Vec2f32 dim_2f32(Rng2f32 r);
Rng2f32 union_2f32(Rng2f32 a, Rng2f32 b);
Rng2f32 intersect_2f32(Rng2f32 a, Rng2f32 b);
Vec2f32 clamp_2f32(Rng2f32 r, Vec2f32 v);


////////////////////////////////
//~ Context

typedef struct Context {
  Arena scratch;
  Arena perm;
} Context;

void      ctx_init(Context* ctx, u64 scratch_size);
void      ctx_init_with_perm(Context* ctx, u64 scratch_size, u64 perm_size);
void      ctx_enter(Context* ctx);
void      ctx_leave_and_reset(void);
void      ctx_leave_and_release(void);
void      ctx_leave_and_preserve(void);
void      ctx_free(void);
void      ctx_set_global(Context* ctx);
Context*  ctx_global(void);
Context*  ctx_current(void);
Arena*    ctx_scratch(void);
Arena*    ctx_perm(void);

////////////////////////////////
//~ Strings

typedef struct String8  { u8*  str; u64 size; } String8;
typedef struct String16 { u16* str; u64 size; } String16;
typedef struct String32 { u32* str; u64 size; } String32;

typedef struct String8Node String8Node;
struct String8Node {
  String8Node* next;
  String8      string;
};

typedef struct String8List {
  String8Node* first;
  String8Node* last;
  u64          node_count;
  u64          total_size;
} String8List;

typedef struct String8Array {
  String8* v;
  u64      count;
} String8Array;

DEFINE_SMALL_VECTOR(String8, str8, 8)

typedef struct String8Vector {
  SVString8 v;
  u64       total_size;
} String8Vector;

typedef u32 StringMatchFlags;
enum {
  StringMatchFlag_CaseInsensitive  = (1 << 0),
  StringMatchFlag_RightSideSloppy  = (1 << 1),
  StringMatchFlag_SlashInsensitive = (1 << 2),
};

typedef u32 StringSplitFlags;
enum {
  StringSplitFlag_KeepEmpties = (1 << 0),
};

typedef enum PathStyle {
  PathStyle_Null,
  PathStyle_Relative,
  PathStyle_WindowsAbsolute,
  PathStyle_UnixAbsolute,
  PathStyle_SystemAbsolute = PathStyle_UnixAbsolute,
} PathStyle;

typedef struct StringJoin {
  String8 pre;
  String8 sep;
  String8 post;
} StringJoin;

//- SmallString (inline small-buffer string)
DEFINE_SMALL_VECTOR(u8, u8, 64)
DEFINE_SMALL_VECTOR(u16, u16, 64)
DEFINE_SMALL_VECTOR(u32, u32, 64)

typedef struct SmallString { SVu8 v; } SmallString;

void    ss_init(Arena* a, SmallString* s);
u64     ss_len(SmallString* s);
void    ss_clear(Arena* a, SmallString* s);
void    ss_push_u8(Arena* a, SmallString* s, u8 c);
void    ss_append(Arena* a, SmallString* s, String8 str);
void    ss_append_cstr(Arena* a, SmallString* s, const char* cstr);
b32     ss_match(SmallString a, SmallString b, StringMatchFlags flags);
String8 ss_str8(SmallString* s);
u8*     ss_cstr(SmallString* s);
void    ss_free(SmallString* s);

#define ss_append_lit(a, s, lit)  ss_append((a), (s), str8_lit(lit))

//- Character classification
b32 char_is_space(u8 c);
b32 char_is_upper(u8 c);
b32 char_is_lower(u8 c);
b32 char_is_alpha(u8 c);
b32 char_is_slash(u8 c);
b32 char_is_digit(u8 c, u32 base);
u8  char_to_lower(u8 c);
u8  char_to_upper(u8 c);
u8  char_to_correct_slash(u8 c);

//- C-string lengths
u64 cstring8_length(u8* c);
u64 cstring16_length(u16* c);
u64 cstring32_length(u32* c);

//- String8 macros
#define str8_lit(S)          str8((u8*)(S), sizeof(S) - 1)
#define str8_lit_comp(S)     {(u8*)(S), sizeof(S) - 1}
#define str8_varg(S)         (int)((S).size), ((S).str)
#define str8_is_empty(s)     ((s).size == 0 || !(s).str)
#define str8_array(S, C)     str8((u8*)(S), sizeof(*(S)) * (C))
#define str8_array_fixed(S)  str8((u8*)(S), sizeof(S))
#define str8_struct(S)       str8((u8*)(S), sizeof(*(S)))
#define str8_dbg(s)          str8_debug(__FILE__, __LINE__, #s, s)
#define str8_fmt             "%.*s"

//- String constructors
String8  str8(u8* str, u64 size);
String8  str8_range(u8* first, u8* one_past_last);
String8  str8_zero(void);
String16 str16(u16* str, u64 size);
String16 str16_zero(void);
String32 str32(u32* str, u64 size);
String32 str32_zero(void);
String8  str8_cstring(char* c);
String16 str16_cstring(u16* c);
String32 str32_cstring(u32* c);

//- String8 operations
#define str8_match_lit(a_lit, b, flags)   str8_match(str8_lit(a_lit), (b), (flags))
#define str8_match_cstr(a_cstr, b, flags) str8_match(str8_cstring(a_cstr), (b), (flags))
b32     str8_match(String8 a, String8 b, StringMatchFlags flags);
u64     str8_find_needle(String8 string, u64 start_pos, String8 needle, StringMatchFlags flags);
u64     str8_find_needle_reverse(String8 string, u64 start_pos, String8 needle, StringMatchFlags flags);
b32     str8_starts_with(String8 string, String8 start, StringMatchFlags flags);
b32     str8_ends_with(String8 string, String8 end, StringMatchFlags flags);

String8 str8_substr(String8 str, Rng1u64 range);
String8 str8_prefix(String8 str, u64 size);
String8 str8_skip(String8 str, u64 amt);
String8 str8_postfix(String8 str, u64 size);
String8 str8_chop(String8 str, u64 amt);
String8 str8_skip_chop_whitespace(String8 string);
String8 str8_skip_chop_slashes(String8 string);

String8 str8_debug(const char* file, i32 line, const char* name, String8 s);
String8 str8_cat(Arena* arena, String8 s1, String8 s2);
String8 str8_copy(Arena* arena, String8 s);
char*   cstring_str8(Arena* arena, String8 s); // opposite direction of str8_cstring above --
                                                    // null-terminates s into arena for handing to
                                                    // a real C API expecting `const char*`/`char*`.
String8 str8fv(Arena* arena, char* fmt, va_list args);
String8 str8f(Arena* arena, char* fmt, ...);

//- String8 parsing
i64 sign_from_str8(String8 string, String8* string_tail);
b32 str8_is_integer(String8 string, u32 radix);
u64 u64_from_str8(String8 string, u32 radix);
i64 i64_from_str8(String8 string, u32 radix);
u32 u32_from_str8(String8 string, u32 radix);
i32 i32_from_str8(String8 string, u32 radix);
b32 try_u64_from_str8_c_rules(String8 string, u64* x);
b32 try_i64_from_str8_c_rules(String8 string, i64* x);

//- String8 formatting
String8 str8_from_memory_size(Arena* arena, u64 size);
String8 str8_from_count(Arena* arena, u64 count);
String8 str8_from_bits_u32(Arena* arena, u32 x);
String8 str8_from_bits_u64(Arena* arena, u64 x);
String8 str8_from_u64(Arena* arena, u64 v, u32 radix, u8 min_digits, u8 digit_group_separator);
String8 str8_from_i64(Arena* arena, i64 v, u32 radix, u8 min_digits, u8 digit_group_separator);

//- String8List
String8Node* str8_list_push_node(String8List* list, String8Node* node);
String8Node* str8_list_push_node_set_string(String8List* list, String8Node* node, String8 string);
String8Node* str8_list_push_node_front(String8List* list, String8Node* node);
String8Node* str8_list_push_node_front_set_string(String8List* list, String8Node* node, String8 string);
String8Node* str8_list_push(Arena* arena, String8List* list, String8 string);
String8Node* str8_list_push_front(Arena* arena, String8List* list, String8 string);
void         str8_list_concat_in_place(String8List* list, String8List* to_push);
String8Node* str8_list_push_aligner(Arena* arena, String8List* list, u64 min, u64 align);
String8Node* str8_list_pushf(Arena* arena, String8List* list, char* fmt, ...);
String8Node* str8_list_push_frontf(Arena* arena, String8List* list, char* fmt, ...);
String8List  str8_list_copy(Arena* arena, String8List* list);
String8List  str8_split_by_string_chars(Arena* arena, String8 string, String8 split_chars, StringSplitFlags flags);
String8      str8_list_join(Arena* arena, String8List* list, StringJoin* optional_params);
void         str8_list_from_flags(Arena* arena,
                                           String8List* list,
                                           u32 flags,
                                           String8*
                                           flag_string_table,
                                           u32 flag_string_count);
String8List  str8_list_split_by_string_chars(Arena* arena,
                                                      String8List list,
                                                      String8 split_chars,
                                                      StringSplitFlags flags);
String8List  str8_split_into_list(Arena* arena,
                                           String8 string,
                                           u8* split_chars,
                                           u64 split_char_count,
                                           StringSplitFlags flags);
#define str8_list_first(list)  ((list)->first ? (list)->first->string : str8_zero())

//- String8Array
String8Array str8_array_zero(void);
String8Array str8_array_from_list(Arena* arena, String8List* list);
String8Array str8_array_reserve(Arena* arena, u64 count);
String8Array str8_array_copy(Arena* arena, String8Array array);

//- String8Vector
String8Vector str8_vector_zero(void);
void          str8_vector_init(String8Vector* sv);
String8Vector str8_vector_copy(Arena* arena, String8Vector* sv);
void          str8_vector_reserve(Arena* arena, String8Vector* sv, u64 count);
void          str8_vector_push(Arena* arena, String8Vector* sv, String8 string);
void          str8_vector_reset(String8Vector* sv);
void          str8_vector_free(String8Vector* sv);
String8       str8_vector_get(String8Vector* sv, u64 index);
void          str8_vector_set(String8Vector* sv, u64 index, String8 val);
b32           str8_vector_is_inline(String8Vector* sv);
String8       str8_vector_join(Arena* arena, String8Vector* sv, StringJoin* optional_params);
void          str8_split_into_vector(Arena *arena,
                                        String8Vector* out,
                                        String8 string,
                                        u8 *split_chars,
                                        u64 split_char_count,
                                        StringSplitFlags flags);


////////////////////////////////
//~ Time

typedef i64 Duration;

// Duration units in nanoseconds
#define Nanosecond   1
#define Microsecond  (1000LL)
#define Millisecond  (1000LL * Microsecond)
#define Second       (1000LL * Millisecond)
#define Minute       (60LL   * Second)
#define Hour         (60LL   * Minute)

#define MIN_DURATION  (INT64_MIN)
#define MAX_DURATION  (INT64_MAX)

typedef struct Time      { i64 nsec; } Time;

typedef struct Stopwatch {
  Time     start_time;
  Duration accumulation;
  b8       running;
} Stopwatch;

typedef enum {
  January = 1, February, March,     April,   May,      June,
  July,        August,   September, October, November, December,
} Month;

typedef enum {
  Sunday = 0, Monday, Tuesday, Wednesday, Thursday, Friday, Saturday,
} Weekday;

//- Wall / monotonic clock
Time time_wall(void);
Time time_now(void);
void sleep_for(Duration d);
void sleep_until(Time t);

//- Time arithmetic
Time     time_add(Time t, Duration d);
Time     time_sub(Time t, Duration d);
Duration time_diff(Time start, Time end);
Duration time_since(Time start);
Duration time_until(Time end);

//- Duration conversion
f64 duration_nanoseconds(Duration d);
f64 duration_microseconds(Duration d);
f64 duration_milliseconds(Duration d);
f64 duration_seconds(Duration d);
f64 duration_minutes(Duration d);
f64 duration_hours(Duration d);

//- Stopwatch
void     stopwatch_start(Stopwatch* stopwatch);
void     stopwatch_stop(Stopwatch* stopwatch);
void     stopwatch_reset(Stopwatch* stopwatch);
Duration stopwatch_duration(Stopwatch* stopwatch);

#if !COMPILER_TCC
////////////////////////////////
//~ Threads

#if !defined(_WIN32)
# include <pthread.h>
#endif
#include <stdatomic.h>

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define Thread_Local _Thread_local
#else
#define Thread_Local __thread
#endif

typedef void ThreadFn(void* p);

typedef struct Mutex {
  u64 v[1];
} Mutex;

typedef struct CondVar {
  u64 v[1];
} CondVar;

typedef struct Barrier {
  u64 v[1];
} Barrier;

typedef struct Thread {
  u64 v[1];
} Thread;

#define mutex_scope(mutex) defer(mutex_take(mutex), mutex_drop(mutex))

Barrier barrier_alloc(u32 count);
void    barrier_release(Barrier barrier);
void    barrier_wait(Barrier b);
Mutex   mutex_alloc(void);
void    mutex_release(Mutex mutex);
void    mutex_take(Mutex mutex);
void    mutex_drop(Mutex mutex);
CondVar cond_var_alloc(void);
void    cond_var_release(CondVar cv);
b32     cond_var_wait(CondVar cv, Mutex mutex, u64 duration_ns);
void    cond_var_signal(CondVar cv);
void    cond_var_broadcast(CondVar cv);
Thread  thread_launch(ThreadFn* func, void* ptr);
b32     thread_join(Thread handle);
void    thread_detach(Thread handle);

typedef struct LaneCtx {
  u32     lane_index;
  u32     lane_count;
  Barrier barrier;
  u64*    broadcast;
} LaneCtx;

typedef struct ThreadContext {
  Arena   perm; // not actually permanent, but within the context of the thread
  Arena   scratch;
  LaneCtx lane;
} ThreadContext;

typedef void AsyncPhaseFn(void);

void           tctx_init(ThreadContext* tctx, u64 scratch_size);
void           tctx_init_with_perm(ThreadContext* tctx, u64 scratch_size, u64 perm_size);
void           tctx_destroy(ThreadContext* tctx);
void           tctx_attach(ThreadContext* t);
void           tctx_detach(void);
ThreadContext* tctx_current(void);
Arena*         tctx_scratch(void);
Arena*         tctx_perm(void);

void supplement_thread_base_entry_point(void (*entry_point)(void* params), void* params);

void    lane_thread_main(void* ptr);
LaneCtx lane_enter(LaneCtx lane_ctx);
void    lane_leave(LaneCtx prev);
void    lane_barrier_wait(void* broadcast_ptr, u64 broadcast_size, u64 broadcast_lane_idx);
Rng1u64 lane_range(u64 work_count);
void    lane_reduce_sum_u64(u64* ptr, u32 dst_lane_idx);
#define lane_sync_u64(ptr, src_lane_idx) lane_barrier_wait((ptr), sizeof(*(ptr)), (src_lane_idx))
#define lane_sync()                      lane_barrier_wait(0, 0, 0)
#define lane_ctx()                       tctx_current()->lane
#define lane_idx()                       (lane_ctx().lane_index)
#define lane_count()                     (lane_ctx().lane_count)
#define lane_from_task_idx(idx)          ((idx) % lane_count())
#define lane_responsible_for(idx)        (lane_idx() == lane_from_task_idx(idx))
#define lane_broadcast()                 (lane_ctx().broadcast)

void async_threads_init(void);
void async_threads_shutdown(void);
u64  async_run_phase(AsyncPhaseFn func);
b32  async_phase_done(u64 generation);
void async_phase_wait(u64 generation);

////////////////////////////////
//~ OS

typedef enum OS_EntityKind {
  OS_EntityKind_Thread,
  OS_EntityKind_Mutex,
  OS_EntityKind_CondVar,
  OS_EntityKind_Barrier,
} OS_EntityKind;

typedef struct OS_Entity OS_Entity;

#if defined(_WIN32)

struct OS_Entity {
  OS_Entity*    next;
  OS_EntityKind kind;
  union {
    struct {
      HANDLE    handle;
      ThreadFn* func;
      void*     ptr;
    } thread;
    SRWLOCK            mutex_handle;
    CONDITION_VARIABLE cond_handle;
    // Hand-rolled -- Windows has no native barrier primitive either
    // (nothing before the Win8 CreateBarrier family, which this doesn't
    // depend on). Built from SRWLOCK+CONDITION_VARIABLE instead, same
    // sense-reversing-barrier algorithm as the POSIX side below.
    struct {
      SRWLOCK            mutex;
      CONDITION_VARIABLE cond;
      u32                count;
      u32                waiting;
      u32                generation;
    } barrier;
  };
};

typedef struct OS_State {
  Arena      arena;
  SRWLOCK    entity_mutex;
  Arena      entity_arena;
  OS_Entity* entity_free;
} OS_State;

#else // POSIX (Linux/Mac)

struct OS_Entity {
  OS_Entity*    next;
  OS_EntityKind kind;
  union {
    struct {
      pthread_t handle;
      ThreadFn* func;
      void*     ptr;
    } thread;
    pthread_mutex_t  mutex_handle;
    struct {
      pthread_cond_t  cond_handle;
      pthread_mutex_t rwlock_mutex_handle;
    } cv;
    // Hand-rolled instead of pthread_barrier_t -- that type (and every
    // pthread_barrier_* function) doesn't exist at all on macOS (a real,
    // long-standing POSIX gap on Darwin; POSIX marks barriers optional
    // and Apple never implemented them). Built from mutex+condvar+a
    // generation counter instead -- a standard sense-reversing barrier,
    // portable everywhere mutex+condvar are (i.e. everywhere).
    struct {
      pthread_mutex_t mutex;
      pthread_cond_t  cond;
      u32             count;
      u32             waiting;
      u32             generation;
    } barrier;
  };
};

typedef struct OS_State {
  Arena arena;
  pthread_mutex_t entity_mutex;
  Arena      entity_arena;
  OS_Entity* entity_free;
} OS_State;

#endif

void       os_state_init(void);
void       os_state_free(void);
Arena*     os_arena(void);
u64        os_get_page_size(void);
u32        os_get_core_count(void);
OS_Entity* os_entity_alloc(OS_EntityKind kind);
void       os_entity_release(OS_Entity* entity);
#if defined(_WIN32)
DWORD WINAPI os_thread_entry_point(LPVOID ptr);
#else
void*      os_thread_entry_point(void* ptr);
#endif
Thread     os_thread_launch(ThreadFn* func, void* ptr);
b32        os_thread_join(Thread handle);
void       os_thread_detach(Thread handle);
Mutex      os_mutex_alloc(void);
void       os_mutex_release(Mutex mutex);
void       os_mutex_take(Mutex mutex);
void       os_mutex_drop(Mutex mutex);
CondVar    os_cond_var_alloc(void);
void       os_cond_var_release(CondVar cv);
b32        os_cond_var_wait(CondVar cv, Mutex mutex, u64 duration_ns);
void       os_cond_var_signal(CondVar cv);
void       os_cond_var_broadcast(CondVar cv);
Barrier    os_barrier_alloc(u64 count);
void       os_barrier_release(Barrier barrier);
void       os_barrier_wait(Barrier barrier);
#endif

////////////////////////////////
//~ File I/O

// Whole-file read into `arena`. `Arena` by value, not `Arena*` -- it's a
// lightweight handle (a backend pointer + vtable pointer; mutation happens
// through what `backend` points AT, not the Arena struct's own fields), so
// copying it is cheap and safe, same convention 3b's language-level
// `arena` type already uses everywhere (it has no arena-pointer concept at
// all). Returns `{0}` (str==NULL, size==0) on any failure (missing file,
// permission denied, directory, ...) -- check `.str == NULL` to detect. No
// separate error/errno out-param on purpose, same "absence is the whole
// signal" shape `nil` already has for pointers elsewhere in this codebase;
// a caller that needs to distinguish failure reasons can still fall back
// to `errno` itself right after the call.
String8 os_file_read(Arena arena, String8 path);

////////////////////////////////
//~ Hash Table

typedef enum { SlotState_Empty, SlotState_Occupied, SlotState_Tombstone } SlotState;

typedef struct {
  String8   key;
  void*     value;
  SlotState state;
} HashSlot;

typedef struct {
  HashSlot* slots;
  u64       capacity;
  u64       count;       // live entries
  u64       tombstones;  // deleted slots
} HashTable;

void  hashtable_init(Arena* arena, HashTable* ht, u64 capacity);
void  hashtable_resize(Arena* arena, HashTable* ht, u64 new_cap);
b32   hashtable_insert(Arena* arena, HashTable* ht, String8 key, void* value, b32 overwrite);
void* hashtable_lookup(HashTable* ht, String8 key);
b32   hashtable_remove(HashTable* ht, String8 key);


typedef struct Handle {
  u32 index;
  u32 generation;
} Handle;

typedef struct HandlePool {
  void* data;        // raw T array
  u32*  generation;  // per-slot generation
  u32*  free_list;
  u32   free_count;
  u32   count;
  u32   capacity;
  u32   stride;     // sizeof(T)
} HandlePool;

#define handle_zero() (Handle){0};
#define handle_valid(h) ((h).index != 0 || (h).generation != 0)

#define DEFINE_HANDLE_POOL(T, Prefix)                                                              \
                                                                                                   \
  typedef Handle     T##Handle;                                                                    \
  typedef HandlePool T##Pool;                                                                      \
                                                                                                   \
  static inline void Prefix##_pool_init(Arena* arena, T##Pool* pool, u32 capacity) {               \
    pool->data       = push_array_zero(arena, T, capacity);                                        \
    pool->generation = push_array_zero(arena, u32, capacity);                                      \
    pool->free_list  = push_array(arena, u32, capacity);                                           \
    pool->capacity   = capacity;                                                                   \
    pool->count      = 0;                                                                          \
    pool->stride     = sizeof(T);                                                                  \
    /* slot 0 reserved as null — fill free list with 1..capacity-1 */                              \
    pool->free_count = capacity - 1;                                                               \
    for (u32 i = 0; i < capacity - 1; i++)                                                         \
      pool->free_list[i] = capacity - 1 - i; /* pop gives 1,2,3,... */                             \
    /* generation starts at 1 so first valid handle is never zero */                               \
    for (u32 i = 1; i < capacity; i++) pool->generation[i] = 1;                                    \
  }                                                                                                \
                                                                                                   \
  static inline T##Handle Prefix##_pool_alloc(T##Pool* pool, T* data) {                            \
    if (pool->free_count == 0) {                                                                   \
      fprintf(stderr, #Prefix "_pool_alloc: pool full\n");                                         \
      return (T##Handle){ 0 };                                                                     \
    }                                                                                              \
    u32 idx = pool->free_list[--pool->free_count];                                                 \
    if (data) MemoryCopy((u8*)pool->data + idx * sizeof(T), data, sizeof(T));                      \
    pool->count++;                                                                                 \
    return (T##Handle){ idx, pool->generation[idx] };                                              \
  }                                                                                                \
                                                                                                   \
  static inline T* Prefix##_pool_get(T##Pool* pool, T##Handle handle) {                            \
    if (handle.index == 0 || handle.index >= pool->capacity) return NULL;                          \
    if (pool->generation[handle.index] != handle.generation) return NULL;                          \
    return (T*)((u8*)pool->data + handle.index * sizeof(T));                                       \
  }                                                                                                \
                                                                                                   \
  static inline b32 Prefix##_pool_free(T##Pool* pool, T##Handle handle) {                          \
    if (handle.index == 0 || handle.index >= pool->capacity) return false;                         \
    if (pool->generation[handle.index] != handle.generation) return false;                         \
    MemoryZero((u8*)pool->data + handle.index * sizeof(T), sizeof(T));                             \
    pool->generation[handle.index]++; /* invalidate all existing handles */                        \
    pool->free_list[pool->free_count++] = handle.index;                                            \
    pool->count--;                                                                                 \
    return true;                                                                                   \
  }                                                                                                \
                                                                                                   \
  static inline T##Handle Prefix##_handle_zero(void) { return (T##Handle){ 0 }; }                  \
                                                                                                   \
  /* NOT a liveness check -- only "is this not the all-zero sentinel                               \
   * handle", regardless of whether its slot has since been freed. A copy                          \
   * of a handle taken before Prefix_pool_free() reports true here                                 \
   * forever, since this never touches the pool at all. The 3b language's                          \
   * `(handle-valid? h)` builtin does NOT call this -- it compiles to                               \
   * Prefix_pool_get(pool, h) != NULL instead (see codegen.c), the same                             \
   * real index+generation check `handle-deref` uses. This helper is kept                           \
   * only for raw C callers that specifically want the cheap zero-check. */                        \
  static inline b32 Prefix##_handle_valid(T##Handle handle) {                                      \
    return handle.index != 0 || handle.generation != 0;                                            \
  }

#endif
