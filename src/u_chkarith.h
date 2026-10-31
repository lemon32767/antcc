#pragma once

#include "antcc.h"

static inline bool
chk_umul(uint *r, uint x, uint y) {
#if HAS_BUILTIN(mul_overflow)
   return !__builtin_mul_overflow(x, y, r);
#else
   u64int z = (u64int) x * y;
   if (z >> 32) return 0;
   return *r = z, 1;
#endif
}

static inline uint
chkx_umul(uint x, uint y) {
   uint r;
   assert(chk_umul(&r, x, y));
   return r;
}
