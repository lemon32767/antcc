#include "c.h"
#include "ir.h"

typedef struct Ref2 { Ref a,b; } Ref2;

Ref
complex2scalar(Function *fn, Type to, const Expr *ex) {
   Ref2 c = compcomplexex(fn, ex, 0);
   Type base = typechild(ex->ty);
   to.t = scalartypet(to);
   if (to.t == TYBOOL) { /* implicit != 0.0 */
      /* meh */
      Ref r = scalarcvt(fn, mktype(TYBOOL), base, c.a),
          i = scalarcvt(fn, mktype(TYBOOL), base, c.b);
      return irbinop(fn, Oior, KI32, r, i);
   } else {
      return scalarcvt(fn, to, base, c.a);
   }
}

Ref2
cvt2complex(Function *fn, Type base, const Expr *ex) {
   assert(isscalar(base) && isflt(base));
   if (iscomplex(ex->ty)) {
      Ref2 c = compcomplexex(fn, ex, 0);
      Type base2 = typechild(ex->ty);
      if (base2.bits != base.bits) {
         c.a = scalarcvt(fn, base, base2, c.a);
         c.b = scalarcvt(fn, base, base2, c.b);
      }
      return c;
   }
   return (Ref2){scalarcvt(fn, base, ex->ty, exprvalue(fn, ex)), mkfltcon_np(type2cls[base.t], 0)};
}

Ref2
complexload(Function *fn, Type base, Ref adr, bool volatyl) {
   assert(isscalar(base) && isflt(base));
   return (Ref2){
      genload(fn, base, adr, volatyl),
      genload(fn, base, genptroffk(fn, adr, targ_primsizes[base.t]), volatyl)
   };
}

void
complexstore(Function *fn, Type base, Ref adr, Ref2 c, bool volatyl) {
   assert(isscalar(base) && isflt(base));
   genstore(fn, base, adr, c.a, volatyl);
   genstore(fn, base, genptroffk(fn, adr, targ_primsizes[base.t]), c.b, volatyl);
}

Ref
complex2addr_cvt(Function *fn, Type to, const Expr *ex) {
   assert(iscomplex(to));
   if (ex->ty.bits == to.bits && islvalue(ex))
      return expraddr(fn, ex);
   Type base = typechild(to);
   Ref2 uv = cvt2complex(fn, base, ex);
   Ref r = addinstr(fn, mkalloca(targ_primsizes[base.t]*2, targ_primalign[base.t]));
   complexstore(fn, base, r, uv, 0);
   return r;
}

Ref2
compcomplexex(Function *fn, const Expr *ex, bool discard) {
   static const Ref2 NIL = { 0 };
   assert(iscomplex(ex->ty));
   Type sty = typechild(ex->ty);
   assert(isflt(sty));
   enum irclass cls = type2cls[sty.t];
   const Expr *sub = ex->sub;
   Ref adr, r;
   Ref2 q, w, z;
   Type ty;
   enum op op;
   Block *tr, *fl, *end;
   switch (ex->t) {
   case ENUMLIT: /* imaginary literal e.g. 1.0iF */
      if (discard) return NIL;
      return (Ref2){mkfltcon_np(cls, 0.0),
                    sty.t == TYFLOAT ? mkfltscon(ex->fs) : mkfltdcon(ex->fd)};
   case ESYM:
      if (discard && !(ex->qual & QVOLATILE)) return NIL;
      adr = expraddr(fn, ex);
   Load:
      return complexload(fn, sty, adr, ex->qual & QVOLATILE);
   case EVAARG:
      adr = builtin_va_arg_comp(fn, ex, discard);
      if (discard) return NIL;
      goto Load;
   case EGETF:
      if (discard && !(ex->qual & QVOLATILE)) return NIL;
      adr = expraddr(fn, ex);
      goto Load;
   case ECAST:
      /* fallthru */
   case EPLUS:
      if (discard) {
         expreffects(fn, sub);
         return NIL;
      }
      return cvt2complex(fn, sty, sub);
   case ENEG:
      q = compcomplexex(fn, sub, discard);
      if (discard) return q;
      assert(sub->ty.bits == ex->ty.bits);
      return (Ref2){irunop(fn, Oneg, cls, q.a), irunop(fn, Oneg, cls, q.b)};
   case EDEREF:
      discard &= (ex->qual & QVOLATILE) == 0;
      adr = compileexpr(fn, sub, discard);
      if (discard) return NIL;
      goto Load;
   case ESUB:
      op = Osub;
      goto Additive;
   case EADD:
      op = Oadd;
   Additive:
      if (discard) {
         expreffects(fn, &sub[0]), expreffects(fn, &sub[1]);
         return NIL;
      }
      q = cvt2complex(fn, sty, &sub[0]);
      w = cvt2complex(fn, sty, &sub[1]);
      /* per ISO C, must avoid unnecessarily addition of constant 0.0, not a NO-OP in IEE754 */
      if (iscomplex(sub[0].ty) && iscomplex(sub[1].ty)) { /* complex + complex */
      CmplxCmplx:
         return (Ref2){
            irbinop(fn, op, cls, q.a, w.a),
            irbinop(fn, op, cls, q.b, w.b),
         };
      } else if (iscomplex(sub[0].ty)) { /* complex + real */
         return (Ref2){ irbinop(fn, op, cls, q.a, w.a), q.b };
      } else { /* real + complex */
         if (op == Osub) goto CmplxCmplx;
         return (Ref2){ irbinop(fn, op, cls, q.a, w.a), w.b };
      }
   case EMUL:
      if (discard) {
         expreffects(fn, &sub[0]), expreffects(fn, &sub[1]);
         return NIL;
      }
      int swp = 0;
      if (iscomplex(sub[0].ty) && !iscomplex(sub[1].ty)) { /* complex * real */
      Scale:
         q = cvt2complex(fn, sty, &sub[swp]);
         r = scalarcvt(fn, sty, sub[swp^1].ty, exprvalue(fn, &sub[swp^1]));
         return (Ref2){
            irbinop(fn, Omul, cls, q.a, r),
            irbinop(fn, Omul, cls, q.b, r),
         };
      } else if (!iscomplex(sub[0].ty) && iscomplex(sub[1].ty)) { /* real * complex */
         swp = 1;
         goto Scale;
      } else RuntimeCall: { /* complex * complex */
         /* call runtime function */
         static const char *rtfn[][3] = {
            {"__mulsc3", "__muldc3", "__mulxc3"},
            {"__divsc3", "__divdc3", "__divxc3"},
         };
         Instr ins = {Ocall, KPTR};
         IRType typ = mkirtype(ex->ty);
         q.a = complex2addr_cvt(fn, ex->ty, &sub[0]);
         q.b = complex2addr_cvt(fn, ex->ty, &sub[1]);
         addinstr(fn, mkarginstr(typ, q.a));
         addinstr(fn, mkarginstr(typ, q.b));
         ins.l = mksymref(intern(rtfn[ex->t == EDIV][type2cls[sty.t] - KF32]), SFUNC);
         ins.r = mkcallarg(mkirtype(ex->ty), 2, -1);
         adr = addinstr(fn, ins);
         goto Load;
      }
   case EDIV:
      if (discard) {
         expreffects(fn, &sub[0]), expreffects(fn, &sub[1]);
         return NIL;
      }
      if (iscomplex(sub[0].ty) && !iscomplex(sub[1].ty)) { /* complex / real */
         q = cvt2complex(fn, sty, &sub[0]);
         r = scalarcvt(fn, sty, sub[1].ty, exprvalue(fn, &sub[1]));
         return (Ref2){
            irbinop(fn, Odiv, cls, q.a, r),
            irbinop(fn, Odiv, cls, q.b, r),
         };
      } else { /* any / complex */
         goto RuntimeCall;
      }
   case ESET:
      assert(sub[0].ty.bits == ex->ty.bits);
      q = cvt2complex(fn, sty, &sub[1]);
      adr = expraddr(fn, &sub[0]);
      complexstore(fn, sty, adr, q, sub[0].qual & QVOLATILE);
      return q;
   case EPREINC:
   case EPOSTINC:
   case EPREDEC:
   case EPOSTDEC:
      assert(ex->ty.bits == sub->ty.bits);
      adr = expraddr(fn, sub);
      q = complexload(fn, sty, adr, sub->qual & QVOLATILE);
      w.a = irbinop(fn, ex->t < EPREDEC ? Oadd : Osub, cls, q.a, mkfltcon_np(cls, 1.0));
      w.b = q.b;
      complexstore(fn, sty, adr, w, sub->qual & QVOLATILE);
      return (ex->t == EPREINC || ex->t == EPREDEC) ? w : q;
   case ESETMUL:
   case ESETDIV:
   case ESETSUB:
   case ESETADD:
      ty = cvtarith(sub[0].ty, sub[1].ty);
      adr = expraddr(fn, &sub[0]);
      w = cvt2complex(fn, sty, &sub[1]);
      q = compcomplexex(fn,
            &mkexpr(ex->t - ESETADD + EADD, ex->span, ty, .sub = (Expr *)sub), 0);
      if (sub[0].ty.bits == sub[1].ty.bits) {
         complexstore(fn, sty, adr, q, sub[0].qual & QVOLATILE);
      } else {
         Type to = typechild(sub[0].ty), from = typechild(ty);
         w.a = scalarcvt(fn, to, from, q.a);
         w.b = scalarcvt(fn, to, from, q.b);
         complexstore(fn, sty, adr, w, sub[0].qual & QVOLATILE);
      }
      return q;
   case ECALL:
      adr = compilecall(fn, ex);
      if (discard) return NIL;
      goto Load;
   case ECOND:
      for (bool c; knowntruthy(&c, &ex->sub[0]);) {
         if (discard) {
            expreffects(fn, &ex->sub[2-c]);
            return NIL;
         }
         return cvt2complex(fn, sty, &ex->sub[2-c]);
      }
      condjump(fn, &sub[0], tr = newblk(fn), fl = newblk(fn));
      useblk(fn, tr);
      q = cvt2complex(fn, sty, &sub[1]);
      end = newblk(fn);
      if (fn->curblk)
         putbranch(fn, end);
      useblk(fn, fl);
      w = cvt2complex(fn, sty, &sub[2]);
      if (fn->curblk)
         putbranch(fn, end);
      useblk(fn, end);
      if (discard) return NIL;
      z.a = addphi(fn, cls, (Ref[2]){q.a, w.a});
      z.b = addphi(fn, cls, (Ref[2]){q.b, w.b});
      return z;
   case EELVIS:
      for (bool c; knowntruthy(&c, &ex->sub[0]);) {
         if (discard) {
            if (!c) expreffects(fn, &ex->sub[1]);
            return NIL;
         }
         return cvt2complex(fn, sty, &ex->sub[c^1]);
      }
      if (discard) {
         condjump(fn, &sub[0], end = newblk(fn), fl = newblk(fn));
         useblk(fn, fl);
         expreffects(fn, &sub[1]);
         if (fn->curblk)
            putbranch(fn, end);
         useblk(fn, end);
         return NIL;
      } else {
         Ref2 lhs;
         Ref cnd;
         if (iscomplex(sub[0].ty)) {
            Type lbase = typechild(sub[0].ty);
            lhs = compcomplexex(fn, &sub[0], 0);
            Ref lr = scalarcvt(fn, mktype(TYBOOL), lbase, lhs.a),
                li = scalarcvt(fn, mktype(TYBOOL), lbase, lhs.b);
            cnd = irbinop(fn, Oior, KI32, lr, li);
            if (lbase.bits != sty.bits) {
               lhs.a = scalarcvt(fn, sty, lbase, lhs.a);
               lhs.b = scalarcvt(fn, sty, lbase, lhs.b);
            }
         } else {
            Ref lv = exprvalue(fn, &sub[0]);
            cnd = lv;
            lhs.a = scalarcvt(fn, sty, sub[0].ty, lv);
            lhs.b = mkfltcon_np(cls, 0.0);
         }
         putcondbranch(fn, cnd, end = newblk(fn), fl = newblk(fn));
         useblk(fn, fl);
         w = cvt2complex(fn, sty, &sub[1]);
         if (fn->curblk)
            putbranch(fn, end);
         useblk(fn, end);
         z.a = addphi(fn, cls, (Ref[2]){lhs.a, w.a});
         z.b = addphi(fn, cls, (Ref[2]){lhs.b, w.b});
         return z;
      }
   case ESEQ:
      expreffects(fn, &sub[0]);
      return compcomplexex(fn, &sub[1], discard);
   case EIRVALUE:
      if (fn->curblk && ex->irref.blk) {
         putbranch(fn, (*ex->irref.blk)[0]);
         useblk(fn, (*ex->irref.blk)[1]);
      }
      if (discard) return NIL;
      return complexload(fn, sty, (Ref){.bits = ex->irref.bits}, 0);
   default: assert(!"nyi cmplx expr");
   }
}
