/* EXPECT:
sizeof x = 20
sizeof a = 624, (n+1)*(m*2)*4 = 624
sizeof p = 8
sizeof *p = 140
sizeof **p = 28
sizeof ***p = 4
sizeof q1 = 100, sizeof *q1 = 20
sizeof q2 = 100, sizeof *q2 = 20
aa[0][0] = 0
aa[0][5] = 5
aa[5][2] = 57
sizeof(char[2][n]) = 10
*/

#include <stdio.h>
#include <string.h>

#define pri(x) printf(#x" = %d\n", (int)(x))
#define prii(x,y) printf(#x" = %d, "#y" = %d\n", (int)(x), (int)(y))
void
foo(char *s) {
   int n = strlen(s);

   {
      int x[n];
      pri(sizeof x);
   }
   {
      int m = n + 8;
      int a[n+1][m*2];
      prii(sizeof a, (n+1)*(m*2)*4);
   }
   {
      int (*p)[n][n+2];
      pri(sizeof p);
      pri(sizeof *p);
      pri(sizeof **p);
      pri(sizeof ***p);
   }

   {
      int q1[5][n];
      prii(sizeof q1, sizeof *q1);
   }

   {
      int q2[n][5];
      prii(sizeof q2, sizeof *q2);
   }
}

int bar(char *s) {
   int n = strlen(s);
   int aa[n+1][n+6], *t = aa[0];
   for (int i = 0; i < sizeof aa / sizeof *t; ++i) t[i] = i;
   pri(aa[0][0]), pri(aa[0][5]), pri(aa[5][2]);
   pri(sizeof(char[2][n]));
}

int
main() {
   foo("12345");
   bar("12345");
}
