// flags: -Wall -Wextra
int z;
long a = (long)&z;
int *p4 = (int*)(long)&z;
int *p5 = (int*)(char*)&z;
int *p6 = (int*)a;
int *p7 = (int*)0x1000;
long p8 = (long)(int*)&z;
