#!/usr/bin/env python3
"""par.py CEREAL SET OUT.json [extra flags...]   SET = corpus|dg
CEREAL_GCCTS (default ~/gccts: sparse gcc-13.3.0 checkout of gcc/testsuite/gcc.dg),
CEREAL_CORPUS (default ~/corpus), CEREAL_PYINC as for corpus.py; CEREAL_GCC (default gcc)
should be 13 (messages and permerrors differ in 14+).
CEREAL_DGDIR (default gcc.dg; e.g. c-c++-common) picks the testsuite directory.
CEREAL_DGOPTS=1 (dg set): honour each test's first `dg-options` line (replacing
the default -std=c99 -pedantic where it gives -std=/-pedantic*) and skip tests
whose dg-options use a target selector a standard other than C99, or options
cereal lacks (anything but -W*, -D, -U, -I, -std, -pedantic*).
Compares gcc vs cereal -fsyntax-only verdicts and diagnostic headers."""
import sys, os, re, json, subprocess, glob, multiprocessing, shlex
S = os.path.dirname(os.path.abspath(__file__))
sys.argv, ARGS = sys.argv[:3], sys.argv
exec(open(os.path.join(S,"..","corpus.py")).read().split("\ndef run(")[0])
sys.argv = ARGS
HDR = re.compile(r'^(?:\S[^:]*):(\d+):(\d+): (fatal error|error|warning): (.*)$')
def hdrs(txt, kinds=("error","warning","fatal error")):
    out=[]
    for l in txt.splitlines():
        m=HDR.match(l)
        if m and m.group(3) in kinds: out.append((int(m.group(1)),int(m.group(2)),m.group(3),m.group(4)))
    return out
def work(job):
    cer, f, flags, cwd, extra, std = job
    base=["-fsyntax-only"]+std+extra+flags
    env=dict(os.environ,LC_ALL="C")
    try:
        g=subprocess.run([os.environ.get("CEREAL_GCC","gcc")]+base+[f],cwd=cwd,capture_output=True,timeout=30,env=env)
        c=subprocess.run([cer]+base+[f],cwd=cwd,capture_output=True,timeout=30,env=env)
    except subprocess.TimeoutExpired:
        return dict(f=f,to=1)
    return dict(f=f,cwd=cwd,flags=flags,grc=g.returncode,crc=c.returncode,
        g=hdrs(g.stderr.decode(errors="replace")),c=hdrs(c.stderr.decode(errors="replace")),
        cerr=c.stderr.decode(errors="replace")[-300:] if c.returncode not in (0,1) else "")
def dgjobs(extra):
    root=os.environ.get("CEREAL_GCCTS",os.path.expanduser("~/gccts"))+"/gcc/testsuite/"+os.environ.get("CEREAL_DGDIR","gcc.dg")
    jobs=[]
    dgo=os.environ.get("CEREAL_DGOPTS")=="1"
    for f in sorted(glob.glob(root+"/*.c")):
        std=["-std=c99","-pedantic"]
        if dgo:
            src=open(f,errors="replace").read()
            m=re.search(r'dg-options\s+"([^"]*)"\s*(\{[^}]*\})?\s*\}',src)
            if m:
                if m.group(2): continue
                o=[x for x in shlex.split(m.group(1)) if not x.startswith("-O") and x!="-g"]
                if any(x.startswith("-std=") and x not in ("-std=c99","-std=gnu99","-std=iso9899:1999") for x in o) or "-ansi" in o:
                    continue            # cereal is C99 only
                o2=[]; k=0
                while k<len(o):         # --param N=V is accepted and ignored
                    if o[k]=="--param": k+=2; o2.append("--param"); continue
                    o2.append(o[k]); k+=1
                if any(x == "-W" or (x!="--param" and x!="-w" and not x.startswith(("-W","-D","-U","-I","-std=","-pedantic","-fdump-","-fcompare-debug","-ftrack-macro-expansion=","--param="))) for x in o2):
                    continue            # options cereal does not take
                std=o if any(x.startswith("-std=") for x in o) else ["-std=c99"]+o
                if not any(x.startswith("-pedantic") for x in std): std=std+["-pedantic"]
        jobs.append((f,["-I"+root]+["-I"+root+"/.."],root,std))
    return jobs
if __name__=="__main__":
    cer,st,out=sys.argv[1:4]; extra=ARGS[4:]
    if st=="corpus":
        jobs=[(f,fl,cw,["-std=c99","-pedantic"]) for n,f,fl,cw in units(os.environ.get("CEREAL_CORPUS",os.path.expanduser("~/corpus")))]
    else: jobs=dgjobs(extra)
    jobs=[(cer,f,fl,cw,extra,std) for f,fl,cw,std in jobs]
    with multiprocessing.Pool(8) as p: res=p.map(work,jobs,chunksize=4)
    json.dump(res,open(out,"w"))
    n=len(res); rv=[r for r in res if not r.get("to") and r["grc"]==0 and r["crc"]!=0]
    ai=[r for r in res if not r.get("to") and r["grc"]!=0 and r["crc"]==0]
    cr=[r for r in res if not r.get("to") and r["crc"] not in(0,1)]
    both=[r for r in res if not r.get("to") and r["grc"]!=0 and r["crc"]!=0]
    same=[r for r in both if r["g"]==r["c"]]
    print(f"n={n} rejects-valid={len(rv)} accepts-invalid={len(ai)} crash={len(cr)} timeouts={sum(1 for r in res if r.get('to'))} bothfail={len(both)} identical-hdrs={len(same)}")
    ge=sum(len([x for x in r["g"] if x[2]!="warning"]) for r in both)
    gm=sum(sum(1 for x in r["g"] if x[2]!="warning" and x in r["c"]) for r in both)
    print(f"both-reject files: gcc errors {ge}, reproduced exactly (line:col:text) {gm}")
