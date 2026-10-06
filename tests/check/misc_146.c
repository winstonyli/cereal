// flags: -Wcast-function-type
typedef void (*vv)(void);
typedef int (*i_l)(long);
typedef int (*i_ul)(unsigned long);
typedef int (*i_p)(void *);
typedef int (*i_cp)(const char *);
typedef int (*i_i)(int);
typedef int (*i_u)(unsigned);
typedef long (*l_l)(long);
typedef unsigned long (*ul_l)(long);
typedef int (*i_ll)(long long);
typedef int (*i_le)(long, ...);
typedef int (*i_un)();
typedef int (*i_ii)(int, int);
typedef void (*v_l)(long);
typedef char (*c_c)(char);
typedef short (*s_s)(short);
typedef _Bool (*b_b)(_Bool);
typedef int (*i_d)(double);
typedef float (*f_f)(float);
typedef int (*i_e)(enum {A,B});
typedef int (*i_s)(struct S *);
typedef int (*i_t)(struct T *);
typedef void (*v_un)();
typedef void *(*p_p)(void *);
typedef char *(*cp_p)(void *);
int f(long); int g(int); void h(void); int u(); long long ll(long long);
void foo(void)
{
#define T(e, ty) (void)(ty)(e);
  T(f, i_ul) T(f, i_p) T(f, i_cp) T(f, i_i) T(f, l_l) T(f, i_ll) T(f, i_le) T(f, i_un) T(f, i_ii) T(f, v_l)
  T(g, i_u) T(g, i_l) T(g, c_c) T(g, i_e) T(g, f_f)
  T(h, i_l) T(h, vv) T(f, vv) T(u, i_l) T(u, i_i) T(u, i_un)
  T(f, p_p) T(f, cp_p) T(f, b_b) T(f, s_s) T(f, i_d) T(f, v_un) T(f, void *) T(f, long)
  T(f, int (*)(struct S *)) T(f, int (*)(struct T *))
  { i_s a = 0; (void)(i_t)a; (void)(i_p)a; (void)(p_p)a; (void)(cp_p)(p_p)0; (void)(vv)a; (void)(vv)(v_l)0; (void)(v_l)(vv)0; }
}
