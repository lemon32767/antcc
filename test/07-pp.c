/* CFLAGS: -D CMD_WORKING -DV=void */
/* EXPECT:
ok /1 "\n"n ;.& 05.5 ADD(1,2)
hi from header ;73
Foo,5 9 1506
join: "x ## y"
wide   L"abc123 猫,€á💫",  U+1f98b
Output ends here
ok ...
*/

#include "07-pp.h"
#include "07-pp.h"
#include <stddef.h>
#include <stdio.h>
#include <wchar.h>
#include <locale.h>
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

#define PUTS p\
u\
t\
s

#define gnu_ext(a, c...) a(c)

#ifdef CMD_WORKING
int
main(V)
#endif // comment
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

   setlocale(LC_ALL, "en_US.utf8");

   printf("wide   L\"%ls\",  U+%x\n", L"abc123 猫,€á💫", L'🦋');

   PUT\
S\
("Output ends here\\
0Not printed" /* After line splicing, the remaining backslash
               * escapes the 0, ending the string early.
               */
);
   gnu_ext(printf, "ok %s\n", "...");

   CAT(ret,urn) 0;
}
