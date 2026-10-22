/* shared declarations of link_a.c and link_b.c (tests/eval_sweep.sh with EVAL_SWEEP_LINK=1) */
struct pair {
    int a, b;
};
extern int shared_base;
int a_twice(int x);
int b_guard(int x);
int b_sum(struct pair p);
int b_apply(int (*f)(int), int x);
struct pair b_make(int x);
