/*
int test1(int a, int b, int c) {
    return a && b ? c - b ? c - b : a+b : 7;
}
*/

int t(unsigned short *p, short i) {
    return p[i];
}

int shcl(int a, int b) {
    return a << (b+1);
}

int zero() { return 0; }
float fzero() { return 0.0f; }

struct p { long x,y; };
struct p divsh(int a) {
    struct p p;
    p.x = a << (a % 5);
    p.y = a;
    return p;
}

struct bitset { unsigned long long u; };
void
bscopy(struct bitset dst[/*siz*/], const struct bitset src[/*siz*/], unsigned siz)
{
   for (; siz; --siz, ++dst, ++src) dst->u = src->u;
}


#if 0
long test(long x) {
    return x + (long)"abc";
}

double ff(double x, double y)
{
    return x + y + .5;
}

void testss() {
    extern struct { long x,y; } aa();
    long x = aa().x;
}

long fma(long x, long y) {
return x + (y <<1) - 2147483648;}
#endif
