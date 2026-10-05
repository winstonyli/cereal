// flags: -std=gnu99
/* access modes compared unless a VLA bound implies one; noipa acts as
 * noinline; packed after a tag reference is ignored, at the tag. */
/* a1 */
__attribute__((access(read_only, 2, 1))) void af1(int n, int a[n]);
__attribute__((access(write_only, 2, 1))) void af2(int n, int a[n]);
__attribute__((access(none, 2, 1))) void af3(int n, int a[n]);
__attribute__((access(read_write, 2, 1))) void af4(int n, int a[n]);
void af5(int n, int a[n]) __attribute__((access(read_only, 2, 1)));
void af6(int n, int a[n]); void af6(int n, int a[n]) __attribute__((access(read_only, 2, 1)));
void af7(int n, const int a[n]) __attribute__((access(write_only, 2, 1)));
__attribute__((access(read_only, 2, 1))) __attribute__((access(write_only, 2, 1))) void af8(int n, int a[n]);
void af9(int n, int a[n]) __attribute__((access(read_only, 2)));

/* k2 */
static inline __attribute__((noipa, always_inline)) int ka(void) { return 1; }
static inline __attribute__((always_inline, noipa)) int kb(void) { return 1; }
static inline __attribute__((noipa)) __attribute__((always_inline)) int kc(void) { return 1; }
static __attribute__((noipa)) int kd(void);
static __attribute__((always_inline)) int kd(void);
static __attribute__((always_inline)) int ke(void);
static __attribute__((noipa)) int ke(void);
static __attribute__((gnu_inline, noipa)) int kf(void);

/* n1 */
static inline int __attribute__((noipa)) na(void) { return 1; }
static inline int __attribute__((noinline)) nb(void) { return 1; }
static inline int __attribute__((noclone)) nc(void) { return 1; }
inline int __attribute__((noipa)) nd(void) { return 1; }
static int __attribute__((noipa)) ne(void) { return 1; }
static inline int nf(void);
static int __attribute__((noipa)) nf(void);
static int ng(void);
inline int __attribute__((noipa)) ng(void);
static inline int nh(void) __attribute__((noipa));
static inline int __attribute__((noipa, noinline)) ni(void) { return 1; }
static inline __attribute__((always_inline, noipa)) int nj(void) { return 1; }

/* p1 */
struct p { int a; };
struct u1;
typedef struct p __attribute__ ((packed)) t1;
typedef __attribute__ ((packed)) struct p t2;
__attribute__ ((packed)) typedef struct p t3;
typedef struct p t4 __attribute__ ((packed));
typedef struct u1 __attribute__ ((packed)) t5;
struct p __attribute__ ((packed)) v1;
struct p v2 __attribute__ ((packed));
typedef union { int a; } __attribute__ ((packed)) t6;
typedef struct p __attribute__ ((aligned(8))) t7;
typedef int __attribute__ ((packed)) t8;
typedef enum e { A } __attribute__ ((packed)) t9;
typedef enum e2 __attribute__ ((packed)) t10;
