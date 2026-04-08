#include "ir.h"

void
lowerstack(Function *fn)
{
   fn->stksiz = 0;
   FREQUIRE(FNUSE);

   Block *blk = fn->entry;
   do {
      for (int i = 0; i < blk->ins.n; ++i) {
         int t = blk->ins.p[i];
         Instr *ins = &instrtab[t];
         if (oisalloca(ins->op)) {
            uint alignlog2 = ins->op - Oalloca1;
            assert(ins->l.i > 0);
            uint siz = ins->l.i << alignlog2;
            fn->stksiz += siz;
            fn->stksiz = alignup(fn->stksiz, 1 << alignlog2);
            if (fn->stksiz > (1<<20)-1) error(NULL, "'%s' stack frame too big", fn->name);
            *ins = mkinstr0(Onop,0);
            replcuses(mkref(RTMP, t), mkref(RSTACK, fn->stksiz-siz));
         }
      }
   } while ((blk = blk->lnext) != fn->entry);

   if (ccopt.dbg.s) {
      bfmt(ccopt.dbgout, "<< After stack >>\n");
      irdump(fn);
   }
}

/* vim:set ts=3 sw=3 expandtab: */
