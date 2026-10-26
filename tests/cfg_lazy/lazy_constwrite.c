/* Writing a static const object is undefined and gcc's build crashes; the evaluator used to write it and leak the value into later calls. It must stop. */
typedef struct UV { int a; } UV;
typedef struct CL { UV *upvals[4]; int n; } CL;
static UV **ref(CL *f, int n)
{
    static UV *const nullup = 0;
    if (1 <= n && n <= f->n)
        return &f->upvals[n - 1];
    return (UV **)&nullup;
}
void join(CL *f1, int n1, CL *f2, int n2)
{
    UV **u1 = ref(f1, n1), **u2 = ref(f2, n2);
    *u1 = *u2;
}
