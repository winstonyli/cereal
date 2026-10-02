// flags: -std=gnu99 -Wall
typedef __SIZE_TYPE__ size_t;
void exit ();
void* memcpy ();
void t (void *p, const void *q, size_t n)
{
  exit ();
  exit (1, 2);
  exit ("");
  memcpy (q, p, n);
  memcpy (p);
}
