#!/bin/sh
# edge_union.sh ERR...: the untaken edges of several --eval-miss runs (same program, other seeds), from their
# `cereal: edge FN BLOCK.EDGE CLASS` lines: how many are untaken in 1, 2, ... N runs, and the classes of those untaken in all.
# The union of taken edges is the total minus the "all N" row.  Class labels are the first file's.
[ $# -ge 1 ] || { echo "usage: edge_union.sh ERR..." >&2; exit 2; }
N=$#
for f in "$@"; do grep '^cereal: edge ' "$f" | awk '{print $3, $4}' | sort -u; done | sort | uniq -c |
    awk '{n[$1]++} END{for (k in n) print n[k], "edges untaken in", k, "of '$N' runs"}' | sort -k5n
for f in "$@"; do grep '^cereal: edge ' "$f" | awk '{print $3, $4}' | sort -u; done | sort | uniq -c | awk -v n=$N '$1==n{print $2" "$3}' | sort > "${TMPDIR:-/tmp}/edge_union.$$"
echo "classes of the edges untaken in all $N:"
grep '^cereal: edge ' "$1" | awk 'NR==FNR{k[$1" "$2]=1; next} (($3" "$4) in k){c=$5; for(i=6;i<=NF;i++)c=c" "$i; n[c]++} END{for(c in n) print n[c], c}' "${TMPDIR:-/tmp}/edge_union.$$" - | sort -rn
rm -f "${TMPDIR:-/tmp}/edge_union.$$"
