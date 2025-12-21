#include "ir.h"

static int
mulk(struct instr *ins, struct block *blk, int *curi)
{
   vlong iv = intconval(ins->r);
   enum irclass cls = ins->cls;
   assert(iv > 1 && "trivial mul not handled by irbinop() ?");
   /* This can be generalized to any sequence of shifts and
    * adds/subtracts, but whether that's worth it depends on the number of them
    * and the microarchitecture..  clang seems to stop after two shifts. Should
    * we compute approximate cost of instrs to determine? For now just handle
    * the po2 (+/- 1) case */
   if (ispo2(iv)) {
      /* x * 2^y ==> x << y */
      ins->op = Oshl;
      ins->r = mkref(RICON, ilog2(iv));
      return 1;
   } else if (ispo2(iv-1)) {
      /* x * 5 ==> (x << 2) + x */
      ins->op = Oadd;
      ins->r = ins->l;
      ins->l = insertinstr(blk, (*curi)++, mkinstr(Oshl, cls, ins->l, mkref(RICON, ilog2(iv-1))));
      return 1;
   } else if (ispo2(iv+1)) {
      /* x * 7 ==> (x << 3) - x */
      ins->op = Osub;
      ins->r = ins->l;
      ins->l = insertinstr(blk, (*curi)++, mkinstr(Oshl, cls, ins->l, mkref(RICON, ilog2(iv+1))));
      return 1;
   }
   return 0;
}

static int
ins(struct instr *ins, struct block *blk, int *curi)
{
   int narg = opnarg[ins->op];
   if (oisarith(ins->op)) {
      union ref r = narg == 1 ? irunop(NULL, ins->op, ins->cls, ins->l)
                              : irbinop(NULL, ins->op, ins->cls, ins->l, ins->r);
      if (r.bits) {
         ins->op = Ocopy;
         ins->cls = insrescls(*ins);
         ins->l = r;
         ins->r = NOREF;
         return 1;
      }
   }
   enum irclass k = ins->cls;
   switch (ins->op) {
   case Omul:
      if (kisflt(k)) break;
      if (isnumcon(ins->l)) rswap(ins->l, ins->r); /* put const in rhs */
      if (isintcon(ins->r)) return mulk(ins, blk, curi);
   }
   return 0;
}

static void
jmpfind(struct block **final, struct block **pblk)
{
   struct block **p2 = &final[(*pblk)->id];
   if (*p2 && !(*p2)->phi.n) {
      jmpfind(final, p2);
      *pblk = *p2;
   }
}

static void
fillpredsrec(struct block *blk)
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
fillpreds(struct function *fn)
{
   struct block *blk = fn->entry, *next;
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

void
simpl(struct function *fn)
{
   FREQUIRE(FNUSE);
   int inschange = 0,
       blkchange = 0;
   struct block **jmpfinal = allocz(fn->passarena, fn->nblk * sizeof *jmpfinal, 0);
   struct block *blk = fn->entry;

   fn->curblk = NULL;
   do {
      for (int i = 0; i < blk->phi.n; ++i) {

      }
      for (int i = 0; i < blk->ins.n; ++i) {
         inschange += ins(&instrtab[blk->ins.p[i]], blk, &i);
      }

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
         struct block *s = blk->s1;
         vpushn(&blk->ins, s->ins.p, s->ins.n);
         blk->jmp = blk->s1->jmp;
         for (int i = 0; i < 2; ++i) {
            struct block *ss = (&s->s1)[i];
            if (ss) for (int j = 0; j < ss->npred; ++j) {
               if (blkpred(ss, j) == s) {
                  blkpred(ss, j) = blk;
                  break;
               }
            }
         }
         /* breaks use for temps used in s */
         if (s->ins.n || s->jmp.arg[0].t == RTMP || s->jmp.arg[1].t == RTMP)
            fn->prop &= ~FNUSE;
         blk->s1 = s->s1;
         blk->s2 = s->s2;
         freeblk(fn, s);
      }
      /* thread jumps.. */
      if (!blk->phi.n && !blk->ins.n) {
         if (blk->jmp.t == Jb && !blk->s2) {
            jmpfind(jmpfinal, &blk->s1);
            if (blk->s1 != blk) {
               ++blkchange;
               jmpfinal[blk->id] = blk->s1;
            }
         }
      }
   } while ((blk = blk->lnext) != fn->entry);
   if (inschange) {
      if (!(fn->prop & FNUSE)) filluses(fn);
      copyopt(fn);
   }
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
   }
}

/* vim:set ts=3 sw=3 expandtab: */
