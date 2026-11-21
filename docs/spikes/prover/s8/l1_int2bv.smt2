(set-logic ALL)
(declare-const r Int)(declare-const c Int)(declare-const i Int)(declare-const j Int)(declare-const len Int)
(assert (and (<= 0 r 2147483647) (<= 0 c 2147483647) (<= 0 i 2147483647) (<= 0 j 2147483647) (<= 0 len 18446744073709551615)
  (<= (* r c) len) (<= (* r c) 2147483647) (<= 0 i) (< i r) (<= 0 j) (< j c)))
; goal in BV via int2bv of the exact Int index, sign-extended view
(define-fun idx () Int (+ (* i c) j))
(assert (not (bvult ((_ sign_extend 32) ((_ int2bv 32) idx)) ((_ int2bv 64) len))))
(check-sat)
