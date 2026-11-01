#pragma once

#include "antcc.h"

NORETURN void _chkxfail(const char *file, int line, const char *func, const char *expr);

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
_chkx_umul(const char *file, uint x, uint y, int line, const char *fn, const char *ex) {
   uint r;
   if (!chk_umul(&r, x, y)) _chkxfail(file, line, fn, ex);
   return r;
}
#define chkx_umul(x, y) _chkx_umul(__FILE__, x, y, __LINE__, __func__, #x" * "#y)

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
_chkx_uadd(const char *file, uint x, uint y, int line, const char *fn, const char *ex) {
   uint r;
   if (!chk_uadd(&r, x, y)) _chkxfail(file, line, fn, ex);
   return r;
}
#define chkx_uadd(x, y) _chkx_uadd(__FILE__, x, y, __LINE__, __func__, #x" + "#y)
