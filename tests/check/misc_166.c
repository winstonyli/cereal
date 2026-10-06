// flags: -Wmisleading-indentation
void test04(int flag, int num) {
#define bar() \
  {		\
    if (flag)	\
      num = 0;	\
      num = 1;	\
  }
  bar();
}
