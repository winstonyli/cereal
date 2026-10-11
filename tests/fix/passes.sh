# one error fix per pass (D6): two member accesses through pointers are
# fixed in two passes; --max-passes=1 leaves the second
. "$TESTS/fix/lib.sh"
cat >t/p.c <<'EOF'
struct S { int x; };
int f(struct S *p, struct S *q) { return p.x + q.x; }
EOF
cp t/p.c t/p2.c
snap
run --max-passes=1 p.c
run p.c
run p2.c --max-passes=3
run p2.c
