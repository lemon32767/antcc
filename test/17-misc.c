/* EXPECT:
-1155497588
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
static intptr_t offst[] = {
   (long)((struct foo *)0)->b,
   (long)&((struct foo *)0)->a,
   (long)&((struct foo *)0)->b[1],
   (intptr_t)("12" - 5)
};

extern int printf(const char *, ...);
#include <assert.h>
int main() {
   printf("%d\n", fn1(-77ull));
   assert(offst[0] == offsetof(struct foo, b));
   assert(offst[1] == offsetof(struct foo, a));
   assert(offst[2] == offsetof(struct foo, b[1]));
}
