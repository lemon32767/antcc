#include "ir.h"
#include "obj.h"
#include "u_hash.h"

uchar type2cls[NTYPETAG];
uchar cls2siz[] = {
   [KI32] = 4, [KI64] = 8, [KF32] = 4, [KF64] = 8,
   [KMI8] = 1, [KMI16] = 2
};
uchar cls2load[] = {
   [KI32] = Oloadu32, [KI64] = Oloadi64,
   [KF32] = Oloadf32, [KF64] = Oloadf64, [KPTR] = -1
}, cls2store[] = {
   [KI32] = Ostorei32, [KI64] = Ostorei64,
   [KF32] = Ostoref32, [KF64] = Ostoref64, [KPTR] = -1
};
const uchar siz2intcls[] = { [1] = KI32, [2] = KI32, [4] = KI32, [8] = KI64 };
const uchar _loadstoreszcls[] = {
   [Oloads8  - Oloads8] = 1|KI32<<4, [Oloadu8  - Oloads8] = 1|KI32<<4,
   [Oloads16 - Oloads8] = 2|KI32<<4, [Oloadu16 - Oloads8] = 2|KI32<<4,
   [Oloads32 - Oloads8] = 4|KI32<<4, [Oloadu32 - Oloads8] = 4|KI32<<4,
   [Oloadi64 - Oloads8] = 8|KI64<<4,
   [Oloadf32 - Oloads8] = 4|KF32<<4,
   [Oloadf64 - Oloads8] = 8|KF64<<4,
   [Ostorei8  - Oloads8] = 1|KI32<<4, [Ostorei16 - Oloads8] = 2|KI32<<4,
   [Ostorei32 - Oloads8] = 4|KI32<<4, [Ostorei64 - Oloads8] = 8|KI64<<4,
   [Ostoref32 - Oloads8] = 4|KF32<<4, [Ostoref64 - Oloads8] = 8|KF64<<4,
};
const uchar _load2ext[] = {
   [Oloads8  - Oloads8] = Oexts8,  [Oloadu8  - Oloads8] = Oextu8,
   [Oloads16 - Oloads8] = Oexts16, [Oloadu16 - Oloads8] = Oextu16,
   [Oloads32 - Oloads8] = Oexts32, [Oloadu32 - Oloads8] = Oextu32,
   [Oloadi64 - Oloads8] = Ocopy,
};

const char *opnames[] = {
   "?\??",
#define _(o,...) #o,
#include "ir_op.def"
#undef _
};

const uchar opnoper[] = {
   0,
#define _(o,n) n,
#include "ir_op.def"
#undef _
};

Instr instrtab[MAXINSTR];
IRUse *instruse[MAXINSTR];
int ninstrtab, nfreeinstr;
static int instrfreelist;
static IRUse *usefreelist;
static Arena **usearena;
struct calltab calltab;
struct phitab phitab;
struct dattab dattab;
struct contab contab;
struct addrtab addrtab;
static ushort *addrht;
static int naddrht;
int visitmark;

void
irinit(Function *fn) {
   static IRCall callsbuf[64];
   static Ref *phisbuf[64];
   static IRDat datsbuf[64];
   static IRCon consbuf[64];
   static IRAddr addrsbuf[64];

   assert(fn->arena && !fn->passarena);

   ninstrtab = 0;
   nfreeinstr = 0;
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
   fn->entry = fn->curblk = allocz(fn->arena, sizeof(Block), 0);
   fn->nblk = 1;
   fn->entry->lprev = fn->entry->lnext = fn->entry;
   fn->prop = FNUSE; /* builder keeps this */
}

static int
newaddr(const IRAddr *addr) {
   if (addrtab.n >= naddrht/4*3 /*75% load factor */) {
      xbgrowz(&addrht, naddrht*2);
      memset(addrht, 0, naddrht * sizeof *addrht);
      naddrht *= 2;
      for (int i = 0; i < addrtab.n; ++i) { /* rehash */
         const IRAddr *addr = &addrtab.p[i];
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

Ref
newxcon(const IRCon *con) {
   assert((con->issym ^ con->isdat) || con->cls);
   vpush(&contab, *con);
   return mkref(RXCON, contab.n-1);
}

IRType
mkirtype(Type t) {
   if (iscomplex(t)) t = complex2struct(t);
   if (t.t == TYVOID || isscalar(t))
      return (IRType) { .cls = type2cls[scalartypet(t)] };
   assert(isagg(t));
   return (IRType) { .isagg = 1, .dat = t.dat };
}

Ref
mkintcon(enum irclass k, s64int i) {
   if (cls2siz[k] == 4) { /* check upper half is zero or -1 */
      assert(in_range((i >> 32) + 1, 0, 1));
      i = (int)i;
   }
   if (i < 1l << 28 && i >= -(1l << 28)) {
      return mkref(RICON, i);
   } else {
      IRCon con = { .cls = k, .i = i };
      return newxcon(&con);
   }
}

Ref
mkfltcon(enum irclass k, double f) {
   IRCon con = { .cls = k, .f = k == KF32 ? (float) f : f };
   return newxcon(&con);
}

Ref
mksymref(internstr s, enum symflags symflags) {
   IRCon con = { .issym = 1, .sym = s, .flag = symflags };
   return newxcon(&con);
}

static bool
textdataok(void) {
   /* openbsd enforces R^X for .text */
   return target.os != OSopenbsd;
}

Ref
mkdatref(internstr name, Type ctype, uint siz, uint align,
         const void *bytes, uint n, bool deref, bool funclocal) {
   IRDat dat = { .ctype = ctype, .align = align, .siz = siz, .name = name, .section = Srodata };

   if (funclocal && textdataok() && objout.code
    && align >= 4 && align <= targ_primsizes[TYPTR] && siz <= 16)
      dat.section = Stext;

   assert(n <= siz && siz && align);
   if (!name) {
      char buf[32];
      WriteBuf wbuf = MEMBUF(buf, sizeof buf);

      bfmt(&wbuf, ".L%c.%d", dat.section == Stext ? 'L' : 'D', dattab.n);
      ioputc(&wbuf, 0);
      assert(!wbuf.err);
      dat.name = name = intern(buf);
   }
   dat.off = objnewdat(name, SLOCAL, dat.section, siz, align);
   uchar *p = (dat.section == Stext ? objout.textbegin : objout.rodata.p) + dat.off;
   if (n) memcpy(p, bytes, n);
   if (dat.section != Stext) memset(p+n, 0, siz - n);
   vpush(&dattab, dat);
   return newxcon(&(IRCon){.isdat = 1, .deref = deref, .dat = dattab.n - 1, .flag = SLOCAL});
}

internstr
xcon2sym(int ref) {
   IRCon con = contab.p[ref];
   assert(con.issym ^ con.isdat);
   return con.issym ? con.sym : dattab.p[con.dat].name;
}

Instr
mkalloca(uint siz, uint align) {
   Instr ins = { .cls = KPTR };
   assert(ispo2(align) && align <= 16);
   ins.op = Oalloca1 + ilog2(align);
   ins.l = mkref(RICON, siz/align + (siz%align != 0));
   return ins;
}

Ref
mkcallarg(IRType ret, uint narg, int vararg) {
   IRCall call = { .ret=ret, .narg=narg, .vararg=vararg };
   assert(vararg == -1 || (uint)vararg <= narg);
   vpush(&calltab, call);
   return mkref(RXXX, calltab.n-1);
}

Ref
mkaddr(IRAddr addr) {
   return mkref(RADDR, newaddr(&addr));
}

void
addpred(Block *blk, Block *p) {
   if (blk->npred == 0) {
      blk->_pred0 = p;
      ++blk->npred;
      return;
   }
   if (blk->npred == 1) {
      Block *p0 = blk->_pred0;
      blk->_pred = NULL;
      xbgrow(&blk->_pred, 4);
      *blk->_pred = p0;
   }
   xbpush(&blk->_pred, &blk->npred, p);
}

int
delpred(Block *blk, Block *p) {
   for (int i = 0; i < blk->npred; ++i) {
      if (blkpred(blk, i) == p) {
         for (int j = 0; j < blk->phi.n; ++j) {
            int phi = blk->phi.p[j];
            Ref *args = phiargs(phi);
            deluse(blk, phi, args[i]);
            for (int k = i; k < blk->npred - 1; ++k) {
               args[k] = args[k + 1];
            }
         }
         for (int k = i; k < blk->npred - 1; ++k) {
            blkpred(blk, k) = blkpred(blk, k + 1);
         }
         if (--blk->npred == 1) {
            Block *p0 = blk->_pred[0];
            xbfree(blk->_pred);
            blk->_pred0 = p0;
         }
         return i;
      }
   }
   //assert(0&&"blk not in p");
   return -1;
}

enum {
   BLKUNINIT  = -(1<<30),
   BLKDELETED = (1<<30),
};

Block *
newblk(Function *fn) {
   Block *blk = allocz(fn->arena, sizeof(Block), 0);
   blk->id = BLKUNINIT;
   return blk;
}

void
freeblk(Function *fn, Block *blk) {
   if (fn->prop & FNUSE) {
      for (int i = 0; i < blk->phi.n; ++i) {
         int ui = blk->phi.p[i];
         Ref *r = phiargs(ui);
         for (int j = 0; j < blk->npred; ++j) {
            deluse(blk, ui, r[j]);
         }
      }
      for (int i = 0; i < blk->ins.n; ++i) {
         int ui = blk->ins.p[i];
         Instr *ins = &instrtab[ui];
         if (ins->l.t == RTMP) deluse(blk, ui, ins->l);
         if (ins->r.t == RTMP) deluse(blk, ui, ins->r);
      }
      for (int i = 0; i < 2; ++i) {
         if (blk->jmp.arg[i].t == RTMP) deluse(blk, USERJUMP, blk->jmp.arg[i]);
      }
   }
   if (blk->npred > 1)
      xbfree(blk->_pred);
   blk->npred = 0;
   blk->_pred = NULL;
   if (blk->s1) delpred(blk->s1, blk);
   if (blk->s2) delpred(blk->s2, blk);
   vfree(&blk->phi);
   vfree(&blk->ins);
   if (blk->id != BLKUNINIT)
      --fn->nblk;
   if (blk->lprev) blk->lprev->lnext = blk->lnext;
   if (blk->lnext) blk->lnext->lprev = blk->lprev;
   blk->id = BLKDELETED;
}

Block *
insertblk(Function *fn, Block *pred, Block *subst) {
   Block *new = newblk(fn);
   Block **s = pred->s1 == subst ? &pred->s1 : &pred->s2;
   assert(*s == subst);
   new->lnext = pred->lnext;
   new->lprev = pred;
   pred->lnext->lprev = new;
   pred->lnext = new;
   *s = new;
   new->jmp.t = Jb;
   new->s1 = subst;
   new->loop = pred->loop;
   new->loopdepth = pred->loopdepth;
   new->idom = pred;
   if (subst->idom == pred) subst->idom = new;
   addpred(new, pred);
   for (int i = 0; i < subst->npred; ++i) {
      if (blkpred(subst, i) == pred) {
         blkpred(subst, i) = new;
         ++fn->nblk;
         new->id = -fn->nblk;
         return new;
      }
   }
   assert(0);
}

Block *
blksplitafter(Function *fn, Block *blk, int idx) {
   Block *new = newblk(fn);
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
      Block *s = (&blk->s1)[i];
      if (s) for (int i = 0; i < s->npred; ++i) {
         if (blkpred(s, i) == blk)
            blkpred(s, i) = new;
      }
   }
   new->s1 = blk->s1, new->s2 = blk->s2;
   new->id = -fn->nblk;
   blk->s1 = new, blk->s2 = NULL;
   addpred(new, blk);
   fn->prop &= ~FNUSE;
   return new;
}

int
allocinstr(void) {
   int t;
   if (instrfreelist != -1) {
      t = instrfreelist;
      instrfreelist = instrtab[t].l.i;
      instrtab[t].l = mkref(RXXX, 0);
      --nfreeinstr;
      if (instruse[t]) deluses(t);
   } else {
      assert(ninstrtab < countof(instrtab));
      t = ninstrtab++;
      instruse[t] = NULL;
   }
   return t;
}

static inline void
freeinstr(int t) {
   instrtab[t].op = Oxxx;
   instrtab[t].l = mkref(RXXX, instrfreelist);
   instrfreelist = t;
   ++nfreeinstr;
}

void
adduse(Block *ublk, int ui, Ref r) {
   if (r.t != RTMP) return;
   IRUse *use;
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
deluse(Block *ublk, int ui, Ref r) {
   if (r.t != RTMP) return 0;

   if (!ublk) assert(ui != USERJUMP);
   for (IRUse **puse = &instruse[r.i]; *puse; puse = &(*puse)->next) {
      IRUse *use = *puse;
      if ((!ublk || use->blk == ublk) && use->u == ui) {
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
filluses(Function *fn) {
   Block *blk = fn->entry;

   for (int i = 0; i < ninstrtab; ++i)
      deluses(i);

   do {
      for (int i = 0; i < blk->phi.n; ++i) {
         int ins = blk->phi.p[i];
         Ref *phi = phiargs(ins);
         for (int i = 0; i < blk->npred; ++i)
            adduse(blk, ins, phi[i]);
      }
      for (int i = 0; i < blk->ins.n; ++i) {
         int ins = blk->ins.p[i];
         for (int i = 0; i < opnoper[instrtab[ins].op]; ++i)
            adduse(blk, ins, instrtab[ins].oper[i]);
      }
      adduse(blk, USERJUMP, blk->jmp.arg[0]);
      adduse(blk, USERJUMP, blk->jmp.arg[1]);
   } while ((blk = blk->lnext) != fn->entry);

   fn->prop |= FNUSE;
}


void
inschoper(Block *b, Instr *ins, int i, Ref r) {
   int t = ins - instrtab;
   Ref *o = &ins->oper[i];
   deluse(b, t, *o);
   adduse(b, t, r);
   *o = r;
}

int
newinstr(Block *at, Instr ins) {
   int new = allocinstr();
   instrtab[new] = ins;
   if (at) {
      for (int i = 0; i < opnoper[ins.op]; ++i)
         adduse(at, new, ins.oper[i]);
   }
   return new;
}

Ref
insertinstr(Block *blk, int idx, Instr ins) {
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

Ref
insertphi(Block *blk, enum irclass cls) {
   int new = allocinstr();
   Ref *refs = NULL;
   assert(blk->npred > 0);
   xbgrowz(&refs, blk->npred);
   vpush(&phitab, refs);
   instrtab[new] = mkinstr1(Ophi, cls, {.i = phitab.n - 1});
   vpush(&blk->phi, new);
   return mkref(RTMP, new);
}

uint
numberinstrs(Function *fn) {
   Block *blk = fn->entry;
   int start = 0;
   do {
      blk->inumstart = start;
      start += blk->ins.n+2;
   } while ((blk = blk->lnext) != fn->entry);
   return start-1;
}

static bool
reachablerec(Function *fn, Block *blk) {
   if (blk == fn->entry) return 1;
   markvisited(blk);
   if (blk->npred == 1 && !wasvisited(blkpred(blk, 0)))
      return reachablerec(fn, blkpred(blk, 0));
   else for (int i = 0; i < blk->npred; ++i) {
      Block *p = blkpred(blk, i);
      if (!wasvisited(p) && reachablerec(fn, p)) return 1;
   }
   return 0;
}

bool
blkreachable(Function *fn, Block *blk) {
   startbbvisit();
   return reachablerec(fn, blk);
}

/* require use */
void
replcuses(Ref from, Ref to, Block *at, enum replcusesmode m) {
   assert(from.t == RTMP);
   assert(m == REPLC_ALL || at != NULL);
   for (IRUse *use, *next = instruse[from.i]; (use = next);) {
      Ref *u;
      int n;
      next = use->next;
      if (use->u == from.i) continue;
      if (m == REPLC_AT && use->blk != at) continue;
      bool isphi = 0;
      if (use->u == USERJUMP) {
         u = &use->blk->jmp.arg[0];
         n = 2;
      } else if (!instrtab[use->u].op) { /* dead */
         continue;
      } else if (instrtab[use->u].op == Ophi) {
         u = phiargs(use->u);
         n = use->blk->npred;
         if (use->blk->phi.n == 0) continue; /* shouldn't happen */
         isphi = 1;
      } else {
         u = instrtab[use->u].oper;
         n = opnoper[instrtab[use->u].op];
      }
      if (m == REPLC_DOM && !isphi && !dominates(at, use->blk)) continue;

      for (int j = 0; j < n; ++j) {
         if (u[j].bits != from.bits) continue;
         if (m == REPLC_DOM && isphi && !dominates(at, blkpred(use->blk, j))) {
            /* for phis, the corresponding predecessor is what actually consumes
             * the value, so check that block for domtree-based replacement */
            continue;
         }
         u[j].bits = to.bits;
         adduse(use->blk, use->u, to);
         deluse(use->blk, use->u, from);
         break;
      }
   }
}

void
deluses(int ins) {
   for (IRUse *use = instruse[ins], *next; use; use = next) {
      next = use->next;
      use->blk = 0;
      use->u = 0;
      use->next = usefreelist;
      usefreelist = use;
   }
   instruse[ins] = NULL;
}

void
moveinstr(Function *fn, Block *srcb, int srci, Block *tob, int toi) {
   int t = srcb->ins.p[srci];
   Instr *ins = &instrtab[t];
   if (toi == -1)
      vpush(&tob->ins, t);
   else
      vinsert(&tob->ins, toi, t);

   for (int i = srci; i < srcb->ins.n - 1; ++i)
      srcb->ins.p[i] = srcb->ins.p[i + 1];
   --srcb->ins.n;
   if (fn->prop & FNUSE) {
      /* fixup uselist for ins' operands */
      for (int oi = 0; oi < opnoper[ins->op]; oi++) {
         if (ins->oper[oi].t != RTMP) continue;
         int usee = ins->oper[oi].i;
         for (IRUse *use = instruse[usee]; use; use = use->next) {
            if (use->u == t) {
               assert(use->blk == srcb || use->blk == tob);
               use->blk = tob;
            }
         }
      }
   }
}

void
delinstr(Block *blk, int idx) {
   int t = blk->ins.p[idx];
   assert(idx >= 0 && idx < blk->ins.n);
   for (int i = 0; i < 2; ++i) {
      deluse(blk, t, (&instrtab[t].l)[i]);
   }
   freeinstr(t);
   deluses(t);
   for (int i = idx; i < blk->ins.n - 1; ++i)
      blk->ins.p[i] = blk->ins.p[i + 1];
   --blk->ins.n;
}

void
delphi(Block *blk, int idx) {
   int t = blk->phi.p[idx];
   assert(idx >= 0 && idx < blk->phi.n);
   Ref *args = phiargs(t);
   for (int i = 0; i < blk->npred; ++i)
      deluse(NULL, t, args[i]);
   freeinstr(t);
   deluses(t);
   for (int i = idx; i < blk->phi.n - 1; ++i)
      blk->phi.p[i] = blk->phi.p[i + 1];
   --blk->phi.n;
}

void
delnops(Block *blk) {
   int i, n, t;
   /* delete trailing nops */
   while (blk->ins.n > 0 && instrtab[t = blk->ins.p[blk->ins.n - 1]].op == Onop) {
      --blk->ins.n;
      deluses(t);
      freeinstr(t);
   }
   /* delete rest of nops */
   for (i = blk->ins.n - 2, n = 0; i >= 0; --i) {
      if (instrtab[t = blk->ins.p[i]].op == Onop) {
         deluses(t);
         freeinstr(t);
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
fillblkids(Function *fn) {
   int i = 0;
   Block *blk = fn->entry;
   do blk->id = i++; while ((blk = blk->lnext) != fn->entry);

   fn->prop |= FNBLKID;
}

/** Misc **/

static void
freefn(Function *fn) {
   Block *blk = fn->entry;
   do {
      if (blk->npred > 1) xbfree(blk->_pred);
      vfree(&blk->phi);
      vfree(&blk->ins);
   } while ((blk = blk->lnext) != fn->entry);
}

static void
pass(Function *fn, const char *pname, void p(Function *)) {
   if (dumpbefore(&fn->name->c, pname)) {
      bfmt(ccopt.dbg.out, "<< Before %s >>\n", pname);
      irdump(fn);
   }
   p(fn);
   if (dumpafter(&fn->name->c, pname)) {
      bfmt(ccopt.dbg.out, "<< After %s >>\n", pname);
      irdump(fn);
   }
}

static int
ipass(Function *fn, const char *pname, int p(Function *), int n) {
   if (dumpbefore(&fn->name->c, pname)) {
      bfmt(ccopt.dbg.out, "<< Before %s ", pname);
      if (n) bfmt(ccopt.dbg.out, "#%d ", n+1);
      bfmt(ccopt.dbg.out, ">>\n");
      irdump(fn);
   }
   int r = p(fn);
   if (dumpafter(&fn->name->c, pname)) {
      bfmt(ccopt.dbg.out, "<< After %s ", pname);
      if (n) bfmt(ccopt.dbg.out, "#%d ", n+1);
      bfmt(ccopt.dbg.out, ">>\n");
      irdump(fn);
   }
   return r;
}

static void
optimize(Function *fn) {
   int redo, fuel = ccopt.o > OPT1 ? 3 : 1, iter = 0;
   do {
      redo = 0;
      if (ipass(fn, "inline", doinline, iter)) {
         redo = 1;
         filluses(fn);
         copyopt(fn);
         mem2reg(fn);
      }
      freearena(fn->passarena);
      filldom(fn);
      if (!(fn->prop & FNUSE)) filluses(fn);
      redo += ipass(fn, "loopopt", loopopt, iter);
      freearena(fn->passarena);
      if (ccopt.ircheck) ircheck(fn);
      if (!(fn->prop & FNUSE)) filluses(fn);
      filldom(fn);
      ipass(fn, "cselim", cselim, iter);
      freearena(fn->passarena);
      redo += ipass(fn, "simpl", simpl, iter);
      freearena(fn->passarena);
      filldom(fn);
      fillloop(fn);
      pass(fn, "gcm", gcm);
      freearena(fn->passarena);
   } while (redo > 1 && ++iter < fuel);
}

void
irfini(Function *fn) {
   extern int nerror;
   static union { char m[sizeof(Arena) + (1<<14)]; Arena *_align; } amem;
   Arena *passarena = (void *)&amem.m;
   passarena->cap = sizeof amem.m - sizeof(Arena);
   fn->passarena = &passarena;
   if (nerror) {
      freefn(fn);
      return;
   }

   if (ccopt.ircheck) ircheck(fn);

   pass(fn, "abi", abi0);
   pass(fn, "lowerintrin", lowerintrin);

   if (ccopt.ircheck) ircheck(fn);

   if (ccopt.o > OPT0) {
      pass(fn, "mem2reg", mem2reg);
      freearena(fn->passarena);
      pass(fn, "copyopt", copyopt);
   }

   if (ccopt.o >= OPT1) pass(fn, "optimize", optimize);

   if (ccopt.ircheck) ircheck(fn);

   if (maybeinlinee(fn)) {
      freearena(fn->passarena);
      return;
   }
   /* extern inline def must have been stashed */
   assert((fn->symflags & (SLOCAL | SC99INLFN)) != SC99INLFN);

   irfini_end(fn);
}

void
irfini_end(Function *fn) {
   pass(fn, "sinkcond", sinkcond);
   pass(fn, "lowerstack", lowerstack);
   freearena(fn->passarena);
   pass(fn, "isel", mctarg->isel);
   pass(fn, "regalloc", regalloc);
   freearena(fn->passarena);
   if (objout.code)
      mctarg->emit(fn);

   freearena(fn->passarena);
   freefn(fn);
}

/* vim:set ts=3 sw=3 expandtab: */
