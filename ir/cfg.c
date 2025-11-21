#include "ir.h"

static void
porec(struct block ***rpo, struct block *b)
{
   if (wasvisited(b)) return;
   markvisited(b);
   if (b->s2) porec(rpo, b->s2);
   if (b->s1) porec(rpo, b->s1);
   *--*rpo = b;
}

/* also blkid */
void
sortrpo(struct function *fn)
{
   static struct block **rpobuf;
   struct block **rpoend, **rpo, *blk, *next;
   int i, ndead;

   xbgrow(&rpobuf, fn->nblk);
   rpo = rpoend = rpobuf + fn->nblk,

   startbbvisit();
   fn->entry->id = 0;
   porec(&rpo, fn->entry);
   ndead = rpo - rpobuf;
   if (ndead > 0) for (blk = fn->entry->lprev; blk != fn->entry; blk = next) {
      next = blk->lprev;
      if (!wasvisited(blk)) {
         for (int i = 0; i < blk->ins.n; ++i) {
            /* if unreachable block has alloca pseudo-instrs, move them to the entry
             * to be able to delete it */
            if (oisalloca(instrtab[blk->ins.p[i]].op)) {
               vpush(&fn->entry->ins, blk->ins.p[i]);
            }
         }
         for (int i = 0; i < blk->npred; ++i)
            assert(!wasvisited(blkpred(blk, i)));
         freeblk(fn, blk);
         --ndead;
      }
   }
   for (i = 1, ++rpo; rpo < rpoend; ++rpo, ++i) {
      rpo[-1]->lnext = rpo[0];
      rpo[0]->lprev = rpo[-1];
      rpo[0]->id = i;
   }
   fn->entry->lprev = rpo[-1];
   rpo[-1]->lnext = fn->entry;

   fn->prop |= FNBLKID | FNRPO;
}

/* vim:set ts=3 sw=3 expandtab: */
