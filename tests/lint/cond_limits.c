/* cond.c limits: current behavior recorded before the BDD rewrite (S1 plan, Slice 0).
   Lines marked "gap" are answers neither solver gives yet.
   cereal-flags: -Wcond-constant */
/* arithmetic on an integer variable: an opaque atom keyed by its text (S1a), still decided when the text repeats */
#if VERSION % 2 == 0
#elif VERSION % 2 == 0 /* expect: cond-dead-branch */
#endif
#if FLAGS & 4
#elif FLAGS & 4 /* expect: cond-dead-branch */
#endif
/* variable-free arithmetic and ternary */
#if 1 + 1 == 2 /* expect: cond-constant */
#endif
#if (3 > 2 ? 5 : 6) == 5 /* expect: cond-constant */
#endif
#if defined(T1) ? 1 : 0
#elif defined(T1) /* expect: cond-dead-branch */
#endif
/* #elifdef / #elifndef are chain branches (S1b): both are dead */
#ifdef E1
#elifdef E1 /* expect: cond-dead-branch */
#endif
#ifndef E2
#elifndef E2 /* expect: cond-dead-branch */
#endif
/* a call is an opaque atom keyed by its text: the same call twice in a chain */
#if __has_include(<stdio.h>)
#elif __has_include(<stdio.h>) /* expect: cond-dead-branch */
#endif
/* 17 variables (the enumeration stopped at 16): the inner #if is dead */
#if defined(W1) && defined(W2) && defined(W3) && defined(W4) && defined(W5) && defined(W6) && defined(W7) && defined(W8) && defined(W9) && defined(W10) && defined(W11) && defined(W12) && defined(W13) && defined(W14) && defined(W15) && defined(W16) && defined(W17)
#  if !defined(W1) /* expect: cond-dead-branch */
#  endif
#endif
/* 70 nested (the enumeration stopped at 64 frames): the innermost #ifndef is dead */
#ifdef N0
#ifdef N1
#ifdef N2
#ifdef N3
#ifdef N4
#ifdef N5
#ifdef N6
#ifdef N7
#ifdef N8
#ifdef N9
#ifdef N10
#ifdef N11
#ifdef N12
#ifdef N13
#ifdef N14
#ifdef N15
#ifdef N16
#ifdef N17
#ifdef N18
#ifdef N19
#ifdef N20
#ifdef N21
#ifdef N22
#ifdef N23
#ifdef N24
#ifdef N25
#ifdef N26
#ifdef N27
#ifdef N28
#ifdef N29
#ifdef N30
#ifdef N31
#ifdef N32
#ifdef N33
#ifdef N34
#ifdef N35
#ifdef N36
#ifdef N37
#ifdef N38
#ifdef N39
#ifdef N40
#ifdef N41
#ifdef N42
#ifdef N43
#ifdef N44
#ifdef N45
#ifdef N46
#ifdef N47
#ifdef N48
#ifdef N49
#ifdef N50
#ifdef N51
#ifdef N52
#ifdef N53
#ifdef N54
#ifdef N55
#ifdef N56
#ifdef N57
#ifdef N58
#ifdef N59
#ifdef N60
#ifdef N61
#ifdef N62
#ifdef N63
#ifdef N64
#ifdef N65
#ifdef N66
#ifdef N67
#ifdef N68
#ifdef N69
#ifndef N0 /* expect: cond-dead-branch */
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
/* 34 branches (the enumeration stopped at 32 priors): the last #elif is dead */
#if defined(P0)
#elif defined(P1)
#elif defined(P2)
#elif defined(P3)
#elif defined(P4)
#elif defined(P5)
#elif defined(P6)
#elif defined(P7)
#elif defined(P8)
#elif defined(P9)
#elif defined(P10)
#elif defined(P11)
#elif defined(P12)
#elif defined(P13)
#elif defined(P14)
#elif defined(P15)
#elif defined(P16)
#elif defined(P17)
#elif defined(P18)
#elif defined(P19)
#elif defined(P20)
#elif defined(P21)
#elif defined(P22)
#elif defined(P23)
#elif defined(P24)
#elif defined(P25)
#elif defined(P26)
#elif defined(P27)
#elif defined(P28)
#elif defined(P29)
#elif defined(P30)
#elif defined(P31)
#elif defined(P32)
#elif defined(P33)
#elif defined(P0) /* expect: cond-dead-branch */
#endif
/* large assignment space (the enumeration stopped at 2^18): the inner #if is dead */
#if (A1 == 3 || A1 == 7 || A1 == 11) && (A2 == 3 || A2 == 7 || A2 == 11) && (A3 == 3 || A3 == 7 || A3 == 11) && (A4 == 3 || A4 == 7 || A4 == 11) && (A5 == 3 || A5 == 7 || A5 == 11) && (A6 == 3 || A6 == 7 || A6 == 11) && (A7 == 3 || A7 == 7 || A7 == 11) && (A8 == 3 || A8 == 7 || A8 == 11) && (A9 == 3 || A9 == 7 || A9 == 11)
#  if A1 == 4 /* expect: cond-dead-branch */
#  endif
#endif
/* wide domain (the enumeration stopped at 24 values): the last #elif is dead */
#if X == 10
#elif X == 20
#elif X == 30
#elif X == 40
#elif X == 50
#elif X == 60
#elif X == 70
#elif X == 80
#elif X == 90
#elif X == 100
#elif X == 10 /* expect: cond-dead-branch */
#endif
