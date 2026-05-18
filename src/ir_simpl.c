#include "ir.h"

static int
mulk(Instr *ins, Block *blk, int *curi)
{
   s64int iv = intconval(ins->r);
   enum irclass cls = ins->cls;
   assert((u64int)iv > 1 && "trivial mul not handled by irbinop() ?");
   bool neg = iv < 0;
   if (neg) iv = -iv;
   /* This can be generalized to any sequence of shifts and
    * adds/subtracts, but whether that's worth it depends on the number of them
    * and the microarchitecture..  clang seems to stop after two shifts. Should
    * we compute approximate cost of instrs to determine? For now just handle
    * the po2 (+/- 1) case */
   if (ispo2(iv)) {
      /* x * 2^y ==> x << y */
      ins->op = Oshl;
      ins->r = mkref(RICON, ilog2(iv));
   } else if (ispo2(iv-1)) {
      /* x * 5 ==> (x << 2) + x */
      ins->op = Oadd;
      ins->r = ins->l;
      ins->l = insertinstr(blk, (*curi)++, mkinstr2(Oshl, cls, ins->l, mkref(RICON, ilog2(iv-1))));
   } else if (ispo2(iv+1)) {
      /* x * 7 ==> (x << 3) - x */
      ins->op = Osub;
      ins->r = ins->l;
      ins->l = insertinstr(blk, (*curi)++, mkinstr2(Oshl, cls, ins->l, mkref(RICON, ilog2(iv+1))));
   } else return 0;
   if (neg) {
      ins->l = insertinstr(blk, (*curi)++, *ins);
      ins->op = Oneg;
      ins->r = NOREF;
   }
   return 1;
}

static int
divmodk(Instr *ins, Block *blk, int *curi)
{
   enum op op = ins->op;
   enum irclass cls = ins->cls;
   s64int iv = intconval(ins->r);
   uint nbit = 8 * cls2siz[cls];
   bool neg = (op == Odiv || op == Orem) && iv < 0;
   if (ispo2(iv) || (neg && ispo2(-iv))) { /* simple po2 cases */
      Ref temp;
      uint s = ilog2(neg ? -iv : iv);
      switch (op) {
      default: assert(0);
      case Oudiv: /* x / 2^s ==> x >> s */
         ins->op = Oslr, ins->r = mkref(RICON, s);
         return 1;
      case Ourem: /* x % 2^s ==> x & 2^s-1 */
         ins->op = Oand, ins->r = mkintcon(cls, iv - 1);
         return 1;
      case Odiv: case Orem:
         /* have to adjust to round negatives toward zero */
         /* x' = (((x < 0 ? -1 : 0) >>> (Nbit - s)) + x) */
         temp = insertinstr(blk, (*curi)++, mkinstr2(Osar, cls, ins->l, mkref(RICON, nbit - 1)));
         temp = insertinstr(blk, (*curi)++, mkinstr2(Oslr, cls, temp, mkref(RICON, nbit - s)));
         temp = insertinstr(blk, (*curi)++, mkinstr2(Oadd, cls, ins->l, temp));
         if (op == Odiv) {
            /* (-) (x' >> s) */
            Instr sar = mkinstr2(Osar, cls, temp, mkref(RICON, s));
            if (!neg) *ins = sar;
            else {
               temp = insertinstr(blk, (*curi)++, sar);
               ins->op = Oneg, ins->l = temp, ins->r = NOREF;
            }
         } else {
            /* x - (x' & -(2^s)) */
            temp = insertinstr(blk, (*curi)++, mkinstr2(Oand, cls, temp, mkintcon(cls, neg ? iv : -iv)));
            ins->op = Osub, ins->r = temp;
         }
         break;
         return 0;
      }
   }
   return 0;
}

static int
doins(Instr *ins, Block *blk, int *curi)
{
   int narg = opnoper[ins->op];
   if (oisarith(ins->op)) {
      Ref r = narg == 1 ? irunop(NULL, ins->op, ins->cls, ins->l)
                        : irbinop(NULL, ins->op, ins->cls, ins->l, ins->r);
      if (r.bits) {
         *ins = mkinstr0(Onop,0);
         replcuses(mkref(RTMP, ins - instrtab), r);
         deluses(ins - instrtab);
         return 1;
      }
   }
   enum irclass k = ins->cls;
   switch (ins->op) {
   case Ocopy:
      if ((ins->l.t == RTMP && k == instrtab[ins->l.i].cls)
       || (isnumcon(ins->l) && k == concls(ins->l))
       || (kisint(k) && ins->l.t == RICON)) {
         Ref it = ins->l;
         *ins = mkinstr0(Onop,0);
         replcuses(mkref(RTMP, ins - instrtab), it);
         deluses(ins - instrtab);
         return 1;
      }
      break;
   case Omul:
      if (kisflt(k)) break;
      if (isnumcon(ins->l)) rswap(ins->l, ins->r); /* put const in rhs */
      if (isintcon(ins->r)) return mulk(ins, blk, curi);
      break;
   case Odiv: case Oudiv: case Orem: case Ourem:
      if (kisflt(k)) break;
      if (isintcon(ins->r)) return divmodk(ins, blk, curi);
      break;
   case Oequ: case Oneq:
      if (ins->l.t == RTMP && isintcon(ins->r)) {
         /* optimize `x <op> C <cmp> Q` */
         /* could apply with add/sub to lth/lte/gth/gte iff no signed wraparound, which isn't assumed */
         Instr *lhs = &instrtab[ins->l.i];
         enum op o = lhs->op;
         if (ins->cls == lhs->cls && (o == Oadd || o == Osub || o == Oxor) && isintcon(lhs->r)) {
            u64int c = intconval(ins->r), q = intconval(lhs->r);
            switch (o) { default: assert(0);
            case Oadd: c -= q; break; /* x + 3 == C ==> x == C - 3 */
            case Osub: c += q; break; /* x - 3 == C ==> x == C + 3 */
            case Oxor: c ^= q; break; /* x ^ 3 == C ==> x == C ^ 3 */
            }
            ins->l = lhs->l, ins->r = mkintcon(ins->cls, c);
            deluse(blk, ins - instrtab, mkref(RTMP, lhs - instrtab));
            if (!instruse[lhs - instrtab]) *lhs = mkinstr0(Onop,0);
            return 1;
         }
      }
   }
   return 0;
}

static void
jmpfind(Block **final, Block **pblk)
{
   Block **p2 = &final[(*pblk)->id];
   if (*p2 && !(*p2)->phi.n) {
      jmpfind(final, p2);
      *pblk = *p2;
   }
}

static void
fillpredsrec(Block *blk)
{
   while (blk && !wasvisited(blk)) {
      markvisited(blk);
      if (!blk->s1) return;
      if (!blk->s1->phi.n) addpred(blk->s1, blk);
      if (!blk->s2) {
         blk = blk->s1;
         continue;
      }
      if (!blk->s2->phi.n) addpred(blk->s2, blk);
      fillpredsrec(blk->s1);
      blk = blk->s2;
   }
}

static void
fillpreds(Function *fn)
{
   Block *blk = fn->entry, *next;
   do {
      if (blk->phi.n) continue;
      if (blk->npred > 1)
         xbfree(blk->_pred);
      blk->npred = 0;
      blk->_pred = NULL;
   } while ((blk = blk->lnext) != fn->entry);
   startbbvisit();
   fillpredsrec(fn->entry);
   blk = fn->entry->lnext;
   do { /* remove dead blocks */
      next = blk->lnext;
      if (!blk->npred) freeblk(fn, blk);
   } while ((blk = next) != fn->entry);
}

static void
mergeblks(Function *fn, Block *p, Block *s)
{
   assert(s->npred == 1 && !s->phi.n);
   vpushn(&p->ins, s->ins.p, s->ins.n);
   p->jmp = p->s1->jmp;
   for (int i = 0; i < 2; ++i) {
      Block *ss = (&s->s1)[i];
      if (ss) for (int j = 0; j < ss->npred; ++j) {
         if (blkpred(ss, j) == s) {
            blkpred(ss, j) = p;
            break;
         }
      }
   }
   /* breaks use for temps used in s */
   if (s->ins.n || s->jmp.arg[0].t == RTMP || s->jmp.arg[1].t == RTMP)
      fn->prop &= ~FNUSE;
   p->s1 = s->s1;
   p->s2 = s->s2;
   freeblk(fn, s);
}

void
simpl(Function *fn)
{
   FREQUIRE(FNUSE);
   int blkchange = 0;
   Block **jmpfinal = allocz(fn->passarena, fn->nblk * sizeof *jmpfinal, 0);
   Block *blk = fn->entry;

   do {
      /* merge blocks:
       * @blk:
       *   ... (1)
       *   b @s
       * @s:
       *   ... (2)
       *   b {...}
       *=>
       * @blk:
       *   ... (1)
       *   ... (2)
       *   b {...}
       * */
      if (blk != fn->entry) while (blk->s1 && !blk->s2 && blk->s1->npred == 1 && !blk->phi.n) {
         mergeblks(fn, blk, blk->s1);
      }
   } while ((blk = blk->lnext) != fn->entry);

   if (!(fn->prop & FNUSE)) filluses(fn);

   do {
      for (int i = 0; i < blk->phi.n; ++i) {
         int phi = blk->phi.p[i];
         /* delete trivial phis */
         Ref *args = phitab.p[instrtab[phi].l.i],
             same = *args;
         if (same.t == RTMP && instrtab[same.i].op == Ophi) goto Next;
         if (blk->npred > 1) for (int j = 1; j < blk->npred; ++j) {
            if (args[j].bits != same.bits) goto Next;
         }
         if (!(fn->prop & FNUSE)) filluses(fn);
         replcuses(mkref(RTMP, phi), same);
         delphi(blk, i);
         Next:;
      }

      int curi = 0;
   DoIns:
      for (; curi < blk->ins.n; ++curi) {
         Instr *ins = &instrtab[blk->ins.p[curi]];
         if (ins->op != Onop) {
            if (!(fn->prop & FNUSE)) filluses(fn);
            doins(ins, blk, &curi);
         }
      }

      if (blk->s2 && isintcon(blk->jmp.arg[0])) {
         /* simplify known conditional branch */
         Block *s = intconval(blk->jmp.arg[0]) ? blk->s1 : blk->s2;
         delpred(s == blk->s1 ? blk->s2 : blk->s1, blk);
         blk->s1 = s, blk->s2 = NULL;
         blk->jmp.arg[0] = NOREF;
         if (blk->s1 && !blk->s2 && blk->s1->npred == 1 && blk->s1->phi.n == 0) {
            mergeblks(fn, blk, blk->s1);
            goto DoIns;
         }
      }

      if (blk != fn->entry && blk->npred == 0) {
         freeblk(fn, blk);
      } else if (!blk->phi.n && !blk->ins.n) { /* thread jumps.. */ 
         if (blk->jmp.t == Jb && !blk->s2) {
            jmpfind(jmpfinal, &blk->s1);
            if (blk->s1 != blk) {
               ++blkchange;
               jmpfinal[blk->id] = blk->s1;
            }
         }
      }
   } while ((blk = blk->lnext) != fn->entry);

   if (blkchange) {
      do {
         if (blk->s1) jmpfind(jmpfinal, &blk->s1);
         if (blk->s2) jmpfind(jmpfinal, &blk->s2);
         if (blk->s1 && blk->s1 == blk->s2) {
            memset(blk->jmp.arg, 0, sizeof blk->jmp.arg);
            blk->s2 = NULL;
         }
      } while ((blk = blk->lnext) != fn->entry);
      fillpreds(fn);
      sortrpo(fn);
   }
   if (!(fn->prop & FNUSE)) filluses(fn);
   fn->prop &= ~FNDOM;
}

/* vim:set ts=3 sw=3 expandtab: */
