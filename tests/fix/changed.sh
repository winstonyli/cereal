# a file saved by an editor between the check and the write is not
# overwritten, and is fixed in the next pass (CEREAL_FIX_HOOK stands in for
# the editor)
. "$TESTS/fix/lib.sh"
printf '#define SUM 1 + 2\nint v = SUM;\n' >t/e.c
printf '#define SUM 1 + 2\nint v = SUM;\n' >t/f.c
snap
export CEREAL_FIX_HOOK='grep -q saved e.c || echo "int saved;" >>e.c'
run e.c f.c
unset CEREAL_FIX_HOOK
run e.c f.c
