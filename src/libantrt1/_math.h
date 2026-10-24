typedef unsigned long long u64int;
typedef unsigned u32int;
#define complex _Complex
#define INFINITY __builtin_inff()
#define isnan(x) __builtin_isnan(x)
#define isinf(x) __builtin_isinf(x)

/** double **/
static inline double
__rt_fabsd(double x) {
   union { double f; u64int u; } v = {x};
   v.u &= ~(1ull<<63);
   return v.f;
}

static inline int
__rt_isfinited(double x) {
   union { double f; u64int u; } v = {x};
   return ((v.u >> 52) & 0x7ff) != 0x7ff;
}

static inline double
__rt_copysignd(double x, double y) {
   union { double f; u64int u; } v = {x}, s = {y};
   v.u = (v.u & ~(1ull<<63)) | (s.u & (1ull<<63));
   return v.f;
}

static inline double
__rt_fmaxd(double x, double y) {
   if (isnan(x)) return y;
   if (isnan(y)) return x;
   if (x == 0.0 && y == 0.0) return 0.0;
   return x > y ? x : y;
}

static inline double
__rt_logbd(double x) {
   union { double f; u64int u; } v = {x};
   u64int e = (v.u >> 52) & 0x7ff, m = v.u & ((1ull<<52) - 1);
   if (e == 0x7ff) return m ? x : INFINITY;
   if (e) return (double)((int)e - 1023);
   if (!m) return -INFINITY;
   return (double)((63 - __builtin_clzll(m)) - 1074);
}

static inline double
__rt_scalbnd(double x, int n) {
   while (n > 1023) x *= 0x1.0p1023, n -= 1023;
   while (n < -1022) x *= 0x1.0p-1022, n += 1022;
   union { u64int u; double f; } p = {.u = (u64int)(n + 1023) << 52};
   return x * p.f;
}

static inline double
__rt_creald(double complex z) {
   return ((double *)&z)[0];
}

static inline double
__rt_cimagd(double complex z) {
   return ((double *)&z)[1];
}

/** float **/
static inline float
__rt_fabsf(float x) {
   union { float f; u32int u; } v = {x};
   v.u &= ~(1U << 31);
   return v.f;
}

static inline int
__rt_isfinitef(float x) {
   union { float f; u32int u; } v = {x};
   return ((v.u >> 23) & 0xff) != 0xff;
}

static inline float
__rt_copysignf(float x, float y) {
   union { float f; u32int u; } v = {x}, s = {y};
   v.u = (v.u & ~(1U << 31)) | (s.u & (1U << 31));
   return v.f;
}

static inline float
__rt_fmaxf(float x, float y) {
   if (isnan(x)) return y;
   if (isnan(y)) return x;
   if (x == 0.0f && y == 0.0f) return 0.0f;
   return x > y ? x : y;
}

static inline float
__rt_logbf(float x) {
   union { float f; u32int u; } v = {x};
   u32int e = (v.u >> 23) & 0xff, m = v.u & ((1U << 23) - 1);
   if (e == 0xff) return m ? x : INFINITY;
   if (e) return (float)((int)e - 127);
   if (!m) return -INFINITY;
   return (float)((31 - __builtin_clz(m)) - 149);
}

static inline float
__rt_scalbnf(float x, int n) {
   while (n > 127) x *= 0x1.0p127f, n -= 127;
   while (n < -126) x *= 0x1.0p-126f, n += 126;
   union { u32int u; float f; } p = {.u = (u32int)(n + 127) << 23};
   return x * p.f;
}

static inline float
__rt_crealf(float complex z) {
   return ((float *)&z)[0];
}

static inline float
__rt_cimagf(float complex z) {
   return ((float *)&z)[1];
}

/** generic **/
#define fabs(x) _Generic(x, double: __rt_fabsd, float: __rt_fabsf)(x)
#define isfinite(x) _Generic(x, double: __rt_isfinited, float: __rt_isfinitef)(x)
#define copysign(x,y) _Generic(x, double: __rt_copysignd, float: __rt_copysignf)(x,y)
#define fmax(x,y) _Generic(x, double: __rt_fmaxd, float: __rt_fmaxf)(x,y)
#define logb(x) _Generic(x, double: __rt_logbd, float: __rt_logbf)(x)
#define scalbn(x,n) _Generic(x, double: __rt_scalbnd, float: __rt_scalbnf)(x,n)
#define creal(x) _Generic(x, complex double: __rt_creald, complex float: __rt_crealf)(x)
#define cimag(x) _Generic(x, complex double: __rt_cimagd, complex float: __rt_cimagf)(x)

#define __complex(t,x,y) ((union{t s[2]; complex t z;}){x,y}).z
#define __builtin_complex(x,y) _Generic((x)+(y), \
   float:__complex(float,x,y), double:__complex(double,x,y), long double:__complex(long double,x,y))
