import sys
MIN, MAX = -2**31, 2**31-1
def q(name, drop, obl):
    h = {
     'rng':   [f'(<= {MIN} {v}) (<= {v} {MAX})' for v in 'rcij'],
     'len':   ['(<= 0 len)', '(< len 18446744073709551616)'],
     'rnn':   ['(<= 0 r)', '(<= 0 c)'],
     'rcl':   ['(<= (* r c) len)'],
     'rcmax': [f'(<= (* r c) {MAX})'],
     'loop':  ['(<= 0 i)', '(< i r)', '(<= 0 j)', '(< j c)'],
    }
    hs = ' '.join(x for k, v in h.items() if k not in drop for x in v)
    ic, icj = '(* i c)', '(+ (* i c) j)'
    inr = lambda e: f'(and (<= {MIN} {e}) (<= {e} {MAX}))'
    # unsigned 64-bit view of the sign-extended index x = i*c+j
    goal = {'wrap1': inr(ic), 'wrap2': inr(icj),
            'goal': f'(or (and (<= 0 {icj}) (< {icj} len)) (and (< {icj} 0) (< (+ {icj} 18446744073709551616) len)))'}[obl]
    return f'(set-logic QF_NIA)\n(declare-const r Int)(declare-const c Int)(declare-const i Int)(declare-const j Int)(declare-const len Int)\n(assert (and {hs}))\n(assert (not {goal}))\n(check-sat)\n'
for name, drop in [('full', []), ('no_rcl', ['rcl']), ('no_rcmax', ['rcmax']), ('no_rnn', ['rnn'])]:
    for obl in ['wrap1', 'wrap2', 'goal']:
        open(f'{name}_{obl}.smt2', 'w').write(q(name, drop, obl))
