#!/usr/bin/env python3
"""par.py CEREAL SET OUT.json [extra flags...]   SET = corpus|dg
Compares gcc vs cereal -fsyntax-only verdicts and diagnostic headers."""
import sys, os, re, json, subprocess, glob, multiprocessing
S = os.path.dirname(os.path.abspath(__file__))
sys.argv, ARGS = sys.argv[:3], sys.argv
exec(open("/home/user/cereal/bench/corpus.py").read().split("\ndef run(")[0])
sys.argv = ARGS
HDR = re.compile(r'^(?:\S[^:]*):(\d+):(\d+): (fatal error|error|warning): (.*)$')
def hdrs(txt, kinds=("error","warning","fatal error")):
    out=[]
    for l in txt.splitlines():
        m=HDR.match(l)
        if m and m.group(3) in kinds: out.append((int(m.group(1)),int(m.group(2)),m.group(3),m.group(4)))
    return out
def work(job):
    cer, f, flags, cwd, extra = job
    base=["-fsyntax-only","-std=c99","-pedantic"]+extra+flags
    env=dict(os.environ,LC_ALL="C")
    try:
        g=subprocess.run(["gcc"]+base+[f],cwd=cwd,capture_output=True,timeout=30,env=env)
        c=subprocess.run([cer]+base+[f],cwd=cwd,capture_output=True,timeout=30,env=env)
    except subprocess.TimeoutExpired:
        return dict(f=f,to=1)
    return dict(f=f,cwd=cwd,flags=flags,grc=g.returncode,crc=c.returncode,
        g=hdrs(g.stderr.decode(errors="replace")),c=hdrs(c.stderr.decode(errors="replace")),
        cerr=c.stderr.decode(errors="replace")[-300:] if c.returncode not in (0,1) else "")
def dgjobs(extra):
    root=S+"/gccts/gcc/testsuite/gcc.dg"
    jobs=[]
    for f in sorted(glob.glob(root+"/*.c")):
        src=open(f,errors="replace").read(2000)
        if "dg-do run" in src or "dg-do compile" in src or True:
            jobs.append((f,["-I"+root]+["-I"+root+"/.."],root))
    return jobs
if __name__=="__main__":
    cer,st,out=sys.argv[1:4]; extra=ARGS[4:]
    if st=="corpus":
        jobs=[(f,fl,cw) for n,f,fl,cw in units(S+"/corpus")]
    else: jobs=dgjobs(extra)
    jobs=[(cer,f,fl,cw,extra) for f,fl,cw in jobs]
    with multiprocessing.Pool(8) as p: res=p.map(work,jobs,chunksize=4)
    json.dump(res,open(out,"w"))
    n=len(res); rv=[r for r in res if not r.get("to") and r["grc"]==0 and r["crc"]!=0]
    ai=[r for r in res if not r.get("to") and r["grc"]!=0 and r["crc"]==0]
    cr=[r for r in res if not r.get("to") and r["crc"] not in(0,1)]
    both=[r for r in res if not r.get("to") and r["grc"]!=0 and r["crc"]!=0]
    same=[r for r in both if r["g"]==r["c"]]
    print(f"n={n} rejects-valid={len(rv)} accepts-invalid={len(ai)} crash={len(cr)} timeouts={sum(1 for r in res if r.get('to'))} bothfail={len(both)} identical-hdrs={len(same)}")
