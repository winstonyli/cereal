// flags: -Wall
/* too few arguments to a no-prototype built-in: no format check follows */
int vfscanf ();
int vprintf ();
void f (void)
{
  vfscanf (1, "");
  vprintf ("");
}
