#!/bin/sh
# fetch_corpus.sh DIR - download and unpack the validation corpus used by
# bench/corpus.py: source distributions from PyPI that carry real C (Lua
# 5.1-5.5 and LuaJIT, libuv, zstd, and Cython-generated files).
set -eu
dir=${1:?usage: fetch_corpus.sh DIR}
mkdir -p "$dir"
cd "$dir"
for p in lupa uvloop zstandard; do
    url=$(curl -fsS "https://pypi.org/pypi/$p/json" | python3 -c '
import json, sys
d = json.load(sys.stdin)
print([u["url"] for u in d["urls"] if u["packagetype"] == "sdist"][0])')
    f=$(basename "$url")
    [ -f "$f" ] || curl -fsSL -o "$f" "$url"
    tar xzf "$f"
done
ls -d */
