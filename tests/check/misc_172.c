// flags: -Wmisleading-indentation
}

void test05(int flag, int num) {
#define baz() (num = 1)
#define bar() \
  {		\
    if (flag)	\
      num = 0;	\
      baz();	\
  }
#define wrapper bar
  wrapper();
#undef bar
}
}
