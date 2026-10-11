# MAYBE fixes (a did-you-mean, a header guard whose misspelt name is tested
# elsewhere) are left alone, and applied with --unsafe
. "$TESTS/fix/lib.sh"
put check/fixit-assign.c
put check/fixit-typo.c
put lint/guard_fixit.c
put lint/inc/guard_safe.h inc/guard_safe.h
put lint/inc/guard_maybe.h inc/guard_maybe.h
run -Wall fixit-assign.c fixit-typo.c guard_fixit.c -Iinc
run -Wall --unsafe fixit-assign.c fixit-typo.c guard_fixit.c -Iinc
run -Wall --unsafe fixit-assign.c fixit-typo.c guard_fixit.c -Iinc
