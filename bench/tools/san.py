import sys,os,re,subprocess,multiprocessing,collections,glob
S=os.path.dirname(os.path.abspath(__file__))
sys.argv,ARGS=sys.argv[:3],sys.argv
exec(open(S+"/par.py").read().split('if __name__')[0])
sys.argv=ARGS
def w(job):
    cer,f,flags,cwd,extra=job
    env=dict(os.environ,LC_ALL="C",ASAN_OPTIONS="detect_leaks=%s"%os.environ.get("LEAKS","0"),UBSAN_OPTIONS="print_stacktrace=1")
    try:
        p=subprocess.run([cer,"-fsyntax-only","-std=c99","-pedantic"]+extra+flags+[f],cwd=cwd,capture_output=True,timeout=300,env=env)
    except subprocess.TimeoutExpired: return (f,"TIMEOUT")
    e=p.stderr.decode(errors="replace")
    m=re.search(r"(runtime error:.*|ERROR: AddressSanitizer.*|ERROR: LeakSanitizer.*)",e)
    if m:
        fr=re.findall(r"#\d+ \S+ in (\S+) (\S+)",e)[:4]
        return (f,m.group(1)[:150]+" @ "+" < ".join(a+":"+b.split("/")[-1] for a,b in fr))
    return None
if __name__=="__main__":
    cer,st=sys.argv[1:3]; extra=ARGS[3:]
    jobs=[(f,fl,cw) for n,f,fl,cw in units(os.environ.get("CEREAL_CORPUS",os.path.expanduser("~/corpus")))] if st=="corpus" else dgjobs([])
    if st=="tests":
        jobs=[(f,[],os.path.dirname(f)) for f in sorted(glob.glob(S+"/../../tests/check/*.c")+glob.glob(S+"/../../tests/parse/*.c"))]
    jobs=[(cer,j[0],j[1],j[2],extra) for j in jobs]
    with multiprocessing.Pool(6) as p: res=[r for r in p.map(w,jobs,chunksize=2) if r]
    g=collections.defaultdict(list)
    for f,m in res: g[m].append(f.split("/")[-1])
    for m,fs in sorted(g.items(),key=lambda kv:-len(kv[1])): print(len(fs),m,fs[:3])
    print("total",len(jobs),"findings",len(res))
