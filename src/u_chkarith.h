#pragma once

#include "antcc.h"

static inline bool
chk_umul(uint *r, uint x, uint y) {
#if HAS_BUILTIN(umul_overflow)
   return !__builtin_umul_overflow(x, y, r);
#else
   u64int z = (u64int) x * y;
   if (z > ~0u) return 0;
   return *r = z, 1;
#endif
}

static inline uint
chkx_umul(uint x, uint y) {
   uint r;
   assert(chk_umul(&r, x, y));
   return r;
}

static inline bool
chk_uadd(uint *r, uint x, uint y) {
#if HAS_BUILTIN(uadd_overflow)
   return !__builtin_uadd_overflow(x, y, r);
#else
   uint z = x + y;
   if (z < x) return 0;
   return *r = z, 1;
#endif
}

static inline uint
chkx_uadd(uint x, uint y) {
   uint r;
   assert(chk_uadd(&r, x, y));
   return r;
}
