#!/bin/sh
# Fetch the gcc 13.3 sources the checker was ported from (reference only;
# not built).  Usage: fetch_gcc_refs.sh [DIR]   (default: ./gcc-refs)
set -e
D=${1:-gcc-refs}; mkdir -p "$D"
B=https://raw.githubusercontent.com/gcc-mirror/gcc/releases/gcc-13.3.0
for f in gcc/c/c-decl.cc gcc/c/c-parser.cc gcc/c/c-typeck.cc gcc/c/c-lex.cc \
         libcpp/charset.cc libcpp/expr.cc; do
    curl -fsSL "$B/$f" -o "$D/$(basename "$f")"
done
