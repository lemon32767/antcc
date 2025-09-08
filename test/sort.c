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
   return N;
}
