/* CFLAGS: -lm */
/* EXPECT:
ok -0.0
*/
typedef unsigned u32;
typedef signed s32;
typedef float f32;
typedef double f64;
typedef unsigned long long u64;
typedef signed long long s64;

f32 s32_to_f32(s32 x) { return x; }
f32 u32_to_f32(u32 x) { return x; }
f64 s32_to_f64(s32 x) { return x; }
f64 u32_to_f64(u32 x) { return x; }
f32 s64_to_f32(s64 x) { return x; }
f32 u64_to_f32(u64 x) { return x; }
f64 s64_to_f64(s64 x) { return x; }
f64 u64_to_f64(u64 x) { return x; }
s32 f32_to_s32(f32 x) { return x; }
u32 f32_to_u32(f32 x) { return x; }
s32 f64_to_s32(f64 x) { return x; }
u32 f64_to_u32(f64 x) { return x; }
s64 f32_to_s64(f32 x) { return x; }
u64 f32_to_u64(f32 x) { return x; }
s64 f64_to_s64(f64 x) { return x; }
u64 f64_to_u64(f64 x) { return x; }
f32 f64_to_f32(f64 x) { return x; }
f64 f32_to_f64(f32 x) { return x; }

#include <stdio.h>
#include <assert.h>

double NAN = 0.0;
double _(double x) { return x; }
double fmod(double, double);
double nummod(double a, double b) {
   double m = fmod(a,b);
   if ((m > 0) ? (printf("bluh\n"), b < 0) : (m < 0 && b > 0)) m += b;
   return m;
}

double branch(double a) {
   if (a + 1.1) return 0.0;
   return -a;
}

int main() {
   NAN/=0.0;
   assert(_(1.0) < _(2.0));
   assert(_(1.0) >= _(1.0));
   assert(_(1.0) >= _(-1.0));
   assert(_(1.0) != _(2.0));
   assert(_(1.0) - 1.0 == 0.0);
   assert(-_(0.0) == 0.0);
   assert(!(NAN < 2.0));
   assert(!(NAN > NAN));
   assert(!(NAN >= NAN));
   assert(!(NAN <= NAN));
   assert(!(NAN < NAN));
   assert(!(NAN == NAN));
   assert(NAN != NAN);
   assert(NAN != 0.0);
   printf("ok %.1f\n", nummod(-10.0, -2.0));
}
