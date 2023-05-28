#include "common.h"
#include "parse.h"

static int
targ2hosttype(enum typetag t)
{
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
   } else if (isfltt(t)) return t;
   return 0;
}

static bool
numcast(enum typetag t, struct expr *dst, const struct expr *src)
{
   struct expr tmp;
   enum typetag td = targ2hosttype(t);
   enum typetag ts = targ2hosttype(src->ty.t == TYENUM ? src->ty.backing : src->ty.t);
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
   else if (isfltt(ts)) { dst->i = src->f; goto Narrow; }
   else {
   Narrow:
      switch (td) {
#define I(Ty, Tag) case Tag: dst->i = (Ty) src->i; break;
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
   dst->ty = mktype(t);
   return 1;
}

static bool
unop(struct expr *ex, enum evalmode mode)
{
   struct expr *sub = ex->sub;

   if (sub->t != ENUMLIT && !eval(sub, mode)) return 0;
   switch (ex->t) {
   case ECAST:
   case EPLUS:
      break;
   case ENEG:
      if (isint(sub->ty)) sub->u = -sub->u;
      else assert(isflt(sub->ty)), sub->f = -sub->f;
      break;
   case ECOMPL:
      if (!isint(sub->ty)) return 0;
      sub->u = ~sub->u;
      break;
   case ELOGNOT:
      if (isint(sub->ty)) sub->u = !sub->u;
      else assert(isflt(sub->ty)), sub->u = !sub->f;
      break;
   default:
      return 0;
   }
   if (!numcast(ex->ty.t, ex, sub)) return 0;
   return 1;
}

static bool
binop(struct expr *ex, enum evalmode mode)
{
   union type oty;
   bool flt;
   bool sgn;
   int t;
   struct expr *lhs = &ex->sub[0], *rhs = &ex->sub[1];

   if (!eval(lhs, mode)) return 0;
   if (!eval(rhs, mode)) return 0;
   if (in_range(ex->t, EADD, ESHR))
      oty = ex->ty;
   else
      oty = cvtarith(lhs->ty, rhs->ty);
   if (!numcast(oty.t, lhs, lhs)) return 0;
   if (!numcast(oty.t, rhs, rhs)) return 0;
   flt = isflt(oty);
   sgn = issigned(oty);
   switch (ex->t) {
#define ef else if
   case EADD: if (flt) lhs->f += rhs->f;
              else     lhs->u += rhs->u;
              break;
   case ESUB: if (flt) lhs->f -= rhs->f;
              else     lhs->u -= rhs->u;
              break;
   case EMUL: if (flt) lhs->f *= rhs->f;
              ef (sgn) lhs->i *= rhs->i;
              else     lhs->u *= rhs->u;
              break;
   case EDIV: if (!flt && !rhs->i) return 0;
              ef (flt) lhs->f /= rhs->f;
              ef (sgn) lhs->i /= rhs->i;
              else     lhs->u /= rhs->u;
              break;
   case EREM: if (!rhs->i) return 0;
              ef (sgn) lhs->i %= rhs->i;
              else     lhs->u %= rhs->u;
              break;
   case EBAND: lhs->u &= rhs->u;
               break;
   case EBIOR: lhs->u |= rhs->u;
               break;
   case EXOR:  lhs->u ^= rhs->u;
               break;
   case ESHL:  if (sgn && lhs->i < 0) return 0;
               ef (rhs->i >= 8*targ_primsizes[oty.t]) return 0;
               lhs->u <<= rhs->u;
               break;
   case ESHR:  if (rhs->i >= 8*targ_primsizes[oty.t]) return 0;
               ef (sgn) lhs->i >>= rhs->i;
               else     lhs->u >>= rhs->u;
               break;
   case ELOGAND: if (flt) t = lhs->f && rhs->f;
                 else     t = lhs->u && rhs->u;
                 lhs->u = t;
                 break;
   case ELOGIOR: if (flt) t = lhs->f || rhs->f;
                 else     t = lhs->u || rhs->u;
                 lhs->u = t;
                 break;
   case EEQU: if (flt) t = lhs->f == rhs->f;
              else     t = lhs->u == rhs->u;
              lhs->u = t;
              break;
   case ENEQ: if (flt) t = lhs->f != rhs->f;
              else     t = lhs->u != rhs->u;
              lhs->u = t;
              break;
   case ELTH: if (flt) t = lhs->f < rhs->f;
              ef (sgn) t = lhs->i < rhs->i;
              else     t = lhs->u < rhs->u;
              lhs->u = t;
              break;
   case EGTH: if (flt) t = lhs->f > rhs->f;
              ef (sgn) t = lhs->i > rhs->i;
              else     t = lhs->u > rhs->u;
              lhs->u = t;
              break;
   case ELTE: if (flt) t = lhs->f <= rhs->f;
              ef (sgn) t = lhs->i <= rhs->i;
              else     t = lhs->u <= rhs->u;
              lhs->u = t;
              break;
   case EGTE: if (flt) t = lhs->f >= rhs->f;
              ef (sgn) t = lhs->i >= rhs->i;
              else     t = lhs->u >= rhs->u;
              lhs->u = t;
              break;
   default: return 0;
#undef ef
   }

   return numcast(ex->ty.t, ex, lhs);
}

bool
eval(struct expr *ex, enum evalmode mode)
{
   if (ex->t == ENUMLIT) {
      if (mode <= EVINTCONST) return isint(ex->ty);
      return 1;
   }
   if (isunop(ex->t)) return unop(ex, mode) && eval(ex, mode);
   if (isbinop(ex->t)) return binop(ex, mode) && eval(ex, mode);
   if (ex->t == ESEQ) {
      if (!eval(&ex->sub[0], mode)) return 0;
      *ex = ex->sub[1];
      return eval(ex, mode);
   }
   if (ex->t == ECOND) {
      if (!eval(&ex->sub[0], mode)) return 0;
      if (!eval(&ex->sub[1], mode)) return 0;
      if (!eval(&ex->sub[2], mode)) return 0;
      *ex = ex->sub[!ex->sub[0].u + 1];
      return eval(ex, mode);
   }
   return 0;
}

/* vim:set ts=3 sw=3 expandtab: */
