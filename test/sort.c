typedef unsigned long size_t;

int printf(const char *, ...);
void *calloc(size_t, size_t);
void qsort(void *, size_t nmemb, size_t size, int (const void *, const void *));
int atoi(const char *);

int
icmp(const void *a, const void *b)
{
   int l = *(int *)a, r = *(int *)b;
   return (l > r) - (l < r);
}

int nswp = 0, ncmp = 0;
void
revsort(int *xs, long lo, long hi)
{
   while (lo < hi) {
      long i = lo - 1, p = hi + 1;
      int pivot = xs[lo], tmp;
      for (;;) {
         do ++i; while (++ncmp, xs[i] > pivot);
         do --p; while (++ncmp, xs[p] < pivot);
         if (i >= p) break;
         tmp = xs[i];
         xs[i] = xs[p];
         xs[p] = tmp;
         ++nswp;
      }
      if (p + 1 >= hi) {
         hi = p;
      } else {
         if (lo < p)
            revsort(xs, lo, p);
         lo = p + 1;
      }
   }
}

int
main(int argc, char **argv)
{
   int N = argc - 1;
   int *xs = calloc(N, sizeof *xs);

   for (int i = 0; i < N; ++i)
      xs[i] = atoi(argv[i+1]);
   qsort(xs, N, sizeof *xs, icmp);
   for (int i = 0; i < N; ++i)
      printf("%d, ", xs[i]);
   printf("\n");
   revsort(xs, 0,N-1);
   printf("%d cmps, %d swaps\n", ncmp, nswp);
   for (int i = 0; i < N; ++i)
      printf("%d, ", xs[i]);
   printf("\n");
   return N;
}
