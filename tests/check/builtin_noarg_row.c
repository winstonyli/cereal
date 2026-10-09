int fegetround(void);
int signbit(double);
int main(void)
{
    return fegetround() + signbit(1.0);
}
