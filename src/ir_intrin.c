#include "ir.h"

typedef struct { Ref *arg, *ty; } Arg;

static int
intrin(Block *blk, int *curi, enum intrin in, Arg *args, int narg, IRType ret) {
   Instr *this = &instrtab[blk->ins.p[*curi]];
   const TypeData *td;
   IRType ty;
   uint ncopy, step;

   switch (in) {
   case 0: assert(0);
   case INstructcopy:
      assert(narg == 2 && args[0].ty->bits == args[1].ty->bits);
      ty = ref2type(*args[0].ty);
      assert(ty.isagg);
      td = &typedata[ty.cls];
      step = td->align <= 8 ? td->align : 8;
      ncopy = td->siz / step;
      if (ncopy > 4) {
         enum irclass cls = siz2intcls[cls2siz[KPTR]];
         /* memcpy */
         *args[1].ty = *args[0].ty = mktyperef(cls2type(KPTR));
         insertinstr(blk, (*curi)++, mkarginstr(cls2type(cls), mkintcon(cls, td->siz)));
         *this = mkinstr2(Ocall, 0, mksymref(intern("memcpy"), SFUNC), this->r);
         calltab.p[this->r.i].narg = 3;
         calltab.p[this->r.i].ret = cls2type(0);
         return 0;
      } else {
         delinstr(blk, (*curi)--);
         for (int off = 0; off < td->siz; off += step) {
            Ref psrc = *args[1].arg, pdst = *args[0].arg, src;
            if (off) {
               pdst = insertinstr(blk, ++*curi, mkinstr2(Oadd, KPTR, *args[0].arg, mkref(RICON, off)));
               psrc = insertinstr(blk, ++*curi, mkinstr2(Oadd, KPTR, *args[1].arg, mkref(RICON, off)));
            }
            src = insertinstr(blk, ++*curi, mkinstr1(Oloads8 + 2*ilog2(step), step < 8 ? KI32 : KI64, psrc));
            insertinstr(blk, ++*curi, mkinstr2(Ostorei8 + ilog2(step), 0, pdst, src));
         }
         return 1;
      }
   }
   assert(0);
}

void
lowerintrin(Function *fn) {
   Block *blk = fn->entry;
   DEF_SVEC(Arg, 32, args);

   do {
      for (int i = 0; i < blk->ins.n; ++i) {
         Instr *ins = &instrtab[blk->ins.p[i]];
         if (ins->op == Oarg)
            vpush(&args, ((Arg){&ins->r, &ins->l}));
         else if (ins->op == Ocall)
            svec_reset(args);
         else if (ins->op == Ointrin) {
            int arg0 = i - args.n;
            assert(calltab.p[ins->r.i].narg == args.n);
            if (intrin(blk, &i, ins->l.i, args.p, args.n, calltab.p[ins->r.i].ret))
               for (int j = args.n; j > 0; --j, --i)
                  delinstr(blk, arg0);
            else
               abi0_call(fn, ins, blk, &i);
            svec_reset(args);
         }
      }
      assert(args.n == 0);
   } while ((blk = blk->lnext) != fn->entry);
}

/* vim:set ts=3 sw=3 expandtab: */
