#!/bin/sh
# verify.sh: gcc.dg, c-c++-common and gcc.dg -O parity vs gcc-13 (needs ~/gccts).
# Writes /tmp/m_*.json (par.py) and /tmp/ds_*.txt (diagstat.py); prints the totals.
export CEREAL_GCC=gcc-13; T=$(dirname "$0"); C=${CEREAL:-$HOME/cereal-t/cereal}
CEREAL_DGOPTS=1 python3 "$T/par.py" "$C" dg /tmp/m_gcc.dg.json >/dev/null
CEREAL_DGOPTS=1 CEREAL_DGDIR=c-c++-common python3 "$T/par.py" "$C" dg /tmp/m_c-c++-common.json >/dev/null
CEREAL_DGOPTS=1 CEREAL_DGO=1 python3 "$T/par.py" "$C" dg /tmp/m_gcc.dg_O.json >/dev/null
python3 "$T/diagstat.py" /tmp/m_gcc.dg.json >/tmp/ds_gcc.dg.txt
python3 "$T/diagstat.py" /tmp/m_c-c++-common.json >/tmp/ds_c-c++-common.txt
python3 "$T/diagstat.py" /tmp/m_gcc.dg_O.json >/tmp/ds_O.txt
grep -H "^files" /tmp/ds_gcc.dg.txt /tmp/ds_c-c++-common.txt /tmp/ds_O.txt
