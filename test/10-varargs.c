/* EXPECT:
1 + 2 + 3 + 4 + 5 + 6 + 7 + 8 = 36
<1.5>
1.1 + 2.1 + 3.1 + 5.1 + 1.5 + -1.5 + 1.5 + -1.5 + 1.5 + -1 = 11.9
*/


#include <stdarg.h>
#include <stdio.h>

int sum(int x, ...) {
   va_list ap;
   va_start(ap, x);
   printf("%d", x);
   for (int y; (y = va_arg(ap, int));) {
      printf(" + %d",y);
      x += y;
   }
   va_end(ap);
   return x;
}

double stkarg(double a, double b, double c, double d, double e, double f, double g, double h, double x) {
   printf("");
   return x;
}

double sumf(double x, ...) {
   va_list ap;
   va_start(ap, x);
   printf("%g", x);
   for (double y; (y = va_arg(ap, double));) {
      printf(" + %g",y);
      x += y;
   }
   va_end(ap);
   return x;
}


int main() {
   printf(" = %d\n", sum(1,2,3,4,5,6,7,8,0,0));
   printf("<%g>\n", stkarg(0,0,0,0,0,0,0,0,1.5));
   printf(" = %g\n", sumf(1.1, 2.1, 3.1, 5.1, 1.5, -1.5, 1.5, -1.5, 1.5, -1.0, 0.0));
}
