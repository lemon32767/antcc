/* CFLAGS: -std=c11 -Werror */
/* EXPECT:
*/

#include <stdlib.h>
#include <stdnoreturn.h>
#include <stdalign.h>

noreturn void quit(int x) {
   exit(x);
}

int foo(int x) {
   if (x < 0) quit(alignof(int *));
   else return x;
}

struct x{int h:2;} X;
int main(int argc, char **argv) {
   return foo(0);
}

