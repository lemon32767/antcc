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

static void
insrtcall1(Function *fn, Instr *ins, Block *b, int *i, enum irclass k, const char *f,
           enum irclass argk, Ref arg) {
   insertinstr(b, (*i)++, mkarginstr(cls2type(argk), arg));
   ins->op = Ocall, ins->cls = k;
   inschoper(b, ins, 0, mksymref(intern(f), SFUNC));
   inschoper(b, ins, 1, mkcallarg(cls2type(k), 1, -1));
   abi0_call(fn, ins, b, i, NOREF);
}

static void
arithlower(Function *fn, Instr *ins, Block *b, int *curi) {
   enum irclass k = ins->cls;
   switch (ins->op) {
   case Opopcnt:
      if (!(mctarg->feat&MFEATPOPCNT)) {
         Ref x = ins->l;
         if (k == KI32 && targ_64bit)
            x = insertinstr(b, (*curi)++, mkinstr1(Oextu32, k = KI64, x));
         insrtcall1(fn, ins, b, curi, KI32, k == KI32 ? "__popcountsi2" : "__popcountdi2", k, x);
      }
      break;
   case Oclrsb: 
      if (target.arch != ISaarch64) {
         /* clrsb(x) = clz(((x*2) ^ (x >> (N-1))) | 1) */
         int N = cls2siz[k]*8;
         Ref x = ins->l, t, w;
         t = insertinstr(b, (*curi)++, mkinstr2(Oshl, k, x, mkref(RICON, 1)));
         w = insertinstr(b, (*curi)++, mkinstr2(Osar, k, x, mkref(RICON, N-1)));
         t = insertinstr(b, (*curi)++, mkinstr2(Oxor, k, t, w));
         t = insertinstr(b, (*curi)++, mkinstr2(Oior, k, t, mkref(RICON, 1)));
         ins->op = Oclz;
         inschoper(b, ins, 0, t);
      }
      break;
   }
}

static const bool mayneedlower[NOPER] = {
   [Oclrsb]=1, [Opopcnt]=1,
};

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
               abi0_call(fn, ins, blk, &i, NOREF);
            svec_reset(args);
         } else if (mayneedlower[ins->op]) {
            arithlower(fn, ins, blk, &i);
         }
      }
      assert(args.n == 0);
   } while ((blk = blk->lnext) != fn->entry);
}

/* vim:set ts=3 sw=3 expandtab: */
