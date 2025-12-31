#include "ir.h"

/**  This pass adds in ABI arguments/returns register mappings
 **  and lowers aggregate params/args/returns into scalars
 **
 **  invariant: all `call` instructions when doing this pass shall be preceded by
 **  exactly narg `arg` instructions with no other instructions in between
 **/

struct abiargsvec { vec_of(struct abiarg); };

static int
abiret(struct abiarg abiret[2], struct abiargsvec *abiargs, uchar *r2off, int *ni, union irtype retty)
{
   short r[2];
   uchar cls[2];
   int retreg = 0;
   retreg = mctarg->abiret(r, cls, r2off, ni, retty);
   if (retty.isagg) {
      if (!retreg) {
         vpush(abiargs, ((struct abiarg) { cls2type(KPTR), .reg = r[1] }));
         if (r[0] == -1) {
            memset(abiret, 0, 2*sizeof *abiret);
         } else {
            abiret[0].ty = cls2type(KPTR);
            abiret[0].reg = r[0];
         }
      }
   } else if (retty.cls) {
      assert(retreg == 1);
   }
   for (int i = 0; i < retreg; ++i) {
      abiret[i].ty = cls2type(cls[i]);
      abiret[i].isstk = 0;
      abiret[i].reg = r[i];
   }
   return retreg;
}

static int
abiarg(struct abiargsvec *abiargs, uchar *r2off, int *ni, int *nf, int *ns, union irtype ty)
{
   short r[2];
   uchar cls[2];
   int ret = mctarg->abiarg(r, cls, r2off, ni, nf, ns, ty);
   if (!ret) { /* in stack */
      vpush(abiargs, ((struct abiarg) { ty, .isstk = 1, .stk = r[0] }));
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
   if (!abi.isstk) { /* reg */
      assert(!abi.ty.isagg);
      return par;
   }
   par.r = mktyperef((union irtype){.cls = KPTR});
   if (!abi.ty.isagg) { /* scalar in stack */
      enum op ld;
      par.cls = KPTR;
      if (abi.ty.cls == KPTR) abi.ty.cls = siz2intcls[cls2siz[abi.ty.cls]];
      switch (abi.ty.cls) {
      default: assert(0);
      case KI32: ld = Oloads32; break;
      case KI64: ld = Oloadi64; break;
      case KF32: ld = Oloadf32; break;
      case KF64: ld = Oloadf64; break;
      }
      return mkinstr(ld, abi.ty.cls, insertinstr(fn->entry, (*curi)++, par));
   } else { /* aggregate in stack */
      par.cls = KPTR;
      return par;
   }
}

static void
patchparam(struct function *fn, int *curi, int *param, int tydat, int nabi, struct abiarg abi[2], uchar r2off)
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
         uint align;

         assert(tydat >= 0);
         td = &typedata[tydat];
         assert(td->siz <= 16 && td->align <= 16);
         align = td->siz <= 4 ? 4 : alignup(td->align, 8);
         nalloc = td->siz/align + (td->siz%align != 0);
         *ins = mkinstr(Oalloca1 + ilog2(align), KPTR, mkref(RICON, nalloc));
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
         st = mkinstr(cls2store[abi[0].ty.cls], 0, alloc, r[0]);
         insertinstr(blk, ++*curi, st);
         if (nabi > 1) {
            struct instr tmp = mkinstr(Oadd, KPTR, alloc, mkref(RICON, r2off));
            st = mkinstr(cls2store[abi[1].ty.cls], 0, insertinstr(blk, ++*curi, tmp), r[1]);
            insertinstr(blk, ++*curi, st);
         }
      }
      ++*param;
      ++*curi;
      break;
   }
}

static void
load2regs(union ref out[2], union irtype typ, union ref src, int nabi, struct abiarg abi[2], uchar r2off, struct block *blk, int *curi)
{
   uint align = typedata[typ.dat].align;
   uint siz = typedata[typ.dat].siz;
   if (src.t == RTMP && oisalloca(instrtab[src.i].op)) {
      /* use actual alignment as opposed to min required type alignment */
      uint aalign = 1 << (instrtab[src.i].op - Oalloca1);
      assert(aalign >= align);
      align = aalign;
   }
   /* deconstruct into
    * %a = load* %x
    * (%b = load* %x + N)
    */
   /* XXX this generates pretty bad code for small-alignment structs even on platforms where unaligned loads are available.. */
   if (align >= 4) {
      for (int i = 0; i < nabi; ++i) {
         struct instr ins = {0};
         union ref temp;
         switch (ins.cls = abi[i].ty.cls) {
         default: assert(0);
         case KI32: ins.op = Oloadu32; break;
         case KI64: ins.op = Oloadi64; break;
         case KF32: ins.op = Oloadf32; break;
         case KF64: ins.op = Oloadf64; break;
         }
         if (i == 0)
            ins.l = src;
         else {
            struct instr adr = mkinstr(Oadd, KPTR, src, mkref(RICON, r2off));
            ins.l = insertinstr(blk, (*curi)++, adr);
         }
         temp = insertinstr(blk, (*curi)++, ins);
         //insertinstr(blk, (*curi)++, mkarginstr(abi[i].ty, temp));
         out[i] = temp;
      }
   } else {
      for (int i = 0; i < nabi; ++i) {
         struct instr ld = {0};
         union ref reg, temp;
         uint n = cls2siz[abi[i].ty.cls] / align;
         assert(n > 0);
         ld.op = Oloadu8 + ilog2(align)*2;
         ld.cls = abi[i].ty.cls;
         for (int o = 0; o < n && (i*cls2siz[ld.cls])+o*align < siz; ++o) {
            if (i+o == 0)
               ld.l = src;
            else {
               struct instr adr = mkinstr(Oadd, KPTR, src, mkref(RICON, (i == 0 ? 0 : r2off) + o*align));
               ld.l = insertinstr(blk, (*curi)++, adr);
            }
            temp = insertinstr(blk, (*curi)++, ld);
            if (o > 0) {
               union ref t = insertinstr(blk, (*curi)++, mkinstr(Oshl, ld.cls, temp, mkref(RICON, o*align*8)));
               reg = insertinstr(blk, (*curi)++, mkinstr(Oior, ld.cls, reg, t));
            } else {
               reg = temp;
            }
         }
         //insertinstr(blk, arginst++, mkarginstr(abi[i].ty, reg));
         out[i] = reg;
      }
   }
}

static int
patcharg(struct block *blk, int *icall, struct call *call,
         int argidx, int nabi, struct abiarg abi[2], uchar r2off)
{
   int arginst = *icall - (call->narg - argidx);
   struct instr *arg = &instrtab[blk->ins.p[arginst]];
   assert(arg->op == Oarg && arg->l.t == RTYPE);
   if (ref2type(arg->l).isagg) { /* aggregate argument */
      if (abi[0].ty.isagg) {  /* aggregate in stack */
         /* XXX do this better.. */
         /* ptr %dst = arg <stk dst> */
         /* (blit %dst, %src) */
         union ref dst = mkref(RTMP, arg - instrtab);
         uint align = typedata[abi->ty.dat].align, siz = typedata[abi->ty.dat].siz;
         union ref src = arg->r;
         if (src.t == RTMP && oisalloca(instrtab[src.i].op)) {
            align = 1 << (instrtab[src.i].op - Oalloca1);
         }
         assert(align <= 8);
         arg->cls = KPTR;
         arg->r = mkref(RICON, abi->stk);
         for (uint off = 0; off < siz; off += align) {
            union ref sadr = off == 0 ? src : insertinstr(blk, ++arginst, mkinstr(Oadd, KPTR, src, mkref(RICON, off)));
            union ref tmp = insertinstr(blk, ++arginst, mkinstr(Oloads8+2*ilog2(align), align < 8 ? KI32 : KI64, sadr));
            union ref dadr = off == 0 ? dst : insertinstr(blk, ++arginst, mkinstr(Oadd, KPTR, dst, mkref(RICON, off)));
            insertinstr(blk, ++arginst, mkinstr(Ostorei8+ilog2(align), 0, dadr, tmp));
         }
         *icall = arginst + (call->narg - argidx);
         return 1;
      } else if (abi[0].ty.cls == KPTR) { /* aggregate by pointer */
         arg->cls = KPTR;
         return 1;
      } else { /* aggregate in registers */
         union ref r[2];
         delinstr(blk, arginst);
         load2regs(r, ref2type(arg->l), arg->r, nabi, abi, r2off, blk, &arginst);
         for (int i = 0; i < nabi; ++i)
            insertinstr(blk, arginst++, mkinstr(Oarg, 0, mktyperef(abi[i].ty), r[i]));
         *icall = arginst + (call->narg - argidx - 1);
         return nabi;
      }
   } else { /* normal scalar argument */
      return 1;
   }
}
void
abi0_call(struct function *fn, struct instr *ins, struct block *blk, int *curi)
{
   union ref retmem;
   struct abiarg abiargsbuf[32];
   struct abiargsvec abiargs = {VINIT(abiargsbuf, countof(abiargsbuf))};
   bool sretarghidden = 0;
   int ni, nf, ns, vararg, nret = 0;
   struct call *call = &calltab.p[ins->r.i];

   vararg = call->vararg;
   ni = nf = ns = 0;
   assert(!ins->cls == !call->ret.bits);
   nret = abiret(call->abiret, &abiargs, &call->r2off, &ni, call->ret);
   if (call->ret.isagg) { /* adjust struct return */
      union irtype retty = call->ret;
      struct typedata *td = &typedata[retty.dat];
      uint align = td->align, ralign;
      struct instr alloca;
      int ialloca;
      for (int i = 0; i < nret; ++i)
         align = align < (ralign = cls2siz[call->abiret[i].ty.cls]) ? ralign : align;
      alloca = mkalloca(td->siz, align);

      sretarghidden = ni == 0;
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
      int arginst = *curi - (call->narg - i);
      struct instr *arg = &instrtab[blk->ins.p[arginst]];
      union irtype pty = ref2type(arg->l);
      uchar r2off;
      int first = abiargs.n;
      int ret = abiarg(&abiargs, &r2off, &ni, &nf, &ns, pty);
      ret = patcharg(blk, curi, call, i, ret, &abiargs.p[first], r2off);
      if (call->vararg == i) vararg = i2;
      i2 += ret;
   }
   call->argstksiz = ns;
   /* adjust return */
   if (call->ret.isagg) {
      ins->cls = 0;
      if (!nret) { /* hidden pointer argument */
         ins->cls = 0;
         if (!call->abiret[0].isstk) {
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
            struct instr store = { cls2store[call->abiret[i].ty.cls] };
            if (i == 0) {
               store.l = retmem;
            } else {
               struct instr addr = mkinstr(Oadd, KPTR, retmem, mkref(RICON, call->r2off));
               store.l = insertinstr(blk, ++*curi, addr);
            }
            store.r = r[i];
            insertinstr(blk, ++*curi, store);
         }
      }
   }

   if (call->ret.isagg) call->ret = (union irtype){0};
   call->vararg = vararg;
   call->abiarg = alloccopy(fn->arena, abiargs.p, abiargs.n * sizeof(struct abiarg), 0);
   call->narg = abiargs.n;
   vfree(&abiargs);
}

void
abi0(struct function *fn)
{
   struct abiarg abiargsbuf[32];
   uint nparam = typedata[fn->fnty.dat].nmemb;
   const union type *paramty = typedata[fn->fnty.dat].param;
   struct abiargsvec abiargs = {VINIT(abiargsbuf, countof(abiargsbuf))};
   int rvovar = -1;
   int ni = 0, nf = 0, ns = 0, istart = 0;
   uchar r2off;
   struct block *blk;
   union ref sret = {0};

   FREQUIRE(FNUSE);

   if (fn->retty.t == TYVOID) {
      fn->nabiret = 0;
   } else {
      fn->nabiret = abiret(fn->abiret, &abiargs, &r2off, &ni, mkirtype(fn->retty));
      if (!fn->nabiret && isagg(fn->retty)) { /* ret agg by hidden pointer */
         struct instr param = copyparam(fn, NULL, 0, abiargs.p[0]);
         sret = insertinstr(fn->entry, 0, param);
         ++istart;
         /* increment real param ordinals */
         for (int i = 1; i < fn->entry->ins.n; ++i) {
            struct instr *ins = &instrtab[fn->entry->ins.p[i]];
            if (ins->op == Oparam) ++ins->l.i;
         }
      }
   }

   /* adjust params */
   for (int i = 0, param = abiargs.n; i < nparam; ++i) {
      union irtype pty = mkirtype(paramty[i]);
      int first = abiargs.n;
      uchar r2off;
      int ret = abiarg(&abiargs, &r2off, &ni, &nf, &ns, pty);
      patchparam(fn, &istart, &param, pty.isagg ? pty.dat : -1, ret+!ret, &abiargs.p[first], r2off);
   }
   fn->abiarg = alloccopy(fn->arena, abiargs.p, abiargs.n * sizeof *abiargs.p, 0);
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
         if (!arg.bits) continue;
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
   int id = 1;
   do {
      /* adjust vaargs and calls */
      for (int iinstr = 0; iinstr < blk->ins.n; ++iinstr) {
         struct instr *ins = &instrtab[blk->ins.p[iinstr]];
         if (ins->op == Ovastart) mctarg->vastart(fn, blk, &iinstr);
         else if (ins->op == Ovaarg) mctarg->vaarg(fn, blk, &iinstr);
         else if (ins->op == Ocall) abi0_call(fn, ins, blk, &iinstr);
      }

      /* adjust returns */
      if (isagg(fn->retty) && blk->jmp.t == Jret && blk->jmp.arg[0].bits) {
         assert(!blk->jmp.arg[1].bits);
         if (fn->nabiret) { /* aggregate return in register(s) */
            union ref r[2];
            int curi = blk->ins.n;
            load2regs(r, mkirtype(fn->retty), blk->jmp.arg[0], fn->nabiret, fn->abiret, r2off, blk, &curi);
            for (int i = 0; i < fn->nabiret; ++i)
               blk->jmp.arg[i] = r[i];
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
      blk->id = id++;
   } while ((blk = blk->lnext) != fn->entry);


   /* vaargs might break these */
   if (!(fn->prop & FNUSE)) filluses(fn);
   fn->prop &= ~(FNBLKID | FNRPO);

   if (ccopt.dbg.a) {
      bfmt(ccopt.dbgout, "<< After abi0 >>\n");
      irdump(fn);
   }
}

/* vim:set ts=3 sw=3 expandtab: */
