#include "ir.h"

void
lowerstack(struct function *fn)
{
   extern int ninstr;
   fn->stksiz = 0;
   FREQUIRE(FNUSE);

   struct block *blk = fn->entry;
   do {
      for (int i = 0; i < blk->ins.n; ++i) {
         uint alignlog2, siz;
         int t = blk->ins.p[i];
         struct instr *ins = &instrtab[t];
         switch (ins->op) {
         case Oalloca1: case Oalloca2: case Oalloca4: case Oalloca8: case Oalloca16:
            alignlog2 = ins->op - Oalloca1;
            assert(ins->l.i > 0);
            siz = ins->l.i << alignlog2;
            fn->stksiz += siz;
            fn->stksiz = alignup(fn->stksiz, 1 << alignlog2);
            if (fn->stksiz > (1<<16)-1) error(NULL, "'%s' stack frame too big", fn->name);
            *ins = mkinstr(Onop,0,);
            replcuses(mkref(RTMP, t), mkref(RSTACK, fn->stksiz));
            continue;
         }
      }
   } while ((blk = blk->lnext) != fn->entry);

   if (ccopt.dbg.s) {
      bfmt(ccopt.dbgout, "<< After stack >>\n");
      irdump(fn);
   }
}

/* vim:set ts=3 sw=3 expandtab: */
