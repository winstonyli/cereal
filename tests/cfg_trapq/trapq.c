/* A solved call that traps (a null read, undefined behaviour) still has a path: the branches before the
 * trap can be flipped.  tests/eval_trapq.sh runs this under --eval-solve; it is not in the gcc-compared corpus. */

int deep(int x, int y, int z)
{
    if (x == 77) {                  /* the seeds miss it; the solver takes it, and that call traps below */
        if (y * 7 == 12341)  /* only the trapped call's path has this branch */
            return 1;
        return *(const int *)(long)(z & 0);
    }
    return 0;
}
