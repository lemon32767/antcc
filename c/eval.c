#include "c.h"

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
   else if (isfltt(t)) return t;
   return 0;
}

static bool
numcast(union type ty, struct expr *dst, const struct expr *src)
{
   struct expr tmp;
   enum typetag td = targ2hosttype(scalartypet(ty));
   enum typetag ts = targ2hosttype(scalartypet(src->ty));
   vlong isrc;
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

static bool
unop(struct expr *ex, enum evalmode mode)
{
   struct expr *sub = ex->sub;

   if (mode >= EVSTATICINI && ex->t == EDEREF) {
      uvlong off;
      uchar *p;
      uint len;
      uint csiz;
      if (sub->t == ESTRLIT) {
         /*  *"s"  */
         off = 0;
         p = sub->s.p, len = sub->s.n;
         csiz = typesize(typechild(sub->ty));
      } else if (sub->t == EADD && sub->sub[0].t == ESTRLIT && eval(&sub->sub[1], EVINTCONST)) {
         /*  "s"[0]  */
         assert(sub->sub[1].t == ENUMLIT && isint(sub->sub[1].ty));
         off = sub->sub[1].u;
         p = sub->sub[0].s.p, len = sub->sub[0].s.n;
         csiz = typesize(typechild(sub->sub[0].ty));
      } else if (sub->t == EADD && sub->sub[1].t == ESTRLIT && eval(&sub->sub[0], EVINTCONST)) {
         /*  0["s"]  */
         assert(sub->sub[0].t == ENUMLIT && isint(sub->sub[0].ty));
         off = sub->sub[0].u;
         p = sub->sub[1].s.p, len = sub->sub[1].s.n;
         csiz = typesize(typechild(sub->sub[1].ty));
      } else return 0;
      if (off > len) return 0;
      ex->t = ENUMLIT;
      ex->ty = mktype(TYINT);
      if (off == len) ex->u = 0;
      else if (csiz == 1) ex->u = p[off];
      else if (csiz == 2) ex->u = ((short *)p)[off];
      else if (csiz == 4) ex->u = ((int *)p)[off];
      return 1;
   }
   if (ex->t == EADDROF) {
      assert(ex->ty.t == TYPTR);
      /* oh boy */
      /* match &(*(T *)12345).fld */
      if (sub->t == EGETF && sub->sub->t == EDEREF && eval(sub->sub->sub, EVFOLD) && sub->sub->sub->t == ENUMLIT) {
         ex->t = ENUMLIT;
         ex->u = sub->sub->sub->u + sub->fld.off;
         return 1;
      }
   }
   if (sub->t != ENUMLIT && !eval(sub, mode)) return 0;
   if (sub->t != ENUMLIT) return 0;
   switch (ex->t) {
   case ECAST:
      if (ex->ty.t == TYPTR) {
         ex->t = ENUMLIT;
         ex->u = sub->u;
         return 1;
      }
      break;
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
      if (isint(sub->ty) || isptrcvt(sub->ty)) sub->u = !sub->u;
      else assert(isflt(sub->ty)), sub->u = !sub->f;
      break;
   default:
      return 0;
   }
   if (!numcast(ex->ty, ex, sub)) return 0;
   return 1;
}

/* link time constants */
static bool
isglobsym(const struct expr *ex)
{
   return ex->t == ESTRLIT || (ex->t == ESYM && ex->ty.t < NTYPETAG && (declsbuf.p[ex->decl].scls & (SCSTATIC | SCEXTERN)));
}

static bool
isaddrconst(struct expr *ex)
{
   if (ex->t == ECAST)
      return isaddrconst(ex->sub) || (eval(ex->sub, EVSTATICINI) && ex->sub->t == ENUMLIT);
   if (ex->t == EADDROF && (isglobsym(ex->sub) || (ex->sub->t == EGETF && isglobsym(ex->sub->sub))))
      return 1;
   if (ex->t == EADDROF && ex->sub->t == EDEREF && isaddrconst(ex->sub->sub)) {
      *ex = *ex->sub->sub;
      return 1;
   }
   if (ex->t == EADDROF && ex->sub->t == EINIT)
      return eval(ex->sub, EVSTATICINI);
   if (ex->t == EGETF && ex->ty.t == TYARRAY && isglobsym(ex->sub))
      return 1;
   if (isglobsym(ex) && in_range(ex->ty.t, TYARRAY, TYFUNC))
      return 1;
   if (ex->t == ESUB) {
      if (isaddrconst(&ex->sub[0]) && isint(ex->sub[1].ty) && eval(&ex->sub[1], EVSTATICINI)) {
         assert(ex->sub[1].t == ENUMLIT);
         if (eval(&ex->sub[0], EVSTATICINI) && ex->sub[0].t == ENUMLIT) {
            /* handles (char *)10 - 5 */
            ex->u = ex->sub[0].u - ex->sub[1].u * typesize(typechild(ex->sub[0].ty));
            ex->t = ENUMLIT;
         }
         return 1;
      }
   } else if (ex->t == EADD) {
      for (int swp = 0; swp < 2; ++swp)
         if (isaddrconst(&ex->sub[swp]) && isint(ex->sub[swp^1].ty) && eval(&ex->sub[swp^1], EVSTATICINI)) {
            assert(ex->sub[swp^1].t == ENUMLIT);
            if (eval(&ex->sub[swp], EVSTATICINI) && ex->sub[swp].t == ENUMLIT) {
               /* handles (char *)1234 + 5678 */
               ex->u = ex->sub[swp].u + ex->sub[swp^1].u * typesize(typechild(ex->sub[swp].ty));
               ex->t = ENUMLIT;
            }
            return 1;
         }
   }
   return 0;
}

static bool
binop(struct expr *ex, enum evalmode mode)
{
   union type oty;
   bool flt;
   bool sgn;
   int t;
   struct expr *lhs = &ex->sub[0], *rhs = &ex->sub[1];

   if (ex->ty.t == TYPTR) mode = EVFOLD;
   if (!eval(lhs, mode)) return 0;
   if (!eval(rhs, mode)) return 0;
   if (in_range(ex->t, EADD, ESHR))
      oty = ex->ty;
   else
      oty = cvtarith(lhs->ty, rhs->ty);
   if (!numcast(oty, lhs, lhs)) return 0;
   if (!numcast(oty, rhs, rhs)) return 0;
   flt = isflt(oty);
   sgn = issigned(oty);
   switch (ex->t) {
#define ef else if
   case EADD: if (flt) lhs->f += rhs->f;
              else     lhs->u += rhs->u;
              break;
   case ESUB: if (oty.t == TYPTR) {
                 assert(lhs->t == ENUMLIT && rhs->t == ENUMLIT);
                 assert(!isincomplete(typechild(ex->ty)));
                 lhs->u = (lhs->u - rhs->u) / typesize(typechild(ex->ty));
              }
              ef (flt) lhs->f -= rhs->f;
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

   if (!in_range(ex->t, EADD, ESHR))
      lhs->ty = mktype(TYINT);
   return numcast(ex->ty, ex, lhs);
}

bool
eval(struct expr *ex, enum evalmode mode)
{
   if (ex->t == ENUMLIT) {
      if (mode <= EVINTCONST) return isint(ex->ty);
      return 1;
   }
   if (ex->t == ESTRLIT && mode > EVINTCONST) return 1;
   if (mode == EVSTATICINI && isptrcvt(ex->ty) && isaddrconst(ex)) {
      struct expr *e = ex;
      while (e->t == ECAST) e = e->sub;
      if (e != ex) {
         if (e->ty.t != TYARRAY)
            e->ty = ex->ty;
         *ex = *e;
      }
      return 1;
   }
   mode += mode == EVINTCONST;
   if (isunop(ex->t)) return unop(ex, mode) && eval(ex, mode);
   if (isbinop(ex->t)) return binop(ex, mode) && eval(ex, mode);
   if (ex->t == ESEQ) {
      if (!eval(&ex->sub[0], mode)) return 0;
      *ex = ex->sub[1];
      return eval(ex, mode);
   }
   if (ex->t == ECOND) {
      if (!eval(&ex->sub[0], mode) || ex->sub[0].t != ENUMLIT) return 0;
      if (!eval(&ex->sub[1], mode)) return 0;
      if (!eval(&ex->sub[2], mode)) return 0;
      *ex = ex->sub[!ex->sub[0].u + 1];
      return eval(ex, mode);
   }
   if (ex->t == EINIT) {
      for (struct initval *v = ex->init->vals; v; v = v->next) {
         if (!eval(&v->ex, mode)) return 0;
      }
      return 1;
   }
   return 0;
}

/* vim:set ts=3 sw=3 expandtab: */
