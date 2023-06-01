#include "common.h"
#include "ir.h"

/* This pass lowers aggregate params/args/returns into scalars, according to abi */

/* RARG can only appear in the entry block (prologue), each RARG can only appear once
 * this function patches arg starting at instruction no. *start according to cls
 * to redirect it to use arg no. `to' (and maybe also `to + 1')
 */
static void
patcharg(struct function *fn, int *start, int arg, int tydat, int to, uchar cls[2])
{
   struct block *blk = fn->entry;
   assert(!blk->phi.n);
   while((*start)++ < blk->ins.n) {
      struct instr *ins = &instrtab[blk->ins.p[*start - 1]];
      if (ins->op == Ocopy && ins->l.t == RARG && ins->l.i == arg) {
         /* originally aggregate argument */
         assert(tydat != -1);
         if (!cls[0]) { /* memory class */
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
            insertinstr(blk, *start, mkinstr(Ostore1 + ilog2(cls2siz[cls[0]]), 0,
                                          mkref(RTMP, ins - instrtab), mkref(RARG, to)));
            *start += 1;
            if (cls[1]) {
               struct instr tmp = mkinstr(Oadd, KPTR,
                                          mkref(RTMP, ins - instrtab), mkref(RICON, cls2siz[cls[0]]));
               insertinstr(blk, *start+1, mkinstr(Ostore1 + ilog2(cls2siz[cls[1]]), 0,
                           insertinstr(blk, *start, tmp), mkref(RARG, to+1)));
               *start += 2;
            }
         }
         break;
      } else if (oisstore(ins->op) && ins->r.t == RARG && ins->r.i == arg) {
         /* normal scalar argument */
         assert(tydat == -1);
         ins->r.i = to;
         break;
      }
   }
}

void
abistruct(struct function *fn)
{
   uint nparam = typedata[fn->fnty.dat].nmemb;
   const union type *paramty = typedata[fn->fnty.dat].param;
   vec_of(struct abiarg) abiargs = {0};
   int retreg = 0;
   struct abiarg retval[2];
   int ni = 0, nf = 0, mi = 0, istart = 0;
   short r[2];
   uchar cls[2];
   struct block *blk;
   union ref sret;

   if (isagg(fn->retty)) {
      retreg = mctarg->abi_retregs(r, cls, mkirtype(fn->retty));
      if (!retreg) {
         /* return location is first (pointer) argument */
         vpush(&abiargs, ((struct abiarg) { {.cls = KPTR}, r[0] }));
         ++ni;
         sret = insertinstr(fn->entry, 0, mkinstr(Ocopy, KPTR, mkref(RARG, 0)));
         ++istart;
      } else for (int i = 0; i < retreg; ++i) {
         /* return in 1 or 2 registers */
         retval[i].ty = (union irtype) {.cls = cls[i]};
         retval[i].reg = r[i];
      }
   }
   memcpy(fn->abiret, retval, sizeof retval);
   fn->nabiret = retreg;

   /* adjust params */
   for (int i = 0, newi; i < nparam; ++i) {
      union irtype pty = mkirtype(paramty[i]);
      int ret;
      assert(mctarg->abi_argregs);
      newi = ni + nf + mi;
      ret = mctarg->abi_argregs(r, cls, &ni, &nf, pty);
      if (!ret) { /* memory */
         vpush(&abiargs, ((struct abiarg) { pty, -1 }));
         ++mi;
      } else {
         vpush(&abiargs, ((struct abiarg) { {.cls = cls[0]}, r[0] }));
         if (ret == 2)
            vpush(&abiargs, ((struct abiarg) { {.cls = cls[1]}, r[1] }));
      }
      if (i != newi || (pty.isagg && ret))
         patcharg(fn, &istart, i, pty.isagg ? pty.dat : -1, newi, cls);
   }
   fn->abiarg = abiargs.p;
   fn->nabiarg = abiargs.n;

   /* adjust calls and returns */
   blk = fn->entry;
   do {
      for (int i = 0; i < blk->ins.n; ++i) {
         struct instr *ins = &instrtab[blk->ins.p[i]];
      }
      if (isagg(fn->retty) && blk->jmp.t == Jret && blk->jmp.arg[0].t) {
         /* aggregate return (arg[0] is pointer to return value) */
         assert(!blk->jmp.arg[1].t);
         if (retreg) {
            union ref src = blk->jmp.arg[0];
            for (int i = 0; i < retreg; ++i) {
               /* XXX this can generate unaligned loads */
               struct instr ins = {0};
               switch (ins.cls = retval[i].ty.cls) {
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
                                              mkref(RICON, cls2siz[retval[0].ty.cls])));
               blk->jmp.arg[i] = insertinstr(blk, blk->ins.n, ins);
            }
         } else {
            /* blit %sret, %arg */
            union ref args[2] = { sret, blk->jmp.arg[0] };
            union irtype typ[2] = { mkirtype(fn->retty) };
            typ[1] = typ[0];
            insertinstr(blk, blk->ins.n, mkbuiltin(fn, BTstructcopy, 0, 2, args, typ));
            memset(&blk->jmp.arg[0], 0, sizeof(union ref));
         }
      }
   } while ((blk = blk->lnext) != fn->entry);
}

/* vim:set ts=3 sw=3 expandtab: */
