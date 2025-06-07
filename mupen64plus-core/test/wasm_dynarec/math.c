__attribute__((noinline)) static int square(int x);

int sumsq(int n) {
    volatile int sum = 0;
    for (volatile int i = 1; i <= n; ++i) {
        sum += square(i);
    }
    return sum;
}

__attribute__((noinline)) static int square(int x) {
    return x * x;
}
