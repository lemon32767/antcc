/* CFLAGS: -std=c89 */
/* EXPECT:
1 2.5 qwer.
*/

kandr(a,b,x,y)
int a;
register float b;
char y,x[sizeof a]; {
   extern printf();
   printf("%d %g %s%c\n", a, b, x,y);
}

#include<stdio.h>

foo;
main(t) {
   foo = t-1;
   kandr(1, 2.5, "qwer",'.');
   return foo;
}

