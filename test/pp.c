
#include "pp.h"
#include "pp.h"
#include <stddef.h>
#include <stdio.h>
//
#define CATl(a) a##bar
#define CATr(a) foo##a
#define CAT(a,b) a##b
#define foobar() foo##bar

#define hash_hash # ## #
#define mkstr(a) # a
#define in_between(a) mkstr(a)
#define join(c, d) in_between(c hash_hash d)
char p[] = join(x, y); // equivalent to   char p[] = "x ## y";

int
main(void)
{
   int CATl(foo);
   ++foobar;
   --CATr(bar);
   CAT(foo,bar) += 3;
   foobar() /=2;
   printf("%s %s\n",STR ( ok /1   "\n"n ;.& 
            05.5), STR(ADD(1,2)));
   hi(ADD(Foo, SQR(Bar+1)));
   int foo123 = 77;
   printf("%s "
         "%s %g\n", str(Foo,5), xstr(Foo), CAT(1.5,e3f) + CAT(7,)-CAT(,1));
   printf("join: \"%s\"\n", p);
   CAT(ret,urn) 0;
}
