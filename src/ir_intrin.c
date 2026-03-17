#include "ir.h"

struct arg { union ref *arg, *ty; };

static int
intrin(struct block *blk, int *curi, enum intrin in, struct arg *args, int narg, union irtype ret)
{
   struct instr *this = &instrtab[blk->ins.p[*curi]];
   const struct typedata *td;
   union irtype ty;
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
         *this = mkinstr(Ocall, 0, mksymref(intern("memcpy"), SFUNC), this->r);
         calltab.p[this->r.i].narg = 3;
         calltab.p[this->r.i].ret = cls2type(0);
         return 0;
      } else {
         delinstr(blk, (*curi)--);
         for (int off = 0; off < td->siz; off += step) {
            union ref psrc = *args[1].arg, pdst = *args[0].arg, src;
            if (off) {
               pdst = insertinstr(blk, ++*curi, mkinstr(Oadd, KPTR, *args[0].arg, mkref(RICON, off)));
               psrc = insertinstr(blk, ++*curi, mkinstr(Oadd, KPTR, *args[1].arg, mkref(RICON, off)));
            }
            src = insertinstr(blk, ++*curi, mkinstr(Oloads8 + 2*ilog2(step), step < 8 ? KI32 : KI64, psrc));
            insertinstr(blk, ++*curi, mkinstr(Ostorei8 + ilog2(step), 0, pdst, src));
         }
         return 1;
      }
   }
   assert(0);
}

void
lowerintrin(struct function *fn)
{
   struct block *blk = fn->entry;
   struct arg argsbuf[32];
   vec_of(struct arg) args = VINIT(argsbuf, countof(argsbuf));

   do {
      for (int i = 0; i < blk->ins.n; ++i) {
         struct instr *ins = &instrtab[blk->ins.p[i]];
         if (ins->op == Oarg)
            vpush(&args, ((struct arg){ &ins->r, &ins->l }));
         else if (ins->op == Ocall)
            vinit(&args, argsbuf, countof(argsbuf));
         else if (ins->op == Ointrin) {
            int arg0 = i - args.n;
            assert(calltab.p[ins->r.i].narg == args.n);
            if (intrin(blk, &i, ins->l.i, args.p, args.n, calltab.p[ins->r.i].ret))
               for (int j = args.n; j > 0; --j, --i)
                  delinstr(blk, arg0);
            else
               abi0_call(fn, ins, blk, &i);
            vinit(&args, argsbuf, countof(argsbuf));
         }
      }
      assert(args.n == 0);
   } while ((blk = blk->lnext) != fn->entry);
}

/* vim:set ts=3 sw=3 expandtab: */
