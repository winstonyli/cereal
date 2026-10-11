# SAFE fixes over several files at once: macro parentheses (lint), missing
# braces, header insertions and a definition's (void) (check).  The second
# run changes nothing and prints nothing.
. "$TESTS/fix/lib.sh"
put lint/hygiene_fixit.c
put check/fixit-braces.c
put check/fixit-include-bare.c
put check/fixit-proto.c
run -Wall -Wstrict-prototypes hygiene_fixit.c fixit-braces.c fixit-include-bare.c fixit-proto.c
run -Wall -Wstrict-prototypes hygiene_fixit.c fixit-braces.c fixit-include-bare.c fixit-proto.c
