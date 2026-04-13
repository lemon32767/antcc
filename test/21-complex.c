/* CFLAGS: -lm */
/* EXPECT:
a + b = 4.0+6.0i
a - b = 2.0+2.0i
a * b = -5.0+10.0i
a / b = 2.2-0.4i
c = -2.2-3.0i
|a| = 5.0
arg(a) = 0.9
conj(a) = 3.0-4.0i
I = 0.0+1.0i
(1+2i)*I = -2.0+1.0i
(1+2i)/I = 2.0-1.0i
polar(1,0) = 1.0+0.0i
foo(a) = 81.0-95.0
a==b = 0; b!=a = 1; !a = 0; a&&b = 1; !!I = 1
e^(i*0.5) = 0.877583+0.479426i
cos(0.5)+i*sin(0.5) = 0.877583+0.479426i
*/

#include <complex.h>
#include <stdio.h>
#include <math.h>


complex double add_cd(complex double a, complex double b) { return a + b; }
complex double sub_cd(complex double a, complex double b) { return a - b; }
complex double mul_cd(complex double a, complex double b) { return a * b; }
complex double scale(complex double z, double s) { return z * s; }
complex double div_cd(complex double a, complex double b) { return a / b; }
complex double sdiv(complex double z, double s) { return z / s; }
complex double mul_i(complex double z) { return z * I; }
complex double div_i(complex double z) { return z / I; }

complex double polar_cd(double r, double theta) {
   return r * (cos(theta) + I * sin(theta));
}

struct s {
   complex float t;
};

complex float foo(struct s s) {
   s.t += 77.0 - 99.0iF;
   ++s.t;
   return s.t;
}

int main() {
   complex double a = 3.0 + 4.0 * I;
   complex double b = 1 + 2.0 * I;
   complex double unit = I;

   complex double sum = add_cd(a, b);
   printf("a + b = %.1f%+.1fi\n", creal(sum), cimag(sum));
   complex double diff = sub_cd(a, b);
   printf("a - b = %.1f%+.1fi\n", creal(diff), cimag(diff));
   complex double prod = mul_cd(a, b);
   printf("a * b = %.1f%+.1fi\n", creal(prod), cimag(prod));
   complex double quot = div_cd(a, b);
   printf("a / b = %.1f%+.1fi\n", creal(quot), cimag(quot));
   complex double c = sdiv(scale(a, 1.5), -2.0);
   printf("c = %.1f%+.1fi\n", creal(c), cimag(c));
   double mag = cabs(a);
   printf("|a| = %.1f\n", mag);
   double ph = carg(a);
   printf("arg(a) = %.1f\n", ph);
   complex double conj_a = conj(a);
   printf("conj(a) = %.1f%+.1fi\n", creal(conj_a), cimag(conj_a));
   printf("I = %.1f%+.1fi\n", creal(unit), cimag(unit));
   complex double mult_i = mul_i(1.0 + 2.0 * I);
   printf("(1+2i)*I = %.1f%+.1fi\n", creal(mult_i), cimag(mult_i));
   complex double divd_i = div_i(1.0 + 2.0 * I);
   printf("(1+2i)/I = %.1f%+.1fi\n", creal(divd_i), cimag(divd_i));
   complex double pol = polar_cd(1.0, 0.0);
   printf("polar(1,0) = %.1f%+.1fi\n", creal(pol), cimag(pol));
   complex float f = foo((struct s){a});
   printf("foo(a) = %.1f%+.1f\n", crealf(f), cimagf(f));
   printf("a==b = %d; b!=a = %d; !a = %d; a&&b = %d; !!I = %d\n", a == b, b != a, !a, a && b, !!unit);
   
   double theta = 0.5;
   complex double eith = cexp(I * theta);
   complex double target = cos(theta) + I * sin(theta);
   printf("e^(i*0.5) = %.6f%+.6fi\n", creal(eith), cimag(eith));
   printf("cos(0.5)+i*sin(0.5) = %.6f%+.6fi\n", creal(target), cimag(target));
   
   return a;
}
