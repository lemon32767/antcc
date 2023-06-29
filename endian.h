#ifndef ENDIAN_H_
#define ENDIAN_H_

#include "common.h"

/*** Macros and functions for endian specific memory access ***/

/** byte-swapping functions **/

#if HAS_BUILTIN(bswap16)
#define bswap16 __builtin_bswap16
#else
static inline ushort
bswap16(ushort x) {
   return x >> 8 | x << 8;
}
#endif

#if HAS_BUILTIN(bswap32)
#define bswap32 __builtin_bswap32
#else
static inline uint
bswap32(uint x) {
   return x >> 24 & 0x000000FF | x >>  8 & 0x0000FF00
        | x <<  8 & 0x00FF0000 | x << 24 & 0xFF000000;
}
#endif

#if HAS_BUILTIN(bswap64)
#define bswap64 __builtin_bswap64
#else
static inline uvlong
bswap64(uvlong x) {
   return (uvlong) bswap32(x) << 32 | bswap32(x >> 32);
}
#endif

#if (defined __BYTE_ORDER__ && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__) ||               \
    defined __hppa__ || defined __m68k__ || defined mc68000 || defined _M_M68K ||       \
    (defined __MIPS__ && defined __MIPSEB__) ||                                         \
    defined __ppc__ || defined __POWERPC__ || defined __powerpc__ || defined __PPC__ || \
    defined __sparc__
#define HOST_BIG_ENDIAN
#define hostntarg_sameendian() (targ_bigendian)
#else
#define HOST_LIL_ENDIAN
#define hostntarg_sameendian() (!targ_bigendian)
#endif

/** little-endian memory writes **/

static inline void
wr16le(uchar *p, ushort x)
{
#ifndef HOST_LIL_ENDIAN
   x = bswap16(x);
#endif
   memcpy(p, &x, sizeof x);
}

static inline void
wr32le(uchar *p, uint x)
{
#ifndef HOST_LIL_ENDIAN
   x = bswap32(x);
#endif
   memcpy(p, &x, sizeof x);
}

static inline void
wr64le(uchar *p, uvlong x)
{
#ifndef HOST_LIL_ENDIAN
   x = bswap64(x);
#endif
   memcpy(p, &x, sizeof x);
}

/** big-endian memory writes **/

static inline void
wr16be(uchar *p, ushort x)
{
#ifndef HOST_BIG_ENDIAN
   x = bswap16(x);
#endif
   memcpy(p, &x, sizeof x);
}

static inline void
wr32be(uchar *p, uint x)
{
#ifndef HOST_BIG_ENDIAN
   x = bswap32(x);
#endif
   memcpy(p, &x, sizeof x);
}

static inline void
wr64be(uchar *p, uvlong x)
{
#ifndef HOST_BIG_ENDIAN
   x = bswap64(x);
#endif
   memcpy(p, &x, sizeof x);
}

/** target-endian memory writes **/

static inline void
wr16targ(uchar *p, ushort x)
{
   if (!hostntarg_sameendian()) x = bswap16(x);
   memcpy(p, &x, sizeof x);
}

static inline void
wr32targ(uchar *p, uint x)
{
   if (!hostntarg_sameendian()) x = bswap32(x);
   memcpy(p, &x, sizeof x);
}

static inline void
wr64targ(uchar *p, uvlong x)
{
   if (!hostntarg_sameendian()) x = bswap64(x);
   memcpy(p, &x, sizeof x);
}

static inline void
wrf32targ(uchar *p, float x)
{
   union { float f; uint i; } u = { x };
   wr32targ(p, u.i);
}

static inline void
wrf64targ(uchar *p, double x)
{
   union { double f; uvlong i; } u = { x };
   wr64targ(p, u.i);
}

#endif /* ENDIAN_H_ */

/* vim:set ts=3 sw=3 expandtab: */
