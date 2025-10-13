
#include "pp.h"
#include "pp.h"
#include <stddef.h>
#include <stdio.h>
//
#define CATl(a) a##bar
#define CATr(a) foo##a
#define CAT(a,b) a##b


int
main(void)
{
   int CATl(foo);
   ++foobar;
   --CATr(bar);
   CAT(foo,bar) += 3;
   printf("%s %s\n",STR ( ok /1   "\n"n ;.& 
            05.5), STR(ADD(1,2)));
   hi(ADD(Foo, SQR(Bar+1)));
   int foo123 = 77;
   printf("%s %s %g\n", str(Foo,5), xstr(Foo), CAT(1.5,e3f) + CAT(7,)-CAT(1,));
}
