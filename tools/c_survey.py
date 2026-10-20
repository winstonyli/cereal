#!/usr/bin/env python3
"""Survey of C language features against cereal: writes the matrix of docs/C_STANDARD.md.

Usage: c_survey.py CEREAL > matrix.md        (run from anywhere; needs gcc, Linux)

Each feature is a small file defining `int t(int k)`.  Per feature the survey records
  gcc   whether gcc accepts it (a probe gcc rejects is marked "probe", not counted)
  front whether `cereal check` accepts it (diagnostics other than gcc's own notes: FAIL)
  back  whether `--dump-asm` emits `t` (else the reason), and, when it does and the
        feature is marked runnable, whether tests/asm_exec.py finds t equal to gcc's
The -std= values cereal accepts are listed first.  A C23 feature is probed under
cereal's -std=gnu23, the others under gnu17; gcc compiles each under its own gstd.
"""
import os, re, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ASM_EXEC = os.path.join(HERE, "..", "tests", "asm_exec.py")

# (area, id, introduced, source, run?)  run: compare behaviour with gcc
F = []
def f(area, id, since, src, run=True, gstd="gnu23"):
    F.append((area, id, since, src, run, gstd))

# ---- C89 / C95
f("C89", "digraphs", "C95", "%:define D 1\nint t(int k){ int a<:2:> = <%1,2%>; return a<:k&1:>+D; }")
f("C89", "iso646", "C95", "#include <iso646.h>\nint t(int k){ return k and 1 or not k; }")
f("C89", "kr-definition", "C89", "int g(a,b) int a; int b; { return a+b; }\nint t(int k){ return g(k,1); }", False, "gnu17")
f("C89", "implicit-int", "C89", "static g(int a){ return a; }\nint t(int k){ return g(k); }", False, "gnu17")
f("C89", "enum-switch", "C89", "enum E{A,B,C}; int t(int k){ enum E e=(enum E)(k&1); switch(e){case A:return 1;case B:return 2;default:return 3;} }")
f("C89", "bitfields", "C89", "struct B{ unsigned a:3; long long b:40; };\nint t(int k){ struct B s={0}; s.b=k; s.a=k; return (int)s.b+s.a; }")
f("C89", "varargs", "C89", "#include <stdarg.h>\nint s(int n,...){ va_list ap; va_start(ap,n); int r=0; while(n--) r+=va_arg(ap,int); va_end(ap); return r; }\nint t(int k){ return s(2,k,3); }")
# ---- C99
f("C99", "vla", "C99", "int t(int k){ int n=k&7; int a[n+1]; a[n]=3; return (int)(sizeof a/sizeof a[0]) + a[n]; }")
f("C99", "vla-param", "C99", "static int s(int n, int a[n]){ return a[n-1]; }\nint t(int k){ int b[3]={1,2,3}; return s(3,b)+k; }")
f("C99", "vla-typedef", "C99", "int t(int k){ int n=(k&3)+1; typedef int A[n]; A a; return (int)sizeof(A)+(int)sizeof a; }")
f("C99", "flexible-array", "C99", "struct F{int n; int d[];};\nint t(int k){ return (int)sizeof(struct F)+k; }")
f("C99", "designated-init", "C99", "struct S{int a,b;};\nint t(int k){ struct S s={.b=k,.a=2}; int a[4]={[2]=k}; return s.a+s.b+a[2]; }")
f("C99", "compound-literal", "C99", "struct S{int a;};\nint t(int k){ return ((struct S){k}).a + (int[]){1,2}[1]; }")
f("C99", "compound-literal-static", "C99", "int *gp=(int[]){1,2,3};\nint t(int k){ return gp[k&1]; }")
f("C99", "inline", "C99", "static inline int sq(int x){ return x*x; }\nint t(int k){ return sq(k); }", False)
f("C99", "restrict", "C99", "static int g(int *restrict a,int *restrict b){ return *a+*b; }\nint t(int k){ return g(&k,&k); }")
f("C99", "mixed-decls", "C99", "int t(int k){ int s=0; for(int i=0;i<4;i++){ s+=i*k; int j=s; s=j; } return s; }")
f("C99", "bool", "C99", "#include <stdbool.h>\nint t(int k){ bool b=k; b+=3; return b; }")
f("C99", "long-long", "C99", "int t(int k){ long long a=k; return (int)(a*3000000000LL>>3); }")
f("C99", "complex", "C99", "#include <complex.h>\nint t(int k){ double _Complex z = k + 2.0*I; return (int)creal(z*z); }", False)
f("C99", "hex-float", "C99", "int t(int k){ double d=0x1.8p1; return (int)d+k; }")
f("C99", "variadic-macro", "C99", "#define F(...) g(__VA_ARGS__)\nint g(int a,...){ return a; }\nint t(int k){ return F(k,1,2); }")
f("C99", "pragma-operator", "C99", "#define P _Pragma(\"GCC diagnostic ignored \\\"-Wall\\\"\")\nP int t(int k){ return k; }")
f("C99", "func-name", "C99", "int t(int k){ return __func__[0]+k; }")
f("C99", "ucn-identifier", "C99", "int t(int k){ int \\u00e9 = k; return \\u00e9; }")
f("C99", "va-copy", "C99", "#include <stdarg.h>\nint s(int n,...){ va_list a,b; va_start(a,n); va_copy(b,a); int r=va_arg(a,int)+va_arg(b,int); va_end(a); va_end(b); return r; }\nint t(int k){ return s(1,k); }")
f("C99", "trailing-enum-comma", "C99", "enum E{A,B,}; int t(int k){ return B+k; }")
f("C99", "float-env-rounding", "C99", "#include <math.h>\nint t(int k){ return (int)lrint(k*0.5); }", False)
# ---- C11
f("C11", "generic", "C11", "int t(int k){ return _Generic(k, int:1, long:2, default:3) + _Generic(1.0f, float:10, double:20); }")
f("C11", "static-assert", "C11", "_Static_assert(sizeof(int)==4,\"x\");\nint t(int k){ return k; }")
f("C11", "alignas", "C11", "#include <stdalign.h>\nstruct A{ alignas(16) char c; };\nint t(int k){ return (int)alignof(struct A)+k; }")
f("C11", "noreturn", "C11", "#include <stdnoreturn.h>\nnoreturn void d(void){ for(;;); }\nint t(int k){ if(k==12345) d(); return k; }")
f("C11", "thread-local", "C11", "_Thread_local int tl=3;\nint t(int k){ tl+=k; return tl; }")
f("C11", "atomic-qualifier", "C11", "_Atomic int a;\nint t(int k){ a=k; a+=2; return a; }")
f("C11", "stdatomic", "C11", "#include <stdatomic.h>\n_Atomic int a;\nint t(int k){ atomic_store(&a,k); atomic_fetch_add(&a,1); return atomic_load(&a); }")
f("C11", "anonymous-members", "C11", "struct S{ int a; union{ int b; float f; }; struct{int c;}; };\nint t(int k){ struct S s={0}; s.b=k; s.c=2; return s.a+s.b+s.c; }")
f("C11", "uchar", "C11", "#include <uchar.h>\nint t(int k){ char16_t c=u'x'; char32_t d=U'y'; return c+d+k+u8\"a\"[0]; }")
f("C11", "generic-struct-ptr", "C11", "struct S{int a;};\nint t(int k){ struct S s={k}; return _Generic(&s, struct S*:s.a, default:0); }")
f("C11", "quick-exit-header", "C11", "#include <stdlib.h>\nint t(int k){ return k; }", False)
# ---- C23
f("C23", "nullptr", "C23", "int t(int k){ int *p = nullptr; return p==nullptr ? k : 0; }")
f("C23", "constexpr", "C23", "constexpr int N=4;\nint t(int k){ int a[N]={0}; return a[0]+N+k; }")
f("C23", "typeof", "C23", "int t(int k){ typeof(k) a=k; typeof_unqual(const int) b=a; return a+b; }")
f("C23", "auto-type", "C23", "int t(int k){ auto x = k+1; return x; }")
f("C23", "bool-keyword", "C23", "int t(int k){ bool b = k; return b ? true : false; }")
f("C23", "attributes", "C23", "[[nodiscard]] int g(void){ return 1; }\n[[maybe_unused]] static int u;\nint t(int k){ [[maybe_unused]] int q=k; return g()+k; }")
f("C23", "fallthrough-attr", "C23", "int t(int k){ switch(k){ case 1: k++; [[fallthrough]]; case 2: k++; } return k; }")
f("C23", "digit-separator", "C23", "int t(int k){ return 1'000+k; }")
f("C23", "binary-literal", "C23", "int t(int k){ return 0b101 + k; }")
f("C23", "enum-underlying", "C23", "enum E : unsigned char { A=1, B };\nint t(int k){ return (int)sizeof(enum E)+B+k; }")
f("C23", "empty-initializer", "C23", "struct S{int a;};\nint t(int k){ struct S s={}; int a[2]={}; return s.a+a[1]+k; }")
f("C23", "static-assert-no-message", "C23", "static_assert(1);\nint t(int k){ return k; }")
f("C23", "bitint", "C23", "int t(int k){ _BitInt(7) b=k; return b; }")
f("C23", "unnamed-parameter", "C23", "static int g(int){ return 1; }\nint t(int k){ return g(k); }", False)
f("C23", "va-start-one-arg", "C23", "#include <stdarg.h>\nstatic int s(...){ va_list ap; va_start(ap); va_end(ap); return 0; }\nint t(int k){ return s(k); }")
f("C23", "u8-char-literal", "C23", "int t(int k){ return u8'a'+k; }")
f("C23", "alignas-keyword", "C23", "int t(int k){ alignas(8) int a=k; return alignof(int)+a; }")
f("C23", "thread-local-keyword", "C23", "thread_local int tl;\nint t(int k){ tl=k; return tl; }")
f("C23", "stdckdint", "C23", "#include <stdckdint.h>\nint t(int k){ int r; int o=ckd_add(&r,k,2147483647); return r+o; }")
f("C23", "stdbit", "C23", "#include <stdbit.h>\nint t(int k){ return (int)stdc_count_ones((unsigned)k); }")
f("C23", "unreachable", "C23", "#include <stddef.h>\nint t(int k){ if(k>1000000) unreachable(); return k; }")
f("C23", "label-before-declaration", "C23", "int t(int k){ goto e; e: int x=k; return x; }")
f("C23", "decimal-float", "C23", "int t(int k){ _Decimal32 d = k; return (int)d; }")
f("C23", "floatN", "C23", "int t(int k){ _Float32 f=k; _Float64 d=f; return (int)d; }")
# ---- GNU C
f("GNU", "statement-expression", "GNU", "int t(int k){ return ({ int a=k; a*2; }); }")
f("GNU", "typeof-gnu", "GNU", "int t(int k){ __typeof__(k) a=k; return a; }")
f("GNU", "auto-type-gnu", "GNU", "int t(int k){ __auto_type a=k; return a; }")
f("GNU", "computed-goto", "GNU", "int t(int k){ void *p = k ? &&a : &&b; goto *p; a: return 1; b: return 2; }")
f("GNU", "label-table", "GNU", "int t(int k){ static void *tab[]={&&a,&&b}; goto *tab[k&1]; a: return 1; b: return 2; }")
f("GNU", "case-range", "GNU", "int t(int k){ switch(k){ case 1 ... 5: return 1; default: return 0; } }")
f("GNU", "conditional-omitted", "GNU", "int t(int k){ return k ?: 7; }")
f("GNU", "zero-length-array", "GNU", "struct Z{int n; int d[0];};\nint t(int k){ return (int)sizeof(struct Z)+k; }")
f("GNU", "nested-function", "GNU", "int t(int k){ int sq(int x){ return x*k; } return sq(3); }")
f("GNU", "local-label", "GNU", "int t(int k){ __label__ l; goto l; l: return k; }")
f("GNU", "int128", "GNU", "int t(int k){ __int128 a=k; a*=a; return (int)(a>>2); }")
f("GNU", "long-double", "GNU", "int t(int k){ long double d=k; d*=1.5L; return (int)d; }")
f("GNU", "float128", "GNU", "int t(int k){ __float128 d=k; d*=1.5; return (int)d; }")
f("GNU", "complex-parts", "GNU", "int t(int k){ _Complex double z=k; __real__ z=2; return (int)(__real__ z+__imag__ z); }")
f("GNU", "range-designator", "GNU", "int t(int k){ int a[8]={[2 ... 5]=k}; return a[3]; }")
f("GNU", "empty-struct", "GNU", "struct E{};\nint t(int k){ struct E e; return (int)sizeof e+k; }")
f("GNU", "attr-packed", "GNU", "struct __attribute__((packed)) P{char c; int i;};\nint t(int k){ return (int)sizeof(struct P)+k; }")
f("GNU", "attr-aligned", "GNU", "struct __attribute__((aligned(32))) A{char c;};\nint t(int k){ return (int)sizeof(struct A)+k; }")
f("GNU", "attr-cleanup", "GNU", "static void c(int *p){ *p=0; }\nint t(int k){ int x __attribute__((cleanup(c)))=k; return x; }")
f("GNU", "attr-alias", "GNU", "int a(void){ return 1; }\nint b(void) __attribute__((alias(\"a\")));\nint t(int k){ return b()+k; }")
f("GNU", "attr-weak", "GNU", "int w(void) __attribute__((weak));\nint t(int k){ return (w ? 1 : 0)+k; }", False)
f("GNU", "attr-constructor", "GNU", "static void c(void) __attribute__((constructor));\nstatic void c(void){}\nint t(int k){ return k; }")
f("GNU", "attr-section", "GNU", "int v __attribute__((section(\".foo\")))=1;\nint t(int k){ return v+k; }")
f("GNU", "attr-unused-used-noinline", "GNU", "static int a __attribute__((unused));\nstatic int __attribute__((noinline)) g(int x){ return x; }\nint t(int k){ return g(k); }", False)
f("GNU", "vector-size", "GNU", "typedef int v4 __attribute__((vector_size(16)));\nint t(int k){ v4 a={k,1,2,3}; v4 b=a+a; return b[0]+b[3]; }")
f("GNU", "asm-statement", "GNU", "int t(int k){ __asm__ volatile(\"\" ::: \"memory\"); return k; }")
f("GNU", "asm-operands", "GNU", "int t(int k){ int r; __asm__(\"movl %1,%0\" : \"=r\"(r) : \"r\"(k)); return r; }")
f("GNU", "named-variadic-macro", "GNU", "#define M(args...) g(args)\nint g(int a,...){ return a; }\nint t(int k){ return M(k,1); }")
f("GNU", "extension-keyword", "GNU", "int t(int k){ return __extension__ ({ k; }); }")
f("GNU", "builtin-choose-expr", "GNU", "int t(int k){ return __builtin_choose_expr(__builtin_types_compatible_p(int,int), k, 0); }")
f("GNU", "builtin-overflow", "GNU", "int t(int k){ int r; int o=__builtin_add_overflow(k,2147483647,&r); int q; o+=__builtin_mul_overflow(k,k,&q); return r+o+q; }")
f("GNU", "builtin-alloca", "GNU", "int t(int k){ char *p=__builtin_alloca(16); p[3]=k; return p[3]; }")
f("GNU", "builtin-offsetof", "GNU", "struct S{char c; int i;};\nint t(int k){ return (int)__builtin_offsetof(struct S,i)+k; }")
f("GNU", "builtin-constant-p", "GNU", "int t(int k){ return __builtin_constant_p(k) + __builtin_constant_p(3); }")
f("GNU", "builtin-frame-address", "GNU", "int t(int k){ return __builtin_frame_address(0)!=0 ? k : 0; }")
f("GNU", "builtin-rotate", "GNU", "int t(int k){ return (int)__builtin_rotateleft32((unsigned)k,3); }", True, "gnu23")
f("GNU", "builtin-mathfn", "GNU", "int t(int k){ return (int)__builtin_fabs((double)k)+(int)__builtin_floor(k*0.5); }", False)
f("GNU", "builtin-trap", "GNU", "int t(int k){ if(k==12345) __builtin_trap(); return k; }")
f("GNU", "builtin-object-size", "GNU", "int t(int k){ char b[8]; return (int)__builtin_object_size(b,0)+k; }")
# ---- preprocessor
f("PP", "has-include", "C23", "#if __has_include(<stdio.h>) && !__has_include(<nonexistent_zz.h>)\n#define V 5\n#else\n#define V 9\n#endif\nint t(int k){ return V+k; }")
f("PP", "has-attribute", "GNU", "#if defined(__has_attribute) && __has_attribute(packed)\n#define V 5\n#else\n#define V 9\n#endif\nint t(int k){ return V+k; }")
f("PP", "has-builtin", "GNU", "#if defined(__has_builtin) && __has_builtin(__builtin_expect)\n#define V 5\n#else\n#define V 9\n#endif\nint t(int k){ return V+k; }")
f("PP", "has-c-attribute", "C23", "#if __has_c_attribute(nodiscard)\n#define V 5\n#else\n#define V 9\n#endif\nint t(int k){ return V+k; }")
f("PP", "va-opt", "C23", "#define F(a,...) g(a __VA_OPT__(,) __VA_ARGS__)\nint g(int a,...){ return a; }\nint t(int k){ return F(k)+F(k,1); }")
f("PP", "elifdef", "C23", "#define A\n#ifdef B\n#define V 0\n#elifdef A\n#define V 1\n#else\n#define V 2\n#endif\nint t(int k){ return V+k; }")
f("PP", "embed", "C23", "static const unsigned char d[]={\n#embed \"emb.bin\"\n};\nint t(int k){ return d[1]+k; }")
f("PP", "has-embed", "C23", "#if __has_embed(\"emb.bin\") == __STDC_EMBED_FOUND__\n#define V 1\n#else\n#define V 2\n#endif\nint t(int k){ return V+k; }")
f("PP", "include-next", "GNU", "#include_next <limits.h>\nint t(int k){ return k; }")
f("PP", "warning-directive", "GNU", "#warning hi\nint t(int k){ return k; }")
f("PP", "counter", "GNU", "int t(int k){ return __COUNTER__ + __COUNTER__ + k; }")
f("PP", "pragma-pack", "GNU", "#pragma pack(push,1)\nstruct P{char c;int i;};\n#pragma pack(pop)\nint t(int k){ return (int)sizeof(struct P)+k; }")
f("PP", "stdc-macros", "C11", "#if __STDC_HOSTED__ && defined(__STDC_UTF_16__) && !defined(__STDC_NO_ATOMICS__)\n#define V 1\n#else\n#define V 0\n#endif\nint t(int k){ return V+k; }")
f("PP", "line-file-date", "C89", "int t(int k){ return __LINE__ + (__DATE__[0]!=0) + k; }", False)


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, stdin=subprocess.DEVNULL, **kw)


def main():
    cereal = os.path.abspath(sys.argv[1])
    d = tempfile.mkdtemp(prefix="csurvey")
    with open(os.path.join(d, "emb.bin"), "wb") as fh:
        fh.write(bytes([7, 8, 9]))
    a = os.path.join(d, "a.c")
    with open(a, "w") as fh:
        fh.write("int main(void){return 0;}\n")
    stds = [s for s in "c89 c90 gnu89 c99 gnu99 c11 gnu11 c17 gnu17 c2x gnu2x c23 gnu23".split()
            if run([cereal, "check", "-std=" + s, a]).returncode == 0]
    print("cereal accepts -std= %s\n" % ", ".join(stds))
    rows = []
    for area, id, since, src, runit, gstd in F:
        p = os.path.join(d, id + ".c")
        with open(p, "w") as fh:
            fh.write(src + "\n")
        g = run(["gcc", "-std=" + gstd, "-fsyntax-only", "-w", p])
        if g.returncode:
            rows.append((area, id, since, "probe", "", ""))
            continue
        cstd = "-std=gnu23" if since == "C23" else "-std=gnu17"
        c = run([cereal, "check", "--target=x86_64-linux-gnu", cstd, p])
        front = "ok" if c.returncode == 0 else "FAIL " + (re.search(r"error: (.*)", c.stdout + c.stderr) or [None, "?"])[1][:60]
        back = ""
        if c.returncode == 0:
            asm = run([cereal, "check", "--target=x86_64-linux-gnu", cstd, "--dump-asm", p]).stdout
            m = re.search(r"^# x86: (\S+) unsupported: (.*)$", asm, re.M)
            if m:
                back = "unsup: " + ("" if m.group(1) == "t" else m.group(1) + ": ") + m.group(2)[:60]
            elif "\nt:\n" not in asm:
                u = re.search(r"^# unit \S+ unsupported: (.*)$", asm, re.M)
                back = "unsup: " + (u.group(1)[:60] if u else "not emitted")
            elif runit and gstd == "gnu23":
                r = run(["python3", "-P", ASM_EXEC, cereal, p], cwd=os.path.join(HERE, "..", "tests", "cfg_exec"),
                        env={**os.environ, "CEREAL_FLAGS": cstd})
                back = "ok, equals gcc" if r.returncode == 0 else "DIFFERS"
            else:
                back = "emits"
        rows.append((area, id, since, "ok", front, back))
    print("| area | feature | since | gcc | front end | back end |\n|---|---|---|---|---|---|")
    for r in rows:
        print("| " + " | ".join(r) + " |")
    n = [r for r in rows if r[3] == "ok"]
    print("\n%d probes: front end ok %d, back end ok %d, probes gcc rejects %d" % (
        len(rows), sum(r[4] == "ok" for r in n), sum(r[5].startswith(("ok", "emits")) for r in n),
        len(rows) - len(n)))


main()
