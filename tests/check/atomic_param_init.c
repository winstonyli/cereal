// flags: -std=c11 -pedantic-errors
void fc0 (int _Atomic);
void fc0 (int);
void fc1 (int);
void fc1 (x) _Atomic int x; { }
void fc2 (x) _Atomic int x; { }
void fc2 (int);
void fc3 (x) _Atomic short x; { }
void fc3 (_Atomic int);
_Atomic char si0[] = "";
_Atomic __WCHAR_TYPE__ si1[] = L"";
