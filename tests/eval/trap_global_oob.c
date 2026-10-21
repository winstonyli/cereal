// trap: out of bounds
int arr[3] = {1, 2, 3};

int main(void)
{
    volatile int i = 3;
    return arr[i];
}
