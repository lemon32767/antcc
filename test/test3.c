/*
int test1(int a, int b, int c) {
    return a && b ? c - b ? c - b : a+b : 7;
}
*/

int t(unsigned short *p, short i) {
    return p[i];
}

#if 1
long test(long x) {
    return x + (long)"abc";
}
#endif

double ff(double x, double y)
{
    return x + y + .5;
}

long fma(long x, long y) {
return x + (y <<1) - 2147483648;}
