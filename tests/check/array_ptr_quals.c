/* pointers to arrays of differently qualified elements */
void t(void *p, const void *pc)
{
    double x0[2], x1[2][2];
    const double z0[2], z1[2][2];
    const double (*a)[2];
    double (*v)[2];
    long d;
    a = pc;                         /* const void * -> pointer to const array */
    a = p;
    d = &x1[1] - &z1[0];
    d = x1 == z1;
    d = x1 < z1;
    d = &x0[1] - &z0[0];
    v = (1 ? z1 : v);
    v = (1 ? p : z1);
    v = (1 ? pc : x1);
    *(1 ? x0 : z0) = 1;
    (*(1 ? x1 : z1))[0] = 1;
    (void)(1 ? x0 : x1);
}
