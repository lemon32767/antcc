#include "ir.h"
#include "common.h"

uchar type2cls[TYARRAY + 1];
uchar cls2siz[KF8+1];
const uchar siz2intcls[] = { [1] = KI4, [2] = KI4, [4] = KI4, [8] = KI8 };

static vec_of(struct irdat) dats;
struct instr instr[1<<14];
static int ninstr;
vec_of(struct ircall) calls;

void
irinit(struct function *fn)
{
   static struct ircall callsbuf[64];

   ninstr = 0;
   vinit(&calls, callsbuf, arraylength(callsbuf));
   if (!type2cls[TYINT]) {
      for (int i = TYCHAR; i <= TYUVLONG; ++i) {
         int siz = targ_primsizes[i];
         type2cls[i] = siz < 8 ? KI4 : KI8;
      }
      type2cls[TYFLOAT] = KF4;
      type2cls[TYDOUBLE] = KF8;
      type2cls[TYPTR] = KIP;
      type2cls[TYARRAY] = KIP;
      cls2siz[KI4] = cls2siz[KF4] = 4;
      cls2siz[KI8] = cls2siz[KF8] = 8;
      cls2siz[KIP] = targ_primsizes[TYPTR];
   }
   fn->entry = fn->curblk = alloc(&fn->arena, sizeof(struct block), 0);
   fn->entry->lprev = fn->entry->lnext = fn->entry;
}

struct xcon conht[1 << 12];

static int
addcon(const struct xcon *con)
{
   uint h = hashb(0, con, sizeof *con);
   uint i = h, n = arraylength(conht);
   assert(con->issym || con->cls);
   for (;; ++i) {
      i &= arraylength(conht) - 1;
      if (!conht[i].issym && !conht[i].cls) {
         conht[i] = *con;
         return i;
      } else if (!memcmp(&conht[i], con, sizeof *con)) {
         return i;
      }
      assert(--n > 0 && "conht full");
   }
}

union irref
adddat(struct function *fn, const struct irdat *dat)
{
   assert(dats.n < 1u<<29);
   vpush(&dats, *dat);
   /* return mkref(RDAT, dats.n - 1); */
}

static void
targwrite2(uchar *d, ushort x)
{
   if (targ_bigendian)
      d[0] = x >> 8, d[1] = x;
   else
      d[0] = x, d[1] = x >> 8;
}

static void
targwrite4(uchar *d, uint x)
{
   if (targ_bigendian)
      d[0] = x >> 24, d[1] = x >> 16, d[2] = x >> 8, d[3] = x;
   else
      d[0] = x, d[1] = x >> 8, d[2] = x >> 16, d[3] = x >> 24;
}

static void
targwrite8(uchar *d, uvlong x)
{
   if (targ_bigendian)
      targwrite4(d, x >> 32), targwrite4(d + 4, x);
   else
      targwrite4(d, x), targwrite4(d + 4, x >> 32);
}

void
conputdat(struct irdat *dat, uint off, enum typetag t, const void *src)
{
   uint siz = targ_primsizes[t];
   bool iszero = 1;
   uchar *pdat;
   assert(off + siz <= dat->siz);
   for (uint i = 0; i < siz; ++i) {
      if (((uchar *)src) != 0) {
         iszero = 0;
         break;
      }
   }
   if (iszero && (dat->siz <= 8 || dat->dat.n < off))
      return;

   if (dat->siz > 8)
      while (off + siz > dat->dat.n)
         vpush(&dat->dat, 0);
   pdat = dat->siz <= 8 ? dat->sdat : dat->dat.p;

   switch (siz) {
   case 1: pdat[off] = *(uchar *)src; break;
   case 2: targwrite2(&pdat[off], *(ushort *)src); break;
   case 4: targwrite4(&pdat[off], *(uint *)src); break;
   case 8: targwrite8(&pdat[off], *(uvlong *)src); break;
   default: assert(0);
   }
}

union irtype
mkirtype(union type t)
{
   assert(t.t != TYVOID);
   if (isscalar(t)) return (union irtype) { .cls = type2cls[t.t] };
   assert(isagg(t));
   return (union irtype) { .isagg = 1, .dat = t.dat };
}

union irref
mkintcon(struct function *fn, enum irclass k, vlong i)
{
   if (i < 1ll << 28 && i >= -(1ll << 28)) {
      return mkref(RICON, i);
   } else if (k == KI4) {
      struct xcon con = { 0, k, .i4 = i };
      return mkref(RXCON, addcon(&con));
   } else {
      struct xcon con = { 0, k, .i8 = i };
      return mkref(RXCON, addcon(&con));
   }
}

union irref
mkfltcon(struct function *fn, enum irclass k, double f)
{
   struct xcon con = { 0, k };
   if (k == KF4) con.fs = f;
   else          con.fd = f;
   return mkref(RXCON, addcon(&con));
}

union irref
mksymref(struct function *fn, const char *s)
{
   struct xcon con = { 1, KIP, .sym = s };
   return mkref(RXCON, addcon(&con));
}

union irref
mkcall(struct function *fn, union type fnty, uint narg, union irref *args, union irtype *typs)
{
   const struct typedata *td = &typedata[fnty.dat];
   struct ircall call = { narg, td->variadic ? td->nmemb : -1 };

   assert(td->variadic ? narg >= td->nmemb : narg == td->nmemb);
   if (narg) {
      call.args = alloc(&fn->arena, narg*sizeof *args + narg*sizeof(union irtype), 0);
      call.typs = (union irtype *)((char *)call.args + narg*sizeof *args);
      memcpy(call.args, args, narg*sizeof *args);
      memcpy(call.typs, typs, narg*sizeof *typs);
   }
   vpush(&calls, call);
   return mkref(RCALL, calls.n-1);
}

union irref
addinstr(struct function *fn, struct instr ins)
{
   assert(ninstr < arraylength(instr));
   assert(fn->curblk != NULL);
   instr[ninstr] = ins;
   vpush(&fn->curblk->ins, ninstr);
   return mkref(RTMP, ninstr++);
}

struct block *
newblk(struct function *fn)
{
   struct block *blk = alloc(&fn->arena, sizeof(struct block), 0);
   memset(blk, 0, sizeof *blk);
   return blk;
}

void
useblk(struct function *fn, struct block *blk)
{
   if (fn->curblk) assert(fn->curblk->jmp.t && "never finished block");
   if (blk) assert(!blk->jmp.t && "reusing built block");
   if (!blk->lprev) { /* initialize */
      blk->lnext = fn->entry;
      blk->lprev = fn->entry->lprev;
      blk->lprev->lnext = blk;
      blk->id = blk->lprev->id + 1;
      fn->entry->lprev = blk;
   }
   fn->curblk = blk;
}

void
putjump(struct function *fn, enum jumpkind j, union irref arg, struct block *t, struct block *f)
{
   fn->curblk->jmp.t = j;
   fn->curblk->jmp.arg = arg;
   fn->curblk->s1 = t;
   fn->curblk->s2 = f;
   fn->curblk = NULL;
}

/* vim:set ts=3 sw=3 expandtab: */
