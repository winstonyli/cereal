// flags: -Wall -Wextra
typedef int wchar_t;
char c1[3] = "abc";
char c2[2] = "abc";
signed char c3[] = "abc";
unsigned char c4[2] = "abc";
wchar_t w1[] = L"ab";
wchar_t w2[2] = L"abc";
char c5[] = L"ab";
wchar_t w3[] = "ab";
int i1[] = "ab";
short s1[] = "ab";
char c6[] = { "ab" };
char c7[] = { "ab", "cd" };
char c8[3] = ("ab");
char *p1 = { "ab" };
char *p2 = { { "ab" } };
char c10[2][4] = { "abcd", "ef" };
char c11[][3] = { "abc", "de" };
char *c12[] = { "a", "bc" };
struct S { char a[2]; char *p; } s = { "xy", "z" };
struct S s2 = { "xyz", "z" };
struct S s3 = { 'x', 'y', "z" };
char c13[5] = "ab" "cd";
char c14[] = "ab" L"cd";
int main(void) {
  char a[2] = "abc";
  char b[] = "abc";
  char c[] = { 'a', 'b' };
  wchar_t d[2] = L"ab";
  return 0;
}
