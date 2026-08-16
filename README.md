# 3b

A small Lisp-flavored systems language that compiles straight to
readable C.

```lisp
(package structs)

(struct Creature
  [name   string
   health i32])

(fn make-orc [] Creature
  (Creature {:name "Orc" :health 50}))

(fn get-orc-health [c Creature] i32
  (get c health))
```

No GC. No hidden control flow. No generics, ever (see
[below](#why-its-shaped-this-way)). Structs, enums, flags, arrays, and
pointers work the way they do in C, just with s-expression syntax and a
real type checker in front of them. `Vector<T>`/
`Map<K,V>`/`Set<T>` (`[T]`/`{K V}`/`{T}` shorthand), multiple return
values, checked/domain-safe math, string comparisons/parsing and a
`stream` primitive for I/O are all built in -- see `examples/`.

The same language also runs a second way: `3b run file.3bs` interprets a
script through an in-process bytecode VM instead of compiling to C,
letting a natively-compiled 3b program embed 3b itself as a
hot-reloadable scripting layer (`native_pkgs/vm`). See [Two backends,
one language](#two-backends-one-language) below.

## Status

Personal project, actively changing. The language, the compiler, and
this file can all be wrong about each other at any given moment. The
closest thing to a spec is [learn-3b-in-y-minutes.org](learn-3b-in-y-minutes.org),
which is kept honest by being checked against real, compiling code
under `examples/`.

Linux is the only platform this runs on day-to-day (CI included). Two
other platforms have real, if incompletely verified, support:

- **macOS**: no known build blockers -- pure POSIX now, with hand-rolled
  workarounds for the two POSIX APIs Apple's pthread has never
  implemented (barriers, `pthread_condattr_setclock`). Unverified on
  real hardware (none available while working on this), not "tested and
  working."
- **Windows**: the compiler cross-compiles clean with
  `x86_64-w64-mingw32-gcc` (`make windows`) and `3b build`/`3b run` have
  been verified end-to-end under Wine, with a real Windows-native
  mingw-w64 GCC on `PATH` inside the Wine prefix: every package under
  `examples/` (except `examples/game`, which needs SDL3/OpenGL) compiles,
  links, and runs correctly through `wine 3b.exe run`, with output matching
  the native Linux run. `3b translate`
  (the C header -> 3b binding generator) isn't included in a Windows
  build -- it needs libclang for its own build, and no Windows libclang
  was available to build/verify against. `build.c`'s `pkg-config`-based
  manifests work there too now -- `3b build` runs `pkg-config` itself and
  splices the flags in, rather than relying on a POSIX shell's `$(...)`
  command substitution, which cmd.exe has no equivalent of -- but Windows
  ships no `pkg-config` of its own, so such a project needs one (pkgconf,
  say) on `PATH`; without it the build stops with that as the reason.
  Verified under Wine against `pkgconf.exe`, not against a real SDL3
  install. Real (non-Wine) Windows hardware is still unverified.

## Building

The compiler itself needs nothing but a C11 compiler and libm/libpthread:

```
make WITHOUT_TRANSLATE=1
```

The one optional dependency is libclang 19, used only by the
header-translation tool `3b translate` (C header -> 3b bindings; see
[below](#why-its-shaped-this-way)). A build without it is complete in
every other respect -- the two backends, the LSP, the whole test suite
-- and `3b translate` reports that it isn't available rather than
misbehaving. To build *with* it:

```
wget https://apt.llvm.org/llvm.sh && chmod +x llvm.sh && sudo ./llvm.sh 19
sudo apt-get install -y libclang-19-dev
make
```

`LLVM_PREFIX` defaults to Debian/Ubuntu's `/usr/lib/llvm-19` and is a
plain make variable -- point it wherever your distribution puts LLVM 19,
e.g. `make LLVM_PREFIX=/usr/lib64/llvm19` or, on macOS with Homebrew,
`make LLVM_PREFIX=$(brew --prefix llvm@19)`.

Either way this builds `./3b`. It's a single binary -- once built, it
doesn't need anything else on disk (the runtime code every compiled 3b
program links against is baked into the compiler binary itself at build
time; see `runtime/` and `tools/embed_runtime.c`). `3b --version` names
the build, including whether it has `3b translate`.

A plain `make` builds a debug toolchain -- unoptimized, with `xassert` and
the arena poisoning compiled in. For a binary to ship:

```
make release        # ./3b, liblib3b.a and ./3b-lsp, all -O2 with the debug checks off
```

That is the whole shipped set in one target, which is what CI builds a
release from. It cleans first, since object files aren't tagged by which
flags built them -- so a plain `make` afterwards needs its own `make
clean`. The knob underneath is `RELEASE=1`, and it propagates to any
sub-make: `make RELEASE=1 windows` cross-compiles a release `3b.exe`.

This is the *toolchain's* own build. Whether a 3b program you compile is a
release build is a separate, per-build choice -- `3b build --release` (see
[Trying it](#trying-it), below) -- exactly as an optimized gcc still
defaults to `-O0`.

## Trying it

```
./3b run examples/structs          # compile to C, link, run
./3b run examples/structs/main.3b  # the same source on the bytecode VM (see below)
```

Any path works from anywhere -- a package directory, or one `.3b` file in
it. Given no path at all, `3b build`/`3b run`/`3b clean` walk up from the
current directory to the nearest one holding `.3b` source, so `cd
examples/structs && ../../3b run` does the same thing.

`examples/` has one small, focused, self-contained package per language
feature area, each with a `main` that prints its own results and fails
loudly if anything's wrong: `structs`, `enums-and-flags`, `control-flow`,
`arenas`, `arrays`, `lanes`, `creatures`, `vector`, `map`, `set`,
`multi-return`, `anon-struct-params`, `type-shorthand`, `comparisons`,
`checked-math`, `string-parsing`, `nested-scratch`, `streams`, `integers`
(integer widths and wrapping, on both backends), `os-portable` (the
embedded `os` package), `dice` (the embedded `rng` package).
`examples/hotreload-demo/` shows the bytecode-VM scripting layer
([below](#two-backends-one-language)) end-to-end. `examples/game/` is
the real one: an SDL3 + OpenGL demo, with its C bindings generated by
`3b translate` rather than hand-written.

`examples/game3d/` is its 3D counterpart, and the largest program in the
tree. Skinned glTF 2.0 models (parsed with
[cgltf](https://github.com/jkuhlmann/cgltf) through a third generated
binding package) walk a lit, textured, shadowed scene under a directional
sun, with point/spot lights casting real shadows of their own, a
procedural or cubemap sky, and text rendered through stb_truetype. `F1`
switches from playing it to editing it: a 4-pane Source-style brush editor
with click-to-pick selection, a translate gizmo, face dragging, grid
snapping, a model browser, a colour picker, and level save/load. Gameplay
objects can be driven by hot-reloadable `.3bs` scripts running on the
bytecode VM ([below](#two-backends-one-language)) inside the
natively-compiled binary. It reuses `examples/game/`'s generated
`gl`/`sdl` binding packages, GLAD shim,
stb_image and stb_truetype by symlink rather than carrying a second copy.

## Two backends, one language

The same `.3b` source can be compiled two different ways:

- **Native (`3b <dir>` / `3b build` / `3b run <dir>`)**: the default,
  described above -- typed AST straight to readable C, then a real C
  toolchain. This is what every `examples/` package (other than the
  scripting demo) uses.
- **Bytecode VM (`3b run file.3bs`, or a bare single-file `.3b` given
  directly to `3b run`/`3b <file>`)**: `bcgen.c` compiles the same typed
  AST to a custom bytecode instead, interpreted in-process by `bcvm.c` --
  no C toolchain step, so it starts instantly and can be reloaded on the
  fly. `.3bs` scripts can `(import build)`/`(import os)` to shell out,
  touch the filesystem, or read the environment (`bcosprims.c`,
  `bcconfigprims.c`); `3b translate`'s own config files are `.3bs`
  scripts for exactly this reason. This is also how `3b run
  examples/arrays/main.3b` can smoke-test a single-file package's bcgen.c
  coverage without a separate `.3bs` copy of anything -- multi-file
  packages still need the native pipeline, since the bytecode driver's
  import mechanism only knows `build`/`os`/the embedded native packages.

The two are meant to produce the same answers from the same source, and
`make backend-diff` (see [Testing](#testing), below) is what holds them
to it. `examples/integers/` is the example written specifically for
that: the VM keeps every integer in a 64-bit register whatever its
declared width, so a `u8` wrapping at 256 or a `u64` dividing as an
unsigned number is free in generated C and deliberate work on the VM.

There is one named exception, and it is about speed rather than answers:
the lane system (`parallel`/`parallel-for`/`lane-*`) runs on the VM as a
**single lane, serially**. `lane-count` is 1 there, so lane-using source
compiles and produces the same results on either backend -- it just
doesn't get faster on the VM. That is a configuration the native backend
also produces, on a single-core machine, rather than a stub; see the
LANES note at the top of `bcgen.c` for why real threads in the
interpreter would be a redesign.

A natively-compiled 3b program can go one step further and embed this
VM itself: `native_pkgs/vm` (`(import vm)`, baked into the compiler, no
project-folder boilerplate needed) exposes `script_load`/`script_call`/
`script_poll_reload`/`script_unload` so a shipped, natively-compiled
binary can load a `.3bs` gameplay script, call into it, and hot-reload it
after an on-disk edit -- the host owns its own state and mutates it
through handles it hands the script, so a reload can swap the script's
code without losing the state it operates on. See
`examples/hotreload-demo/` and `native_pkgs/vm/vm.3b`'s own notes.
`native_pkgs/os` and `native_pkgs/rng` (a PCG32 PRNG, `examples/dice/`)
are embedded the same way -- `(import os)`/`(import rng)` need no
on-disk package directory, on either backend.

`os` in particular is two independent implementations behind one name
(the embedded C runtime for compiled code, `bcosprims.c`'s host imports
for the VM), which is the shape that drifts silently -- and did, until
the two were reconciled into a full mirror of each other. Every verb and
constant now exists on both sides with the same signature and the same
semantics; `examples/os-portable/` exercises the module end to end and
`make os-parity` (part of `make check`) requires byte-identical output
from both backends.

## Why it's shaped this way

- **No generics, ever.** Not "not yet" -- a deliberate, permanent
  decision. Anything generic-shaped (a typed container, `print`,
  `min`/`max`, a use-after-free-safe object pool) is a compiler builtin,
  special-cased per call site, instead of a monomorphized generic
  type. See `runtime/bbb_handle.h`'s `bbb_DEFINE_HANDLE_POOL` for what
  that looks like for object pools specifically.

- **No GC.** Allocation goes through arenas (bump allocators) --
  `create`/`destroy`/`reset`/`push`/`push0`/`scratch`. See
  `examples/arenas/`.

- **Compiles to C by default.** The generated `.c` is meant to be
  readable, and `extern` FFI into existing C is direct: no wrapper
  layer, no marshaling. The bytecode VM
  ([above](#two-backends-one-language)) exists specifically for the
  cases C-toolchain-per-build is wrong for -- fast startup, on-the-fly
  reload -- not as a replacement for it.

- **C interop is generated, not hand-written.** `3b translate` reads a
  real C header via libclang and emits a 3b package exposing it --
  that's how the SDL3, OpenGL, and stb_image bindings under
  `examples/game/` exist. A library that passes structs BY VALUE gets a
  generated `<pkg>_byval.h` alongside, holding the C shims those calls
  are routed through; `3b build` finds and `-include`s it on its own.

- **Threads, not async.** `parallel`/`parallel-for` fork-join work
  across a fixed, process-wide pool of OS threads; `lane-index`/
  `lane-count`/`lane-sync`/`lane-arena` are the only lane-local
  primitives. See `examples/lanes/`.

## Layout

```
lexer.c parser.c atom.c     -- read .3b source into an untyped s-expr AST
lower.c                     -- untyped AST -> typed AST (surface-syntax desugaring)
checker.c                   -- type checking over the typed AST
codegen.c                   -- typed AST -> C source (native backend)
compiler.c build.c          -- package/import resolution; toolchain invocation
bytecode.h bcgen.c bcvm.c   -- typed AST -> bytecode, and the VM that runs it
bcio.c bcmap.c bcnative.c   -- bytecode (de)serialization/caching, Map/Set VM support,
                               and bridging a bytecode call to a native C signature
bcosprims.c bcconfigprims.c -- host imports scripts get via `(import os)`/`(import build)`
script.c script_native.c    -- `3b run *.3bs` driver; script_load/call/reload API a
                               NATIVELY-compiled 3b program embeds the VM through
format.c                    -- `3b format`, a syntax-level pretty-printer
runtime/                    -- the runtime every COMPILED 3b program links against
                               (arenas, strings, dynamic arrays, handle pools,
                               threads/lanes) -- baked into the 3b binary at
                               build time, not read off disk at runtime
native_pkgs/                -- os/vm/rng: packages baked into the compiler binary
                               itself, importable with no on-disk project folder
translate/                  -- the libclang-based C header -> 3b binding generator
base/                       -- the C utility layer the COMPILER's own
                               implementation is written against -- arenas,
                               String8, OS detection (distinct from runtime/,
                               which is what COMPILED 3b programs link
                               against; see runtime/'s own notes). Derived
                               from Ryan Fleury's base layer -- see
                               base/NOTICE.md
lsp/                        -- `3b-lsp`, a language server (diagnostics, local
                               var/param completion, collection builtins, and
                               clangd-style hover popups: signature, the
                               comment block above the declaration, and where
                               it was declared -- for struct fields named
                               through `.`/`get` too, not just declarations;
                               plus document/workspace symbol outlines and
                               format-on-save, which serves `3b format`)
examples/                   -- real, runnable/buildable 3b programs
```

## Testing

```
make                                           # build the compiler (./3b) and liblib3b.a
make release                                   # the same set optimized, for shipping (see Building)
make check                                     # every suite in the tree, in one command
make lsp                                       # build 3b-lsp, the language server
make backend-diff                              # check the two backends agree (see below)
make sanitize                                  # rebuild the compiler itself with ASAN+UBSAN, as ./3b-sanitize
make sanitize-examples                         # run the GENERATED programs under ASAN+UBSAN (+TSAN for lanes)
```

`make backend-diff` (`test/backend_diff.sh`, also a step inside `make
check`) runs every runnable example through BOTH backends -- the C
toolchain and the bytecode VM -- and diffs their stdout and exit status.
"The same source runs on either backend" is a design goal, and every
other suite here exercises exactly one of them, so this is the only check
that catches the two disagreeing. It carries a short skip list, each entry
naming the specific gap that makes that example incomparable
(`examples/lanes` uses `parallel`, `examples/hotreload-demo` is
interactive) -- when a gap closes, its entry goes away and the example
starts being compared, as `examples/creatures` did once the VM driver
learned to compile a whole package rather than one file.

`make sanitize-examples` (`test/sanitize_examples.sh`) is the counterpart
of `make sanitize` for the other half of the tree. `make sanitize`
instruments the COMPILER, catching bugs in 3b's own execution; it says
nothing about the C that `codegen.c` emits or the runtime prelude shipped
with it (arenas, dynamic arrays, the lane pool). This target rebuilds
every example with the generated program under ASAN+UBSAN, and again
under TSAN wherever it uses `parallel`, which is what points a race
detector at the lane fork-join pool.

Flags reach the generated program's toolchain through `BBB_EXTRA_CFLAGS`,
which `3b build` splices into every compile line and the link line:

```
BBB_EXTRA_CFLAGS="-fsanitize=thread" ./3b build examples/lanes
```

It is spelled `BBB_`, not `3B_` like the compiler's own debug knobs,
because a name starting with a digit is not a valid shell assignment.

CI runs all of the above, plus building and running every example under
`examples/` (except `examples/game/` and `examples/game3d/`, which need
SDL3/OpenGL dev packages CI doesn't have set up yet).

## License

MIT -- see [LICENSE](LICENSE).

Two categories of file are third-party and keep their own terms: `base/`,
the compiler's C utility layer, is derived from Ryan Fleury's base layer
(MIT -- see [base/NOTICE.md](base/NOTICE.md)), and the SDL3/OpenGL demos
under `examples/` vendor GLAD, stb_image, stb_truetype and cgltf, plus a
Khronos sample model and the DejaVu Sans font. None of those are needed
to build `3b` itself. [THIRD-PARTY.md](THIRD-PARTY.md) indexes all of
them with their licenses.
