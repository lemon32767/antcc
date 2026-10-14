/* CFLAGS: -std=c11 -pedantic */
/* EXPECT:
*/

#include <stdlib.h>
#include <stddef.h>
#include <stdnoreturn.h>
#include <stdalign.h>

alignas(16) int aglob;
alignas(int *) int atype;
alignas(0) int azero;

struct astrct {
   char c;
   alignas(8) int x;
};
_Static_assert(_Alignof(struct astrct) == 8, "alignas member align");
_Static_assert(offsetof(struct astrct, x) == 8, "alignas member offset");

struct manyalignas {
   alignas(8) alignas(16) int x; /* strictest wins */
};
_Static_assert(_Alignof(struct manyalignas) == 16, "alignas strictest");

noreturn void quit(int x) {
   exit(x);
}

int foo(int x) {
   if (x < 0) quit(alignof(int *));
   else return x;
}

int s(int n, int x[]);
int s(int n, int *x);
int s(int n, int (*const x));
int s(int n, int x[*]);

struct x{int h:2;} X;
int main(int argc, char **argv) {
   alignas(16) int local;
   return foo(0);
}

