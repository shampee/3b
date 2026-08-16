#ifndef BCCONFIGPRIMS_H
#define BCCONFIGPRIMS_H
#include "3b.h"
#include "bytecode.h"
#include "translate/translate.h" // Config

// Registers the translator's Config-mutation host imports (config-add-header,
// config-add-type-map, ...) into `table`, and points `table->userdata` at
// `cfg` -- every registered import reaches `cfg` through the `userdata`
// parameter every Trampoline BcHostFn call already receives (see
// BcHostFn's own comment in bytecode.h), not through any hidden state, so
// setting BOTH in one call here removes a real footgun (forgetting to
// wire `userdata` up separately would mean every registered import
// silently mutates through a NULL pointer the first time a script
// actually calls one).
//
// The "config" embedded module (translate/config.3bs) declares matching
// `extern`s and wraps each in a nicer name (add-header, add-type-map,
// ...) -- same layering bcosprims.c/build.3bs already established for the
// generic OS primitives. Deliberately a SEPARATE module from `build`:
// these primitives are meaningless without a real Config* to mutate, so
// they're registered ONLY by whatever driver is specifically reading a
// `.3bs` config file -- NOT by run_script_cmd's general
// `3b run <script.3bs>` path, which has no Config to point at.
// TRANSLATE-tool-specific (this file is only built as part of
// TRANSLATE_SOURCES, gated behind WITHOUT_TRANSLATE the same way
// translate/config.c itself already is) unlike bcosprims.c, which is
// genuinely general-purpose.
void bc_register_config_primitives(BcHostImportTable* table, Arena* arena, Config* cfg);

#endif
