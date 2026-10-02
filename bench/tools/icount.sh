#!/bin/sh
# icount.sh CEREAL FILE [flags...]: instructions for `parse` and `check` of FILE
# (callgrind), which unlike wall time does not move with machine load.
cer=$1; f=$2; shift 2
for m in parse check; do
    printf '%s ' "$m"
    valgrind --tool=callgrind --callgrind-out-file=/dev/null "$cer" "$m" "$@" "$f" 2>&1 |
        sed -n 's/.*Collected : //p'
done
