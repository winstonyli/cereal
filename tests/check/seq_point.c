// flags: -Wsequence-point
struct S { int f; };
int f(int, ...);
void t(int a, int b, int c, int n, int *p, int *arr, struct S s)
{
    a = c++ + c++;
    a = (c++ + (c = c));
    a = ((c = c) + c++);
    a = (c++ + (c += 1));
    a = c++ + (a = c);
    a = (c = c) + c;
    if (c++ + (c = c)) a = 1;
    if ((c = c) + c++) a = 1;
    if (c++ - (c = 1)) a = 1;
    while ((c = 1) + c++) a = 1;
    a = (c++ + c++) != 0;
    arr[a++] = a;
    *p++ = *p;
    s.f = s.f++;
    a = f(c++, c++);
    a = f(c++, c);
    c = c++;
    a = (a++, a++);
    a = n ? c++ : c--;
    a = c++ && c++;
    a = (1 || c++) + c++;
    a = (c++, 1) + c;
    a = c++ + 1;
    a = a++ + b++;
    a = (c++ + (c = c)) + 1;
    for (a = 0; c++ + c++; a++) ;
    switch (c++ + c++) { default: break; }
    return;
}
