# T0 of docs/NARROW_INT_PLAN.md: the S8 goals with the view-change fact in the hypotheses (not a goal disjunction).
MIN, MAX, P = -2**31, 2**31-1, 2**64
def vc(v, u):   # u is the unsigned 64-bit view of signed v
    return f'(or (and (<= 0 {v}) (= {u} {v})) (and (< {v} 0) (= {u} (+ {v} {P}))))'
def q(drop, obl):
    h = [f'(<= {MIN} {v}) (<= {v} {MAX})' for v in 'rcij'] + ['(<= 0 len)', f'(< len {P})', '(<= 0 r)', '(<= 0 c)',
         '(<= 0 i)', '(< i r)', '(<= 0 j)', '(< j c)']
    if 'rcl' not in drop: h += [vc('(* r c)', 'pu'), '(<= pu len)']       # spec r*c <= $len: signed product, unsigned compare
    if 'rcmax' not in drop: h += [f'(<= (* r c) {MAX})']
    x = '(+ (* i c) j)'
    inr = lambda e: f'(and (<= {MIN} {e}) (<= {e} {MAX}))'
    goal = {'wrap1': inr('(* i c)'), 'wrap2': inr(x), 'goal': '(< xu len)'}[obl]
    extra = vc(x, 'xu') if obl == 'goal' else 'true'
    return (f'(set-logic QF_NIA)\n(declare-const r Int)(declare-const c Int)(declare-const i Int)(declare-const j Int)'
            f'(declare-const len Int)(declare-const pu Int)(declare-const xu Int)\n(assert (and {" ".join(h)} {extra}))\n(assert (not {goal}))\n(check-sat)\n')
for name, drop in [('full', []), ('no_rcl', ['rcl']), ('no_rcmax', ['rcmax'])]:
    for obl in ['wrap1', 'wrap2', 'goal']:
        open(f'vc_{name}_{obl}.smt2', 'w').write(q(drop, obl))
