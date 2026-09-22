#include "ir.h"

void
copyopt(Function *fn)
{
   Block *blk = fn->entry;

   FREQUIRE(FNUSE);
   do {
      for (int i = 0; i < blk->phi.n; ++i) {
         /* simplify same-arg phi */
         int phi = blk->phi.p[i];
         Ref *arg = phiargs(phi);
         for (int j = 1; j < blk->npred; ++j) {
            if (arg[j].bits != arg->bits) goto Next;
         }
         replcuses(mkref(RTMP, phi), *arg, NULL, REPLC_ALL);
         delphi(blk, i--);
      Next:;
      }
      for (int i = 0; i < blk->ins.n; ++i) {
         Ref var = mkref(RTMP, blk->ins.p[i]);
         Instr *ins = &instrtab[var.i];
         enum irclass k;

         if (ins->op == Ocopy) {
            Ref arg = ins->l;
            if (arg.t == RTMP) k = insrescls(instrtab[arg.i]);
            else if (arg.t == RICON) k = cls2siz[ins->cls] == 4 ? KI32 : KI64;
            else if (arg.t == RXCON) k = isnumcon(arg) ? concls(arg) : KPTR;
            else continue;
            if (ins->cls != k) continue;

            replcuses(var, arg, NULL, REPLC_ALL);
            if (arg.t == RTMP)
               deluse(blk, var.i, arg);
            *ins = mkinstr0(Onop,0);
            deluses(var.i);
         }
      }
      delnops(blk);
   } while ((blk = blk->lnext) != fn->entry);
}

/* vim:set ts=3 sw=3 expandtab: */
