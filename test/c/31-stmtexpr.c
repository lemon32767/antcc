/* CFLAGS: -lm */
/* EXPECT:
ok
*/

#include <assert.h>
#include <complex.h>
#include <stdio.h>

static int f_ret(void) {
   return ({ return 5; }), 6;
}

static int f_static(void) {
   return ({ static int c; c++; });
}

static int add(int a, int b) { return a + b; }

static int vc;
static void vnote(void) { vc++; }

static void vfn(void) {
   return ({ vnote(); });
}

struct S { int x, y; };

int main(void) {
   int s, i, r;

   s = 0;
   r = ({ s = 3; });
   assert(r == 3 && s == 3);
   assert(({ 2 + 3 * 4; }) == 14);

   // with deferred for iter
   for (s = i = 0; i < 4; ({ i++; }))
      s += i;
   assert(s == 6 && i == 4);
   for (s = i = 0; i < 3; ({ int t = i * 10; s += t; i++; }))
      s += i;
   assert(s == 33);
   s = 0;
   for (int j = ({ s = 1; 2; }); j < 4; j++)
      s += j;
   assert(s == 6);

   // condexprrec/condjump
   s = 0;
   assert((0 && ({ s = 1; 1; })) == 0);
   assert((1 || ({ s = 2; 0; })) == 1);
   assert((0 ? ({ s = 4; 1; }) : 5) == 5);
   assert((1 ? 6 : ({ s = 8; 2; })) == 6);
   assert(s == 0);

   // unevaluated, sizeof
   s = i = 0;
   unsigned long z = sizeof(({ i++; 1; }));
   assert(z == sizeof(int) && i == 0);
   assert(sizeof(({ i++; })) == sizeof(int) && i == 0);

   // silly control flow
   s = 0;
   r = ({ int k; for (k = 0; k < 10; k++) { if (k == 2) continue; if (k == 5) break; s += k; } k; });
   assert(s == 8 && r == 5);

   // jumps out of stmtexpr
   s = 0;
   for (i = 0; i < 5; i++) {
      s += ({ if (i == 2) continue; i; });
      s += 100;
   }
   assert(s == 408);
   s = 0;
   for (i = 0; i < 10; i++)
      s += ({ if (i == 3) break; i; });
   assert(s == 3);

   // weird
   r = ({ int j = 0; if (0) { lab: j = 7; } if (j == 0) goto lab; j; });
   assert(r == 7);

   // gotos
   r = 0;
   r = ({ if (r) goto out1; 5; });
out1:
   assert(r == 5);

   r = 1;
   r = ({ if (r) goto out2; 5; });
   r = 99;
out2:
   assert(r == 1);

   r = 0;
   ({ r = 1; goto out3; r = 2; });
out3:
   assert(r == 1);

   //various side effect
  
   assert(f_ret() == 5);
   assert(f_static() == 0 && f_static() == 1);

   int a[3] = { 10, 20, 30 };
   int *p = ({ int *q = a; q + 1; });
   assert(*p == 20 && p[1] == 30);

   i = 10;
   i += ({ 5; });
   i *= ({ 2; });
   assert(i == 30);

   assert(add(({ 3; }), ({ 4; })) == 7);

   int il[3] = { ({ 1; }), ({ 2; }), ({ 3; }) };
   assert(il[0] == 1 && il[1] == 2 && il[2] == 3);

   assert(({ struct S sm = { 5, 6 }; sm.y; }) == 6);

   i = 2;
   s = ({switch (({ i; })) {
   case 1: r = 1; break;
   case 2: r = 2; break;
   default: r = 3; break;
   } r;});
   assert(s == 2);

   s = 0;
   r = ({ s += 1; 1; }) + ({ s += 10; 2; });
   assert(r == 3 && s == 11);

   s = 0;
   assert((1 && ({ s = 8; 0; }) || ({ s = 9; 3; })) == 1 && s == 9);

   // agg/arr typed
   struct S sa = ({ struct S t1 = { 3, 4 }; t1; });
   assert(sa.y == 4);
   assert(({ struct S t2 = { 5, 6 }; t2; }).y == 6);
   assert(({ a; })[1] == 20 && ({ a; })[2] == 30);

   // scope
   int t = 1;
   assert(({ int t = 3; t; }) + t == 4);
   assert(({ int t = 3; t; }) + ({ struct S t = { 4, 5 }; t.y; }) == 8);

   // voids
   vc = 0;
   ({ vnote(); vnote(); });
   ({}), ({ ; }), ({ int loc = 0; });
   assert(vc == 2);
   assert(({ vnote(), 5; }) == 5 && vc == 3);
   assert(sizeof(({ vnote(); })) == 1 && vc == 3);
   ({ for (int q = 0; q < 2; q++) vnote(); });
   assert(vc == 5);
   vfn();
   assert(vc == 6);

   // complex
   double complex cz = ({ 1.0 + 2.0 * I; });
   assert(creal(cz) == 1.0 && cimag(cz) == 2.0);
   cz = ({ double complex u = 3.0 + 4.0 * I; u * u; });
   assert(creal(cz) == -7.0 && cimag(cz) == 24.0);
   assert(creal(({ 5.0 + 6.0 * I; })) == 5.0);
   float complex cf = ({ 1.0f + 2.0f * I; });
   assert(crealf(cf) == 1.0f && cimagf(cf) == 2.0f);
   ({ 1.0 + 2.0 * I; });

   puts("ok");
   return 0;
}
