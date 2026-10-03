// flags: -Wtype-limits
_Bool f(unsigned p){ return 0ULL > p; }
_Bool g(unsigned p){ return 0ULL <= p; }
_Bool h(unsigned p){ return 0U > p; }
_Bool i(unsigned p){ return 0 > p; }
_Bool j(unsigned long p){ return 0ULL > p; }
_Bool k(unsigned p){ return 0LL <= p; }
