/* A missing ';' skips to an unmatched closing bracket, which then fails as a
 * statement of its own; a ')' or ']' cannot start a statement. */
int f(void)
{
    int x = 0;
    x = 1); 
    x = 2];
    );
    while (int i);
    return 1);
}
