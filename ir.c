#include "ir.h"
#include "endian.h"

uchar type2cls[NTYPETAG];
uchar cls2siz[KF8+1];
const uchar siz2intcls[] = { [1] = KI4, [2] = KI4, [4] = KI4, [8] = KI8 };

const char *opnames[] = {
   "?\??",
#define _(o,...) #o,
#include "op.def"
#undef _
};

struct instr instrtab[MAXINSTR];
int ninstr;
static int instrfreelist;
struct calltab calltab;
struct phitab phitab;
struct dattab dattab;
struct addr addrht[1 << 12];
static int naddrht;
struct xcon conht[1 << 12];
static int nconht;

void
irinit(struct function *fn)
{
   static struct call callsbuf[64];
   static union ref *phisbuf[64];
   static struct irdat datsbuf[64];

   ninstr = 0;
   instrfreelist = -1;
   vinit(&calltab, callsbuf, arraylength(callsbuf));
   for (int i = 0; i < phitab.n; ++i) xbfree(phitab.p[i]);
   vinit(&phitab, phisbuf, arraylength(phisbuf));
   vinit(&dattab, datsbuf, arraylength(datsbuf));
   if (naddrht >= arraylength(addrht)/2)
      memset(addrht, naddrht = 0, sizeof addrht);
   if (nconht >= arraylength(conht)/2)
      memset(addrht, nconht = 0, sizeof addrht);
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
   fn->nblk = 1;
   memset(fn->entry, 0, sizeof *fn->entry);
   fn->entry->lprev = fn->entry->lnext = fn->entry;
}

static int
addaddr(const struct addr *addr)
{
   uint h = hashb(0, addr, sizeof *addr);
   uint i = h, n = arraylength(addrht);
   for (;; ++i) {
      i &= arraylength(addrht) - 1;
      if (!addrht[i].base.t && !addrht[i].index.t) {
         addrht[i] = *addr;
         ++naddrht;
         return i;
      } else if (!memcmp(&addrht[i], addr, sizeof *addr)) {
         return i;
      }
      assert(--n > 0 && "addrht full");
   }
}

static int
addcon(const struct xcon *con)
{
   uint h = hashb(0, con, sizeof *con);
   uint i = h, n = arraylength(conht);
   assert(con->issym || con->isdat || con->cls);
   for (;; ++i) {
      i &= arraylength(conht) - 1;
      if (!conht[i].issym && !conht[i].cls) {
         conht[i] = *con;
         ++nconht;
         return i;
      } else if (!memcmp(&conht[i], con, sizeof *con)) {
         return i;
      }
      assert(--n > 0 && "conht full");
   }
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
   case 2: wr16targ(&pdat[off], *(ushort *)src); break;
   case 4: wr32targ(&pdat[off], *(uint *)src); break;
   case 8: wr64targ(&pdat[off], *(uvlong *)src); break;
   default: assert(0);
   }
}

union irtype
mkirtype(union type t)
{
   if (t.t == TYVOID || isscalar(t)) return (union irtype) { .cls = type2cls[t.t] };
   assert(isagg(t));
   return (union irtype) { .isagg = 1, .dat = t.dat };
}

union ref
mkintcon(enum irclass k, vlong i)
{
   if (i < 1l << 28 && i >= -(1l << 28)) {
      return mkref(RICON, i);
   } else {
      struct xcon con = { .cls = k, .i = i };
      if (cls2siz[k] == 4) /* check upper half is zero or -1 */
         assert(in_range((i >> 32) + 1, 0, 1));
      return mkref(RXCON, addcon(&con));
   }
}

union ref
mkfltcon(enum irclass k, double f)
{
   struct xcon con = { .cls = k, .f = k == KF4 ? (float) f : f };
   return mkref(RXCON, addcon(&con));
}

union ref
mksymref(const char *s)
{
   struct xcon con = { .issym = 1, .sym = s };
   return mkref(RXCON, addcon(&con));
}

union ref
mkdatref(const char *name, uint siz, uint align, const void *bytes, uint n, bool deref)
{
   struct irdat dat = { .align = align, .siz = siz, .name = name };
   if (bytes) {
      if (siz <= 8) memcpy(dat.sdat, bytes, n < siz ? n : siz);
      else {
         while (((uchar *)bytes)[n-1] == 0) --n; /* nip trailing zeroes */
         if (n) vpushn(&dat.dat, bytes, n);
      }
   }
   if (!name) {
      extern const char *intern(const char *);
      char buf[32];
      struct wbuf wbuf = MEMBUF(buf, sizeof buf);

      bfmt(&wbuf, ".L.%d", dattab.n);
      ioputc(&wbuf, 0);
      assert(!wbuf.err);
      dat.name = intern(buf);
   }
   vpush(&dattab, dat);
   return mkref(RXCON, addcon(&(struct xcon){.isdat = 1, .deref = deref, .dat = dattab.n - 1}));
}

const char *
xcon2sym(int ref)
{
   struct xcon con = conht[ref];
   assert(con.issym ^ con.isdat);
   return con.issym ? con.sym : dattab.p[con.dat].name;
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
mkcallarg(union irtype ret, uint narg, int vararg)
{
   struct call call = { .ret=ret, .narg=narg, .vararg=vararg };
   assert((long) vararg <= narg);
   vpush(&calltab, call);
   return mkref(RMORE, calltab.n-1);
}

union ref
mkaddr(struct addr addr)
{
   return mkref(RMORE, addaddr(&addr));
}

static inline void
addpred(struct block *blk, struct block *p)
{
   if (blk->npred == 0) {
      blk->_pred0 = p;
      ++blk->npred;
      return;
   }
   if (blk->npred == 1) {
      struct block *p0 = blk->_pred0;
      blk->_pred = 0;
      xbgrow(&blk->_pred, 4);
      *blk->_pred = p0;
   }
   xbpush(&blk->_pred, &blk->npred, p);
}

static inline int
newinstr(void)
{
   if (instrfreelist != -1) {
      int t = instrfreelist;
      memcpy(&instrfreelist, &instrtab[t], sizeof(int));
      return t;
   }
   assert(ninstr < arraylength(instrtab));
   return ninstr++;
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
   instrfreelist = blk->ins.p[idx];
   for (int i = idx; i < blk->ins.n; ++i)
      blk->ins.p[i] = blk->ins.p[i + 1];
   --blk->ins.n;
}

/** IR builders **/

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
      ++fn->nblk;
      fn->entry->lprev = blk;
   }
   fn->curblk = blk;
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
addphi(struct function *fn, enum irclass cls, union ref *r)
{
   int new;
   struct instr ins = { Ophi, cls };
   union ref **prefs;

   vpush(&phitab, NULL);
   prefs = &phitab.p[phitab.n - 1];
   xbgrow(prefs, fn->curblk->npred);
   memcpy(*prefs, r, fn->curblk->npred * sizeof *r);
   ins.l = mkref(RMORE, phitab.n-1);
   assert(fn->curblk != NULL);
   assert(fn->curblk->ins.n == 0);
   new = newinstr();
   instrtab[new] = ins;
   vpush(&fn->curblk->phi, new);
   return mkref(RTMP, new);
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
   assert(fn->curblk && blk);
   addpred(blk, fn->curblk);
   putjump(fn, Jb, NOREF, NOREF, blk, NULL);
}

void
putcondbranch(struct function *fn, union ref arg, struct block *t, struct block *f)
{
   assert(fn->curblk && t && f);
   addpred(t, fn->curblk);
   addpred(f, fn->curblk);
   putjump(fn, Jb, arg, NOREF, t, f);
}

void
putreturn(struct function *fn, union ref r0, union ref r1)
{
   putjump(fn, Jret, r0, r1, NULL, NULL);
}

#undef putjump

/** Misc **/

void
blkreplref(struct block *blk, int i0, union ref from, union ref to)
{
   if (i0 == 0) for (int i = 0; i < blk->phi.n; ++i) {
      union ref *phi = phitab.p[instrtab[blk->phi.p[i]].l.i];
      for (int i = 0; i < blk->npred; ++i)
         if (phi[i].bits == to.bits) phi[i] = from;
   }

   for (int i = i0; i < blk->ins.n; ++i) {
      struct instr *ins = &instrtab[blk->ins.p[i]];
      for (int i = 0; i < 2; ++i) {
         union ref *r = &(&ins->l)[i];
         if (r->bits == from.bits) *r = to;
      }
   }
}

void
replref(struct function *fn, struct block *blk, int i0, union ref from, union ref to)
{
   do {
      blkreplref(blk, i0, from, to);
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
      mem2reg(fn);
      lowerintrin(fn);
      mctarg->isel(fn);
      regalloc(fn);
      if (!ccopt.dbg.any)
         mctarg->emit(fn);
   }

   freefn(fn);
}

/* vim:set ts=3 sw=3 expandtab: */
