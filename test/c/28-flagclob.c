/* EXPECT:
f(6) = 6, f(-6) = -1006
g(-4,2) = 98
*/

// regression test for miscompilations where isel misses flag clobbers

int abs(int);
int f(int x) {
   int fwd = x >= 0;
   int y = abs(x);
   if (fwd) return y;
   return -1000 - y;
}
int g(int a, int b) {
   int t = a < b;
   int q = a / b;
   if (t) return q + 100;
   return -1;
}

#include <stdio.h>
int main() {
   printf("f(6) = %d, f(-6) = %d\n", f(6), f(-6));
   printf("g(-4,2) = %d\n", g(-4, 2));
}
