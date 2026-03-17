#include "c.h"
#include "ir.h"
#include <limits.h>

static int
targ2hosttype(enum typetag t)
{
   if (t == TYPTR) t = targ_64bit ? TYUVLONG : TYUINT;
   if (isintt(t)) {
      int siz = targ_primsizes[t];
      int sgn = issignedt(t);
#define U(Ty,Tag) if (!sgn & (siz == sizeof(unsigned Ty))) return Tag;
#define S(Ty,Tag) if ( sgn & (siz == sizeof(signed   Ty))) return Tag;
      U(char, TYUCHAR)
      S(char, TYSCHAR)
      U(short, TYUSHORT)
      S(short, TYSHORT)
      U(int, TYUINT)
      S(int, TYINT)
      U(long long, TYUVLONG)
      S(long long, TYVLONG)
#undef U
#undef S
   } else if (t == TYLDOUBLE) return TYDOUBLE;
   else if (isfltt(t) || iscomplext(t)) return t;
   return 0;
}

static bool
numcast(union type ty, struct expr *dst, const struct expr *src)
{
   enum typetag td = targ2hosttype(scalartypet(ty)),
                ts = targ2hosttype(scalartypet(src->ty));
   vlong isrc;
   struct expr tmp;
   if (src == dst) tmp = *src, src = &tmp;

   assert(src->t == ENUMLIT);
#define TT(d,s) (td == d && ts == s)
#define TF(d) (td == d && isfltt(ts))
   if (!ts || !td) return 0;
   else if (TT(TYFLOAT, TYFLOAT)) dst->f = (float) src->f;
   else if (TT(TYFLOAT, TYDOUBLE)) dst->f = (float) src->f;
   else if (TT(TYDOUBLE, TYFLOAT)) dst->f = src->f;
   else if (TT(TYDOUBLE, TYDOUBLE)) dst->f = src->f;
   else if (TT(TYFLOAT, TYUVLONG)) dst->f = (float) src->u;
   else if (TT(TYDOUBLE, TYUVLONG)) dst->f = (double) src->u;
   else if (td == TYFLOAT) dst->f = (float) src->i;
   else if (td == TYDOUBLE) dst->f = (double) src->i;
   else if (TF(TYUVLONG)) dst->u = src->f;
   else if (TF(TYBOOL)) dst->i = (bool) src->f;
   else if (isfltt(ts)) { isrc = src->f; goto Narrow; }
   else {
      isrc = src->i;
   Narrow:
      switch (td) {
#define I(Ty, Tag) case Tag: dst->i = (Ty) isrc; break;
      I(bool, TYBOOL)
      I(signed   char, TYSCHAR)
      I(unsigned char, TYUCHAR)
      I(signed   short, TYSHORT)
      I(unsigned short, TYUSHORT)
      I(signed   int, TYINT)
      I(unsigned int, TYUINT)
      I(signed   long long, TYVLONG)
      I(unsigned long long, TYUVLONG)
#undef I
      case TYFLOAT: dst->f = (float) src->f; break;
      case TYDOUBLE: dst->f = src->f; break;
      default: assert(0 && "bad cast?");
      }
   }
#undef TT
#undef TF

   dst->t = ENUMLIT;
   dst->ty = ty;
   return 1;
}

static struct expr *
lit2ssym(struct expr *ex)
{
   ex->ssym.sym = xcon2sym(expraddr(NULL, ex).i);
   ex->ssym.local = 1;
   ex->ssym.func = ex->ssym.off = 0;
   ex->t = ESSYMREF;
   return ex;
}

static struct expr
staticaddrof(struct expr *ex, enum evalmode mode)
{
   struct expr ret = { .ty = mkptrtype(ex->ty, ex->qual), .span = ex->span };
   if (ex->t == ESYM && ex->ty.t < NTYPETAG) {
      const struct decl *decl = &declsbuf.p[ex->decl];
      if (decl->sym && (decl->scls & (SCAUTO|SCREGISTER)) == 0) {
         ret.t = ESSYMREF;
         ret.ssym.sym = decl->sym;
         ret.ssym.off = 0;
         ret.ssym.func = decl->ty.t == TYFUNC;
         ret.ssym.local = (decl->scls == SCSTATIC || decl->isdef);
      }
   } else if (ex->t == EDEREF && eval(ex->sub, EVSTATICINI)) {
      ret = *ex->sub;
   } else if (ex->t == EGETF && (ret = staticaddrof(ex->sub, mode)).t) {
      if (ret.t == ESSYMREF) {
         ex->t = ESSYMREF;
         vlong off = (vlong) ret.ssym.off + ex->fld.off;
         if ((int) off != off) return ret.t = 0, ret;
         ret.ssym.off = off;
      } else if (ret.t == ENUMLIT) {
         ret.u += ex->fld.off;
      } else assert(0);
   } else if (ex->t == ESTRLIT || (mode == EVSTATICINI && ex->t == EINIT)) {
      ret = *lit2ssym(ex);
   } else if (ex->t == ENUMLIT || ex->t == ESSYMREF) ret = *ex;
   return ret;
}

static bool
isstaticlval(const struct expr *ex, enum evalmode mode)
{
   return ex->t == ESTRLIT || ex->t == ESSYMREF || (mode == EVSTATICINI && ex->t == EINIT);
}

static bool
truthy(const struct expr *ex)
{
   switch (ex->t) {
   default: assert(0 && "!scalar?");
   case ENUMLIT:
      return isflt(ex->ty) ? ex->f != 0.0 : ex->u != 0;
   case ESTRLIT: case ESSYMREF:
      return 1;
   }
}

static bool
unop(struct expr *ex, enum evalmode mode)
{
   struct expr *sub = ex->sub;

   if (mode >= EVSTATICINI && ex->t == EDEREF) {
      uvlong off;
      uchar *p;
      uint len;
      uint csiz;
      /* HACK */
      if (sub->t == ESTRLIT) {
         /*  *"s"  */
         off = 0;
         p = sub->s.p, len = sub->s.n;
         csiz = typesize(typechild(sub->ty));
      StrRead:
         if (off > len) return 0;
         ex->t = ENUMLIT;
         ex->ty = mktype(TYINT);
         if (off == len) ex->u = 0;
         else if (csiz == 1) ex->u = p[off];
         else if (csiz == 2) ex->u = ((short *)p)[off];
         else if (csiz == 4) ex->u = ((int *)p)[off];
         return 1;
      } else if (sub->t == EADD && sub->sub[0].t == ESTRLIT && eval(&sub->sub[1], EVINTCONST)) {
         /*  "s"[0]  */
         assert(sub->sub[1].t == ENUMLIT && isint(sub->sub[1].ty));
         off = sub->sub[1].u;
         p = sub->sub[0].s.p, len = sub->sub[0].s.n;
         csiz = typesize(typechild(sub->sub[0].ty));
         goto StrRead;
      } else if (sub->t == EADD && sub->sub[1].t == ESTRLIT && eval(&sub->sub[0], EVINTCONST)) {
         /*  0["s"]  */
         assert(sub->sub[0].t == ENUMLIT && isint(sub->sub[0].ty));
         off = sub->sub[0].u;
         p = sub->sub[1].s.p, len = sub->sub[1].s.n;
         csiz = typesize(typechild(sub->sub[1].ty));
         goto StrRead;
      } else return 0;
   } else if (ex->t == EADDROF) {
      assert(ex->ty.t == TYPTR);
      struct expr ex2 = staticaddrof(ex->sub, mode);
      if (!ex2.t) return 0;
      ex2.span = ex->span;
      ex2.ty = ex->ty;
      *ex = ex2;
      return 1;
   } else if (ex->t == EGETF && !ex->fld.bitsiz) {
      /* <lvalue>.memb -> is an address lvalue if 'memb' is of array type */
      if (ex->ty.t == TYARRAY) {
         struct expr ex2;
         if ((ex2 = staticaddrof(ex->sub, mode)).t) {
            if (ex2.t == ENUMLIT) {
               ex->t = ENUMLIT;
               ex->u = ex2.u + ex->fld.off;
               return 1;
            } else {
               assert(ex2.t == ESSYMREF);
               ex->t = ESSYMREF;
               vlong off = (vlong) sub->ssym.off + ex->fld.off;
               if ((int) off != off) return 0;
               ex->ssym = ex2.ssym;
               ex->ssym.off = off;
               return mode >= EVSTATICINI;
            }
         }
      }
      return 0;
   }
   if (!eval(sub, mode)) return 0;
   switch (ex->t) {
   case ECAST:
      if (ex->ty.t == TYPTR && sub->t == ENUMLIT) {
         ex->t = ENUMLIT;
         ex->u = sub->u;
         return 1;
      } else if (isstaticlval(sub, mode)
       && (ex->ty.t == TYPTR || (isint(ex->ty) && typesize(ex->ty) == targ_primsizes[TYPTR]))) {
         /* ptr -> int */
         if (sub->t == ESTRLIT || sub->t == EINIT)
            lit2ssym(sub);
         sub->span = ex->span, sub->ty = ex->ty;
         *ex = *sub;
         return 1;
      }
      break;
   case EPLUS:
      break;
   case ENEG:
      if (sub->t != ENUMLIT) return 0;
      if (isint(sub->ty)) sub->u = -sub->u;
      else assert(isflt(sub->ty)), sub->f = -sub->f;
      break;
   case ECOMPL:
      if (sub->t != ENUMLIT) return 0;
      assert(isint(sub->ty));
      sub->u = ~sub->u;
      break;
   case ELOGNOT:
      sub->u = !truthy(sub);
      sub->t = ENUMLIT;
      break;
   default:
      return 0;
   }
   if (sub->t != ENUMLIT || !numcast(ex->ty, ex, sub)) return 0;
   return 1;
}

static bool
binop(struct expr *ex, enum evalmode mode)
{
   struct expr *a = &ex->sub[0], *b = &ex->sub[1];
   if (!eval(a, mode)) return 0;
   union type opty;
   if (in_range(ex->t, EADD, ESHR))
      opty = ex->ty;
   else /* compare, logical, set (result type != operation type) */
      opty = cvtarith(a->ty, b->ty);
   if (isstaticlval(a, mode)) {
      if ((ex->t == EADD || ex->t == ESUB) && eval(b, mode) && b->t == ENUMLIT) {
         assert(isint(b->ty));
         assert(in_range(a->ty.t, TYPTR, TYARRAY));
         if (a->t == ESTRLIT) {
            lit2ssym(a);
         } else assert(a->t == ESSYMREF);
         vlong addend = b->i * typesize(typechild(a->ty)),
               off = a->ssym.off + (uvlong) (ex->t == EADD ? addend : -addend);
         ex->t = ESSYMREF;
         if ((int) off != off) return 0;
         ex->ssym = a->ssym;
         ex->ssym.off = off;
         return 1;
      }
      return 0;
   }

   enum { U = 0,     S = 1<<8 ,       F = 1<<9 };
   int op = issigned(opty)<<8 | isflt(opty)<<9 | ex->t;
   bool c;
   if (ex->t != ELOGAND && ex->t != ELOGIOR) {
      if (!numcast(opty, a, a)) return 0;
      if (!eval(b, mode) || !numcast(opty, b, b)) return 0;
   }
   switch (op) {
   case EADD|U:
   case EADD|S: a->u += opty.t == TYPTR ? b->u * typesize(typechild(opty)) : b->u; break;
   case EADD|F: a->f += b->f; break;

   case ESUB|U:
   case ESUB|S: if (opty.t == TYPTR) {
                   assert(a->t == ENUMLIT && b->t == ENUMLIT);
                   assert(!isincomplete(typechild(ex->ty)));
                   a->u = (a->u - b->u) / typesize(typechild(ex->ty));
                } else a->u -= b->u;
                break;
   case ESUB|F: a->f -= b->f; break;

   case EMUL|U:
   case EMUL|S: a->u *= b->u; break;
   case EMUL|F: a->f *= b->f; break;

   case EDIV|U: if (!b->u) return 0;
                a->u /= b->u;
                break;
   case EDIV|S: if (!b->i) return 0;
                if (a->i == LLONG_MIN && b->i == -1) break;
                a->i /= b->i;
                break;
   case EDIV|F: a->f /= b->f; break;

   case EREM|U: if (!b->u) return 0;
                a->u %= b->u;
                break;
   case EREM|S: if (!b->i) return 0;
                if (a->i == LLONG_MIN && b->i == -1) a->i = 0;
                a->i %= b->i;
                break;

   case EBAND|U:
   case EBAND|S: a->u &= b->u; break;

   case EBIOR|U:
   case EBIOR|S: a->u |= b->u; break;

   case EXOR|U:
   case EXOR|S: a->u ^= b->u; break;

   case ESHL|S: if (a->i < 0) return 0;
   case ESHL|U: if (b->u >= 8*targ_primsizes[opty.t]) return 0;
                a->u <<= b->u;
                break;

   case ESHR|U:  if (b->u >= 8*targ_primsizes[opty.t]) return 0;
                 a->u >>= b->i;
                 break;
   case ESHR|S:  if (b->u >= 8*targ_primsizes[opty.t]) return 0;
                 a->i >>= b->i;
                 break;

   case EEQU|U:
   case EEQU|S: a->u = a->u == b->u; break;
   case EEQU|F: a->u = a->f == b->f; break;

   case ENEQ|U:
   case ENEQ|S: a->u = a->u != b->u; break;
   case ENEQ|F: a->u = a->f != b->f; break;

   case ELTH|U: a->u = a->u < b->u; break;
   case ELTH|S: a->u = a->i < b->i; break;
   case ELTH|F: a->u = a->f < b->f; break;

   case EGTH|U: a->u = a->u > b->u; break;
   case EGTH|S: a->u = a->i > b->i; break;
   case EGTH|F: a->u = a->f > b->f; break;

   case ELTE|U: a->u = a->u <= b->u; break;
   case ELTE|S: a->u = a->i <= b->i; break;
   case ELTE|F: a->u = a->f <= b->f; break;

   case EGTE|U: a->u = a->u >= b->u; break;
   case EGTE|S: a->u = a->i >= b->i; break;
   case EGTE|F: a->u = a->f >= b->f; break;

   case ELOGAND|U:
   case ELOGAND|S:
   case ELOGAND|F:
      c = (op & F) ? a->f : a->u;
      if (c) {
         if (!eval(b, mode) || !numcast(opty, b, b)) return 0;
         c = (op & F) ? b->f : b->u;
      }
      a->u = c;
      break;

   case ELOGIOR|U:
   case ELOGIOR|S:
   case ELOGIOR|F:
      c = op & F ? a->f : a->u;
      if (!c) {
         if (!eval(b, mode) || !numcast(opty, b, b)) return 0;
         c = (op & F) ? b->f : b->u;
      }
      a->u = c;
      break;
   default: return 0;
   }

   if (!in_range(ex->t, EADD, ESHR)) {
      a->t = ENUMLIT;
      a->ty = mktype(TYINT);
   }
   return numcast(ex->ty, ex, a);
}

bool
eval(struct expr *ex, enum evalmode mode)
{
   switch (ex->t) {
   case EGETF: goto Unop;
   case ESEQ:
      if (!eval(&ex->sub[0], mode)) return 0;
      *ex = ex->sub[1];
      return eval(ex, mode);
   case ECOND:
      if (!eval(&ex->sub[0], mode)) return 0;
      *ex = ex->sub[2-truthy(&ex->sub[0])];
      return eval(ex, mode);
   case EINIT:
      for (struct initval *v = ex->init->vals; v; v = v->next) {
         if (!eval(&v->ex, mode)) return 0;
      }
      return 1;
   case ENUMLIT:
      if (mode <= EVINTCONST) return !isflt(ex->ty);
      return 1;
   case ESYM:
      if (in_range(ex->ty.t, TYARRAY, TYFUNC)
       && mode >= EVSTATICINI) {
         struct expr ex2 = staticaddrof(ex, mode);
         if (ex2.t) {
            union type ty = ex->ty;
            *ex = ex2;
            ex->ty = ty;
            return 1;
         }
      }
      return 0;
   case ESTRLIT: case ESSYMREF:
      return mode >= EVSTATICINI;
   default:
      if (isunop(ex->t)) Unop: return unop(ex, mode);
      if (isbinop(ex->t)) return binop(ex, mode);
   }
   return 0;
}

/* vim:set ts=3 sw=3 expandtab: */
