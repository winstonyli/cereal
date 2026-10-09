#!/bin/sh
exec nice -n 10 python3 -P "$(dirname "$0")/symcov.py" "$@" </dev/null
