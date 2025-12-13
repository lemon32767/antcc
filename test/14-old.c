/* CFLAGS: -std=c89 -trigraphs */
/* EXPECT:
*/

??=include<stdio.h>

foo;
main(t) ??<
   foo = t-1;
   return foo;
??>

