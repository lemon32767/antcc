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

extern int printf(const char *, ...);
int main() {
   printf("%d\n", fn1(-77ull));
}
