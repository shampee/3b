////////////////////////////////
//~ Strings

typedef struct bbb_String8 { u8* str; u64 size; } bbb_String8;

typedef u32 bbb_StringMatchFlags;
enum {
  bbb_StringMatchFlag_CaseInsensitive  = (1 << 0),
  bbb_StringMatchFlag_RightSideSloppy  = (1 << 1),
  bbb_StringMatchFlag_SlashInsensitive = (1 << 2),
};

// bbb_Rng1u64 itself is declared earlier, in the Context/lane section --
// bbb_lane_range needs it and gets emitted first.

#define bbb_str8_lit(S)          bbb_str8((u8*)(S), sizeof(S) - 1)
#define bbb_str8_varg(S)         (int)((S).size), ((S).str)
#define bbb_str8_fmt             "%.*s"

bbb_String8 bbb_str8(u8* str, u64 size);
bbb_String8 bbb_str8_range(u8* first, u8* one_past_last);
bbb_String8 bbb_str8_cstring(char* c);

u64     bbb_str8_hash(bbb_String8 s); // FNV-1a -- used by `(Map string V)`/`(Set string)`'s
                                        // monomorphized hash table (see bbb_hashtable.h)
b32     bbb_str8_match(bbb_String8 a, bbb_String8 b, bbb_StringMatchFlags flags);
i32     bbb_str8_compare(bbb_String8 a, bbb_String8 b); // strcmp-style 3-way lexicographic compare
                                        // (byte values, always exact -- no bbb_StringMatchFlags,
                                        // same "no hidden fuzziness" reasoning as bbb_str8_match's
                                        // own comment) -- backs the language's `<`/`<=`/`>`/`>=` on
                                        // `string` operands (see codegen.c's ordering-op case)
u64     bbb_str8_find_needle(bbb_String8 string, u64 start_pos, bbb_String8 needle, bbb_StringMatchFlags flags);
u64     bbb_str8_find_needle_reverse(bbb_String8 string, u64 start_pos, bbb_String8 needle, bbb_StringMatchFlags flags);
b32     bbb_str8_starts_with(bbb_String8 string, bbb_String8 start, bbb_StringMatchFlags flags);
b32     bbb_str8_ends_with(bbb_String8 string, bbb_String8 end, bbb_StringMatchFlags flags);

bbb_String8 bbb_str8_substr(bbb_String8 str, bbb_Rng1u64 range);
bbb_String8 bbb_str8_prefix(bbb_String8 str, u64 size);
bbb_String8 bbb_str8_skip(bbb_String8 str, u64 amt);
bbb_String8 bbb_str8_postfix(bbb_String8 str, u64 size);
bbb_String8 bbb_str8_chop(bbb_String8 str, u64 amt);

bbb_String8 bbb_str8_cat(bbb_Arena* arena, bbb_String8 s1, bbb_String8 s2);
bbb_String8 bbb_str8_copy(bbb_Arena* arena, bbb_String8 s);
char*       bbb_cstring_str8(bbb_Arena* arena, bbb_String8 s);
bbb_String8 bbb_str8fv(bbb_Arena* arena, char* fmt, va_list args);
bbb_String8 bbb_str8f(bbb_Arena* arena, char* fmt, ...);

// `string-match` builtin -- thin bridge over bbb_str8_match, with a clean
// bool/u32 signature instead of exposing b32/bbb_StringMatchFlags directly
// to generated code (those are C-side implementation details, not language
// types). flags is a raw bbb_StringMatchFlags bitmask.
bool bbb_string_match(bbb_String8 a, bbb_String8 b, u32 flags);

// `string-find`/`string-find-reverse`/`string-starts-with`/`string-ends-with`
// builtins -- thin bridges over bbb_str8_find_needle/bbb_str8_find_needle_reverse/
// bbb_str8_starts_with/bbb_str8_ends_with, same bbb_StringMatchFlags-as-raw-u32
// rationale as `string-match` above.
u64  bbb_string_find(bbb_String8 s, bbb_String8 needle, u64 start_pos, u32 flags);
u64  bbb_string_find_reverse(bbb_String8 s, bbb_String8 needle, u64 start_pos, u32 flags);
bool bbb_string_starts_with(bbb_String8 s, bbb_String8 prefix, u32 flags);
bool bbb_string_ends_with(bbb_String8 s, bbb_String8 suffix, u32 flags);

////////////////////////////////
//~ `string-to-i32`/`string-to-i64`/`string-to-u32`/`string-to-u64`/
// `string-to-f32`/`string-to-f64` builtins -- each backs a `(bool T)`
// multi-value return in generated code (see codegen.c's TypedNodeKind_
// ParseNumber case, which bridges one of these fixed-shape results into
// the caller's own synthesized AnonReturn struct). `ok` is false for
// anything malformed: empty input, a sign with no digits, any non-digit
// byte before the string ends (no silent partial parse of "12abc"), or a
// magnitude that doesn't fit the target width -- never a silent 0/garbage
// fallback the way base.c's OWN (compiler-internal, non-validating)
// u64_from_str8/i64_from_str8 family works.
//
// Integer parsing is hand-rolled, byte-indexed over `bbb_String8`'s own
// `str`/`size` pair -- deliberately NOT strtoll, since a String8 is not
// guaranteed null-terminated (it's a fat pointer/view, possibly into the
// middle of a larger buffer) and handing its raw `str` to a C string
// function could read (or successfully "parse") past the intended size.
// Float parsing DOES use strtod/strtof, since implementing a correct
// decimal<->binary float parser by hand is its own substantial undertaking
// -- the input is first copied into a small fixed-size, guaranteed-
// null-terminated stack buffer (any string too long to fit, `>= 64`
// bytes, is already implausible for a real numeric literal and treated as
// a parse failure) specifically so strtod/strtof never reads past what
// the caller's `bbb_String8` actually points at.
typedef struct bbb_ParseI64Result { b32 ok; i64 value; } bbb_ParseI64Result;
typedef struct bbb_ParseI32Result { b32 ok; i32 value; } bbb_ParseI32Result;
typedef struct bbb_ParseU64Result { b32 ok; u64 value; } bbb_ParseU64Result;
typedef struct bbb_ParseU32Result { b32 ok; u32 value; } bbb_ParseU32Result;
typedef struct bbb_ParseF64Result { b32 ok; f64 value; } bbb_ParseF64Result;
typedef struct bbb_ParseF32Result { b32 ok; f32 value; } bbb_ParseF32Result;

bbb_ParseI64Result bbb_string_to_i64(bbb_String8 s);
bbb_ParseI32Result bbb_string_to_i32(bbb_String8 s);
bbb_ParseU64Result bbb_string_to_u64(bbb_String8 s);
bbb_ParseU32Result bbb_string_to_u32(bbb_String8 s);
bbb_ParseF64Result bbb_string_to_f64(bbb_String8 s);
bbb_ParseF32Result bbb_string_to_f32(bbb_String8 s);
