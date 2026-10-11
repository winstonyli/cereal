#!/bin/sh
# clangd_diff.sh [CEREAL] [--no-real]: cereal lsp against clangd (see clangd_diff.py)
d=$(dirname "$0")
exec nice python3 -P "$d/clangd_diff.py" "$@" </dev/null
