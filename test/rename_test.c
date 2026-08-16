// rename_test.c -- the pattern-rename engine (translate/rename.c) behind
// the `(rename-const-pattern "regex" "template")` config forms.
//
// Tested at the rename_patterns_apply level rather than through a real
// `3b translate` run: what actually needs pinning down is the SPELLING
// each style produces for the awkward inputs real C headers are full of
// (`PCM_S16LE`, `8SVX_EXP`, `XMLParser`), and a header-driven test would
// bury those hundred cases behind libclang setup for no extra coverage.
// The one thing it can't check -- that emit.c really consults this for
// constants/functions/types -- is covered by the `--names` mode, which
// prints exactly what the emitter computed.
//
// Same rig as the other test/*_test.c files.
#include "3b.h"
#include "translate/translate.h"
#include <stdio.h>
#include <string.h>

static int g_failures = 0;

// `pattern`/`replacement` as one single-rule list, applied to `input`.
// `want` of NULL means "no rule should match" (an empty result).
static void
expect_rename(const char* pattern, const char* replacement, const char* input, const char* want) {
  RenamePattern* rules = NULL;
  RenamePattern  rule  = { str8_cstring((char*)pattern), str8_cstring((char*)replacement) };
  dyn_push(ctx_perm(), rules, rule);

  String8     got      = rename_patterns_apply(ctx_perm(), rules, str8_cstring((char*)input));
  const char* want_str = want ? want : "";
  u64         want_len = strlen(want_str);
  if (got.size != want_len || (want_len > 0 && memcmp(got.str, want_str, want_len) != 0)) {
    fprintf(stderr, "FAIL /%s/ \"%s\" on \"%s\": got \"%.*s\", want \"%s\"\n",
            pattern, replacement, input, (int)got.size, (char*)got.str, want_str);
    g_failures += 1;
  }
}

// Every style applied to the same capture, so a change to word splitting
// shows up as one readable block of failures rather than scattered ones.
static void
expect_all_styles(const char* input, const char* pascal, const char* camel,
                  const char* snake, const char* kebab) {
  expect_rename("^(.*)$", "{1:pascal}", input, pascal);
  expect_rename("^(.*)$", "{1:camel}", input, camel);
  expect_rename("^(.*)$", "{1:snake}", input, snake);
  expect_rename("^(.*)$", "{1:kebab}", input, kebab);
}

int
main(void) {
  Context ctx;
  ctx_init(&ctx, MB(16));

  ////////////////////////////////
  //~ Word splitting, via the four word-based styles

  expect_all_styles("MPEG_TS", "MpegTs", "mpegTs", "mpeg_ts", "mpeg-ts");
  expect_all_styles("open_input", "OpenInput", "openInput", "open_input", "open-input");
  expect_all_styles("CreateWindow", "CreateWindow", "createWindow", "create_window", "create-window");

  // An uppercase run stays one word until the last letter before a
  // lowercase one -- the acronym rule emit.c's camel_to_kebab uses.
  expect_all_styles("XMLParser", "XmlParser", "xmlParser", "xml_parser", "xml-parser");

  // Digits attach to the word before them: real codec/format names
  // (`PCM_S16LE`, `A64_MULTI5`) read as one token per underscore-separated
  // piece, never split at the alpha/digit seam.
  expect_all_styles("PCM_S16LE", "PcmS16le", "pcmS16le", "pcm_s16le", "pcm-s16le");
  expect_all_styles("TexImage3D", "TexImage3d", "texImage3d", "tex_image3d", "tex-image3d");

  // A word STARTING with a digit keeps the digit and lowercases the rest
  // rather than growing a bogus capital -- `8SVX_EXP` -> `8svxExp`.
  expect_rename("^(.*)$", "{1:pascal}", "8SVX_EXP", "8svxExp");
  expect_rename("^(.*)$", "{1:pascal}", "4XM", "4xm");
  expect_rename("^(.*)$", "{1:pascal}", "012V", "012v");

  // Runs of separators collapse; leading/trailing ones vanish entirely.
  expect_all_styles("__A__B__", "AB", "aB", "a_b", "a-b");

  ////////////////////////////////
  //~ upper/lower are plain case mapping, NOT word-based -- separators
  // survive, which is the whole reason they're separate from snake/kebab.

  expect_rename("^(.*)$", "{1:upper}", "Mpeg_Ts", "MPEG_TS");
  expect_rename("^(.*)$", "{1:lower}", "Mpeg_Ts", "mpeg_ts");
  expect_rename("^(.*)$", "{1:lower}", "AV-Codec.ID", "av-codec.id");

  ////////////////////////////////
  //~ Template shape

  expect_rename("^CODEC_ID_(.*)$", "{1:pascal}", "CODEC_ID_INTERPLAY_VIDEO", "InterplayVideo");
  expect_rename("^CODEC_ID_(.*)$", "{1}", "CODEC_ID_H264", "H264");           // no style = verbatim
  expect_rename("^CODEC_ID_(.*)$", "{1:verbatim}", "CODEC_ID_H264", "H264");  // ...spelled out
  expect_rename("^OPT_FLAG_(.*)$", "Opt{1:pascal}", "OPT_FLAG_ENCODING", "OptEncoding"); // literal prefix
  expect_rename("^(.*)_T$", "{1:pascal}Type", "SAMPLE_FMT_T", "SampleFmtType");           // literal suffix
  expect_rename("^A_(.*)_B$", "{1:kebab}", "A_ONE_TWO_B", "one-two");                     // literal both sides

  // `{0}` is the whole match, same as POSIX's own group 0.
  expect_rename("PIX_FMT_.*", "{0:pascal}", "AV_PIX_FMT_RGB24", "PixFmtRgb24");

  // Multiple groups, in any order, reused freely.
  expect_rename("^([a-z]+)_([a-z]+)$", "{2:pascal}{1:pascal}", "open_input", "InputOpen");
  expect_rename("^([a-z]+)_([a-z]+)$", "{1}-{1}-{2}", "open_input", "open-open-input");

  // Braces are escapable, in both directions.
  expect_rename("^(.*)$", "{{{1:lower}}}", "X", "{x}");

  ////////////////////////////////
  //~ Non-matches and malformed rules both mean "this rule didn't apply",
  // never a half-applied or empty name. (The malformed ones each print
  // one diagnostic to stderr -- expected output, not a failure.)

  expect_rename("^CODEC_ID_(.*)$", "{1:pascal}", "PIX_FMT_RGB24", NULL); // regex doesn't match
  expect_rename("^(.*)$", "{2}", "ANY", NULL);                            // no such capture group
  expect_rename("^(.*)$", "{1:bogus}", "ANY", NULL);                      // unknown style
  expect_rename("^(.*)$", "{1", "ANY", NULL);                             // unterminated brace
  expect_rename("^(.*)$", "{}", "ANY", NULL);                             // no group number
  expect_rename("^(.*)$", "{x}", "ANY", NULL);                            // group number isn't a number
  expect_rename("^(.*)$", "{1:pascal}", "___", NULL);                     // styles to the empty name
  expect_rename("*[bad", "{0}", "ANY", NULL);                             // uncompilable regex

  ////////////////////////////////
  //~ First matching rule wins, so specific rules can be ordered ahead of
  // general ones (translate.h's RenamePattern comment).

  {
    RenamePattern* rules = NULL;
    RenamePattern  specific = { str8_lit("^CODEC_ID_PCM_(.*)$"), str8_lit("Pcm{1:pascal}") };
    RenamePattern  general  = { str8_lit("^CODEC_ID_(.*)$"), str8_lit("{1:pascal}") };
    dyn_push(ctx_perm(), rules, specific);
    dyn_push(ctx_perm(), rules, general);

    String8 got = rename_patterns_apply(ctx_perm(), rules, str8_lit("CODEC_ID_PCM_S16LE"));
    if (!str8_match(got, str8_lit("PcmS16le"), 0)) {
      fprintf(stderr, "FAIL first-rule-wins: got \"%.*s\", want \"PcmS16le\"\n", (int)got.size, (char*)got.str);
      g_failures += 1;
    }
    // ...and a name only the general rule matches still falls through to it.
    got = rename_patterns_apply(ctx_perm(), rules, str8_lit("CODEC_ID_H264"));
    if (!str8_match(got, str8_lit("H264"), 0)) {
      fprintf(stderr, "FAIL fallthrough-to-general: got \"%.*s\", want \"H264\"\n", (int)got.size, (char*)got.str);
      g_failures += 1;
    }
  }

  // An empty rule list is the overwhelmingly common case (no patterns
  // configured at all) and must be a cheap no-match, not a crash.
  {
    String8 got = rename_patterns_apply(ctx_perm(), NULL, str8_lit("ANYTHING"));
    if (got.size != 0) {
      fprintf(stderr, "FAIL empty-rule-list: got \"%.*s\", want no match\n", (int)got.size, (char*)got.str);
      g_failures += 1;
    }
  }

  ////////////////////////////////
  //~ rename_regex_matches -- the same compiled-regex cache, used by
  // emit.c's `(enum-group Name (match "..."))` grouping predicate.

  if (!rename_regex_matches(str8_lit("^PIX_FMT_"), str8_lit("PIX_FMT_RGB24"))) {
    fprintf(stderr, "FAIL rename_regex_matches: expected a match\n");
    g_failures += 1;
  }
  if (rename_regex_matches(str8_lit("^PIX_FMT_"), str8_lit("CODEC_ID_H264"))) {
    fprintf(stderr, "FAIL rename_regex_matches: expected no match\n");
    g_failures += 1;
  }
  if (rename_regex_matches(str8_lit("*[bad"), str8_lit("anything"))) {
    fprintf(stderr, "FAIL rename_regex_matches: an uncompilable regex must never match\n");
    g_failures += 1;
  }

  if (g_failures == 0) printf("rename_test: all checks passed\n");
  else                 printf("rename_test: %d check(s) FAILED\n", g_failures);

  ctx_free();
  return g_failures == 0 ? 0 : 1;
}
