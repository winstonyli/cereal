# --diff and --dry-run change nothing and exit 1 when there is something to
# do; --apply is the default; the options exclude each other
. "$TESTS/fix/lib.sh"
put check/fixit-braces.c
put lint/hygiene_fixit.c
run -Wall --diff fixit-braces.c hygiene_fixit.c
run -Wall --dry-run fixit-braces.c hygiene_fixit.c
run -Wall --diff --dry-run fixit-braces.c
run -Wall --apply --diff fixit-braces.c
run -Wall --apply fixit-braces.c hygiene_fixit.c
run -Wall --diff fixit-braces.c hygiene_fixit.c
run -Wall --dry-run fixit-braces.c hygiene_fixit.c
