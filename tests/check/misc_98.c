// flags: -std=gnu99
asm const ("");    /* { dg-error {expected '\(' before 'const'} } */
asm volatile (""); /* { dg-error {expected '\(' before 'volatile'} } */
asm restrict (""); /* { dg-error {expected '\(' before 'restrict'} } */
asm inline ("");   /* { dg-error {expected '\(' before 'inline'} } */
asm goto ("");     /* { dg-error {expected '\(' before 'goto'} } */
void
f (void)
{
  asm volatile ("");
  asm const ("");
  asm restrict ("");
  asm __const__ ("" ::: "memory");
  asm volatile goto ("" :::: l);
l:;
}
