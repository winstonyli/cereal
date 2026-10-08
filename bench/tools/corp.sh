#!/bin/sh
# corp.sh DIR [INCLUDE...]: per C file in DIR, compare cereal and gcc-13
# diagnostics (file:line:col severity message, quotes and [-W...] tags
# normalised) under -std=gnu99 -Wall -Wextra.  Prints "N files, M differ"; the
# per-file differences ("<" gcc only, ">" cereal only) go to $CORP_OUT
# (default /tmp/corp.out).  CEREAL_GCC picks the reference compiler (default
# gcc-13: gcc 14+ makes implicit declarations errors).  Corpus sources are not
# vendored; the ones used so far are lupa-2.8 (Lua 5.1-5.5, LuaJIT) and
# uvloop-0.22.1 (libuv) under ~/corpus.
cd "$(dirname "$0")/../.." || exit 1
cereal=$PWD/cereal
gcc=${CEREAL_GCC:-gcc-13}
d=$1; shift
inc=""
for i in "$@"; do inc="$inc -I$i"; done
out=${CORP_OUT:-/tmp/corp.out}
g=$(mktemp); c=$(mktemp)
: > "$out"
n=0; bad=0
norm() {
  grep -a ': \(warning\|error\):' | sed "s/[‘’\`\"]/'/g; s/ \[-W[a-z=0-9-]*\]$//" | sort
}
for f in "$d"/*.c; do
  n=$((n+1))
  $gcc -std=gnu99 -Wall -Wextra -fsyntax-only $inc "$f" 2>&1 | norm >"$g"
  "$cereal" -fsyntax-only -std=gnu99 -Wall -Wextra $inc "$f" 2>&1 | norm >"$c"
  if ! cmp -s "$g" "$c"; then
    bad=$((bad+1))
    echo "== $f" >>"$out"
    diff "$g" "$c" | grep -a '^[<>]' | cut -c1-200 >>"$out"
  fi
done
rm -f "$g" "$c"
echo "$d: $n files, $bad differ"
