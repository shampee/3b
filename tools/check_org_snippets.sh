#!/bin/sh
# check_org_snippets.sh -- keeps learn-3b-in-y-minutes.org honest.
#
# README calls that file "the closest thing to a spec ... kept honest by
# being checked against real, compiling code under examples/". Nothing
# actually enforced that, and it had silently rotted: a `for` section
# claiming collection iteration didn't exist yet (it does), a dead
# examples/game/stb/ link left behind by a rename, and a build.cfg.3b
# snippet naming c-sources that had moved.
#
# Two checks, both cheap enough to run on every `make check`:
#
#   1. PARSE -- every `#+BEGIN_SRC lisp` block goes through `3b format`,
#      which is parse-only and exits non-zero on a syntax error. This
#      does NOT type-check: most blocks are deliberate fragments (a bare
#      `Creature*`, a call against undeclared locals) that could never
#      compile standalone, and the ones that CAN compile live in
#      examples/ and are covered by building those. What it catches is
#      unbalanced parens, bad tokens, and malformed literals -- the ways
#      a hand-edited snippet actually breaks.
#
#      A block that is deliberately not parseable 3b can opt out with
#      `#+BEGIN_SRC lisp :check no`.
#
#   2. LINKS -- every `[[file:...]]` target still exists on disk (this is
#      what caught the stb/ -> stbimg/ rename), and every `[[#slug]]`
#      section link names a heading that declares that `:CUSTOM_ID:`.
#
#      The anchor half is here because the file once used `[[*Heading]]`
#      links, which only Emacs resolves: org-ruby, which is what GitHub
#      renders .org with, emits the target verbatim as href="*Heading", so
#      all 8 were dead on the rendered page while this script happily
#      reported "links resolved". `[[*...]]` is now rejected outright, and
#      each CUSTOM_ID has to equal the slug GitHub derives from its own
#      heading text -- an id that doesn't is a link that resolves here and
#      404s on the page, which is the whole failure being guarded against.
#
# Usage: tools/check_org_snippets.sh [path/to/3b] [path/to/file.org]
set -eu

BB=${1:-./3b}
ORG=${2:-learn-3b-in-y-minutes.org}

[ -x "$BB" ] || { echo "check-doc: no 3b binary at '$BB' (run make first)" >&2; exit 1; }
[ -f "$ORG" ] || { echo "check-doc: no such file: '$ORG'" >&2; exit 1; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

failed=0
checked=0
skipped=0

# Split the org file into one file per lisp block, named NNNN.3b where NNNN
# is the block's opening line number -- so a failure can name the line to
# go look at.
awk '
  /^#\+BEGIN_SRC[ \t]+lisp([ \t]|$)/ {
    if ($0 ~ /:check[ \t]+no/) { print NR > (tmp "/skipped.txt"); next }
    inblock = 1; out = sprintf("%s/%04d.3b", tmp, NR); next
  }
  /^#\+END_SRC/ { if (inblock) close(out); inblock = 0; next }
  inblock { print $0 > out }
' tmp="$TMP" "$ORG"

for f in "$TMP"/*.3b; do
  [ -e "$f" ] || break
  checked=$((checked + 1))
  line=$(basename "$f" .3b | sed 's/^0*//')
  if ! err=$("$BB" format "$f" 2>&1 >/dev/null); then
    echo "$ORG:$line: lisp block does not parse"
    echo "$err" | sed 's/^/    /'
    failed=$((failed + 1))
  fi
done

if [ -f "$TMP/skipped.txt" ]; then skipped=$(wc -l < "$TMP/skipped.txt"); fi

# `[[file:some/path][label]]` (and the bare `[[file:some/path]]` form) --
# check the target, relative to the org file. `[^]]*` deliberately stops at
# the FIRST `]`, which is the end of the path in both forms.
org_dir=$(dirname "$ORG")
links=0
grep -o '\[\[file:[^]]*\]' "$ORG" 2>/dev/null | sed 's/^\[\[file://; s/\]$//' | sort -u > "$TMP/links.txt" || true
while IFS= read -r target; do
  [ -n "$target" ] || continue
  links=$((links + 1))
  if [ ! -e "$org_dir/$target" ]; then
    echo "$ORG: dead link: $target"
    failed=$((failed + 1))
  fi
done < "$TMP/links.txt"

# `[[*Heading][label]]` -- Emacs-only, dead on GitHub. Nothing to look up:
# the syntax itself is the bug.
grep -o '\[\[\*[^]]*\]' "$ORG" 2>/dev/null | sed 's/^\[\[\*//; s/\]$//' | sort -u > "$TMP/heading_links.txt" || true
while IFS= read -r target; do
  [ -n "$target" ] || continue
  echo "$ORG: Emacs-only heading link: [[*$target]]"
  echo "    org-ruby (what GitHub renders .org with) does not resolve the '*'"
  echo "    heading syntax -- it emits href=\"*$target\" literally, so the link is"
  echo "    dead on the rendered page. Use [[#custom-id][...]] against a"
  echo "    :PROPERTIES:/:CUSTOM_ID:/:END: drawer on the target heading."
  failed=$((failed + 1))
done < "$TMP/heading_links.txt"

# Collect the `:CUSTOM_ID:`s the file defines, and flag any that doesn't
# match the slug GitHub generates from its heading: lowercased, everything
# but letters/digits/hyphens dropped, spaces to hyphens.
: > "$TMP/custom_ids.txt"
: > "$TMP/bad_slugs.txt"
awk '
  function slug(s,   t) {
    t = tolower(s); gsub(/[^a-z0-9 -]/, "", t); gsub(/ /, "-", t); return t
  }
  /^\*+[ \t]/ { heading = $0; sub(/^\*+[ \t]+/, "", heading); next }
  /^[ \t]*:CUSTOM_ID:[ \t]*/ {
    id = $0; sub(/^[ \t]*:CUSTOM_ID:[ \t]*/, "", id); sub(/[ \t]+$/, "", id)
    print id > (ids)
    if (id != slug(heading)) printf "%d\t%s\t%s\t%s\n", NR, id, slug(heading), heading > (bad)
  }
' ids="$TMP/custom_ids.txt" bad="$TMP/bad_slugs.txt" "$ORG"

# `[[#slug][label]]` (and the bare `[[#slug]]`) -- the target has to be one
# of those ids.
anchors=0
grep -o '\[\[#[^]]*\]' "$ORG" 2>/dev/null | sed 's/^\[\[#//; s/\]$//' | sort -u > "$TMP/anchors.txt" || true
while IFS= read -r slug; do
  [ -n "$slug" ] || continue
  anchors=$((anchors + 1))
  if ! grep -qxF "$slug" "$TMP/custom_ids.txt"; then
    echo "$ORG: dead link: #$slug (no heading declares :CUSTOM_ID: $slug)"
    failed=$((failed + 1))
  fi
done < "$TMP/anchors.txt"

while IFS='	' read -r line id expected heading; do
  [ -n "$id" ] || continue
  echo "$ORG:$line: :CUSTOM_ID: $id does not match its heading"
  echo "    \"$heading\" gets the anchor #$expected on GitHub, so a [[#$id]]"
  echo "    link resolves here but 404s on the rendered page."
  failed=$((failed + 1))
done < "$TMP/bad_slugs.txt"

if [ "$failed" -ne 0 ]; then
  echo
  echo "check-doc: $failed problem(s) in $ORG"
  exit 1
fi
echo "check-doc: $ORG ok ($checked lisp blocks parsed, $skipped skipped, $links links resolved, $anchors anchors resolved)"
