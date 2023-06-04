#include "ir.h"

/**  This pass adds in ABI arguments/returns register mappings
 **  and lowers aggregate params/args/returns into scalars
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
         vpush(abiargs, ((struct abiarg) { {.cls = KPTR}, r[1] }));
         if (r[0] == -1) {
            memset(abiret, 0, 2*sizeof *abiret);
         } else {
            abiret[0].ty = (union irtype) {.cls = KPTR};
            abiret[0].reg = r[0];
         }
      }
   } else if (retty.cls) {
      retreg = mctarg->abiret(r, cls, ni, retty);
      assert(retreg == 1);
   }
   for (int i = 0; i < retreg; ++i) {
      abiret[i].ty = (union irtype) {.cls = cls[i]};
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
   if (!ret) { /* aggregate in stack */
      vpush(abiargs, ((struct abiarg) { ty, -1 }));
   } else if (ret == 1 && ty.isagg && cls[0] == KPTR) { /* aggregate by pointer */
      vpush(abiargs, ((struct abiarg) { {.cls = cls[0]}, r[0] }));
   } else {
      vpush(abiargs, ((struct abiarg) { {.cls = cls[0]}, r[0] }));
      if (ret == 2)
         vpush(abiargs, ((struct abiarg) { {.cls = cls[1]}, r[1] }));
   }
   return ret;
}

/* RPARAM can only appear in the entry block (prologue), each RARG can only appear once.
 * this function patches param starting at instruction no. *start according to cls
 * to patch it to use arg no. `to' (and maybe also `to + 1') */
static void
patchparam(struct function *fn, int *start, int param, int tydat, int to, struct abiarg abi[2])
{
   struct block *blk = fn->entry;
   assert(!blk->phi.n);
   while((*start)++ < blk->ins.n) {
      struct instr *ins = &instrtab[blk->ins.p[*start - 1]];
      if (ins->op == Ocopy && ins->l.t == RPARAM && ins->l.i == param) {
         /* originally aggregate argument */
         assert(tydat != -1);
         if (abi[0].ty.isagg /* aggregate in stack */
          || abi[0].ty.cls == KPTR) /* aggregate by pointer */
         {
            ins->l.i = to;
         } else { /* aggregate in registers */
            const struct typedata *td = &typedata[tydat];
            /* transform
             *  %x = copy %argX
             * into
             *  %x = alloca...
             *  store* %x, %argN
             *  store* %x + I, %argM
             */
            assert(td->siz <= 16 && td->align <= 16);
            ins->op = Oalloca8 + (td->align == 16);
            ins->l = mkref(RICON, td->align == 16 ? 1 : td->siz / 8);
            insertinstr(blk, *start, mkinstr(Ostore1 + ilog2(cls2siz[abi[0].ty.cls]), 0,
                                          mkref(RTMP, ins - instrtab), mkref(RPARAM, to)));
            *start += 1;
            if (td->siz > 8) {
               struct instr tmp = mkinstr(Oadd, KPTR,
                                          mkref(RTMP, ins - instrtab), mkref(RICON, cls2siz[abi[0].ty.cls]));
               insertinstr(blk, *start+1, mkinstr(Ostore1 + ilog2(cls2siz[abi[1].ty.cls]), 0,
                           insertinstr(blk, *start, tmp), mkref(RPARAM, to+1)));
               *start += 2;
            }
         }
         break;
      } else if (oisstore(ins->op) && ins->r.t == RPARAM && ins->r.i == param) {
         /* normal scalar argument */
         assert(tydat == -1);
         ins->r.i = to;
         break;
      }
   }
}

static int
patcharg(union ref newargs[2], union irtype newtyps[2], struct block *blk, int *icall,
         struct call *call, int arg, int nabi, struct abiarg abi[2])
{
   if (call->typs[arg].isagg) {
      /* originally aggregate argument */
      if (abi[0].ty.isagg  /* aggregate in stack */
       || abi[0].ty.cls == KPTR) /* aggregate by pointer */
      {
         newargs[0] = call->args[arg];
         newtyps[0] = call->typs[arg];
      } else { /* aggregate in registers */
         union ref src = call->args[arg];
         /* deconstruct into
          * %a = load* %x
          * (%b = load* %x + N)
          */
         for (int i = 0; i < nabi; ++i) {
            /* XXX this can generate unaligned loads */
            struct instr ins = {0};
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
               ins.l = insertinstr(blk, (*icall)++ - 1,
                                   mkinstr(Oadd, KPTR, src,
                                           mkref(RICON, cls2siz[abi[0].ty.cls])));
            newtyps[i] = (union irtype) { .cls = ins.cls };
            newargs[i] = insertinstr(blk, (*icall)++ - 1, ins);
         }
         return nabi;
      }
   } else {
      /* normal scalar argument */
      newargs[0] = call->args[arg];
      newtyps[0] = call->typs[arg];
   }
   return 1;
}

void
abi0(struct function *fn)
{
   uint nparam = typedata[fn->fnty.dat].nmemb;
   const union type *paramty = typedata[fn->fnty.dat].param;
   static struct abiarg abiargsbuf[32];
   struct abiargsvec abiargs = {VINIT(abiargsbuf, arraylength(abiargsbuf))};
   int rvovar = -1;
   int ni = 0, nf = 0, ns = 0, istart = 0;
   struct block *blk;
   union ref sret;
   bool sretarghidden = 0;

   if (fn->retty.t == TYVOID) {
      fn->nabiret = 0;
   } else {
      fn->nabiret = abiret(fn->abiret, &abiargs, &ni, mkirtype(fn->retty));
      if (!fn->nabiret && isagg(fn->retty)) { /* ret by hidden pointer */
         sretarghidden = ni == 0;
         sret = insertinstr(fn->entry, 0, mkinstr(Ocopy, KPTR, mkref(RPARAM, 0)));
         ++istart;
      }
   }

   /* adjust params */
   for (int i = 0; i < nparam; ++i) {
      union irtype pty = mkirtype(paramty[i]);
      int thisi = sretarghidden + ni + nf + ns;
      int first = abiargs.n;
      int ret = abiarg(&abiargs, &ni, &nf, &ns, pty);
      if (i != thisi || (pty.isagg && ret))
         patchparam(fn, &istart, i, pty.isagg ? pty.dat : -1, thisi, &abiargs.p[first]);
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
         if (arg.t != RTMP || !oisalloca(instrtab[arg.idx].op)) {
            rvovar = -1;
            break;
         }
         if (rvovar == -1) {
            rvovar = arg.idx;
         } else if (arg.idx != rvovar) {
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
         struct call *call = &calltab.p[ins->r.idx];
         static union ref newargsbuf[32];
         static union irtype newtypsbuf[32];
         vec_of(union ref) newargs;
         vec_of(union irtype) newtyps;
         bool structbyval;
         int vararg;

         if (ins->op != Ocall) continue;

         vararg = call->vararg;
         vinit(&abiargs, abiargsbuf, arraylength(abiargsbuf));
         vinit(&newargs, newargsbuf, arraylength(newargsbuf));
         vinit(&newtyps, newtypsbuf, arraylength(newtypsbuf));
         if (!(structbyval = call->sret))
            for (int i = 0; i < call->narg; ++i)
               if ((structbyval = call->typs[i].isagg))
                     break;

         ni = nf = ns = 0;
         memset(call->abiret, 0, sizeof call->abiret);
         if (call->sret) {
            union irtype retty = call->typs[call->narg];
            int ret = abiret(call->abiret, &abiargs, &ni, retty);
            if (retty.isagg) {
               union ref temp;
               struct instr alloca = { .cls = KPTR };
               struct typedata *td = &typedata[retty.dat];

               sretarghidden = ni == 0;
               alloca.op = Oalloca8 + (td->align == 16);
               alloca.l = mkref(RICON, td->align == 16 ? 1 : td->siz / 8);
               temp = insertinstr(blk, iinstr++, alloca);
               replref(fn, blk, iinstr, mkref(RTMP, ins - instrtab), temp);
               if (!ret) { /* hidden pointer argument */
                  vpush(&newargs, temp);
                  vpush(&newtyps, (union irtype) {.cls = KPTR});
                  ins->cls = 0;
               } else {
                  union ref call2r;
                  int to = iinstr + 1;
                  assert(in_range(ret, 1, 2));
                  ins->cls = call->abiret[0].ty.cls;
                  if (ret == 2)
                     call2r = insertinstr(blk, to++, mkinstr(Ocall2r, call->abiret[1].ty.cls,
                                                             mkref(RTMP, ins - instrtab)));
                  for (int i = 0; i < ret; ++i) {
                     uchar cls;
                     struct instr store = {0};
                     /* XXX this can generate unaligned stores */
                     switch (cls = call->abiret[i].ty.cls) {
                     default: assert(0);
                     case KF4: case KI4: store.op = Ostore4; break;
                     case KI8: case KF8: store.op = Ostore8; break;
                     }
                     if (i == 0) {
                        store.l = temp;
                        store.r = mkref(RTMP, ins - instrtab);
                     } else {
                        store.l = insertinstr(blk, to++,
                                            mkinstr(Oadd, KPTR, temp,
                                                    mkref(RICON, cls2siz[call->abiret[0].ty.cls])));
                        store.r = call2r;
                     }
                     insertinstr(blk, to++, store);
                  }
               }
            }
         } else if (ins->cls) {
            int ret = abiret(call->abiret, &abiargs, &ni, (union irtype){.cls = ins->cls});
            assert(ret == 1 && !ni);
         }

         for (int i = 0; i < call->narg; ++i) {
            union irtype pty = call->typs[i];
            int thisi = sretarghidden + ni + nf + ns;
            int first = abiargs.n;
            int ret = abiarg(&abiargs, &ni, &nf, &ns, pty);
            union ref argss[2];
            union irtype typss[2];
            ret = patcharg(argss, typss, blk, &iinstr, call, i, ret, &abiargs.p[first]);
            if (structbyval) {
               vpushn(&newargs, argss, ret);
               vpushn(&newtyps, typss, ret);
            }
            if (call->vararg == i)
               vararg = thisi;
         }
         call->sret = 0;
         call->vararg = vararg;
         if (structbyval && newargs.n != call->narg) {
            call->args = alloc(&fn->arena, newargs.n * (sizeof *newargs.p + sizeof *newtyps.p), 0);
            call->typs = (union irtype *)((char *)call->args + newargs.n * sizeof *newargs.p);
         }
         memcpy(call->typs, newtyps.p, newtyps.n * sizeof *call->typs);
         if (structbyval)
            memcpy(call->args, newargs.p, newargs.n * sizeof *call->args);
         call->abiargregs = alloc(&fn->arena, abiargs.n * sizeof *call->abiargregs, 0);
         for (int i = 0; i < abiargs.n; ++i) call->abiargregs[i] = abiargs.p[i].reg;
         call->narg = abiargs.n;
         vfree(&abiargs);
         vfree(&newargs);
         vfree(&newtyps);
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
               union ref args[2] = { sret, blk->jmp.arg[0] };
               union irtype typ[2] = { mkirtype(fn->retty) };
               typ[1] = typ[0];
               insertinstr(blk, blk->ins.n, mkintrin(fn, INstructcopy, 0, 2, args, typ));
            } else assert(blk->jmp.arg[0].bits == mkref(RTMP, rvovar).bits);
            if (fn->abiret[0].ty.cls) blk->jmp.arg[0] = rvovar == -1 ? sret : mkref(RTMP, rvovar);
            else memset(blk->jmp.arg, 0, sizeof blk->jmp.arg);
         }
      }
   } while ((blk = blk->lnext) != fn->entry);
}

/* vim:set ts=3 sw=3 expandtab: */
