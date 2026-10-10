#!/bin/sh
# mkccdb.sh OUT DIR [FLAGS...] -- FILES...: write a compile_commands.json (no
# build system needed) with one entry per file, run in DIR (made absolute):
#   cc FLAGS... -c FILE
# FILES are relative to DIR or absolute.  For tests and for corpus projects
# whose build has no compilation-database export.
set -eu
[ $# -ge 3 ] || { echo "usage: mkccdb.sh OUT DIR [FLAGS...] -- FILES..." >&2; exit 2; }
out=$1 dir=$(cd "$2" && pwd)
shift 2
q() { printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g'; }
flags=""
while [ $# -gt 0 ] && [ "$1" != "--" ]; do flags="$flags \"$(q "$1")\","; shift; done
[ $# -gt 0 ] || { echo "mkccdb.sh: missing --" >&2; exit 2; }
shift
{
    printf '[\n'
    first=1
    for f in "$@"; do
        [ $first = 1 ] || printf ',\n'
        first=0
        printf ' {"directory": "%s", "file": "%s", "arguments": ["cc",%s "-c", "%s"]}' \
            "$(q "$dir")" "$(q "$f")" "$flags" "$(q "$f")"
    done
    printf '\n]\n'
} >"$out"
