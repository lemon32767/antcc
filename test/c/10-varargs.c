/* EXPECT:
1 + 2 + 3 + 4 + 5 + 6 + 7 + 8 = 36
<1.5>
1.1 + 2.1 + 3.1 + 5.1 + 1.5 + -1.5 + 1.5 + -1.5 + 1.5 + -1 = 11.9
fwd()/1: Hello World 42; -1.5,0; -1
fwd()/2: Hello World 42; -1.5,0; -1
tail: 5 6 7 800 900
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
#include <string.h>

void fwd(const char *fmt, ...) {
   va_list ap, aq;
   va_start(ap, fmt);
   va_copy(aq, ap);
   printf("fwd()/1: ");
   vprintf(fmt, ap);
   va_end(ap);
   printf("\n");
   printf("fwd()/2: ");
   int off = vprintf(fmt, aq);
   printf("\n");
   va_end(aq);
   char fmt2[1000], buh[1000];
   strcpy(fmt2, fmt);
   strcat(fmt2, "%d %d %d %d %d");
   va_end(aq);
   va_start(aq, fmt);
   vsprintf(buh, fmt2, aq);
   printf("tail: %s\n", buh + off);
   va_end(aq);
}


int main() {
   printf(" = %d\n", sum(1,2,3,4,5,6,7,8,0,0));
   printf("<%g>\n", stkarg(0,0,0,0,0,0,0,0,1.5));
   printf(" = %g\n", sumf(1.1, 2.1, 3.1, 5.1, 1.5, -1.5, 1.5, -1.5, 1.5, -1.0, 0.0));
   fwd("%s %s %d; %g,%g; %d", "Hello", "World", 42, -1.5, 0.0, -1,
         5, 6, 7, 800, 900);
}
