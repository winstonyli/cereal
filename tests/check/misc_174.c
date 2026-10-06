// flags: -Wbidi-chars=ucn,unpaired
void f(void)
{
  const char *s = "a ⁧ b\
    ";
  (void)s;
}
