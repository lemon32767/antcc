#include "ir.h"

uchar type2cls[NTYPETAG];
uchar cls2siz[KF8+1];
const uchar siz2intcls[] = { [1] = KI4, [2] = KI4, [4] = KI4, [8] = KI8 };

struct instr instrtab[1<<14];
static int ninstr;
static int instrfreelist;
struct calltab calltab;
struct phitab phitab;
struct dattab dattab;

void
irinit(struct function *fn)
{
   static struct call callsbuf[64];
   static struct phi phisbuf[64];

   ninstr = 0;
   instrfreelist = -1;
   vinit(&calltab, callsbuf, arraylength(callsbuf));
   vinit(&phitab, phisbuf, arraylength(phisbuf));
   if (!type2cls[TYINT]) {
      for (int i = TYBOOL; i <= TYUVLONG; ++i) {
         int siz = targ_primsizes[i];
         type2cls[i] = siz < 8 ? KI4 : KI8;
      }
      type2cls[TYFLOAT] = KF4;
      type2cls[TYDOUBLE] = KF8;
      type2cls[TYPTR] = KPTR;
      type2cls[TYARRAY] = KPTR;
      cls2siz[KI4] = cls2siz[KF4] = 4;
      cls2siz[KI8] = cls2siz[KF8] = 8;
      cls2siz[KPTR] = targ_primsizes[TYPTR];
   }
   fn->entry = fn->curblk = alloc(&fn->arena, sizeof(struct block), 0);
   memset(fn->entry, 0, sizeof *fn->entry);
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

union ref
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

union ref
mkfltcon(struct function *fn, enum irclass k, double f)
{
   struct xcon con = { 0, k };
   if (k == KF4) con.fs = f;
   else          con.fd = f;
   return mkref(RXCON, addcon(&con));
}

union ref
mksymref(struct function *fn, const char *s)
{
   struct xcon con = { 1, KPTR, .sym = s };
   return mkref(RXCON, addcon(&con));
}

union ref
mkdatref(struct function *fn, uint siz, uint align, const void *bytes, uint n)
{
   struct irdat dat = { align, 0, siz };
   if (siz <= 8) memcpy(dat.sdat, bytes, n < siz ? n : siz);
   else {
      while (((uchar *)bytes)[n-1] == 0) --n; /* nip trailing zeroes */
      vpushn(&dat.dat, bytes, n);
   }
   vpush(&dattab, dat);
   return mkref(RDAT, dattab.n - 1); 
}

struct instr
mkalloca(uint siz, uint align)
{
   struct instr ins = { .cls = KPTR };
   assert(ispo2(align) && align <= 16);
   ins.op = Oalloca1 + ilog2(align);
   ins.l = mkref(RICON, siz/align + (siz%align != 0));
   return ins;
}

union ref
mkcallarg(struct function *fn, bool sret, uint narg, int vararg, union ref *args, union irtype *typs)
{
   struct call call = { .sret=sret, .narg=narg, .vararg=vararg };

   assert((long) vararg <= narg);
   if (narg) {
      call.args = alloc(&fn->arena, narg*sizeof *args + (narg+sret)*sizeof(union irtype), 0);
      call.typs = (union irtype *)((char *)call.args + narg*sizeof *args);
      memcpy(call.args, args, narg*sizeof *args);
      memcpy(call.typs, typs, (narg+sret)*sizeof *typs);
   }
   vpush(&calltab, call);
   return mkref(RMORE, calltab.n-1);
}

static inline int
newinstr(void)
{
   if (instrfreelist != -1) {
      int t = instrfreelist;
      memcpy(&instrfreelist, &instrtab[instrfreelist], sizeof(int));
      return t;
   }
   assert(ninstr < arraylength(instrtab));
   return ninstr++;
}

union ref
addinstr(struct function *fn, struct instr ins)
{
   int new = newinstr();
   assert(fn->curblk != NULL);
   instrtab[new] = ins;
   vpush(&fn->curblk->ins, new);
   return mkref(RTMP, new);
}

union ref
insertinstr(struct block *blk, int idx, struct instr ins)
{
   int new = newinstr();
   instrtab[new] = ins;
   if (idx == blk->ins.n) vpush(&blk->ins, new);
   else {
      assert(idx >= 0 && idx < blk->ins.n);
      vpush_((void **)&blk->ins.p, &blk->ins._cap, &blk->ins.n, sizeof *blk->ins.p);
      vresize(&blk->ins, blk->ins.n);
      for (int i = blk->ins.n++; i > idx; --i)
         blk->ins.p[i] = blk->ins.p[i - 1];
      blk->ins.p[idx] = new;
   }
   return mkref(RTMP, new);
}

void
delinstr(struct block *blk, int idx)
{
   assert(idx >= 0 && idx < blk->ins.n);
   memcpy(&instrtab[blk->ins.p[idx]], &instrfreelist, sizeof(int));
   instrfreelist = idx;
   for (int i = idx; i < blk->ins.n; ++i)
      blk->ins.p[i] = blk->ins.p[i + 1];
   --blk->ins.n;
}

union ref
addphi2(struct function *fn, enum irclass cls,
        struct block *b1, union ref r1, struct block *b2, union ref r2)
{
   int new;
   struct phi phi = { .n = 2, .cap = -1 };
   struct instr ins = { Ophi, cls };

   phi.blk = alloc(&fn->arena, 2*sizeof(struct block *) + 2*sizeof r1, 0);
   phi.ref = (union ref *)((char *)phi.blk + 2*sizeof(struct block *));
   phi.blk[0] = b1;
   phi.ref[0] = r1;
   phi.blk[1] = b2;
   phi.ref[1] = r2;
   vpush(&phitab, phi);
   ins.l = mkref(RMORE, phitab.n-1);
   assert(fn->curblk != NULL);
   assert(fn->curblk->ins.n == 0);
   new = newinstr();
   instrtab[new] = ins;
   vpush(&fn->curblk->phi, new);
   return mkref(RTMP, new);
}

union ref
addphi(struct function *fn, enum irclass cls, struct block **blk, union ref *ref, uint n)
{
   int new;
   struct phi phi = { .n = n, .cap = -1 };
   struct instr ins = { Ophi, cls };

   assert(n > 0);
   phi.blk = alloc(&fn->arena, n*sizeof(struct block *) + n*sizeof(union ref), 0);
   phi.ref = (union ref *)((char *)phi.blk + n*sizeof(struct block *));
   memcpy(phi.blk, blk, n * sizeof(struct block *));
   memcpy(phi.ref, ref, n * sizeof(union ref));
   vpush(&phitab, phi);
   ins.l = mkref(RMORE, phitab.n-1);
   assert(fn->curblk != NULL);
   assert(fn->curblk->ins.n == 0);
   new = newinstr();
   instrtab[new] = ins;
   vpush(&fn->curblk->phi, new);
   return mkref(RTMP, new);
}

struct block *
newblk(struct function *fn)
{
   struct block *blk = alloc(&fn->arena, sizeof(struct block), 0);
   memset(blk, 0, sizeof *blk);
   blk->id = -1;
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

#define putjump(fn, j, arg0, arg1, T, F) \
   fn->curblk->jmp.t = j;                \
   fn->curblk->jmp.arg[0] = arg0;        \
   fn->curblk->jmp.arg[1] = arg1;        \
   fn->curblk->s1 = T;                   \
   fn->curblk->s2 = F;                   \
   fn->curblk = NULL;

void
putbranch(struct function *fn, struct block *blk)
{
   assert(blk);
   putjump(fn, Jb, NOREF, NOREF, blk, NULL);
}

void
putcondbranch(struct function *fn, union ref arg, struct block *t, struct block *f)
{
   assert(t && f);
   putjump(fn, Jb, arg, NOREF, t, f);
}

void
putreturn(struct function *fn, union ref r0, union ref r1)
{
   putjump(fn, Jret, r0, r1, NULL, NULL);
}

#undef putjump

void
replref(struct function *fn, struct block *blk, int i0, union ref from, union ref to)
{
   do {
      if (!i0) for (int i = 0; i < blk->phi.n; ++i) {
         struct phi *phi = &phitab.p[instrtab[blk->phi.p[i]].l.i];
         for (int i = 0; i < phi->n; ++i)
            if (phi->ref[i].bits == to.bits) phi->ref[i] = from;
      }

      for (int i = i0; i < blk->ins.n; ++i) {
         struct instr *ins = &instrtab[blk->ins.p[i]];
         for (int i = 0; i < 2; ++i) {
            union ref *r = &(&ins->l)[i];
            if (r->bits == from.bits) *r = to;
            else if (r->t == RMORE) {
               struct call *call = &calltab.p[r->i];
               assert(ins->op == Ocall || ins->op == Ointrin);
               for (int i = 0; i < call->narg; ++i)
                  if (call->args[i].bits == from.bits)
                     call->args[i] = to;
            }
         }
      }
      i0 = 0;
   } while ((blk = blk->lnext) != fn->entry);
}

static void
freefn(struct function *fn)
{
   struct block *blk = fn->entry;
   do {
      vfree(&blk->phi);
      vfree(&blk->ins);
      blk = blk->lnext;
   } while (blk != fn->entry);
}

void
irfini(struct function *fn)
{
   extern int nerror;
   if (!nerror) {
      abi0(fn);
      regalloc(fn);
   }

   freefn(fn);
}

/* vim:set ts=3 sw=3 expandtab: */
