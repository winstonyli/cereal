#!/bin/sh
# ccdb.sh DB: replay a compile_commands.json and compare with gcc.  Per C
# entry: $CEREAL_GCC (default gcc-13) in the entry's directory with its
# arguments minus -c, -o and the -M family plus -fsyntax-only, against
# `cereal check --compile-commands DB FILE`; diagnostics normalised as in
# corp.sh, plus the verdict.  Prints "N entries, M differ" (differences to
# $CCDB_OUT, default /tmp/ccdb.out), then one full replay at -j12 for the
# time and cereal's summary line.  CCDB_JOBS (default 8) bounds the
# comparison's concurrency.
exec python3 -P "$(dirname "$0")/ccdb.py" "$@" </dev/null
