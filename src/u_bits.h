#pragma once

#include "antcc.h"

static inline bool
bstest(const BitSet *bs, uint i)
{
   return bs[i/BSNBIT].u >> i%BSNBIT & 1;
}

static inline void
bsset(BitSet *bs, uint i)
{
   bs[i/BSNBIT].u |= 1ull << i%BSNBIT;
}

static inline void
bsclr(BitSet *bs, uint i)
{
   bs[i/BSNBIT].u &= ~(1ull << i%BSNBIT);
}

static inline void
bszero(BitSet bs[/*siz*/], uint siz)
{
   memset(bs, 0, siz * sizeof *bs);
}

static inline void
bscopy(BitSet dst[/*siz*/], const BitSet src[/*siz*/], uint siz)
{
   while (siz--) dst++->u = src++->u;
}

static inline void
bsunion(BitSet dst[/*siz*/], const BitSet src[/*siz*/], uint siz)
{
   while (siz--) dst++->u |= src++->u;
}

static inline uint
bscount(BitSet bs[/*siz*/], uint siz)
{
   uint n = 0;
   while (siz--) n += popcnt(bs++->u);
   return n;
}

static inline bool
bsiter(uint *i, BitSet bs[/*siz*/], uint siz)
{
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
#define bs_each(T, var, bs, siz) for (T (var) = 0; bsiter(&(var), (bs), (siz)); ++(var))

static inline bool
bsiterzr(uint *i, BitSet bs[/*siz*/], uint siz)
{
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
