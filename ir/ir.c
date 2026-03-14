#include "ir.h"
#include "../obj/obj.h"

uchar type2cls[NTYPETAG];
uchar cls2siz[] = { [KI32] = 4, [KI64] = 8, [KF32] = 4, [KF64] = 8 };
uchar cls2load[] = {
   [KI32] = Oloadu32, [KI64] = Oloadi64,
   [KF32] = Oloadf32, [KF64] = Oloadf64, [KPTR] = -1
}, cls2store[] = {
   [KI32] = Ostorei32, [KI64] = Ostorei64,
   [KF32] = Ostoref32, [KF64] = Ostoref64, [KPTR] = -1
};
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
int ninstr;
static int instrfreelist;
static struct use *usefreelist;
static struct arena **usearena;
struct calltab calltab;
struct phitab phitab;
struct dattab dattab;
struct contab contab;
struct addrtab addrtab;
static ushort *addrht;
static int naddrht;
int visitmark;

void
irinit(struct function *fn)
{
   static struct call callsbuf[64];
   static union ref *phisbuf[64];
   static struct irdat datsbuf[64];
   static struct xcon consbuf[64];
   static struct addr addrsbuf[64];

   assert(fn->arena && !fn->passarena);

   ninstr = 0;
   instrfreelist = -1;
   usefreelist = NULL;
   usearena = fn->arena;
   vinit(&calltab, callsbuf, countof(callsbuf));
   for (int i = 0; i < phitab.n; ++i) xbfree(phitab.p[i]);
   vinit(&phitab, phisbuf, countof(phisbuf));
   vinit(&contab, consbuf, countof(consbuf));
   if (!dattab.p) vinit(&dattab, datsbuf, countof(datsbuf));
   naddrht = 128;
   if (!addrht) xbgrowz(&addrht, naddrht);
   else if (addrtab.n) memset(addrht, 0, xbcap(addrht) * sizeof *addrht);
   vinit(&addrtab, addrsbuf, countof(addrsbuf));
   
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
      cls2siz[KPTR] = targ_primsizes[TYPTR];
      cls2load[KPTR] = targ_64bit ? Oloadi64 : Oloads32;
      cls2store[KPTR] = targ_64bit ? Ostorei64 : Ostorei32;
   }
   fn->entry = fn->curblk = allocz(fn->arena, sizeof(struct block), 0);
   fn->nblk = 1;
   fn->entry->lprev = fn->entry->lnext = fn->entry;
   fn->prop = FNUSE; /* builder keeps this */
}

static int
newaddr(const struct addr *addr)
{
   if (addrtab.n >= naddrht/4*3 /*75% load factor */) {
      xbgrowz(&addrht, naddrht*2);
      memset(addrht, 0, naddrht * sizeof *addrht);
      naddrht *= 2;
      for (int i = 0; i < addrtab.n; ++i) { /* rehash */
         const struct addr *addr = &addrtab.p[i];
         for (uint h = hashb(0, addr, sizeof *addr), j = h;; ++j) {
            if (!addrht[j &= naddrht - 1]) {
               addrht[j] = i+1;
               break;
            }
         }
      }
   }
   for (uint h = hashb(0, addr, sizeof *addr), i = h;; ++i) {
      i &= naddrht - 1;
      if (!addrht[i]) {
         vpush(&addrtab, *addr);
         return (addrht[i] = addrtab.n) - 1;
      } else if (!memcmp(&addrtab.p[addrht[i]-1], addr, sizeof *addr)) {
         return addrht[i] - 1;
      }
   }
}

union ref
newxcon(const struct xcon *con)
{
   assert((con->issym ^ con->isdat) || con->cls);
   vpush(&contab, *con);
   return mkref(RXCON, contab.n-1);
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
      return newxcon(&con);
   }
}

union ref
mkfltcon(enum irclass k, double f)
{
   struct xcon con = { .cls = k, .f = k == KF32 ? (float) f : f };
   return newxcon(&con);
}

union ref
mksymref(internstr s, enum symflags symflags)
{
   struct xcon con = { .issym = 1, .sym = s, .flag = symflags };
   return newxcon(&con);
}

union ref
mkdatref(internstr name, union type ctype, uint siz, uint align,
         const void *bytes, uint n, bool deref, bool funclocal)
{
   struct irdat dat = { .ctype = ctype, .align = align, .siz = siz, .name = name, .section = Srodata };
   if (funclocal && objout.code && align >= 4 && align <= targ_primsizes[TYPTR] && siz <= 16)
      dat.section = Stext;

   assert(n <= siz && siz && align);
   if (!name) {
      char buf[32];
      struct wbuf wbuf = MEMBUF(buf, sizeof buf);

      bfmt(&wbuf, ".L%c.%d", dat.section == Stext ? 'L' : 'D', dattab.n);
      ioputc(&wbuf, 0);
      assert(!wbuf.err);
      dat.name = name = intern(buf);
   }
   dat.off = objnewdat(name, dat.section, 0, siz, align);
   uchar *p = (dat.section == Stext ? objout.textbegin : objout.rodata.p) + dat.off;
   if (n) memcpy(p, bytes, n);
   if (dat.section != Stext) memset(p+n, 0, siz - n);
   vpush(&dattab, dat);
   return newxcon(&(struct xcon){.isdat = 1, .deref = deref, .dat = dattab.n - 1, .flag = SLOCAL});
}

internstr
xcon2sym(int ref)
{
   struct xcon con = contab.p[ref];
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
   return mkref(RADDR, newaddr(&addr));
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

void
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
   if (blk->lprev) blk->lprev->lnext = blk->lnext;
   if (blk->lnext) blk->lnext->lprev = blk->lprev;
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
   if (idx == -1) {
      new->ins = blk->ins;
      memset(&blk->ins, 0, sizeof blk->ins);
   } else {
      if (idx < blk->ins.n-1)
         vpushn(&new->ins, &blk->ins.p[idx+1], blk->ins.n-idx-1);
      blk->ins.n -= new->ins.n;
   }
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
      if (instruse[t]) deluses(t);
   } else {
      assert(ninstr < countof(instrtab));
      t = ninstr++;
      instruse[t] = NULL;
   }
   return t;
}

void
adduse(struct block *ublk, int ui, union ref r) {
   if (r.t != RTMP) return;
   struct use *use; 
   if (usefreelist) {
      use = usefreelist;
      usefreelist = usefreelist->next;
   } else {
      use = alloc(usearena, sizeof *use, 0);
   }
   assert(use != instruse[r.i]);
   use->next = instruse[r.i];
   use->blk = ublk,
   use->u = ui;
   instruse[r.i] = use;
}

bool
deluse(struct block *ublk, int ui, union ref r) {
   if (r.t != RTMP) return 0;

   for (struct use **puse = &instruse[r.i]; *puse; puse = &(*puse)->next) {
      struct use *use = *puse;
      if (use->blk == ublk && use->u == ui) {
         *puse = use->next;
         use->blk = 0;
         use->u = 0;
         use->next = usefreelist;
         usefreelist = use;
         return 1;
      }
   }
   return 0;
}

void
filluses(struct function *fn)
{
   struct block *blk = fn->entry;

   for (int i = 0; i < ninstr; ++i)
      deluses(i);

   do {
      for (int i = 0; i < blk->phi.n; ++i) {
         int ins = blk->phi.p[i];
         union ref *phi = phitab.p[instrtab[ins].l.i];
         for (int i = 0; i < blk->npred; ++i)
            adduse(blk, ins, phi[i]);
      }
      for (int i = 0; i < blk->ins.n; ++i) {
         int ins = blk->ins.p[i];
         adduse(blk, ins, instrtab[ins].l);
         adduse(blk, ins, instrtab[ins].r);
      }
      adduse(blk, USERJUMP, blk->jmp.arg[0]);
      adduse(blk, USERJUMP, blk->jmp.arg[1]);
   } while ((blk = blk->lnext) != fn->entry);

   fn->prop |= FNUSE;
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
   for (struct use *use = instruse[from.i], *next; use; use = next) {
      union ref *u;
      int n, j;
      next = use->next;
      if (use->u == from.i) continue;
      if (use->u == USERJUMP) {
         u = &use->blk->jmp.arg[0];
         n = 2;
      } else if (instrtab[use->u].op == Ophi) {
         u = phitab.p[instrtab[use->u].l.i];
         n = use->blk->npred;
         if (use->blk->phi.n == 0) continue; /* shouldn't happen */
      } else {
         u = &instrtab[use->u].l;
         n = 2;
      }
      
      for (j = 0; j < n; ++j) {
         if (u[j].bits == from.bits) {
            u[j].bits = to.bits;
            adduse(use->blk, use->u, to);
            next = use;
            break;
         }
      }
   }
}

void
deluses(int ins)
{
   for (struct use *use = instruse[ins], *next; use; use = next) {
      next = use->next;
      use->blk = 0;
      use->u = 0;
      use->next = usefreelist;
      usefreelist = use;
   }
   instruse[ins] = NULL;
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
   static union { char m[sizeof(struct arena) + (4<<10)]; struct arena *_align; } amem;
   struct arena *passarena = (void *)&amem.m;
   fn->passarena = &passarena;
   if (nerror) {
      freefn(fn);
      return;
   }

   abi0(fn);
   lowerintrin(fn);
   if (ccopt.o > OPT0) {
      mem2reg(fn);
      freearena(fn->passarena);
      copyopt(fn);
   }
   if (ccopt.o >= OPT1) {
      doinline(fn);
      filldom(fn);
      if (!(fn->prop & FNUSE)) filluses(fn);
      cselim(fn);
      freearena(fn->passarena);
      simpl(fn);
      freearena(fn->passarena);
   }
   if (maybeinlinee(fn)) {
      // goto Fin; XXX do this by having inline function rematerialization when symbol is actually referenced
   }
   lowerstack(fn);
   freearena(fn->passarena);
   if (ccopt.dbg.o) {
      bfmt(ccopt.dbgout, "<< Before isel >>\n");
      irdump(fn);
   }
   mctarg->isel(fn);
   regalloc(fn);
   freearena(fn->passarena);
   if (objout.code)
      mctarg->emit(fn);

//Fin:
   freearena(fn->passarena);
   freefn(fn);
}

/* vim:set ts=3 sw=3 expandtab: */
