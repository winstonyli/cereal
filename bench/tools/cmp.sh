#!/bin/sh
# cmp.sh FILE [flags]: diff gcc-13 and cereal error/warning header lines for one
# file (< gcc only, > cereal only); prints SAME when they agree.
f=$1; shift
d=$(dirname "$f"); b=$(basename "$f")
CER=${CEREAL:-$HOME/cereal-t/cereal}
hdr() { grep -E "^[^ ]+:[0-9]+:[0-9]+: (fatal error|error|warning): " | sed "s/^\.\///"; }
cd "$d" || exit 1
LC_ALL=C gcc-13 -fsyntax-only -std=c99 -pedantic -I. -I.. "$@" "$b" 2>&1 | hdr >/tmp/cmp_g.$$
"$CER" -fsyntax-only -std=c99 -pedantic -I. -I.. "$@" "$b" 2>&1 | hdr >/tmp/cmp_c.$$
if diff /tmp/cmp_g.$$ /tmp/cmp_c.$$ >/tmp/cmp_d.$$; then echo SAME; else cat /tmp/cmp_d.$$; fi
rm -f /tmp/cmp_g.$$ /tmp/cmp_c.$$ /tmp/cmp_d.$$
