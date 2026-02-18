
/* CFLAGS: -O1 */
/* EXPECT:
25;0
*/

static inline int sqr(int x) {return x*x;}
static inline int foo(int);
static inline int ind(int (*f)(int), int arg) {
   return f(arg);
}

#include <stdio.h>
int main() {
   int q = ind(sqr, 5);
   printf("%d;%d\n", q, ind(foo,-2));
}

static inline int foo(int w) {
   return w+2;
}

