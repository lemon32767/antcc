#pragma once

#include "antcc.h"

#if HAS_BUILTIN(ctzll)
#define ctz_nonzero(x) __builtin_ctzll(x)
#else
static inline uint
ctz_nonzero(u64int x) {
   assert(x);
   uint n = 0;
   while ((x&1) == 0) ++n, x >>= 1;
   return n;
}
#endif
static inline uint
bclz(uint N, u64int x) {
   if (N < 64) x &= ((1ULL<<N) - 1);
   if (!x) return N;
   uint n = 0;
   while ((x >> (N-1 - n) & 1) == 0) ++n;
   return n;
}
static inline uint
bclrsb(uint N, s64int x) {
   if (N < 64) x = (s64int)((u64int)x << (64-N)) >> (64-N);
   return bclz(N, x ^ (x >> (N-1))) - 1;
}
#if HAS_BUILTIN(popcountll)
#define popcnt(x) __builtin_popcountll(x)
#else
static inline uint
popcnt(u64int x) {
   uint n = 0;
   while (x) n += x&1, x >>= 1;
   return n;
}
#endif

static inline bool
bstest(const BitSet *bs, uint i) {
   return bs[i/BSNBIT].u >> i%BSNBIT & 1;
}

static inline void
bsset(BitSet *bs, uint i) {
   bs[i/BSNBIT].u |= 1ull << i%BSNBIT;
}

static inline void
bsclr(BitSet *bs, uint i) {
   bs[i/BSNBIT].u &= ~(1ull << i%BSNBIT);
}

static inline void
bszero(BitSet bs[/*siz*/], uint siz) {
   memset(bs, 0, siz * sizeof *bs);
}

static inline void
bscopy(BitSet dst[/*siz*/], const BitSet src[/*siz*/], uint siz) {
   while (siz--) dst++->u = src++->u;
}

static inline void
bsunion(BitSet dst[/*siz*/], const BitSet src[/*siz*/], uint siz) {
   while (siz--) dst++->u |= src++->u;
}

static inline uint
bscount(const BitSet bs[/*siz*/], uint siz) {
   uint n = 0;
   while (siz--) n += popcnt(bs++->u);
   return n;
}

static inline bool
bsiter(uint *i, const BitSet bs[/*siz*/], uint siz) {
   uint k = *i/BSNBIT, j = *i%BSNBIT;
   if (k >= siz) return 0;
   size_t t = bs[k].u & ~(((size_t)1 << j) - 1);
   while (!t) {
      if (++k >= siz) return 0;
      t = bs[k].u;
   }
   *i = k*BSNBIT + lowestsetbit(t);
   return 1;
}
#define bs_each(var, bs, siz) for (uint (var) = 0; bsiter(&(var), (bs), (siz)); ++(var))

static inline bool
bsiterzr(uint *i, const BitSet bs[/*siz*/], uint siz) {
   uint k = *i/BSNBIT, j = *i%BSNBIT;
   if (k >= siz) return 0;
   size_t t = ~bs[k].u & ~(((size_t)1 << j) - 1);
   while (!t) {
      if (++k >= siz) return 0;
      t = ~bs[k].u;
   }
   *i = k*BSNBIT + lowestsetbit(t);
   return 1;
}
