/* EXPECT:
1 2 3 4 5 6 0x0 9 abcdef
: 10, 20
1.000000 2.000000 3.000000 {4.000000,5.000000} -0.000000(80000000h) 99.000000
: -1.000000
*/

#include <stdio.h>
#include <inttypes.h>

struct l { short s[10]; };
struct x { long p; };
struct ll { long a, b; };

struct ll f(int a, int b, struct l c, int d, int e, int f, void *g, struct x x, char *s) {
   printf("%d %d %d %d ", a, b, c.s[0], d);
   printf("%d %d 0x%"PRIxPTR" %ld %s\n", e, f, (intptr_t)g, x.p, s);
   return (struct ll){10,20
   };
}

struct f2 { float f[2]; };
union fi { float f; int i; };

struct hs { float x[10]; };

struct hs g(float a, float b, float c, struct f2 d, union fi e, double z) {
   printf("%f %f %f {%f,%f} ", a, b, c, d.f[0], d.f[1]);
   printf("%f(%xh) %f\n", e.f, e.i, z);
   return (struct hs){-1.0f};
}

int main() {
   struct ll a = f(1,2,(struct l){3},4,5,6, NULL, (struct x){9}, "abcdef");
   printf(": %ld, %ld\n", a.a, a.b);
   struct hs h = g(1.0f, 2.0, 3.0f, (struct f2){4.0,5.0}, (union fi){-0.0f}, 99.f);
   printf(": %f\n", h.x[0]);
}

