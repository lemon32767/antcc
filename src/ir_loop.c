#include "ir.h"
#include "u_bits.h"

/* ref: https://llvm.org/docs/LoopTerminology.html */
struct Loop {
   Loop *next;
   Loop *parent;
   Block *head,
         *end;
   Block *prehead,
         *latch;
   int mintrips;
   BitSet *loopdefs; /* set of temps def'd in loop body */
   struct Counted *counted; /* counted loop info */
   /* hash table of induction variables */
   struct IndVar **ivht, *iv0, **ivtail;
   int niv, ivN;
};

bool
inloop(Loop *l, Block *b) {
   for (Loop *bl = b->loop; bl; bl = bl->parent)
      if (bl == l) return 1;
   return 0;
}

/* fix up a group of preds while iterating over them
 * to turn the corresponding edges into a single one by introducing a merge block,
 * doing all the surgery to fix edges and phis.
 * used to get unique loop preheader+latch */
static void
uniqedge(Function *fn, Block *head, int *ipred, Block **via, int *ipredvia0) {
   Block *p = blkpred(head, *ipred);
   if (!*via) {
      *via = p;
      (*via)->visit = 0;
      *ipredvia0 = *ipred;
   } else {
      if (!(*via)->visit) {
         *via = insertblk(fn, *via, head);
         (*via)->visit = -1;
         for (int j = 0; j < head->phi.n; ++j) {
            Instr *old = &instrtab[head->phi.p[j]];
            Ref new = insertphi(*via, old->cls);
            Ref input = phitab.p[old->l.i][*ipredvia0];
            phiargs(new.i)[0] = input;
            phitab.p[old->l.i][*ipredvia0] = new;
            fn->prop &=~ FNUSE;
         }
      }
      if (p->s1 == head) p->s1 = *via;
      else assert(p->s2 == head), p->s2 = *via;
      for (int j = 0; j < head->phi.n; ++j) {
         Instr *old = &instrtab[head->phi.p[j]];
         Instr *new = &instrtab[(*via)->phi.p[j]];
         int n = (*via)->npred;
         Ref input = phitab.p[old->l.i][*ipred];
         xbpush(&phitab.p[new->l.i], &n, input);
      }
      addpred(*via, p);
      delpred(head, p);
      --*ipred;
   }
}

/* unique preheader + latch, ensures loop header has 2 preds {0: pre, 1: latch} */
static void
loopsimpl(Function *fn, Loop *l) {
   FREQUIRE(FNRPO | FNLOOP | FNDOM);
   l->prehead = l->latch = NULL;
   int preh0i = 1<<30, latch0i = 1<<30;
   Block *hd = l->head;
   for (int pi = 0; pi < hd->npred; ++pi) {
      Block *p = blkpred(hd, pi);
      if (inloop(l, p)) {
         Block *oldlatch = l->latch;
         uniqedge(fn, hd, &pi, &l->latch, &latch0i);
         if (l->end == oldlatch) l->end = l->latch;
      } else {
         uniqedge(fn, hd, &pi, &l->prehead, &preh0i);
      }
   }

   assert(hd->npred == 2);
   if (preh0i != 0) {
      assert(latch0i == 0);
      assert(blkpred(hd, 0) == l->latch && blkpred(hd, 1) == l->prehead);
      /* swap preds and phi inputs */
      blkpred(hd, 0) = l->prehead;
      blkpred(hd, 1) = l->latch;
      for (int i = 0; i < hd->phi.n; ++i) {
         int p = hd->phi.p[i];
         Ref *a = phiargs(p),
              a0 = a[0];
         a[0] = a[1], a[1] = a0;
      }
   } else assert(latch0i == 1);
}

static bool
singleexit(Loop *l, Block *exit) {
   for (Block *b = l->head; b != l->end->lnext; b = b->lnext) {
      if (!inloop(l, b)) continue;
      for (int i = 0; i < 2; ++i) {
         Block *s = (&b->s1)[i];
         if (s && !inloop(l, s) && s != exit) return 0;
      }
   }
   return 1;
}

enum { MAXINVHDRNINS = 16 };

/* invert if header is small, and check
 *   phi defs in head are only used within loop
 *   and non-phi defs are only used within head
 *   and no phi backedge input is def'd in head
 *   and the loop exit has no phis yet
 * (conservatively reject circular dependencies in the header,
 *  needs tricky phis surgery; TODO implement such cases)
*/
static bool
caninvert(Loop *l, Block **exit, int *escapingphis, int *nescapingphis) {
   *nescapingphis = 0;
   /* match `while (H) { non-empty B }` */
   if (!l->head->s2 || l->latch == l->head)
      return 0;
   if (!(!inloop(l, *exit = l->head->s2) || !inloop(l, *exit = l->head->s1)))
      return 0;
   if ((*exit)->phi.n > 0) return 0;
   Block *h = l->head;
   if (h->ins.n + h->phi.n > MAXINVHDRNINS) return 0;

   int ibackedge = blkpred(h, 0) == l->latch ? 0 : 1;
   assert(blkpred(h, ibackedge) == l->latch);
   int backinputs[MAXINVHDRNINS];
   int nphi = h->phi.n;
   for (int i = 0; i < nphi; ++i) {
      Ref r = phiargs(h->phi.p[i])[ibackedge];
      if (r.t == RTMP) backinputs[i] = r.i;
      else backinputs[i] = -1;
   }
   for (int i = 0; i < nphi; ++i) {
      int phi = h->phi.p[i];
      for (int j = 0; j < nphi; ++j) {
         if (backinputs[j] == phi) return 0;
      }
      for (IRUse *u = instruse[phi]; u; u = u->next) {
         if (!inloop(l, u->blk)) {
            escapingphis[(*nescapingphis)++] = phi;
            break;
         }
      }
   }
   for (int i = 0; i < h->ins.n; ++i) {
      int t = h->ins.p[i];
      for (int j = 0; j < nphi; ++j) {
         if (backinputs[j] == t) return 0;
      }
      for (IRUse *u = instruse[t]; u; u = u->next)
         if (u->blk != h) return 0;
   }
   /* TODO with escaping phis rewire them for multiple exits too
    * in the general case it needs SSA repair */
   if (*nescapingphis > 0 && !singleexit(l, *exit)) return 0;
   return 1;
}

static Ref
mapref(Ref *instrmap, Ref r) {
   assert(r.bits);
   if (r.t == RTMP && instrmap[r.i].bits) return instrmap[r.i];
   assert(r.t != RADDR && r.t != RSTACK);
   return r;
}

static void
copyins(Function *fn, Block *dst, Block *src, Ref *instrmap) {
   fn->curblk = dst;
   for (int i = 0; i < src->ins.n; ++i) {
      int srct = src->ins.p[i];
      const Instr *srci = &instrtab[srct];
      Instr ins = *srci;
      if (ins.op == Ocall || ins.op == Ointrin) {
         ins.l = mapref(instrmap, ins.l);
         vpush(&calltab, calltab.p[ins.r.i]);
         ins.r.i = calltab.n-1;
      } else for (Ref *o = ins.oper, *oend = o + opnoper[ins.op]; o != oend; ++o) {
         if (!o->bits) break;
         if (o->t) *o = mapref(instrmap, *o);
      }
      instrmap[srct] = foldaddinstr(fn, ins);
   }
   dst->jmp.arg[0] = mapref(instrmap, src->jmp.arg[0]);
}

static bool
dbgp(Function *fn) {
   return ccopt.dbg.loop && dumpfilt(&fn->name->c);
}
#define dbgp(fn, ...) if (dbgp(fn)) bfmt(ccopt.dbg.out, __VA_ARGS__)

/* inversion: transform a natural loop like
 *    while (cond) { ... }
 * into
 *    if (cond0) do { ... } while (cond')
 * cond0 often folds into a constant, for example:
 *     for (int i = 0; i < 100; ++i) ...
 * --> if (0 < 100) do { ...; ++i; } while (i < 100);
 * --> do { ...; ++i; } while (i < 100);
 */
static int
loopinv(Function *fn, Loop *l) {
   FREQUIRE(FNUSE | FNLOOP | FNDOM | FNRPO);
   Block *exit;
   struct {
      int p[MAXINVHDRNINS];
      int n;
   } escapingphis;

   if (!caninvert(l, &exit, escapingphis.p, &escapingphis.n)) return 0;

   assert(l->head->jmp.t == Jb);
   assert(l->head->npred == 2 && "not simpl'd");
   dbgp(fn, "; doing loopinv(@%d-@%d)\n", l->head->id, l->end->id);
   /*
   * transform
   *    while (H0) { B...; L0; } exit:
   * into
   *    if (G) { P: do { H1: B...; L0; } while (L1); } exit:
   */
   Block *head0 = l->head;
   Block *body = exit == l->head->s2 ? l->head->s1 : l->head->s2;
   Block *latch0 = l->latch;

   Block *guard = head0; /* reuse H0 for G */
   l->prehead = insertblk(fn, guard, body); /* G -> P -> H1 */
   Block *head1 = insertblk(fn, l->prehead, body); /* P -> H1 -> B */

   /* H1.phi move= H0.phi */
   head1->phi = head0->phi, memset(&head0->phi, 0, sizeof head0->phi);

   assert(latch0);
   /* del H0 <- L0 backedge, add H1 <- L0 backedge */
   int l0pi0 = delpred(head0, latch0);
   int l0pi1 = head1->npred;
   addpred(head1, latch0);
   /* must match for phi args */
   assert(l0pi0 == l0pi1);

   /* replace L0 -> H0 edge with L0 -> H1 */
   if (latch0->s1 == head0) latch0->s1 = head1;
   else assert(latch0->s2 == guard), latch0->s2 = head1;
   /* insert L1::  L0 -> L1 -> H1,E */
   Block *latch1 = insertblk(fn, latch0, head1);
   latch1->id = -fn->nblk;
   if (head0->s1 == exit) {
      latch1->s1 = exit;
      latch1->s2 = head1;
   } else {
      latch1->s1 = head1;
      latch1->s2 = exit;
   }
   addpred(exit, latch1);

   /* copy loop exit condition body from H0 to L1 */
   extern int ninstrtab;
   Ref *instrmap = allocz(fn->passarena, (ninstrtab*2)*sizeof *instrmap, 0);
   /* ... rewiring H0 phis to their L0 inputs */
   for (int i = 0; i < head1->phi.n; ++i) {
      int phi = head1->phi.p[i];
      instrmap[phi] = phiargs(phi)[l0pi1];
   }
   copyins(fn, latch1, head0, instrmap);

   int gpred = 0;
   assert(blkpred(head1, gpred) == l->prehead);

   fillblkids(fn);
   /* fixup escaping phis */
   for (int i = 0; i < escapingphis.n; ++i) {
      int p = escapingphis.p[i];
      Ref p1 = insertphi(exit, instrtab[p].cls);
      const Ref *args = phiargs(p);
      Ref *args1 = phiargs(p1.i);
      for (int j = 0; j < exit->npred; ++j) {
         Block *ep = blkpred(exit, j);
         if (ep == guard) args1[j] = args[gpred];
         else if (ep == latch1) args1[j] = args[l0pi1];
         else args1[j] = mkref(RTMP, p); /* early exits get the loop phi */
      }
      replcuses(mkref(RTMP, p), p1, exit, REPLC_DOM);
   }

   /* fixup H0 phis in G */
   for (int i = 0; i < head1->phi.n; ++i) {
      int t = head1->phi.p[i];
      Instr *phi = &instrtab[t];
      replcuses(mkref(RTMP, t), phitab.p[phi->l.i][gpred], guard, REPLC_AT);
   }

   /* update loopinfo */
   l->prehead->loop = guard->loop = guard->idom->loop;
   l->prehead->loopdepth = guard->loopdepth = guard->idom->loopdepth;
   l->head = head1;
   l->latch = latch1;
   if (l->end->id < latch1->id) l->end = latch1;
   l->mintrips = 1;

   fn->prop &= ~FNUSE;
   return 1;
}

static inline bool
canspeculate(Instr *ins) {
   /* allow arith ops that can't trap */
   switch (ins->op) {
   case Odiv: case Orem: /* signed div traps for INT_MIN / -1 */
      if (ins->r.bits == mkref(RICON, -1).bits) return 0;
      /* fallthru */
   case Oudiv: case Ourem:
      /* may be div zero */
      return isintcon(ins->r) && ins->r.bits != ZEROREF.bits;
   default:
      return oisarith(ins->op);
   }
}

static bool
canhoist(Loop *l, int t) {
   Instr *ins = &instrtab[t];
   if (!canspeculate(ins)) return 0;
   for (int oi = 0; oi < opnoper[ins->op]; oi++) {
      if (ins->oper[oi].t != RTMP) continue;
      if (bstest(l->loopdefs, ins->oper[oi].i)) return 0;
   }
   return 1;
}

static int
licm(Function *fn, Loop *l) {
   extern int ninstrtab;
   l->loopdefs = anewbitset(fn->passarena, ninstrtab);
   int chg = 0;
   for (Block *b = l->head; b != l->end->lnext; b = b->lnext) {
      if (!inloop(l, b)) continue;
      for (int i = 0; i < b->phi.n; ++i) {
         bsset(l->loopdefs, b->phi.p[i]);
      }
      for (int i = 0; i < b->ins.n; ++i) {
         int t = b->ins.p[i];
         if (canhoist(l, t)) {
            ++chg;
            moveinstr(fn, b, i--, l->prehead, -1);
         } else {
            bsset(l->loopdefs, t);
         }
      }
   }
   return chg;
}

enum ivkind { IVBASIC, IVDERIVOFFSET, IVDERIVSCALED, IVDERIVWIDEN };
typedef struct IndVar {
   struct IndVar *next;
   Ref value, _init, _stride;
   struct IndVar *base;
   uchar kind;
   uchar cls;
   schar mark;
   union {
      Ref phinext;
      Ref offset;
      Ref scale;
      enum op op;
   };
} IndVar;

static bool
isconstinit(IndVar *iv) {
   if (iv->_init.bits) return isintcon(iv->_init);
   if (iv->kind != IVBASIC && !isconstinit(iv->base)) return 0;
   switch (iv->kind) {
   case IVDERIVOFFSET: return isintcon(iv->offset);
   case IVDERIVSCALED: return isintcon(iv->scale);
   case IVDERIVWIDEN:  return 1;
   }
   return 0;
}

static bool
isconststride(IndVar *iv) {
   if (iv->_stride.bits) return isintcon(iv->_stride);
   if (iv->kind != IVBASIC && !isconststride(iv->base)) return 0;
   switch (iv->kind) {
   case IVDERIVOFFSET: return 1;
   case IVDERIVSCALED: return isintcon(iv->scale);
   case IVDERIVWIDEN:  return 1;
   }
   return 0;
}

static int
dfmtiv(WriteBuf *b, void *p) {
   IndVar *iv = p;
   static const char kinds[][10] = {"basic", "offset", "scaled", "widen"};
   int n = bfmt(b, "[%sIV %k ", kinds[iv->kind], iv->cls);
   n += bfmt(b, "%r(%s) {%r,+,%r}", iv->value, opnames[instrtab[iv->value.i].op],
                                          iv->_init, iv->_stride);
   if (iv->kind != IVBASIC) bfmt(b, " base=%r", iv->base->value);
   switch (iv->kind) {
   case IVBASIC: n += bfmt(b, " phinext=%r(%s)", iv->phinext, opnames[instrtab[iv->phinext.i].op]); break;
   case IVDERIVOFFSET: n += bfmt(b, " offset=%r", iv->offset); break;
   case IVDERIVSCALED: n += bfmt(b, " scale=%r", iv->scale); break;
   }
   n += bfmt(b, "]");
   return n;
}

static Ref
ivinitv(Function *fn, Loop *l, IndVar *iv) {
   if (iv->_init.bits) return iv->_init;
   assert(iv->kind != IVBASIC);
   if (fn) fn->curblk = l->prehead;
   switch (iv->kind) { default: assert(0);
   case IVDERIVOFFSET:
      return iv->_init = irbinop(fn, Oadd, iv->cls, ivinitv(fn, l, iv->base), iv->offset);
   case IVDERIVSCALED:
      return iv->_init = irbinop(fn, Omul, iv->cls, ivinitv(fn, l, iv->base), iv->scale);
   case IVDERIVWIDEN:
      return iv->_init = irunop(fn, iv->op, iv->cls, ivinitv(fn, l, iv->base));
   }
}

static Ref
ivstridev(Function *fn, Loop *l, IndVar *iv) {
   if (iv->_stride.bits) return iv->_stride;
   assert(iv->kind != IVBASIC);
   if (fn) fn->curblk = l->prehead;
   switch (iv->kind) { default: assert(0);
   case IVDERIVOFFSET:
      return iv->_stride = ivstridev(fn, l, iv->base);
   case IVDERIVSCALED:
      return iv->_stride = irbinop(fn, Omul, iv->cls, ivstridev(fn, l, iv->base), iv->scale);
   case IVDERIVWIDEN:
      return iv->_stride = irunop(fn, Oexts32, iv->cls, ivstridev(fn, l, iv->base));
   }
}

static inline bool
invariant(Loop *l, Ref r) {
   assert(l->loopdefs);
   return iscon(r) || (r.t == RTMP && !bstest(l->loopdefs, r.i));
}

static bool
matchbasiciv(Loop *l, IndVar *iv, int phi) {
   Ref init = phiargs(phi)[0];
   Ref phinext = phiargs(phi)[1];
   if (!invariant(l, init) || !kisint(instrtab[phi].cls) || phinext.t != RTMP)
      return 0;
   Instr *ins = &instrtab[phinext.i];
   Ref phiref = mkref(RTMP, phi);
   /* NOTE: purposely only match RICON, small int const strides; for now */
   if (ins->op == Oadd && ins->l.bits == phiref.bits && ins->r.t == RICON && ins->r.i != 0) {
      /* i + K */
      iv->_stride = ins->r;
   } else if (ins->op == Osub && ins->l.bits == phiref.bits && ins->r.t == RICON && ins->r.i != 0) {
      /* i - K */
      iv->_stride = mkintcon(ins->cls, -(u64int)intconval(ins->r));
   } else {
      return 0;
   }
   iv->kind = IVBASIC;
   iv->cls = ins->cls;
   iv->_init = init;
   iv->value = mkref(RTMP, phi);
   iv->phinext = phinext;
   return 1;
}

static IndVar *
getiv(Loop *l, Ref r) {
   if (r.t != RTMP) return NULL;
   for (int m = l->ivN - 1, h = r.i; l->ivht[h &= m]; ++h) {
      if (l->ivht[h]->value.bits == r.bits) return l->ivht[h];
   }
   return NULL;
}

static void
putiv(Function *fn, Loop *l, IndVar *iv) {
   if (l->niv == l->ivN/2) { /* rehash */
      IndVar **old = l->ivht;
      l->ivht = allocz(fn->passarena, (l->ivN *= 2) * sizeof *l->ivht, 0);
      for (int n = l->niv, m = l->ivN - 1, h; n > 0; ++old) {
         if (!*old) continue;
         for (h = (*old)->value.i; l->ivht[h &= m]; ++h) { }
         l->ivht[h] = *old, --n;
      }
   }
   int m = l->ivN - 1, h;
   for (h = iv->value.i; l->ivht[h &= m]; ++h) { }
   iv = l->ivht[h] = alloccopy(fn->passarena, iv, sizeof *iv, 0);
   iv->mark = 0;
   iv->next = NULL;
   if (!l->ivtail) l->ivtail = &l->iv0;
   *l->ivtail = iv, l->ivtail = &iv->next;
   ++l->niv;
   dbgp(fn, "found %?\n", dfmtiv, iv);
}

static bool
matchderivediv(Loop *l, IndVar *iv, int t) {
   Instr *ins = &instrtab[t];
   iv->cls = ins->cls;
   iv->value = mkref(RTMP, t);
   iv->_init = iv->_stride = NOREF;
   IndVar *base;
   enum op op = ins->op;
   Ref x;
   if (op == Oadd && ((invariant(l, x = ins->r) && (base = getiv(l, ins->l)))
                   || (invariant(l, x = ins->l) && (base = getiv(l, ins->r))))) {
      /* 'base + offset' | 'offset + base' */
      iv->kind = IVDERIVOFFSET;
      iv->_stride = base->_stride;
      iv->offset = x;
   } else if (op == Omul && invariant(l, ins->r) && (base = getiv(l, ins->l))) {
      /* base * scale */
      iv->kind = IVDERIVSCALED;
      iv->scale = ins->r;
   } else if (op == Oshl && (ins->r.t == RICON && ins->r.i < 8*cls2siz[iv->cls]) && (base = getiv(l, ins->l))) {
      /* base << shift */
      iv->kind = IVDERIVSCALED;
      iv->scale = mkintcon(iv->cls, 1ull << intconval(ins->r));
   } else if (in_range(op, Oexts32, Oextu32) && (base = getiv(l, ins->l))) {
      /* e.g. (long) base */
      /* XXX ensure base can't overflow for correctness */
      iv->kind = IVDERIVWIDEN;
      iv->op = ins->op;
   } else {
     return 0;
   }
   iv->base = base;
   if (isconstinit(iv)) ivinitv(NULL, l, iv);
   if (isconststride(iv)) ivstridev(NULL, l, iv);
   return 1;
}

static void
findindvars(Function *fn, Loop *l) {
   extern int ninstrtab;
   l->ivht = allocz(fn->passarena, (l->ivN = 32) * sizeof *l->ivht, 0);
   l->iv0 = NULL;
   l->ivtail = NULL;
   Block *hd = l->head;
   assert(hd->npred == 2 && blkpred(hd, 0) == l->prehead && blkpred(hd, 1) == l->latch);
   for (int i = 0; i < hd->phi.n; ++i) {
      int p = hd->phi.p[i];
      IndVar iv;
      if (matchbasiciv(l, &iv, p))
         putiv(fn, l, &iv);
   }
   for (Block *b = l->head; b != l->end->lnext; b = b->lnext) {
      if (!inloop(l, b)) continue;
      for (int i = 0; i < b->ins.n; ++i) {
         int t = b->ins.p[i];
         IndVar iv;
         if (matchderivediv(l, &iv, t))
            putiv(fn, l, &iv);
      }
   }
}

/* counted loop info. a counted loop has an exit condition like (iv < limit) */
typedef struct Counted {
   Ref limitcheck;
   IndVar *counter;
   Ref limit;
   Ref trips;
   bool inverted; /* true if limitcheck is in latch */
} Counted;

static enum { IVDOWN = -1, IVUP = 1 }
ivdir(Function *fn, Loop *l, IndVar *iv) {
   if (!isintcon(ivstridev(fn, l, iv))) return 0;
   s64int s = intconval(ivstridev(fn, l, iv));
   return s < 0 ? IVDOWN : s > 0 ? IVUP : 0;
}

static Ref
safemaxtrips(Function *fn, Loop *l, IndVar *iv, Ref limit, bool inverted) {
   /* check no overflow possible, conservative, works for either IV direction */
   Ref initv = ivinitv(fn, l, iv), stridev = ivstridev(fn, l, iv);
   if (!kisint(iv->cls) || !isintcon(initv) || !isintcon(limit) || !isintcon(stridev))
      return NOREF;

   s64int init = intconval(initv), lim = intconval(limit), s = intconval(stridev), lower, upper;
   int dir;
   if (s > 0) lower = init, upper = lim, dir = 1;
   else lower = lim, upper = init, s = -s, dir = -1;

   enum { MAXBOUND = 1 << 20 };
   if (lower >= upper || lower <= -MAXBOUND || upper >= MAXBOUND)
      return NOREF;
   s64int tmax = (~0ull >> (64 - 8*cls2siz[iv->cls])) >> 1, tmin = -tmax - 1;
   s64int d = upper - lower;
   s64int trips = d / s + (d % s != 0);
   s64int last = init + (dir > 0 ? 1 : -1) * (trips - 1) * s;
   if (dir > 0 ? (last >= 0 && s > tmax - last)
               : (last <  0 && s > last - tmin))
      return NOREF;
   return mkintcon(KI32, trips + inverted);
}

static bool
detectcounted(Function *fn, Loop *l) {
   Counted ct[1];
   /* TODO flipped s1/s2 */
   if (l->head->s2 && !inloop(l, l->head->s2)) {
      ct->inverted = 0;
      ct->limitcheck = l->head->jmp.arg[0];
   } else if (l->latch->s2 && !inloop(l, l->latch->s2)) {
      ct->inverted = 1;
      ct->limitcheck = l->latch->jmp.arg[0];
   } else {
      return 0;
   }
   if (ct->limitcheck.t != RTMP) return 0;
   Instr *cmp = &instrtab[ct->limitcheck.i];

   IndVar *iv;
   if (cmp->op == Olth && (iv = getiv(l, cmp->l)) && invariant(l, cmp->r) && ivdir(fn, l, iv) == IVUP) {
      /* i < limit; i += K */
      ct->limit = cmp->r;
      ct->trips = safemaxtrips(fn, l, iv, ct->limit, ct->inverted);
   } else if (cmp->op == Ogth && (iv = getiv(l, cmp->l)) && invariant(l, cmp->r) && ivdir(fn, l, iv) == IVDOWN) {
      /* i > limit; i -= K */
      ct->limit = cmp->r;
      ct->trips = safemaxtrips(fn, l, iv, ct->limit, ct->inverted);
   } else {
      return 0;
   }
   if (!ct->trips.bits)
      return 0;
   Ref initv = ivinitv(fn, l, iv);
   dbgp(fn, "limitcheck init = %r, limit = %r\n", initv, ct->limit);
   ct->counter = iv;
   l->counted = alloccopy(fn->passarena, ct, sizeof ct, 0);
   return 1;
}

/* iv is a candidate for strength reduction when there are non trivial (offset)
 * operations in the iv chain */
static bool
shouldreduce(IndVar *iv) {
   for (; iv->kind != IVBASIC; iv = iv->base)
      if (iv->kind != IVDERIVOFFSET)
         return 1;
   return 0;
}

/* kill t if nothing live consumes it, then retry what it consumes */
static bool
killdead(int t) {
   Instr *ins = &instrtab[t];
   if (ins->op == Onop || ins->op == Ophi)
      return 0; /* phis are removed via delphi() only */
   if (ins->keep || (!oisarith(ins->op) && ins->op != Ocopy)) /* pure? */
      return 0;
   for (IRUse *u = instruse[t]; u; u = u->next)
      if (u->u == USERJUMP || instrtab[u->u].op != Onop) /* live use? */
         return 0;

   int noper = opnoper[ins->op];
   ins->op = Onop;
   for (int i = 0; i < noper; ++i) {
      Ref r = ins->oper[i];
      if (r.t != RTMP) continue;
      deluse(NULL, t, r);
      if (instrtab[r.i].op != Onop)
         killdead(r.i);
   }
   memset(ins->oper, 0, sizeof ins->oper);
   return 1;
}

enum { MAXNEWPHI = 6 };

/* basic IV strength reduction: materialize derived ivs into one phi + one add
 * when profitable */
static int
ivsimpl(Function *fn, Loop *l) {
   if (l->niv == 0) return 0;
   if (!(fn->prop & FNUSE)) filluses(fn);
   /* mark ivs used by a non-iv node */
   for (IndVar *iv = l->iv0; iv; iv = iv->next) {
      if (l->counted && iv == l->counted->counter) iv->mark = 1;
      else for (IRUse *use = instruse[iv->value.i]; use; use = use->next) {
         if (use->u == USERJUMP || !getiv(l, mkref(RTMP, use->u))) {
            iv->mark = 1;
            break;
         }
      }
   }

   int change = 0;
   int budget = MAXNEWPHI;
   for (IndVar *iv = l->iv0; iv && budget > 0; iv = iv->next) {
      if (!iv->mark || iv->kind == IVBASIC || !shouldreduce(iv))
         continue;
      --budget;
      dbgp(fn, "needed iv: %r\n", iv->value);
      Ref newphi = insertphi(l->head, iv->cls);
      Ref init = phiargs(newphi.i)[0] = ivinitv(fn, l, iv);
      Ref stride = ivstridev(fn, l, iv);
      fn->curblk = l->latch;
      dbgp(fn, " --> {%r,+,%r}=%r\n", init, stride, newphi);
      Ref update = phiargs(newphi.i)[1] = irbinop(fn, Oadd, iv->cls, newphi, stride);
      adduse(l->head, newphi.i, init);
      adduse(l->head, newphi.i, update);
      replcuses(iv->value, newphi, NULL, REPLC_ALL);
      iv->kind = IVBASIC;
      iv->phinext = update;
      iv->value = newphi;
      ++change;
   }

   /* any marked derived iv (not reduced) keeps the original instr, so its base
    * must be kept (marked); and the base may also be such an iv, hence the fixpoint */
   for (bool chg = 1; chg;) {
      chg = 0;
      for (IndVar *iv = l->iv0; iv; iv = iv->next) {
         if (!iv->mark || iv->kind == IVBASIC)
            continue;
         if (!iv->base->mark) {
            iv->base->mark = 1;
            chg = 1;
         }
      }
   }

   /* mark unneeded basic iv phis to kill later */
   for (IndVar *iv = l->iv0; iv; iv = iv->next) {
      if (iv->mark || iv->kind != IVBASIC) continue;
      dbgp(fn, "not needed iv: %r\n", iv->value);
      instrtab[iv->value.i].op = Onop;
      ++change;
   }

   /* kill dead phis and their now-dead users */
   for (bool chg = 1; chg;) {
      chg = 0;
      Block *fin = l->end->lnext;
      /* sweep instructions first since loop phis may use them */
      for (Block *b = l->prehead; b != fin; b = b->lnext) {
         for (int i = 0; i < b->ins.n; ++i)
            chg |= killdead(b->ins.p[i]);
      }

      /* and kill the tombstoned phis */
      for (int i = 0; i < l->head->phi.n; ++i) {
         int t = l->head->phi.p[i];
         if (instrtab[t].op != Onop) continue;
         dbgp(fn, "delete dead phi %r\n", mkref(RTMP, t));
         delphi(l->head, i--);
         chg = 1;
      }
   }

   return change;
}

int
loopopt(Function *fn) {
   int changed = 0;
   filldom(fn);
   fillloop(fn);
   for (Loop *l = fn->loops; l; l = l->next) {
      loopsimpl(fn, l);
      if (!(fn->prop & FNUSE)) filluses(fn);
      changed += loopinv(fn, l);
      changed += licm(fn, l);
      findindvars(fn, l);
      if (detectcounted(fn, l)) {
         dbgp(fn, "counted loop with %r trips; @%d-@%d\n", l->counted->trips, l->head->id, l->end->id);
      }
      changed += ivsimpl(fn, l);
      if ((dbgp)(fn)) {
         dbgp(fn, "<< After opt loop @%d-@%d >>\n", l->head->id, l->end->id);
         irdump(fn);
      }
   }
   if (changed) ircheck(fn); /* these transforms are too tricksy to disable this check for now */

   return changed;
}

static int
loopmark(Loop *l, Block *blk) {
   if (blk->id < l->head->id || blk->visit == -l->head->id) return 0;
   if (dominates(l->head, blk)) {
      blk->visit = -l->head->id;
      ++blk->loopdepth;
      if (blk->id > l->end->id) l->end = blk;
      int irreducible = 0;
      for (int i = 0; i < blk->npred; ++i)
         irreducible += loopmark(l, blkpred(blk, i));
      return irreducible;
   } else {
      return 1;
   }
}

void
fillloop(Function *fn) {
   Block *b = fn->entry;
   int id = 0;
   FREQUIRE(FNRPO | FNDOM);
   do {
      b->id = id++;
      b->visit = 0;
      b->loopdepth = 0;
      b->loop = NULL;
   } while ((b = b->lnext) != fn->entry);

   fn->loops = NULL;
   Loop **ltail = &fn->loops;
   do {
      Loop _l = {.parent = b->loop, .head = b, .end = b},
           *l = &_l;
      int iscyc = 0, badloop = 0;
      for (int i = 0; i < b->npred; ++i) {
         Block *p = blkpred(b, i);
         if (p->id >= b->id && dominates(b, p)) { /* b is loop header */
            assert(b->id > 0); /* entry cannot be loop header */
            iscyc = 1;
            badloop += loopmark(l, p);
         }
      }
      /* do not record irreducible cyles */
      if (iscyc && !badloop) {
         assert(b != fn->entry);
         l = alloccopy(fn->passarena, l, sizeof *l, 0);

         /* record each block in body as belonging to loop */
         for (Block *in = l->end; in != b->lprev; in = in->lprev) {
            if (in->visit == -b->id)
               in->loop = l;
         }
         *ltail = l;
         ltail = &l->next;
      }
   } while ((b = b->lnext) != fn->entry);
   fn->prop |= FNBLKID;
   fn->prop |= FNLOOP;
}

/*  vim:set ts=3 sw=3 expandtab:  */
