#!/bin/sh
# globals.sh: the mutable-global gate (docs/B5_DESIGN.md 4.1).  Index checks
# run on several threads, so a new .data/.bss symbol in an object must be
# justified in bench/tools/globals.allow (one line per "OBJECT SYMBOL reason";
# OBJECT is the path under build/src without .o).  RELRO data and TLS cannot
# race and are not listed.  Function-local statics are matched without their
# ".N" suffix.  Fails on a symbol not allowed; prints allowed ones no longer
# there.  Run after a build.
cd "$(dirname "$0")/../.." || exit 1
ALLOW=bench/tools/globals.allow
T=${TMPDIR:-/tmp}/globals.$$
trap 'rm -f $T.*' EXIT INT TERM
[ -d build/src ] || { echo "globals: no build/src (run make first)" >&2; exit 2; }
find build/src -name '*.o' | sort | while read -r o; do
    objdump -t "$o" | awk -v k="${o#build/src/}" '
        NF >= 6 && $(NF-3) == "O" && $(NF-2) ~ /^\.(bss|data)(\.|$)/ && $(NF-2) !~ /^\.data\.rel\.ro/ {
            n = $NF; sub(/\.[0-9]+$/, "", n); sub(/\.o$/, "", k); print k, n }'
done | sort -u >$T.cur
awk '!/^#/ && NF >= 2 { print $1, $2 }' $ALLOW | sort -u >$T.allow
comm -13 $T.allow $T.cur >$T.new
comm -23 $T.allow $T.cur >$T.stale
if [ -s $T.stale ]; then echo "globals: allowed but gone:"; sed 's/^/  /' $T.stale; fi
if [ -s $T.new ]; then
    echo "globals: mutable globals not in $ALLOW (justify them there, or make them const, atomic or __thread):"
    sed 's/^/  /' $T.new
    exit 1
fi
echo "globals: $(wc -l <$T.cur) mutable globals, all allowed"
