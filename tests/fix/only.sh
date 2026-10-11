# --only picks diagnostics by their option names
. "$TESTS/fix/lib.sh"
put lint/hygiene_fixit.c
put check/fixit-braces.c
run -Wall --only=missing-braces hygiene_fixit.c fixit-braces.c
run -Wall --only=macro-unparenthesized-param,macro-unparenthesized-body hygiene_fixit.c fixit-braces.c
run -Wall --only=nothing hygiene_fixit.c fixit-braces.c
# an id no fix-it can carry is a usage error naming it, whatever else is listed
run -Wall --only=missing-braces,bogus-id hygiene_fixit.c fixit-braces.c
run -Wall --only= hygiene_fixit.c
# every id that can carry one is accepted
run -Wall --only=error,builtin-declaration-mismatch,cond-typo,format=,header-guard,implicit-function-declaration,logical-not-parentheses,macro-unparenthesized-body,macro-unparenthesized-param,missing-braces,parentheses,strict-prototypes,xor-used-as-pow hygiene_fixit.c
