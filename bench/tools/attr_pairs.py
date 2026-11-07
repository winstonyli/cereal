#!/usr/bin/env python3
# attr_pairs.py CEREAL_BIN [SHOW [TOP [CEILING]]]: probe every ordered pair of 16 attributes in 9 declaration contexts
# (function returning int/void/pointer, redeclaration, function-pointer variable, variable, after a
# _Noreturn declaration, definition) against gcc and print how many output lines differ, per context, and
# the TOP most common message shapes with an example.  SHOW (default 0) lists that many lines per context.
# Compares the ordered warning/error messages per declaration line (not columns).  Needs gcc 15.2 (the
# PARKED.md "Attribute exclusion gaps" evidence is from it).  Writes /tmp/stress.c.  Not part of the gate.
# With CEILING (bench/attr_pairs.ceiling) it exits 1 if a context differs on more lines than its ceiling,
# and 2 if one differs on fewer (lower the ceiling), so the counts only shrink.
# Run: python3 -P bench/tools/attr_pairs.py ./cereal 0 12 [bench/attr_pairs.ceiling] </dev/null
import itertools, subprocess, re, sys, collections
A = ["pure","const","noreturn","malloc","cold","hot","noinline","always_inline","returns_twice",
     "warn_unused_result","common","nocommon","alloc_size(1)","alloc_align(1)","leaf","nothrow"]
T = [
 ("fn-int",   lambda n,a,b: ["int f%d(int) __attribute__((%s, %s));"%(n,a,b)]),
 ("fn-void",  lambda n,a,b: ["void f%d(void) __attribute__((%s, %s));"%(n,a,b)]),
 ("fn-ptr",   lambda n,a,b: ["char *f%d(int) __attribute__((%s, %s));"%(n,a,b)]),
 ("redecl",   lambda n,a,b: ["int f%d(int) __attribute__((%s));"%(n,a), "int f%d(int) __attribute__((%s));"%(n,b)]),
 ("redecl-v", lambda n,a,b: ["void f%d(void) __attribute__((%s));"%(n,a), "void f%d(void) __attribute__((%s));"%(n,b)]),
 ("fnptr-var",lambda n,a,b: ["int (*p%d)(int) __attribute__((%s, %s));"%(n,a,b)]),
 ("var",      lambda n,a,b: ["int v%d __attribute__((%s, %s));"%(n,a,b)]),
 ("kw-redecl",lambda n,a,b: ["_Noreturn int k%d(int);"%n, "int k%d(int) __attribute__((%s, %s));"%(n,a,b)]),
 ("def",      lambda n,a,b: ["int d%d(int) __attribute__((%s));"%(n,b), "__attribute__((%s)) int d%d(int x) { return x; }"%(a,n)]),
]
lines=[]; meta=[]
n=0
for tname,t in T:
    for a,b in itertools.product(A,A):
        if a==b: continue
        n+=1
        for l in t(n,a,b):
            lines.append(l); meta.append((tname,a,b))
open("/tmp/stress.c","w").write("\n".join(lines)+"\n")
hdr=re.compile(r"^([^ :]+):(\d+):(\d+): (warning|error): (.*)$")
def run(cmd):
    p=subprocess.run(cmd+["-fsyntax-only","-std=gnu11","-Wall","-Wextra","/tmp/stress.c"],capture_output=True,text=True,errors="replace")
    out=collections.defaultdict(list)
    for l in (p.stdout+p.stderr).splitlines():
        m=hdr.match(l)
        if not m: continue
        msg=re.sub(r"[‘’`]","'",m.group(5)); msg=re.sub(r" \[-W[a-z=0-9-]*\]$","",msg)
        out[int(m.group(2))].append(msg)
    return out
g=run(["gcc"]); c=run([sys.argv[1]])
TOP=int(sys.argv[3]) if len(sys.argv)>3 else 12
bad=collections.defaultdict(list)
for i,(tname,a,b) in enumerate(meta,1):
    if g.get(i,[])!=c.get(i,[]):
        bad[tname].append((i,a,b,g.get(i,[]),c.get(i,[])))
print("lines",len(lines),"differ",sum(len(v) for v in bad.values()))
for tname,v in bad.items():
    print("==",tname,len(v))
    for i,a,b,gm,cm in v[:(int(sys.argv[2]) if len(sys.argv)>2 else 0)]:
        print(" ",lines[i-1][:90]); print("    gcc:",gm); print("    cer:",cm)
sig=collections.Counter(); ex={}
for tname,v in bad.items():
    for i,a,b,gm,cm in v:
        go=[m for m in gm if m not in cm]; co=[m for m in cm if m not in gm]
        if not go and not co: s=("order",)
        else:
            f=lambda m: re.sub(r"'[a-z_0-9()]+'","'X'",m)
            s=(tname,"gcc-only:"+"|".join(sorted(f(m) for m in go)),"cer-only:"+"|".join(sorted(f(m) for m in co)))
        sig[s]+=1; ex.setdefault(s,(lines[i-1][:100],gm,cm))
for s,k in sig.most_common(TOP):
    print(k,s); print("    e.g.",ex[s][0]); print("    gcc:",ex[s][1]); print("    cer:",ex[s][2])
if len(sys.argv)>4:
    ceil={}
    for ln in open(sys.argv[4]):
        if ln.strip() and not ln.startswith("#"):
            k,n=ln.split(); ceil[k]=int(n)
    up=[(k,len(bad.get(k,[])),ceil.get(k,0)) for k in set(bad)|set(ceil) if len(bad.get(k,[]))>ceil.get(k,0)]
    down=[(k,len(bad.get(k,[])),ceil.get(k,0)) for k in set(bad)|set(ceil) if len(bad.get(k,[]))<ceil.get(k,0)]
    for k,n,m in sorted(up): print("OVER",k,n,">",m)
    for k,n,m in sorted(down): print("LOWER the ceiling",k,n,"<",m)
    sys.exit(1 if up else 2 if down else 0)
