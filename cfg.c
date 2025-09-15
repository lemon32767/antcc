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

void
sortrpo(struct function *fn)
{
   static struct block **rpobuf;
   struct block **rpoend, **rpo;
   int i, ndead;

   xbgrow(&rpobuf, fn->nblk);
   rpo = rpoend = rpobuf + fn->nblk,

   startbbvisit();
   fn->entry->id = 0;
   markvisited(fn->entry);
   if (fn->entry->s1) porec(&rpo, fn->entry->s1);
   if (fn->entry->s2) porec(&rpo, fn->entry->s2);
   *--rpo = fn->entry;
   ndead = rpo - rpobuf;
   for (i = 1, ++rpo; rpo < rpoend; ++rpo, ++i) {
      rpo[-1]->lnext = rpo[0];
      rpo[0]->lprev = rpo[-1];
      rpo[0]->id = i;
   }
   fn->entry->lprev = rpo[-1];
   rpo[-1]->lnext = fn->entry;
   for (rpo = rpobuf; ndead > 0; --ndead) {
      (*rpo)->lnext = (*rpo)->lprev = NULL;
      freeblk(fn, *rpo);
   }
}

/* vim:set ts=3 sw=3 expandtab: */
