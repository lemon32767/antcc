/* coment */

int glob;

int add (int x, int y) {
    return x + y + glob;
}

int abs(int x){
    return (x ^ x >> 31) + ((unsigned)x >> 31);
}

int popcnt(unsigned x) {
    int n = 0;
    while (x) x >>= 1, n++;
    return n;
}

int foo(int x) {
    int work[10];
    work[x]=x;
    if (x == 0)
        return 1;
    else if (x == 1)
        return -1;
    else if (x < 0)
        return x;
    else
        return 0;
}

extern void printf(char *, ...);
int main() {
    char x;
    int k = x += 1;
    return abs(-33);
}

//
