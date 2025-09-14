int foo(int a, int b, int c, int d, int e, int f, int (*g)(void)) {
   int aa[4];
   void bar(int *);
   bar(aa);
   if (a>0)
      f-=10*(g()&f);
   bar(aa);
   return a + b + c + d + e + f + g();
}
