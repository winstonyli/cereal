/* A constructor and functions taking its result (--eval-state): the sweep builds the state once
 * and every call starts from it; gcc runs the constructor afresh per call. */
#include <stdlib.h>
#include <string.h>

typedef struct Node {
    int val;
    struct Node *next;
} Node;

typedef struct Ctx {
    int magic, count;
    char name[8];
    Node *head;
} Ctx;

Ctx *ctx_new(void)
{
    Ctx *c = malloc(sizeof *c);
    Node *n = malloc(sizeof *n);
    if (!c || !n)
        return 0;
    n->val = 41;
    n->next = 0;
    c->magic = 7;
    c->count = 0;
    strcpy(c->name, "ctx");
    c->head = n;
    return c;
}

/* only reachable with the built state: magic is 7 there */
int ctx_check(Ctx *c, int x)
{
    if (c->magic != 7)
        return -1;
    if (x == c->head->val)
        return 100;
    return x < 0 ? 1 : 2;
}

int ctx_bump(Ctx *c, int by)
{
    c->count += by;
    c->head->val += c->count;
    return c->head->val;
}

int ctx_name(const Ctx *c, char ch)
{
    int n = 0;
    while (n < 8 && c->name[n])
        n++;
    return c->name[0] == ch ? 1 : n;
}

/* a builder that takes a state: the sweep builds a second Ctx from the first (a chain) */
Ctx *ctx_child(Ctx *p)
{
    Ctx *c = malloc(sizeof *c);
    if (!c)
        return 0;
    *c = *p;
    c->magic = p->magic + 1;
    p->count++;
    return c;
}
