#include "ir.h"

void
copyopt(struct function *fn)
{
   struct block *blk = fn->entry;

   FREQUIRE(FNUSE);
   do {
      for (int i = 0; i < blk->phi.n; ++i) {
         /* simplify same-arg phi */
         int phi = blk->phi.p[i];
         union ref *arg = phitab.p[instrtab[phi].l.i];
         for (int j = 1; j < blk->npred; ++j) {
            if (arg[j].bits != arg->bits) goto Next;
         }
         /* being conservative here because phis could have circular dependencies? */
         if (arg->t != RTMP || instrtab[arg->i].op != Ophi) {
            replcuses(mkref(RTMP, phi), *arg);
            delphi(blk, i--);
         }
      Next:;
      }
      for (int i = 0; i < blk->ins.n; ++i) {
         union ref var = mkref(RTMP, blk->ins.p[i]);
         struct instr *ins = &instrtab[var.i];
         enum irclass k;

         if (ins->op == Ocopy) {
            union ref arg = ins->l;
            if (arg.t == RTMP) k = insrescls(instrtab[arg.i]);
            else if (arg.t == RICON) k = cls2siz[ins->cls] == 4 ? KI32 : KI64;
            else if (arg.t == RXCON) k = isnumcon(arg) ? conht[arg.i].cls : KPTR;
            else continue;
            if (ins->cls != k) continue;

            replcuses(var, arg);
            *ins = mkinstr(Onop,0,);
            deluses(var.i);
         }
      }
      delnops(blk);
   } while ((blk = blk->lnext) != fn->entry);
}

/* vim:set ts=3 sw=3 expandtab: */
