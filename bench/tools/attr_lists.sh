#!/bin/sh
# attr_lists.sh: the attribute-name guard (docs/ATTR_TABLE_DESIGN.md "Guard").  Every per-name fact
# about an attribute lives in a row of src/c/attrs.def; this fails when
#   - the rows are not strictly strcmp-sorted and unique (attr_lookup bisects them), or
#   - a src/c file other than attrs.def/attrs.h holds an array initialiser (the outermost "= {" ... "};" of
#     a statement) of three or more string literals that are all attribute names, unless
#     bench/tools/attr_lists.allow lists it ("FILE ARRAY", FILE without the directory).
# An allowed entry that no longer matches is reported and fails too, so the list only shrinks.
# A text check: it does not look at strcmp chains, and misses a list built any other way.
# ATTR_ROOT overrides the tree (the self-check in docs/ATTR_TABLE_PLAN.md Task 6).
ROOT=${ATTR_ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}
cd "$ROOT" || exit 1
DEF=src/c/attrs.def
ALLOW=${ATTR_ALLOW:-bench/tools/attr_lists.allow}
T=${TMPDIR:-/tmp}/attr_lists.$$
trap 'rm -f $T.*' EXIT INT TERM
bad=0
[ -f $DEF ] || { echo "attr lists: no $DEF" >&2; exit 2; }
sed -n 's/^ATTR(\([^,]*\),.*/\1/p' $DEF >$T.names
if ! LC_ALL=C sort -c -u $T.names 2>$T.err; then
    echo "attr lists: $DEF rows are not strictly sorted and unique (LC_ALL=C sort -c -u): $(cat $T.err)"
    bad=1
fi
for f in src/c/*.c src/c/*.h; do
    case $f in src/c/attrs.h) continue ;; esac
    awk -v names=$T.names -v file="${f##*/}" '
        BEGIN { while ((getline l < names) > 0) isname[l] = 1 }
        function strip(s) { gsub(/"([^"\\]|\\.)*"/, "\"\"", s); return s }
        {
            if (!depth) {
                if (match($0, /[A-Za-z_][A-Za-z_0-9]*[ \t]*\[[^]]*\][ \t]*=[ \t]*\{/)) {
                    nm = substr($0, RSTART, RLENGTH); sub(/[ \t]*\[.*/, "", nm)
                    text = substr($0, RSTART)
                    cur = nm; lits = 0; allname = 1
                    scan(text); depth = 0; cnt(strip(text)); next
                }
                next
            }
            scan($0); cnt(strip($0))
        }
        function scan(s,   t, w) {
            t = s
            while (match(t, /"([^"\\]|\\.)*"/)) {
                w = substr(t, RSTART + 1, RLENGTH - 2); t = substr(t, RSTART + RLENGTH)
                lits++; if (!(w in isname)) allname = 0
            }
        }
        function cnt(s,   i, ch) {
            for (i = 1; i <= length(s); i++) {
                ch = substr(s, i, 1)
                if (ch == "{") depth++
                else if (ch == "}") { depth--; if (depth == 0) { done(); return } }
            }
        }
        function done() { if (lits >= 3 && allname) print file, cur }
    ' "$f" || echo "!awk $f"
done | sort -u >$T.hits
if grep -q "^!awk" $T.hits; then
    echo "attr lists: awk failed on: $(grep "^!awk" $T.hits)"
    exit 2
fi
awk '!/^#/ && NF >= 2 { print $1, $2 }' $ALLOW 2>/dev/null | sort -u >$T.allow
comm -13 $T.allow $T.hits >$T.new
comm -23 $T.allow $T.hits >$T.stale
if [ -s $T.new ]; then
    echo "attr lists: attribute-name lists outside $DEF (add the facts to a row there, or justify the list in $ALLOW):"
    sed 's/^/  /' $T.new
    bad=1
fi
if [ -s $T.stale ]; then
    echo "attr lists: allowed in $ALLOW but gone (delete the entry):"
    sed 's/^/  /' $T.stale
    bad=1
fi
[ $bad = 0 ] && echo "attr lists: ok"
exit $bad
