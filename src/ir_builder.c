#include "ir.h"

/* binary arithmetic builder with peephole optimizations */
Ref
irbinop(Function *fn, enum op op, enum irclass k, Ref l, Ref r) {
   static const Ref ONE = {.t=RICON, .i=1};
   s64int iv;
   Ref c;

   if (foldbinop(&c, op, k, l, r))
      return c;

   switch (op) {
   case Oadd:
      if (l.bits == ZEROREF.bits) return r; /* 0 + x ==> x */
      if (r.bits == ZEROREF.bits) return l; /* x + 0 ==> x */
      break;
   case Osub:
      if (r.bits == ZEROREF.bits) return l; /* x - 0 ==> x */
      if (kisint(k) && l.bits == r.bits) return ZEROREF; /* x - x ==> 0 */
      break;
   case Omul:
      if (isnumcon(l)) rswap(l, r); /* put const in rhs */
      if (kisflt(k)) break;
      if (r.bits == ZEROREF.bits) /* x * 0 ==> 0 */
         return ZEROREF;
      if (r.bits == ONE.bits) /* x * 1 ==> x */
         return l;
      if (isintcon(r) && ispo2(iv = intconval(r))) {
         /* x * 2^y ==> x << y */
         op = Oshl;
         r = mkref(RICON, ilog2(iv));
      } else if (isintcon(r) && intconval(r) == -1) {
         /* x * -1 ==> -x */
         op = Oneg;
         r.bits = 0;
      }
      break;
   case Odiv:
      if (kisflt(k)) break;
      if (r.bits == ONE.bits) /* x/1 ==> x */
         return l;
      if (isintcon(r) && intconval(r) == -1) {
         /* x / -1 ==> -x */
         op = Oneg;
         r.bits = 0;
      }
      break;
   case Oudiv:
      if (r.bits == ONE.bits) /* x/1 ==> x */
         return l;
      if (isintcon(r) && ispo2(iv = intconval(r))) {
         /* x / 2^y ==> x >> y */
         op = Oslr;
         r = mkref(RICON, ilog2(iv));
      }
      break;
   case Orem:
      if (r.bits == ONE.bits || (isintcon(r) && intconval(r) == -1)) {
         /* x % ±1 ==> 0 */
         return ZEROREF;
      }
      break;
   case Ourem:
      if (r.bits == ONE.bits) /* x % 1 ==> 0 */
         return ZEROREF;
      if (isintcon(r) && ispo2(iv = intconval(r))) {
         /* x % 2^y ==> x & 2^y-1 */
         op = Oand;
         r = mkintcon(k, iv - 1);
      }
      break;
   case Oand:
      if (isnumcon(l)) rswap(l, r); /* put const in rhs */
      if (r.bits == ZEROREF.bits) /* x & 0 ==> 0 */
         return ZEROREF;
      break;
   case Oior:
      if (isnumcon(l)) rswap(l, r); /* put const in rhs */
      if (r.bits == ZEROREF.bits) /* x | 0 ==> x */
         return l;
   case Oxor:
      if (isnumcon(l)) rswap(l, r); /* put const in rhs */
      if (r.bits == ZEROREF.bits) /* x ^ 0 ==> x */
         return l;
   case Oshl: case Osar: case Oslr:
      if (r.bits == ZEROREF.bits) /* x shift 0 ==> x */
         return l;
      break;
   case Oequ:
      if (r.bits == l.bits && kisint(k))
         return ONE;
      break;
   case Oneq:
      if (r.bits == l.bits && kisint(k))
         return ZEROREF;
      break;
   case Olth: case Ogth:
   case Olte: case Ogte:
      break;
   case Oulth:
      if (r.bits == ZEROREF.bits) /* x u< 0 ==> f */
         return ZEROREF;
      break;
   case Ougth:
      if (l.bits == ZEROREF.bits) /* 0 u> x ==> f */
         return ZEROREF;
      break;
   case Oulte:
      if (l.bits == ZEROREF.bits) /* 0 u<= x ==> t */
         return ONE;
      break;
   case Ougte:
      if (r.bits == ZEROREF.bits) /* x u>= 0 ==> t */
         return ONE;
      break;
   default:
      assert(!"binop?");
   }
   return fn ? addinstr(fn, mkinstr2(op, k, l, r)) : NOREF;
}

/* implements f32/f64 -> u64 conversion */
static Ref
cvtfu64(Function *fn, enum irclass from, Ref x) {
   Block *t, *f, *merge;
   Ref tmp, phiarg[2];
   /* if (x < 2p63) cvtfXs(x) else (cvtfXs(x - 2p63) | (1<<63)) */
   Ref max = mkfltcon(from, 0x1.0p63);
   enum op cvt = from == KF32 ? Ocvtf32s : Ocvtf64s;
   putcondbranch(fn, irbinop(fn, Olth, from, x, max), t = newblk(fn), f = newblk(fn));

   useblk(fn, t);
   phiarg[0] = irunop(fn, cvt, KI64, x);
   putbranch(fn, merge = newblk(fn));

   useblk(fn, f);
   tmp = irbinop(fn, Osub, from, x, max);
   tmp = irunop(fn, cvt, KI64, tmp);
   phiarg[1] = irbinop(fn, Oior, KI64, tmp, mkintcon(KI64, 1ull<<63));
   putbranch(fn, merge);

   useblk(fn, merge);
   return addphi(fn, KI64, phiarg);
}

/* implements u64 -> f32/f64 conversion */
static Ref
cvtu64f(Function *fn, enum irclass to, Ref x) {
   Block *t, *f, *merge;
   Ref t1, t2, phiarg[2];

   /* if ((s64)x >= 0) cvts64f(x) else cvts64f((x>>1)|(x&1))*2  */

   putcondbranch(fn, irbinop(fn, Ogte, KI64, x, ZEROREF), t = newblk(fn), f = newblk(fn));

   useblk(fn, t);
   phiarg[0] = irunop(fn, Ocvts64f, to, x);
   putbranch(fn, merge = newblk(fn));

   useblk(fn, f);
   t1 = irbinop(fn, Oslr, KI64, x, mkref(RICON, 1));
   t2 = irbinop(fn, Oand, KI64, x, mkref(RICON, 1));
   t1 = irbinop(fn, Oior, KI64, t1, t2);
   t1 = irunop(fn, Ocvts64f, to, t1);
   phiarg[1] = irbinop(fn, Oadd, to, t1, t1);
   putbranch(fn, merge);

   useblk(fn, merge);
   return addphi(fn, to, phiarg);
}

Ref
irunop(Function *fn, enum op op, enum irclass k, Ref a) {
   Ref c;
   Instr *ins = NULL;
   if (foldunop(&c, op, k, a))
      return c;
   if (a.t == RTMP) ins = &instrtab[a.i];
   switch (op) {
   case Oneg:
      if (ins && ins->op == Oneg) /* -(-x) ==> x */
         return ins->l;
      break;
   case Onot:
      if (ins && ins->op == Onot) /* ~(~x) ==> x */
         return ins->l;
      break;
   case Ocvtf32s: case Ocvtf32f64: case Ocvtf64s:
   case Ocvtf64f32: case Ocvts32f: case Ocvtu32f:
   case Ocvts64f:
      break;
   case Ocvtf32u: case Ocvtf64u:
      if (target.arch == ISx86_64 && k == KI64 && fn) {
         /* this should probably be handled in a separate "arithmetic-lowering" pass, earlier than isel
          */
         return cvtfu64(fn, op == Ocvtf32u ? KF32 : KF64, a);
      }
      break;
   case Ocvtu64f:
      /* XXX see above */
      if (target.arch == ISx86_64 && fn)
         return cvtu64f(fn, k, a);
   case Oexts8: case Oextu8: case Oexts16: case Oextu16:
   case Oexts32: case Oextu32:
   case Ocopy:
      break;
   case Obswap16: case Obswap32: case Obswap64:
      break;
   default: assert(!"unop?");
   }
   return fn ? addinstr(fn, mkinstr1(op, k, a)) : NOREF;
}

int allocinstr(void);

Ref
addinstr(Function *fn, Instr ins) {
   int new = allocinstr();
   assert(fn->curblk != NULL);
   instrtab[new] = ins;
   adduse(fn->curblk, new, ins.l);
   adduse(fn->curblk, new, ins.r);
   vpush(&fn->curblk->ins, new);
   return mkref(RTMP, new);
}

Ref
foldaddinstr(Function *fn, Instr ins) {
   if (oisarith(ins.op)) {
      return ins.op < Oadd ? irunop(fn, ins.op, ins.cls, ins.l)
                           : irbinop(fn, ins.op, ins.cls, ins.l, ins.r);
   }
   return addinstr(fn, ins);
}

void
useblk(Function *fn, Block *blk) {
   extern int nerror;
   if (fn->curblk && nerror == 0) assert(fn->curblk->jmp.t && "never finished block");
   if (blk) assert(!blk->jmp.t && "reusing built block");
   if (blk && !blk->lprev) { /* initialize */
      blk->lnext = fn->entry;
      blk->lprev = fn->entry->lprev;
      blk->lprev->lnext = blk;
      blk->id = fn->nblk++;
      fn->entry->lprev = blk;
   }
   fn->curblk = blk;
}

Ref
addphi(Function *fn, enum irclass cls, Ref *r) {
   assert(fn->curblk);
   if (fn->curblk->npred == 0) return UNDREF;
   if (fn->curblk->npred == 1) /* 1-argument phi is identity */
      return *r;

   Ref *refs = NULL;
   xbgrow(&refs, fn->curblk->npred);
   memcpy(refs, r, fn->curblk->npred * sizeof *r);
   vpush(&phitab, refs);
   /*assert(fn->curblk->ins.n == 0);*/
   int new = allocinstr();
   instrtab[new] = mkinstr1(Ophi, cls, {.i=phitab.n-1});
   for (int i = 0; i < fn->curblk->npred; ++i) {
      adduse(fn->curblk, new, r[i]);
   }
   vpush(&fn->curblk->phi, new);
   return mkref(RTMP, new);
}

#define putjump(fn, j, arg0, arg1, T, F) \
   fn->curblk->jmp.t = j;                \
   fn->curblk->jmp.arg[0] = arg0;        \
   fn->curblk->jmp.arg[1] = arg1;        \
   fn->curblk->s1 = T;                   \
   fn->curblk->s2 = F;                   \
   fn->curblk = NULL;

void
putbranch(Function *fn, Block *blk) {
   assert(fn->curblk && blk);
   addpred(blk, fn->curblk);
   putjump(fn, Jb, NOREF, NOREF, blk, NULL);
}

void
putcondbranch(Function *fn, Ref arg, Block *t, Block *f) {
   assert(fn->curblk && t && f);
   if (iscon(arg)) {
      bool truthy;
      if (isintcon(arg)) truthy = intconval(arg) != 0;
      else if (isfltcon(arg)) truthy = fltconval(arg) != 0.0;
      else if (isaddrcon(arg,0)) truthy = 1; /* XXX ok to assume symbols have non null addresses? */
      else goto Cond;
      putbranch(fn, truthy ? t : f);
   } else {
   Cond:
      adduse(fn->curblk, USERJUMP, arg);
      addpred(t, fn->curblk);
      addpred(f, fn->curblk);
      putjump(fn, Jb, arg, NOREF, t, f);
   }
}

void
putreturn(Function *fn, Ref r0, Ref r1) {
   assert(fn->curblk);
   adduse(fn->curblk, USERJUMP, r0);
   adduse(fn->curblk, USERJUMP, r1);
   putjump(fn, Jret, r0, r1, NULL, NULL);
}

void
puttrap(Function *fn) {
   assert(fn->curblk);
   putjump(fn, Jtrap, NOREF, NOREF, NULL, NULL);
}

#undef putjump

/* vim:set ts=3 sw=3 expandtab: */
