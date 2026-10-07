/* CFLAGS: -O2 */
/* EXPECT:
wordalign: ok
bsunion: ok
decr_gt0: ok
count_down: ok
incr_lt: ok
incr_sqr_lt: ok
str_loop: ok
len_dec: ok
plain_for: ok
do_while: ok
nested: ok
*/

#include <stdio.h>

static int nfail;

static void chk(const char *name, int bad) {
   printf("%s: %s\n", name, bad ? "FAIL" : "ok");
   nfail += bad;
}

static void wordalign(unsigned char *buf, int *np, unsigned off, unsigned align) {
   while (off++ & (align - 1))
      buf[(*np)++] = 0;
}
static void t_wordalign(void) {
   int bad = 0;
   for (unsigned a = 1; a <= 64; a <<= 1) {
      for (unsigned off = 0; off < 131; ++off) {
         unsigned char buf[256] = {0};
         int n = 0;
         wordalign(buf, &n, off, a);
         unsigned fa = ((off + a - 1) / a) * a;
         if ((unsigned)n != fa - off) bad = 1;
         for (int i = 0; i < n; ++i)
            if (buf[i] != 0) bad = 1;
      }
   }
   chk("wordalign", bad);
}

typedef unsigned long long u64int;
static void bsunion(u64int *dst, const u64int *src, unsigned siz) {
   while (siz--) *dst++ |= *src++;
}
static void t_bsunion(void) {
   int bad = 0;
   for (unsigned n = 0; n <= 8; ++n) {
      u64int a[16] = {0}, b[16];
      for (unsigned i = 0; i < 16; ++i) b[i] = (u64int)i * 3 + 1;
      bsunion(a, b, n);
      for (unsigned i = 0; i < n; ++i)
         if (a[i] != b[i]) bad = 1;
      for (unsigned i = n; i < 16; ++i)
         if (a[i] != 0) bad = 1; /* one extra OR would corrupt a[n] */
   }
   chk("bsunion", bad);
}

static void decr_gt0(int n, int *sp) {
   while (n-- > 0) (*sp)++;
}
static void t_decr_gt0(void) {
   int bad = 0;
   for (int n = 0; n < 30; ++n) {
      int s = 0;
      decr_gt0(n, &s);
      if (s != n) bad = 1;
   }
   chk("decr_gt0", bad);
}

static void count_down(int n, int *sp) {
   for (int i = 100; i > 0; --i) *sp += i * n;
}
static void t_count_down(void) {
   int bad = 0;
   for (int n = 0; n < 30; ++n) {
      int s = 0;
      count_down(n, &s);
      if (s != 5050 * n) bad = 1;
   }
   chk("count_down", bad);
}

static void incr_lt(int n, int *cp) {
   int x = 0;
   while ((x = x + 1) < n) *cp += x;
}
static void t_incr_lt(void) {
   int bad = 0;
   for (int n = 0; n < 30; ++n) {
      int c = 0;
      incr_lt(n, &c);
      int ref = n > 0 ? n * (n - 1) / 2 : 0;
      if (c != ref) bad = 1;
   }
   chk("incr_lt", bad);
}

static void incr_sqr_lt(int n, int *c) {
   int x = 0;
   while ((x = x + 1), x * (x + 1) < n) (*c)++;
}
static void t_incr_sqr_lt(void) {
   int bad = 0;
   for (int n = 0; n < 60; ++n) {
      int c = 0;
      incr_sqr_lt(n, &c);
      int ref = 0;
      for (int i = 1; i * (i + 1) < n; ++i) ref++;
      if (c != ref) bad = 1;
   }
   chk("incr_sqr_lt", bad);
}

static void str_loop(const unsigned char *s, int *sp) {
   for (; *s; ++s) *sp += *s;
}
static void t_str_loop(void) {
   int bad = 0;
   const unsigned char *ts[] = {
      (const unsigned char *)"",
      (const unsigned char *)"a",
      (const unsigned char *)"abc",
      (const unsigned char *)"hello world!",
      (const unsigned char *)"x\0y"
   };
   for (int i = 0; i < 5; ++i) {
      int s = 0, ref = 0;
      str_loop(ts[i], &s);
      for (int j = 0; ts[i][j]; ++j) ref += ts[i][j];
      if (s != ref) bad = 1;
   }
   chk("str_loop", bad);
}

static void len_dec(int len, int *sp) {
   for (; len-- > 0;) *sp += len;
}
static void t_len_dec(void) {
   int bad = 0;
   for (int len = 0; len < 30; ++len) {
      int s = 0;
      len_dec(len, &s);
      if (s != len * (len - 1) / 2) bad = 1; /* 0 + ... + (len-1) */
   }
   chk("len_dec", bad);
}

static void plain_for(int n, int *sp) {
   for (int i = 0; i < n; ++i) *sp += i;
}
static void t_plain_for(void) {
   int bad = 0;
   for (int n = 0; n < 30; ++n) {
      int s = 0;
      plain_for(n, &s);
      if (s != n * (n - 1) / 2) bad = 1;
   }
   chk("plain_for", bad);
}

static void do_while(int n, int *sp) {
   int i = 0;
   do *sp += i++; while (i < n);
}
static void t_do_while(void) {
   int bad = 0;
   for (int n = 0; n < 30; ++n) {
      int s = 0;
      do_while(n, &s);
      if (s != n * (n - 1) / 2) bad = 1;
   }
   chk("do_while", bad);
}

static void nested(unsigned n, unsigned *totp) {
   for (unsigned i = 0; i < n; ++i) {
      unsigned off = i;
      while (off++ & 7) (*totp)++;
   }
}
static void t_nested(void) {
   int bad = 0;
   for (unsigned n = 0; n < 40; ++n) {
      unsigned t = 0;
      nested(n, &t);
      unsigned ref = 0;
      for (unsigned i = 0; i < n; ++i) {
         unsigned fa = ((i + 7) / 8) * 8;
         ref += fa - i;
      }
      if (t != ref) bad = 1;
   }
   chk("nested", bad);
}

int main() {
   t_wordalign();
   t_bsunion();
   t_decr_gt0();
   t_count_down();
   t_incr_lt();
   t_incr_sqr_lt();
   t_str_loop();
   t_len_dec();
   t_plain_for();
   t_do_while();
   t_nested();
   return nfail ? 1 : 0;
}

/*  vim:set ts=3 sw=3 expandtab:  */
