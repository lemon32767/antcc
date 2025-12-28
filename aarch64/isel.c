#include "all.h"

/* map alloca tmp -> stack frame displacement (0 if not alloca) */
static ushort *stkslots;
static uint nstkslots;

#define isstkslot(r) ((r).t == RTMP && (r).i < nstkslots && stkslots[(r).i])
#define isimm32(r) (iscon(r) && concls(r) == KI32)

static void
picfixsym(union ref *r, struct block *blk, int *curi)
{
   if (!ccopt.pic || !isaddrcon(*r,0)) return;
   *r = insertinstr(blk, (*curi)++, mkinstr(Ocopy, KPTR, .l = *r));
}

static inline uint
clz(uvlong x)
{
#if HAS_BUILTIN(clzll)
   return __builtin_clzll(x);
#else
   int i = 0;
   for (uvlong mask = BIT(63);; ++i, mask >>= 1)
      if (x & mask)
         break;
   return i;
#endif
}

/* Encode logical immediate */
bool
aarch64_logimm(uint *enc, enum irclass k, uvlong x)
{
   /* https://github.com/v8/v8/blob/927ccc6076e25a614787c7011315468e40fe39a4/src/codegen/arm64/assembler-arm64.cc#L4409 */
   if (k == KI32) x = (uint)x | x << 32;
   bool neg;
   if ((neg = x & 1)) x = ~x;
   if (x == 0) return 0;
   uvlong a = x & (~x + 1),
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
   static const uvlong M[] = {
      0x0000000000000001, 0x0000000100000001, 0x0001000100010001,
      0x0101010101010101, 0x1111111111111111, 0x5555555555555555,
   };
   int i = clz(d) - 57;
   assert((uint)i < countof(M));
   uvlong m = M[i];
   uvlong y = (b - a) * m;
   if (y != x) return 0;
   if (enc) {
      int clzb = b == 0 ? -1 : clz(b),
          s = clza - clzb,
          r;
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

static void
fixarg(union ref *r, struct instr *ins, struct block *blk, int *curi)
{
   enum op op = ins ? ins->op : 0;
   if (isintcon(ins->r)) {
      vlong x = intconval(ins->r);
      switch (op) {
      default:
         if (oiscmp(op)) {
         case Oadd: case Osub:
            /* imm12 (lsl 12) */
            if ((x &~ 0xFFF) == 0 || (x &~ 0xFFF000) == 0) return;
            break;
         case Oshl: case Osar: case Oslr:
            if ((uvlong)x < (ins->cls == KI32 ? 32 : 64)) return;
            break;
         case Oand: case Oior: case Oxor:
            if (aarch64_logimm(NULL, ins->cls, x)) return;
            break;
         }
      }
      goto Copy;
   } else if (isstkslot(*r)) {
      struct instr adr = mkinstr(Oadd, KPTR, mkref(RREG, FP), mkintcon(KI32, -stkslots[r->i]));
      if (ins && ins->op == Ocopy)
         *ins = adr;
      else
         *r = insertinstr(blk, (*curi)++, adr);
   } else if (r->t != RTMP) Copy: {
      *r = insertinstr(blk, (*curi)++, mkinstr(Ocopy, r->t == RTMP ? instrtab[r->i].cls : ins->cls ? ins->cls : KI32, *r));
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
selcall(struct function *fn, struct instr *ins, struct block *blk, int *curi)
{
   const struct call *call = &calltab.p[ins->r.i];
   int iarg = *curi - 1;
   enum irclass cls;
   uint argstksiz = alignup(call->argstksiz, 16);

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
      } else {
         union ref adr = mkaddr((struct addr){mkref(RREG, SP), .disp = abi.stk});
         int iargsave = iarg;
         if (!abi.ty.isagg) { /* scalar arg in stack */
            *arg = mkinstr(Ostore8+ilog2(cls2siz[abi.ty.cls]), 0, adr, arg->r);
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
      insertinstr(blk, iarg--, (struct instr){Osub, KPTR, .keep=1, .reg = SP+1, .l=mkref(RREG,SP), disp});
      ++*curi;
      insertinstr(blk, *curi+1, (struct instr){Oadd, KPTR, .keep=1, .reg = SP+1, .l=mkref(RREG,SP), disp});
   }
   if (isimm32(ins->l))
      ins->l = mkaddr((struct addr){.base = ins->l});
   else if (isintcon(ins->l))
      ins->l = insertinstr(blk, (*curi)++, mkinstr(Ocopy, KPTR, ins->l));

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
aimm(struct addr *addr, int disp)
{
   if (addr->index.bits) return 0;
   vlong a = addr->disp;
   a += disp;
   if ((int)a == a) {
      addr->disp = a;
      return 1;
   }
   return 0;
}

static bool
ascale(struct addr *addr, union ref a, union ref b, uint siz/*1,2,4,8*/)
{
   if (b.t != RICON) return 0;
   if (addr->index.bits || addr->disp) return 0;
   if ((unsigned)b.i > 3 || 1<<b.i != siz) return 0;
   if (a.t == RREG || a.t == RTMP) {
      addr->index = a;
      addr->shift = b.i;
      return 1;
   }
   return 0;
}

static bool
aadd(struct addr *addr, struct block *blk, int *curi, union ref r, uint siz/*1,2,4,8*/)
{
   if (r.t == RSTACK) {
      if (addr->base.bits || addr->index.bits || !aimm(addr, -r.i)) goto Ref;
      addr->base = mkref(RREG, FP);
   } else if (r.t == RTMP) {
      struct instr *ins = &instrtab[r.i];
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
         r = insertinstr(blk, (*curi)++, mkinstr(Oadd, KPTR, mkref(RREG, FP), mkref(RICON, -r.i)));
      }
      if (!addr->base.bits) addr->base = r;
      else if (!addr->index.bits) addr->index = r;
      else return 0;
   } else return 0;
   return 1;
}

static bool
fuseaddr(union ref *r, struct block *blk, int *curi, uint siz/*1,2,4,8*/)
{
   struct addr addr = {0};

   if (isaddrcon(*r,1)) return 1;

   if (r->t != RSTACK && r->t != RTMP) return 0;
   if (!aadd(&addr, blk, curi, *r, siz)) return 0;
   if (isaddrcon(addr.base,0) && (ccopt.pic || (ccopt.pie && addr.index.bits) || (conht[addr.base.i].flag & SFUNC))) {
      /* pic needs to load from GOT */
      /* pie cannot encode RIP-relative address with index register */
      /*     first load symbol address into a temp register */
      union ref temp = mkaddr((struct addr){.base = addr.base, .disp = ccopt.pic ? 0 : addr.disp});
      addr.base = insertinstr(blk, (*curi)++, mkinstr(Ocopy, KPTR, .l = temp));
      if (!ccopt.pic) addr.disp = 0;
   }
   if (!(addr.disp >= -256 && addr.disp < 256) /* for 9-bit signed unscaled offset */
    && !(!(addr.disp & (siz-1)) && (uvlong)addr.disp < (1<<12)*siz)) /* 12-bit unsigned scaled offset */
      return 0;
   *r = mkaddr(addr);
   return 1;
}


static void
loadstoreaddr(struct block *blk, union ref *r, int *curi, uint siz)
{
   if (isimm32(*r)) {
      *r = mkaddr((struct addr){.base = *r});
   } else if (isaddrcon(*r, 0)) {
      picfixsym(r, blk, curi);
   } else if (r->t == RTMP || r->t == RSTACK) {
      fuseaddr(r, blk, curi, siz);
   } else if (r->t != RREG) {
      *r = insertinstr(blk, (*curi)++, mkinstr(Ocopy, KPTR, *r));
   }
}

static void
sel(struct function *fn, struct instr *ins, struct block *blk, int *curi)
{
   uint siz, alignlog2;
   int t = ins - instrtab;
   struct instr temp = {0};
   enum op op = ins->op;

   if (oisarith(ins->op) && arithfold(ins)) {
      fixarg(&ins->l, ins, blk, curi);
      return;
   }

   switch (op) {
   //default: assert(0);
   case Onop: break;
   case Oalloca1: case Oalloca2: case Oalloca4: case Oalloca8: case Oalloca16:
      alignlog2 = ins->op - Oalloca1;
      assert(ins->l.i > 0);
      siz = ins->l.i << alignlog2;
      fn->stksiz += siz;
      fn->stksiz = alignup(fn->stksiz, 1 << alignlog2);
      if (fn->stksiz > (1<<16)-1) error(NULL, "'%s' stack frame too big", fn->name);
      stkslots[t] = fn->stksiz;
      *ins = mkinstr(Onop,0,);
      break;
   case Oparam:
      assert(ins->l.t == RICON && ins->l.i < fn->nabiarg);
      if (!fn->abiarg[ins->l.i].isstk)
         *ins = mkinstr(Ocopy, ins->cls, mkref(RREG, fn->abiarg[ins->l.i].reg));
      else /* stack */
         *ins = mkinstr(Oadd, KPTR, mkref(RREG, FP), mkref(RICON, 16+fn->abiarg[ins->l.i].stk));
      break;
   case Oadd: case Osub:
      if (ins->r.t == RICON && ins->r.i < 0) {
         op = ins->op ^= 1;
         ins->r.i = -ins->r.i;
      }
      fixarg(&ins->l, ins, blk, curi);
      fixarg(&ins->r, ins, blk, curi);
      break;
   case Oand: case Oior: case Oxor:
   case Oshl: case Osar: case Oslr:
      fixarg(&ins->r, ins, blk, curi);
      break;
   case Oarg:
      fixarg(&ins->r, ins, blk, curi);
      break;
   case Ocall:
      selcall(fn, ins, blk, curi);
      break;
   case Oloads8: case Oloadu8: case Oloads16: case Oloadu16:
   case Oloads32: case Oloadu32: case Oloadi64:
      loadstoreaddr(blk, &ins->l, curi, 1<<((op - Oloads8)/2));
      break;
   case Ostore8: case Ostore16: case Ostore32: case Ostore64:
      loadstoreaddr(blk, &ins->l, curi, 1<<(op - Ostore8));
      fixarg(&ins->r, ins, blk, curi);
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
         enum irclass cls = c.t == RICON ? KI32 : c.t == RXCON && conht[c.i].cls ? conht[c.i].cls : KPTR;
         int curi = blk->ins.n;

         c = insertinstr(blk, blk->ins.n, mkinstr(Ocopy, cls, c));
         sel(fn, &instrtab[c.i], blk, &curi);
      }
      if (!oiscmp(instrtab[c.i].op)) {
         struct instr *ins;
         int curi = blk->ins.n;
         blk->jmp.arg[0] = insertinstr(blk, blk->ins.n, mkinstr(Oneq, insrescls(instrtab[c.i]), c, ZEROREF));
         ins = &instrtab[blk->jmp.arg[0].i];
         if (kisflt(ins->cls)) {
            ins->r = insertinstr(blk, curi, mkinstr(Ocopy, ins->cls, ZEROREF));
         }
         ins->keep = 1;
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
         }
      }
   }
}

void
aarch64_isel(struct function *fn)
{
   extern int ninstr;
   struct block *blk = fn->entry;

   fn->stksiz = 0;
   stkslots = allocz(fn->passarena, (nstkslots = ninstr) * sizeof *stkslots, 0);
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
      for (i = 0; i < blk->ins.n; ++i) {
         struct instr *ins = &instrtab[blk->ins.p[i]];
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
