import json,sys,re
lv=sys.argv[1]
g=json.load(open("/tmp/fz_gcc_%s.json"%lv)); c=json.load(open("/tmp/fz_cereal_%s.json"%lv))
def norm(v):
    return [l for l in v if "warning" in l or "note" in l or "error" in l]
bad=0
for k in g:
    gv=norm(g[k]); cv=norm(c[k])
    gv=[re.sub(r"\[-W[a-z=-]+\]","",x) for x in gv]; cv=[re.sub(r"\[-W[a-z=-]+\]","",x) for x in cv]
    if any("error" in x for x in gv): continue
    if gv!=cv:
        bad+=1
        if bad<=int(sys.argv[2]):
            print(k); print("  gcc:", " ¦ ".join(gv)); print("  cer:", " ¦ ".join(cv))
print("diffs",bad)
