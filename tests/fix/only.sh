# --only picks diagnostics by their option names
. "$TESTS/fix/lib.sh"
put lint/hygiene_fixit.c
put check/fixit-braces.c
run -Wall --only=missing-braces hygiene_fixit.c fixit-braces.c
run -Wall --only=macro-unparenthesized-param,macro-unparenthesized-body hygiene_fixit.c fixit-braces.c
run -Wall --only=nothing hygiene_fixit.c fixit-braces.c
