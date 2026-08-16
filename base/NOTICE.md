# base/ provenance

`base.c` / `base.h` are **derived from Ryan Fleury's base layer**, not
written from scratch. The core conventions come from there more or less
intact -- the `Arena` bump allocator and its `ArenaTemp` scratch pairs,
the `String8` length-carrying string type and its `str8_*` /
`String8List` operations, the `MemoryZero`/`MemoryCopy` wrappers, and the
compiler/OS/architecture detection block at the top of `base.h`.

The most widely available published form of that base layer is the
`src/base/` directory of
[raddebugger](https://github.com/EpicGamesExt/raddebugger), which is
distributed under the **MIT License**. This copy is used and
redistributed on those terms; credit for the original design and code
belongs to Ryan Fleury.

## What is local to this repository

The file has diverged substantially in the parts 3b leans on, and those
changes are this repository's own work, under its
[MIT license](../LICENSE):

- `ArenaOps` and the pluggable `ArenaVMBackend` / `ArenaHeapBackend`
  split, so the same arena API works on a reserve-and-commit virtual
  memory backend or a plain `malloc` one.
- `ArenaPoison` and `ArenaMark` -- poison-on-reset/pop debug scrubbing,
  and the mark/restore pairing the compiler's own passes use.
- `String8Vector` / `String8Array` and the string operations built on
  them that the lexer, parser and code generators need.
- `Context` and `ThreadContext`
- `SmallVector`
- `HashTable`
- `HandlePool`
- `Stopwatch`
