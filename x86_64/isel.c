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
static const uchar opflags[NOPER] = {
   [Oneg]  = ZF|CLOBF,
   [Oadd]  = ZF|CLOBF,
   [Osub]  = ZF|CLOBF,
   [Omul]  = CLOBF,
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
};

static int iflagsrc = -1;

#define inscopy(blk, pcuri, k, r) insertinstr((blk), (*(pcuri))++, mkinstr(Ocopy, k, .l = (r)))
static void
picfixsym(union ref *r, struct block *blk, int *curi)
{
   if (!ccopt.pic || !isaddrcon(*r,0)) return;
   *r = inscopy(blk, curi, KPTR, *r);
}

static void
fixarg(union ref *r, struct instr *ins, struct block *blk, int *curi)
{
   int sh;
   enum op op = ins ? ins->op : 0;
   enum irclass cls = ins ? ins->cls : 0;

Begin:
   if (r->t == RXCON) {
      struct xcon *con = &contab.p[r->i];
      if (in_range(op, Oshl, Oslr) && r == &ins->r) {
         sh = con->i;
         goto ShiftImm;
      } else if (cls == KI32 && in_range(con->cls, KI64, KPTR)) {
         *r = mkintcon(KI32, (int)con->i);
         goto Begin;
      }
      if (in_range(op, Oadd, Osub) && con->i == 2147483648 && r == &ins->r) {
         /* add X, INT32MAX+1 -> sub X, INT32MIN */
         ins->op = Oadd + (op == Oadd);
         *r = mkintcon(KI32, -2147483648);
      } else if (kisflt(con->cls) && con->i == 0) {
         /* copy of positive float zero -> regular zero, that emit() will turn into xor x,x */
         if (in_range(op, Ocopy, Omove) || op == Ophi)
            *r = ZEROREF;
         else
            *r = inscopy(blk, curi, con->cls, ZEROREF);
      } else if (con->cls >= KI64) {
         /* float immediates & 64bit immediates are loaded from memory */
         uchar data[8];
         uint ksiz = cls2siz[con->cls];
         union type ctype;
         /* can't use memory arg in rhs if lhs is memory */
         bool docopy = ins && &ins->l != r && (oisstore(ins->op) || ins->l.t == RADDR);
         if (con->cls <= KPTR && (in_range(op, Ocopy, Omove) || op == Ophi))
            /* in this case we can use movabs */
            return;
         else if (!docopy || con->cls >= KF32) {
            if (con->cls != KF32) {
               wr64le(data, con->i);
               ctype = mktype(con->cls == KF64 ? TYDOUBLE : TYVLONG);
            } else {
               union { float f; int i; } pun = { con->f };
               wr32le(data, pun.i);
               ctype = mktype(TYFLOAT);
            }
            *r = mkdatref(NULL, ctype, ksiz, /*align*/ksiz, data, ksiz, /*deref*/1);
         }
         if (docopy)
            *r = inscopy(blk, curi, con->cls, *r);
      } else if (op != Omove && ins && isaddrcon(*r,0) && r == &ins->r) {
         *r = inscopy(blk, curi, KPTR, *r);
      } else if (in_range(op, Odiv, Ourem) && kisint(ins->cls))
         goto DivImm;
   } else if (r->t == RICON && in_range(op, Odiv, Ourem) && kisint(ins->cls) && r == &ins->r) {
   DivImm: /* there is no division by immediate, must be copied to a register */
      *r = inscopy(blk, curi, ins->cls, *r);
   } else if (r->t == RICON && in_range(op, Oshl, Oslr) && r == &ins->r) {
      sh = r->i;
   ShiftImm: /* shift immediate is always 8bit */
      *r = mkref(RICON, sh & 255);
   } else if (r->t == RSTACK) {
      struct instr adr = mkinstr(Oadd, KPTR, mkref(RREG, RBP), mkintcon(KI32, -r->i));
      if (op == Ocopy)
         *ins = adr;
      else
         *r = insertinstr(blk, (*curi)++, adr);
   } else if (r->bits == UNDREF.bits && ins && !in_range(op, Ocopy, Omove) && op != Ophi) {
      *r = inscopy(blk, curi, ins->cls, *r);
   }
   picfixsym(r, blk, curi);
}

#define isimm32(r) (iscon(r) && concls(r) == KI32)

static void
selcall(struct function *fn, struct instr *ins, struct block *blk, int *curi)
{
   const struct call *call = &calltab.p[ins->r.i];
   int iarg = *curi - 1;
   enum irclass cls;
   uint argstksiz = alignup(call->argstksiz, 16);
   int nsse = 0;

   for (int i = call->narg - 1; i >= 0; --i) {
      struct abiarg abi = call->abiarg[i];
      struct instr *arg;
      for (;; --iarg) {
         assert(iarg >= 0 && i >= 0 && "arg?");
         if ((arg = &instrtab[blk->ins.p[iarg]])->op == Oarg)
            break;
      }

      if (!abi.isstk) {
         assert(!abi.ty.isagg);
         *arg = mkinstr(Omove, call->abiarg[i].ty.cls, mkref(RREG, abi.reg), arg->r);
         if (abi.reg >= XMM0) ++nsse;
      } else {
         union ref adr = mkaddr((struct addr){mkref(RREG, RSP), .disp = abi.stk});
         int iargsave = iarg;
         if (!abi.ty.isagg) { /* scalar arg in stack */
            *arg = mkinstr(cls2store[abi.ty.cls], 0, adr, arg->r);
            if (isaddrcon(arg->r,1) || arg->r.t == RADDR)
               arg->r = insertinstr(blk, iarg++, mkinstr(Ocopy, abi.ty.cls, arg->r));
            else
               fixarg(&ins->r, ins, blk, &iarg);
         } else { /* aggregate arg in stack, callee stack frame destination address */
            *arg = mkinstr(Ocopy, KPTR, adr);
         }
         *curi += iarg - iargsave;
      }
   }
   if (call->argstksiz) {
      union ref disp = mkref(RICON, argstksiz);
      insertinstr(blk, iarg--, (struct instr){Osub, KPTR, .keep=1, .reg = RSP+1, .l=mkref(RREG,RSP), disp});
      ++*curi;
      insertinstr(blk, *curi+1, (struct instr){Oadd, KPTR, .keep=1, .reg = RSP+1, .l=mkref(RREG,RSP), disp});
   }
   if (isimm32(ins->l))
      ins->l = mkaddr((struct addr){.base = ins->l});
   else if (isintcon(ins->l))
      ins->l = inscopy(blk, curi, KPTR, ins->l);

   if (call->vararg >= 0) {
      /* variadic calls write number of sse regs used to AL */
      insertinstr(blk, (*curi)++, mkinstr(Omove, KI32, mkref(RREG, RAX), mkref(RICON, nsse), .keep=1));
   }
   cls = ins->cls;
   ins->cls = 0;
   if (cls) {
      /* duplicate to reuse same TMP ref */
      insertinstr(blk, (*curi)++, *ins);
      *ins = mkinstr(Ocopy, cls, mkref(RREG, call->abiret[0].reg));
      for (int i = 1; i <= 2; ++i) {
         if (*curi + i >= blk->ins.n) break;
         if (instrtab[blk->ins.p[*curi + i]].op == Ocall2r) {
            ins = &instrtab[blk->ins.p[*curi += i]];
            *ins = mkinstr(Ocopy, ins->cls, mkref(RREG, call->abiret[1].reg));
            break;
         }
      }
   }
}

static bool
aimm(struct addr *addr, vlong disp)
{
   vlong a = addr->disp;
   a += disp;
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
   if ((unsigned)b.i > 3) return 0;
   if (a.t == RREG) {
   Scaled:
      addr->index = a;
      addr->shift = b.i;
      return 1;
   } else if (a.t == RTMP) {
      struct instr *ins = &instrtab[a.i];
      /* factor out shifted immediate from 'shl {add %x, imm}, s' */
      /* XXX maybe we shouldn't do this here because it should be done by a generic
       * arithemetic optimization pass ? */
      if (ins->op == Oadd && (ins->l.t == RREG || ins->l.t == RTMP) && isintcon(ins->r)) {
         vlong a = ((vlong) addr->disp + intconval(ins->r)) * (1 << b.i);
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
aadd(struct addr *out, struct block *blk, int *curi, union ref r, bool recurring)
{
   if (r.t == RSTACK) {
      if (out->base.bits || !aimm(out, -r.i)) {
         r = insertinstr(blk, (*curi)++, mkinstr(Oadd, KPTR, mkref(RREG, RBP), mkref(RICON, -r.i)));
         goto Ref;
      }
      out->base = mkref(RREG, RBP);
   } else if (r.t == RTMP) {
      struct instr *ins = &instrtab[r.i];
      struct addr adr = {0};
      if (ins->op == Oadd) {
         if (recurring) goto Ref;
         if (aadd(&adr, blk, curi, ins->l, 1) && aadd(&adr, blk, curi, ins->r, 1)) {
         Add2:;
            int n1 = !!out->base.bits + !!out->index.bits;
            int n2 = !!adr.base.bits + !!adr.index.bits;
            vlong off = (vlong) out->disp + adr.disp;
            if (n1+n2 > 2 || (int)off != off) goto Ref;
            if (n1 == 0) {
               *out = adr;
            } else if (n1 == 1 && n2 == 1) {
               assert(!out->index.bits);
               out->index = adr.index.bits ? adr.index : adr.base;
               out->shift = adr.shift;
            } else assert(n1 <= 2 && n2 == 0);
            out->disp = off;
            ins->skip = 1;
         } else return 0;
      } else if (ins->op == Ocopy && ins->l.t == RADDR) {
         const struct addr *adr2 = &addrtab.p[ins->l.i];
         adr = *adr2;
         goto Add2;
      } else if (ins->op == Oshl) {
         if (!ascale(out, ins->l, ins->r)) goto Ref;
         ins->skip = 1;
      } else goto Ref;
   } else if (isnumcon(r)) {
      assert(isintcon(r));
      return aimm(out, intconval(r));
   } else if (isaddrcon(r,1)) {
      if (!out->base.bits && !isaddrcon(out->index,1)) out->base = r;
      else return 0;
   } else if (r.t == RREG) {
      /* temporaries are single assignment, but register aren't, so they can't be *
       * safely hoisted into an address value, unless they have global lifetime */
      if (!rstest(mctarg->rglob, r.i)) return 0;
   Ref:
      if (!out->base.bits) out->base = r;
      else if (!out->index.bits) out->index = r;
      else return 0;
   } else return 0;
   return 1;
}

static bool
fuseaddr(union ref *r, struct block *blk, int *curi)
{
   struct addr addr = { 0 };

   if (isaddrcon(*r,1)) return 1;
   if (!aadd(&addr, blk, curi, *r, 0)) return 0;

   if (isaddrcon(addr.base,0) && (ccopt.pic || (ccopt.pie && addr.index.bits) || (contab.p[addr.base.i].flag & SFUNC))) {
      /* pic needs to load from GOT */
      /* pie cannot encode RIP-relative address with index register */
      /*     first load symbol address into a temp register */
      union ref temp = mkaddr((struct addr){.base = addr.base, .disp = ccopt.pic ? 0 : addr.disp});
      addr.base = inscopy(blk, curi, KPTR, temp);
      if (!ccopt.pic) addr.disp = 0;
   }

   if (!addr.base.bits) {
      /* absolute int address in disp */
      if (addr.index.bits) return 0;
      addr.base = mkintcon(KPTR, addr.disp);
      addr.disp = 0;
   }

   *r = mkaddr(addr);
   return 1;
}

/* is add instruction with this arg a candidate to transform into efective addr? */
static bool
addarg4addrp(union ref r)
{
   struct instr *ins;
   if (isaddrcon(r, 0)) return 1;
   if (r.t == RSTACK) return 1;
   if (r.t != RTMP) return 0;
   ins = &instrtab[r.i];
   return (ins->op == Ocopy && ins->l.t == RADDR) || ins->op == Oadd || ins->op == Oshl;
}

static void
loadstoreaddr(struct block *blk, union ref *r, int *curi)
{
   if (isimm32(*r)) {
      *r = mkaddr((struct addr){.base = *r});
   } else if (isaddrcon(*r, 0)) {
      picfixsym(r, blk, curi);
   } else if (r->t == RSTACK || (r->t == RTMP && addarg4addrp(*r))) {
      fuseaddr(r, blk, curi);
   } else if (r->t != RREG) {
      *r = insertinstr(blk, (*curi)++, mkinstr(Ocopy, KPTR, *r));
   }
}

static bool
arithfold(struct instr *ins)
{
   if (isnumcon(ins->l) && (!ins->r.t || isnumcon(ins->r))) {
      union ref r;
      bool ok = ins->r.t ? foldbinop(&r, ins->op, ins->cls, ins->l, ins->r) : foldunop(&r, ins->op, ins->cls, ins->l);
      assert(ok && "fold?");
      *ins = mkinstr(Ocopy, insrescls(*ins), r);
      return 1;
   }
   return 0;
}

static void
sel(struct function *fn, struct instr *ins, struct block *blk, int *curi)
{
   int t = ins - instrtab;
   struct instr temp = {0};
   enum op op = ins->op;

   if (oisarith(ins->op) && arithfold(ins)) {
      fixarg(&ins->l, ins, blk, curi);
      return;
   }

   switch (op) {
   default: assert(0);
   case Onop: break;
   case Oalloca1: case Oalloca2: case Oalloca4: case Oalloca8: case Oalloca16:
      assert(!"unlowered alloca");
      break;
   case Oparam:
      assert(ins->l.t == RICON && ins->l.i < fn->nabiarg);
      if (!fn->abiarg[ins->l.i].isstk)
         *ins = mkinstr(Ocopy, ins->cls, mkref(RREG, fn->abiarg[ins->l.i].reg));
      else /* stack */
         *ins = mkinstr(Oadd, KPTR, mkref(RREG, RBP), mkref(RICON, 16+fn->abiarg[ins->l.i].stk));
      break;
   case Oarg:
      fixarg(&ins->r, ins, blk, curi);
      break;
   case Ocall:
      selcall(fn, ins, blk, curi);
      break;
   case Ointrin:
      break;
   case Oshl: case Osar: case Oslr:
      if (!isintcon(ins->r)) {
         /* shift amount register is always CL */
         insertinstr(blk, (*curi)++, mkinstr(Omove, KI32, mkref(RREG, RCX), ins->r));
         ins->r = mkref(RREG, RCX);
      }
      goto ALU;
   case Oequ: case Oneq:
   case Olth: case Ogth: case Olte: case Ogte:
   case Oulth: case Ougth: case Oulte: case Ougte:
      if (iscon(ins->l) && !iscon(ins->r)) {
         /* lth imm, x -> gth x, imm */
         if (!in_range(ins->op, Oequ, Oneq))
            ins->op = ((op - Olth) ^ 1) + Olth;
         rswap(ins->l, ins->r);
      }
      if (ins->l.t != RTMP && ins->l.t != RREG && ins->l.t != RSTACK)
         ins->l = inscopy(blk, curi, ins->cls, ins->l);
      else
         fixarg(&ins->l, ins, blk, curi);
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
       if (isintcon(ins->l)) {
         /* sub imm, x -> sub x, imm; neg x */
         fixarg(&ins->l, ins, blk, curi);
         ins->inplace = 1;
         struct instr sub = *ins;
         rswap(sub.l, sub.r);
         ins->op = op = Oneg;
         ins->l = insertinstr(blk, (*curi)++, sub);
         ins->r = NOREF;
         goto ALU;
      } else if (kisint(ins->cls) && isintcon(ins->r)) {
         ins->op = op = Oadd;
         ins->r = mkintcon(concls(ins->r), -intconval(ins->r));
      } else {
         goto ALU;
      }
      /* fallthru */
   case Oadd:
      if (kisint(ins->cls) && (addarg4addrp(ins->l) || addarg4addrp(ins->r))) {
         union ref it = mkref(RTMP, ins - instrtab);
         if (fuseaddr(&it, blk, curi)) {
            *ins = mkinstr(Ocopy, ins->cls, it);
            break;
         }
      }
      /* fallthru */
   case Omul:
   case Oand: case Oxor: case Oior:
      /* commutative ops */
      if (iscon(ins->l))
         rswap(ins->l, ins->r);
      goto ALU;
   case Oneg:
      if (kisflt(ins->cls)) {
         /* flip sign bit with XORPS/D */
         static const uvlong sd[2] = {0x8000000000000000,0x8000000000000000};
         static const uint sf[4] = {0x80000000,80000000,0x80000000,80000000};
         ins->op = Oxor;
         ins->r = mkdatref(NULL, mktype(ins->cls == KF32 ? TYFLOAT : TYDOUBLE), /*siz*/16,
               /*align*/16, ins->cls == KF32 ? (void *)sf : sd, /*siz*/16, /*deref*/1);
      }
      /* fallthru */
   case Onot:
   ALU:
      if (!(op == Oadd && kisint(ins->cls))) /* 3-address add is lea */
      if (!(op == Omul && kisint(ins->cls) && isimm32(ins->r))) /* for (I)MUL r,r/m,imm */
      if (!(op == Oshl && ins->r.t == RICON && ins->r.i <= 3)) /* can be lea */
         ins->inplace = 1;
      if (iscon(ins->l) || ins->l.t == RSTACK) {
         fixarg(&ins->l, ins, blk, curi);
         ins->l = insertinstr(blk, (*curi)++, mkinstr(Ocopy, ins->cls, ins->l));
      }
      if (ins->r.bits)
   case Omove:
         fixarg(&ins->r, ins, blk, curi);
      if (op == Oadd && isaddrcon(ins->r,1)) /* no 3-address add if rhs is mem */
         ins->inplace = 1;
      break;
   case Oloads8: case Oloadu8: case Oloads16: case Oloadu16:
   case Oloads32: case Oloadu32: case Oloadi64: case Oloadf32: case Oloadf64:
      loadstoreaddr(blk, &ins->l, curi);
      break;
   case Ostorei8: case Ostorei16: case Ostorei32: case Ostorei64: case Ostoref32: case Ostoref64:
      loadstoreaddr(blk, &ins->l, curi);
      if (isaddrcon(ins->r,1) || ins->r.t == RADDR)
         ins->r = insertinstr(blk, (*curi)++, mkinstr(Ocopy, KPTR, ins->r));
      else
         fixarg(&ins->r, ins, blk, curi);
      break;
   case Ocvtu32f:
      fixarg(&ins->l, ins, blk, curi);
      ins->l = insertinstr(blk, (*curi)++, mkinstr(Oextu32, KI64, ins->l));
      ins->op = Ocvts64f;
      break;
   case Ocvtf32u: case Ocvtf64u:
      fixarg(&ins->l, ins, blk, curi);
      if (ins->cls == KI32) {
         ins->l = insertinstr(blk, (*curi)++, mkinstr(ins->op == Ocvtf32u ? Ocvtf32s : Ocvtf64s, KI64, ins->l));
         ins->op = Oextu32;
     } else assert(!"nyi flt -> u64");
      break;
   case Oextu32:
      if (ins->l.t == RTMP && insrescls(instrtab[ins->l.i]) == KI32 && instrtab[ins->l.i].op != Ocopy) {
         /* no need to explicitly zero extend 32 -> 64bit regs in x86-64 */
         /* this copy can be optimized away in regalloc */
         ins->op = op = Ocopy;
         ins->cls = KI32;
      }
      /* fallthru */
   case Ocvtf32f64: case Ocvtf64f32: case Ocvtf32s: case Ocvtf64s: case Ocvts32f: case Ocvts64f:
   case Ocvtu64f:
   case Oexts8: case Oextu8: case Oexts16: case Oextu16: case Oexts32:
      if (isnumcon(ins->l)) {
         union ref it;
         bool ok = foldunop(&it, ins->op, ins->cls, ins->l);
         assert(ok);
         ins->op = Ocopy;
         ins->l = it;
         break;
      }
   case Ocopy:
      fixarg(&ins->l, ins, blk, curi);
      break;
   case Obswap16: case Obswap32: case Obswap64:
      ins->inplace = 1;
      if (ins->l.t != RTMP) {
         ins->l = insertinstr(blk, *curi, mkinstr(Ocopy, ins->cls, ins->l));
         fixarg(&instrtab[ins->l.i].l, ins, blk, curi);
         ++*curi;
      }
      break;
   case Oxvaprologue:
      fuseaddr(&ins->l, blk, curi);
      assert(ins->l.t == RADDR);
      /* !this must be the first instruction */
      assert(*curi == 1);
      assert(blk == fn->entry);
      t = blk->ins.p[0];
      blk->ins.p[0] = blk->ins.p[1];
      blk->ins.p[1] = t;
      break;
   }
}

static void
seljmp(struct function *fn, struct block *blk)
{
   if (blk->jmp.t == Jb && blk->jmp.arg[0].bits) {
      int curi = blk->ins.n;
      fixarg(&blk->jmp.arg[0], NULL, blk, &curi);
      union ref c = blk->jmp.arg[0];
      if (c.t != RTMP) {
         enum irclass cls = c.t == RICON ? KI32 : c.t == RXCON && contab.p[c.i].cls ? contab.p[c.i].cls : KPTR;
         int curi = blk->ins.n;

         c = insertinstr(blk, blk->ins.n, mkinstr(Ocopy, cls, c));
         sel(fn, &instrtab[c.i], blk, &curi);
      }
      if (iflagsrc == c.i             /*           test                   cmp              */
       && (oiscmp(instrtab[c.i].op) || instrtab[c.i].op == Oand || instrtab[c.i].op == Osub)) {
         instrtab[c.i].keep = 1;
      } else {
         if (!(opflags[instrtab[c.i].op] & ZF) || blk->ins.n == 0 || c.i != blk->ins.p[blk->ins.n - 1]) {
            struct instr *ins;
            int curi = blk->ins.n;
            blk->jmp.arg[0] = insertinstr(blk, blk->ins.n, mkinstr(Oneq, insrescls(instrtab[c.i]), c, ZEROREF));
            ins = &instrtab[blk->jmp.arg[0].i];
            if (kisflt(ins->cls)) {
               ins->r = insertinstr(blk, curi, mkinstr(Ocopy, ins->cls, ZEROREF));
            }
            ins->keep = 1;
         } else if (instrtab[c.i].op == Oadd) {
            /* prevent a 3-address add whose flag results are used from becoming a LEA */
            instrtab[c.i].inplace = 1;
         }
      }
   } else if (blk->jmp.t == Jret) {
      if (blk->jmp.arg[0].bits) {
         int curi;
         union ref r = mkref(RREG, fn->abiret[0].reg);
         struct instr *ins = &instrtab[insertinstr(blk, blk->ins.n, mkinstr(Omove, fn->abiret[0].ty.cls, r, blk->jmp.arg[0])).i];
         curi = blk->ins.n-1;
         fixarg(&ins->r, ins, blk, &curi);
         blk->jmp.arg[0] = r;
         if (blk->jmp.arg[1].bits) {
            r = mkref(RREG, fn->abiret[1].reg);
            ins = &instrtab[insertinstr(blk, blk->ins.n, mkinstr(Omove, fn->abiret[1].ty.cls, r, blk->jmp.arg[1])).i];
            curi = blk->ins.n-1;
            fixarg(&ins->r, ins, blk, &curi);
            blk->jmp.arg[1] = r;
         }
      }
   }
}

void
x86_64_isel(struct function *fn)
{
   struct block *blk = fn->entry;

   do {
      for (int i = 0; i < blk->phi.n; ++i) {
         struct instr *ins = &instrtab[blk->phi.p[i]];
         union ref *phi = phitab.p[ins->l.i];
         for (int i = 0; i < blk->npred; ++i) {
            int curi = blkpred(blk, i)->ins.n;
            fixarg(&phi[i], ins, blkpred(blk, i), &curi);
         }
      }
      iflagsrc = -1;
      for (int i = 0; i < blk->ins.n; ++i) {
         struct instr *ins = &instrtab[blk->ins.p[i]];
         sel(fn, ins, blk, &i);
         if (ins->op < countof(opflags) && kisint(insrescls(*ins))) {
            if (opflags[ins->op] & ZF) iflagsrc = ins - instrtab;
            else if (opflags[ins->op] & CLOBF) iflagsrc = -1;
         }
      }
      seljmp(fn, blk);
   } while ((blk = blk->lnext) != fn->entry);

   if (ccopt.dbg.i) {
      bfmt(ccopt.dbgout, "<< After isel >>\n");
      irdump(fn);
   }

   fn->prop = 0;
}

/* vim:set ts=3 sw=3 expandtab: */
