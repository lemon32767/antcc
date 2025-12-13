#include "ir.h"
#include "../obj/obj.h"

uchar type2cls[NTYPETAG];
uchar cls2siz[KF64+1];
const uchar siz2intcls[] = { [1] = KI32, [2] = KI32, [4] = KI32, [8] = KI64 };

const char *opnames[] = {
   "?\??",
#define _(o,...) #o,
#include "op.def"
#undef _
};

const uchar opnarg[] = {
   0,
#define _(o,n) n,
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
   vinit(&calltab, callsbuf, countof(callsbuf));
   for (int i = 0; i < phitab.n; ++i) xbfree(phitab.p[i]);
   vinit(&phitab, phisbuf, countof(phisbuf));
   if (!dattab.p) vinit(&dattab, datsbuf, countof(datsbuf));
   if (naddrht >= countof(addrht)/2)
      memset(addrht, naddrht = 0, sizeof addrht);
   if (nconht >= countof(conht)/2)
      memset(conht, nconht = 0, sizeof conht);
   if (!type2cls[TYINT]) {
      for (int i = TYBOOL; i <= TYUVLONG; ++i) {
         int siz = targ_primsizes[i];
         type2cls[i] = siz < 8 ? KI32 : KI64;
      }
      type2cls[TYFLOAT] = KF32;
      type2cls[TYDOUBLE] = KF64;
      type2cls[TYLDOUBLE] = KF64;
      type2cls[TYPTR] = KPTR;
      type2cls[TYARRAY] = KPTR;
      cls2siz[KI32] = cls2siz[KF32] = 4;
      cls2siz[KI64] = cls2siz[KF64] = 8;
      cls2siz[KPTR] = targ_primsizes[TYPTR];
   }
   fn->entry = fn->curblk = allocz(fn->arena, sizeof(struct block), 0);
   fn->nblk = 1;
   fn->entry->lprev = fn->entry->lnext = fn->entry;
   fn->prop = FNUSE; /* builder keeps this */
}

static int
addaddr(const struct addr *addr)
{
   uint h = hashb(0, addr, sizeof *addr);
   uint i = h, n = countof(addrht);
   for (;; ++i) {
      i &= countof(addrht) - 1;
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
   uint i = h, n = countof(conht);
   assert((con->issym ^ con->isdat && !(con->isdat && con->isfunc)) || con->cls);
   for (;; ++i) {
      i &= countof(conht) - 1;
      if (!conht[i].issym && !conht[i].isdat && !conht[i].cls) {
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
   if (t.t == TYVOID || isscalar(t))
      return (union irtype) { .cls = type2cls[scalartypet(t)] };
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
   struct xcon con = { .cls = k, .f = k == KF32 ? (float) f : f };
   return mkref(RXCON, addcon(&con));
}

union ref
mksymref(const char *s, bool isfunc)
{
   struct xcon con = { .issym = 1, .sym = s, .isfunc = isfunc };
   return mkref(RXCON, addcon(&con));
}

union ref
mkdatref(const char *name, union type ctype, uint siz, uint align, const void *bytes, uint n, bool deref)
{
   struct irdat dat = { .ctype = ctype, .align = align, .siz = siz, .name = name, .section = Srodata };

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
   assert(vararg == -1 || (uint)vararg <= narg);
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
      blk->_pred = NULL;
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
         for (int j = 0; j < blk->phi.n; ++j) {
            union ref *phiargs = phitab.p[instrtab[blk->phi.p[j]].l.i];
            for (int k = i; k < blk->npred - 1; ++k) {
               phiargs[k] = phiargs[k + 1];
            }
         }
         for (int k = i; k < blk->npred - 1; ++k) {
            blkpred(blk, k) = blkpred(blk, k + 1);
         }
         if (--blk->npred == 1) {
            struct block *p0 = blk->_pred[0];
            xbfree(blk->_pred);
            blk->_pred0 = p0;
         }
         return;
      }
   }
   //assert(0&&"blk not in p");
}

struct block *
newblk(struct function *fn)
{
   struct block *blk = allocz(fn->arena, sizeof(struct block), 0);
   blk->id = -1;
   return blk;
}

void
freeblk(struct function *fn, struct block *blk)
{
   if (blk->npred > 1)
      xbfree(blk->_pred);
   blk->npred = 0;
   blk->_pred = NULL;

   for (int i = 0; i < blk->phi.n; ++i) {
      int ui = blk->phi.p[i];
      union ref *r = phitab.p[instrtab[ui].l.i];
      for (int j = 0; j < blk->npred; ++j) {
         deluse(blk, ui, *r);
      }
   }
   for (int i = 0; i < blk->ins.n; ++i) {
      int ui = blk->ins.p[i];
      struct instr *ins = &instrtab[ui];
      if (ins->l.t == RTMP) deluse(blk, ui, ins->l);
      if (ins->r.t == RTMP) deluse(blk, ui, ins->r);
   }
   for (int i = 0; i < 2; ++i) {
      if (blk->jmp.arg[i].t == RTMP) deluse(blk, USERJUMP, blk->jmp.arg[i]);
   }
   if (blk->s1) delpred(blk->s1, blk);
   if (blk->s2) delpred(blk->s2, blk);
   vfree(&blk->phi);
   vfree(&blk->ins);
   if (blk->id != -1)
      --fn->nblk;
   if (blk->lnext) blk->lnext->lprev = blk->lprev;
   if (blk->lprev) blk->lprev->lnext = blk->lnext;
   blk->id = 1u<<31;
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

struct block *
blksplitafter(struct function *fn, struct block *blk, int idx)
{
   struct block *new = newblk(fn);
   ++fn->nblk;
   new->lprev = blk;
   new->lnext = blk->lnext;
   blk->lnext = new;
   new->lnext->lprev = new;
   if (idx < blk->ins.n-1)
      vpushn(&new->ins, &blk->ins.p[idx+1], blk->ins.n-idx-1);
   blk->ins.n -= new->ins.n;
   new->jmp = blk->jmp;
   blk->jmp.t = Jb;
   memset(blk->jmp.arg, 0, sizeof blk->jmp.arg);
   for (int i = 0; i < 2; ++i) {
      struct block *s = (&blk->s1)[i];
      if (s) for (int i = 0; i < s->npred; ++i) {
         if (blkpred(s, i) == blk)
            blkpred(s, i) = new;
      }
   }
   new->s1 = blk->s1, new->s2 = blk->s2;
   blk->s1 = new, blk->s2 = NULL;
   addpred(new, blk);
   fn->prop &= ~FNUSE;
   return new;
}

int
allocinstr(void)
{
   int t;
   if (instrfreelist != -1) {
      t = instrfreelist;
      memcpy(&instrfreelist, &instrtab[t], sizeof(int));
   } else {
      assert(ninstr < countof(instrtab));
      t = ninstr++;
   }
   if (instruse[t] != instrusebuf[t])
      xbfree(instruse[t]);
   instruse[t] = instrusebuf[t];
   instrnuse[t] = 0;
   return t;
}

void
adduse(struct block *ublk, int ui, union ref r) {
   struct use user = { ublk, ui };

   if (r.t != RTMP) return;
   if (instrnuse[r.i] < countof(instrusebuf[r.i])) {
      instruse[r.i][instrnuse[r.i]++] = user;
   } else if (instrnuse[r.i] == countof(*instrusebuf)) {
      struct use *use = NULL;
      xbgrow(&use, countof(*instrusebuf) + 2);
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
   if (instrnuse[r.i] == countof(*instrusebuf) + 1) {
      struct use *use = instruse[r.i];
      instruse[r.i] = instrusebuf[r.i];
      memcpy(instruse[r.i], use, sizeof *instrusebuf);
      xbfree(use);
   }
   --instrnuse[r.i];
   return 1;
}

int
newinstr(struct block *at, struct instr ins)
{
   int new = allocinstr();
   instrtab[new] = ins;
   if (at) {
      adduse(at, new, ins.l);
      adduse(at, new, ins.r);
   }
   return new;
}

union ref
insertinstr(struct block *blk, int idx, struct instr ins)
{
   int new = newinstr(blk, ins);
   if (idx == blk->ins.n) vpush(&blk->ins, new);
   else {
      assert(idx >= 0 && idx < blk->ins.n);
      vpush_(&blk->ins._vb, sizeof *blk->ins.p);
      vresize(&blk->ins, blk->ins.n);
      for (int i = blk->ins.n++; i > idx; --i)
         blk->ins.p[i] = blk->ins.p[i - 1];
      blk->ins.p[idx] = new;
   }
   return mkref(RTMP, new);
}

union ref
insertphi(struct block *blk, enum irclass cls)
{
   int new = allocinstr();
   union ref *refs = NULL;
   assert(blk->npred > 0);
   xbgrowz(&refs, blk->npred);
   vpush(&phitab, refs);
   instrtab[new] = mkinstr(Ophi, cls, mkref(RXXX, phitab.n - 1));
   vpush(&blk->phi, new);
   return mkref(RTMP, new);
}

void
numberinstrs(struct function *fn)
{
   struct block *blk = fn->entry;
   int start = 0;
   do {
      blk->inumstart = start;
      start += blk->ins.n+2;
   } while ((blk = blk->lnext) != fn->entry);
}

static bool
reachablerec(struct function *fn, struct block *blk)
{
   if (blk == fn->entry) return 1;
   markvisited(blk);
   if (blk->npred == 1 && !wasvisited(blkpred(blk, 0)))
      return reachablerec(fn, blkpred(blk, 0));
   else for (int i = 0; i < blk->npred; ++i) {
      struct block *p = blkpred(blk, i);
      if (!wasvisited(p) && reachablerec(fn, p)) return 1;
   }
   return 0;
}

bool
blkreachable(struct function *fn, struct block *blk)
{
   startbbvisit();
   return reachablerec(fn, blk);
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
            --i;
            break;
         }
      }
   }
}

void
deluses(int ins)
{
   if (instruse[ins] != instrusebuf[ins]) {
      xbfree(instruse[ins]);
      instruse[ins] = instrusebuf[ins];
   }
   instrnuse[ins] = 0;
}

void
delinstr(struct block *blk, int idx)
{
   int t = blk->ins.p[idx];
   assert(idx >= 0 && idx < blk->ins.n);
   for (int i = 0; i < 2; ++i) {
      deluse(blk, t, (&instrtab[t].l)[i]);
   }
   memcpy(&instrtab[t], &instrfreelist, sizeof(int));
   instrfreelist = t;
   deluses(t);
   for (int i = idx; i < blk->ins.n - 1; ++i)
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
   for (int i = idx; i < blk->phi.n - 1; ++i)
      blk->phi.p[i] = blk->phi.p[i + 1];
   --blk->phi.n;
}

void
delnops(struct block *blk)
{
   int i, n, t;
   /* delete trailing nops */
   while (blk->ins.n > 0 && instrtab[t = blk->ins.p[blk->ins.n - 1]].op == Onop) {
      --blk->ins.n;
      deluses(t);
      memcpy(&instrtab[t], &instrfreelist, sizeof(int));
      instrfreelist = t;
   }
   /* delete rest of nops */
   for (i = blk->ins.n - 2, n = 0; i >= 0; --i) {
      if (instrtab[t = blk->ins.p[i]].op == Onop) {
         deluses(t);
         memcpy(&instrtab[t], &instrfreelist, sizeof(int));
         instrfreelist = t;
         ++n;
      } else if (n) {
         memmove(blk->ins.p+i+1, blk->ins.p+i+1+n, (blk->ins.n - n - i - 1)*sizeof *blk->ins.p);
         blk->ins.n -= n;
         n = 0;
      }
   }
   if (n)
      memmove(blk->ins.p, blk->ins.p + n, (blk->ins.n -= n)*sizeof *blk->ins.p);
}

void
fillblkids(struct function *fn)
{
   int i = 0;
   struct block *blk = fn->entry;
   do blk->id = i++; while ((blk = blk->lnext) != fn->entry);

   fn->prop |= FNBLKID;
}

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
   if (ccopt.dbg.o) {
      bfmt(ccopt.dbgout, "<< Before isel >>\n");
      irdump(fn);
   }
   mctarg->isel(fn);
   regalloc(fn);
   if (!ccopt.dbg.any)
      mctarg->emit(fn);

   freefn(fn);
}

/* vim:set ts=3 sw=3 expandtab: */
