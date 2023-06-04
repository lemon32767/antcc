/* coment */

#if 1+1 < (-2*'a')
wawa
#elif 9<<1
#define wow 3
#else
boop
#endif

#define NULL ((void *)0)
int glob [ wow+wow];

struct foo {
    int x, y, z;
};

struct v2f { double x, y; };
struct big { int x[10]; };

struct v2f add(struct v2f a, struct v2f b, struct big B, struct { char c; } small, void *prt) {
    struct v2f r;
    r.y = a.y + b.y;
    return r;
}

struct pair {
    long x, y;
};

struct pair pair(long x, long y) {
    struct pair p;
    p.x = x;
    p.y = y;
    return p;
}

struct quad {
    long x, y, z, w;
};

struct quad quad(long x, long y, long z, long w) {
    struct quad q;
    q.x = x, q.y = y, q.z = z, q.w = w;
    return q;
}

void silly(struct pair *p, struct quad *q)
{
    *p = pair(1,2);
    *q = quad(1,2,3,4);
}

void test2(struct big *b) {
    struct big s = *b;
    extern void h(int, struct big, float);
    s.x[5] += 2;
    h(0, s, 0);
}

struct f2 { float f,g; };
struct f2 f2test(struct f2 *r) {
    return *r;
}

void fill(char *p, int c, unsigned long n)
{
    int t;
    if (n) do t = *p++ = c; while (--n);
}

void zero(void *p, unsigned long n) { fill(p,0,n); }

enum ball  {
    neg = -1,
    Zero,
    Two = 2,
    Three,
    One = Zero + 1,
    X = 2147483647,
    Y,
    Z,
    W = ~0ul
};

main(t) {
    putc(t + 1, t + 2);
}


//
