#ifndef TRANSLATE_H
#define TRANSLATE_H
#include "3b.h"
#include "file.h"

////////////////////////////////
//~ Config: the parsed form of the translator's config file. Read with 3b's own
// lexer and parser into the generic atom/list/vector/map AST, then walked into
// these plain structs. It is never compiled as a program.

typedef struct DefineKV {
  String8 name;
  String8 value;
} DefineKV;

// One `:platform [NAME VALUE ...]` group inside `(ifdef ...)`. `:common`
// applies unconditionally; any other keyword applies only under a matching
// `--platform`.
typedef struct DefineGroup {
  String8   platform; // "common", "windows", "linux", "macos", ... (without the leading ':')
  DefineKV* defines;  // dyn array
} DefineGroup;

typedef struct TypeMapEntry {
  String8 c_name;
  String8 b3_name;
} TypeMapEntry;

// One `pin-type` entry. `c_spelling` is the C type the emitted typedef stands
// on, and is empty for the common case where that is `c_name` itself. It
// differs when the name a binding wants to pin is NOT visible in the generated
// header -- SDL's `Uint64` needs `typedef uint64_t sdl_Uint64;`, since the .h
// sees <stdint.h> through 3b_runtime.h but never SDL's own headers. See
// emit_pinned_type_aliases.
typedef struct PinTypeEntry {
  String8 c_name;
  String8 b3_name;
  String8 c_spelling;
} PinTypeEntry;

typedef enum ConstGroupKind {
  ConstGroupKind_Enum,
  ConstGroupKind_Flags,
} ConstGroupKind;

// `(enum-group Name [Member ...])` or `(enum-group Name (match "regex"))`.
// Exactly one of `members`/`pattern` is populated, per `has_pattern`. Both are
// matched against the constant's name after strip-const-prefix is applied, so
// config authors write the logical name rather than the raw C spelling.
typedef struct ConstGroup {
  ConstGroupKind kind;
  String8        name;
  String8*       members; // dyn array, used when !has_pattern
  b32            has_pattern;
  String8        pattern; // POSIX ERE, used when has_pattern
} ConstGroup;

typedef enum OutparamKind {
  OutparamKind_Construct,
  OutparamKind_Mutate,
  OutparamKind_Arena,
} OutparamKind;

// `(construct-outparam name [param N])`, `(mutate-outparam name [param N])` or
// `(arena-outparam name [count-param N out-param M])`. `func_name` matches the
// function's final 3b name, after mangling and renaming.
typedef struct OutparamRule {
  OutparamKind kind;
  String8      func_name;
  u32          param_index;       // Construct / Mutate
  u32          count_param_index; // Arena
  u32          out_param_index;   // Arena
} OutparamRule;

typedef struct RenameRule {
  String8 from;
  String8 to;
} RenameRule;

// `(rename-const-pattern "^CODEC_ID_(.*)$" "{1:pascal}")` and its func/type
// siblings: one rule renaming a whole family of names. A C enum with hundreds
// of members wanting identical mechanical treatment -- drop a prefix, re-case
// the rest -- would otherwise need one hand-written `(rename-const ...)` line
// each.
//
// `pattern` is a POSIX ERE matched against whichever name the corresponding
// explicit rename form matches; see Config's per-field comments. `replacement`
// is a template: literal text passes through, `{N}` interpolates capture group
// N (`{0}` is the whole match), `{N:style}` re-cases it first (styles are
// listed at rename_patterns_apply), and `{{` is a literal `{`.
//
// Explicit renames always win over patterns, and among patterns the first
// match in config file order wins. That precedence is what lets a config
// pattern-rename a family and then hand-write the few members whose mechanical
// name reads wrong.
typedef struct RenamePattern {
  String8 pattern;
  String8 replacement;
} RenamePattern;

typedef struct Config {
  String8      package_name;
  String8*     headers;         // dyn array of header paths, e.g. "GL/gl.h"

  DefineGroup* define_groups;   // dyn array, from `(ifdef ...)`

  String8      strip_const_prefix;  // e.g. "GL_", empty String8 if unset
  String8      strip_func_prefix;   // e.g. "gl"
  String8      strip_struct_prefix; // e.g. "SDL_"; applied to struct/union/enum/typedef/
                                      // opaque-handle names, at the declaration and at every
                                      // reference alike so the two stay in sync

  TypeMapEntry* type_map;        // dyn array

  PinTypeEntry* pin_type;        // dyn array. Each entry becomes
                                  // `(alias C_NAME B3_NAME "C_SPELLING")`, and every use of C_NAME
                                  // resolves to that alias instead of to B3_NAME directly.
                                  // See emit_pinned_type_aliases for why a binding wants this.

  ConstGroup*  const_groups;     // dyn array, enum-group and flags-group together in
                                  // file order, which decides `match` resolution

  String8*     force_opaque;     // dyn array of C type names

  OutparamRule* outparam_rules;  // dyn array

  RenameRule*  func_renames;     // dyn array
  RenameRule*  const_renames;    // dyn array, `(rename-const [...])`. `.from` matches a #define
                                  // constant's or C enum member's logical (post-strip-const-prefix)
                                  // name, as enum-group membership does; `.to` is the 3b name to
                                  // emit. Renaming is the only lever over a real C enum's members:
                                  // unlike #define constants, they always emit together as one
                                  // `(enum Name [...])` in C declaration order.
  RenameRule*  type_renames;     // dyn array, `(rename-type [...])`. `.from` is a struct/union/
                                  // enum/typedef's RAW C name -- strip-struct-prefix is the default
                                  // this overrides, so matching post-strip would be circular.
                                  // Applies at the declaration and every reference alike.

  // Pattern equivalents of the three explicit rename forms above, matched
  // against the same names their siblings are: logical (post-strip-const-prefix)
  // names for consts, raw C names for funcs and types. See RenamePattern for
  // precedence.
  RenamePattern* const_rename_patterns; // dyn array, `(rename-const-pattern "re" "tmpl")`
  RenamePattern* func_rename_patterns;  // dyn array, `(rename-func-pattern  "re" "tmpl")`
  RenamePattern* type_rename_patterns;  // dyn array, `(rename-type-pattern  "re" "tmpl")`

  String8*     excluded_funcs;   // dyn array, C names
  String8*     excluded_consts;  // dyn array, C names
  b32          skip_deprecated;  // `(skip-deprecated)` -- drop every function the header marks
                                  // deprecated, rather than wrapping it. Opt-in, since dropping
                                  // them is an API change: the wrapper's own body is what calls
                                  // the deprecated function, so binding one means a
                                  // -Wdeprecated-declarations warning in generated code that no
                                  // call site of yours is responsible for.
} Config;

// Reads a config file's full text into a Config. `platform` selects which
// `(ifdef ...)` groups are active alongside the always-active `:common`;
// NULL or empty means common-only. Errors go to stderr, and a false return
// leaves `out_config` partially populated.
b32 config_read(Arena* arena, String8 src, const char* platform, Config* out_config, u32 file_id);
void config_print(Config* config);

////////////////////////////////
//~ Pattern renaming (rename.c), the engine behind the `(rename-*-pattern ...)`
// config forms. Separate from emit.c: it is pure string and regex work with no
// notion of C declarations, and emit.c's `(match "...")` grouping predicate
// shares its compiled-regex cache.

// Applies the first rule in `patterns` (a dyn array, possibly NULL) whose regex
// matches `name`, expanding its template. Returns an empty String8 when no rule
// matched; a rule that matches but produces an empty name is an error, reported
// and skipped.
//
// Template styles, applied to one capture group via `{N:style}`:
//   pascal  MPEG_TS -> MpegTs      camel   MPEG_TS -> mpegTs
//   snake   MpegTs  -> mpeg_ts     kebab   MpegTs  -> mpeg-ts
//   upper / lower              ASCII case mapping, separators kept
// The four word-based styles split on `_`, `-`, any non-alphanumeric, and case
// boundaries, with digits attaching to the word before them (`PCM_S16LE` ->
// `Pcm` + `S16le`) -- the same splitting emit.c's camel_to_kebab uses.
String8 rename_patterns_apply(Arena* arena, RenamePattern* patterns, String8 name);

// POSIX ERE match test over the same compiled-regex cache
// rename_patterns_apply uses. An invalid pattern is reported once, then treated
// as never matching.
b32 rename_regex_matches(String8 pattern, String8 name);

////////////////////////////////
//~ Extracted C declarations: libclang's view of the configured headers, reduced
// to what the translator needs. The shape mirrors 3b's own TypeRef -- Pointer
// and Array wrap a pointee, Named holds a literal C spelling -- but stays a
// separate type, since this side also tracks C-only facts (opaque-handle-ness,
// function-pointer-ness) that mean nothing once translated.

typedef enum CTypeKind {
  CTypeKind_Void,
  CTypeKind_Named,          // a builtin ("unsigned int") or a tag/typedef name ("GLenum",
                             // "struct Foo"), as a literal C spelling. Resolved against
                             // Config.type_map, the opaque set and the record table in
                             // emit.c, not here.
  CTypeKind_Pointer,        // .pointee set
  CTypeKind_Array,          // .pointee = element type, .array_count = size, 0 for anything
                             // but a ConstantArray, which is treated as a bare pointer
                             // per C's own array-decays-to-pointer rule
  CTypeKind_FunctionPointer, // a C function pointer, or the rarer bare function type reached
                             // through a typedef. Translated to 3b's `(fn [name type ...] Ret)`
                             // (TypeKind_Fn) when the signature allows; 3b has no variadic fn
                             // type, so those are left for emit.c's
                             // type_references_unresolvable to handle like any other
                             // uncaptured type, via exclude-func or an opaque fallback.
} CTypeKind;

typedef struct CType CType;
struct CType {
  CTypeKind kind;
  String8   name;    // Named only
  CType*    pointee;  // Pointer / Array
  u64       array_count; // Array only
  b32       is_const; // qualifier at THIS level, mirroring 3b's `(const T)` binding to the base type
  b32       pointee_is_incomplete; // Pointer only: the pointee is void, or a record with no
                                    // definition anywhere in the TU. This is the signal
                                    // opaque-handle detection runs on.

  // FunctionPointer only: the pointed-to function's signature. Param names are
  // not tracked, since C function types do not carry them and 3b's
  // `(fn [name type ...] Ret)` ignores them; emit.c synthesizes throwaway names
  // to satisfy that form's name/type-pair shape.
  CType*    fn_params;      // arena array, length fn_param_count (NULL when 0)
  u32       fn_param_count;
  CType*    fn_return;      // boxed return type, never NULL when kind == FunctionPointer
  b32       fn_is_variadic; // 3b's `fn` type has no `...`, so a variadic function pointer is
                            // unsupported whatever fn_params/fn_return hold
};

typedef struct CField {
  String8 name;
  CType   type;
  u64     c_offset;    // byte offset within the owning record, as libclang measured it.
                        // Meaningless when is_bitfield (a bitfield's storage is a bit range,
                        // not a byte boundary) or when the owner's layout_unknown is set.
  b32     is_bitfield; // `int flags : 3` -- has no 3b spelling at all, so the emitted mirror
                        // silently gives it a whole field's worth of storage. See
                        // verify_record_layouts.
} CField;

// One enumerator of a real C `enum`, as opposed to a `#define` constant (see
// CConstant). `value_text` is always decimal, resolved from clang's signed
// enumerator value, so negative enumerators work -- unlike CConstant, which
// passes source text through.
typedef struct CEnumerator {
  String8 name;
  String8 value_text;
} CEnumerator;

// A real C `enum Name { ... }` or `typedef enum { ... } Name;`. Anonymous ones
// never arrive as a CEnum: with no type name for anything to reference, cwalk.c
// pushes their enumerators into CUnit.constants, as it does for a `#define`.
typedef struct CEnum {
  String8      name;
  CEnumerator* enumerators; // dyn array
} CEnum;

// A struct or union. Anonymous ones -- nested `struct { ... }` field types, or
// the `struct { ... } Name;` typedef pattern -- get a synthesized `AnonN` name
// at extraction time, flagged by `is_anonymous`. That matches 3b's own
// anonymous-type-position convention, so emit.c can use it directly.
typedef struct CRecord {
  String8  name;
  b32      is_union;
  b32      is_anonymous;
  b32      is_complete;   // false = forward-declared only, so an opaque candidate
  CField*  fields;         // dyn array, only meaningful when is_complete
  b32      is_packed;
  u32      align;          // 0 = unset/default

  // What libclang measured for the REAL C record, which is what the emitted 3b
  // mirror has to reproduce byte for byte -- the generated wrapper passes a
  // pointer to the mirror straight into a C function expecting the original.
  // verify_record_layouts is what actually compares the two; these are its
  // inputs. `layout_unknown` covers the cases libclang declines to measure
  // (an incomplete or dependent type), where there is nothing to compare
  // against and the check stands down.
  u64      c_size;
  u64      c_align;
  b32      layout_unknown;
  u32      bitfield_count;   // members this mirror cannot represent at all
  u32      anon_member_count; // C11 anonymous struct/union members, which are dropped

  // A `typedef ... Name;` in the source spells this record under its bare
  // name, so `Name` alone is a valid C type there. Set by capture_typedef's
  // case-1 dedup, which is the only place that can observe it: BOTH
  // `typedef struct { ... } Name;` and `typedef struct Name { ... } Name;`
  // arrive here as a record named `Name`, and only the second also has a
  // `struct Name` tag to spell it with. Without this, the first would be
  // written out as `struct Name` -- which C reads as a brand-new incomplete
  // type, not the struct at all. See c_decl_spelling / c_type_text.
  b32      has_typedef_name;
} CRecord;

// `is_opaque_handle` is true when `underlying` is a Pointer to an incomplete
// type or to void -- the mechanically detectable opaque-handle case. emit.c
// turns those into `(alias Name any)` and everything else into an ordinary type
// reference.
typedef struct CTypedef {
  String8 name;
  CType   underlying;
  b32     is_opaque_handle;
} CTypedef;

typedef struct CParam {
  String8 name; // may be empty; C allows unnamed prototype params
  CType   type;
} CParam;

typedef struct CFunction {
  String8   name;
  CType     return_type;
  CParam*   params;             // dyn array
  b32       is_variadic;
  b32       has_function_pointer; // return type or a param is directly a CTypeKind_FunctionPointer.
                                   // Informational, for cwalk_print_unit's dump: emit.c decides
                                   // exclusion from the signature itself, via
                                   // type_references_unresolvable.
  b32       is_deprecated;        // the declaration carries `__attribute__((deprecated))` or the
                                   // equivalent. Acted on only under Config.skip_deprecated.
} CFunction;

// One `#define NAME VALUE` surviving the macro filter; see cwalk_extract's
// CXCursor_MacroDefinition handling in cwalk.c.
typedef struct CConstant {
  String8 name;
  String8 value_text; // raw literal text, decimal-or-hex normalized by the caller
  b32     is_hex;
} CConstant;

typedef struct CUnit {
  CRecord*   records;   // dyn array, structs and unions, named and anonymous alike
  CTypedef*  typedefs;  // dyn array
  CFunction* functions; // dyn array
  CConstant* constants; // dyn array, #define constants and anonymous enums' enumerators
  CEnum*     enums;      // dyn array, named enums only (see CEnum)
  u32        next_anon_id;

  // Stack, innermost last, of the records whose fields are being collected
  // right now. A record can reach itself part-way through its own capture: a
  // function-pointer field whose parameter list names the enclosing struct,
  // e.g. cgltf's
  //   cgltf_result (*read)(..., const struct cgltf_file_options*, ...);
  // inside cgltf_file_options. `records` alone cannot break that cycle, since a
  // record is not appended there until its fields are finished, and the
  // unguarded recursion runs the stack out.
  //
  // Kept separate rather than appending an incomplete record to `records`
  // early: that would also break the cycle, but would emit each record before
  // the anonymous ones nested inside it rather than after, reshuffling every
  // already-generated binding the next time it was regenerated.
  String8*   records_in_progress; // dyn array
} CUnit;

// Parses the configured headers as one synthetic translation unit, each pulled
// in via `#include` so libclang produces a single fully-resolved AST with no
// order-of-inclusion surprises. Collects every top-level declaration -- record,
// typedef, function, enum and `#define` constant alike -- located inside one of
// `cfg->headers` rather than in a header those include transitively.
//
// `platform` selects which `(ifdef ...)` groups become `-D` flags, by the same
// rule as config_read. Returns false on a libclang parse failure, with clang's
// own diagnostics on stderr.
b32 cwalk_extract(Arena* arena, Config* cfg, const char* platform, CUnit* out_unit);

void cwalk_print_unit(CUnit* unit);

////////////////////////////////
//~ Emission: applies the config's rules -- renames, exclusions, constant
// grouping, force-opaque, outparam wrappers -- and writes 3b source for `unit`
// to `out`. A false return means naming collisions were found and the config
// needs fixing; output is still written (see emit.c's try_register_name).
//
// `byval_out` receives the generated C bridge header (see emit.c's "By-value
// struct bridge"); pass NULL to translate without one, which costs only the
// functions that would have needed it. `*out_byval_count`, when given, receives
// how many shims were written -- zero means `byval_out` holds nothing but a
// preamble and the caller should not keep the file.
b32 emit_package(Config* cfg, CUnit* unit, FILE* out, FILE* byval_out, u32* out_byval_count);

// Verifies that the struct/union mirrors emit_package would write actually
// match the C records they stand in for, byte for byte -- see emit.c's own
// "Layout verification" note for why that is the load-bearing assumption of
// every translated binding, and why a mismatch is fatal rather than a warning.
// Reports each failure to stderr and returns false if there were any; the
// caller then writes nothing.
b32 verify_record_layouts(Config* cfg, CUnit* unit);

// Verifies that no type this package emits mangles to a C name the translated
// headers already define -- see emit.c's own "Generated-name collision" note.
// Same contract as verify_record_layouts: reports to stderr, returns false if
// any collided, and the caller then writes nothing.
b32 verify_type_names(Config* cfg, CUnit* unit);

// `3b translate --names`: writes a `C name -> 3b name -> origin` table for every
// constant, enum member, type and function, without emitting a package. This is
// the audit half of pattern renaming, since the only way to find the names a
// pattern guesses wrong is to read what it produced. Names two declarations
// both land on are called out at the end.
void emit_name_report(Config* cfg, CUnit* unit, FILE* out);

////////////////////////////////
//~ Entry point for `3b translate ...`, the counterpart to main.c's
// format_file_cmd. `argv[0]` should be the mode name, so usage messages match
// how the mode was invoked; main() shifts argv by one first, as for any
// subcommand.
int translate_main(int argc, char** argv);

#endif
