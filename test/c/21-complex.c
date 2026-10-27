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
? 2.0+0.0i 3.5+0.0i 1.5+2.5i
?: 3.0+4.0i 7.0+0.0i 3.0+4.0i 2.0+0.0i
CMPLX nan = 1 1.0
CMPLX inf = 1.0 1
mul inf = 1 1
div zero = 1 1
mul iinf = 1 1
div inf = 1 1
mulf inf = 1 1
divf zero = 1 1
(static)x = 1.0+0.0i
(static)add = 4.0+2.0i
(static)sub = 2.0-2.0i
(static)cadd = 4.0+2.0i
(static)csub = -2.0+2.0i
(static)neg = -1.0-2.0i
(static)-0 = -2.0 1
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

static _Complex float x = 1.0;
static _Complex double k_add = 3.0 + (1.0 + 2.0i);
static _Complex double k_sub = 3.0 - (1.0 + 2.0i);
static _Complex double k_cadd = (1.0 + 2.0i) + 3.0;
static _Complex double k_csub = (1.0 + 2.0i) - 3.0;
static _Complex float k_neg = -(1.0f + 2.0if);
static _Complex double k_negzero = 1.0 - (3.0 + 0.0i);

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

   volatile int z0 = 0, z1 = 7;
   complex double c1 = z0 ? a : 2.0, c2 = z1 ? 3.5 : a, c3 = z1 ? (complex float)(1.5 + 2.5 * I) : a;
   printf("? %.1f%+.1fi %.1f%+.1fi %.1f%+.1fi\n", creal(c1), cimag(c1), creal(c2), cimag(c2), creal(c3), cimag(c3));
   complex double e1 = z0 ?: a, e2 = z1 ?: a, e3 = a ?: 2.0, e4 = (a - a) ?: 2.0;
   printf("?: %.1f%+.1fi %.1f%+.1fi %.1f%+.1fi %.1f%+.1fi\n", creal(e1), cimag(e1), creal(e2), cimag(e2), creal(e3), cimag(e3), creal(e4), cimag(e4));

   volatile double inf = INFINITY, one = 1.0, zero = 0.0, nan = NAN;
   complex double cnan = CMPLX(nan, one);
   printf("CMPLX nan = %d %.1f\n", !!isnan(creal(cnan)), cimag(cnan));
   complex double cinf = CMPLX(one, inf);
   printf("CMPLX inf = %.1f %d\n", creal(cinf), !!isinf(cimag(cinf)));
   complex double mul_inf = CMPLX(inf, zero) * CMPLX(one, zero);
   printf("mul inf = %d %d\n", !!isinf(creal(mul_inf)), !!isnan(cimag(mul_inf)));
   complex double div_zero = CMPLX(one, zero) / CMPLX(zero, zero);
   printf("div zero = %d %d\n", !!isinf(creal(div_zero)), !!isnan(cimag(div_zero)));
   complex double mul_iinf = CMPLX(zero, inf) * CMPLX(zero, one);
   printf("mul iinf = %d %d\n", !!isinf(creal(mul_iinf)), !!isnan(cimag(mul_iinf)));
   complex double div_inf = CMPLX(inf, zero) / CMPLX(one, zero);
   printf("div inf = %d %d\n", !!isinf(creal(div_inf)), !!isnan(cimag(div_inf)));
   volatile float finf = INFINITY, fone = 1.0f, fzero = 0.0f;
   complex float mulf_inf = CMPLXF(finf, fzero) * CMPLXF(fone, fzero);
   printf("mulf inf = %d %d\n", !!isinf(crealf(mulf_inf)), !!isnan(cimagf(mulf_inf)));
   complex float divf_zero = CMPLXF(fone, fzero) / CMPLXF(fzero, fzero);
   printf("divf zero = %d %d\n", !!isinf(crealf(divf_zero)), !!isnan(cimagf(divf_zero)));
   printf("(static)x = %.1f%+.1fi\n", crealf(x), cimagf(x));
   printf("(static)add = %.1f%+.1fi\n", creal(k_add), cimag(k_add));
   printf("(static)sub = %.1f%+.1fi\n", creal(k_sub), cimag(k_sub));
   printf("(static)cadd = %.1f%+.1fi\n", creal(k_cadd), cimag(k_cadd));
   printf("(static)csub = %.1f%+.1fi\n", creal(k_csub), cimag(k_csub));
   printf("(static)neg = %.1f%+.1fi\n", crealf(k_neg), cimagf(k_neg));
   printf("(static)-0 = %.1f %d\n", creal(k_negzero), !!signbit(cimag(k_negzero)));

   return a;
}
