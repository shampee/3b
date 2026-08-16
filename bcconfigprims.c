// bcconfigprims.c -- Trampoline host imports that mutate a translate.h Config
// struct from a `.3bs` script (see bcconfigprims.h). The config half of the
// layered design: `build`'s generic OS primitives (bcosprims.c) plus these
// Config mutators are what let `3b translate` consume a `.3bs` config script
// alongside the static `config_read` DSL (translate/config.c).
//
// ONE ENTRY PER CALL: config-add-header adds a single header rather than a
// whole `[string]` vector, unlike the static DSL's `(headers ["a.h" "b.h"])`,
// so a config script calls `(add-header "a.h")` once per header. A per-entry
// call is a fine shape for a config script; nothing prevents a vector form if
// one is ever wanted.
//
// STRING CONVENTION AND LIFETIME: the same boxed-{ptr,size}-header convention
// bcosprims.c uses. But unlike bcosprims.c, every string stored into `cfg` is
// first COPIED into `heap` (bc_config_copy_string).
//
// That copy is load-bearing, and removing it is a use-after-free: a string
// ARGUMENT's bytes live in whatever arena the calling script's compile-time
// machinery used, which for script_run_file is the short-lived `fn_temp` arena
// it tears down before returning. Storing that pointer directly leaves every
// Config field pointing at poisoned memory the moment the script finishes.
// `heap` is the long-lived arena the script's top-level caller controls
// (script_run_file passes `ctx_perm()`), which is what a Config must outlive
// its script on -- unlike bcosprims.c's os-file-exists, whose string arguments
// are only read within a single call.
#include "bcconfigprims.h"
#include <stdint.h>

// Unboxes AND COPIES a string argument's bytes into `heap`, never just an
// unboxing read -- see STRING CONVENTION AND LIFETIME at the top of this file
// for why the copy is load-bearing. bcosprims.c's bc_os_unbox_string skips it
// safely only because its strings are read within a single call.
static String8
bc_config_copy_string(Arena* heap, i64 arg) {
  String8* header = (String8*)(intptr_t)arg;
  return str8_copy(heap, *header);
}

// Finds `cfg`'s own DefineGroup for `platform` (from an EARLIER
// config-add-define call for the same platform), or appends and returns
// a fresh one -- mirrors `(ifdef :platform [...])`'s own "one group per
// platform keyword" shape from the static DSL, just built up incrementally
// across possibly-many calls instead of parsed from one form.
static DefineGroup*
bc_config_find_or_create_define_group(Arena* arena, Config* cfg, String8 platform) {
  foreach_index(i, dyn_count(cfg->define_groups)) {
    if (str8_match(cfg->define_groups[i].platform, platform, 0)) return &cfg->define_groups[i];
  }
  DefineGroup group = {0};
  group.platform    = platform;
  dyn_push(arena, cfg->define_groups, group);
  return &cfg->define_groups[dyn_count(cfg->define_groups) - 1];
}

// Same idea as bc_config_find_or_create_define_group, for a `(kind, name)`
// pair -- `kind` is part of the identity since `enum-group Foo` and
// `flags-group Foo` are two DIFFERENT groups in the static DSL (nothing
// requires the two keyword namespaces to stay disjoint).
static ConstGroup*
bc_config_find_or_create_const_group(Arena* arena, Config* cfg, ConstGroupKind kind, String8 name) {
  foreach_index(i, dyn_count(cfg->const_groups)) {
    if (cfg->const_groups[i].kind == kind && str8_match(cfg->const_groups[i].name, name, 0)) {
      return &cfg->const_groups[i];
    }
  }
  ConstGroup group = {0};
  group.kind        = kind;
  group.name        = name;
  dyn_push(arena, cfg->const_groups, group);
  return &cfg->const_groups[dyn_count(cfg->const_groups) - 1];
}

static i64
bc_config_set_package(i64* args, u32 arg_count, Arena* heap, void* userdata) {
  (void)arg_count;
  Config* cfg   = (Config*)userdata;
  cfg->package_name = bc_config_copy_string(heap, args[0]);
  return 0;
}

static i64
bc_config_add_header(i64* args, u32 arg_count, Arena* heap, void* userdata) {
  (void)arg_count;
  Config* cfg = (Config*)userdata;
  dyn_push(heap, cfg->headers, bc_config_copy_string(heap, args[0]));
  return 0;
}

static i64
bc_config_add_define(i64* args, u32 arg_count, Arena* heap, void* userdata) {
  (void)arg_count;
  Config*      cfg      = (Config*)userdata;
  String8      platform = bc_config_copy_string(heap, args[0]);
  DefineGroup* group     = bc_config_find_or_create_define_group(heap, cfg, platform);
  DefineKV     kv        = { bc_config_copy_string(heap, args[1]), bc_config_copy_string(heap, args[2]) };
  dyn_push(heap, group->defines, kv);
  return 0;
}

static i64
bc_config_set_strip_const_prefix(i64* args, u32 arg_count, Arena* heap, void* userdata) {
  (void)arg_count;
  ((Config*)userdata)->strip_const_prefix = bc_config_copy_string(heap, args[0]);
  return 0;
}

static i64
bc_config_set_strip_func_prefix(i64* args, u32 arg_count, Arena* heap, void* userdata) {
  (void)arg_count;
  ((Config*)userdata)->strip_func_prefix = bc_config_copy_string(heap, args[0]);
  return 0;
}

static i64
bc_config_set_strip_struct_prefix(i64* args, u32 arg_count, Arena* heap, void* userdata) {
  (void)arg_count;
  ((Config*)userdata)->strip_struct_prefix = bc_config_copy_string(heap, args[0]);
  return 0;
}

static i64
bc_config_add_type_map(i64* args, u32 arg_count, Arena* heap, void* userdata) {
  (void)arg_count;
  Config*      cfg   = (Config*)userdata;
  TypeMapEntry entry = { bc_config_copy_string(heap, args[0]), bc_config_copy_string(heap, args[1]) };
  dyn_push(heap, cfg->type_map, entry);
  return 0;
}

// The DSL's optional third element is an empty string here rather than an
// absent one -- a script calls `add-pin-type` (which passes "") or
// `add-pin-type-spelled`, and emit reads empty as "stand on c_name".
static i64
bc_config_add_pin_type(i64* args, u32 arg_count, Arena* heap, void* userdata) {
  (void)arg_count;
  Config*      cfg   = (Config*)userdata;
  PinTypeEntry entry = { bc_config_copy_string(heap, args[0]),
                         bc_config_copy_string(heap, args[1]),
                         bc_config_copy_string(heap, args[2]) };
  dyn_push(heap, cfg->pin_type, entry);
  return 0;
}

static i64
bc_config_add_enum_group_member(i64* args, u32 arg_count, Arena* heap, void* userdata) {
  (void)arg_count;
  Config*     cfg   = (Config*)userdata;
  ConstGroup* group = bc_config_find_or_create_const_group(heap, cfg, ConstGroupKind_Enum, bc_config_copy_string(heap, args[0]));
  dyn_push(heap, group->members, bc_config_copy_string(heap, args[1]));
  return 0;
}

static i64
bc_config_add_flags_group_member(i64* args, u32 arg_count, Arena* heap, void* userdata) {
  (void)arg_count;
  Config*     cfg   = (Config*)userdata;
  ConstGroup* group = bc_config_find_or_create_const_group(heap, cfg, ConstGroupKind_Flags, bc_config_copy_string(heap, args[0]));
  dyn_push(heap, group->members, bc_config_copy_string(heap, args[1]));
  return 0;
}

static i64
bc_config_set_enum_group_pattern(i64* args, u32 arg_count, Arena* heap, void* userdata) {
  (void)arg_count;
  Config*     cfg   = (Config*)userdata;
  ConstGroup* group = bc_config_find_or_create_const_group(heap, cfg, ConstGroupKind_Enum, bc_config_copy_string(heap, args[0]));
  group->has_pattern = true;
  group->pattern      = bc_config_copy_string(heap, args[1]);
  return 0;
}

static i64
bc_config_set_flags_group_pattern(i64* args, u32 arg_count, Arena* heap, void* userdata) {
  (void)arg_count;
  Config*     cfg   = (Config*)userdata;
  ConstGroup* group = bc_config_find_or_create_const_group(heap, cfg, ConstGroupKind_Flags, bc_config_copy_string(heap, args[0]));
  group->has_pattern = true;
  group->pattern      = bc_config_copy_string(heap, args[1]);
  return 0;
}

static i64
bc_config_add_force_opaque(i64* args, u32 arg_count, Arena* heap, void* userdata) {
  (void)arg_count;
  Config* cfg = (Config*)userdata;
  dyn_push(heap, cfg->force_opaque, bc_config_copy_string(heap, args[0]));
  return 0;
}

static i64
bc_config_add_construct_outparam(i64* args, u32 arg_count, Arena* heap, void* userdata) {
  (void)arg_count;
  Config*      cfg  = (Config*)userdata;
  OutparamRule rule = {0};
  rule.kind         = OutparamKind_Construct;
  rule.func_name    = bc_config_copy_string(heap, args[0]);
  rule.param_index  = (u32)args[1];
  dyn_push(heap, cfg->outparam_rules, rule);
  return 0;
}

static i64
bc_config_add_mutate_outparam(i64* args, u32 arg_count, Arena* heap, void* userdata) {
  (void)arg_count;
  Config*      cfg  = (Config*)userdata;
  OutparamRule rule = {0};
  rule.kind         = OutparamKind_Mutate;
  rule.func_name    = bc_config_copy_string(heap, args[0]);
  rule.param_index  = (u32)args[1];
  dyn_push(heap, cfg->outparam_rules, rule);
  return 0;
}

static i64
bc_config_add_arena_outparam(i64* args, u32 arg_count, Arena* heap, void* userdata) {
  (void)arg_count;
  Config*      cfg  = (Config*)userdata;
  OutparamRule rule = {0};
  rule.kind              = OutparamKind_Arena;
  rule.func_name         = bc_config_copy_string(heap, args[0]);
  rule.count_param_index = (u32)args[1];
  rule.out_param_index   = (u32)args[2];
  dyn_push(heap, cfg->outparam_rules, rule);
  return 0;
}

static i64
bc_config_add_rename_func(i64* args, u32 arg_count, Arena* heap, void* userdata) {
  (void)arg_count;
  Config*    cfg  = (Config*)userdata;
  RenameRule rule = { bc_config_copy_string(heap, args[0]), bc_config_copy_string(heap, args[1]) };
  dyn_push(heap, cfg->func_renames, rule);
  return 0;
}

static i64
bc_config_add_rename_const(i64* args, u32 arg_count, Arena* heap, void* userdata) {
  (void)arg_count;
  Config*    cfg  = (Config*)userdata;
  RenameRule rule = { bc_config_copy_string(heap, args[0]), bc_config_copy_string(heap, args[1]) };
  dyn_push(heap, cfg->const_renames, rule);
  return 0;
}

static i64
bc_config_add_rename_type(i64* args, u32 arg_count, Arena* heap, void* userdata) {
  (void)arg_count;
  Config*    cfg  = (Config*)userdata;
  RenameRule rule = { bc_config_copy_string(heap, args[0]), bc_config_copy_string(heap, args[1]) };
  dyn_push(heap, cfg->type_renames, rule);
  return 0;
}

// The three `(rename-*-pattern "regex" "template")` forms. One primitive
// each rather than one taking a "which list" selector: the bytecode side's
// host-import signatures are checked by name (see bc_host_import_table_add),
// so a selector would turn a config-script typo into a silently-wrong
// rename list instead of an unresolved-import error.
static i64
bc_config_add_rename_const_pattern(i64* args, u32 arg_count, Arena* heap, void* userdata) {
  (void)arg_count;
  Config*       cfg = (Config*)userdata;
  RenamePattern p   = { bc_config_copy_string(heap, args[0]), bc_config_copy_string(heap, args[1]) };
  dyn_push(heap, cfg->const_rename_patterns, p);
  return 0;
}

static i64
bc_config_add_rename_func_pattern(i64* args, u32 arg_count, Arena* heap, void* userdata) {
  (void)arg_count;
  Config*       cfg = (Config*)userdata;
  RenamePattern p   = { bc_config_copy_string(heap, args[0]), bc_config_copy_string(heap, args[1]) };
  dyn_push(heap, cfg->func_rename_patterns, p);
  return 0;
}

static i64
bc_config_add_rename_type_pattern(i64* args, u32 arg_count, Arena* heap, void* userdata) {
  (void)arg_count;
  Config*       cfg = (Config*)userdata;
  RenamePattern p   = { bc_config_copy_string(heap, args[0]), bc_config_copy_string(heap, args[1]) };
  dyn_push(heap, cfg->type_rename_patterns, p);
  return 0;
}

static i64
bc_config_add_exclude_func(i64* args, u32 arg_count, Arena* heap, void* userdata) {
  (void)arg_count;
  Config* cfg = (Config*)userdata;
  dyn_push(heap, cfg->excluded_funcs, bc_config_copy_string(heap, args[0]));
  return 0;
}

// A flag rather than a value: the DSL's `(skip-deprecated)` takes no operand,
// so the script wrapper takes none either, and calling it is the whole signal.
static i64
bc_config_set_skip_deprecated(i64* args, u32 arg_count, Arena* heap, void* userdata) {
  (void)args; (void)arg_count; (void)heap;
  ((Config*)userdata)->skip_deprecated = true;
  return 0;
}

static i64
bc_config_add_exclude_const(i64* args, u32 arg_count, Arena* heap, void* userdata) {
  (void)arg_count;
  Config* cfg = (Config*)userdata;
  dyn_push(heap, cfg->excluded_consts, bc_config_copy_string(heap, args[0]));
  return 0;
}

void
bc_register_config_primitives(BcHostImportTable* table, Arena* arena, Config* cfg) {
  table->userdata = cfg;

  TypeRef string_ty = {0}; string_ty.kind = TypeKind_String;
  TypeRef i32_ty    = {0}; i32_ty.kind    = TypeKind_I32;
  TypeRef void_ty    = {0}; void_ty.kind    = TypeKind_Void;

  TypeRef* one_string = push_array(arena, TypeRef, 1);
  one_string[0] = string_ty;
  TypeRef* two_strings = push_array(arena, TypeRef, 2);
  two_strings[0] = string_ty; two_strings[1] = string_ty;
  TypeRef* three_strings = push_array(arena, TypeRef, 3);
  three_strings[0] = string_ty; three_strings[1] = string_ty; three_strings[2] = string_ty;
  TypeRef* string_i32 = push_array(arena, TypeRef, 2);
  string_i32[0] = string_ty; string_i32[1] = i32_ty;
  TypeRef* string_i32_i32 = push_array(arena, TypeRef, 3);
  string_i32_i32[0] = string_ty; string_i32_i32[1] = i32_ty; string_i32_i32[2] = i32_ty;

  bc_host_import_table_add(table, arena, str8_lit("config-set-package"), bc_config_set_package, one_string, 1, void_ty);
  bc_host_import_table_add(table, arena, str8_lit("config-add-header"), bc_config_add_header, one_string, 1, void_ty);
  bc_host_import_table_add(table, arena, str8_lit("config-add-define"), bc_config_add_define, three_strings, 3, void_ty);
  bc_host_import_table_add(table, arena, str8_lit("config-set-strip-const-prefix"), bc_config_set_strip_const_prefix, one_string, 1, void_ty);
  bc_host_import_table_add(table, arena, str8_lit("config-set-strip-func-prefix"), bc_config_set_strip_func_prefix, one_string, 1, void_ty);
  bc_host_import_table_add(table, arena, str8_lit("config-set-strip-struct-prefix"), bc_config_set_strip_struct_prefix, one_string, 1, void_ty);
  bc_host_import_table_add(table, arena, str8_lit("config-add-type-map"), bc_config_add_type_map, two_strings, 2, void_ty);
  bc_host_import_table_add(table, arena, str8_lit("config-add-pin-type"), bc_config_add_pin_type, three_strings, 3, void_ty);
  bc_host_import_table_add(table, arena, str8_lit("config-add-enum-group-member"), bc_config_add_enum_group_member, two_strings, 2, void_ty);
  bc_host_import_table_add(table, arena, str8_lit("config-add-flags-group-member"), bc_config_add_flags_group_member, two_strings, 2, void_ty);
  bc_host_import_table_add(table, arena, str8_lit("config-set-enum-group-pattern"), bc_config_set_enum_group_pattern, two_strings, 2, void_ty);
  bc_host_import_table_add(table, arena, str8_lit("config-set-flags-group-pattern"), bc_config_set_flags_group_pattern, two_strings, 2, void_ty);
  bc_host_import_table_add(table, arena, str8_lit("config-add-force-opaque"), bc_config_add_force_opaque, one_string, 1, void_ty);
  bc_host_import_table_add(table, arena, str8_lit("config-add-construct-outparam"), bc_config_add_construct_outparam, string_i32, 2, void_ty);
  bc_host_import_table_add(table, arena, str8_lit("config-add-mutate-outparam"), bc_config_add_mutate_outparam, string_i32, 2, void_ty);
  bc_host_import_table_add(table, arena, str8_lit("config-add-arena-outparam"), bc_config_add_arena_outparam, string_i32_i32, 3, void_ty);
  bc_host_import_table_add(table, arena, str8_lit("config-add-rename-func"), bc_config_add_rename_func, two_strings, 2, void_ty);
  bc_host_import_table_add(table, arena, str8_lit("config-add-rename-const"), bc_config_add_rename_const, two_strings, 2, void_ty);
  bc_host_import_table_add(table, arena, str8_lit("config-add-rename-type"), bc_config_add_rename_type, two_strings, 2, void_ty);
  bc_host_import_table_add(table, arena, str8_lit("config-add-rename-const-pattern"), bc_config_add_rename_const_pattern, two_strings, 2, void_ty);
  bc_host_import_table_add(table, arena, str8_lit("config-add-rename-func-pattern"), bc_config_add_rename_func_pattern, two_strings, 2, void_ty);
  bc_host_import_table_add(table, arena, str8_lit("config-add-rename-type-pattern"), bc_config_add_rename_type_pattern, two_strings, 2, void_ty);
  bc_host_import_table_add(table, arena, str8_lit("config-add-exclude-func"), bc_config_add_exclude_func, one_string, 1, void_ty);
  bc_host_import_table_add(table, arena, str8_lit("config-add-exclude-const"), bc_config_add_exclude_const, one_string, 1, void_ty);
  bc_host_import_table_add(table, arena, str8_lit("config-set-skip-deprecated"), bc_config_set_skip_deprecated, NULL, 0, void_ty);
}

// The CLI side of this lives in translate/translate.c's load_config_script,
// which builds a host_imports table from both bc_register_os_primitives and
// bc_register_config_primitives, runs the script, and feeds the populated
// Config to cwalk.c/emit.c exactly as config_read's result is fed. The
// primitives themselves are tested directly in test/bcconfigprims_test.c.
