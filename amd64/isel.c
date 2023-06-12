#include "all.h"

static bool fuseaddr(struct function *fn, union ref *r);

static void
fixarg(struct function *fn, union ref *r, struct instr *ins, struct block *blk, int *curi)
{
   int sh;
   enum op op = ins->op;

   if (r->t == RXCON) {
      struct xcon *con = &conht[r->i];
      if (in_range(op, Oshl, Oslr)) {
         sh = con->cls == KI4 ? con->i4 : con->i8;
         goto ShiftImm;
      } else if (in_range(op, Oadd, Osub) && con->i8 == 2147483648) {
         /* add X, INT32MAX+1 -> sub X, INT32MIN */
         ins->op = Oadd + (op == Oadd);
         *r = mkintcon(fn, KI4, -2147483648);
      } else if (con->cls && (kisflt(con->cls) || con->cls == KI8)) {
         /* float immediates & >32b immediates are loaded from memory */
         uchar data[8];
         uint siz = cls2siz[con->cls];
         if (con->cls == KI4 || con->cls == KF4) wr32le(data, con->i4);
         else wr64le(data, con->i8);
         *r = mkdatref(fn, siz, /*align*/siz, data, siz, /*deref*/1);
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

#define iscon(r) (in_range((r).t, RICON, RXCON))
#define isimm32(r) ((r).t == RICON || ((r).t == RXCON && conht[(r).i].cls == KI4))
#define rswap(a,b) do { union ref _t = (a); (a) = (b); (b) = _t; } while (0)

static bool
acon(struct function *fn, struct addr *addr, union ref r)
{
   vlong a = addr->disp;
   if (r.t == RICON) {
      a += r.i;
   } else {
      assert(r.t == RXCON && kisint(conht[r.i].cls));
      a += conht[r.i].cls == KI4 ? conht[r.i].i4 : conht[r.i].i8;
   }
   if ((int)a == a) {
      addr->disp = a;
      return 1;
   }
   return 0;
}

static bool
ascale(struct function *fn, struct addr *addr, union ref a, union ref b)
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
aadd(struct function *fn, struct addr *addr, union ref r, bool rec)
{
   struct instr *ins = &instrtab[r.i];
   if (r.t == RTMP && ins->op == Oadd) {
      if (!aadd(fn, addr, ins->l, rec)) return 0;
      if (!aadd(fn, addr, ins->r, rec)) return 0;
      ins->skip = 1;
   } else if (r.t == RTMP && ins->op == Oshl) {
      if (!ascale(fn, addr, ins->l, ins->r)) return 0;
      ins->skip = 1;
   } else if (!rec && r.t == RTMP && ins->op == Ocopy && ins->l.t == RMORE) {
      struct addr save = *addr, *addr2 = &addrtab.p[ins->l.i];
      if ((!addr2->base.t || aadd(fn, addr, addr2->base, 1))
       && aadd(fn, addr, mkintcon(fn, KI4, addr2->disp), 1)
       && (!addr2->index.t || ascale(fn, addr, addr2->index, mkref(RICON, addr2->shift))))
      {
         ins->skip = 1;
      } else {
         *addr = save;
         goto Ref;
      }
   } else if (iscon(r)) return acon(fn, addr, r);
   else Ref: {
      if (!addr->base.t) addr->base = r;
      else if (!addr->index.t) addr->index = r;
      else return 0;
   }
   return 1;
}

static bool
fuseaddr(struct function *fn, union ref *r)
{
   struct addr addr = { 0 };
   struct instr *ins = &instrtab[r->i];
   if (r->t == RMORE) return 1;
   else if (r->t != RTMP) return 0;
   else if (ins->op == Oadd) {
      if (ins->l.t == RTMP && instrtab[ins->l.i].op == Ocopy && instrtab[ins->l.i].l.t == RMORE)
         /* put ADDR in rhs because this code is dumb and it might be better */
         rswap(ins->l, ins->r);
      if (!aadd(fn, &addr, ins->l, 0)) return 0;
      if (!aadd(fn, &addr, ins->r, 0)) return 0;
      ins->skip = 1;
   } else if (ins->op == Oshl) {
      if (!ascale(fn, &addr, ins->l, ins->r)) return 0;
      ins->skip = 1;
   }
   else return 0;
   vpush(&addrtab, addr);
   *r = mkref(RMORE, addrtab.n-1);
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
   struct instr temp = {0};
   enum op op = ins->op;

   switch (op) {
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
      insertinstr(blk, (*curi)++, *ins); /* duplicate ins to reuse tmp ref */
      *ins = mkinstr(Ocopy, ins->cls, mkref(RREG, op < Orem ? RAX : RDX)); /* get output */
      temp = mkinstr(Ocopy, ins->cls, mkref(RREG, op < Orem ? RDX : RAX)); /* clobber other reg*/
      insertinstr(blk, ++(*curi), temp);
      break;
   case Osub:
      if (iscon(ins->l)) {
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
         fixarg(fn, &ins->r, ins, blk, curi);
      break;
   case Oloads1: case Oloadu1: case Oloads2: case Oloadu2:
   case Oloads4: case Oloadu4: case Oloadi8: case Oloadf4: case Oloadf8:
      if (ins->l.t != RTMP && ins->l.t != RREG)
         ins->l = insertinstr(blk, (*curi)++, mkinstr(Ocopy, ins->cls, ins->l));
      fuseaddr(fn, &ins->l);
      break;
   case Ostore1: case Ostore2: case Ostore4: case Ostore8:
      if (ins->l.t != RTMP && ins->l.t != RREG)
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

   do {
      struct instr *ins;
      for (int i = 0; i < blk->phi.n; ++i) {
         struct phi *phi = &phitab.p[instrtab[blk->phi.p[i]].l.i];
         for (int i = 0; i < phi->n; ++i) {
            fixarg(fn, &phi->ref[i], NULL, NULL, NULL);
         }
      }
      for (int i = 0; i < blk->ins.n; ++i) {
         ins = &instrtab[blk->ins.p[i]];
         sel(fn, ins, blk, &i);
      }
   } while ((blk = blk->lnext) != fn->entry);

   if (ccopt.dbg.i) {
      efmt("after isel:\n");
      irdump(fn, fn->name);
   }
}

/* vim:set ts=3 sw=3 expandtab: */
