#!/bin/sh
# format_corpus.sh -- the end-to-end article behind `3b format -w`: for every
# real package in the tree, emit its generated C, format every `.3b` in it,
# emit again, and require the C to be byte-identical.
#
# This is the corpus-capture ritual that until now was performed BY HAND
# (stash, build, capture over a corpus, restore, rebuild, diff) whenever the
# formatter or a printer near it was touched. Doing it by hand meant it was
# done when someone remembered to; as a target it runs in `make check`.
#
# Nothing here touches the working tree. Each package is copied into a
# temporary directory first and formatted THERE, so a failure leaves the repo
# exactly as it found it -- the single most important property for a script
# whose whole subject is a command that rewrites source files in place.
#
# Package granularity is the point of this script, and what test/format_test.c
# structurally cannot do: that one hand-rolls parse -> lower -> check against
# single self-contained fixtures, so it can never cover a multi-file package or
# one that imports another. Everything here is a real directory going through
# the real `3b` binary.
#
# `3b <dir>` (not `3b build <dir>`) is deliberate: it emits output/ and stops
# before invoking the C toolchain. The comparison is over the C that codegen.c
# produced, so gcc has nothing to add but minutes.
#
# usage: tools/format_corpus.sh [path-to-3b-binary] [repo-root]

set -eu

BIN=${1:-./3b}
ROOT=${2:-.}

if [ ! -x "$BIN" ]; then
  echo "format_corpus: no 3b binary at '$BIN' -- run 'make' first" >&2
  exit 1
fi
BIN=$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")
ROOT=$(cd "$ROOT" && pwd)

WORK=$(mktemp -d "${TMPDIR:-/tmp}/3b-format-corpus.XXXXXX")
trap 'rm -rf "$WORK"' EXIT INT TERM

# Packages to sweep: every examples/ directory except the two game demos,
# which need SDL/GL headers present and pull in package directories symlinked
# from outside this repo -- neither is something a formatter test should
# depend on. (test/format_test.c still formats their `.3b` sources; it just
# doesn't need them to compile.) Plus the translator's own small fixture
# package, which is a real package like any other.
PACKAGES=""
for d in "$ROOT"/examples/*/; do
  name=$(basename "$d")
  case $name in
    game|game3d) continue ;;
  esac
  PACKAGES="$PACKAGES $d"
done
PACKAGES="$PACKAGES $ROOT/translate/tests/testsmall/"

failures=0
checked=0

for pkg in $PACKAGES; do
  [ -d "$pkg" ] || continue
  name=$(basename "$pkg")

  src="$WORK/$name"
  rm -rf "$src"
  cp -r "$pkg" "$src"
  rm -rf "$src/output"

  # (1) Emit the C for the package as committed.
  if ! "$BIN" "$src" >"$WORK/$name.before.log" 2>&1; then
    echo "SKIP $name (does not compile as committed)"
    sed 's/^/    /' "$WORK/$name.before.log" | head -5
    continue
  fi
  mkdir -p "$WORK/$name.before"
  cp "$src"/output/*.c "$src"/output/*.h "$WORK/$name.before/" 2>/dev/null || true

  # (2) Format every source file in place -- the operation under test.
  fmt_failed=0
  for f in $(find "$src" -name '*.3b' -not -path '*/output/*'); do
    if ! "$BIN" format -w "$f" >/dev/null 2>&1; then
      echo "FAIL $name: 3b format -w failed on $(basename "$f")"
      fmt_failed=1
    fi
  done
  if [ "$fmt_failed" -ne 0 ]; then
    failures=$((failures + 1))
    continue
  fi

  # (3) Emit again from the formatted sources.
  rm -rf "$src/output"
  if ! "$BIN" "$src" >"$WORK/$name.after.log" 2>&1; then
    echo "FAIL $name: package no longer compiles AFTER formatting"
    sed 's/^/    /' "$WORK/$name.after.log" | head -20
    failures=$((failures + 1))
    continue
  fi
  mkdir -p "$WORK/$name.after"
  cp "$src"/output/*.c "$src"/output/*.h "$WORK/$name.after/" 2>/dev/null || true

  # (4) The property: same C, byte for byte.
  checked=$((checked + 1))
  if diff -r -q "$WORK/$name.before" "$WORK/$name.after" >/dev/null 2>&1; then
    echo "ok   $name"
  else
    echo "FAIL $name: generated C changed after formatting"
    diff -r -u "$WORK/$name.before" "$WORK/$name.after" | head -40 | sed 's/^/    /'
    failures=$((failures + 1))
  fi
done

# A sweep that silently matched no packages would report success having
# compared nothing -- the one way this script can lie.
if [ "$checked" -lt 10 ]; then
  echo "FAIL: only $checked package(s) compared -- the sweep is not finding the tree"
  failures=$((failures + 1))
fi

echo
if [ "$failures" -eq 0 ]; then
  echo "format_corpus: all checks passed ($checked packages)"
  exit 0
fi
echo "format_corpus: $failures failure(s)"
exit 1
