#include "common.h"
#include "ir.h"

/* require use, keeps use */
void
copyopt(struct function *fn)
{
   struct block *blk = fn->entry;

   do {
      for (int i = 0; i < blk->ins.n; ++i) {
         struct use *use, *uend;
         union ref var = mkref(RTMP, blk->ins.p[i]);
         struct instr *ins = &instrtab[var.i];

         if (ins->op == Ocopy) {
            assert(ins->l.t != RREG);

            replcuses(var, ins->l);
            *ins = mkinstr(Onop,0,);
            deluses(var.i);
         }
      }
   } while ((blk = blk->lnext) != fn->entry);
}

void
filluses(struct function *fn)
{
   extern int ninstr;
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
}

/* vim:set ts=3 sw=3 expandtab: */
