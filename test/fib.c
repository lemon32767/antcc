unsigned fib(unsigned x) {
   unsigned r = 0, q = 1;
   while (x--) {
      unsigned s = r + q;
      r = q;
      q = s;
   }
   return q;
}

int atoi(const char *);
int printf(const char *, ...);

int main(int argc, char **argv) {
   unsigned n = argv[1] ? atoi(argv[1]) : 10;
   printf("fib(%u) = %u\n", n, fib(n));
}

/* vim:set ts=3 sw=3 expandtab: */
