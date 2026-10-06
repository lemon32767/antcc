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
 1 2
 4 5
 7 8
sum 27
callvla = 1
szvla = 1
condvla = 1
castvla = 1
globvla = 1
memvla = 1
derefvla = 1
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

void bar(char *s) {
   int n = strlen(s);
   int aa[n+1][n+6], *t = aa[0];
   for (int i = 0; i < sizeof aa / sizeof *t; ++i) t[i] = i;
   pri(aa[0][0]), pri(aa[0][5]), pri(aa[5][2]);
   pri(sizeof(char[2][n]));
}


void primat(int n, int m, int xs[m][n]) {
   for (int i = 0; i < m; ++i) {
      printf(" ");
      for (int j = 0; j < n; ++j)
         printf("%d%c", xs[i][j], j<n-1 ? ' ' : '\n');
   }
}
int summat(int n, int m, int xs[m][n]) {
   int s = 0;
   for (int i = 0; i < m; ++i)
      for (int j = 0; j < n; ++j)
         s += xs[i][j];
   return s;
}

int g(int x) { return x + 1; }
int glob = 3;
struct S { int n; };

/* edge cases for VLAs in parameters */
static int callvla(int n, int (*p)[g(n)]) { return sizeof *p == (n+1)*4; }
static int szvla(int n, int (*a)[n], int (*b)[sizeof *a / sizeof(int)]) {
   return sizeof *b == sizeof *a && sizeof *a == n*4;
}
static int condvla(int n, int (*p)[n ? n : 1]) { return sizeof *p == n*4; }
static int castvla(int n, int (*p)[(int)(n + 1)]) { return sizeof *p == (n+1)*4; }
static int globvla(int (*p)[glob]) { return sizeof *p == glob*4; }
static int memvla(struct S s, int (*p)[s.n]) { return sizeof *p == s.n*4; }
static int derefvla(int *q, int (*p)[*q]) { return sizeof *p == *q*4; }

int
main() {
   foo("12345");
   bar("12345");
   int m[3][2] = {
      {1,2}, {4,5}, {7,8}
   };
   primat(2,3,m);
   printf("sum %d\n", summat(2, 3, m));

   int a[8] = {0};
   struct S s = {4};
   int q = 4;
   printf("callvla = %d\n", callvla(4, (int (*)[5])a));
   printf("szvla = %d\n", szvla(4, (int (*)[4])a, (int (*)[4])a));
   printf("condvla = %d\n", condvla(4, (int (*)[4])a));
   printf("castvla = %d\n", castvla(4, (int (*)[5])a));
   printf("globvla = %d\n", globvla((int (*)[3])a));
   printf("memvla = %d\n", memvla(s, (int (*)[4])a));
   printf("derefvla = %d\n", derefvla(&q, (int (*)[4])a));
}
