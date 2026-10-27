/* A pointer cell drawn to share its own object: sp_run reads an int through x->p, which can be the cell's own bytes (Lua's savedpc into the CallInfo that holds it). The sweep must redraw the cell, not stop on most calls. */
typedef struct Node { int *p; long n; } Node;
long sp_run(Node *x)
{
    return *x->p;
}
