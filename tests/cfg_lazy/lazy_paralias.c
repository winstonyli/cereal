/* Two pointer parameters subtracted: the sweep must make them share an object (a retry forces the later parameter's alias), not stop at "different objects" on most calls. */
long pa_run(char *a, char *b)
{
    return b - a;
}
