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


int test0(struct foo *foo) { return foo->x ? foo->y : foo->z; }
int test1(int x, int y, int z) { return x && y || z; }
int test2(int x, int y, int z) { return x || y && z; }

extern void f();
int test3(int x, int y) {
    if (x < 0 && y < 0 && 1) return x - y;
    if ((x == 0 || x > 0) && y > 0) return y;
    if (f(), x == y ? x && 1 : 0 || y) return x;
    return x + y;
}

int test4(int c) {
    return c == 'a' || c == 'x' ? 1
         : (f(), c == 'b' || c == 'y') ? 2
         : c == 'c' || c == 'z' ? 3
         : 0;
}

int test5(int *p)
{
    return p ? *p : 0;
}

int test6(int x)
{
    return !!!!x;
}

float sqr(float x) { return x * x; }

int mula(int x, int y, int z) {
    if (x < 0)
        return -x * y + z;
    return x * y + z;
}

void *copy(char *d, char *s, int n) {
    while (n--)
        *d++ = *s++;
    return d;
}

int hmm(float x, int a, char *p, char *q) {
    return x > 1 ? a || p : p < q && a > 0;
}

//
