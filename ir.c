#include "ir.h"
#include "endian.h"
#include "obj.h"

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
struct use *instruse[MAXINSTR];
short instrnuse[MAXINSTR];
static struct use instrusebuf[MAXINSTR][2];
int ninstr;
static int instrfreelist;
struct calltab calltab;
struct phitab phitab;
struct dattab dattab;
struct addr addrht[1 << 12];
static int naddrht;
struct xcon conht[1 << 12];
static int nconht;
int visitmark;

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
   if (!dattab.p) vinit(&dattab, datsbuf, arraylength(datsbuf));
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
      if (!addrht[i].base.bits && !addrht[i].index.bits) {
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
   struct irdat dat = { .align = align, .siz = siz, .name = name, .section = Srodata };

   assert(n <= siz && siz && align);
   if (!name) {
      extern const char *intern(const char *);
      char buf[32];
      struct wbuf wbuf = MEMBUF(buf, sizeof buf);

      bfmt(&wbuf, ".L.%d", dattab.n);
      ioputc(&wbuf, 0);
      assert(!wbuf.err);
      dat.name = name = intern(buf);
   }
   dat.off = objnewdat(name, dat.section, 0, siz, align);
   memcpy(objout.rodata.p+dat.off, bytes, n);
   memset(objout.rodata.p+dat.off+n, 0, siz - n);
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
   return mkref(RXXX, calltab.n-1);
}

union ref
mkaddr(struct addr addr)
{
   return mkref(RADDR, addaddr(&addr));
}

void
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

static void
delpred(struct block *blk, struct block *p)
{
   for (int i = 0; i < blk->npred; ++i) {
      if (blkpred(blk, i) == p) {
         for (int k = i; k < blk->npred - 1; ++k) {
            blkpred(blk, k) = blkpred(blk, k + 1);
         }
         --blk->npred;
         return;
      }
   }
   assert(0&&"blk not in p");
}

struct block *
insertblk(struct function *fn, struct block *pred, struct block *subst)
{
   struct block *new = newblk(fn);
   struct block **s = pred->s1 == subst ? &pred->s1 : &pred->s2;
   assert(*s == subst);
   new->lnext = pred->lnext;
   new->lprev = pred;
   pred->lnext->lprev = new;
   pred->lnext = new;
   *s = new;
   new->jmp.t = Jb;
   new->s1 = subst;
   addpred(new, pred);
   for (int i = 0; i < subst->npred; ++i) {
      if (blkpred(subst, i) == pred) {
         blkpred(subst, i) = new;
         ++fn->nblk;
         return new;
      }
   }
   assert(0);
}


static int
newinstr(void)
{
   int t;
   if (instrfreelist != -1) {
      t = instrfreelist;
      memcpy(&instrfreelist, &instrtab[t], sizeof(int));
   } else {
      assert(ninstr < arraylength(instrtab));
      t = ninstr++;
   }
   if (instrnuse[t] > arraylength(*instrusebuf))
      xbfree(instruse[t]);
   instruse[t] = instrusebuf[t];
   instrnuse[t] = 0;
   return t;
}

void
adduse(struct block *ublk, int ui, union ref r) {
   struct use user = { ublk, ui };
   if (r.t != RTMP) return;
   if (instrnuse[r.i] < arraylength(*instrusebuf)) {
      instruse[r.i][instrnuse[r.i]++] = user;
   } else if (instrnuse[r.i] == arraylength(*instrusebuf)) {
      struct use *use = NULL;
      xbgrow(&use, arraylength(*instrusebuf) + 2);
      memcpy(use, instruse[r.i], sizeof(*instrusebuf));
      use[instrnuse[r.i]++] = user;
      instruse[r.i] = use;
   } else {
      xbpush(&instruse[r.i], &instrnuse[r.i], user);
   }
}

bool
deluse(struct block *ublk, int ui, union ref r) {
   if (r.t != RTMP) return 0;

   for (int i = 0; i < instrnuse[r.i]; ++i) {
      if (instruse[r.i][i].blk == ublk && instruse[r.i][i].u == ui) {
         for (int k = i; k < instrnuse[r.i] - 1; ++k) {
            instruse[r.i][k] = instruse[r.i][k+1];
         }
         goto Shrink;
      }
   }
   return 0;

Shrink:
   if (instrnuse[r.i] == arraylength(*instrusebuf) + 1) {
      struct use *use = instruse[r.i];
      instruse[r.i] = instrusebuf[r.i];
      memcpy(instruse[r.i], use, sizeof *instrusebuf);
   }
   --instrnuse[r.i];
   return 1;
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
   adduse(blk, new, ins.l);
   adduse(blk, new, ins.r);
   return mkref(RTMP, new);
}

union ref
insertphi(struct block *blk, enum irclass cls)
{
   int new = newinstr();
   union ref *refs = NULL;
   assert(blk->npred > 0);
   xbgrow(&refs, blk->npred);
   memset(refs, 0, blk->npred * sizeof *refs);
   vpush(&phitab, refs);
   instrtab[new] = mkinstr(Ophi, cls, mkref(RXXX, phitab.n - 1));
   vpush(&blk->phi, new);
   return mkref(RTMP, new);
}

/* require use */
void
replcuses(union ref from, union ref to)
{
   assert(from.t == RTMP);
   for (int i = 0; i < instrnuse[from.i]; ++i) {
      struct use use = instruse[from.i][i];
      union ref *u;
      int n, j;
      if (use.u == from.i) continue;
      if (use.u == USERJUMP) {
         u = &use.blk->jmp.arg[0];
         n = 2;
      } else if (instrtab[use.u].op == Ophi) {
         u = phitab.p[instrtab[use.u].l.i];
         n = use.blk->npred;
      } else {
         u = &instrtab[use.u].l;
         n = 2;
      }
      
      for (j = 0; j < n; ++j) {
         if (u[j].bits == from.bits) {
            u[j].bits = to.bits;
            adduse(use.blk, use.u, to);
            deluse(use.blk, use.u, from);
            --i;
            break;
         }
      }
   }
}

void
deluses(int ins)
{
   if (instrnuse[ins] > 2)
      xbfree(instruse[ins]);
   instrnuse[ins] = 0;
}

void
delinstr(struct block *blk, int idx)
{
   int t = blk->ins.p[idx];
   assert(idx >= 0 && idx < blk->ins.n);
   memcpy(&instrtab[t], &instrfreelist, sizeof(int));
   instrfreelist = t;
   deluses(t);
   for (int i = idx; i < blk->ins.n; ++i)
      blk->ins.p[i] = blk->ins.p[i + 1];
   --blk->ins.n;
}

void
delphi(struct block *blk, int idx)
{
   int t = blk->phi.p[idx];
   assert(idx >= 0 && idx < blk->phi.n);
   memcpy(&instrtab[t], &instrfreelist, sizeof(int));
   instrfreelist = t;
   deluses(t);
   for (int i = idx; i < blk->phi.n; ++i)
      blk->phi.p[i] = blk->phi.p[i + 1];
   --blk->phi.n;
}

void
fillblkids(struct function *fn)
{
   int i = 0;
   struct block *blk = fn->entry;
   do blk->id = i++; while ((blk = blk->lnext) != fn->entry);
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
   adduse(fn->curblk, new, ins.l);
   adduse(fn->curblk, new, ins.r);
   vpush(&fn->curblk->ins, new);
   return mkref(RTMP, new);
}

union ref
addphi(struct function *fn, enum irclass cls, union ref *r)
{
   int new;
   struct instr ins = { Ophi, cls };
   union ref *refs = NULL;

   xbgrow(&refs, fn->curblk->npred);
   memcpy(refs, r, fn->curblk->npred * sizeof *r);
   vpush(&phitab, refs);
   ins.l = mkref(RXXX, phitab.n-1);

   assert(fn->curblk != NULL);
   assert(fn->curblk->ins.n == 0);
   new = newinstr();
   instrtab[new] = ins;
   for (int i = 0; i < fn->curblk->npred; ++i) {
      adduse(fn->curblk, new, r[i]);
   }
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
   adduse(fn->curblk, USERJUMP, arg);
   addpred(t, fn->curblk);
   addpred(f, fn->curblk);
   putjump(fn, Jb, arg, NOREF, t, f);
}

void
putreturn(struct function *fn, union ref r0, union ref r1)
{
   adduse(fn->curblk, USERJUMP, r0);
   adduse(fn->curblk, USERJUMP, r1);
   putjump(fn, Jret, r0, r1, NULL, NULL);
}

#undef putjump

/** Misc **/

static void
freefn(struct function *fn)
{
   struct block *blk = fn->entry;
   do {
      if (blk->npred > 1) xbfree(blk->_pred);
      vfree(&blk->phi);
      vfree(&blk->ins);
   } while ((blk = blk->lnext) != fn->entry);
}

void
irfini(struct function *fn)
{
   extern int nerror;
   if (nerror) {
      freefn(fn);
      return;
   }

   abi0(fn);
   lowerintrin(fn);
   mem2reg(fn);
   copyopt(fn);
   mctarg->isel(fn);
   regalloc(fn);
   if (!ccopt.dbg.any)
      mctarg->emit(fn);

   freefn(fn);
}

/* vim:set ts=3 sw=3 expandtab: */
