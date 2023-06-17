#include "all.h"

static void
fixarg(struct function *fn, union ref *r, struct instr *ins, struct block *blk, int *curi)
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
      } else if (in_range(op, Ocopy, Omove) && kisflt(con->cls) && con->f == 0) {
         /* copy of float zero -> regular zero, that emit() will turn into xor x,x */
         *r = mkref(RICON, 0);
      } else if (kisflt(con->cls) || con->cls == KI8) {
         /* float immediates & >32b immediates are loaded from memory */
         uchar data[8];
         uint siz = cls2siz[con->cls];
         if (ins) assert(ins->cls == con->cls);
         if (con->cls == KI4) wr32le(data, con->i);
         else if (con->cls != KF4) wr64le(data, con->i);
         else {
            union { float f; int i; } pun = { con->f };
            wr32le(data, pun.i);
         }
         *r = mkdatref(siz, /*align*/siz, data, siz, /*deref*/1);
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
   if (addr->index.t) return 0;
   if (a.t != RTMP && a.t != RREG) return 0;
   if ((unsigned)b.i > 3) return 0;
   addr->shift = b.i;
   addr->index = a;
   return 1;
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
      } else if (ins->op == Ocopy && ins->l.t == RMORE) {
         struct addr save = *addr, *addr2 = &addrht[ins->l.i];
         if ((!addr2->base.t || aadd(addr, addr2->base))
          && acon(addr, mkintcon(KI4, addr2->disp))
          && (!addr2->index.t || ascale(addr, addr2->index, mkref(RICON, addr2->shift))))
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
   } else if (iscon(r)) {
      return acon(addr, r);
   } else if (r.t == RREG) {
      /* temporaries are single assignment, but register aren't, so they can't be *
       * safely hoisted into an address value, unless they have global lifetime */
      if (!bstest(mctarg->rglob, r.i)) return 0;
   Ref:
      if (!addr->base.t) addr->base = r;
      else if (!addr->index.t) addr->index = r;
      else return 0;
   } else return 0;
   return 1;
}

static bool
fuseaddr(struct function *fn, union ref *r)
{
   struct addr addr = { 0 };

   if (r->t == RMORE) return 1;
   if (r->t != RTMP) return 0;

   if (!aadd(&addr, *r)) return 0;

   *r = mkaddr(addr);
   return 1;
}

/* is add instruction with this arg a candidate to transform into efective addr? */
static bool
addarg4addrp(union ref r)
{
   struct instr *ins = &instrtab[r.i];
   if (r.t != RTMP) return 0;
   return ins->op == Oshl || (ins->op == Ocopy && ins->l.t == RMORE) || ins->op == Oadd;
}

static void
sel(struct function *fn, struct instr *ins, struct block *blk, int *curi)
{
   uint siz, alignlog2;
   struct instr temp = {0};
   enum op op = ins->op;

   switch (op) {
   default: assert(0);
   case Onop: break;
   case Oalloca1: case Oalloca2: case Oalloca4: case Oalloca8: case Oalloca16:
      alignlog2 = ins->op - Oalloca1;
      siz = ins->l.i << alignlog2;
      fn->stksiz += siz;
      fn->stksiz = alignup(fn->stksiz, 1 << alignlog2);
      if (fn->stksiz > 1<<24) error(NULL, "'%s' stack frame too big", fn->name);
      *ins = mkinstr(Oadd, KPTR, mkref(RREG, mctarg->bpr), mkref(RICON, -fn->stksiz));
      break;
   case Ocall: case Ointrin:
      break;
   case Oshl: case Osar: case Oslr:
      if (!iscon(ins->r)) {
         /* shift amount register is always CL */
         insertinstr(blk, (*curi)++, mkinstr(Omove, KI4, mkref(RREG, RCX), ins->r));
         ins->r = mkref(RREG, RCX);
      }
      goto ALU;
   case Olth: case Ogth: case Olte: case Ogte:
   case Oulth: case Ougth: case Oulte: case Ougte:
      if (iscon(ins->l)) {
         /* lth imm, x -> gth x, imm */
         ins->op = ((op - Olth) ^ 1) + Olth;
         rswap(ins->l, ins->r);
      }
      goto ALU;
   case Odiv: case Oudiv: case Orem: case Ourem:
      if (kisflt(ins->cls)) goto ALU;
      /* TODO fuse div/rem pair */

      /* (I)DIV dividend is always in RDX:RAX, output also in those regs */
      insertinstr(blk, (*curi)++, mkinstr(Omove, ins->cls, mkref(RREG, RAX), ins->l));
      /* mark RDX as clobbered. sign/zero-extending RAX into RDX is handled in emit() */
      insertinstr(blk, (*curi)++, mkinstr(Omove, ins->cls, mkref(RREG, RDX), mkref(RREG, RDX)));
      fixarg(fn, &ins->r, ins, blk, curi); /* make sure rhs is memory or reg */
      ins->l = mkref(RREG, RAX);
      if (op == Orem) ins->op = Odiv;
      else if (op == Ourem) ins->op = Oudiv;
      insertinstr(blk, (*curi)++, *ins); /* duplicate ins to reuse tmp ref */
      *ins = mkinstr(Ocopy, ins->cls, mkref(RREG, op < Orem ? RAX : RDX)); /* get output */
      temp = mkinstr(Ocopy, ins->cls, mkref(RREG, op < Orem ? RDX : RAX)); /* clobber other reg*/
      insertinstr(blk, ++(*curi), temp);
      break;
   case Osub:
       if (ins->r.bits == mkref(RICON, 1).bits) {
         /* sub x,1 -> dec x */
         ins->op = op = Oxdec;
         ins->r = NOREF;
      } else if (iscon(ins->l)) {
         /* sub imm, x -> sub x, imm; neg x */
         struct instr sub = *ins;
         rswap(sub.l, sub.r);
         ins->op = Oneg;
         ins->l = insertinstr(blk, (*curi)++, sub);
         ins->r = NOREF;
      }
      goto ALU;
   case Oadd:
      if (kisint(ins->cls) && (addarg4addrp(ins->l) || addarg4addrp(ins->r))) {
         temp.op = Ocopy;
         temp.cls = ins->cls;
         temp.l = mkref(RTMP, ins - instrtab);
         if (fuseaddr(fn, &temp.l)) {
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
      /* fallthru */
   case Omul: case Oumul:
   case Oand: case Oxor: case Oior:
   case Oequ: case Oneq:
      /* commutative ops */
      if (iscon(ins->l))
         rswap(ins->l, ins->r);
   case Oneg: case Onot:
   case Oexts1: case Oextu1: case Oexts2: case Oextu2: case Oexts4: case Oextu4:
   ALU:
      if (!(op == Oadd && kisint(ins->cls))) /* 3-address add is lea */
      if (!(in_range(op, Omul, Oumul) && kisint(ins->cls) && isimm32(ins->r))) /* for (I)MUL r,r/m,imm */
         ins->inplace = 1;
      if (ins->l.t != RTMP && ins->l.t != RREG)
         ins->l = insertinstr(blk, (*curi)++, mkinstr(Ocopy, ins->cls, ins->l));
      if (ins->r.t)
   case Omove:
         fixarg(fn, &ins->r, ins, blk, curi);
      break;
   case Oloads1: case Oloadu1: case Oloads2: case Oloadu2:
   case Oloads4: case Oloadu4: case Oloadi8: case Oloadf4: case Oloadf8:
      if (ins->l.t != RTMP && ins->l.t != RREG && ins->l.t != RMORE)
         ins->l = insertinstr(blk, (*curi)++, mkinstr(Ocopy, ins->cls, ins->l));
      fuseaddr(fn, &ins->l);
      break;
   case Ostore1: case Ostore2: case Ostore4: case Ostore8:
      if (ins->l.t != RTMP && ins->l.t != RREG && ins->l.t != RMORE)
         ins->l = insertinstr(blk, (*curi)++, mkinstr(Ocopy, ins->cls, ins->l));
      fuseaddr(fn, &ins->l);
      fixarg(fn, &ins->r, ins, blk, curi);
      break;
   case Ocopy:
      fixarg(fn, &ins->l, ins, blk, curi);
      break;
   }
}

void
amd64_isel(struct function *fn)
{
   struct block *blk = fn->entry;

   fn->stksiz = 0;

   do {
      for (int i = 0; i < blk->phi.n; ++i) {
         struct phi *phi = &phitab.p[instrtab[blk->phi.p[i]].l.i];
         for (int i = 0; i < phi->n; ++i) {
            fixarg(fn, &phi->ref[i], NULL, NULL, NULL);
         }
      }
      for (int i = 0; i < blk->ins.n; ++i) {
         sel(fn, &instrtab[blk->ins.p[i]], blk, &i);
      }
   } while ((blk = blk->lnext) != fn->entry);

   if (ccopt.dbg.i) {
      efmt("<< After isel >>\n");
      irdump(fn);
   }
}

/* vim:set ts=3 sw=3 expandtab: */
