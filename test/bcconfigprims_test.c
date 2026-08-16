// bcconfigprims_test.c -- validates the Config-mutation host imports
// (bcconfigprims.c/h) end to end: a REAL `.3bs` script, run through
// script_run_file with `(import config)`, ends up building a real
// translate.h Config struct that's byte-for-byte correct across every
// one of its own fields. Same rig as the other bcgen_*_test.c/
// bcosprims_test.c files.
//
// Exercises every one of the 19 registered primitives at least once,
// including the two "find or create by name" cases (config-add-define
// across TWO different platforms, config-add-enum-group-member across
// TWO different groups) -- proving bc_config_find_or_create_define_group/
// bc_config_find_or_create_const_group both correctly APPEND to an
// existing group on a repeat name, not just create-once. Also exercises
// config.3bs's batch wrappers (add-headers, add-exclude-funcs, ...,
// added once `.3bs` grew real collection `for`) via a trailing `scratch`
// block that builds `(Vector string)`s and calls each batch form once --
// proving both that the internal `for` loop over a Vector-typed host-
// import argument actually works, and that it appends to an
// ALREADY-EXISTING group (TextureTarget/BufferMask) rather than only a
// freshly-created one. Verified by reading `cfg`'s own fields directly
// in C afterward, never through any `.3bs`-level equality (avoids
// relying on the string comparison this same design thread only just
// fixed -- not because it's untrusted, but because the REAL, more direct
// check here is "did the native Config struct end up byte-correct", which
// reading it in C answers most
// directly).
#include "3b.h"
#include "script.h"
#include "bcconfigprims.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;

static void
expect_true(const char* what, b32 got) {
  if (!got) {
    fprintf(stderr, "FAIL %s: got false, want true\n", what);
    g_failures += 1;
  }
}

static void
expect_eq_i64(const char* what, i64 got, i64 want) {
  if (got != want) {
    fprintf(stderr, "FAIL %s: got %lld, want %lld\n", what, (long long)got, (long long)want);
    g_failures += 1;
  }
}

static void
expect_str8_eq(const char* what, String8 got, const char* want) {
  u64 want_len = strlen(want);
  if (got.size != want_len || (want_len > 0 && memcmp(got.str, want, want_len) != 0)) {
    fprintf(stderr, "FAIL %s: got \"%.*s\" (size %llu), want \"%s\"\n",
            what, (int)got.size, (char*)got.str, (unsigned long long)got.size, want);
    g_failures += 1;
  }
}

static void
write_script(const char* path, const char* content) {
  FILE* f = fopen(path, "w");
  xassert(f);
  fputs(content, f);
  fclose(f);
}

static const char* g_script_source =
  "(package configtest)\n"
  "(import config)\n"
  "\n"
  "(fn main [] i32\n"
  "  (config/set-package \"gl\")\n"
  "  (config/add-header \"GL/gl.h\")\n"
  "  (config/add-header \"GL/glext.h\")\n"
  "  (config/add-define \"windows\" \"WIN32_LEAN_AND_MEAN\" \"1\")\n"
  "  (config/add-define \"windows\" \"NOMINMAX\" \"1\")\n"
  "  (config/add-define \"linux\" \"_GNU_SOURCE\" \"1\")\n"
  "  (config/set-strip-const-prefix \"GL_\")\n"
  "  (config/set-strip-func-prefix \"gl\")\n"
  "  (config/set-strip-struct-prefix \"GL_\")\n"
  "  (config/add-type-map \"GLenum\" \"u32\")\n"
  "  (config/add-type-map \"GLint\" \"i32\")\n"
  "  (config/add-enum-group-member \"TextureTarget\" \"TEXTURE_2D\")\n"
  "  (config/add-enum-group-member \"TextureTarget\" \"TEXTURE_3D\")\n"
  "  (config/add-flags-group-member \"BufferMask\" \"COLOR_BUFFER_BIT\")\n"
  "  (config/set-enum-group-pattern \"BlendFactor\" \"^(SRC|DST)_.*\")\n"
  "  (config/set-flags-group-pattern \"ClearMask\" \"^.*_BUFFER_BIT$\")\n"
  "  (config/add-force-opaque \"GLsync\")\n"
  "  (config/add-construct-outparam \"glGenTextures\" 1)\n"
  "  (config/add-mutate-outparam \"glGetIntegerv\" 1)\n"
  "  (config/add-arena-outparam \"glGetString\" 0 1)\n"
  "  (config/add-rename-func \"glClear\" \"clear\")\n"
  "  (config/add-rename-const \"GL_TRUE\" \"True\")\n"
  "  (config/add-exclude-func \"glDebugMessageCallback\")\n"
  "  (config/add-exclude-const \"GL_DEBUG_SEVERITY_HIGH\")\n"
  "  (config/set-skip-deprecated)\n"
  "  (scratch [t]\n"
  "    (var extra-headers (Vector string))\n"
  "    (dyn-push t extra-headers \"GL/gl3.h\")\n"
  "    (dyn-push t extra-headers \"GL/gl4.h\")\n"
  "    (config/add-headers extra-headers)\n"
  "    (var extra-exclude-funcs (Vector string))\n"
  "    (dyn-push t extra-exclude-funcs \"glFoo\")\n"
  "    (dyn-push t extra-exclude-funcs \"glBar\")\n"
  "    (config/add-exclude-funcs extra-exclude-funcs)\n"
  "    (var extra-exclude-consts (Vector string))\n"
  "    (dyn-push t extra-exclude-consts \"GL_FOO\")\n"
  "    (dyn-push t extra-exclude-consts \"GL_BAR\")\n"
  "    (config/add-exclude-consts extra-exclude-consts)\n"
  "    (var extra-opaque (Vector string))\n"
  "    (dyn-push t extra-opaque \"GLsync2\")\n"
  "    (config/add-force-opaques extra-opaque)\n"
  "    (var extra-texture-members (Vector string))\n"
  "    (dyn-push t extra-texture-members \"TEXTURE_1D\")\n"
  "    (dyn-push t extra-texture-members \"TEXTURE_CUBE_MAP\")\n"
  "    (config/add-enum-group-members \"TextureTarget\" extra-texture-members)\n"
  "    (var extra-buffer-members (Vector string))\n"
  "    (dyn-push t extra-buffer-members \"DEPTH_BUFFER_BIT\")\n"
  "    (config/add-flags-group-members \"BufferMask\" extra-buffer-members))\n"
  "  0)\n";

int
main(void) {
  Context ctx;
  ctx_init(&ctx, MB(16));

  const char* path = "/tmp/3b_bcconfigprims_test_script.3bs";
  write_script(path, g_script_source);

  Config             cfg          = {0};
  BcHostImportTable  host_imports = {0};
  bc_register_config_primitives(&host_imports, ctx_perm(), &cfg);

  BcResult result;
  b32 ok = script_run_file(path, &host_imports, &result);
  expect_true("config script runs", ok);
  remove(path);
  if (!ok) {
    printf("bcconfigprims_test: %d check(s) FAILED (script didn't run)\n", g_failures + 1);
    ctx_free();
    return 1;
  }

  expect_str8_eq("package_name", cfg.package_name, "gl");

  // ~~ 2 singular add-header calls + a batch add-headers([...]) of 2 more
  // -- proves the new `for`-loop-driven batch wrapper appends correctly
  // alongside the singular form, in call order.
  expect_eq_i64("headers count", (i64)dyn_count(cfg.headers), 4);
  if (dyn_count(cfg.headers) == 4) {
    expect_str8_eq("headers[0]", cfg.headers[0], "GL/gl.h");
    expect_str8_eq("headers[1]", cfg.headers[1], "GL/glext.h");
    expect_str8_eq("headers[2]", cfg.headers[2], "GL/gl3.h");
    expect_str8_eq("headers[3]", cfg.headers[3], "GL/gl4.h");
  }

  // ~~ Two defines under "windows", one under "linux" -- proves
  // find-or-create APPENDS to an existing group on a repeat platform name.
  expect_eq_i64("define_groups count (2 distinct platforms)", (i64)dyn_count(cfg.define_groups), 2);
  {
    DefineGroup* windows = NULL;
    DefineGroup* linux_  = NULL;
    foreach_index(i, dyn_count(cfg.define_groups)) {
      if (str8_match(cfg.define_groups[i].platform, str8_lit("windows"), 0)) windows = &cfg.define_groups[i];
      if (str8_match(cfg.define_groups[i].platform, str8_lit("linux"), 0))   linux_  = &cfg.define_groups[i];
    }
    expect_true("windows group found", windows != NULL);
    expect_true("linux group found", linux_ != NULL);
    if (windows) {
      expect_eq_i64("windows group has 2 defines", (i64)dyn_count(windows->defines), 2);
      if (dyn_count(windows->defines) == 2) {
        expect_str8_eq("windows define[0].name", windows->defines[0].name, "WIN32_LEAN_AND_MEAN");
        expect_str8_eq("windows define[0].value", windows->defines[0].value, "1");
        expect_str8_eq("windows define[1].name", windows->defines[1].name, "NOMINMAX");
      }
    }
    if (linux_) {
      expect_eq_i64("linux group has 1 define", (i64)dyn_count(linux_->defines), 1);
      if (dyn_count(linux_->defines) == 1) expect_str8_eq("linux define[0].name", linux_->defines[0].name, "_GNU_SOURCE");
    }
  }

  expect_str8_eq("strip_const_prefix", cfg.strip_const_prefix, "GL_");
  expect_str8_eq("strip_func_prefix", cfg.strip_func_prefix, "gl");
  expect_str8_eq("strip_struct_prefix", cfg.strip_struct_prefix, "GL_");

  expect_eq_i64("type_map count", (i64)dyn_count(cfg.type_map), 2);
  if (dyn_count(cfg.type_map) == 2) {
    expect_str8_eq("type_map[0].c_name", cfg.type_map[0].c_name, "GLenum");
    expect_str8_eq("type_map[0].b3_name", cfg.type_map[0].b3_name, "u32");
    expect_str8_eq("type_map[1].c_name", cfg.type_map[1].c_name, "GLint");
    expect_str8_eq("type_map[1].b3_name", cfg.type_map[1].b3_name, "i32");
  }

  // ~~ Four DISTINCT const groups: TextureTarget (enum, 2 members),
  // BufferMask (flags, 1 member), BlendFactor (enum, pattern),
  // ClearMask (flags, pattern) -- proves find-or-create keys on
  // (kind, name) together, not name alone.
  expect_eq_i64("const_groups count", (i64)dyn_count(cfg.const_groups), 4);
  {
    ConstGroup* texture_target = NULL;
    ConstGroup* buffer_mask    = NULL;
    ConstGroup* blend_factor   = NULL;
    ConstGroup* clear_mask     = NULL;
    foreach_index(i, dyn_count(cfg.const_groups)) {
      ConstGroup* g = &cfg.const_groups[i];
      if (g->kind == ConstGroupKind_Enum  && str8_match(g->name, str8_lit("TextureTarget"), 0)) texture_target = g;
      if (g->kind == ConstGroupKind_Flags && str8_match(g->name, str8_lit("BufferMask"), 0))    buffer_mask    = g;
      if (g->kind == ConstGroupKind_Enum  && str8_match(g->name, str8_lit("BlendFactor"), 0))   blend_factor   = g;
      if (g->kind == ConstGroupKind_Flags && str8_match(g->name, str8_lit("ClearMask"), 0))     clear_mask     = g;
    }
    expect_true("TextureTarget (enum) group found", texture_target != NULL);
    expect_true("BufferMask (flags) group found", buffer_mask != NULL);
    expect_true("BlendFactor (enum, pattern) group found", blend_factor != NULL);
    expect_true("ClearMask (flags, pattern) group found", clear_mask != NULL);
    if (texture_target) {
      expect_true("TextureTarget has no pattern", !texture_target->has_pattern);
      // ~~ 2 singular add-enum-group-member calls + a batch
      // add-enum-group-members(...) of 2 more into the SAME existing
      // group -- proves the batch form's internal `for` loop correctly
      // finds-and-appends-to an already-created group, not just a
      // freshly-created one.
      expect_eq_i64("TextureTarget has 4 members", (i64)dyn_count(texture_target->members), 4);
      if (dyn_count(texture_target->members) == 4) {
        expect_str8_eq("TextureTarget member[0]", texture_target->members[0], "TEXTURE_2D");
        expect_str8_eq("TextureTarget member[1]", texture_target->members[1], "TEXTURE_3D");
        expect_str8_eq("TextureTarget member[2]", texture_target->members[2], "TEXTURE_1D");
        expect_str8_eq("TextureTarget member[3]", texture_target->members[3], "TEXTURE_CUBE_MAP");
      }
    }
    if (buffer_mask) {
      expect_eq_i64("BufferMask has 2 members", (i64)dyn_count(buffer_mask->members), 2);
      if (dyn_count(buffer_mask->members) == 2) {
        expect_str8_eq("BufferMask member[0]", buffer_mask->members[0], "COLOR_BUFFER_BIT");
        expect_str8_eq("BufferMask member[1]", buffer_mask->members[1], "DEPTH_BUFFER_BIT");
      }
    }
    if (blend_factor) {
      expect_true("BlendFactor has_pattern", blend_factor->has_pattern);
      expect_str8_eq("BlendFactor pattern", blend_factor->pattern, "^(SRC|DST)_.*");
    }
    if (clear_mask) {
      expect_true("ClearMask has_pattern", clear_mask->has_pattern);
      expect_str8_eq("ClearMask pattern", clear_mask->pattern, "^.*_BUFFER_BIT$");
    }
  }

  expect_eq_i64("force_opaque count", (i64)dyn_count(cfg.force_opaque), 2);
  if (dyn_count(cfg.force_opaque) == 2) {
    expect_str8_eq("force_opaque[0]", cfg.force_opaque[0], "GLsync");
    expect_str8_eq("force_opaque[1]", cfg.force_opaque[1], "GLsync2");
  }

  expect_eq_i64("outparam_rules count", (i64)dyn_count(cfg.outparam_rules), 3);
  if (dyn_count(cfg.outparam_rules) == 3) {
    OutparamRule* construct = &cfg.outparam_rules[0];
    OutparamRule* mutate    = &cfg.outparam_rules[1];
    OutparamRule* arena     = &cfg.outparam_rules[2];
    expect_true("outparam[0] is Construct", construct->kind == OutparamKind_Construct);
    expect_str8_eq("outparam[0].func_name", construct->func_name, "glGenTextures");
    expect_eq_i64("outparam[0].param_index", construct->param_index, 1);
    expect_true("outparam[1] is Mutate", mutate->kind == OutparamKind_Mutate);
    expect_str8_eq("outparam[1].func_name", mutate->func_name, "glGetIntegerv");
    expect_eq_i64("outparam[1].param_index", mutate->param_index, 1);
    expect_true("outparam[2] is Arena", arena->kind == OutparamKind_Arena);
    expect_str8_eq("outparam[2].func_name", arena->func_name, "glGetString");
    expect_eq_i64("outparam[2].count_param_index", arena->count_param_index, 0);
    expect_eq_i64("outparam[2].out_param_index", arena->out_param_index, 1);
  }

  expect_eq_i64("func_renames count", (i64)dyn_count(cfg.func_renames), 1);
  if (dyn_count(cfg.func_renames) == 1) {
    expect_str8_eq("func_renames[0].from", cfg.func_renames[0].from, "glClear");
    expect_str8_eq("func_renames[0].to", cfg.func_renames[0].to, "clear");
  }
  expect_eq_i64("const_renames count", (i64)dyn_count(cfg.const_renames), 1);
  if (dyn_count(cfg.const_renames) == 1) {
    expect_str8_eq("const_renames[0].from", cfg.const_renames[0].from, "GL_TRUE");
    expect_str8_eq("const_renames[0].to", cfg.const_renames[0].to, "True");
  }

  expect_eq_i64("excluded_funcs count", (i64)dyn_count(cfg.excluded_funcs), 3);
  if (dyn_count(cfg.excluded_funcs) == 3) {
    expect_str8_eq("excluded_funcs[0]", cfg.excluded_funcs[0], "glDebugMessageCallback");
    expect_str8_eq("excluded_funcs[1]", cfg.excluded_funcs[1], "glFoo");
    expect_str8_eq("excluded_funcs[2]", cfg.excluded_funcs[2], "glBar");
  }
  expect_eq_i64("excluded_consts count", (i64)dyn_count(cfg.excluded_consts), 3);
  if (dyn_count(cfg.excluded_consts) == 3) {
    expect_str8_eq("excluded_consts[0]", cfg.excluded_consts[0], "GL_DEBUG_SEVERITY_HIGH");
    expect_str8_eq("excluded_consts[1]", cfg.excluded_consts[1], "GL_FOO");
    expect_str8_eq("excluded_consts[2]", cfg.excluded_consts[2], "GL_BAR");
  }

  // The one flag with no value to carry -- calling the wrapper IS the setting.
  expect_true("skip_deprecated", cfg.skip_deprecated != 0);

  if (g_failures == 0) printf("bcconfigprims_test: all checks passed\n");
  else                 printf("bcconfigprims_test: %d check(s) FAILED\n", g_failures);

  ctx_free();
  return g_failures == 0 ? 0 : 1;
}
