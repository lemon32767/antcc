/* coment */

#if 1+1 < (-2*2)
wawa
#elif 9<<1
#define wow 3
#else
boop
#endif
int glob [ wow+wow];

int add (int x, int y) {
    return x + y + *glob;
}

int abs(int x){
    return (x ^ x >> 31) - (x >> 31);
}

int popcnt(unsigned x) {
    int n = 0;
    while (x) x >>= 1, n++;
    return n + sizeof &"á"[0];
}

struct f {
    char x[(1<<12)-1];
};

int diff(struct f *x, struct f *y)
{
    x += 3;
    --x;
    return x - y;
}

_Bool narrow(int x)
{
    return (float) x;
}

#define xx 2

int waaa[xx == 0 ?  'Z'
        : xx == 1? 'O'
        : xx == 2? 'T'
        : '?'];

extern int printf(char *, ...);
int main() {
    unsigned char x = 255;
    int k = x += 1;
    1+waaa;
    return abs(k);
}

//
