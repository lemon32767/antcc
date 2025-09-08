int printf(const char *, ...);
void *malloc(unsigned long);
int main() {
   int *mem = malloc(sizeof(int) * 4000);

   int cmax = 0;
   for (int nv = 1; nv < 1000; ++nv) {
      int n = nv, c = 0;
      while (n != 1) {
         if (n < nv) {
            c += mem[n];
            break;
         }
         if ((n & 1) != 0) {
            n = (3 * n) + 1;
         } else {
            n /= 2;
            //n >>= 1;
         }
         ++c;
      }
      mem[nv] = c;
      if (c > cmax) {
         cmax = c;
      }
   }
   printf("should print 178: %d\n", cmax);
}

