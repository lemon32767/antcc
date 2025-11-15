#include <stdarg.h>
#include <stdio.h>

int sum(int x, ...) {
   va_list ap;
   va_start(ap, x);
   for (int y; (y = va_arg(ap, int));) {
      printf("got %d\n",y);
      x += y;
   }
   va_end(ap);
   return x;
}

int main() {
   printf("%d\n", sum(1,2,3,4,5,6,5.5,7,0,0));
}
