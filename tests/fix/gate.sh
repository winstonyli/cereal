# the gate: a hygiene fix that would change a diagnostic at a use (the
# parentheses make the -Wparentheses warning go away) fails the semantic
# check; the SAFE fix that passes goes in, the other waits for --unsafe
. "$TESTS/fix/lib.sh"
cat >t/g.c <<'EOF'
#define OR(x) x || 0
int f(int a, int b) { return OR(a && b); }
EOF
snap
run -Wall --diff g.c
run -Wall g.c
run -Wall --unsafe g.c
run -Wall --unsafe g.c
