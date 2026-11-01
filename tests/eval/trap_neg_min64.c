// trap: signed integer overflow
int main(void) { long long a = -9223372036854775807LL - 1; return (int)-a; }
