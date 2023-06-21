#include "ir.h"

/**  This pass adds in ABI arguments/returns register mappings
 **  and lowers aggregate params/args/returns into scalars
 **
 **  invariant: all `call` instructions when doing this pass shall be preceded by
 **  exactly narg `arg` instructions with no other instructions in between
 **/

struct abiargsvec { vec_of(struct abiarg); };

static int
abiret(struct abiarg abiret[2], struct abiargsvec *abiargs, int *ni, union irtype retty)
{
   short r[2];
   uchar cls[2];
   int retreg = 0;
   if (retty.isagg) {
      retreg = mctarg->abiret(r, cls, ni, retty);
      if (!retreg) {
         vpush(abiargs, ((struct abiarg) { cls2type(KPTR), .stk = r[1] }));
         if (r[0] == -1) {
            memset(abiret, 0, 2*sizeof *abiret);
         } else {
            abiret[0].ty = cls2type(KPTR);
            abiret[0].reg = r[0];
         }
      }
   } else if (retty.cls) {
      retreg = mctarg->abiret(r, cls, ni, retty);
      assert(retreg == 1);
   }
   for (int i = 0; i < retreg; ++i) {
      abiret[i].ty = cls2type(cls[i]);
      abiret[i].reg = r[i];
   }
   return retreg;
}

static int
abiarg(struct abiargsvec *abiargs, int *ni, int *nf, int *ns, union irtype ty)
{
   short r[2];
   uchar cls[2];
   int ret = mctarg->abiarg(r, cls, ni, nf, ns, ty);
   if (!ret) { /* in stack */
      vpush(abiargs, ((struct abiarg) { ty, .stk = r[0] }));
   } else if (ret == 1 && ty.isagg && cls[0] == KPTR) { /* aggregate by pointer */
      vpush(abiargs, ((struct abiarg) { cls2type(cls[0]), .reg = r[0] }));
   } else { /* by regs */
      vpush(abiargs, ((struct abiarg) { cls2type(cls[0]), .reg = r[0] }));
      if (ret == 2)
         vpush(abiargs, ((struct abiarg) { cls2type(cls[1]), .reg = r[1] }));
   }
   return ret;
}

static struct instr
copyparam(struct function *fn, int *curi, int param, struct abiarg abi)
{
   struct instr par = mkinstr(Oparam, abi.ty.cls, mkref(RICON, param), mktyperef(abi.ty));
   if (abi.reg >= 0) { /* reg */
      assert(!abi.ty.isagg);
      return par;
   } else if (!abi.ty.isagg) { /* scalar in stack */
      enum op ld;
      par.cls = KPTR;
      if (abi.ty.cls == KPTR) abi.ty.cls = siz2intcls[cls2siz[abi.ty.cls]];
      switch (abi.ty.cls) {
      default: assert(0);
      case KI4: ld = Oloadu4; break;
      case KI8: ld = Oloadi8; break;
      case KF4: ld = Oloadf4; break;
      case KF8: ld = Oloadf8; break;
      }
      return mkinstr(ld, abi.ty.cls, insertinstr(fn->entry, (*curi)++, par));
   } else { /* aggregate in stack */
      par.cls = KPTR;
      return par;
   }
}

static void
patchparam(struct function *fn, int *curi, int *param, int tydat, int nabi, struct abiarg abi[2])
{
   struct block *blk = fn->entry;
   assert(in_range(nabi,1,2));

   for (; *curi < blk->ins.n; ++*curi) {
      struct instr *ins = &instrtab[blk->ins.p[*curi]];
      if (ins->op != Oparam) continue;
      assert(ins->r.t == RTYPE
            && ins->r.i == (tydat < 0 ? abi[0].ty : (union irtype){.isagg=1, .dat=tydat}).bits);
      if (abi[0].ty.isagg || tydat < 0) {
         /* aggregate in stack or scalar, just copy */
         assert(nabi == 1);
         *ins = copyparam(fn, curi, *param, abi[0]);
      } else { /* aggregate in registers, materialize */
         union ref alloc, r[2];
         struct instr st;
         const struct typedata *td;
         uint nalloc;

         assert(tydat >= 0);
         td = &typedata[tydat];
         assert(td->siz <= 16 && td->align <= 16);
         nalloc = td->align == 16 ? 1 : td->siz/8 + (td->siz%8 != 0);
         *ins = mkinstr(Oalloca8 + (td->align==16), KPTR, mkref(RICON, nalloc));
         alloc = mkref(RTMP, ins - instrtab);
         r[0] = insertinstr(blk, ++*curi, copyparam(fn, NULL, *param, abi[0]));
         if (nabi > 1)
            r[1] = insertinstr(blk, ++*curi, copyparam(fn, NULL, ++*param, abi[1]));
         /* transform
          *  %x = copy %p
          * into
          *  %x = alloca...
          *  store* %x, %a
          *  store* %x + N, %b
          */
         st = mkinstr(Ostore1 + ilog2(cls2siz[abi[0].ty.cls]), 0, alloc, r[0]);
         insertinstr(blk, ++*curi, st);
         if (nabi > 1) {
            struct instr tmp = mkinstr(Oadd, KPTR, alloc, mkref(RICON, cls2siz[abi[0].ty.cls]));
            st = mkinstr(Ostore1 + ilog2(cls2siz[abi[1].ty.cls]), 0, insertinstr(blk, ++*curi, tmp), r[1]);
            insertinstr(blk, ++*curi, st);
         }
      }
      ++*param;
      ++*curi;
      break;
   }
}

static int
patcharg(struct block *blk, int *icall, struct call *call,
         int argidx, int nabi, struct abiarg abi[2])
{
   int arginst = *icall - (call->narg - argidx);
   struct instr *arg = &instrtab[blk->ins.p[arginst]];
   assert(arg->op == Oarg && arg->l.t == RTYPE);
   if (ref2type(arg->l).isagg) {
      /* aggregate argument */
      if (abi[0].ty.isagg  /* aggregate in stack */
       || abi[0].ty.cls == KPTR) /* aggregate by pointer */
      {
         return 1;
      } else { /* aggregate in registers */
         union ref src = arg->r;
         /* deconstruct into
          * %a = load* %x
          * (%b = load* %x + N)
          */
         delinstr(blk, arginst);
         for (int i = 0; i < nabi; ++i) {
            /* XXX this can generate unaligned loads */
            struct instr ins = {0};
            union ref temp;
            switch (ins.cls = abi[i].ty.cls) {
            default: assert(0);
            case KI4: ins.op = Oloadu4; break;
            case KI8: ins.op = Oloadi8; break;
            case KF4: ins.op = Oloadf4; break;
            case KF8: ins.op = Oloadf8; break;
            }
            if (i == 0)
               ins.l = src;
            else
               ins.l = insertinstr(blk, arginst++,
                                   mkinstr(Oadd, KPTR, src,
                                           mkref(RICON, cls2siz[abi[0].ty.cls])));
            temp = insertinstr(blk, arginst++, ins);
            insertinstr(blk, arginst++, mkarginstr(abi[i].ty, temp));
         }
         *icall += arginst - (call->narg - argidx);
         return nabi;
      }
   } else /* normal scalar argument */
      return 1;
}

static struct abiarg abiargsbuf[32];

void
abi0_call(struct function *fn, struct instr *ins, struct block *blk, int *curi)
{
   union ref retmem;
   struct abiargsvec abiargs = {VINIT(abiargsbuf, arraylength(abiargsbuf))};
   bool sretarghidden = 0;
   int ni, nf, ns, vararg, nret = 0;
   struct call *call = &calltab.p[ins->r.i];

   vararg = call->vararg;
   vinit(&abiargs, abiargsbuf, arraylength(abiargsbuf));
   ni = nf = ns = 0;
   assert(!ins->cls == !call->ret.bits);
   nret = abiret(call->abiret, &abiargs, &ni, call->ret);
   if (call->ret.isagg) { /* adjust struct return */
      union irtype retty = call->ret;
      struct instr alloca = { .cls = KPTR };
      struct typedata *td = &typedata[retty.dat];
      int ialloca ;

      sretarghidden = ni == 0;
      alloca.op = Oalloca8 + (td->align == 16);
      alloca.l = mkref(RICON, td->align == 16 ? 1 : td->siz / 8);
      /* swap alloca and call temps so users of original call point to alloca */
      retmem = insertinstr(blk, ialloca = (*curi)++ - call->narg, *ins);
      *ins = alloca;
      blk->ins.p[ialloca] = ins - instrtab;
      blk->ins.p[*curi] = retmem.i;
      ins = &instrtab[retmem.i];
      retmem.i = blk->ins.p[ialloca];

      if (!nret) /* hidden pointer argument */
         insertinstr(blk, (*curi)++ - call->narg,
                     mkinstr(Oarg, 0, mktyperef((union irtype){.cls=KPTR}), retmem));
   }

   /* adjust args */
   for (int i = 0, i2 = ni + sretarghidden; i < call->narg; ++i) {
      union irtype pty = ref2type(instrtab[blk->ins.p[*curi - call->narg + i]].l);
      int first = abiargs.n;
      int ret = abiarg(&abiargs, &ni, &nf, &ns, pty);
      ret = patcharg(blk, curi, call, i, ret, &abiargs.p[first]);
      if (call->vararg == i) vararg = i2;
      i2 += ret;
   }
   /* adjust return */
   if (call->ret.isagg) {
      ins->cls = 0;
      if (!nret) { /* hidden pointer argument */
         ins->cls = 0;
         if (call->abiret[0].reg >= 0) {
            /* the result location pointer is also returned by the callee, e.g. in x86 */
            ins->cls = KPTR;
            ++nret;
            /* even if this is not used, the register copy
             * must be emitted for the register allocator to know */
         }
      } else { /* aggregate returned in regs */
         union ref r[2];
         struct instr ret2;
         assert(in_range(nret, 1, 2));
         ins->cls = call->abiret[0].ty.cls;
         r[0] = mkref(RTMP, ins - instrtab);
         if (nret == 2) {
            ret2 = mkinstr(Ocall2r, call->abiret[1].ty.cls, r[0]);
            r[1] = insertinstr(blk, ++*curi, ret2);
         }
         for (int i = 0; i < nret; ++i) {
            struct instr store = {0};
            /* XXX this can generate unaligned stores */
            switch (call->abiret[i].ty.cls) {
            default: assert(0);
            case KF4: case KI4: store.op = Ostore4; break;
            case KI8: case KF8: store.op = Ostore8; break;
            }
            if (i == 0) {
               store.l = retmem;
            } else {
               union ref off = mkref(RICON, cls2siz[call->abiret[0].ty.cls]);
               struct instr addr = mkinstr(Oadd, KPTR, retmem, off);
               store.l = insertinstr(blk, ++*curi, addr);
            }
            store.r = r[i];
            insertinstr(blk, ++*curi, store);
         }
      }
   }

   if (call->ret.isagg) call->ret = (union irtype){0};
   call->vararg = vararg;
   call->abiarg = alloccopy(&fn->arena, abiargs.p, abiargs.n * sizeof(struct abiarg), 0);
   call->narg = abiargs.n;
   vfree(&abiargs);
}

void
abi0(struct function *fn)
{
   uint nparam = typedata[fn->fnty.dat].nmemb;
   const union type *paramty = typedata[fn->fnty.dat].param;
   struct abiargsvec abiargs = {VINIT(abiargsbuf, arraylength(abiargsbuf))};
   int rvovar = -1;
   int ni = 0, nf = 0, ns = 0, istart = 0;
   struct block *blk;
   union ref sret = {0};

   if (fn->retty.t == TYVOID) {
      fn->nabiret = 0;
   } else {
      fn->nabiret = abiret(fn->abiret, &abiargs, &ni, mkirtype(fn->retty));
      if (!fn->nabiret && isagg(fn->retty)) { /* ret agg by hidden pointer */
         struct instr param = copyparam(fn, NULL, 0, abiargs.p[0]);
         sret = insertinstr(fn->entry, 0, param);
         ++istart;
      }
   }

   /* adjust params */
   for (int i = 0, param = 0; i < nparam; ++i) {
      union irtype pty = mkirtype(paramty[i]);
      int first = abiargs.n;
      int ret = abiarg(&abiargs, &ni, &nf, &ns, pty);
      patchparam(fn, &istart, &param, pty.isagg ? pty.dat : -1, ret+!ret, &abiargs.p[first]);
   }
   fn->abiarg = alloccopy(&fn->arena, abiargs.p, abiargs.n * sizeof *abiargs.p, 0);
   fn->nabiarg = abiargs.n;
   vfree(&abiargs);

   if (!fn->nabiret && isagg(fn->retty)) {
      /* for structures returned by hidden pointer argument,
       * if all return instrs return local var X, make X point to the result location,
       * (return value optimization (RVO)) */
      blk = fn->entry;
      do {
         union ref arg = blk->jmp.arg[0];
         if (blk->jmp.t != Jret) continue;
         if (!arg.t) continue;
         if (arg.t != RTMP || !oisalloca(instrtab[arg.i].op)) {
            rvovar = -1;
            break;
         }
         if (rvovar == -1) {
            rvovar = arg.i;
         } else if (arg.i != rvovar) {
            rvovar = -1;
            break;
         }
      } while ((blk = blk->lnext) != fn->entry);
      if (rvovar != -1)
         instrtab[rvovar] = mkinstr(Ocopy, KPTR, sret);
   }

   blk = fn->entry->lnext;
   do {
      /* adjust calls */
      for (int iinstr = 0; iinstr < blk->ins.n; ++iinstr) {
         struct instr *ins = &instrtab[blk->ins.p[iinstr]];
         if (ins->op != Ocall) continue;
         abi0_call(fn, ins, blk, &iinstr);
      }

      /* adjust returns */
      if (isagg(fn->retty) && blk->jmp.t == Jret && blk->jmp.arg[0].t) {
         assert(!blk->jmp.arg[1].t);
         if (fn->nabiret) { /* aggregate return in register(s) */
            union ref src = blk->jmp.arg[0];
            for (int i = 0; i < fn->nabiret; ++i) {
               /* XXX this can generate unaligned loads */
               struct instr ins = {0};
               switch (ins.cls = fn->abiret[i].ty.cls) {
               default: assert(0);
               case KI4: ins.op = Oloadu4; break;
               case KI8: ins.op = Oloadi8; break;
               case KF4: ins.op = Oloadf4; break;
               case KF8: ins.op = Oloadf8; break;
               }
               if (i == 0)
                  ins.l = src;
               else
                  ins.l = insertinstr(blk, blk->ins.n,
                                      mkinstr(Oadd, KPTR, src,
                                              mkref(RICON, cls2siz[fn->abiret[0].ty.cls])));
               blk->jmp.arg[i] = insertinstr(blk, blk->ins.n, ins);
            }
         } else {
            /* aggregate return (arg[0] is pointer to return value) */
            if (rvovar == -1) {
               /* blit %sret, %arg */
               union irtype typ = mkirtype(fn->retty);
               insertinstr(blk, blk->ins.n, mkarginstr(typ, sret));
               insertinstr(blk, blk->ins.n, mkarginstr(typ, blk->jmp.arg[0]));
               insertinstr(blk, blk->ins.n, mkintrin(INstructcopy, 0, 2));
            } else assert(blk->jmp.arg[0].bits == mkref(RTMP, rvovar).bits);
            if (fn->abiret[0].ty.cls) blk->jmp.arg[0] = rvovar == -1 ? sret : mkref(RTMP, rvovar);
            else memset(blk->jmp.arg, 0, sizeof blk->jmp.arg);
         }
      }
   } while ((blk = blk->lnext) != fn->entry);

   if (ccopt.dbg.a) {
      efmt("<< After abi0 >>\n");
      irdump(fn);
   }
}

/* vim:set ts=3 sw=3 expandtab: */
