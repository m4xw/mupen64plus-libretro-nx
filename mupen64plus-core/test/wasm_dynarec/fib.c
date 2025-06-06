int fib(int n) {
    volatile int a = 0;
    volatile int b = 1;
    for (volatile int i = 0; i < n; ++i) {
        volatile int t = a + b;
        a = b;
        b = t;
    }
    return a;
}
