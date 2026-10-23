/* CFLAGS: -O2 */
/* EXPECT:
wordalign: 135038
bsunion: 1944
decr_gt0: 435
count_down: 2196750
incr_lt: 4060
incr_sqr_lt: 245
str_loop: 1660
len_dec: 4060
plain_for: 4060
do_while: 4060
nested: 2800
ivunsignwrap: 51002736354
*/

#include <stdio.h>
typedef long long s64int;
typedef unsigned long long u64int;

static void wordalign(unsigned char *buf, int *np, unsigned off, unsigned align) {
   while (off++ & (align - 1))
      buf[(*np)++] = 0;
}
static s64int t_wordalign(void) {
   s64int r = 0;
   for (unsigned a = 1; a <= 64; a <<= 1) {
      for (unsigned off = 0; off < 131; ++off) {
         unsigned char buf[256] = {0};
         int n = 0;
         wordalign(buf, &n, off, a);
         unsigned fa = ((off + a - 1) / a) * a;
         r += (s64int)n + (s64int)fa + (s64int)off;
         for (int i = 0; i < n; ++i)
            r += buf[i];
      }
   }
   return r;
}

static void bsunion(u64int *dst, const u64int *src, unsigned siz) {
   while (siz--) *dst++ |= *src++;
}
static u64int t_bsunion(void) {
   u64int r = 0;
   for (unsigned n = 0; n <= 8; ++n) {
      u64int a[16] = {0}, b[16];
      for (unsigned i = 0; i < 16; ++i) b[i] = (u64int)i * 3 + 1;
      bsunion(a, b, n);
      for (unsigned i = 0; i < 16; ++i)
         r += a[i] + i + n;
   }
   return r;
}

static void decr_gt0(int n, int *sp) {
   while (n-- > 0) (*sp)++;
}
static s64int t_decr_gt0(void) {
   s64int r = 0;
   for (int n = 0; n < 30; ++n) {
      int s = 0;
      decr_gt0(n, &s);
      r += s;
   }
   return r;
}

static void count_down(int n, int *sp) {
   for (int i = 100; i > 0; --i) *sp += i * n;
}
static s64int t_count_down(void) {
   s64int r = 0;
   for (int n = 0; n < 30; ++n) {
      int s = 0;
      count_down(n, &s);
      r += s;
   }
   return r;
}

static void incr_lt(int n, int *cp) {
   int x = 0;
   while ((x = x + 1) < n) *cp += x;
}
static s64int t_incr_lt(void) {
   s64int r = 0;
   for (int n = 0; n < 30; ++n) {
      int c = 0;
      incr_lt(n, &c);
      r += c;
   }
   return r;
}

static void incr_sqr_lt(int n, int *c) {
   int x = 0;
   while ((x = x + 1), x * (x + 1) < n) (*c)++;
}
static s64int t_incr_sqr_lt(void) {
   s64int r = 0;
   for (int n = 0; n < 60; ++n) {
      int c = 0;
      incr_sqr_lt(n, &c);
      r += c;
   }
   return r;
}

static void str_loop(const unsigned char *s, int *sp) {
   for (; *s; ++s) *sp += *s;
}
static s64int t_str_loop(void) {
   s64int r = 0;
   const unsigned char *ts[] = {
      (const unsigned char *)"",
      (const unsigned char *)"a",
      (const unsigned char *)"abc",
      (const unsigned char *)"hello world!",
      (const unsigned char *)"x\0y"
   };
   for (int i = 0; i < 5; ++i) {
      int s = 0;
      str_loop(ts[i], &s);
      r += s;
   }
   return r;
}

static void len_dec(int len, int *sp) {
   for (; len-- > 0;) *sp += len;
}
static s64int t_len_dec(void) {
   s64int r = 0;
   for (int len = 0; len < 30; ++len) {
      int s = 0;
      len_dec(len, &s);
      r += s;
   }
   return r;
}

static void plain_for(int n, int *sp) {
   for (int i = 0; i < n; ++i) *sp += i;
}
static s64int t_plain_for(void) {
   s64int r = 0;
   for (int n = 0; n < 30; ++n) {
      int s = 0;
      plain_for(n, &s);
      r += s;
   }
   return r;
}

static void do_while(int n, int *sp) {
   int i = 0;
   do *sp += i++; while (i < n);
}
static s64int t_do_while(void) {
   s64int r = 0;
   for (int n = 0; n < 30; ++n) {
      int s = 0;
      do_while(n, &s);
      r += s;
   }
   return r;
}

static void nested(unsigned n, unsigned *totp) {
   for (unsigned i = 0; i < n; ++i) {
      unsigned off = i;
      while (off++ & 7) (*totp)++;
   }
}
static s64int t_nested(void) {
   s64int r = 0;
   for (unsigned n = 0; n < 40; ++n) {
      unsigned t = 0;
      nested(n, &t);
      r += t;
   }
   return r;
}

static void ivunsignwrap(int n, s64int *sp) {
   s64int s = 0;
   for (int i = 0; i < n; ++i) {
      unsigned u = (unsigned)i * 0x0FFFFFFFu;
      s += (s64int)(int)u;
   }
   *sp = s;
}
static s64int t_ivunsignwrap(void) {
   s64int r = 0;
   for (int n = 0; n <= 12; ++n) {
      s64int s = 0;
      ivunsignwrap(n, &s);
      r += s;
   }
   return r;
}

int main(void) {
   printf("wordalign: %lld\n", t_wordalign());
   printf("bsunion: %llu\n", t_bsunion());
   printf("decr_gt0: %lld\n", t_decr_gt0());
   printf("count_down: %lld\n", t_count_down());
   printf("incr_lt: %lld\n", t_incr_lt());
   printf("incr_sqr_lt: %lld\n", t_incr_sqr_lt());
   printf("str_loop: %lld\n", t_str_loop());
   printf("len_dec: %lld\n", t_len_dec());
   printf("plain_for: %lld\n", t_plain_for());
   printf("do_while: %lld\n", t_do_while());
   printf("nested: %lld\n", t_nested());
   printf("ivunsignwrap: %lld\n", t_ivunsignwrap());
   return 0;
}

/*  vim:set ts=3 sw=3 expandtab:  */
