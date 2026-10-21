// args: one two three
#include <stdio.h>

int main(int argc, char **argv)
{
    int i;
    for (i = 0; i < argc; i++)
        printf("%d:%s\n", i, i ? argv[i] : "prog");
    printf("%d\n", argv[argc] == 0);
    return argc;
}
