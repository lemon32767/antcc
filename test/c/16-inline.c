/* CFLAGS: -O1 */
/* EXPECT:
25;0
-3,-6,-9,-12,-15,
quack
woof
11;22;33
*/

static inline int sqr(int x) {return x*x;}
static inline int foo(int);
static inline int ind(int (*f)(int), int arg) {
   return f(arg);
}

int declfirst(void);
extern int extdecl(void);
inline int extdecl(void) { return 33; }
inline int declafter(void) { return 22; }
int declafter(void);

#include <stdio.h>

static inline void each(int *xs, size_t n, void (*f)(void *, int *), void *u) {
   for (; n > 0; --n, ++xs) f(u, xs);
}
static inline void multp(void *u, int *x) { *x *= *(int *)u; }
static inline void printi(void *_, int *x) { printf("%d,", *x); }

inline void quack(void) {
   puts("quack");
}

extern inline void bark(void) {
   puts("woof");
}

int main() {
   int q = ind(sqr, 5);
   printf("%d;%d\n", q, ind(foo,-2));
   int xs[5] = {1,2,3,4,5};
   int m = -3;
   each(xs, 5, multp, &m);
   each(xs, 5, printi, NULL);
   printf("\n");

   quack();
   bark();

   /* force no inlining to ensure we use the external defs */
   int (*volatile f)(void);
   f = declfirst, printf("%d;", f());
   f = declafter, printf("%d;", f());
   f = extdecl, printf("%d\n", f());
}
extern inline void quack(void);

inline int declfirst(void) { return 11; }

static inline int foo(int w) {
   return w+2;
}
