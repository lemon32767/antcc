/* EXPECT:
fib(10)  = 55
fibf(10) = 55
fibr(10) = 55
*/

unsigned fib(unsigned x) {
   unsigned r = 0, q = 1;
   for (; x > 1; --x) {
      unsigned s = r + q;
      r = q;
      q = s;
   }
   return q;
}

unsigned fibr(unsigned x) {
   if (x < 2) return x;
   return fibr(x-1) + fibr(x-2);
}

double fibf(unsigned x) {
   double r = 0., q = 1.;
   while (x-- > 1) {
      double s = r + q;
      r = q;
      q = s;
   }
   return q;
}

int atoi(const char *);
int printf(const char *, ...);

int main(int argc, char **argv) {
   unsigned n = argv[1] ? atoi(argv[1]) : 10;
   printf("fib(%u)  = %u\n", n, fib(n));
   printf("fibf(%u) = %g\n", n, fibf(n));
   printf("fibr(%u) = %u\n", n, fibr(n));
}

/* vim:set ts=3 sw=3 expandtab: */
