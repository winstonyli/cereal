void f(int k) {
  [[gnu::assume (k >= 92)]] [[gnu::assume (k >= 92)]]
  ;
  [[gnu::assume (k >= 92)]] ;
  [[gnu::assume (k >= 92)]] [[gnu::assume (k >= 92)]] ;
}
