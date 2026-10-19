/* Structs across the System V ABI for tests/asm_exec.py: abi.c is compiled by
 * us, abi.abi.c by gcc; each side calls the other. */
struct s2i { int a, b; };
struct s2l { long long a, b; };
struct sd { double d; };
struct sdd { double x, y; };
struct sid { int i; double d; };
struct sdi { double d; long long l; };
struct sff { float x, y; };
struct sf3 { float v[3]; };
struct sch { char a; short b; };
struct sbig { long long v[5]; };
struct nest { struct s2i p; struct sd q; };
union ul { long long l; double d; };

/* gcc-built callees (abi.abi.c) */
struct s2i ext_s2i(int k);
struct s2l ext_s2l(int k);
struct sd ext_sd(int k);
struct sdd ext_sdd(int k);
struct sid ext_sid(int k);
struct sdi ext_sdi(int k);
struct sff ext_sff(int k);
struct sf3 ext_sf3(int k);
struct sch ext_sch(int k);
struct sbig ext_sbig(int k);
struct nest ext_nest(int k);
union ul ext_ul(int k);
long long ext_take(struct s2i a, struct s2l b, struct sd c, struct sdd d, struct sid e,
                   struct sdi f, struct sff g, struct sf3 h, struct sch i, struct nest j,
                   union ul u);
long long ext_big(int a, int b, int c, int d, int e, struct sbig big, struct s2l l, int z);
long long ext_spill(int a, int b, int c, int d, int e, struct s2l l, double x, struct sdd y);

/* our callees (abi.c) */
struct s2l mine_s2l(struct s2l a, int k);
struct sdi mine_sdi(int k, struct sdi a);
struct sbig mine_sbig(struct sbig a, int k);
struct sid mine_sid(struct sid a, struct sff b);
long long mine_take(struct s2i a, struct s2l b, struct sd c, struct sdd d, struct sid e,
                    struct sdi f, struct sff g, struct sf3 h, struct sch i, struct nest j,
                    union ul u);

union tp { int *ip; long long *lp; } __attribute__((transparent_union));
long long ext_tp(union tp p);
