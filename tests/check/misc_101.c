struct data { char c; int i; } __attribute__((packed));
extern void bar(int *);
int *r1(struct data *p) { return (int *) &p->i; }
int *r2(struct data *p) { return &p->i; }
void s1(struct data *p) { int *q = (int *) &p->i; (void)q; }
void s2(struct data *p) { int *q = &p->i; (void)q; }
void s3(struct data *p) { int *q; q = (int *) &p->i; q = (int *)(&p->i); (void)q; }
void s4(struct data *p) { bar((int *) &p->i); bar(&p->i); bar((int *)(&p->i)); }
void s5(struct data *p) { char *q = (char *) &p->i; (void)q; }
void s6(struct data *p) { long *q = (long *) &p->i; (void)q; }
