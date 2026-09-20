#include "ir.h"
#include "u_bits.h"

/* ref: https://llvm.org/docs/LoopTerminology.html */

bool
inloop(Loop *l, Block *b)
{
   for (Loop *bl = b->loop; bl; bl = bl->parent)
      if (bl == l) return 1;
   return 0;
}

/* fix up a group of preds while iterating over them
 * to turn the corresponding edges into a single one by introducing a merge block,
 * doing all the surgery to fix edges and phis.
 * used to get unique loop preheader+latch */
static void
uniqedge(Function *fn, Block *head, int *ipred, Block **via, int *ipredvia0)
{
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

/* unique preheader + latch */
static void
loopsimpl(Function *fn, Loop *l)
{
   FREQUIRE(FNRPO | FNLOOP | FNDOM);
   l->prehead = l->latch = NULL;
   int preh0i = 1<<30, latch0i = 1<<30;
   for (int pi = 0; pi < l->head->npred; ++pi) {
      Block *p = blkpred(l->head, pi);
      if (inloop(l, p)) {
         uniqedge(fn, l->head, &pi, &l->latch, &latch0i);
      } else {
         uniqedge(fn, l->head, &pi, &l->prehead, &preh0i);
      }
   }
}

/* invert if header is small, and check
 *   phi defs in head are only used within loop
 *   and non-phi defs are only used within head
 *   and no phi backedge input is def'd in head
 * (conservatively reject any circular dependencies in header,
 *  needs tricky phis surgery; TODO implement such cases) */
static bool
caninvert(Loop *l, Block **exit)
{
   /* match `while (H) { non-empty B }` */
   if (!l->head->s2 || l->latch == l->head)
      return 0;
   if (!(!inloop(l, *exit = l->head->s2) || !inloop(l, *exit = l->head->s1)))
      return 0;
   if ((*exit)->phi.n > 0) return 0;
   Block *h = l->head;
   if (h->ins.n + h->phi.n > 16) return 0;

   int ibkedge = blkpred(h, 0) == l->latch ? 0 : 1;
   assert(blkpred(h, ibkedge) == l->latch);
   int backinputs[16];
   int nphi = h->phi.n;
   for (int i = 0; i < nphi; ++i) {
      Ref r = phiargs(h->phi.p[i])[ibkedge];
      if (r.t == RTMP) backinputs[i] = r.i;
      else backinputs[i] = -1;
   }
   for (int i = 0; i < nphi; ++i) {
      int phi = h->phi.p[i];
      for (int j = 0; j < nphi; ++j) {
         if (backinputs[j] == phi) return 0;
      }
      for (IRUse *u = instruse[phi]; u; u = u->next)
         if (!inloop(l, u->blk)) return 0;
   }
   for (int i = 0; i < h->ins.n; ++i) {
      int t = h->ins.p[i];
      for (int j = 0; j < nphi; ++j) {
         if (backinputs[j] == t) return 0;
      }
      for (IRUse *u = instruse[t]; u; u = u->next)
         if (u->blk != h) return 0;
   }
   return 1;
}

static Ref
mapref(Ref *instrmap, Ref r)
{
   assert(r.bits);
   if (r.t == RTMP && instrmap[r.i].bits) return instrmap[r.i];
   assert(r.t != RADDR && r.t != RSTACK);
   return r;
}

static void
copyins(Function *fn, Block *dst, Block *src, Ref *instrmap)
{
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
dbgp(Function *fn)
{
   return ccopt.dbg.loop && dumpfilt(&fn->name->c);
}

static int
loopinv(Function *fn, Loop *l)
{
   FREQUIRE(FNUSE | FNLOOP | FNDOM | FNRPO);
   Block *exit;

   if (!caninvert(l, &exit)) return 0;

   assert(l->head->jmp.t == Jb);
   assert(l->head->npred == 2 && "not simpl'd");
   if (dbgp(fn))
      bfmt(ccopt.dbg.out, "; doing loopinv(@%d-@%d)\n", l->head->id, l->end->id);
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
   /* fixup H0 phis in G */
   for (int i = 0; i < head1->phi.n; ++i) {
      int t = head1->phi.p[i];
      Instr *phi = &instrtab[t];
      replcuses(mkref(RTMP, t), phitab.p[phi->l.i][gpred], guard);
   }

   /* update loopinfo */
   l->prehead->loop = guard->loop = guard->idom->loop;
   l->prehead->loopdepth = guard->loopdepth = guard->idom->loopdepth;
   l->head = head1;
   l->latch = latch1;
   while (inloop(l, l->end->lnext)) l->end = l->end->lnext;
   l->mintrips = 1;

   fn->prop &= ~FNUSE;
   fn->prop &= ~FNBLKID;
   return 1;
}

static inline bool
canspeculate(Instr *ins)
{
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
canhoist(Loop *l, BitSet *loopdefs, int t)
{
   Instr *ins = &instrtab[t];
   if (!canspeculate(ins)) return 0;
   for (int oi = 0; oi < opnoper[ins->op]; oi++) {
      if (ins->oper[oi].t != RTMP) continue;
      if (bstest(loopdefs, ins->oper[oi].i)) return 0;
   }
   return 1;
}

static void
moveinstr(Function *fn, Block *srcb, int srci, Block *tob)
{
   int t = srcb->ins.p[srci];
   Instr *ins = &instrtab[t];
   vpush(&tob->ins, t);
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

static int
licm(Function *fn, Loop *l)
{
   extern int ninstrtab;
   BitSet *loopdefs = anewbitset(fn->passarena, ninstrtab);
   int chg = 0;
   for (Block *b = l->head; b != l->end->lnext; b = b->lnext) {
      if (!inloop(l, b)) continue;
      for (int i = 0; i < b->phi.n; ++i) {
         bsset(loopdefs, b->phi.p[i]);
      }
      for (int i = 0; i < b->ins.n; ++i) {
         int t = b->ins.p[i];
         if (canhoist(l, loopdefs, t)) {
            ++chg;
            moveinstr(fn, b, i--, l->prehead);
         } else {
            bsset(loopdefs, t);
         }
      }
   }
   return chg;
}

int
loopopt(Function *fn)
{
   int changed = 0;
   filldom(fn);
   fillloop(fn);
   for (Loop *l = fn->loops; l; l = l->next) {
      loopsimpl(fn, l);
      if (!(fn->prop & FNUSE)) filluses(fn);
      changed += loopinv(fn, l);
      changed += licm(fn, l);
   }

   return changed;
}

static int
loopmark(Loop *l, Block *blk)
{
   if (blk->id < l->head->id || blk->visit == -l->head->id) return 0;
   if (dominates(l->head, blk)) {
      blk->visit = -l->head->id;
      ++blk->loopdepth;
      int irreducible = 0;
      for (int i = 0; i < blk->npred; ++i)
         irreducible += loopmark(l, blkpred(blk, i));
      return irreducible;
   } else {
      return 1;
   }
}

void
fillloop(Function *fn)
{
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
            if (p->id > l->end->id) l->end = p;
            badloop += loopmark(l, p);
         }
      }
      /* do not record irreducible cyles */
      if (iscyc && !badloop) {
         assert(b != fn->entry);
         l = alloccopy(fn->passarena, l, sizeof *l, 0);

         /* mark each loop body block and gather loop exits */
         for (Block *in = l->end; in != b->lprev; in = in->lprev) {
            if (in->visit == -b->id) {
               in->loop = l;
               for (int is = 0; is < 2; ++is) {
                  Block *s = (&in->s1)[is];
                  if (s && s->visit != -b->id && s->visit != 0xdeadbeef) {
                     s->visit = 0xdeadbeef;
                     struct BlkList xs = {l->exits, s};
                     l->exits = alloccopy(fn->passarena, &xs, sizeof xs, 0);
                  }
               }
            }
         }
         *ltail = l;
         ltail = &l->next;
      }
   } while ((b = b->lnext) != fn->entry);
   fn->prop |= FNBLKID;
   fn->prop |= FNLOOP;
}

/*  vim:set ts=3 sw=3 expandtab:  */
