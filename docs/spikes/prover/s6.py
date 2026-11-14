# S6 throwaway: general VC generator over `cereal --dump-cfg` text for one function with cut points at
# back-edge targets, a1 memory (one array per pointer parameter), Houdini invariant candidates from templates
# fixed before the run: T1 bound weakening (v <= b from the loop condition v < b), T2 postcondition forall with its
# bound replaced by the loop variable, T3 (env T3=1) x + y == c for two assigned locals and a scalar parameter.
# Obligations: bounds on every access, loop invariant on entry and preserved, postcondition at ret.
# Values are 64-bit; chars are sign-extended on load and truncated on store (32-bit wrap is not modelled).
import re, subprocess, sys, time, os, tempfile, itertools

Z3 = os.path.expanduser('~/tools/z3/z3')
BV = '(_ BitVec 64)'
ARR = '(Array (_ BitVec 64) (_ BitVec 8))'
RL = 400000
Z = '(_ bv0 64)'


def parse(path):
    blocks, order, cur = {}, [], None
    for ln in open(path):
        ln = ln.rstrip('\n')
        m = re.match(r'^(B\d+):$', ln)
        if m:
            cur = m.group(1); blocks[cur] = {'nodes': [], 'term': None}; order.append(cur); continue
        if cur is None:
            continue
        m = re.match(r'^  #(\d+) (\w+) ?(.*)$', ln)
        if m:
            blocks[cur]['nodes'].append((int(m.group(1)), m.group(2), m.group(3).split())); continue
        m = re.match(r'^  (jmp|br|ret) ?(.*)$', ln)
        if m:
            blocks[cur]['term'] = (m.group(1), m.group(2).split())
    return blocks, order


def name_of(tok): return tok.split('@')[0]


class Ctx:   # what spec lambdas see
    pass


SPECS = {
    'copy': dict(ptrs={'dst': 'D', 'src': 'S'}, scal=['n'], req=['n<=szD', 'n<=szS'],
                 posts=[('forall', 'n', lambda c, k: f'(= {c.sel("D", k)} {c.sel("S", k)})')]),
    'blen': dict(ptrs={'s': 'S'}, scal=['n'], req=['n<=szS'],
                 posts=[('forall', 'ret', lambda c, k: f'(not (= {c.sel("S", k)} #x00))'),
                        ('plain', lambda c: f'(bvule {c.ret} {c.p["n"]})'),
                        ('plain', lambda c: f'(=> (bvult {c.ret} {c.p["n"]}) (= {c.sel("S", c.ret)} #x00))')]),
    'rev': dict(ptrs={'d': 'D', 's': 'S'}, scal=['n'], req=['n<=szD', 'n<=szS'],
                posts=[('forall', 'n', lambda c, k: f'(= {c.sel("D", k)} {c.sel("S", f"(bvsub (bvsub {c.p["n"]} {k}) (_ bv1 64))")})')]),
    'fillnz': dict(ptrs={'d': 'D', 's': 'S'}, scal=['n'], req=['n<=szD', 'n<=szS'],
                   posts=[('forall', 'n', lambda c, k: f'(not (= {c.sel("D", k)} #x00))')]),
}


class VC:
    def __init__(self, blocks, spec):
        self.B, self.spec = blocks, spec
        self.decls, self.cnt = {}, 0
        self.allocs = sorted(set(spec['ptrs'].values()))
        self.sz = {a: self.fresh(BV, 'sz' + a) for a in self.allocs}
        self.p = {n: self.fresh(BV, n) for n in spec['scal']}
        self.arr0 = {a: self.fresh(ARR, 'm' + a) for a in self.allocs}
        drop = os.environ.get('DROP')
        reqs = [r for i, r in enumerate(spec['req']) if str(i) != drop]
        self.facts = [self.req_f(r) for r in reqs]
        self.edges, self.obls = [], []   # edges: (src, src_state, path, tgt, tgt_state); obls: (src, src_state, path, goal, desc)
        self.heads = self.find_heads()
        self.assigned, self.written = self.scan()

    def fresh(self, sort, hint):
        self.cnt += 1; nm = f'{hint}_{self.cnt}'; self.decls[nm] = sort; return nm

    def req_f(self, r):
        a, b = r.split('<=')
        v = lambda x: self.p[x] if x in self.p else self.sz[x[2:]]
        return f'(bvule {v(a)} {v(b)})'

    def find_heads(self):
        heads, color = set(), {}
        def succ(b):
            t = self.B[b]['term']
            if t is None or t[0] == 'ret': return []
            return [x for x in t[1] if re.fullmatch(r'B\d+', x)]
        def dfs(b):
            color[b] = 1
            for s in succ(b):
                if color.get(s) == 1: heads.add(s)
                elif s not in color: dfs(s)
            color[b] = 2
        dfs('B0')
        return heads

    def scan(self):
        assigned, written = set(), set()
        for b, blk in self.B.items():
            ops = {nid: (op, t) for nid, op, t in blk['nodes']}
            for nid, op, t in blk['nodes']:
                if op != 'store': continue
                tgt = int(t[0][1:]); o, tt = ops[tgt]
                if o == 'sym': assigned.add(name_of(tt[0]))
                elif o == 'deref':
                    pp = ops[int(tt[0][1:])]               # ptradd *1 #p #i
                    ld = ops[int(pp[1][1][1:])]            # load of sym
                    sy = ops[int(ld[1][0][1:])]
                    written.add(self.spec['ptrs'][name_of(sy[1][0])])
        return assigned, written

    # ---- states ----
    def init_state(self):
        env = {n: ('bv', v) for n, v in self.p.items()}
        for pn, a in self.spec['ptrs'].items(): env[pn] = ('ptr', a, Z)
        return {'env': env, 'mem': dict(self.arr0)}

    def havoc(self, st):
        env = dict(st['env'])
        for n in self.assigned: env[n] = ('bv', self.fresh(BV, n))
        mem = dict(st['mem'])
        for a in self.written: mem[a] = self.fresh(ARR, 'm' + a)
        return {'env': env, 'mem': mem}

    def ctx(self, st, ret=None):
        c = Ctx(); c.p = {n: self.p[n] for n in self.p}; c.ret = ret
        c.sel = lambda a, k: f'(select {st["mem"][a]} {k})'
        return c

    # ---- symbolic execution ----
    def run(self, bid, st, path, src, src_state):
        st = {'env': dict(st['env']), 'mem': dict(st['mem'])}
        blk = self.B[bid]; val = {}
        for nid, op, t in blk['nodes']:
            g = lambda i: val[int(t[i][1:])]
            if op == 'local':
                if t and t[-1].startswith('#') or (len(t) > 1 and t[1].startswith('#')):
                    k = [x for x in t[1:] if x.startswith('#')]
                    if k: st['env'][name_of(t[0])] = val[int(k[0][1:])]
                val[nid] = None
            elif op in ('exprstmt', 'post'): val[nid] = None
            elif op == 'sym': val[nid] = ('ref', name_of(t[0]))
            elif op == 'const': val[nid] = ('bv', f'(_ bv{t[0]} 64)')
            elif op == 'cast': val[nid] = g(0)
            elif op == 'ptradd': p, i = g(1), g(2); val[nid] = ('ptr', p[1], f'(bvadd {p[2]} {i[1]})')
            elif op == 'deref': p = g(0); val[nid] = ('lv', p[1], p[2])
            elif op == 'load':
                r = g(0)
                if r[0] == 'ref': val[nid] = st['env'][r[1]]
                else:
                    _, a, off = r
                    self.obls.append((src, src_state, list(path), f'(bvult {off} {self.sz[a]})', f'read {a}[..]@{bid}'))
                    ch = 'char' in ' '.join(t[1:]) and '*' not in t[1:]
                    s = f'(select {st["mem"][a]} {off})'
                    val[nid] = ('bv', f'((_ sign_extend 56) {s})' if ch else s)
            elif op == 'binop':
                a, b = g(1), g(2); f = {'+': 'bvadd', '-': 'bvsub'}[t[0]]
                val[nid] = ('bv', f'({f} {a[1]} {b[1]})')
            elif op == 'cmp':
                a, b = g(1), g(2); x, y = a[1], b[1]
                e = {'<': f'(bvult {x} {y})', '<=': f'(bvule {x} {y})', '>': f'(bvugt {x} {y})', '>=': f'(bvuge {x} {y})',
                     '==': f'(= {x} {y})', '!=': f'(not (= {x} {y}))'}[t[0]]
                val[nid] = ('bool', e)
            elif op == 'store':
                r, v = g(0), g(1)
                if r[0] == 'ref': st['env'][r[1]] = v
                else:
                    _, a, off = r
                    self.obls.append((src, src_state, list(path), f'(bvult {off} {self.sz[a]})', f'write {a}[..]@{bid}'))
                    st['mem'][a] = f'(store {st["mem"][a]} {off} ((_ extract 7 0) {v[1]}))'
                val[nid] = v
            else: raise SystemExit(f'unsupported op {op}')
        kind, args = blk['term']
        if kind == 'jmp': self.go(args[0], st, path, src, src_state)
        elif kind == 'br':
            c = val[int(args[0][1:])][1]
            self.go(args[1], st, path + [c], src, src_state)
            self.go(args[2], st, path + [f'(not {c})'], src, src_state)
        else:
            ret = val[int(args[0][1:])][1] if args else None
            self.obls.append((src, src_state, list(path), ('post', st, ret), f'post@{bid}'))

    def go(self, b, st, path, src, src_state):
        if b in self.heads: self.edges.append((src, src_state, list(path), b, st))
        else: self.run(b, st, path, src, src_state)

    def build(self):
        s0 = self.init_state()
        self.run('B0', s0, [], None, s0)
        self.hstate = {}
        for h in sorted(self.heads):
            hs = self.havoc(s0); self.hstate[h] = hs
            self.run(h, hs, [], h, hs)

    # ---- candidates ----
    def head_info(self, h):
        blk = self.B[h]; ops = {nid: (op, t) for nid, op, t in blk['nodes']}
        for nid, op, t in blk['nodes']:
            if op == 'cmp' and t[0] in ('<',):
                def var(i):
                    o, tt = ops[int(t[i][1:])]
                    if o != 'load': return None
                    so, st_ = ops[int(tt[0][1:])]
                    return name_of(st_[0]) if so == 'sym' else None
                return var(1), var(2)
        return None, None

    def cands(self, h):
        v, b = self.head_info(h); out = {}
        cv = lambda name: (lambda st: st['env'][name][1])
        if v and b: out[f'{v}<={b}'] = lambda st, v=v, b=b: f'(bvule {st["env"][v][1]} {self.val(st, b)})'
        for i, po in enumerate(self.spec['posts']):
            if po[0] != 'forall' or not v: continue
            def f(st, po=po, v=v):
                k = 'kq'
                c = self.ctx(st)
                return f'(forall (({k} {BV})) (=> (bvult {k} {st["env"][v][1]}) {po[2](c, k)}))'
            out[f'forall k<{v} (post{i})'] = f
        if os.environ.get('T3'):
            loc = sorted(self.assigned)
            for x, y in itertools.combinations(loc, 2):
                for c in self.p:
                    out[f'{x}+{y}=={c}'] = lambda st, x=x, y=y, c=c: f'(= (bvadd {st["env"][x][1]} {st["env"][y][1]}) {self.p[c]})'
        return out

    def val(self, st, name): return st['env'][name][1] if name in st['env'] else self.p[name]

    # ---- discharge ----
    def holds(self, hyps, goal, quant_ok=True):
        tiers = []
        if 'forall' not in goal: tiers.append([h for h in self.facts + hyps if 'forall' not in h])
        if 'forall' in goal or any('forall' in h for h in hyps):
            # quantifier-free tier: skolemize the goal's foralls, replace each hypothesis forall by its instances at the
            # skolem constants and every 64-bit constant (a consequence of it, so sound)
            ks = [self.fresh(BV, 'ks')]
            consts = ks + [k for k, s in self.decls.items() if s == BV and not k.startswith('ks')]
            g2 = subq(goal, lambda body: re.sub(r'\bkq\b', ks[0], body))
            h2 = [subq(h, lambda body: '(and ' + ' '.join(re.sub(r'\bkq\b', c, body) for c in consts) + ')') for h in self.facts + hyps]
            tiers.append(('qf', h2, g2))
        tiers.append(('full', self.facts + hyps, goal))
        for tier in tiers:
            hs, gl = (tier[1], tier[2]) if isinstance(tier, tuple) else (tier, goal)
            r = z3run(self.decls, hs, gl, self.cap)
            self.nq += 1
            if r == 'unsat': return True
        return False

    def src_hyps(self, src, src_state, live):
        if src is None: return []
        return [c(src_state) for n, c in self.cands(src).items() if n in live[src]]

    def post_goal(self, st, ret):
        c = self.ctx(st, ret); parts = []
        for po in self.spec['posts']:
            if po[0] == 'plain': parts.append(po[1](c))
            else:
                bound = ret if po[1] == 'ret' else self.p[po[1]]
                parts.append(f'(forall ((kq {BV})) (=> (bvult kq {bound}) {po[2](c, "kq")}))')
        return '(and ' + ' '.join(parts) + ')'


LASTMODEL = None
FA = '(forall ((kq (_ BitVec 64))) '


def subq(s, fn):
    # replace each (forall ((kq T)) BODY) in s by fn(BODY)
    out, i = '', 0
    while True:
        j = s.find(FA, i)
        if j < 0: return out + s[i:]
        k, depth = j + len(FA), 1
        while depth:
            depth += {'(': 1, ')': -1}.get(s[k], 0); k += 1
        # k is one past the closing paren of the forall; its body ends one before
        body = s[j + len(FA):k - 1]
        out += s[i:j] + fn(body); i = k


def z3run(decls, hyps, goal, cap=False):
    global LASTMODEL
    lines = [f'(set-option :rlimit {RL})', '(set-logic ALL)']
    lines += [f'(declare-const {n} {s})' for n, s in decls.items()]
    lines += [f'(assert {h})' for h in hyps]
    if cap:
        lines += [f'(assert (bvult {k} (_ bv64 64)))' for k, s in decls.items() if s == BV]
    lines += [f'(assert (not {goal}))', '(check-sat)']
    bvs = [k for k, s in decls.items() if s == BV]
    lines.append('(get-value (' + ' '.join(bvs) + '))')
    with tempfile.NamedTemporaryFile('w', suffix='.smt2', delete=False) as f:
        f.write('\n'.join(lines)); p = f.name
    out = subprocess.run([Z3, '-smt2', p], capture_output=True, text=True, stdin=subprocess.DEVNULL, timeout=120).stdout.strip().split('\n')
    if os.environ.get("KEEP"): open("/tmp/last_" + out[0] + ".smt2", "w").write(open(p).read())
    os.unlink(p)
    LASTMODEL = None
    if out[0] == 'sat':
        LASTMODEL = {m[0]: int(m[1][2:], 16) for m in re.findall(r'\((\w+) (#x[0-9a-f]+)\)', ' '.join(out[1:]))}
    return out[0]


def main(dump, fname):
    spec = SPECS[fname]; blocks, _ = parse(dump)
    V = VC(blocks, spec); V.nq = 0; V.cap = False; V.build(); t0 = time.time()
    live = {h: list(V.cands(h)) for h in V.heads}
    # Houdini over edges
    rounds = 0
    while True:
        rounds += 1; changed = False
        for src, ss, path, tgt, st in V.edges:
            hyps = V.src_hyps(src, ss, live) + path
            keep = []
            for n in live[tgt]:
                if V.holds(hyps, V.cands(tgt)[n](st)): keep.append(n)
                else:
                    changed = True
                    if os.environ.get("V"): print("   drop", n, "edge", src, "->", tgt, "path", path)
            live[tgt] = keep
        if not changed: break
    print(f'{fname}: invariants {live} rounds {rounds}')
    bad = []
    for src, ss, path, goal, desc in V.obls:
        hyps = V.src_hyps(src, ss, live) + path
        if isinstance(goal, tuple): goal = V.post_goal(goal[1], goal[2])
        ok = V.holds(hyps, goal)
        if not ok:
            bad.append(desc); V.cap = True; V.holds(hyps, goal); V.cap = False
            if LASTMODEL is not None and os.environ.get('DRIVER') and 'DRV' not in globals():
                globals()['DRV'] = dict(LASTMODEL)
    # edge obligations were Houdini-pruned; the surviving candidates are the invariant, edges proved by construction
    print(f'{fname}: {"PROVED" if not bad else "FAILED " + str(bad)} queries {V.nq} {time.time()-t0:.2f}s')
    if bad and os.environ.get('DRIVER') and 'DRV' in globals():
        M = DRV
        sizes = {a: M[V.sz[a]] for a in V.allocs}; scal = {n: M[V.p[n]] for n in V.p}
        print('   model sizes', sizes, 'scalars', scal)
        for fill in (0, 1):
            args = []; pre = []
            for pn, a in spec['ptrs'].items():
                pre.append(f'char *{pn} = malloc({sizes[a]}); {{ unsigned long k; for (k = 0; k < {sizes[a]}; k++) {pn}[k] = {fill}; }}')
                args.append(pn)
            args += [str(scal[n]) for n in spec['scal']]
            drv = f'\nvoid *malloc(unsigned long);\nint drv(void) {{ {" ".join(pre)} {fname}({", ".join(args)}); return 0; }}\n'
            out = os.environ['DRIVER'] + f'_{fill}.c'
            open(out, 'w').write(open(os.environ['SRC']).read() + drv)
            print('   driver', out)


main(sys.argv[1], sys.argv[2])
