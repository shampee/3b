////////////////////////////////
//~ OS Detection
//
// runtime/ is deliberately self-contained (no #include of base.h -- see
// this module set's own top-level notes), so it needs its own OS
// detection rather than reusing base.h's OS_WINDOWS/OS_MAC/OS_LINUX --
// used by bbb_arena.c (VirtualAlloc vs mmap) and bbb_thread.c (Win32
// threads/sync vs pthreads).

#if defined(_WIN32)
# define BBB_OS_WINDOWS 1
#elif defined(__APPLE__)
# define BBB_OS_MAC 1
#else
# define BBB_OS_LINUX 1
#endif

////////////////////////////////
//~ Basic Types
//
// NOT bbb_-prefixed, unlike everything else in runtime/ -- these ARE the C
// types 3b's own primitive type keywords (i32, u64, f32, bool, ...) compile
// down to 1:1 (see c_type_from_typeref_base in codegen.c), not merely names
// borrowed from base.h. Renaming them would mean renaming the language's
// own primitive type spellings, which is a much bigger and unrelated
// change than namespacing the runtime support library.

typedef unsigned char    b8;
typedef unsigned short   b16;
typedef unsigned int     b32;
typedef unsigned long    b64;

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

typedef float             f32;
typedef double            f64;

////////////////////////////////
//~ Size Helpers

#define bbb_KB(n)  (((u64)(n)) << 10)
#define bbb_MB(n)  (((u64)(n)) << 20)
#define bbb_GB(n)  (((u64)(n)) << 30)

////////////////////////////////
//~ Memory Operations

#define bbb_MemoryCopy(dst, src, size)   memmove((dst), (src), (size))
#define bbb_MemorySet(dst, byte, size)   memset((dst), (byte), (size))
#define bbb_MemoryCompare(a, b, size)    memcmp((a), (b), (size))
#define bbb_MemoryMatch(a, b, z)         (bbb_MemoryCompare((a), (b), (z)) == 0)
#define bbb_MemoryZero(s, z)             memset((s), 0, (z))
#define bbb_MemoryZeroStruct(s)          bbb_MemoryZero((s), sizeof(*(s)))

////////////////////////////////
//~ Debug / Xassert

#if defined(__clang__) || defined(__GNUC__) || defined(__GNUG__)
# define bbb_Trap() __builtin_trap()
#else
# include <signal.h>
# define bbb_Trap() raise(SIGTRAP)
#endif

#ifdef XDEBUG
# include <assert.h>
# define bbb_xassert assert
#else
# define bbb_xassert (void)
#endif

////////////////////////////////
//~ Utility Macros (scalar math / language builtins)

#define bbb_Min(A, B)         (((A) < (B)) ? (A) : (B))
#define bbb_Max(A, B)         (((A) > (B)) ? (A) : (B))
#define bbb_ClampTop(A, X)    bbb_Min(A, X)
#define bbb_ClampBot(X, B)    bbb_Max(X, B)
#define bbb_Clamp(A, X, B)    (((X) < (A)) ? (A) : ((X) > (B)) ? (B) : (X))
#define bbb_Abs(X)            (((X) < 0) ? -(X) : (X))

#define bbb_Swap(T, a, b)     do { T t__ = a; a = b; b = t__; } while (0)

#if defined(_MSC_VER) || defined(__clang__)
# define bbb_AlignOf(T) __alignof(T)
#elif defined(__GNUC__) || defined(__GNUG__)
# define bbb_AlignOf(T) __alignof__(T)
#else
# error bbb_AlignOf not defined for this compiler.
#endif

#define bbb_AlignPow2(x, b)      (((x) + (b) - 1) & (~((b) - 1)))
#define bbb_AlignDownPow2(x, b)  ((x) & (~((b) - 1)))
#define bbb_AlignPadPow2(x, b)   ((0 - (x)) & ((b) - 1))
#define bbb_IsPow2(x)            ((x) != 0 && ((x) & ((x) - 1)) == 0)
#define bbb_IsPow2OrZero(x)      ((((x) - 1) & (x)) == 0)

#define bbb_foreach_index(it, count) \
  for (u64 it = 0; it < (count); it += 1)

#define bbb_foreach_index_reverse(it, count) \
  for (u64 it = (count) - 1; it != (u64)-1; it -= 1)
