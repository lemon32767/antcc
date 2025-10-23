#include "all.h"
#include "../endian.h"

enum flag {
   ZF = 1 << 0,
   SF = 1 << 1,
   CF = 1 << 2,
   OF = 1 << 3,
   CLOBF = 1 << 4,
};

/* flags modified by each integer op */
static const uchar opflags[] = {
   [Oneg]  = ZF|CLOBF,
   [Oadd]  = ZF|CLOBF,
   [Osub]  = ZF|CLOBF,
   [Omul]  = CLOBF,
   [Oumul] = CLOBF,
   [Odiv]  = CLOBF,
   [Oudiv] = CLOBF,
   [Orem]  = CLOBF,
   [Ourem] = CLOBF,
   [Oand]  = ZF|CLOBF,
   [Oior]  = ZF|CLOBF,
   [Oxor]  = ZF|CLOBF,
   [Oshl]  = ZF|CLOBF,
   [Osar]  = ZF|CLOBF,
   [Oslr]  = ZF|CLOBF,
   [Oequ]  = ZF|CLOBF,
   [Oneq]  = ZF|CLOBF,
   [Olth]  = ZF|CLOBF,
   [Ogth]  = ZF|CLOBF,
   [Olte]  = ZF|CLOBF,
   [Ogte]  = ZF|CLOBF,
   [Oulth] = ZF|CLOBF,
   [Ougth] = ZF|CLOBF,
   [Oulte] = ZF|CLOBF,
   [Ougte] = ZF|CLOBF,
   [Ocall] = CLOBF,
   [Oxinc] = ZF|CLOBF,
   [Oxdec] = ZF|CLOBF,
};

static int iflagsrc = -1;

static void
picfixsym(union ref *r, struct block *blk, int *curi)
{
   if (!ccopt.pic || !isaddrcon(*r)) return;
   *r = insertinstr(blk, (*curi)++, mkinstr(Ocopy, KPTR, .l = *r));
}

static void
fixarg(union ref *r, struct instr *ins, struct block *blk, int *curi)
{
   int sh;
   enum op op = ins ? ins->op : 0;

   if (r->t == RXCON) {
      struct xcon *con = &conht[r->i];
      if (in_range(op, Oshl, Oslr)) {
         sh = con->i;
         goto ShiftImm;
      } else if (in_range(op, Oadd, Osub) && con->i == 2147483648) {
         /* add X, INT32MAX+1 -> sub X, INT32MIN */
         ins->op = Oadd + (op == Oadd);
         *r = mkintcon(KI4, -2147483648);
      } else if (kisflt(con->cls) && con->f == 0) {
         /* copy of float zero -> regular zero, that emit() will turn into xor x,x */
         if (in_range(op, Ocopy, Omove) || op == Ophi)
            *r = ZEROREF;
         else
            *r = insertinstr(blk, (*curi)++, mkinstr(Ocopy, con->cls, ZEROREF));
      } else if (con->cls >= KI8) {
         /* float immediates & 64bit immediates are loaded from memory */
         uchar data[8];
         uint siz = cls2siz[con->cls];
         if (con->cls <= KPTR && in_range(ins->op, Ocopy, Omove)) /* in this case we can use movabs */
            return;
         if (con->cls != KF4) wr64le(data, con->i);
         else {
            union { float f; int i; } pun = { con->f };
            wr32le(data, pun.i);
         }
         *r = mkdatref(NULL, siz, /*align*/siz, data, siz, /*deref*/1);
         if (&ins->l != r && ins->l.t == RADDR) {
            /* can't use memory arg in rhs if lhs is memory */
            *r = insertinstr(blk, (*curi)++, mkinstr(Ocopy, con->cls, *r));
         }
      } else if (in_range(op, Odiv, Ourem) && kisint(ins->cls))
         goto DivImm;
   } else if (r->t == RICON && in_range(op, Odiv, Ourem) && kisint(ins->cls)) {
   DivImm: /* there is no division by immediate, must be copied to a register */
      *r = insertinstr(blk, (*curi)++, mkinstr(Ocopy, ins->cls, *r));
   } else if (r->t == RICON && in_range(op, Oshl, Oslr)) {
      sh = r->i;
   ShiftImm: /* shift immediate is always 8bit */
      *r = mkref(RICON, sh & 255);
   }
   picfixsym(r, blk, curi);
}

static void
selcall(struct function *fn, struct instr *ins, struct block *blk, int *curi)
{
   const struct call *call = &calltab.p[ins->r.i];
   int iarg = *curi - 1;
   enum irclass cls;

   for (int i = call->narg - 1; i >= 0; --i) {
      struct instr *arg;
      for (;;) {
         assert(i >= 0 && "arg?");
         if ((arg = &instrtab[blk->ins.p[iarg--]])->op == Oarg)
            break;
      }

      if (call->abiarg[i].reg >= 0) {
         assert(!call->abiarg[i].ty.isagg);
         *arg = mkinstr(Omove, call->abiarg[i].ty.cls, mkref(RREG, call->abiarg[i].reg), arg->r);
      }
   }
   cls = ins->cls;
   ins->cls = 0;
   if (cls) {
      /* duplicate to reuse same TMP ref */
      insertinstr(blk, (*curi)++, *ins);
      *ins = mkinstr(Ocopy, cls, mkref(RREG, call->abiret[0].reg));
      if (*curi + 1 < blk->ins.n)
      if (instrtab[blk->ins.p[*curi + 1]].op == Ocall2r) {
         ins = &instrtab[blk->ins.p[++*curi]];
         *ins = mkinstr(Ocopy, ins->cls, mkref(RREG, call->abiret[1].reg));
      }
   }
}

#define isimm32(r) (concls(r) == KI4)
#define rswap(a,b) do { union ref _t = (a); (a) = (b); (b) = _t; } while (0)

static bool
acon(struct addr *addr, union ref r)
{
   vlong a = addr->disp;
   assert(isintcon(r));
   a += intconval(r);
   if ((int)a == a) {
      addr->disp = a;
      return 1;
   }
   return 0;
}

static bool
ascale(struct addr *addr, union ref a, union ref b)
{
   if (b.t != RICON) return 0;
   if (addr->index.bits) return 0;
   if (a.t == RREG) {
   Scaled:
      if ((unsigned)b.i > 3) return 0;
      addr->index = a;
      addr->shift = b.i;
      return 1;
   } else if (a.t == RTMP) {
      struct instr *ins = &instrtab[a.i];
      /* factor out shifted immediate from 'shl {add %x, imm}, s' */
      /* XXX maybe we shouldn't do this here because it should be done by a generic
       * arithemetic optimization pass ? */
      if (ins->op == Oadd && (ins->l.t == RREG || ins->l.t == RTMP) && isintcon(ins->r)) {
         vlong a = ((vlong) addr->disp + intconval(ins->r)) << b.i;
         if (a != (int) a) return 0;
         addr->disp = a;
         addr->index = ins->l;
         addr->shift = b.i;
         return 1;
      } else {
         goto Scaled;
      }
   }
   return 0;
}

static bool
aadd(struct addr *addr, union ref r)
{
   if (r.t == RTMP) {
      struct instr *ins = &instrtab[r.i];

      if (ins->op == Oadd) {
         if (!aadd(addr, ins->l)) goto Ref;
         if (!aadd(addr, ins->r)) goto Ref;
         ins->skip = 1;
      } else if (ins->op == Oshl) {
         if (!ascale(addr, ins->l, ins->r)) goto Ref;
         ins->skip = 1;
      } else if (ins->op == Ocopy && ins->l.t == RADDR) {
         struct addr save = *addr, *addr2 = &addrht[ins->l.i];
         if ((!addr2->base.bits || aadd(addr, addr2->base))
          && acon(addr, mkintcon(KI4, addr2->disp))
          && (!addr2->index.bits || ascale(addr, addr2->index, mkref(RICON, addr2->shift))))
         {
            ins->skip = 1;
         } else {
            *addr = save;
            goto Ref;
         }
      } else if (ins->op == Ocopy) {
         if (!aadd(addr, ins->l)) goto Ref;
         ins->skip = 1;
      } else goto Ref;
   } else if (isnumcon(r)) {
      return acon(addr, r);
   } else if (isaddrcon(r)) {
      if (!addr->base.bits && !isaddrcon(addr->index)) addr->base = r;
      else return 0;
   } else if (r.t == RREG) {
      /* temporaries are single assignment, but register aren't, so they can't be *
       * safely hoisted into an address value, unless they have global lifetime */
      if (!rstest(mctarg->rglob, r.i)) return 0;
   Ref:
      if (!addr->base.bits) addr->base = r;
      else if (!addr->index.bits) addr->index = r;
      else return 0;
   } else return 0;
   return 1;
}

static bool
fuseaddr(union ref *r, struct block *blk, int *curi)
{
   struct addr addr = { 0 };

   if (r->t == RADDR) return 1;
   if (isaddrcon(*r)) return 1;
   if (r->t != RTMP) return 0;
   if (!aadd(&addr, *r)) return 0;

   if (isaddrcon(addr.base) && (ccopt.pic || (ccopt.pie && addr.index.bits))) {
      /* pic needs to load from GOT */
      /* pie cannot encode RIP-relative address with index register */
      /*     first load symbol address into a temp register */
      union ref temp = mkaddr((struct addr){.base = addr.base, .disp = ccopt.pic ? 0 : addr.disp});
      addr.base = insertinstr(blk, (*curi)++, mkinstr(Ocopy, KPTR, .l = temp));
      if (!ccopt.pic) addr.disp = 0;
   }

   *r = mkaddr(addr);
   return 1;
}

/* is add instruction with this arg a candidate to transform into efective addr? */
static bool
addarg4addrp(union ref r)
{
   struct instr *ins;
   if (r.t == RXCON && !conht[r.i].cls && !conht[r.i].deref) return 1; /* sym or dat ref */
   if (r.t != RTMP) return 0;
   ins = &instrtab[r.i];
   return ins->op == Oshl || (ins->op == Ocopy && ins->l.t == RADDR) || ins->op == Oadd;
}

static void
loadstoreaddr(struct block *blk, union ref *r, int *curi)
{
   if (isimm32(*r)) {
      *r = mkaddr((struct addr){.base = *r});
   } else if (!fuseaddr(r, blk, curi) && r->t != RTMP && r->t != RREG) {
      *r = insertinstr(blk, (*curi)++, mkinstr(Ocopy, KPTR, *r));
   } else {
      picfixsym(r, blk, curi);
   }
}

static void
sel(struct function *fn, struct instr *ins, struct block *blk, int *curi)
{
   uint siz, alignlog2;
   int t;
   struct instr temp = {0};
   enum op op = ins->op;

   switch (op) {
   default: assert(0);
   case Onop: break;
   case Oalloca1: case Oalloca2: case Oalloca4: case Oalloca8: case Oalloca16:
      alignlog2 = ins->op - Oalloca1;
      assert(ins->l.i > 0);
      siz = ins->l.i << alignlog2;
      fn->stksiz += siz;
      fn->stksiz = alignup(fn->stksiz, 1 << alignlog2);
      if (fn->stksiz > 1<<24) error(NULL, "'%s' stack frame too big", fn->name);
      *ins = mkinstr(Oadd, KPTR, mkref(RREG, RBP), mkref(RICON, -fn->stksiz));
      break;
   case Oparam:
      assert(ins->l.t == RICON && ins->l.i < fn->nabiarg);
      if (fn->abiarg[ins->l.i].reg >= 0)
         *ins = mkinstr(Ocopy, ins->cls, mkref(RREG, fn->abiarg[ins->l.i].reg));
      else /* stack */
         *ins = mkinstr(Oadd, KPTR, mkref(RREG, RBP), mkref(RICON, -fn->abiarg[ins->l.i].stk));
      break;
   case Oarg:
      fixarg(&ins->r, ins, blk, curi);
      break;
   case Ocall:
      selcall(fn, ins, blk, curi);
      break;
   case Ocall2r: assert(0);
   case Ointrin:
      break;
   case Oshl: case Osar: case Oslr:
      if (!iscon(ins->r)) {
         /* shift amount register is always CL */
         insertinstr(blk, (*curi)++, mkinstr(Omove, KI4, mkref(RREG, RCX), ins->r));
         ins->r = mkref(RREG, RCX);
      }
      goto ALU;
   case Oequ: case Oneq:
   case Olth: case Ogth: case Olte: case Ogte:
   case Oulth: case Ougth: case Oulte: case Ougte:
      if (iscon(ins->l)) {
         /* lth imm, x -> gth x, imm */
         if (!in_range(ins->op, Oequ, Oneq))
            ins->op = ((op - Olth) ^ 1) + Olth;
         rswap(ins->l, ins->r);
      }
      if (ins->l.t != RTMP && ins->l.t != RREG)
         ins->l = insertinstr(blk, (*curi)++, mkinstr(Ocopy, ins->cls, ins->l));
      fixarg(&ins->r, ins, blk, curi);
      break;
   case Odiv: case Oudiv: case Orem: case Ourem:
      if (kisflt(ins->cls)) goto ALU;
      /* TODO fuse div/rem pair */

      /* (I)DIV dividend is always in RDX:RAX, output also in those regs */
      insertinstr(blk, (*curi)++, mkinstr(Omove, ins->cls, mkref(RREG, RAX), ins->l));
      /* mark RDX as clobbered. sign/zero-extending RAX into RDX is handled in emit() */
      insertinstr(blk, (*curi)++, mkinstr(Omove, ins->cls, mkref(RREG, RDX), mkref(RREG, RDX)));
      fixarg(&ins->r, ins, blk, curi); /* make sure rhs is memory or reg */
      ins->l = mkref(RREG, RAX);
      ins->keep = 1;
      if (op == Orem) ins->op = Odiv;
      else if (op == Ourem) ins->op = Oudiv;
      insertinstr(blk, (*curi)++, *ins); /* duplicate ins to reuse tmp ref */
      *ins = mkinstr(Ocopy, ins->cls, mkref(RREG, op < Orem ? RAX : RDX)); /* get output */
      temp = mkinstr(Ocopy, ins->cls, mkref(RREG, op < Orem ? RDX : RAX)); /* clobber other reg*/
      insertinstr(blk, ++(*curi), temp);
      /* swap instrs so that clobber goes first */
      t = blk->ins.p[*curi - 1];
      blk->ins.p[*curi - 1] = blk->ins.p[*curi - 0];
      blk->ins.p[*curi - 0] = t;
      break;
   case Osub:
       if (ins->r.bits == mkref(RICON, 1).bits) {
         /* sub x,1 -> dec x */
         ins->op = op = Oxdec;
         ins->r = NOREF;
      } else if (isintcon(ins->l)) {
         /* sub imm, x -> sub x, imm; neg x */
         struct instr sub = *ins;
         rswap(sub.l, sub.r);
         ins->op = Oneg;
         ins->l = insertinstr(blk, (*curi)++, sub);
         ins->r = NOREF;
      }
      goto ALU;
   case Oadd:
      if (kisint(ins->cls)) {
         if ((addarg4addrp(ins->l) || addarg4addrp(ins->r))) {
            temp.op = Ocopy;
            temp.cls = ins->cls;
            temp.l = mkref(RTMP, ins - instrtab);
            if (fuseaddr(&temp.l, blk, curi)) {
               *ins = temp;
               break;
            }
         } else if (ins->l.bits == mkref(RICON, 1).bits) {
            /* add 1,x -> inc x */
            ins->op = op = Oxinc;
            ins->l = ins->r;
            ins->r = NOREF;
            goto ALU;
         } else if (ins->r.bits == mkref(RICON, 1).bits) {
            /* add x,1 -> inc x */
            ins->op = op = Oxinc;
            ins->r = NOREF;
            goto ALU;
         }
      }
      /* fallthru */
   case Omul: case Oumul:
   case Oand: case Oxor: case Oior:
      /* commutative ops */
      if (iscon(ins->l))
         rswap(ins->l, ins->r);
   case Oneg: case Onot:
   ALU:
      if (!(op == Oadd && kisint(ins->cls))) /* 3-address add is lea */
      if (!(in_range(op, Omul, Oumul) && kisint(ins->cls) && isimm32(ins->r))) /* for (I)MUL r,r/m,imm */
         ins->inplace = 1;
      if (iscon(ins->l))
         ins->l = insertinstr(blk, (*curi)++, mkinstr(Ocopy, ins->cls, ins->l));
      if (ins->r.bits)
   case Omove:
         fixarg(&ins->r, ins, blk, curi);
      break;
   case Oloads1: case Oloadu1: case Oloads2: case Oloadu2:
   case Oloads4: case Oloadu4: case Oloadi8: case Oloadf4: case Oloadf8:
      loadstoreaddr(blk, &ins->l, curi);
      break;
   case Ostore1: case Ostore2: case Ostore4: case Ostore8:
      loadstoreaddr(blk, &ins->l, curi);
      if (isaddrcon(ins->r))
         ins->r = insertinstr(blk, (*curi)++, mkinstr(Ocopy, KPTR, ins->r));
      else
         fixarg(&ins->r, ins, blk, curi);
      break;
   case Ocvtu4f:
      fixarg(&ins->l, ins, blk, curi);
      ins->l = insertinstr(blk, (*curi)++, mkinstr(Oextu4, KI8, ins->l));
      ins->op = Ocvts8f;
      break;
   case Ocvtf4u: case Ocvtf8u:
      fixarg(&ins->l, ins, blk, curi);
      if (ins->cls == KI4) {
         ins->l = insertinstr(blk, (*curi)++, mkinstr(ins->op == Ocvtf4u ? Ocvtf4s : Ocvtf8s, KI8, ins->l));
         ins->op = Oextu4;
     } else assert(!"nyi flt -> u64");
      break;
   case Ocvtf4f8: case Ocvtf8f4: case Ocvtf4s: case Ocvtf8s: case Ocvts4f: case Ocvts8f:
   case Ocvtu8f:
   case Oexts1: case Oextu1: case Oexts2: case Oextu2: case Oexts4: case Oextu4:
   case Ocopy:
      fixarg(&ins->l, ins, blk, curi);
      break;
   }
}

static void
seljmp(struct function *fn, struct block *blk)
{
   if (blk->jmp.t == Jb && blk->jmp.arg[0].bits) {
      union ref c = blk->jmp.arg[0];
      if (c.t != RTMP) {
         enum irclass cls = c.t == RICON ? KI4 : c.t == RXCON && conht[c.i].cls ? conht[c.i].cls : KPTR;
         int curi = blk->ins.n;

         c = insertinstr(blk, blk->ins.n, mkinstr(Ocopy, cls, c));
         sel(fn, &instrtab[c.i], blk, &curi);
      }
      if (iflagsrc == c.i             /*           test                   cmp              */
       && (oiscmp(instrtab[c.i].op) || instrtab[c.i].op == Oand || instrtab[c.i].op == Osub)) {
         instrtab[c.i].keep = 1;
      } else {
         if (!(opflags[instrtab[c.i].op] & ZF) || c.i != blk->ins.p[blk->ins.n - 1]) {
            blk->jmp.arg[0] = insertinstr(blk, blk->ins.n, mkinstr(Oneq, instrtab[c.i].cls, c, ZEROREF));
            instrtab[blk->jmp.arg[0].i].keep = 1;
         }
      }
   } else if (blk->jmp.t == Jret) {
      if (blk->jmp.arg[0].bits) {
         int curi;
         union ref r = mkref(RREG, fn->abiret[0].reg);
         struct instr *ins = &instrtab[insertinstr(blk, blk->ins.n, mkinstr(Omove, fn->abiret[0].ty.cls, r , blk->jmp.arg[0])).i];
         curi = blk->ins.n;
         fixarg(&ins->r, ins, blk, &curi);
         blk->jmp.arg[0] = r;
         if (blk->jmp.arg[1].bits) {
            r = mkref(RREG, fn->abiret[1].reg);
            ins = &instrtab[insertinstr(blk, blk->ins.n, mkinstr(Omove, fn->abiret[1].ty.cls, r, blk->jmp.arg[1])).i];
            curi = blk->ins.n;
            fixarg(&ins->r, ins, blk, &curi);
            blk->jmp.arg[1] = r;
         }
      }
   }
}

void
amd64_isel(struct function *fn)
{
   struct block *blk = fn->entry;

   fn->stksiz = 0;

   do {
      int i;
      for (i = 0; i < blk->phi.n; ++i) {
         struct instr *ins = &instrtab[blk->phi.p[i]];
         union ref *phi = phitab.p[ins->l.i];
         for (int i = 0; i < blk->npred; ++i) {
            int curi = blkpred(blk, i)->ins.n;
            fixarg(&phi[i], ins, blkpred(blk, i), &curi);
         }
      }
      iflagsrc = -1;
      for (i = 0; i < blk->ins.n; ++i) {
         struct instr *ins = &instrtab[blk->ins.p[i]];
         sel(fn, ins, blk, &i);
         if (ins->op < arraylength(opflags) && kisint(insrescls(*ins))) {
            if (opflags[ins->op] & ZF) iflagsrc = ins - instrtab;
            else if (opflags[ins->op] & CLOBF) iflagsrc = -1;
         }
      }
      seljmp(fn, blk);
   } while ((blk = blk->lnext) != fn->entry);

   if (ccopt.dbg.i) {
      efmt("<< After isel >>\n");
      irdump(fn);
   }
}

/* vim:set ts=3 sw=3 expandtab: */
