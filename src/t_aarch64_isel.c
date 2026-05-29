#include "t_aarch64.h"
#include "obj.h"

#define isimm32(r) (iscon(r) && concls(r) == KI32)

static inline uint
clz(u64int x)
{
#if HAS_BUILTIN(clzll)
   return __builtin_clzll(x);
#else
   int i = 0;
   for (u64int mask = BIT(63);; ++i, mask >>= 1)
      if (x & mask)
         break;
   return i;
#endif
}

/* Encode logical immediate */
bool
aarch64_logimm(uint *enc, enum irclass k, u64int x)
{
   /* https://github.com/v8/v8/blob/927ccc6076e25a614787c7011315468e40fe39a4/src/codegen/arm64/assembler-arm64.cc#L4409 */
   if (k == KI32) x = (uint)x | x << 32;
   bool neg;
   if ((neg = x & 1)) x = ~x;
   if (x == 0) return 0;
   u64int a = x & (~x + 1),
          xa = x + a,
          b = xa & (~xa + 1),
          xa_b = xa - b,
          c = xa_b & (~xa_b + 1),
          mask;
   uint clza = clz(a),
        d, outn;
   if (c != 0) {
      d = clza - clz(c);
      mask = BIT(d) - 1;
      outn = 0;
   } else {
      assert(a != 0);
      d = 64;
      mask = ~0ull;
      outn = 1;
   }
   if (!ispo2(d)) return 0;
   if (((b - a) & ~mask) != 0) return 0;
   static const u64int M[] = {
      0x0000000000000001, 0x0000000100000001, 0x0001000100010001,
      0x0101010101010101, 0x1111111111111111, 0x5555555555555555,
   };
   int i = clz(d) - 57;
   assert((uint)i < countof(M));
   u64int m = M[i];
   u64int y = (b - a) * m;
   if (y != x) return 0;
   if (enc) {
      int clzb = b == 0 ? -1 : clz(b),
          s = clza - clzb, r;
      if (neg) {
         s = d - s;
         r = (clzb + 1) & (d - 1);
      } else {
         r = (clza + 1) & (d - 1);
      }
      *enc = outn<<12 | r<<6 | (((-d * 2) | (s - 1)) & 0x3F);
   }
   return 1;
}


static void fixarg(Ref *r, Instr *ins, Block *blk, int *curi);
static void
regarg(Ref *r, enum irclass k, Block *blk, int *curi)
{
   if (r->t != RTMP && r->t != RREG) {
      *r = insertinstr(blk, (*curi)++, mkinstr1(Ocopy, k, *r));
      if (kisflt(k) || instrtab[r->i].l.t == RSTACK) {
         int iprev = *curi-1;
         fixarg(&instrtab[r->i].l, &instrtab[r->i], blk, &iprev);
         *curi = iprev+1;
      }
   }
}

static void
fixarg(Ref *r, Instr *ins, Block *blk, int *curi)
{
   enum op op = ins ? ins->op : 0;
   if (isintcon(*r)) {
      s64int x = intconval(*r);
      switch (op) {
      case Ocopy: case Omove:
         if (kisint(ins->cls))
            return;
      case Oarg:
         if (ref2type(ins->l).isagg || !kisflt(ref2type(ins->l).cls))
            return;
      default:
         if (oiscmp(op)) {
         case Oadd: case Osub:
            /* imm12 (lsl 12) */
            if ((x &~ 0xFFF) == 0 || (x &~ 0xFFF000) == 0) return;
            break;
         case Oshl: case Osar: case Oslr:
            if ((u64int)x < (ins->cls == KI32 ? 32 : 64)) return;
            break;
         case Oand: case Oior: case Oxor:
            if (aarch64_logimm(NULL, ins->cls, x)) return;
            break;
         }
      }
      goto Reg;
   } else if (isfltcon(*r)) {
      enum irclass k = concls(*r), ki = KI32 + k-KF32;
      /* allow positive zero (copy from rzr) */
      if (contab.p[r->i].i != 0) {
         union {
            s64int i64;
            int i32;
            float f32;
            double f64;
         } pun;
         s64int i;
         if (k == KF32) {
            pun.f32 = contab.p[r->i].f;
            i = pun.i32;
         } else {
            pun.f64 = contab.p[r->i].f;
            i = pun.i64;
         }
         Ref gpr = insertinstr(blk, (*curi)++, mkinstr1(Ocopy, ki, mkintcon(ki, i)));
         *r = insertinstr(blk, (*curi)++, mkinstr1(Ocopy, k, gpr));
      } else if (oiscmp(op) || ((op == Ocopy || op == Omove) && kisflt(ins->cls))) {
         return;
      } else if (op == Oarg && !ref2type(ins->l).isagg && kisflt(ref2type(ins->l).cls)) {
         return;
      } else {
         *r = insertinstr(blk, (*curi)++, mkinstr1(Ocopy, k, *r));
      }
   } else if (r->t == RSTACK) {
      if (op == Ocopy || op == Omove || op == Ophi || op == Oarg)
         return;
      goto Reg;
   } else if (r->t != RTMP) Reg: {
      enum irclass k;
      if (r->t == RTMP) k = insrescls(instrtab[r->i]);
      else if (op == Oarg) {
         IRType ty = ref2type(ins->l);
         k = ty.isagg ? KPTR : ty.cls;
      } else {
         k = ins ? ins->cls : KI32;
      }
      regarg(r, k, blk, curi);
   }
}

static bool
arithfold(Instr *ins)
{
   if (isnumcon(ins->l) && (!ins->r.t || isnumcon(ins->r))) {
      Ref r;
      bool ok = ins->r.t ? foldbinop(&r, ins->op, ins->cls, ins->l, ins->r) : foldunop(&r, ins->op, ins->cls, ins->l);
      assert(ok && "fold?");
      *ins = mkinstr1(Ocopy, insrescls(*ins), r);
      return 1;
   }
   return 0;
}

static void
selcall(Function *fn, Instr *ins, Block *blk, int *curi)
{
   const IRCall *call = &calltab.p[ins->r.i];
   int iarg = *curi - 1;
   enum irclass cls;
   uint argstksiz = alignup(call->argstksiz, 16);

   for (int i = call->narg - 1; i >= 0; --i) {
      ABIArg abi = call->abiarg[i];
      Instr *arg;
      for (;; --iarg) {
         assert(iarg >= 0 && i >= 0 && "arg?");
         if ((arg = &instrtab[blk->ins.p[iarg]])->op == Oarg)
            break;
      }

      if (!abi.isstk) {
         assert(!abi.ty.isagg);
         *arg = mkinstr2(Omove, call->abiarg[i].ty.cls, mkref(RREG, abi.reg), arg->r);
      } else {
         Ref adr = mkaddr((IRAddr){.base = mkref(RREG, SP), .disp = abi.stk});
         int iargsave = iarg;
         if (!abi.ty.isagg) { /* scalar arg in stack */
            *arg = mkinstr2(cls2store[abi.ty.cls], 0, adr, arg->r);
            regarg(&arg->r, abi.ty.cls, blk, &iarg);
         } else { /* aggregate arg in stack, callee stack frame destination address */
            *arg = mkinstr1(Ocopy, KPTR, adr);
         }
         *curi += iarg - iargsave;
      }
   }
   if (call->argstksiz) {
      Ref disp = mkref(RICON, argstksiz);
      insertinstr(blk, iarg--, (Instr){Osub, KPTR, .keep=1, .reg = SP+1, .l=mkref(RREG,SP), .r=disp});
      ++*curi;
      insertinstr(blk, *curi+1, (Instr){Oadd, KPTR, .keep=1, .reg = SP+1, .l=mkref(RREG,SP), .r=disp});
   }
   if (isimm32(ins->l))
      ins->l = mkaddr((IRAddr){.base = ins->l});
   else if (isintcon(ins->l))
      ins->l = insertinstr(blk, (*curi)++, mkinstr1(Ocopy, KPTR, ins->l));

   cls = ins->cls;
   ins->cls = 0;
   if (cls) {
      /* duplicate to reuse same TMP ref */
      insertinstr(blk, (*curi)++, *ins);
      *ins = mkinstr1(Ocopy, cls, mkref(RREG, call->abiret[0].reg));
      for (int i = 1; i <= 2; ++i) {
         if (*curi + i >= blk->ins.n) break;
         if (instrtab[blk->ins.p[*curi + i]].op == Ocall2r) {
            ins = &instrtab[blk->ins.p[*curi += i]];
            *ins = mkinstr1(Ocopy, ins->cls, mkref(RREG, call->abiret[1].reg));
            break;
         }
      }
   }
}

static bool
aimm(IRAddr *addr, s64int disp)
{
   if (addr->index.bits) return 0;
   s64int a = addr->disp;
   a += disp;
   if ((int)a == a) {
      addr->disp = a;
      return 1;
   }
   return 0;
}

static bool
ascale(IRAddr *addr, Ref a, Ref b, uint siz/*1,2,4,8*/)
{
   if (b.t != RICON) return 0;
   if (addr->index.bits || addr->base.t == RSTACK
         || (addr->disp && !isaddrcon(addr->base,1))) return 0;
   if ((unsigned)b.i > 3 || 1<<b.i != siz) return 0;
   if (a.t == RREG || a.t == RTMP) {
      addr->index = a;
      addr->shift = b.i;
      return 1;
   }
   return 0;
}

static bool
aadd(IRAddr *addr, Block *blk, int *curi, Ref r, uint siz/*1,2,4,8*/)
{
   if (r.t == RSTACK) {
      if (addr->base.bits) goto Ref;
      addr->base = r;
   } else if (r.t == RADDR) {
      if (!addr->base.bits && !addr->index.bits && !addr->disp) {
         *addr = addrtab.p[r.i];
      } else goto Ref;
   } else if (r.t == RTMP) {
      Instr *ins = &instrtab[r.i];
      if (ins->op == Oadd) {
         if (!aadd(addr, blk, curi, ins->l, siz)) goto Ref;
         if (!aadd(addr, blk, curi, ins->r, siz)) goto Ref;
         ins->skip = 1;
      } else if (ins->op == Osub) {
         if (!aadd(addr, blk, curi, ins->l, siz)) goto Ref;
         if (!isintcon(ins->r)) goto Ref;
         if (!aimm(addr, -intconval(ins->r))) goto Ref;
         ins->skip = 1;
      } else if (ins->op == Oshl) {
         if (!ascale(addr, ins->l, ins->r, siz)) goto Ref;
         ins->skip = 1;
      } else if (ins->op == Ocopy) {
         if (!aadd(addr, blk, curi, ins->l, siz)) goto Ref;
         ins->skip = 1;
      } else goto Ref;
   } else if (isnumcon(r)) {
      assert(isintcon(r));
      return aimm(addr, intconval(r));
   } else if (isaddrcon(r,1)) {
      if (!addr->base.bits && !isaddrcon(addr->index,1)) addr->base = r;
      else return 0;
   } else if (r.t == RREG) {
      /* temporaries are single assignment, but register aren't, so they can't be *
       * safely hoisted into an address value, unless they have global lifetime */
      if (!rstest(mctarg->rglob, r.i)) return 0;
   Ref:
      if (r.t == RSTACK && (addr->base.bits || addr->index.bits)) {
         r = insertinstr(blk, (*curi)++, mkinstr1(Ocopy, KPTR, r));
      }
      if (!addr->base.bits) addr->base = r;
      else if (!addr->index.bits && addr->base.t != RSTACK && !addr->disp) addr->index = r;
      else return 0;
   } else return 0;
   return 1;
}

static bool
fuseaddr(Ref *r, Block *blk, int *curi, uint siz/*1,2,4,8*/)
{
   IRAddr addr = {0};

   if (isaddrcon(*r,1)) return 1;

   if (r->t == RSTACK) return 1;
   if (r->t != RTMP) return 0;
   if (!aadd(&addr, blk, curi, *r, siz)) return 0;
   if (!(addr.disp >= -256 && addr.disp < 256) /* for 9-bit signed unscaled offset */
    && !(!(addr.disp & (siz-1)) && (u64int)addr.disp < (1<<12)*siz)) /* 12-bit unsigned scaled offset */
      return 0;
   if (isaddrcon(addr.base,0) && (!(contab.p[addr.base.i].flag & SLOCAL) || addr.index.bits)) {
      /* first load symbol address into a temp register */
      if (addr.disp && (ccopt.pic || ccopt.pie || (contab.p[addr.base.i].flag & SFUNC)) && !addr.index.bits) {
         addr.base = insertinstr(blk, (*curi)++, mkinstr1(Ocopy, KPTR, addr.base));
      } else {
         addr.base = insertinstr(blk, (*curi)++, mkinstr1(Ocopy, KPTR,
                  mkaddr((IRAddr){addr.base, .disp = addr.disp})));
         addr.disp = 0;
      }
   }
   assert(!addr.index.bits || !addr.disp);
   *r = mkaddr(addr);
   return 1;
}

static const uchar loadsz[] = {
   [Oloads8  - Oloads8] = 1, [Oloadu8  - Oloads8] = 1,
   [Oloads16 - Oloads8] = 2, [Oloadu16 - Oloads8] = 2,
   [Oloads32 - Oloads8] = 4, [Oloadu32 - Oloads8] = 4,
   [Oloadi64 - Oloads8] = 8,
   [Oloadf32 - Oloads8] = 4,
   [Oloadf64 - Oloads8] = 8,
};
static const uchar storesz[] = {
   [Ostorei8  - Ostorei8] = 1,
   [Ostorei16 - Ostorei8] = 2,
   [Ostorei32 - Ostorei8] = 4,
   [Ostorei64 - Ostorei8] = 8,
   [Ostoref32 - Ostorei8] = 4,
   [Ostoref64 - Ostorei8] = 8,
};

/* can use LDR* PC-relative literal form? only for local symbols in the .text section */
static bool
ldrlitok(enum op op, const IRCon *con)
{
   return in_range(op, Oloads32, Oloadf64)
      && (con->flag & SLOCAL)
      && objhassym(con->sym, NULL) == Stext;
}

static void
loadstoreaddr(Block *blk, Ref *r, int *curi, enum op op)
{
   uint siz = oisload(op) ? loadsz[op-Oloads8] : storesz[op-Ostorei8];
   if (isimm32(*r)) {
      regarg(r, KPTR, blk, curi);
   } else if (isaddrcon(*r, 0)) {
      if (!ldrlitok(op, &contab.p[r->i]))
         regarg(r, KPTR, blk, curi);
   } else if (r->t == RSTACK) {
      /*pass*/
   } else if (r->t == RTMP) {
      Ref b;
      if (fuseaddr(r, blk, curi, siz)
       && isaddrcon(b = addrtab.p[r->i].base,0)
       && !ldrlitok(op, &contab.p[b.i]))
         regarg(r, KPTR, blk, curi);
   } else if (r->t != RREG) {
      *r = insertinstr(blk, (*curi)++, mkinstr1(Ocopy, KPTR, *r));
   }
}

static void
sel(Function *fn, Instr *ins, Block *blk, int *curi)
{
   Ref tmp;
   enum irclass cls;
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
   case Ocopy:
      fixarg(&ins->l, ins, blk, curi);
      break;
   case Oparam:
      assert(ins->l.t == RICON && ins->l.i < fn->nabiarg);
      if (!fn->abiarg[ins->l.i].isstk)
         *ins = mkinstr1(Ocopy, ins->cls, mkref(RREG, fn->abiarg[ins->l.i].reg));
      else /* stack */
         *ins = mkinstr1(Ocopy, KPTR, mkref(RSTACK, -fn->abiarg[ins->l.i].stk-8));
      break;
   case Oneg: case Onot:
   case Ocvtf32s: case Ocvtf32u:
   case Ocvtf32f64: case Ocvtf64s:
   case Ocvtf64u: case Ocvtf64f32:
   case Ocvts32f: case Ocvtu32f:
   case Ocvts64f: case Ocvtu64f:
   case Oexts8: case Oextu8:
   case Oexts16: case Oextu16:
   case Oexts32:
      regarg(&ins->l, ins->cls, blk, curi);
      break;
   case Oextu32:
      regarg(&ins->l, ins->cls, blk, curi);
      ins->op = Ocopy;
      break;
   case Obswap32: case Obswap64:
      regarg(&ins->l, ins->cls, blk, curi);
      break;
   case Obswap16:
      /* %tmp = rev %x
       * %res = lsr %tmp, 16 */
      regarg(&ins->l, ins->cls, blk, curi);
      tmp = insertinstr(blk, (*curi)++, mkinstr1(Obswap32, KI32, ins->l));
      ins->op = Oslr;
      ins->l = tmp;
      ins->r = mkref(RICON, 16);
      break;
   case Oadd:
      if (isnumcon(ins->l)) {
         /* swap to have const in rhs */
         Ref tmp = ins->l;
         ins->l = ins->r;
         ins->r = tmp;
      }
   case Osub:
      if (ins->r.t == RICON && ins->r.i < 0) {
         op = ins->op ^= 1;
         ins->r.i = -ins->r.i;
      }
      if (isaddrcon(ins->l,0)) {
         if (!(contab.p[ins->l.i].flag & SLOCAL) || !isintcon(ins->r) || ins->cls != KPTR) CopyFirst: {
            regarg(&ins->l, ins->cls, blk, curi);
         } else TryAdr: {
            IRAddr adr = {.base = ins->l};
            s64int disp = intconval(ins->r);
            if (!aimm(&adr, ins->op == Osub ? -(u64int)disp : disp)) goto CopyFirst;
            ins->op = Ocopy;
            ins->l = mkaddr(adr);
            ins->r = NOREF;
            break;
         }
      } else if (ins->l.t == RSTACK) {
         if (isintcon(ins->r)) goto TryAdr;
      }
      regarg(&ins->l, ins->cls, blk, curi);
      fixarg(&ins->r, ins, blk, curi);
      break;
   case Oand: case Oior: case Oxor:
      if (isnumcon(ins->l)) {
         /* swap to have const in rhs */
         tmp = ins->l;
         ins->l = ins->r;
         ins->r = tmp;
      }
   case Oshl: case Osar: case Oslr:
   case Oequ: case Oneq:
   case Olth: case Ogth: case Olte: case Ogte:
   case Oulth: case Ougth: case Oulte: case Ougte:
      regarg(&ins->l, ins->cls, blk, curi);
   case Omove:
      fixarg(&ins->r, ins, blk, curi);
      break;
   case Omul: case Odiv: case Oudiv:
      regarg(&ins->l, ins->cls, blk, curi);
      regarg(&ins->r, ins->cls, blk, curi);
      break;
   case Orem: case Ourem:
      regarg(&ins->l, ins->cls, blk, curi);
      regarg(&ins->r, ins->cls, blk, curi);
      /* %tmp = div %l, %r
       * %res = msub %tmp, %r, %l */
      tmp = insertinstr(blk, (*curi)++,
            mkinstr2(op == Orem ? Odiv : Oudiv, ins->cls, ins->l, ins->r));
      ins->op = Omsub;
      ins->oper[2] = ins->l;
      ins->oper[0] = tmp;
      break;
   case Oarg:
      fixarg(&ins->r, ins, blk, curi);
      break;
   case Ocall:
      selcall(fn, ins, blk, curi);
      break;
   case Oloads8: case Oloadu8: case Oloads16: case Oloadu16:
   case Oloads32: case Oloadu32: case Oloadi64: case Oloadf32: case Oloadf64:
      loadstoreaddr(blk, &ins->l, curi, op);
      break;
   case Ostorei8: case Ostorei16: case Ostorei32: cls = KI32; goto Store;
   case Ostorei64: cls = KI64; goto Store;
   case Ostoref32: cls = KF32; goto Store;
   case Ostoref64: cls = KF64; Store:
      loadstoreaddr(blk, &ins->l, curi, op);
      regarg(&ins->r, cls, blk, curi);
      break;
   case Oxvaprologue:
      fuseaddr(&ins->l, blk, curi, 8);
      assert(ins->l.t == RSTACK);
      /* !this must be the first instruction */
      assert(*curi == 1);
      assert(blk == fn->entry);
      int t = blk->ins.p[0];
      blk->ins.p[0] = blk->ins.p[1];
      blk->ins.p[1] = t;
      break;
   }
}

static void
seljmp(Function *fn, Block *blk)
{
   if (blk->jmp.t == Jb && blk->jmp.arg[0].bits) {
      int curi = blk->ins.n;
      fixarg(&blk->jmp.arg[0], NULL, blk, &curi);
      Ref c = blk->jmp.arg[0];
      if (c.t != RTMP) {
         enum irclass cls = c.t == RICON ? KI32 : c.t == RXCON && contab.p[c.i].cls ? contab.p[c.i].cls : KPTR;
         int curi = blk->ins.n;

         c = insertinstr(blk, blk->ins.n, mkinstr1(Ocopy, cls, c));
         sel(fn, &instrtab[c.i], blk, &curi);
      }
      if (!oiscmp(instrtab[c.i].op)) {
         enum irclass k = insrescls(instrtab[c.i]);
         blk->jmp.arg[0] = insertinstr(blk, blk->ins.n, mkinstr2(Oneq, k, c, kisint(k) ? ZEROREF : mkfltcon(k, 0)));
         Instr *ins = &instrtab[blk->jmp.arg[0].i];
         ins->keep = 1;
      } else {
         instrtab[c.i].keep = 1;
      }
   } else if (blk->jmp.t == Jret) {
      if (blk->jmp.arg[0].bits) {
         Ref r = mkref(RREG, fn->abiret[0].reg);
         Instr *ins = &instrtab[insertinstr(blk, blk->ins.n, mkinstr2(Omove, fn->abiret[0].ty.cls, r, blk->jmp.arg[0])).i];
         int curi = blk->ins.n-1;
         fixarg(&ins->r, ins, blk, &curi);
         blk->jmp.arg[0] = r;
         if (blk->jmp.arg[1].bits) {
            r = mkref(RREG, fn->abiret[1].reg);
            ins = &instrtab[insertinstr(blk, blk->ins.n, mkinstr2(Omove, fn->abiret[1].ty.cls, r, blk->jmp.arg[1])).i];
            blk->jmp.arg[1] = r;
         }
      }
   }
}

void
aarch64_isel(Function *fn)
{
   Block *blk = fn->entry;

   do {
      int i;
      for (i = 0; i < blk->phi.n; ++i) {
         Instr *ins = &instrtab[blk->phi.p[i]];
         Ref *phi = phitab.p[ins->l.i];
         for (int i = 0; i < blk->npred; ++i) {
            int curi = blkpred(blk, i)->ins.n;
            fixarg(&phi[i], ins, blkpred(blk, i), &curi);
         }
      }
      for (i = 0; i < blk->ins.n; ++i) {
         Instr *ins = &instrtab[blk->ins.p[i]];
         sel(fn, ins, blk, &i);
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
