/* EXPECT:
-1155497588
S3 =16, f2@8, q@14
*/

typedef unsigned long long uvlong;
int fn1(uvlong p_9) {
/* extract from ldrgen seed=637312671 */
   uvlong v_73, q;
   if (p_9) {
      v_73 = 909910719;
   }
   q = (uvlong)p_9 / ((v_73 - 572547313ull) + 445ull);
   return q;
}

/* Excerpt from linux/kernel.h */
/*
 * This returns a constant expression while determining if an argument is
 * a constant expression, most importantly without evaluating the argument.
 * Glory to Martin Uecker <Martin.Uecker@med.uni-goettingen.de>
 */
#define __is_constexpr(x) \
	(sizeof(int) == sizeof(*(8 ? ((void *)((long)(x) * 0l)) : (int *)8)))

_Static_assert(__is_constexpr(5 &&(1<<3) || 0/0), "");
_Static_assert(!__is_constexpr(5/0), "");
_Static_assert(!__is_constexpr(fn1(0)), "");

#include <stddef.h>
#include <stdint.h>
struct foo { int *a, b[2]; };

static int g_37[3][5] = {{1,2,3,4,5},{11,12,13,14,15}};
static intptr_t offst[] = {
   (long)((struct foo *)0)->b,
   (long)&((struct foo *)0)->a,
   (long)&((struct foo *)0)->b[1],
   (intptr_t)("12" - 5),
};
static int *g_131 = &g_37[0][4];
static int *g_132 = &g_37[1][3];
static int *g_133 = &g_37[2][0];

#include <stdint.h>
struct S3 {
   unsigned f0 : 21;
   volatile unsigned f1 : 13;
   unsigned  f2;
   unsigned : 0;
   short f3 : 13;
   char q;
};

extern int printf(const char *, ...);
#include <assert.h>
int main() {
   printf("%d\n", fn1(-77ull));
   assert(offst[0] == offsetof(struct foo, b));
   assert(offst[1] == offsetof(struct foo, a));
   assert(offst[2] == offsetof(struct foo, b[1]));
   assert(*g_131 == 5 && *g_132 == 14 && *g_133 == 0);
   printf("S3 =%d, f2@%d, q@%d\n", (int)sizeof(struct S3), (int)offsetof(struct S3, f2), (int)offsetof(struct S3, q));
}
