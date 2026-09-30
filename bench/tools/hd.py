import json,sys,re,collections
r=json.load(open(sys.argv[1])); r=[x for x in r if not x.get("to")]
def norm(m): return re.sub(r"'[^']*'","'X'",re.sub(r"\d+","N",m))[:80]
anyd=[x for x in r if x["g"] or x["c"]]
same=[x for x in anyd if x["g"]==x["c"]]
both=[x for x in r if x["grc"]!=0 and x["crc"]!=0]
bs=[x for x in both if x["g"]==x["c"]]
print("files with any diag: %d identical %d (%.1f%%)"%(len(anyd),len(same),100*len(same)/max(1,len(anyd))))
print("both fail: %d identical %d (%.1f%%)"%(len(both),len(bs),100*len(bs)/max(1,len(both))))
# verdict-agreeing: both-fail same error set
def errs(h): return [e for e in h if e[2]!="warning"]
bes=[x for x in both if errs(x["g"])==errs(x["c"])]
print("both fail, identical error headers: %d (%.1f%%)"%(len(bes),100*len(bes)/max(1,len(both))))
miss=collections.Counter(); extra=collections.Counter()
for x in anyd:
    g=collections.Counter((h[0],h[1],h[3]) for h in x["g"]); c=collections.Counter((h[0],h[1],h[3]) for h in x["c"])
    for k in (g-c): miss[norm(k[2])]+=1
    for k in (c-g): extra[norm(k[2])]+=1
print("MISSING in cereal (gcc emits):"); [print("  %4d %s"%(v,k)) for k,v in miss.most_common(int(sys.argv[2]) if len(sys.argv)>2 else 15)]
print("EXTRA in cereal:"); [print("  %4d %s"%(v,k)) for k,v in extra.most_common(int(sys.argv[2]) if len(sys.argv)>2 else 15)]
