#include "c.h"
#include "c_lex.h"
#include "u_endian.h"
#include "u_bits.h"
#include "ir.h"
#include "obj.h"

/** Parsing helper functions **/
#define peek(Cm,Tk) lexpeek((Cm)->lx,Tk)
static int
lexc(CComp *cm, Token *tk)
{
   Token tk2, tk_[1];
   int t = lex(cm->lx, tk ? tk : tk_);
   if (t == TKSTRLIT && peek(cm, &tk2) == TKSTRLIT && tk2.wide == tk->wide) {
      /* 5.1.1.2 Translation phase 6: concatenate adjacent string literal tokens */
      static char buf[200];
      vec_of(char) rest = VINIT(buf, sizeof buf);
      do {
         lex(cm->lx, NULL);
         if (tk) {
            joinspan(&tk->span.ex, tk2.span.ex);
            if (!tk->wide)
               vpushn(&rest, tk2.s, tk2.len);
            else if (tk->wide && targ_primsizes[targ_wchartype] == 2)
               vpushn(&rest, tk2.ws16, tk2.len*2);
            else
               vpushn(&rest, tk2.ws32, tk2.len*4);
         }
      } while (peek(cm, &tk2) == TKSTRLIT && tk2.wide == tk->wide);
      if (tk) {
         if (!tk->wide) {
            tk->s = memcpy(alloc(&cm->exarena, tk->len + rest.n, 1), tk->s, tk->len);
            memcpy((char *)tk->s + tk->len, rest.p, rest.n);
            tk->len += rest.n;
         } else if (tk->wide == 1) {
            tk->ws16 = memcpy(alloc(&cm->exarena, tk->len + rest.n*2, 2), tk->ws16, tk->len*2);
            memcpy((short *)tk->s + tk->len, rest.p, rest.n);
            tk->len += rest.n * 2;
         } else {
            tk->ws32 = memcpy(alloc(&cm->exarena, tk->len + rest.n*4, 4), tk->ws32, tk->len*4);
            memcpy((int *)tk->s + tk->len, rest.p, rest.n);
            tk->len += rest.n * 4;
         }
      }
      vfree(&rest);
   }
   if (ccopt.pedant && in_range(t, TKWBEGIN_, TKWEND_) && (tk = tk ? tk : tk_)->extwarn) {
      static BitSet already[BSSIZE(TKWEND_-TKWBEGIN_+1)];
      if (!bstest(already, t-TKWBEGIN_)) {
         bsset(already, t-TKWBEGIN_);
         warn(&tk->span, "%'tk in %M is an extension", tk);
      }
   }
   return t;
}
#define lex(Cm,Tk) lexc(Cm,Tk)
static bool
match(CComp *cm, Token *tk, enum toktag t)
{
   if (peek(cm, NULL) == t) {
      lex(cm, tk);
      return 1;
   }
   return 0;
}
static bool
expect(CComp *cm, enum toktag t, const char *s)
{
   Token tk;
   if (!match(cm, &tk, t)) {
      peek(cm, &tk);
      if (aisprint(t)) tk.span.ex.len = tk.span.sl.len = 1;
      error(&tk.span, "expected %'tt%s%s", t, s?" ":"",s ? s : "");
      return 0;
   }
   return 1;
}

/******************************************/
/* Data structures for declaration parser */
/******************************************/

enum declkind {
   DTOPLEVEL,
   DFUNCPARAM,
   DFUNCVAR,
   DFIELD,
   DCASTEXPR,
};

/* Since a declaration can have multiple declarators, and we need to process
 * each one individually, the declaration parser is a state machine
 * (conceptually a generator coroutine); the state is zero-initialized (except
 * for the .kind field), each call to pdecl yields the next individual decl,
 * st.more indicates whether there are more decls left to parse (the coroutine
 * has yielded), or this declaration list is done (the coroutine has finalized)
 */
typedef struct DeclState {
   enum declkind kind;
   Type base;
   uchar scls;
   uchar qual;
   bool fnnoreturn : 1,
        fninline : 1;
   uint align;
   bool base0, /* caller set initial base type, but there may be declspecs to parse */
        more, /* caller should keep calling pdecl to get next decl */
        varini, /* caller should parse an initializer ('=' <ini>) and
                   call pdecl() to advance state before checking .more */
        funcdef, /* caller should parse an func definition ('{' <body> '}').
                    the declaration list is finished. */
        bitf, /* caller should parse a bitfield size and
                 call pdecl() to advance state before checking .more */
        tagdecl, /* declarator is a tagged type */
        empty; /* nothing decl (';') */
   internstr *pnames; /* param names for function definition */
   Span *pspans; /* param spans ditto */
   uchar *pqual; /* param quals ditto */
   int attr;
} DeclState;
static Decl pdecl(DeclState *st, CComp *cm);

static Decl *finddecl(CComp *cm, internstr name);

/* next token starts a decl? */
static bool
isdecltok(CComp *cm)
{
   Token tk;
   if (peek(cm, &tk) == TKIDENT) {
      Decl *decl = finddecl(cm, tk.name);
      return decl && decl->scls == SCTYPEDEF;
   } else {
      static const bool kws[] = {
#define kw(x) [TKW##x-TKWBEGIN_] = 1
         kw(auto), kw(extern), kw(static), kw(register), kw(typedef),
         kw(_Thread_local), kw(thread_local), kw(_Static_assert),
         kw(inline), kw(_Noreturn),
         kw(const), kw(volatile), kw(restrict), kw(_Atomic),
         kw(void), kw(float), kw(double), kw(_Complex),
         kw(signed), kw(unsigned), kw(short), kw(long),
         kw(int), kw(char), kw(_Bool), kw(bool),
         kw(struct), kw(union), kw(enum),
         kw(__typeof__), kw(typeof), kw(typeof_unqual),
         kw(__attribute__)
#undef kw
      };
      return ((uint)tk.t-TKWBEGIN_) < countof(kws) && kws[tk.t-TKWBEGIN_];
   }
}

/* next token starts an expr? */
static bool
isexprtok(CComp *cm)
{
   Token tk;
   if (peek(cm, &tk) == TKIDENT) {
      Decl *decl = finddecl(cm, tk.name);
      return !decl || decl->scls != SCTYPEDEF;
   } else {
      static const bool tks[] = {
#define tk(x) [x] = 1
         tk('+'), tk('-'), tk('*'), tk('&'), tk('~'), tk('!'), tk(TKINC), tk(TKDEC),
         tk(TKWsizeof), tk(TKW_Alignof), tk(TKWalignof), tk(TKWtrue), tk(TKWfalse),
         tk('('), tk(TKNUMLIT), tk(TKCHRLIT), tk(TKSTRLIT), tk(TKW_Generic)
#undef tk
      };
      return tk.t < countof(tks) && tks[tk.t];
   }
}

/**********************************/
/* Environment (scope) management */
/**********************************/

struct declsbuf declsbuf;
typedef struct { /* a tagged type declaration */
   Type ty;
   Span span;
} Tagged;
static Tagged envtaggedbuf[1<<7];
static vec_of(Tagged) envtagged = VINIT(envtaggedbuf, countof(envtaggedbuf));
struct Env {
   Env *up;
   /* list of decls is implicitly envdecls[decl..ndecl] */
   ushort decl, ndecl;
   /* ditto for envtagged[] */
   ushort tagged, ntagged;
};
/* use a hashmap for lookups of top-level declarations, since there's usually many of those */
static pmap_of(ushort) tldeclmap;

static void
envdown(CComp *cm, Env *e)
{
   assert(cm->env->decl + cm->env->ndecl == declsbuf.n);
   assert(cm->env->tagged + cm->env->ntagged == envtagged.n);
   e->decl = declsbuf.n;
   e->tagged = envtagged.n;
   e->ndecl = e->ntagged = 0;
   e->up = cm->env;
   cm->env = e;
}

static void
envup(CComp *cm)
{
   Env *env = cm->env;
   assert(env->decl + env->ndecl == declsbuf.n);
   declsbuf.n -= env->ndecl;
   envtagged.n -= env->ntagged;
   assert(env->up);
   cm->env = env->up;
}

int
envadddecl(Env *env, const Decl *d)
{
   assert(env->decl + env->ndecl == declsbuf.n);
   vpush(&declsbuf, *d);
   assert(declsbuf.n < 1<<16);
   ++env->ndecl;
   if (!env->up) pmap_set(&tldeclmap, d->name, declsbuf.n-1);
   return declsbuf.n - 1;
}

/* iters in reversed order of insertion (most to least recent) */
/* use like so: for (d = NULL; enviterdecl(&d, env);) ... */
static inline bool
enviterdecl(Decl **d, Env *env)
{
   if (!env->ndecl) return 0;
   if (!*d) *d = &declsbuf.p[env->decl + env->ndecl - 1];
   else if (*d == &declsbuf.p[env->decl]) return 0;
   else --*d;
   return 1;
}

static Tagged *
envaddtagged(Env *env, Type ty, const Span *span)
{
   Tagged tagged = { ty, *span };
   assert(env->tagged + env->ntagged == envtagged.n);
   vpush(&envtagged, tagged);
   assert(envtagged.n < 1<<16);
   ++env->ntagged;
   return &envtagged.p[envtagged.n - 1];
}

/* like enviterdecl */
static inline bool
envitertagged(Tagged **l, Env *env)
{
   if (!env->ntagged) return 0;
   if (!*l) *l = &envtagged.p[env->tagged + env->ntagged - 1];
   else if (*l == &envtagged.p[env->tagged]) return 0;
   else --*l;
   return 1;
}

static bool
redeclarationok(const Decl *old, const Decl *new)
{
   bool takeoldscls = 0;
   if (old->scls != new->scls) {
      if (old->scls == SCSTATIC && (new->scls &~ SCEXTERN) == SCNONE && old->ty.t == TYFUNC && new->ty.t == TYFUNC)
         takeoldscls = 1;
      else
         return 0;
   }
   switch (old->scls) {
   case SCSTATIC:
   case SCEXTERN:
      if (old->ty.bits == new->ty.bits) goto OkFuncs;
      if (old->ty.t != new->ty.t) return 0;
      if (old->ty.t == TYARRAY /* allow 'int x[]; int x[100];' */
       && typechild(old->ty).bits == typechild(new->ty).bits
       && (isincomplete(old->ty) || isincomplete(new->ty)))
      {
         return 1;
      }
      if (old->ty.t == TYFUNC /* allow 'int f(); int f(int);' (some K&R) */
       && typedata[old->ty.dat].ret.bits == typedata[new->ty.dat].ret.bits
       && (typedata[old->ty.dat].kandr || typedata[new->ty.dat].kandr))
      { OkFuncs:
         if (takeoldscls) ((Decl *)new)->scls = old->scls;
         return 1;
      }
      return 0;
   case SCTYPEDEF:
      return old->ty.bits == new->ty.bits;
   }
   return 0;
}

static int
putdecl(CComp *cm, const Decl *decl)
{
   for (Env *env = cm->env; env; env = env->up) {
      Decl *l;
      if (!env->up) {
         ushort *pi = pmap_get(&tldeclmap, decl->name);
         if (pi) {
            l = &declsbuf.p[*pi];
            goto Match;
         }
      } else for (l = NULL; enviterdecl(&l, env);) {
         if (decl->name == l->name) {
         Match:
            if ((cm->env->up != NULL && decl->scls == SCSTATIC) || (l->isdef && decl->isdef)) {
               error(&decl->span, "redefinition of '%s'", decl->name);
               note(DGERROR, &l->span, "previously defined here");
               break;
            } else if (!redeclarationok(l, decl)) {
               error(&decl->span, "incompatible redeclaration of '%s'", decl->name);
               note(DGERROR, &l->span, "previously declared here");
               break;
            }
            if (l->isdef && !decl->isdef) return l - declsbuf.p;
            break;
         }
      }
      if (decl->scls != SCEXTERN) break;
   }
   return envadddecl(cm->env, decl);
}

static Decl *
finddecl(CComp *cm, internstr name)
{
   assert(name);
   for (Env *e = cm->env; e; e = e->up) {
      if (!e->up) {
         ushort *pi = pmap_get(&tldeclmap, name);
         if (pi) return &declsbuf.p[*pi];
      } else for (Decl *l = NULL; enviterdecl(&l, e);) {
         if (name == l->name)
            return l;
      }
   }
   return NULL;
}

static Type
gettagged(CComp *cm, Span *span, enum typetag tt, internstr name, bool dodef)
{
   TypeData td = {0};
   assert(name);
   for (Env *e = cm->env; e; e = e->up) {
      for (Tagged *l = NULL; envitertagged(&l, e);) {
         if (name == tagtypetags[typedata[l->ty.dat].id]) {
            if (dodef && e != cm->env)
               goto Break2;
            *span = l->span;
            return l->ty;
         }
      }
   }
   if (tt == TYENUM && ccopt.pedant) {
      warn(span, "forward-declared enum is an extension");
   }
Break2:
   td.t = tt;
   return envaddtagged(cm->env, mktagtype(name, &td), span)->ty;
}

static Type
deftagged(CComp *cm, Span *span, enum typetag tt, internstr name, Type ty)
{
   TypeData td = {0};
   assert(name);
   for (Tagged *l = NULL; envitertagged(&l, cm->env);) {
      if (name == tagtypetags[typedata[l->ty.dat].id]) {
         *span = l->span;
         return l->ty;
      }
   }
   td.t = tt;
   return envaddtagged(cm->env, ty.t ? ty : mktagtype(name, &td), span)->ty;
}

/*********************/
/* Expr Typechecking */
/*********************/

#define iszero(ex) ((ex).t == ENUMLIT && isint((ex).ty) && (ex).u == 0)

static bool
islvalue(const Expr *ex)
{
   if (ex->t == EGETF) return islvalue(ex->sub);
   return ex->t == ESYM || ex->t == EDEREF || ex->t == EINIT || ex->t == ESTRLIT;
}

static Type /* 6.5.2.6 default argument promotions */
argpromote(Type t)
{
   if (isint(t)) t.t = intpromote(t.t);
   else if (t.t == TYFLOAT) t.t = TYDOUBLE;
   else if (t.t == TYARRAY) return mkptrtype(typechild(t), t.flag & TFCHLDQUAL);
   else if (t.t == TYFUNC) return mkptrtype(t, 0);
   return t;
}

bool
assigncheck(Type t, const Expr *src)
{
   Type srcty = typedecay(src->ty);;
   if (assigncompat(t, srcty)) {
      if (t.t == TYPTR && srcty.t == TYPTR
       && (t.flag & TFCHLDQUAL & srcty.flag & TFCHLDQUAL) != (srcty.flag & TFCHLDQUAL)) {
         warn(&src->span, "usage of '%ty' discards pointer qualifiers", src->ty);
      }
      return 1;
   } else if (t.t == TYPTR && srcty.t == TYPTR) {
      warn(&src->span, "converting between incompatible pointer types ('%ty' -> '%ty')", srcty, t);
      return 1;
   } else if (t.t == TYPTR && iszero(*src)) return 1;
   return 0;
}

static bool
initcheck(Type t, const Expr *src)
{
   if (assigncheck(t, src)) return 1;
   if (t.bits == src->ty.bits && (src->t == EINIT || src->t == ESTRLIT)) return 1;
   return 0;
}

static void
incdeccheck(enum toktag tt, const Expr *ex, const Span *span)
{
   if (!isscalar(ex->ty))
      error(&ex->span, "invalid operand to %tt '%ty'", tt, ex->ty);
   else if (!islvalue(ex))
      error(&ex->span, "operand to %tt is not an lvalue", tt);
   else if (ex->ty.t == TYPTR && isincomplete(typechild(ex->ty)))
      error(span, "arithmetic on pointer to incomplete type '%ty'", ex->ty);
   else if (ex->ty.t == TYPTR && typechild(ex->ty).t == TYFUNC)
      error(span, "arithmetic on function pointer '%ty'", ex->ty);
}

static bool /* 6.5.4 Cast operators */
castcheck(Type to, const Expr *ex)
{
   Type src = ex->ty;
   if (to.t == TYVOID) return 1;
   if (isagg(to)) return 0;
   if (to.bits == src.bits) return 1;
   if (isarith(to) && isarith(src)) return 1;
   if (isint(to) && isptrcvt(src)) return 1;
   if (to.t == TYPTR && isint(src)) return 1;
   if (to.t == TYPTR && isptrcvt(src)) return 1;
   return 0;
}

static Type /* 6.5.2.1 Array subscripting */
subscriptcheck(const Expr *ex, const Expr *rhs, const Span *span)
{
   Type ty;
   if (ex->ty.t == TYPTR || ex->ty.t == TYARRAY) {
      if (isincomplete(typedecay(ty = typechild(ex->ty)))) {
         error(span, "cannot dereference pointer to incomplete type '%ty'", ty);
         ty = mktype(TYINT);
      } else if (ty.t == TYFUNC) {
         error(span, "subscripted value is pointer to function");
         ty = mktype(TYINT);
      }
   } else {
      error(&ex->span, "subscripted value is not pointer-convertible '%ty'", ex->ty);
      ty = mktype(TYINT);
   }
   if (!isint(rhs->ty))
      error(&rhs->span, "array subscript is not integer ('%ty')", rhs->ty);
   return ty;
}

static uint /* 6.5.3.4 The sizeof and _Alignof operators */
sizeofalignofcheck(const Span *span, enum toktag tt, Type ty, const Expr *ex)
{
   uint r = (tt == TKWsizeof ? typesize : typealign)(ty);
   if (ty.t == TYVOID) {
      if (ccopt.pedant) warn(span, "applying %'tt to void type", tt);
      r = 1;
   } else if (isincomplete(ty)) {
      error(span, "cannot apply %'tt to incomplete type '%ty'", tt, ty);
   } else if (ty.t == TYFUNC) {
      error(span, "cannot apply %'tt to function type '%ty'", tt, ty);
   } else if (tt == TKWsizeof && ex && ex->t == EGETF && ex->fld.bitsiz) {
      error(span, "cannot apply %'tt to bitfield", tt);
   }
   if (tt != TKWsizeof && ex && ccopt.pedant)
      warn(span, "%'tt applied to an expression is a GNU extension", tt);
   return r;
}

static bool /* 6.5.8 Relational operators */
relationalcheck(const Expr *a, const Expr *b)
{
   Type t1 = a->ty, t2 = b->ty;
   if (isarith(t1) && isarith(t2)) return 1;
   if (isptrcvt(t1) && isptrcvt(t2)) {
      t1 = typedecay(t1);
      t2 = typedecay(t2);
      return t1.dat == t2.dat;
   }
   return 0;
}

static bool
isnullpo(const Expr *ex) /* match '0' or '(void *) 0' */
{
   static const Type voidptr = {{ TYPTR, .flag = TFCHLDPRIM, .child = TYVOID }};
   while (ex->t == ECAST && ex->ty.bits == voidptr.bits)
      ex = ex->sub;
   if (iszero(*ex)) return 1;
   return eval((Expr *)ex, EVINTCONST) /* GNU extension. should we warn? */
       && iszero(*ex);
}

static bool /* 6.5.9 Equality operators */
equalitycheck(const Expr *a, const Expr *b)
{
   Type t1 = a->ty, t2 = b->ty;
   if (isarith(t1) && isarith(t2)) return 1;
   if (isptrcvt(t1) && isptrcvt(t2)) {
      t1 = typedecay(t1), t2 = typedecay(t2);
      /* comparing .dat works for both TFCHLDPRIM and not, (checks equal child types)
       * quals are ignored either way */
      return t1.dat == t2.dat || typechild(t1).t == TYVOID || typechild(t2).t == TYVOID;
   }
   return (isptrcvt(t1) && isnullpo(b)) || (isptrcvt(t2) && isnullpo(a));
}

static Type /* 6.5.15 Conditional operator */
condtype(const Expr *a, const Expr *b)
{
   Type t1 = typedecay(a->ty), t2 = typedecay(b->ty), s1, s2;
   if (isarith(t1) && isarith(t2)) return cvtarith(t1, t2);
   if (t1.bits == t2.bits) return t1;
   if (t1.t == TYPTR && isnullpo(b)) return t1;
   if (isnullpo(a) && t2.t == TYPTR) return t2;
   if (t1.t == TYPTR && t2.t == TYPTR) {
      s1 = typechild(t1), s2 = typechild(t2);
      if (s1.bits == s2.bits || s2.t == TYVOID || s1.t == TYVOID) {
         return mkptrtype(s1.t == TYVOID ? s1 : s2, (t1.flag | t2.flag) & TFCHLDQUAL);
      }
   }
   return mktype(0);
}

static void
bintypeerr(const Span *span, enum toktag tt, Type lhs, Type rhs)
{
   error(span, "bad operands to %tt ('%ty', '%ty')", tt, lhs, rhs);
}

enum binopclass { /* binary operator type-checking classes */
   BCSET = 1<<7, /* is a (compound) assignment operator? */
   BCSEQ = 1, BCADDITIVE, BCARITH, BCINT, BCSHFT, BCEQL, BCCMP, BCLOG,
};

/* table indexed by binary op token;
 *    containing precedence level, expression kind and type-checking class */
static const struct { uchar prec, t, k; } bintab[] = {
   ['*'] = {13, EMUL, BCARITH},
   ['/'] = {13, EDIV, BCARITH},
   ['%'] = {13, EREM, BCINT},
   ['+'] = {12, EADD, BCADDITIVE},
   ['-'] = {12, ESUB, BCADDITIVE},
   [TKSHL] = {11, ESHL, BCSHFT},
   [TKSHR] = {11, ESHR, BCSHFT},
   ['<']   = {10, ELTH, BCCMP},
   ['>']   = {10, EGTH, BCCMP},
   [TKLTE] = {10, ELTE, BCCMP},
   [TKGTE] = {10, EGTE, BCCMP},
   [TKEQU] = {9, EEQU, BCEQL},
   [TKNEQ] = {9, ENEQ, BCEQL},
   ['&'] = {8, EBAND, BCINT},
   ['^'] = {7, EXOR,  BCINT},
   ['|'] = {6, EBIOR, BCINT},
   [TKLOGAND] = {5, ELOGAND, BCLOG},
   [TKLOGIOR] = {4, ELOGIOR, BCLOG},
   ['?'] = {3, ECOND}, /* not actually a binop (special cased) */
   ['='] = {2, ESET, BCSET},
   [TKSETADD] = {2, ESETADD, BCSET|BCADDITIVE}, [TKSETSUB] = {2, ESETSUB, BCSET|BCADDITIVE},
   [TKSETMUL] = {2, ESETMUL, BCSET|BCARITH},    [TKSETDIV] = {2, ESETDIV, BCSET|BCARITH},
   [TKSETREM] = {2, ESETREM, BCSET|BCINT},      [TKSETAND] = {2, ESETAND, BCSET|BCINT},
   [TKSETIOR] = {2, ESETIOR, BCSET|BCINT},      [TKSETXOR] = {2, ESETXOR, BCSET|BCINT},
   [TKSETSHL] = {2, ESETSHL, BCSET|BCSHFT},     [TKSETSHR] = {2, ESETSHR, BCSET|BCSHFT},
   [','] = {1, ESEQ, BCSEQ}
};

static Type
bintypecheck(const Span *span, enum toktag tt, Expr *lhs, Expr *rhs)
{
   enum binopclass k = bintab[tt].k;
   Type ty = lhs->ty;

   assert(k);
   if (k & BCSET) {
      if (!islvalue(lhs))
         error(&lhs->span, "left-hand-side of assignment is not an lvalue");
      else if (lhs->qual & QCONST)
         error(&lhs->span, "cannot assign to const-qualified lvalue (%tq)", ty, lhs->qual);
      else if (isincomplete(ty))
         error(&lhs->span, "cannot assign to incomplete type '%ty'", ty);
      else if (ty.t == TYARRAY)
         error(&lhs->span, "cannot assign to array type '%ty'", ty);
      else if (ty.t == TYFUNC)
         error(&lhs->span, "cannot assign to function designator '%ty'", lhs->ty);
   }
   switch (k &~ BCSET) {
   case 0:
      if (isagg(ty) && !(lhs->qual & QCONST) && typedata[ty.dat].anyconst)
         error(&lhs->span, "cannot assign to aggregate with const-qualified member");
      if (!assigncheck(ty, rhs))
         goto Error;
      break;
   case BCSEQ:
      ty = rhs->ty;
      break;
   case BCADDITIVE:
      if (tt == '+' && isptrcvt(rhs->ty)) {
         /* int + ptr -> ptr + int (for convenience) */
         const Expr swaptmp = *lhs;
         *lhs = *rhs;
         *rhs = swaptmp;
         ty = lhs->ty;
      }
      if (isarith(ty) && isarith(rhs->ty)) {
         /* num +/- num */
         ty = cvtarith(ty, rhs->ty);
         assert(ty.t);
      } else if ((ty.t == TYPTR || ty.t == TYARRAY) && isint(rhs->ty)) {
         /* ptr +/- int */
         Type pointee = typechild(ty);
         if (isincomplete(pointee))
            error(span, "arithmetic on pointer to incomplete type '%ty'", ty);
         else if (pointee.t == TYFUNC)
            error(span, "arithmetic on function pointer '%ty'", ty);
         ty = typedecay(ty);
      } else if (tt == '-' && isptrcvt(ty) && isptrcvt(rhs->ty)) {
         /* ptr - ptr */
         Type pointee1 = typechild(typedecay(ty)),
                    pointee2 = typechild(typedecay(rhs->ty));
         if (isincomplete(pointee1))
            error(span, "arithmetic on pointer to incomplete type '%ty'", ty);
         else if (pointee1.t == TYFUNC)
            error(span, "arithmetic on function pointer '%ty'", lhs->ty);
         else if (pointee1.bits != pointee2.bits) {
            error(span, "arithmetic on incompatible pointer types: '%ty', '%ty'",
                  ty, rhs->ty);
         }
         ty = mktype(targ_ptrdifftype);
      } else goto Error;
      break;
   case BCARITH:
      ty = cvtarith(ty, rhs->ty);
      if (!ty.t) {
         ty.t = TYINT;
      Error:
         bintypeerr(span, tt, lhs->ty, rhs->ty);
      }
      break;
   case BCINT:
      if (!isint(ty) || !isint(rhs->ty))
         goto Error;
      ty = cvtarith(ty, rhs->ty);
      assert(ty.t);
      break;
   case BCSHFT: /* 6.5.7 Bitwise shift operators */
      if (!isint(ty) || !isint(rhs->ty)) {
         ty = mktype(TYINT);
         goto Error;
      }
      ty.t = intpromote(ty.t);
      assert(ty.t);
      break;
   case BCEQL:
      ty = mktype(TYINT);
      if (!equalitycheck(lhs, rhs)) {
         if (isptrcvt(lhs->ty) && isptrcvt(rhs->ty))
            warn(span, "comparison of distinct pointer types ('%ty' and '%ty')", lhs->ty, rhs->ty);
         else
            goto Error;
      }
      break;
   case BCCMP:
      ty = mktype(TYINT);
      if (!relationalcheck(lhs, rhs)) {
         if (isptrcvt(lhs->ty) && isptrcvt(rhs->ty))
            warn(span, "comparison of distinct pointer types ('%ty' and '%ty')", lhs->ty, rhs->ty);
         else
            goto Error;
      }
      break;
   case BCLOG: /* 6.5.13-14 Logical AND/OR operator */
      ty = mktype(TYINT);
      if (!isscalar(typedecay(ty)) || !isscalar(typedecay(rhs->ty)))
         goto Error;
      break;
   }
   return (k & BCSET) || !ty.t ? lhs->ty : ty;
}

/****************/
/* Expr Parsing */
/****************/

#define mkexpr(t_,span_,ty_,...) ((Expr){.t=(t_), .ty=(ty_), .span=(span_), __VA_ARGS__})

static Expr *
exprdup(CComp *cm, const Expr *e)
{
   return alloccopy(&cm->exarena, e, sizeof *e, 0);
}
static Expr *
exprdup2(CComp *cm, const Expr *e1, const Expr *e2)
{
   Expr *r = alloc(&cm->exarena, 2*sizeof *r, 0);
   r[0] = *e1, r[1] = *e2;
   return r;
}

static Expr expr(CComp *cm);
static Expr commaexpr(CComp *cm);

enum { IMPLICITSYMTY = 0xFF, };

static Expr /* 6.5.2.2 Function calls */
callexpr(CComp *cm, const Span *span_, const Expr *callee)
{
   Token tk;
   Expr ex, arg;
   Span span = callee->span;
   Type ty = callee->ty;
   const TypeData *td = NULL;
   Expr argbuf[10];
   vec_of(Expr) args = VINIT(argbuf, countof(argbuf));
   bool spanok = joinspan(&span.ex, span_->ex);
   bool printsig = 0;
   const Builtin *builtin = NULL;

   if (callee->t == ESYM && !callee->ty.t && declsbuf.p[callee->decl].isbuiltin) {
      builtin = declsbuf.p[callee->decl].builtin;
      assert(!ty.t);
   }

   if (callee->t == ESYM && ty.t == IMPLICITSYMTY) { /* implicit function decl.. */
      internstr name = callee->implicitsym;
      Decl decl = {
         (ty = mkfntype(mktype(TYINT), 0, NULL, /* kandr */ 1, 0)),
         .scls = SCEXTERN, .span = span, .name = name, .sym = name
      };
      warn(&span, "call to undeclared function '%s'", name);
      ((Expr *)callee)->ty = decl.ty;
      ((Expr *)callee)->decl = putdecl(cm, &decl);
   }

   if (!builtin) {
      if (ty.t == TYPTR) /* auto-deref when calling a function pointer */
         ty = typechild(ty);
      if (ty.t != TYFUNC)
         error(&span, "calling a value of type '%ty'", callee->ty);
      else
         td = &typedata[ty.dat];
      if (ty.t == TYFUNC && td->ret.t != TYVOID && isincomplete(td->ret))
         error(&span, "cannot call function with incomplete return type '%ty'", td->ret);
   }

   if (!match(cm, &tk, ')')) for (;;) {
      arg = expr(cm);
      spanok = spanok && joinspan(&span.ex, callee->span.ex);
      if (td && args.n == td->nmemb && !td->variadic && !td->kandr) {
         error(&arg.span, "too many args to function taking %d params", td->nmemb);
         printsig = 1;
      }
      if (arg.ty.t == TYVOID) {
         error(&arg.span, "invalid use of void expression");
      } else if (td && args.n < td->nmemb && !td->kandr) {
         if (!assigncheck(td->param[args.n], &arg)) {
            error(&arg.span, "arg #%d of type '%ty' is incompatible with '%ty'",
                  args.n+1, arg.ty, td->param[args.n]);
            printsig = 1;
         }
      }
      vpush(&args, arg);
      peek(cm, &tk);
      if (match(cm, &tk, ',')) {
         spanok = spanok && joinspan(&span.ex, tk.span.ex);
      } else if (expect(cm, ')', "or ',' after arg")) {
         break;
      }
   }
   if (!spanok || !joinspan(&span.ex, tk.span.ex)) span = *span_;

   if (td && !td->variadic && !td->kandr && args.n < td->nmemb) {
      error(&tk.span, "not enough args to function taking %d param%s",
            td->nmemb, td->nmemb != 1 ? "s" : "");
      printsig = 1;
   }
   if (printsig) note(DGERROR, &callee->span, "function signature is '%ty'", ty);

   ex = mkexpr(ECALL, span, ty.t == TYFUNC ? td->ret : ty, .narg = args.n,
               .sub = alloc(&cm->exarena, (args.n+1)*sizeof(Expr), 0));
   ex.sub[0] = *callee;
   memcpy(ex.sub+1, args.p, args.n*sizeof(Expr));
   vfree(&args);
   if (builtin) {
      builtin->sema(cm, &ex);
   }
   return ex;
}

static void
ppostfixopers(CComp *cm, Expr *ex)
{
   Expr tmp, rhs;
   Token tk, tk2;
   Span span;
   Type ty;

   for (;;) switch (peek(cm, &tk)) {
   default: return;
   case TKINC:
   case TKDEC:
      lex(cm, &tk);
      span = ex->span;
      if (!joinspan(&span.ex, tk.span.ex)) span = tk.span;
      incdeccheck(tk.t, ex, &span);
      *ex = mkexpr(tk.t == TKINC ? EPOSTINC : EPOSTDEC, span, ex->ty, .sub = exprdup(cm, ex));
      continue;
   case '[': /* a[subscript] */
      lex(cm, NULL);
      rhs = commaexpr(cm);
      span = ex->span;
      if (!joinspan(&span.ex, tk.span.ex) || !joinspan(&span.ex, ex->span.ex)
       || (peek(cm, &tk), !joinspan(&span.ex, tk.span.ex)))
         span = tk.span;
      expect(cm, ']', NULL);

      if (isint(ex->ty) && isptrcvt(rhs.ty)) {
         /* swap idx[ptr] -> ptr[idx] */
         tmp = *ex;
         *ex = rhs;
         rhs = tmp;
      }

      ty = subscriptcheck(ex, &rhs, &span);
      assert(ty.t);
      if (!iszero(rhs)) {
         tmp.sub = exprdup2(cm, ex, &rhs);
         tmp.t = EADD;
         tmp.span = span;
         tmp.ty = typedecay(ex->ty);
      }
      tmp.sub = exprdup(cm, iszero(rhs) ? ex : &tmp);
      tmp.span = span;
      tmp.t = EDEREF;
      tmp.qual = ex->ty.flag & TFCHLDQUAL;
      tmp.ty = ty;
      *ex = tmp;
      continue;
   case '(': /* call(args) */
      lex(cm, &tk);
      span = ex->span;
      *ex = callexpr(cm, &span, ex);
      continue;
   case TKARROW:
      if (ex->ty.t != TYPTR && ex->ty.t != TYARRAY)
         error(&ex->span, "operand to -> is not a pointer: '%ty'", ex->ty);
      else
         *ex = mkexpr(EDEREF, ex->span, typechild(ex->ty), .qual = ex->ty.flag & TFCHLDQUAL,
                     .sub = exprdup(cm, ex));
      /* fallthru */
   case '.':
      lex(cm, &tk);
      span = ex->span;
      peek(cm, &tk2); /* field name */
      if (!expect(cm, TKIDENT, NULL)) tk2.s = "";
      if (!joinspan(&span.ex, tk.span.ex) || !joinspan(&span.ex, tk2.span.ex))
         span = tk.span;
      if (!isagg(ex->ty)) {
         error(&span, "member access operand is not an aggregate: '%ty'%s", ex->ty,
               ex->ty.t == TYPTR && isagg(typechild(ex->ty)) ? "; did you mean to use '->'?" : "");
      } else {
         FieldData fld = {.t = mktype(TYINT)};
         if (*tk2.s && !getfield(&fld, ex->ty, tk2.name))
            error(&span, "'%ty' has no such field: '%s'", ex->ty, tk2.name);
         if (ex->t == EGETF && ex->qual == fld.qual) { /* accumulate */
            ex->span = span;
            ex->ty = fld.t;
            ex->fld.off += fld.off;
            ex->fld.bitoff = fld.bitoff;
            ex->fld.bitsiz = fld.bitsiz;
         } else {
            *ex = mkexpr(EGETF, span, fld.t, .qual = ex->qual | fld.qual, .sub = exprdup(cm, ex),
                        .fld = { fld.off, fld.bitsiz, fld.bitoff });
         }
      }
      continue;
   }
}

static Expr
vaargexpr(CComp *cm, Span *span)
{
   Token tk;
   Expr ex = mkexpr(EXXX, *span, mktype(TYVOID), );
   if (expect(cm, '(', "after __builtin_va_arg")) {
      Expr arg = expr(cm);
      Decl decl;
      Type ty;
      expect(cm, ',', NULL);
      decl = pdecl(&(DeclState){DCASTEXPR}, cm);
      ty = decl.ty;
      peek(cm, &tk);
      if (expect(cm, ')', NULL))
         joinspan(&span->ex, tk.span.ex);
      if (ty.t == TYARRAY)
         warn(&decl.span, "va_arg type argument is array type '%ty', which is undefined behavior", decl.ty);
      else if (ty.t == TYFUNC)
         error(&decl.span, "va_arg type argument is function type '%ty'", decl.ty);
      else {
         ty = argpromote(ty);
         if (ty.bits != decl.ty.bits) {
            warn(&decl.span,
                  "va_arg type argument is promotable type '%ty', which has undefined behavior"
                  " (it will be promoted to '%ty')", decl.ty, ty);
         }
      }
      ex = mkexpr(EVAARG, *span, decl.ty, .sub = exprdup(cm, &arg));
   }
   return ex;
}

static Expr
genericexpr(CComp *cm, Span *span)
{
   Token tk;
   if (expect(cm, '(', "after _Generic")) {
      Expr control = expr(cm), dfault = {0}, ex = {0};
      expect(cm, ',', NULL);
      for (;;) {
         if (match(cm, &tk, TKWdefault)) {
            expect(cm, ':', NULL);
            if (dfault.t) {
               error(&tk.span, "duplicate 'default' specifier in generic selection expression");
               (void)expr(cm);
            } else {
               dfault = expr(cm);
            }
         } else {
            Decl decl = pdecl(&(DeclState){DCASTEXPR}, cm);
            Type ty = decl.ty;
            expect(cm, ':', NULL);
            if (!ex.t &&
                (typedecay(ty).bits == typedecay(control.ty).bits
               || ((ty.t == TYENUM || control.ty.t == TYENUM) && scalartypet(ty) == scalartypet(control.ty))))
                  ex = expr(cm);
            else
               (void)expr(cm);
         }
         if (match(cm, &tk, ')')) break;
         else if (!expect(cm, ',', "or `)'")) {
            if (!isdecltok(cm)) {
               peek(cm, &tk); /* want the span */
               break;
            }
         }
      }
      if (!ex.t) ex = dfault;
      if (!ex.t) {
         error(&control.span,
               "controlling type '%ty' not compatible with any generic association type",
               control.ty);
         ex.ty.t = TYINT;
      }
      ex.span = *span;
      joinspan(&ex.span.ex, tk.span.ex);
      return ex;
   }
   return mkexpr(ENUMLIT,*span,mktype(TYINT),);
}

static inline int
tkprec(int tt)
{
   return ((uint)tt < countof(bintab)) ?  bintab[tt].prec : 0;
}

static Expr initializer(CComp *cm, Type *ty, enum evalmode ev,
                               bool globl, enum qualifier qual, internstr name);

static internstr istr__func__, istr_main, istr_memset;

static internstr mkhiddensym(const char *fnname, const char *name, int id);

/* parse an expression with the given operator precedence */
/* param ident is a kludge to support block labels without backtracking or extra lookahead
 * see stmt() */
enum exprctx { EFROMSTMT = 1, EARRAYCOUNT, EATTRARG };
static Expr
exprparse(CComp *cm, int prec, const Token *ident, enum exprctx ctx)
{
   Token tk;
   Span span;
   Expr ex;
   Type ty;
   struct {
      Span span;
      union {
         Type ty; /* cast type */
         struct {
            uchar t0; /* t == 0 */
            short tt; /* token */
         };
      };
   } unops[4];
   int nunop = 0;

   if (ident) {
      assert(ident->t == TKIDENT);
      tk = *ident;
      ident = NULL;
      goto Ident;
   }

Unary:
   switch (lex(cm, &tk)) {
   /* unary operators (gather) */
   case '*':
      if (ctx == EARRAYCOUNT && peek(cm, NULL) == ']') {
         /* kludge for C99 `int x[*]` (unk VLA size) */
         return (Expr) { 0, .span = tk.span };
      }
      /* fallthru */
   case '+': case '-': case '~': case '!':
   case '&': case TKINC: case TKDEC:
   Unops:
      unops[nunop].span = tk.span;
      unops[nunop].t0 = 0;
      unops[nunop].tt = tk.t;
      if (++nunop >= countof(unops)) {
         ex = exprparse(cm, 999, NULL, 0);
         break;
      }
      goto Unary;

   /* might be unary op (cast) or primary expr */
   case '(':
      if (!isdecltok(cm)) { /* (expr) */
         Span span = tk.span;
         ex = commaexpr(cm);
         joinspan(&span.ex, ex.span.ex);
         peek(cm, &tk);
         if (expect(cm, ')', NULL)) joinspan(&span.ex, tk.span.ex);
         ex.span = span;
      } else { /* (type) expr */
         DeclState st = { DCASTEXPR };
         Decl decl = pdecl(&st, cm);
         Span span = tk.span;
         assert(decl.ty.t);
         peek(cm, &tk);
         if (expect(cm, ')', NULL))
            joinspan(&span.ex, tk.span.ex);
         if (peek(cm, NULL) == '{') {
            if (ccopt.cstd < STDC99)
               warn(&tk.span, "compound literals are a c99 feature");
            ex = initializer(cm, &decl.ty, (decl.scls & SCSTATIC) ? EVSTATICINI : EVFOLD,
                                 /* globl */ 0, decl.qual, NULL);
            break;
         }
         unops[nunop].span = span;
         unops[nunop].ty = decl.ty;
         if (++nunop >= countof(unops)) {
            ex = exprparse(cm, 999, NULL, 0);
            break;
         }
         goto Unary;
      }
      break;
   /* base exprs */
   case TKNUMLIT:
   case TKCHRLIT:
      ex = mkexpr(ENUMLIT, tk.span, mktype(0), );
      if (!(ty.t = parsenumlit(&ex.u, &ex.f, &tk, 0)))
         error(&tk.span, "bad %s literal %'tk", tk.t == TKNUMLIT ? "number" : "character", &tk);
      ex.ty.t = ty.t ? ty.t : TYINT;
      break;
   case TKWtrue: case TKWfalse:
      ex = mkexpr(ENUMLIT, tk.span, mktype(TYBOOL), .u = tk.t == TKWtrue);
      break;
   case TKSTRLIT:
      ty = mktype(((const char []){TYCHAR, TYSHORT, TYINT})[tk.wide]);
      ex = mkexpr(ESTRLIT, tk.span, mkarrtype(ty, 0, tk.len+1), { .s.p = (void *)tk.s, .s.n = tk.len });
      break;
   case TKIDENT: Ident: {
      Decl *decl = finddecl(cm, tk.name);
      if (!decl) {
         if (cm->env->up && tk.name->c == '_'
          && (!strcmp(&tk.name->c, "__FUNCTION__") || !strcmp(&tk.name->c, "__PRETTY_FUNCTION__"))) {
            /* hack: treat these identifiers as __func__ synonym to support the GNU extension */
            warn(&tk.span, "%'tk is a GNU extension", &tk);
            decl = finddecl(cm, istr__func__);
            assert(decl && decl->scls == SCSTATIC);
            goto Sym;
         } else if (ctx == EATTRARG && nunop == 0 && (peek(cm, NULL) == ',' || peek(cm, NULL) == ')')) {
            return ex = mkexpr(ESYM, tk.span, mktype(IMPLICITSYMTY), .implicitsym = tk.name);
         } else if (peek(cm, NULL) == '(') { /* implicit function decl? */
            ex = mkexpr(ESYM, tk.span, mktype(IMPLICITSYMTY), .implicitsym = tk.name);
         } else {
            error(&tk.span, "undeclared identifier %'tk", &tk);
            ex = mkexpr(ESYM, tk.span, mktype(TYINT), .implicitsym = NULL);
         }
      } else if (decl->scls == SCTYPEDEF) {
         error(&tk.span, "unexpected typename %'tk (expected expression)", &tk);
         ex = mkexpr(ESYM, tk.span, decl->ty, .implicitsym = NULL);
      } else if (decl->isenum) {
         ex = mkexpr(ENUMLIT, tk.span, decl->ty, .i = decl->value);
      } else Sym: {
         if (decl->name == istr__func__ && decl->isbuiltin) { /* lazy __func__ */
            internstr fnname = decl->sym;
            decl->isbuiltin = 0;
            decl->sym = mkhiddensym(&fnname->c, "__func__", 1);
            assert(decl->ty.t == TYARRAY && typechild(decl->ty).t == TYCHAR);
            uint siz = typesize(decl->ty);
            (void) mkdatref(decl->sym, decl->ty, siz, 1, fnname, siz, 0, 1);
         }
         ex = mkexpr(ESYM, tk.span, decl->ty, .qual = decl->qual, .decl = decl - declsbuf.p);
      }
      break; }
   case TKWsizeof: case TKW_Alignof: case TKWalignof: {
      enum toktag tt = tk.t;
      uint res;
      span = tk.span;
      if (!match(cm, NULL, '(')) /* sizeof/alignof expr */
         goto Unops;
      else if (isdecltok(cm)) { /* sizeof/alignof (type) */
         DeclState st = { DCASTEXPR };
         ty = pdecl(&st, cm).ty;
         peek(cm, &tk);
         if (expect(cm, ')', NULL))
            joinspan(&span.ex, tk.span.ex);
         res = sizeofalignofcheck(&span, tt, ty, NULL);
      } else { /* sizeof/alignof expr */
         Expr tmp = commaexpr(cm);
         peek(cm, &tk);
         if (expect(cm, ')', NULL))
            joinspan(&span.ex, tk.span.ex);
         ppostfixopers(cm, &tmp);
         ty = tmp.ty;
         res = sizeofalignofcheck(&span, tt, ty, &tmp);
      }
      ex = mkexpr(ENUMLIT, span, mktype(targ_sizetype), .u = res);
      break; }
   case TKW__builtin_va_arg:
      span = tk.span;
      ex = vaargexpr(cm, &span);
      break;
   case TKW_Generic:
      span = tk.span;
      ex = genericexpr(cm, &span);
      break;
   default:
      fatal(&tk.span, "expected %s (near %'tk)", ctx == EFROMSTMT ? "statement" : "expression", &tk);
   }

   ppostfixopers(cm, &ex);

   /* unary operators (process) */
   while (nunop-- > 0) {
      enum exprkind ek;
      span = unops[nunop].span;
      joinspan(&span.ex, ex.span.ex);
      if (unops[nunop].t0 == 0) {
         switch (unops[nunop].tt) {
         case '+':
            ek = EPLUS;
            goto Alu;
         case '-':
            ek = ENEG;
            goto Alu;
         case '~':
            ek = ECOMPL;
            goto Alu;
         case '!':
            ek = ELOGNOT;
         Alu:
            ty = ek == ELOGNOT ? mktype(TYINT) : cvtarith(ex.ty, ex.ty);
            if (!ty.t || (ek == ECOMPL && !isint(ty))) {
               error(&tk.span, "invalid operand to %'tk '%ty'", &tk, ex.ty);
               ty = mktype(TYINT);
            }
            ex = mkexpr(ek, span, ty, .sub = exprdup(cm, &ex));
            break;
         case TKINC: case TKDEC:
            ty = ex.ty;
            incdeccheck(tk.t, &ex, &span);
            ex = mkexpr(unops[nunop].tt == TKINC ? EPREINC : EPREDEC, span, ty,
                        .sub = exprdup(cm, &ex));
            break;
         case '*':
            if (ex.ty.t == TYPTR || ex.ty.t == TYARRAY) {
               ty = typechild(ex.ty);
               if (ty.t != TYVOID && isincomplete(typedecay(ty))) {
                  error(&span, "cannot dereference pointer to incomplete type '%ty'", ty);
                  ty = mktype(TYINT);
               }
            } else {
               error(&span, "invalid operand to unary * '%ty'", ex.ty);
               ty = mktype(TYINT);
            }
            ex = mkexpr(EDEREF, span, ty, .qual = ex.ty.flag & TFCHLDQUAL,
                        .sub = exprdup(cm, &ex));
            break;
         case '&':
            if (!islvalue(&ex))
               error(&span, "operand to unary & is not an lvalue");
            if (ex.t == EGETF && ex.fld.bitsiz)
               error(&span, "cannot take address of bitfield");
            ex = mkexpr(EADDROF, span, mkptrtype(ex.ty, ex.qual), .sub = exprdup(cm, &ex));
            break;
         case TKWsizeof: case TKW_Alignof: case TKWalignof:
            ex = mkexpr(ENUMLIT, span, mktype(targ_sizetype),
                  .u = sizeofalignofcheck(&span, unops[nunop].tt, ex.ty, &ex));
            break;
         default: assert(0);
         }
      } else { /* cast */
         ty = unops[nunop].ty;
         if (!castcheck(ty, &ex))
            error(&span, "cannot cast value of type '%ty' to '%ty'", ex.ty, ty);
         if (ex.t == ENUMLIT && isint(ex.ty) && ty.t == TYPTR) {
            ex.ty = ty;
            ex.span = span;
         } else {
            ex = mkexpr(ECAST, span, ty, .sub = exprdup(cm, &ex));
         }
      }
   }

   /* binary operators */
   for (int opprec; (opprec = tkprec(peek(cm, &tk))) >= prec;) {
      enum exprkind ek = bintab[tk.t].t;
      Expr rhs, tmp;
      lex(cm, NULL);
      if (ek != ECOND) {
         /* only the assignment operators are right-associative */
         bool leftassoc = (bintab[tk.t].k & BCSET) == 0;
         /* ex OP rhs */
         span.sl = tk.span.sl;
         span.ex = ex.span.ex;
         rhs = exprparse(cm, opprec + leftassoc, NULL, 0);
         if (!joinspan(&span.ex, tk.span.ex) || !joinspan(&span.ex, rhs.span.ex))
            span.ex = tk.span.ex;
         ty = bintypecheck(&span, tk.t, &ex, &rhs);
         assert(ty.t);
         ex = mkexpr(ek, span, ty, .sub = exprdup2(cm, &ex, &rhs));
      } else {
         /* logical-OR-expression ? expression : conditional-expression */
         Expr *sub;
         span.sl = tk.span.sl;
         span.ex = ex.span.ex;
         if (!isscalar(ex.ty) && !isptrcvt(ex.ty))
            error(&ex.span, "?: condition is not a scalar type: '%ty'", ex.ty);
         tmp = commaexpr(cm);
         joinspan(&tk.span.ex, tmp.span.ex);
         expect(cm, ':', NULL);
         rhs = exprparse(cm, opprec, NULL, 0);
         if (!joinspan(&span.ex, tk.span.ex) || !joinspan(&span.ex, tmp.span.ex)
           || !joinspan(&span.ex, rhs.span.ex))
            span.ex = tk.span.ex;
         ty = condtype(&tmp, &rhs);
         if (!ty.t) {
            error(&span, "incompatible types in conditional expression: '%ty', '%ty'", tmp.ty, rhs.ty);
            ty = tmp.ty;
         }
         sub = alloc(&cm->exarena, 3 * sizeof*sub, 0);
         sub[0] = ex, sub[1] = tmp, sub[2] = rhs;
         ex = mkexpr(ECOND, span, ty, .sub = sub);
      }
   }

   return ex;
}

static Expr
expr(CComp *cm)
{
   return exprparse(cm, bintab['='].prec, NULL, 0); /* non-comma expr */
}

static Expr
arraycountexpr(CComp *cm)
{
   return exprparse(cm, bintab['='].prec, NULL, EARRAYCOUNT); /* non-comma expr, or lone '*' */
}

static Expr
constantexpr(CComp *cm)
{
   return exprparse(cm, bintab['?'].prec, NULL, 0); /* conditional-expr */
}

static Expr
commaexpr(CComp *cm)
{
   return exprparse(cm, 1, NULL, 0);
}

/****************/
/* Initializers */
/****************/

static uint
nmemb(Type ty)
{
   switch (ty.t) {
   case TYARRAY: return typearrlen(ty) ? typearrlen(ty) : -1u;
   case TYUNION: case TYSTRUCT: return typedata[ty.dat].nmemb;
   default: return 1;
   }
}

static bool
objectp(Type ty)
{
   return isagg(ty) || ty.t == TYARRAY;
}

static bool
chrarrayof(Type ty, Type chld)
{
   assert(isint(chld));
   return ty.t == TYARRAY && isint(typechild(ty)) && typesize(typechild(ty)) == typesize(chld);
}

static Type
membertype(uint *off, uint *bitsiz, uint *bitoff, Type ty, uint idx)
{
   *bitsiz = *bitoff = 0;
   if (!objectp(ty)) {
      *off = 0;
      return ty;
   } else if (ty.t == TYARRAY) {
      *off = typesize(typechild(ty)) * idx;
      return typechild(ty);
   } else if (idx < typedata[ty.dat].nmemb) {
      FieldData fld = typedata[ty.dat].fld[idx].f;
      *off = fld.off;
      *bitsiz = fld.bitsiz, *bitoff = fld.bitoff;
      return fld.t;
   }
   *off = ~0u;
   return mktype(0);
}

typedef struct InitCur InitCur;
typedef struct InitReloc InitReloc;
typedef struct InitParser {
   struct InitCur {
      Type ty;
      uint idx;
      uint off;
      short prev;
   } buf[32], *cur, *sub;
   Arena **arena;
   uint arrlen;
   enum evalmode ev;
   bool dyn; /* when set, data is written to a temporary buffer first, because either:
                  - size is not known until parsing done (implicit array size)
                  - data section is not known until parsing done (to avoid relocs in .rodata)
                otherwise write to the corresponding object data section buffer directly */
   union {
      Init *init; /* for initializer with automatic storage */
      struct {  /* for static storage (dyn = 0) */
         enum section sec;
         uint off;
      };
      struct { /* for static storage (dyn = 1) */
         vec_of(uchar) ddat;
         struct InitReloc {
            struct InitReloc *link;
            internstr sym;
            vlong addend;
            uint off;
         } *drel;
      };
   };
} InitParser;

static void
excesscheck(InitParser *ip, const Span *span)
{
   Type sub = ip->sub->ty;
   uint n = nmemb(sub);
   if (ip->sub->idx == n) {
      if (sub.t == TYARRAY)
         warn(span, "excess elements in array initializer for '%ty'", sub);
      else if (sub.t == TYSTRUCT)
         warn(span, "excess elements in initializer; '%ty' has %u member%s", sub, n, &"s"[n==1]);
      else if (sub.t == TYUNION)
         warn(span, "excess elements in union initializer");
      else
         warn(span, "excess elements in scalar initializer");
   }
}

#if 1
#define dumpini(_)
#else
/* debugging */
static void
dumpini(InitParser *ip)
{
   efmt(">>>\n");
   for (InitCur *s = ip->buf; s < ip->sub+1; ++s) {
      efmt("  ");
      efmt("%d. [%ty, %u]", s- ip->buf, s->ty, s->idx);
      if (s == ip->cur) efmt(" <-- cursor");
      ioputc(&bstderr, '\n');
   }
   efmt("<<<\n");
}
#endif

static vlong /* -> returns addend */
expr2reloc(internstr *psym, const Expr *ex)
{
   if (ex->t == ESSYMREF) {
      *psym = ex->ssym.sym;
      return ex->ssym.off;
   } else if (ex->t == ESTRLIT || ex->t == EINIT) {
      if (ex->t == ESTRLIT) assert(ex->ty.t == TYARRAY);
      *psym = xcon2sym(expraddr(NULL, ex).i);
      return 0;
   }
   fatal(&ex->span, "internal bug: non static reloc?");
}

static bool
rodatarelocok(void)
{
   return !(ccopt.pie | ccopt.pic);
}

static void
iniwrite(CComp *cm, InitParser *ip, uint off, uint bitsiz, uint bitoff, Type ty, Expr *ex)
{
   if (ex->ty.t == TYSTRUCT && ip->ev == EVSTATICINI) {
      assert(ty.bits == ex->ty.bits);
      for (uint i = 0, n = nmemb(ex->ty); i < n; ++i) {
         uint suboff;
         Type sub = membertype(&suboff, &bitsiz, &bitoff, ex->ty, i);
         iniwrite(cm, ip, off + suboff, bitsiz, bitoff, sub, exprdup(cm, &mkexpr(EGETF, ex->span, sub, .sub = ex)));
      }
   } else if (ip->ev == EVSTATICINI) {
      uchar *p;
      uint siz = typesize(ty);
      if (nerror) return;
      if (ip->dyn) {
         if (ip->ddat.n < off + siz) {
            /* extending ddata, can only happen for unknown-size arrays */
            uint old = ip->ddat.n;
            vresize(&ip->ddat, off + siz);
            memset(ip->ddat.p + old, 0, ip->ddat.n - old);
            assert(off + siz == ip->ddat.n);
         }
         p = ip->ddat.p + off;
      } else {
         p = (ip->sec == Sdata ? objout.data.p : objout.rodata.p) + ip->off + off;
      }

      if (ex->t == ENUMLIT) {
         Expr *e = ex, tmp;
         if (ex->ty.bits != ty.bits && ty.t != TYPTR) {
            tmp = mkexpr(ECAST, ex->span, ty, .sub = ex);
            e = &tmp;
            assert(eval(e, EVSTATICINI));
            assert(e->t == ENUMLIT);
         }
         if (!bitsiz) switch (siz) {
         default: assert(0);
         case 1: *p = e->u; break;
         case 2: wr16targ(p, e->u); break;
         case 4: isint(ty) ? wr32targ(p, e->u) : wrf32targ(p, e->f); break;
         case 8: isint(ty) ? wr64targ(p, e->u) : wrf64targ(p, e->f); break;
         } else {
            uvlong mask = (bitsiz == 64 ? -1ull : (1ull << bitsiz) - 1) << bitoff;
            if (bitoff + bitsiz > siz*8) siz <<= 1; /* straddles an allocation boundary */
            switch (siz) {
            default: assert(0);
            case 1: *p = (*p &~ mask) | (e->u << bitoff & mask); break;
            case 2: wr16targ(p, (rd16targ(p) &~ mask) | (e->u << bitoff & mask)); break;
            case 4: wr32targ(p, (rd32targ(p) &~ mask) | (e->u << bitoff & mask)); break;
            case 8: wr64targ(p, (rd64targ(p) &~ mask) | (e->u << bitoff & mask)); break;
            }
         }
      } else if (ty.t == TYARRAY && ex->t == ESTRLIT) {
         uint n = ex->s.n * typesize(typechild(ty));
         if (siz < n) n = siz;
         /* XXX endian for wide strs */
         memcpy(p, ex->s.p, n);
      } else {
         internstr sym;
         vlong addend = expr2reloc(&sym, ex);
         if (!ip->dyn) {
            assert(ip->sec != Srodata || rodatarelocok());
            objreloc(sym, targ_64bit ? REL_ABS64 : REL_ABS32,
                     ip->sec, ip->off + off, addend);
         } else {
            InitReloc *rel = alloc(ip->arena, sizeof *rel, 0);
            rel->link = ip->drel;
            rel->sym = sym;
            rel->off = off;
            rel->addend = addend;
            ip->drel = rel;
         }
      }
   } else {
      assert(cm != NULL);
      Init *init = ip->init;
      InitElem val = {
         .off = off,
         .bitsiz = bitsiz,
         .bitoff = bitoff,
         .ex = *ex
      }, *new = alloccopy(&cm->exarena, &val, sizeof val, 0);
      *init->tail = new;
      init->tail = &new->next;
      if (!bitsiz) for (uint i = off, end = i + typesize(ex->ty); i < end; ++i) {
         if (BSSIZE(end) > countof(init->zero)) break;
         bsclr(init->zero, i);
      }
   }
}

static bool
iniwriterec(CComp *cm, InitParser *ip, uint off, Expr *ex)
{
   assert(ex->t == EINIT);
   for (InitElem *v = ex->init->vals; v; v = v->next) {
      if (v->ex.t == EINIT) iniwriterec(cm, ip, off + v->off, &v->ex);
      else if (ip->ev && !eval(&v->ex, ip->ev) && ip->ev != EVFOLD) return 0;
      else iniwrite(cm, ip, off + v->off, v->bitsiz, v->bitoff, v->ex.ty, &v->ex);
   }
   return 1;
}

static InitCur *
iniadvance(InitParser *ip, InitCur *c, const Span *span)
{
   if (c - ip->buf >= countof(ip->buf) - 1)
      fatal(span, "too many nested initializers");
   return c + 1;
}

/* set the initializer cursor object */
static void
inifocus(InitParser *ip, CComp *cm, const Span *span, uint idx)
{
   while (idx >= nmemb(ip->sub->ty) && ip->sub != ip->cur) {
      --ip->sub;
      idx = ip->sub->idx;
   }
   uint off, bitsiz, bitoff;
   Type targ = membertype(&off, &bitsiz, &bitoff, ip->sub->ty, idx);
   InitCur *next = iniadvance(ip, ip->cur, span);
   assert(!bitsiz);

   if (isagg(ip->sub->ty) && targ.t == TYARRAY && !typearrlen(targ))
      error(span, "cannot initialize flexible array member");
   excesscheck(ip, span);

   next->ty = targ;
   next->idx = 0;
   next->off = ip->sub->off + off;
   next->prev = ip->cur - ip->buf;
   ++ip->cur->idx;
   ip->sub = ip->cur = next;
}

/* initialize a character array with a string literal */
static void
inistrlit(CComp *cm, Expr *ex, Type *ty)
{
   if (isincomplete(*ty)) {
      *ty = mkarrtype(typechild(*ty), ty->flag & TFCHLDQUAL, ex->s.n + 1);
   } else if (typearrlen(*ty) < ex->s.n) {
      warn(&ex->span, "string literal in initializer is truncated from %u to %u bytes",
            (ex->s.n+1)*typesize(typechild(*ty)), typesize(*ty));
   }
   ex->ty = *ty;
}

/* read scalar initializer into initializer list and avance */
static void
ininext(InitParser *ip, CComp *cm)
{
   uint off, bitsiz, bitoff;
   Type targ;
   Expr ex = expr(cm);

Retry:
   targ = membertype(&off, &bitsiz, &bitoff, ip->sub->ty, ip->sub->idx);

   if (isagg(ip->sub->ty) && targ.t == TYARRAY && !typearrlen(targ)) {
      error(&ex.span, "cannot initialize flexible array member");
      ++ip->sub->idx;
      return;
   }
   if (ex.t == ESTRLIT && chrarrayof(targ, typechild(ex.ty))) {
      assert(!isincomplete(targ));
      inistrlit(cm, &ex, &targ);
      iniwrite(cm, ip, ip->sub->off + off, 0,0, targ, &ex);
      ++ip->sub->idx;
      return;
   } else if (ex.t == ESTRLIT && ip->sub->idx == 0 && chrarrayof(ip->sub->ty, typechild(ex.ty))) {
      /* handle e.g. (char []){"foo"} */
      assert(off == 0);
      targ = ip->sub->ty;
      inistrlit(cm, &ex, &targ);
      iniwrite(cm, ip, ip->sub->off, 0,0, targ, &ex);
      if (ip->sub == ip->buf && ip->arrlen < ex.s.n+1)
         ip->arrlen = ex.s.n+1;
      --ip->sub;
      return;
   } else if (ip->sub->idx >= nmemb(ip->sub->ty) && ip->sub != ip->cur) {
      --ip->sub;
      goto Retry;
   } else if (objectp(targ) && targ.bits != ex.ty.bits) {
      InitCur *next = iniadvance(ip, ip->sub, &ex.span);
      if (ip->sub - ip->buf == countof(ip->buf) - 1)
         fatal(&ex.span, "too many nested initializers");
      ++ip->sub->idx;
      *next = (InitCur) { targ, .off = ip->sub->off + off };
      ip->sub = next;
      goto Retry;
   }
   excesscheck(ip, &ex.span);

   if (targ.t) {
      if (!initcheck(targ, &ex))
         error(&ex.span, "cannot initialize '%ty' with expression of type '%ty'", targ, ex.ty);
      else {
         if (targ.bits == ex.ty.bits && ex.t == EINIT) {
            if (!iniwriterec(cm, ip, ip->sub->off + off, &ex))
               goto CannotEval;
         } else if (ip->ev && !eval(&ex, ip->ev) && ip->ev != EVFOLD) {
         CannotEval:
            error(&ex.span, "cannot evaluate expression statically");
         } else {
            Expr *pex = &ex;
            if (ip->ev != EVSTATICINI) {
               if (ex.ty.bits != targ.bits)
                  ex = mkexpr(ECAST, ex.span, targ, .sub = exprdup(cm, &ex));
               pex = exprdup(cm, &ex);
            }
            iniwrite(cm, ip, ip->sub->off + off, bitsiz, bitoff, targ, pex);
         }
      }
   }
   if (ip->sub == ip->buf && ip->arrlen < ip->sub->idx+1)
      ip->arrlen = ip->sub->idx+1;

   if (++ip->sub->idx == 0) {
      error(&ex.span, "element makes object too large");
      --ip->sub->idx;
   }
}

static int
aggdesignator(InitParser *ip, Type ty, internstr name, const Span *span)
{
   const TypeData *td = &typedata[ty.dat];
   for (int i = 0; i < td->nmemb; ++i) {
      NamedField *fld = &td->fld[i];
      if (fld->name == name) {
         return i;
      } else if (!fld->name) {
         int save, sub;
         InitCur *next = iniadvance(ip, ip->sub, span);
         save = ip->sub->idx;
         ip->sub->idx = i+1;
         *next = (InitCur) { fld->f.t, .off = ip->sub->off + fld->f.off };
         ip->sub = next;
         sub = aggdesignator(ip, fld->f.t, name, span);
         if (sub == -1) {
            --ip->sub;
            ip->sub->idx = save;
         } else return sub;
      }
   }
   return -1;
}

static bool
designators(InitParser *ip, CComp *cm)
{
   Token tk;
   Span span;
   bool some = 0;

   for (;;) {
      uint off, bitsiz, bitoff;
      uvlong idx = ~0ull;
      if (match(cm, &tk, '[')) {
         Expr ex = commaexpr(cm);
         span = tk.span;
         joinspan(&span.ex, ex.span.ex);
         peek(cm, &tk);
         if (some) {
            Type ty = membertype(&off, &bitsiz, &bitoff, ip->sub->ty, ip->sub->idx++);
            InitCur *next = iniadvance(ip, ip->sub, &tk.span);
            assert(!bitsiz);
            *next = (InitCur) { ty, .off = ip->sub->off + off };
            ip->sub = next;
            dumpini(ip);
         }
         if (expect(cm, ']', NULL)) joinspan(&span.ex, tk.span.ex);
         if (ip->sub->ty.t != TYARRAY)
            error(&ex.span, "array designator used with non-array type '%ty'", ip->sub->ty);
         if (!eval(&ex, EVINTCONST))
            error(&ex.span, "array designator index is not an integer constant");
         else if (issigned(ex.ty) && ex.i < 0)
            error(&ex.span, "negative array designator index");
         else if (ex.i > ~0u - 1)
            error(&ex.span, "index too large");
         else {
            idx = ex.u;
            ip->sub->idx = idx;
            if (ip->sub == ip->buf && ip->arrlen < idx+1)
               ip->arrlen = idx+1;
            dumpini(ip);
         }
         some = 1;
      } else if (match(cm, &tk, '.')) {
         span = tk.span;
         peek(cm, &tk);
         if (some) {
            Type ty = membertype(&off, &bitsiz, &bitoff, ip->sub->ty, ip->sub->idx++);
            InitCur *next = iniadvance(ip, ip->sub, &tk.span);
            *next = (InitCur) { ty, .off = ip->sub->off + off };
            ip->sub = next;
            dumpini(ip);
         }
         if (expect(cm, TKIDENT, NULL)) joinspan(&span.ex, tk.span.ex);
         if (!isagg(ip->sub->ty))
            error(&span, "member designator used with non-aggregate type '%ty'", ip->sub->ty);
         else if (tk.t == TKIDENT) {
            int idx;
            for (;;) {
               idx = aggdesignator(ip, ip->sub->ty, tk.name, &span);
               if (idx >= 0 || ip->sub == ip->cur) break;
               --ip->sub;
            }
            ip->sub->idx = idx;
            if (idx < 0)
               error(&span, "%ty has no such field: '%s'", ip->cur->ty, tk.name);
            dumpini(ip);
         }
         some = 1;
      } else {
         if (some) {
            expect(cm, '=', NULL);
         }
         return some;
      }
   }
}

static Expr
initializer(CComp *cm, Type *ty, enum evalmode ev, bool globl,
            enum qualifier qual, internstr sym)
{
   Token tk;
   Span span;
   Init res = {0};
   InitParser ip[1] = {0};

   ip->arena = &cm->exarena;
   ip->ev = ev;
   if (ev == EVSTATICINI) {
      if (ty->t == TYARRAY && isincomplete(*ty)) {
         ip->dyn = 1;
      } else if (qual & QCONST && !rodatarelocok()) {
         ip->dyn = 1;
         vresize(&ip->ddat, typesize(*ty));
         memset(ip->ddat.p, 0, typesize(*ty));
      } else {
         ip->sec = qual & QCONST ? Srodata : Sdata;
         if (!nerror)
            ip->off = objnewdat(sym, ip->sec, globl, typesize(*ty), typealign(*ty));
      }
   } else {
      ip->init = &res;
      res.tail = &res.vals;
   }

   if (!match(cm, &tk, '{')) {
      Expr ex = expr(cm);
      if (ex.t == ESTRLIT && chrarrayof(*ty, typechild(ex.ty))) {
         inistrlit(cm, &ex, ty);
         iniwrite(cm, ip, 0, 0, 0, *ty, &ex);
      } else if (!initcheck(*ty, &ex)) {
         error(&ex.span, "cannot initialize '%ty' with expression of type '%ty'", *ty, ex.ty);
      } else {
         if (ev && !eval(&ex, ev) && ev != EVFOLD)
            error(&ex.span, "cannot evaluate expression statically");
         else
            iniwrite(cm, ip, 0, 0, 0, *ty, &ex);
      }
      if (ip->dyn)
         goto Dynfix;
      return ex;
   }

   assert(countof(res.zero) == BSSIZE(64));
   if (ev != EVSTATICINI) {
      memset(res.zero, 0xFF, sizeof res.zero);
   }

   span = tk.span;
   ip->sub = ip->cur = ip->buf;
   ip->cur->ty = *ty;
   for (;;) {
      peek(cm, &tk);
      joinspan(&span.ex, tk.span.ex);
      if (tk.t == '[' || tk.t == '.') {
         designators(ip, cm);
      }
      if (match(cm, &tk, '}')) {
         if (ip->cur == ip->buf) break;
         ip->sub = ip->cur = ip->buf + ip->cur->prev;
         dumpini(ip);
      } else if (match(cm, &tk, '{')) {
         Span span = tk.span;
         inifocus(ip, cm, &tk.span, ip->sub->idx);
         if (peek(cm, &tk) == '}') {
            if (!joinspan(&span.ex, tk.span.ex)) span = tk.span;
            if (!objectp(ip->sub->ty)) {
               error(&span, "scalar initializer cannot be empty");
            } else if (ccopt.cstd < STDC23 && ccopt.pedant) {
               warn(&span, "empty initializer in %M is an extension");
            }
         } else if (ip->sub->ty.t && !objectp(ip->sub->ty)) {
            warn(&span, "brace initializer for scalar object '%ty'", ip->sub->ty);
         }
         continue;
      } else {
         dumpini(ip);
         ininext(ip, cm);
      }
      match(cm, NULL, ',');
      if (peek(cm, &tk) != '}' && ip->sub->ty.t == TYUNION) {
         if (ip->sub == ip->cur) {
            warn(&tk.span, "excess elements in union initializer");
         } else while (ip->sub != ip->cur && ip->sub->ty.t == TYUNION) {
            --ip->sub;
         }
      }
   }
   if (ip->dyn) {
      enum section sec;
      uint off, siz, align;
      uchar *p;

      if (isincomplete(*ty)) {
         uint len = ip->arrlen > ip->cur->idx ? ip->arrlen : ip->cur->idx;
         if (len == 0)
            error(&span, "array cannot have zero length");
         *ty = mkarrtype(typechild(*ty), ty->flag & TFCHLDQUAL, len);
      }
   Dynfix:
      if (qual & QCONST && (ip->drel == NULL || rodatarelocok()))
         sec = Srodata;
      else
         sec = Sdata;
      if (!nerror) {
         off = objnewdat(sym, sec, globl, siz = typesize(*ty), align = typealign(*ty));
         p = sec == Srodata ? objout.rodata.p : objout.data.p;
         assert(ip->ddat.n <= siz);
         memcpy(p + off, ip->ddat.p, ip->ddat.n);
         memset(p + off + ip->ddat.n, 0, siz - ip->ddat.n);
         for (InitReloc *rel = ip->drel; rel; rel = rel->link) {
            objreloc(rel->sym, targ_64bit ? REL_ABS64 : REL_ABS32, sec, off + rel->off, rel->addend);
         }
      }
      vfree(&ip->ddat);
   }
   dumpini(ip);

   if (ev == EVSTATICINI) {
      return (Expr){.span = span};
   } else {
      uint siz;
      if (isincomplete(*ty)) {
         uint len = ip->arrlen > ip->cur->idx ? ip->arrlen : ip->cur->idx;
         if (!len)
            error(&span, "initializer creates a zero-sized array");
         *ty = mkarrtype(typechild(*ty), ty->flag & TFCHLDQUAL, len);
      }

      assert(countof(res.zero) == 1);
      siz = typesize(*ty);
      if (siz && siz <= 64)
         res.zero->u &= ~0ull >> (64 - siz);

      return mkexpr(EINIT, span, *ty, .init = alloccopy(&cm->exarena, &res, sizeof res, 0));
   }
}

/* debugging */
void
dumpexpr(const Expr *ex, bool prity)
{
   static const char *name[] = {
      [EXXX] = "xxx",         [ENUMLIT] = "numlit", [ESTRLIT] = "strlit",
      [ESSYMREF] = "ssymref", [ESYM] = "sym",       [EVAARG] = "vaarg",
      [EINIT] = "init",       [EGETF] = "getf",     [ECALL] = "call",
      [ECOND] = "cond",       [EPLUS] = "plus",     [ENEG] = "neg",
      [ECOMPL] = "compl",     [ELOGNOT] = "lognot", [EDEREF] = "deref",
      [EADDROF] = "addrof",   [ECAST] = "cast",     [EPREINC] = "preinc",
      [EPOSTINC] = "postinc", [EPREDEC] = "predec", [EPOSTDEC] = "postdec",
      [EADD] = "add",         [ESUB] = "sub",       [EMUL] = "mul",
      [EDIV] = "div",         [EREM] = "rem",       [EBAND] = "band",
      [EBIOR] = "bior",       [EXOR] = "xor",       [ESHL] = "shl",
      [ESHR] = "shr",         [ELOGAND] = "logand", [ELOGIOR] = "logior",
      [EEQU] = "equ",         [ENEQ] = "neq",       [ELTH] = "lth",
      [EGTH] = "gth",         [ELTE] = "lte",       [EGTE] = "gte",
      [ESET] = "set",         [ESETADD] = "setadd", [ESETSUB] = "setsub",
      [ESETMUL] = "setmul",   [ESETDIV] = "setdiv", [ESETREM] = "setrem",
      [ESETAND] = "setand",   [ESETIOR] = "setior", [ESETXOR] = "setxor",
      [ESETSHL] = "setshl",   [ESETSHR] = "setshr", [ESEQ] = "seq",
   };
   ioputc(&bstderr, '(');
   efmt("%s ", name[ex->t]);
   if (ex->ty.t && (prity || ex->t == EVAARG || ex->t == ECAST))
      efmt("<%ty> ", ex->ty);
   int nsub = 0;
   switch (ex->t) {
   case ENUMLIT:  if (!isflt(ex->ty)) efmt(isunsigned(ex->ty) ? "%lu" : "%ld", ex->u);
                  else efmt("%f", ex->f);
                  break;
   case ESTRLIT:  efmt("%'S", ex->s.p, ex->s.n); break; /* XXX widestr */
   case ESYM:     efmt("%y", declsbuf.p[ex->decl].name); break;
   case ESSYMREF: efmt("%y%+d", ex->ssym.sym, ex->ssym.off);
                  if (ex->ssym.func) efmt(" @func");
                  if (ex->ssym.local) efmt(" @local");
                  break;
   case EVAARG: dumpexpr(ex->sub, prity); break;
   case EGETF:  dumpexpr(ex->sub, prity); efmt(" #+%u", ex->fld.off);
                if (ex->fld.bitsiz) efmt("[%d:%d]", ex->fld.bitoff, ex->fld.bitsiz);
                break;
   case ECALL:  nsub = ex->narg+1; goto Sub;
   case ECOND:  nsub = ex->narg+1; goto Sub;
   default:
      if (in_range(ex->t, EPLUS, EPOSTDEC)) nsub = 1;
      else nsub = 2;
   Sub:
      for (int i = 0; i < nsub; ++i) {
         if (i) ioputc(&bstderr, ' ');
         dumpexpr(&ex->sub[i], prity);
      }
      break;
   case EINIT: assert(!"nyi");
   }
   ioputc(&bstderr, ')');
}

/*****************/
/* Decls Parsing */
/*****************/

static Type
buildagg(CComp *cm, enum typetag tt, internstr name, int id)
{
   Token tk;
   Type t;
   Span flexspan;
   NamedField fbuf[32];
   vec_of(NamedField) fld = VINIT(fbuf, countof(fbuf));
   TypeData td = {tt};
   bool isunion = tt == TYUNION;
   const char *tag = isunion ? "union" : "struct";
   uint bitsiz = 0, bitfbyteoff = 0,
        bitoff = 0, bitftypesiz = 0;

   while (!match(cm, &tk, '}')) {
      DeclState st = { DFIELD };
      do {
         Decl decl = pdecl(&st, cm);
         uint tysize;
         if (st.empty) {
            if (ccopt.pedant)
               warn(&decl.span, "extra semicolon in aggregate");
            continue;
         }
         tysize = typesize(decl.ty);
         if (fld.n && td.flexi) {
            td.flexi = 0;
            error(&flexspan, "flexible array member is not at end of struct");
         }
         if (!isunion && decl.ty.t == TYARRAY && !typearrlen(decl.ty)) {
            td.flexi = 1;
            flexspan = decl.span;
         } else if (isincomplete(decl.ty)) {
            error(&decl.span, "field has incomplete type '%ty'", decl.ty);
         } else if (decl.ty.t == TYFUNC)  {
            error(&decl.span, "field has function type '%ty'", decl.ty);
         }
         bitsiz = 0;
         if (st.bitf) {
            Expr ex = constantexpr(cm);
            const char *name = decl.name ? &decl.name->c : "<anonymous>";
            if (!isint(decl.ty)) {
               error(&decl.span, "bit-field '%s' has non-integer type '%ty'", name, decl.ty);
            } else if (!isint(ex.ty)) {
               error(&ex.span, "integer constant expression has non-integer type '%ty'", decl.ty);
            } else if (!eval(&ex, EVINTCONST)) {
               error(&ex.span, "cannot evaluate integer constant expression");
            } else if (ex.i < 0) {
               error(&ex.span, "bit-field '%s' has negative width '%ld'", name, ex.i);
            } else if (ex.i > 8*tysize) {
               error(&ex.span, "width of bit-field '%s' (%ld) exceeds width of type (%d)",
                               name, ex.i, 8*tysize);
            } else if (ex.i == 0 && decl.name) {
               error(&ex.span, "named bit-field '%s' has zero width", name);
            } else {
               bitsiz = ex.i;
               if (bitsiz == 0) {
                  bitsiz = bitftypesiz - bitoff;
                  continue;
               } else if (bitftypesiz && bitftypesiz < tysize) {
                  /* end of previous bitfield */
                  bitoff = 0;
                  bitfbyteoff += bitftypesiz;
               } else if (!bitftypesiz) {
                  bitoff = 0;
                  bitfbyteoff = alignup(td.siz, typealign(decl.ty));
               } else if (bitoff + bitsiz > 8*bitftypesiz) {
                  /* no straddling boundaries */
                  bitoff = 0;
                  bitfbyteoff += bitftypesiz;
               }
               if (tysize > bitftypesiz) bitftypesiz = tysize;
            }
            pdecl(&st, cm);
         } else {
            bitftypesiz = bitoff = bitsiz = 0;
         }
         if (decl.ty.t) {
            uint align = typealign(decl.ty);
            uint siz = tysize;
            uint off = bitftypesiz ? bitfbyteoff : isunion ? 0 : alignup(td.siz, align);
            NamedField f = { decl.name, { decl.ty, off, bitsiz, bitoff, .qual = decl.qual }};
            if (bitftypesiz && siz != bitftypesiz) while (f.f.bitoff + f.f.bitsiz > 8*siz) {
               /* adjust bitfields narrower than container type */
               f.f.off += siz;
               f.f.bitoff -= 8*siz;
            }
            if (!decl.name && !bitftypesiz) {
               if (!isagg(decl.ty) || tagtypetags[typedata[decl.ty.dat].id]) {
                  warn(&decl.span, "declaration does not declare anything");
                  continue;
               } else if (ccopt.cstd < STDC11 && ccopt.pedant) {
                  warn(&decl.span, "anonymous %s in %M is an extension",
                       decl.ty.t == TYUNION ? "union" : "struct");
               }
            }
            if (decl.name || !bitftypesiz)
               vpush(&fld, f);
            td.anyconst |= decl.qual & QCONST;
            if (isagg(decl.ty)) {
               td.anyconst |= typedata[decl.ty.dat].anyconst;
               if (typedata[decl.ty.dat].flexi && !isunion)
                  error(&decl.span, "nested aggregate has flexible array member");
            }
            if (isunion)
               td.siz = td.siz < siz ? siz : td.siz;
            else
               td.siz = off + siz;
            td.align = td.align < align ? align : td.align;
            bitoff += bitsiz;
         }
      } while (st.more);
   }
   if (td.flexi && fld.n == 1)
      error(&flexspan, "flexible array member in otherwise empty aggregate");
   if (td.flexi && ccopt.cstd < STDC99 && ccopt.pedant)
      warn(&flexspan, "flexible array member in %M is an extension");
   if (fld.n == 0) {
      NamedField dummy = { intern(""), { mktype(TYCHAR), 0 }};
      error(&tk.span, "%s cannot have zero members", tag);
      vpush(&fld, dummy);
      td.siz = td.align = 1;
   }
   td.siz = alignup(td.siz, td.align);
   td.fld = fld.p;
   td.nmemb = fld.n;
   if (id != -1)
      t = completetype(name, id, &td);
   else
      t = mktagtype(name, &td);
   vfree(&fld);
   return t;
}

static inline void
inttyminmax(vlong *min, uvlong *max, enum typetag tt)
{
   uint bits = 8*targ_primsizes[tt];
   *min = isunsignedt(tt) ? 0 : -(1ull << (bits - 1));
   *max = isunsignedt(tt) ? ~0ull >> (64 - bits) : bits == 64 ? ~0ull>>1 : (1ll << (bits - 1)) - 1;
}

/* the backing type of enum (without a C23 fixed backing type) is int or the
 * smallest-rank type that all the enumerators fit in, or if it doesn't exist,
 * then the biggest signed type. the type of enumeration constants is the type of
 * its defining expression when present or the type of the previous enumerator
 * or in case of overflow the smallest type that fits (previous value + 1)
 * this isn't strictly conforming since pre C23 enums are pretty loosely defined,
 * and this is similar to existing compiler's de-facto behaviour (though gcc
 * prefers to use unsigned types when possible). should add support for -fshort-enums
 */
static Type
buildenum(CComp *cm, internstr name, const Span *span, int id)
{
   Token tk;
   vlong tymin, minv = 0;
   uvlong tymax, maxv = 0;
   TypeData td = {TYENUM, .backing = TYINT};
   Type ty = mktype(td.backing);
   Span maxvspan;
   vlong iota = 0;
   bool somelonglong = 0;

   inttyminmax(&tymin, &tymax, td.backing);
   while (!match(cm, &tk, '}')) {
      Decl decl = {0};
      peek(cm, &tk);
      expect(cm, TKIDENT, NULL);
      if (match(cm, NULL, '=') || (peek(cm, NULL) == TKNUMLIT && !expect(cm, '=', NULL))) {
         Expr ex = expr(cm);
         if (eval(&ex, EVINTCONST)) {
            iota = ex.i;
            if (ex.ty.t != ty.t)
               inttyminmax(&tymin, &tymax, ex.ty.t);
            ty = ex.ty;
         } else {
            error(&ex.span, "enum value is not an integer constant");
         }
      } else if (tk.t != TKIDENT) {
         lex(cm, NULL);
         continue;
      }
      while (issigned(ty) ? (iota > (vlong)tymax || iota < tymin) : iota > tymax)
         inttyminmax(&tymin, &tymax, ++ty.t);
      somelonglong |= ty.t >= TYVLONG;
      if ((isunsigned(ty) || iota > 0) && iota > maxv)
         maxv = iota, maxvspan = tk.span;
      else if (issigned(ty) && iota < minv)
         minv = iota;

      decl.name = tk.name;
      decl.ty = ty;
      decl.isenum = 1;
      decl.value = iota++;
      putdecl(cm, &decl);
      if (!match(cm, &tk, ',')) {
         if (expect(cm, '}', "or `,'"))
            break;
         else lex(cm, NULL);
      }
   }

   td.backing = 0;
   if (minv >= 0 && maxv <= ~0u) {
      td.backing = TYUINT;
   } else for (int t = TYINT; t <= TYUVLONG; ++t) {
      inttyminmax(&tymin, &tymax, t);
      if (minv >= tymin && maxv <= tymax) {
         td.backing = t;
         break;
      }
   }
   if (!td.backing) {
      td.backing = !somelonglong && ccopt.cstd == STDC89 && ccopt.pedant ? TYLONG : TYVLONG;
      warn(&maxvspan, "enumerators exceed range of enum's backing type '%ty'", mktype(td.backing));
   }
   if (td.backing >= TYVLONG && !somelonglong && ccopt.cstd == STDC89 && ccopt.pedant)
      warn(span, "enum backing type is '%ty' in %M", mktype(td.backing));

   if (id != -1)
      ty = completetype(name, id, &td);
   else
      ty = mktagtype(name, &td);
   ty.backing = td.backing;
   return ty;
}

static Type
tagtype(CComp *cm, enum toktag kind)
{
   Token tk;
   Type t;
   Span span;
   enum typetag tt = kind == TKWenum ? TYENUM : kind == TKWstruct ? TYSTRUCT : TYUNION;
   internstr tag = NULL;

   peek(cm, &tk);
   if (match(cm, &tk, TKIDENT))
      tag = tk.name;
   span = tk.span;
   if (!match(cm, NULL, '{')) {
      if (!tag) {
         error(&tk.span, "expected %tt name or '{'", kind);
         return mktype(0);
      }
      t = gettagged(cm, &span, tt, tag, /* def? */ peek(cm, NULL) == ';');
   } else {
      if (tag) {
         t = deftagged(cm, &span, tt, tag, mktype(0));
         if (t.t != tt || !isincomplete(t)) {
            if (t.t != tt)
               error(&tk.span,
                     "defining tagged type %'tk as %tt clashes with previous definition",
                     &tk, kind);
            else
               error(&tk.span, "redefinition of '%tt %s'", kind, tag);
            note(DGERROR, &span, "previous definition:");
         }
      }
      if (tt == TYENUM)
         t = buildenum(cm, tag, &span, tag ? typedata[t.dat].id : -1);
      else
         t = buildagg(cm, tt, tag, tag ? typedata[t.dat].id : -1);
   }

   if (t.t != tt) {
      error(&tk.span, "declaring tagged type %'tk as %tt clashes with previous definition",
            &tk, kind);
      note(DGERROR, &span, "previous definition:");
   }
   return t;
}

static bool
attrspec(CComp *cm, int *attr)
{ /* __attribute__ (( attribute-list )) */
   if (!match(cm, NULL, TKW__attribute__)) return 0;
   if (!expect(cm, '(', "after __attribute__") || !expect(cm, '(', "after __attribute__")) {
   Bad:
      fatal(NULL, NULL);
   }
   while (!match(cm, NULL, ')')) {
      Token tk;
      lex(cm, &tk);
      if (tk.t != TKIDENT && !in_range(tk.t, TKWBEGIN_, TKWEND_)) {
         fatal(&tk.span, "expected attribute name");
      }
      internstr name = tk.name;
      int ltrim = name[0].c == '_' && name[1].c == '_',
          rtrim = tk.len > 2 && name[tk.len-1].c == '_' && name[tk.len-2].c == '_';
      if (ltrim || rtrim) { /* trim surrounding '__' */
         name = intern_(&name->c + ltrim*2, tk.len - ltrim*2 - rtrim*2);
      }
      if (match(cm, NULL, '(')) {
         while (!match(cm, NULL, ')')) {
            (void)exprparse(cm, bintab['='].prec, NULL, EATTRARG);
            if (!match(cm, NULL, ',')) {
               if (expect(cm, ')', NULL)) break;
               else goto Bad;
            }
         }
      }
      (void)name;
      if (!match(cm, NULL, ',')) {
         if (expect(cm, ')', NULL)) break;
         else goto Bad;
      }
   }
   if (!expect(cm, ')', NULL))
      goto Bad;
   return 1;
}

static Type
ptypeof(CComp *cm)
{
   Type ty;
   expect(cm, '(', NULL);
   if (isdecltok(cm)) { /* typeof (type) */
      DeclState st = { DCASTEXPR };
      ty = pdecl(&st, cm).ty;
   } else { /* typeof (expr) */
      ty = commaexpr(cm).ty;
   }
   expect(cm, ')', NULL);
   return ty;
}

static void
declspec(DeclState *st, CComp *cm, Span *pspan)
{
   Token tk;
   Decl *decl;
   enum arith {
      KSIGNED   = 1<<0,
      KUNSIGNED = 1<<1,
      KBOOL     = 1<<2,
      KCHAR     = 1<<3,
      KSHORT    = 1<<4,
      KLONG     = 1<<5,
      KLONGLONG = 1<<6,
      KINT      = 1<<7,
      KFLOAT    = 1<<8,
      KDOUBLE   = 1<<9,
      KCOMPLEX  = 1<<10,
   } arith = 0;
   Span span = {0};
   Type ty = st->base;
   bool properdecl = st->kind == DFUNCVAR || st->kind == DTOPLEVEL;

   for (bool first = 1;; first = 0) {
      enum storageclass scls = 0;
      peek(cm, &tk);
      if (first) span = tk.span;
      switch (tk.t) {
      /* storage-class-specifier */
      case TKWtypedef:  scls = SCTYPEDEF; break;
      case TKWextern:   scls = SCEXTERN; break;
      case TKWstatic:   scls = SCSTATIC; break;
      case TKWauto:     scls = SCAUTO; break;
      case TKWregister: scls = SCREGISTER; break;
      case TKWthread_local:
      case TKW_Thread_local: scls = SCTHREADLOCAL; break;

      /* function-specifier */
      case TKWinline:
         if (!properdecl) BadFnSpec: {
            error(&tk.span, "function specifier %'tk is not allowed here", &tk);
            break;
         }
         st->fninline = 1;
         break;
      case TKW_Noreturn:
         if (!properdecl) goto BadFnSpec;
         st->fnnoreturn = 1;
         break;

      /* alignment-specifier */
      case TKW_Alignas: assert(!"nyi alignas"); break;

      /* type-qualifier */
      case TKWconst: st->qual |= QCONST; break;
      case TKWvolatile: st->qual |= QVOLATILE; break;
      case TKWrestrict: /* unimplemented */ break;
      case TKW_Atomic: /* unimplemented */ break;

      /* type-specifier */
      case TKWvoid:
         if (st->base.t) {
         DupBase:
            error(&tk.span, "more than one data type in declaration specifier");
         } else {
            st->base = mktype(TYVOID);
         }
         break;
      case TKWsigned:
         arith |= KSIGNED;
         break;
      case TKWunsigned:
         arith |= KUNSIGNED;
         break;
      case TKW_Bool: case TKWbool:
         if (arith & KBOOL) goto DupArith;
         arith |= KBOOL;
         break;
      case TKWchar:
         if (arith & KCHAR) {
         DupArith:
            error(&tk.span, "duplicate %tk specifier", &tk);
         }
         arith |= KCHAR;
         break;
      case TKWshort:
         arith |= KSHORT;
         break;
      case TKWlong:
         if ((arith & (KLONG | KLONGLONG)) == KLONG)
            arith = (arith &~ KLONG) | KLONGLONG;
         else if ((arith & (KLONG | KLONGLONG)) == 0)
            arith |= KLONG;
         else
            error(&tk.span, "too long");
         break;
      case TKWint:
         if (arith & KINT) goto DupArith;
         arith |= KINT;
         break;
      case TKWfloat:
         if (arith & KFLOAT) goto DupArith;
         arith |= KFLOAT;
         break;
      case TKWdouble:
         if (arith & KDOUBLE) goto DupArith;
         arith |= KDOUBLE;
         break;
      case TKW_Complex:
         if (arith & KCOMPLEX) goto DupArith;
         arith |= KCOMPLEX;
         break;
      case TKWenum:
      case TKWstruct:
      case TKWunion:
         lex(cm, &tk);
         ty = tagtype(cm, tk.t);
         st->tagdecl = 1;
         joinspan(&span.ex, tk.span.ex);
         if (st->base.t) goto DupBase;
         st->base = ty;
         continue;
      case TKW__typeof__: case TKWtypeof:
         lex(cm, &tk);
         ty = ptypeof(cm);
         joinspan(&span.ex, tk.span.ex);
         if (st->base.t) goto DupBase;
         st->base = ty;
         continue;
      case TKIDENT:
         if (!st->base.t && !arith && (decl = finddecl(cm, tk.name))
             && decl->scls == SCTYPEDEF) {
            lex(cm, &tk);
            st->base = decl->ty;
            continue;
         }
         /* fallthru */
      default:
         goto End;
      case TKW_BitInt:
      case TKW_Decimal128: case TKW_Decimal32:
      case TKW_Decimal64: case TKW_Imaginary:
         error(&tk.span, "%'tk is unsupported", &tk);
         arith = arith ? arith : KINT;
      }

      if ((!properdecl && scls && !(st->kind == DFUNCPARAM && scls == SCREGISTER)) || (scls == SCAUTO && st->kind == DTOPLEVEL))
         error(&tk.span, "storage class specifier %'tk is not allowed here", &tk);
      else
         st->scls |= scls;

      joinspan(&span.ex, tk.span.ex);
      lex(cm, &tk);
   }
End:
   if (pspan) *pspan = span;

   if (st->scls && properdecl) {
      if (popcnt(st->scls) > 1)
         error(&span, "invalid combination of storage class specifiers");
   }
   if (st->base.t && arith) {
      /* combining arith type specifiers and other types */
   Bad:
      error(&span, "invalid type specifiers");
      st->base = mktype(TYINT);
   } else if (!st->base.t && arith) {
      enum typetag t;
      if (arith == KFLOAT)
         t = TYFLOAT;
      else if (arith == KDOUBLE)
         t = TYDOUBLE;
      else if (arith == (KLONG | KDOUBLE))
         t = TYLDOUBLE;
      else if (arith == KBOOL)
         t = TYBOOL;
      else if (arith == KCHAR)
         t = TYCHAR;
      else if (arith == (KSIGNED | KCHAR))
         t = TYSCHAR;
      else if (arith == (KUNSIGNED | KCHAR))
         t = TYUCHAR;
      else if ((arith & ~KINT & ~KSIGNED) == KSHORT)
         t = TYSHORT;
      else if ((arith & ~KINT) == (KUNSIGNED | KSHORT))
         t = TYUSHORT;
      else if ((arith & ~KINT & ~KSIGNED) == 0)
         t = TYINT;
      else if ((arith & ~KINT) == KUNSIGNED)
         t = TYUINT;
      else if ((arith & ~KINT & ~KSIGNED) == KLONG)
         t = TYLONG;
      else if ((arith & ~KINT) == (KUNSIGNED | KLONG))
         t = TYULONG;
      else if ((arith & ~KINT & ~KSIGNED) == KLONGLONG)
         t = TYVLONG;
      else if ((arith & ~KINT) == (KUNSIGNED | KLONGLONG))
         t = TYUVLONG;
      else if (arith == (KCOMPLEX | KFLOAT))
         t = TYCOMPLEXF;
      else if (arith == (KCOMPLEX | KDOUBLE))
         t = TYCOMPLEX;
      else if (arith == (KCOMPLEX | KLONG | KDOUBLE))
         t = TYCOMPLEXL;
      else
         goto Bad;
      st->base = mktype(t ? t : TYINT);
   } else if (!st->base.t) {
      if (ccopt.cstd < STDC23 && peek(cm, NULL) == TKIDENT) {
         if (ccopt.cstd > STDC89)
            warn(&tk.span, "type implicitly declared as int");
         st->base = mktype(TYINT);
      } else {
         error(&tk.span, "type specifier missing");
      }
   }
}

/* circular doubly linked list used to parse declarators */
enum { EARRAYUNSIZED = 0xFF /* for count.t */ };
typedef struct DeclList DeclList;
static struct DeclList {
   DeclList *prev, *next;
   uchar t; /* TYPTR, TYARRAY or TYFUNC */
   union {
      struct {
         uchar qual; /* TYPTR, TYARRAY */
         Expr count; /* TYARRAY */
      };
      struct { /* TYFUNC */
         Type *param;
         internstr *pnames;
         Span *pspans;
         uchar *pqual;
         short npar;
         bool kandr : 1, variadic : 1;
      };
   };
   Span span;
} decltmp[64], *declfreelist;
static bool usingdeclparamtmp;
static Type declparamtmp[16];
static internstr declpnamestmp[16];
static Span declpspanstmp[16];
static uchar declpqualtmp[16];

static void
declinsert(DeclList *list, const DeclList *node)
{
   DeclList *pnode = declfreelist;
   if (!pnode) fatal(NULL, "too many nested declarators");
   declfreelist = declfreelist->next;
   *pnode = *node;
	pnode->next = list->next;
	pnode->prev = list;
	list->next->prev = pnode;
	list->next = pnode;
}

static int
cvqual(CComp *cm)
{
   Token tk;
   int q = 0;
   while (match(cm, &tk, TKWconst) || match(cm, &tk, TKWvolatile) || match(cm, &tk, TKWrestrict))
      q |= tk.t == TKWconst ? QCONST : tk.t == TKWvolatile ? QVOLATILE : 0;
   return q;
}

static void
decltypes(CComp *cm, DeclList *list, internstr *name, Span *span, Span *namespan)
{
   Token tk;
   DeclList *ptr, node;

   while (match(cm, &tk, '*')) {
      node.t = TYPTR;
      node.qual = cvqual(cm);
      node.span = tk.span;
      declinsert(list, &node);
      joinspan(&span->ex, tk.span.ex);
   }
   ptr = list->next;
   switch (peek(cm, &tk)) {
   case '(':
      lex(cm, &tk);
      if (isdecltok(cm)) {
         goto Func;
      } else if (match(cm, &tk, ')')) {
         /* T () is K&R func proto */
         node.span = tk.span;
         node.t = TYFUNC;
         node.param = NULL;
         node.pqual = NULL;
         node.pnames = NULL;
         node.pspans = NULL;
         node.variadic = 0;
         node.kandr = 1;
         node.npar = 0;
         declinsert(ptr->prev, &node);
         joinspan(&span->ex, tk.span.ex);
         break;
      } else {
         decltypes(cm, list, name, span, namespan);
         expect(cm, ')', NULL);
         joinspan(&span->ex, tk.span.ex);
      }
      break;
   case TKIDENT:
      if (!name)
         error(&tk.span, "unexpected identifier in type name");
      else {
         *name = tk.name;
         *namespan = tk.span;
      }
      lex(cm, &tk);
      joinspan(&span->ex, tk.span.ex);
      break;
   default:
      if (name)
         *name = NULL;
   }
   for (;;) {
      if (match(cm, &tk, '[')) {
         node.span = tk.span;
         node.qual = 0;
         bool statik = 0;
         if (in_range(peek(cm, &tk), TKWBEGIN_, TKWEND_)) {
            node.qual = cvqual(cm);
            statik = match(cm, NULL, TKWstatic);
            node.qual |= cvqual(cm);
         }
         (void)statik; /* stub */

         if (match(cm, &tk, ']')) {
            node.count.t = EARRAYUNSIZED;
         } else {
            node.count = arraycountexpr(cm);
            peek(cm, &tk);
            joinspan(&node.span.ex, tk.span.ex);
            expect(cm, ']', NULL);
         }
         node.t = TYARRAY;
         declinsert(ptr->prev, &node);
         joinspan(&span->ex, node.span.ex);
      } else if (match(cm, &tk, '(')) Func: {
         vec_of(Type) params = {0};
         vec_of(uchar) qual = {0};
         vec_of(internstr) names = {0};
         vec_of(Span) spans = {0};

         if (!usingdeclparamtmp) {
            usingdeclparamtmp = 1;
            vinit(&params, declparamtmp, countof(declparamtmp));
            vinit(&qual, declpqualtmp, countof(declpqualtmp));
            vinit(&names, declpnamestmp, countof(declpnamestmp));
            vinit(&spans, declpspanstmp, countof(declpspanstmp));
         }

         node.span = tk.span;
         node.kandr = 0;
         node.variadic = 0;
         if (ccopt.cstd < STDC23 && !isdecltok(cm)) {
            node.kandr = 1;
         }

         if (!match(cm, &tk, ')')) for (;;) {
            if (match(cm, &tk, TKDOTS)) {
               node.variadic = 1;
               expect(cm, ')', NULL);
               break;
            }
            if (node.kandr) {
               if (match(cm, &tk, TKIDENT)) {
                  vpush(&params, mktype(TYINT));
                  vpush(&names, tk.name);
                  vpush(&spans, tk.span);
               } else error(&tk.span, "expected identifier");
            } else if (!isdecltok(cm) && peek(cm, &tk) != TKIDENT) {
               error(&tk.span, "expected parameter declarator");
            } else {
               DeclState st = { DFUNCPARAM };
               Decl decl;
               decl = pdecl(&st, cm);
               decl.ty = typedecay(decl.ty);
               vpush(&params, decl.ty);
               vpush(&names, decl.name);
               vpush(&spans, decl.span);
               vpush(&qual, decl.qual);
               if (decl.ty.t == TYVOID) {
                  if (params.n > 1 || decl.qual || decl.name || peek(cm, &tk) != ')') {
                     error(&decl.span, "function parameter #%d has void type",
                           params.n, decl.ty, qual.p[params.n-1]);
                  }
               }
            }
            peek(cm, &tk);
            joinspan(&node.span.ex, tk.span.ex);
            if (!match(cm, &tk, ',')) {
               expect(cm, ')', "or `,'");
               break;
            }
         } else {
            if (ccopt.cstd < STDC23) node.kandr = 1;
         }
         if (node.kandr && ccopt.cstd != STDC89 && params.n > 0) {
            warn(&node.span, "K&R function prototype is deprecated");
         } else if (params.n == 1 && params.p[0].t == TYVOID && !qual.p[0] && !names.p[0]) { /* (void) */
            vfree(&params);
            vfree(&names);
            vfree(&spans);
            vfree(&qual);
         }
         node.t = TYFUNC;
         node.param = params.n ? params.p : NULL;
         node.pqual = qual.n ? qual.p : NULL;
         node.pnames = params.n ? names.p : NULL;
         node.pspans = params.n ? spans.p : NULL;
         node.npar = params.n;
         declinsert(ptr->prev, &node);
         joinspan(&span->ex, node.span.ex);
      } else break;
   }
}

static Decl
declarator(DeclState *st, CComp *cm, Span span0) {
   Decl decl = { st->base, st->scls, .qual = st->qual, .span = span0 };
   DeclList list = { &list, &list }, *l;
   Span namespan = {0};
   static bool inidecltmp;
   if (!inidecltmp) {
      inidecltmp = 1;
      for (int i = 0; i < countof(decltmp); ++i) {
         decltmp[i].next = declfreelist;
         declfreelist = &decltmp[i];
      }
   }

   decltypes(cm, &list, st->kind == DCASTEXPR ? NULL : &decl.name, &decl.span, &namespan);
   if (!decl.name && st->kind != DCASTEXPR && st->kind != DFUNCPARAM) {
      if (list.prev == &list) lex(cm, NULL);
      error(&decl.span, "expected `(', `*' or identifier");
   }
   for (l = list.prev; l != &list; l = l->prev) {
      switch (l->t) {
      case TYPTR:
         decl.ty = mkptrtype(decl.ty, decl.qual);
         decl.qual = l->qual;
         break;
      case TYARRAY:
         if (isincomplete(decl.ty))
            error(&l->span, "array has incomplete element type '%ty'", decl.ty);
         else if (decl.ty.t == TYFUNC)
            error(&l->span, "array has element has function type '%ty'", decl.ty);
         if (l->count.t == EARRAYUNSIZED) /* unsized '[]' */
            decl.ty = mkarrtype(decl.ty, decl.qual, 0);
         else {
            uint n = 0;
            Expr *ex = &l->count;
            if (!ex->t) { /* ['*'] */
               if (l->prev != &list) error(&l->span, "[*] array declarator is not allowed here");
            } else if (!eval(ex, EVINTCONST)) {
               error(&ex->span, "array length is not an integer constant");
            } else if (issigned(ex->ty) && ex->i < 0) {
               error(&ex->span, "array length is negative");
            } else if (ex->u > (1ull << (8*sizeof n)) - 1) {
               error(&ex->span, "array too long (%ul)", ex->u);
            } else if (ex->u == 0) {
               /* when struct field, silently accept zero-length as synonym of '[]' for flexible array member */
               if (l->prev != &list || st->kind != DFIELD) {
                  warn(&ex->span, "array cannot have zero length");
               }
            } else {
               n = ex->u;
            }
            decl.ty = mkarrtype(decl.ty, decl.qual, n);
         }
         decl.qual = l->qual;
         break;
      case TYFUNC:
         if (decl.ty.t == TYFUNC)
            error(&decl.span, "function cannot return function type '%ty'", decl.ty);
         else if (decl.ty.t == TYARRAY)
            error(&decl.span, "function cannot return array type '%ty'", decl.ty);
         if (l->kandr && ccopt.cstd > STDC89 && (ccopt.cstd >= STDC23 || ccopt.pedant))
            warn(&l->span, "function declaration without a prototype is deprecated");
         decl.ty = mkfntype(decl.ty, l->npar, l->param, l->kandr, l->variadic);
         if (l->param != declparamtmp) free(l->param);
         if (l->prev == &list && l->npar) { /* last */
            st->pnames = alloccopy(&cm->fnarena, l->pnames, l->npar * sizeof(char *), 0);
            st->pspans = alloccopy(&cm->fnarena, l->pspans, l->npar * sizeof(Span), 0);
            st->pqual = l->pqual ? alloccopy(&cm->fnarena, l->pqual, l->npar, 1) : NULL;
            decl.inlin = st->fninline;
            decl.noret = st->fnnoreturn;
         }
         if (l->pqual != declpqualtmp) free(l->pqual);
         if (l->pnames != declpnamestmp) free(l->pnames);
         if (l->pspans != declpspanstmp) free(l->pspans);
         if (l->param == declparamtmp) usingdeclparamtmp = 0;
         decl.qual = 0;
         break;
      }

      l->next = declfreelist;
      declfreelist = l;
   }
   if (st->kind != DCASTEXPR && decl.name)
      decl.span = namespan;
   return decl;
}

static void
pstaticassert(CComp *cm, Span *span)
{
   Expr ex;
   Token tk, msg = {0};

   /* _Static_assert '(' <expr> [ ',' <strlit> ] ')' ';' */
   expect(cm, '(', NULL);
   ex = expr(cm);
   peek(cm, &tk);
   if (match(cm, &tk, ',')) {
      peek(cm, &msg);
      expect(cm, TKSTRLIT, NULL);
   }
   peek(cm, &tk);
   expect(cm, ')', NULL);
   expect(cm, ';', NULL);

   joinspan(&span->ex, tk.span.ex);
   if (!msg.t && ccopt.cstd == STDC11)
      warn(span, "static assert without message is a C23 extension");
   if (!eval(&ex, EVINTCONST)) {
      error(&ex.span, "static assert expression is not an integer constant");
   } else if (iszero(ex)) {
      if (msg.t)
         error(&ex.span, "static assertion failed: %'S", msg.s, msg.len);
      else
         error(&ex.span, "static assertion failed");
   }
}

static Decl
pdecl(DeclState *st, CComp *cm) {
   Token tk;
   Decl decl;
   bool properdecl = st->kind == DTOPLEVEL || st->kind == DFUNCVAR;
   bool first = 0;

   assert(!st->funcdef);

   if (st->varini || st->bitf) {
      memset(&decl, 0, sizeof decl);
      goto AfterIniBitf;
   }
   decl.sym = NULL;

   if (st->base0) goto DeclSpec;
   if (!st->base.t) {
      if (properdecl && (match(cm, &tk, TKW_Static_assert) || match(cm, &tk, TKWstatic_assert))) {
         pstaticassert(cm, &tk.span);
         return (Decl){0};
      }
      first = 1;
      if (match(cm, &tk, ';')) {
         st->empty = 1;
         return (Decl){.span = tk.span};
      }

   DeclSpec:
      st->base0 = 0;
      while (attrspec(cm, &st->attr)) ;
      declspec(st, cm, &decl.span);
   } else {
      peek(cm, &tk);
      decl.span = tk.span;
   }
   if (st->scls == SCTYPEDEF) properdecl = 0;

   if (first && st->tagdecl && match(cm, &tk, ';')) {
      decl = (Decl) { st->base, st->scls, st->qual, .span = decl.span };
      return decl;
   } else if (st->kind == DFIELD && match(cm, &tk, ':')) {
      decl = (Decl) { st->base, st->scls, st->qual, .span = decl.span };
      st->bitf = 1;
      return decl;
   }
   decl = declarator(st, cm, decl.span);
   if (decl.ty.t != TYFUNC && st->fninline)
      error(&decl.span, "`inline' used on non-function declaration");
   if (decl.ty.t != TYFUNC && st->fnnoreturn)
      error(&decl.span, "`_Noreturn' used on non-function declaration");
   /* trailing attributes */
   if (st->kind == DTOPLEVEL || st->kind == DFUNCVAR) {
      while (attrspec(cm, &st->attr)) ;
      if (match(cm, NULL, TKW__asm__) && expect(cm, '(', NULL)) {
         peek(cm, &tk);
         if (expect(cm, TKSTRLIT, "asm symbol name")) {
            decl.sym = intern_(tk.s, tk.len);
         }
         expect(cm, ')', NULL);
      }
      while (attrspec(cm, &st->attr)) ;
   }

   if (properdecl && match(cm, &tk, '=')) {
      st->varini = 1;
      return decl;
   } else if (first && decl.ty.t == TYFUNC && match(cm, &tk, '{')) {
      st->funcdef = 1;
      return decl;
   } else if (st->kind == DFIELD && match(cm, &tk, ':')) {
      st->bitf = 1;
      return decl;
   }

AfterIniBitf:
   st->varini = st->bitf = 0;
   st->more = 0;
   if (st->kind != DCASTEXPR && st->kind != DFUNCPARAM) {
      if (match(cm, &tk, ','))
         st->more = 1;
      else expect(cm, st->kind == DFUNCPARAM ? ')' : ';', "or `,'");
   }

   return decl;
}

/*****************/
/* IR Generation */
/*****************/

static inline Ref
exprvalue(Function *fn, const Expr *ex)
{
   return compileexpr(fn, ex, /*discard*/ 0);
}
static inline void
expreffects(Function *fn, const Expr *ex)
{
   compileexpr(fn, ex, /*discard*/ 1);
}

static void
structcopy(Function *fn, Type ty, Ref dst, Ref src)
{
   IRType typ = mkirtype(ty);
   addinstr(fn, mkarginstr(typ, dst));
   addinstr(fn, mkarginstr(typ, src));
   addinstr(fn, mkintrin(INstructcopy, 0, 2));
}

static Ref
structreturn(Function *fn, const Expr *src)
{
   return expraddr(fn, src);
}

static Ref compilecall(Function *fn, const Expr *ex);

static internstr
mkhiddensym(const char *fnname, const char *name, int id)
{
   char buf[200];
   WriteBuf wbuf = MEMBUF(buf, sizeof buf);
   assert(id > 0);
   if (fnname)
      bfmt(&wbuf, "%s.%s.%d", fnname, name, id-1);
   else
      bfmt(&wbuf, "%s.%d", name, id-1);
   ioputc(&wbuf, 0);
   assert(!wbuf.err);
   return intern(buf);
}

static void geninit(Function *fn, Type t, Ref dst, const Expr *src);
static Ref condexprvalue(Function *fn, const Expr *ex, bool discard);

Ref
expraddr(Function *fn, const Expr *ex)
{
   Decl *decl;
   Ref r;

   switch (ex->t) {
   case ESYM:
      decl = &declsbuf.p[ex->decl];
      assert(decl != NULL);
      switch (decl->scls) {
      case SCAUTO: case SCREGISTER:
         assert(decl->id >= 0);
         return mkref(RTMP, decl->id);
      case SCEXTERN: case SCNONE: case SCSTATIC:
         return mksymref(decl->sym, (SFUNC & -(decl->ty.t == TYFUNC)) | (SLOCAL & -(decl->scls == SCSTATIC || decl->isdef)));
      default:
         assert(0);
      }
      break;
   case ESSYMREF:
      return irbinop(fn, Oadd, KPTR,
               mksymref(ex->ssym.sym, (SFUNC & -ex->ssym.func) | (SLOCAL & -ex->ssym.local)),
               mkintcon(KPTR, ex->ssym.off));
   case ESTRLIT:
      /* XXX endian for wide strs */
      return mkdatref(NULL, ex->ty, typesize(ex->ty), typealign(ex->ty), ex->s.p, ex->s.n * typesize(typechild(ex->ty)), /*deref*/0, fn != NULL);
   case EDEREF:
      return exprvalue(fn, ex->sub);
   case EGETF:
      r = expraddr(fn, ex->sub);
      assert(ex->fld.bitsiz == 0);
      return irbinop(fn, Oadd, KPTR, r, mkintcon(KI32, ex->fld.off));
   case ESET:
      assert(isagg(ex->ty));
      r = expraddr(fn, &ex->sub[1]);
      structcopy(fn, ex->ty, expraddr(fn, &ex->sub[0]), r);
      return r;
   case ECALL:
      assert(isagg(ex->ty));
      return compilecall(fn, ex);
   case EVAARG:
      assert(isagg(ex->ty));
      return builtin_va_arg_comp(fn, ex, 0);
   case EINIT:
      if (fn) {
         /* compound literal, allocate temp */
         r = addinstr(fn, mkalloca(typesize(ex->ty), typealign(ex->ty)));
         geninit(fn, ex->ty, r, ex);
         return r;
      } else {
         /* emit static dat */
         static int id;
         InitParser ip[1] = {0};
         Type ty = ex->ty;
         internstr sym = mkhiddensym(NULL, ".LC", ++id);
         ip->sec = Sdata; /* TODO put in rodata if possible */
         ip->ev = EVSTATICINI;
         assert(!isincomplete(ty));
         ip->off = objnewdat(sym, ip->sec, 0, typesize(ty), typealign(ty));
         if (!iniwriterec(NULL, ip, 0, (Expr *)ex))
            error(&ex->span, "cannot not evaluate expression statically");
         return mksymref(sym, 0);
      }
   case ESEQ:
      expreffects(fn, &ex->sub[0]);
      return expraddr(fn, &ex->sub[1]);
   case ECOND:
      assert(isagg(ex->ty));
      return condexprvalue(fn, ex, 0);
   default:
      assert(!"lvalue?>");
   }

}

static Ref
genload(Function *fn, Type t, Ref ref, bool volatyl)
{
   Instr ins = {0};

   assert(isscalar(t));
   ins.cls = type2cls[scalartypet(t)];
   assert(ins.cls);
   switch (typesize(t)) {
   case 1: ins.op = issigned(t) ? Oloads8 : Oloadu8; break;
   case 2: ins.op = issigned(t) ? Oloads16 : Oloadu16; break;
   case 4: ins.op = isflt(t) ? Oloadf32 : issigned(t) ? Oloads32 : Oloadu32; break;
   case 8: ins.op = isflt(t) ? Oloadf64 : Oloadi64; break;
   default: assert(0);
   }
   ins.l = ref;
   ins.keep = volatyl;
   return addinstr(fn, ins);
}

static Ref
genstore(Function *fn, Type t, Ref ptr, Ref val)
{
   Instr ins = {0};

   assert(isscalar(t));
   switch (typesize(t)) {
   case 1: ins.op = Ostorei8; break;
   case 2: ins.op = Ostorei16; break;
   case 4: ins.op = isflt(t) ? Ostoref32 : Ostorei32; break;
   case 8: ins.op = isflt(t) ? Ostoref64 : Ostorei64; break;
   default: assert(0);
   }
   ins.l = ptr;
   ins.r = val;
   return addinstr(fn, ins);
}

static void genbitfstore(Function *fn, const Type ty, Ref addr,
                         const ExprGetFld *fld, Ref tmp, Ref val);

static void
geninit(Function *fn, Type t, Ref dst, const Expr *src)
{
   Ref adr;
   if (src->t == EINIT) {
      Init *ini = src->init;
      uint siz = typesize(t);
      uint align = typealign(t);
      BitSet azero[1] = {0};

      if (BSSIZE(siz) <= countof(ini->zero)) {
         for (int i = 0; i < siz; i += align) {
            for (int j = 0; j < align; ++j) {
               if (bstest(ini->zero, i + j)) {
                  bsset(azero, i);
                  break;
               }
            }
         }
         if (bscount(azero, countof(azero)) < 32) {
            /* write individual zeros at non initialized gaps */
            for (uint i = 0; bsiter(&i, azero, countof(azero)) && i < siz; i += align) {
               adr = irbinop(fn, Oadd, KPTR, dst, mkref(RICON, i));
               addinstr(fn, mkinstr(Ostorei8 + ilog2(align), 0, .l = adr, .r = ZEROREF));
            }
         } else {
            goto Memset0;
         }
      } else Memset0: {
         /* memset(dst,0,siz) */
         /* TODO make it into an intrinsic */
         Instr call = { Ocall, KPTR };
         addinstr(fn, mkarginstr(cls2type(KPTR), dst));
         addinstr(fn, mkarginstr(cls2type(KI32), ZEROREF));
         addinstr(fn, mkarginstr(cls2type(type2cls[targ_sizetype]), mkintcon(type2cls[targ_sizetype], siz)));
         call.l = mksymref(istr_memset, 1);
         call.r = mkcallarg(cls2type(KPTR), 3, -1);
         addinstr(fn, call);
      }
      for (InitElem *val = ini->vals; val; val = val->next) {
         uint off = val->off;
         Expr *ex = &val->ex;
         adr = irbinop(fn, Oadd, KPTR, dst, mkref(RICON, off));
         if (ex->t == EINIT || ex->t == ESTRLIT) {
            geninit(fn, ex->ty, adr, ex);
         } else if (isagg(ex->ty)) {
            structcopy(fn, ex->ty, adr, expraddr(fn, ex));
         } else if (!val->bitsiz) {
            genstore(fn, ex->ty, adr, exprvalue(fn, ex));
         } else {
            Ref q = exprvalue(fn, ex);
            genbitfstore(fn, ex->ty, adr, &(ExprGetFld){0, val->bitsiz, val->bitoff}, NOREF, q);
         }
      }
   } else if (src->t == ESTRLIT) {
      Type ctyp = typechild(src->ty);
      uint csiz = typesize(ctyp);
      adr = dst;
      for (uint i = 0; i < src->s.n; ++i) {
         if (csiz == 1)
            genstore(fn, ctyp, adr, mkref(RICON, src->s.p[i]));
         else if (csiz == 2)
            genstore(fn, ctyp, adr, mkref(RICON, src->s.w16[i]));
         else
            genstore(fn, ctyp, adr, mkintcon(KI32, src->s.w32[i]));
         adr = irbinop(fn, Oadd, KPTR, dst, mkref(RICON, (i+1)*csiz));
      }
      genstore(fn, ctyp, adr, ZEROREF); /* null term */
   } else assert(0);
}

static bool
isboollike(Function *fn, Ref r)
{
   Instr *ins;
   if (r.t == RICON && (r.i == 0 || r.i == 1)) return 1;
   if (r.t != RTMP) return 0;
   ins = &instrtab[r.i];
   if (oiscmp(ins->op)) /* these instrs already have output range of [0,1] */
      return 1;
   if (ins->op == Ophi) { /* check if all the phi args are boollike */
      Block *blk;
      Ref *phi = NULL;
      for (blk = fn->curblk;; blk = blk->lprev) {
         /* find blk that defines phi */
         assert(blk != fn->entry);
         for (int i = 0; i < blk->phi.n; ++i){
            if (blk->phi.p[i] == r.i) {
               phi = phitab.p[ins->l.i];
               goto Found;
            }
         }
      }
   Found:
      for (int i = 0; i < blk->npred; ++i) {
         if (!isboollike(fn, phi[i])) {
            return 0;
         }
      }
      return 1;
   }
   if (in_range(ins->op, Oand, Oxor))
      return isboollike(fn, ins->l) && isboollike(fn, ins->r);
   if (ins->op == Ocopy || in_range(ins->op, Oexts8, Oextu32))
      return isboollike(fn, ins->l);
   if (ins->op == Oparam)
      return typedata[fn->fnty.dat].param[ins->l.i].t == TYBOOL;
   return 0;
}

Ref
scalarcvt(Function *fn, Type to, Type from, Ref ref)
{
   enum irclass kto = type2cls[scalartypet(to)], kfrom = type2cls[scalartypet(from)];
   enum op op;
   if (to.bits == from.bits) return ref;
   assert(kto && kfrom);
   if (kto == kfrom && to.t != TYBOOL) return ref;

   if (kisflt(kto) || kisflt(kfrom)) {
      if (ref.t == RICON) {
         assert(kisflt(kto) && kisint(kfrom));
         return mkfltcon(kto, kto == KF32 ? (float)ref.i : (double)ref.i);
      }
      if (kisflt(kto) && kfrom == KI32) op = issigned(from) ? Ocvts32f : Ocvtu32f;
      else if (to.t == TYBOOL && kisflt(kfrom)) return irbinop(fn, Oneq, kfrom, ref, mkfltcon(kfrom, 0.0));
      else if (kisflt(kto) && kfrom == KI64) op = issigned(from) ? Ocvts64f : Ocvtu64f;
      else if (kto == KF64 && kfrom == KF32) op = Ocvtf32f64;
      else if (kto == KF32 && kfrom == KF64) op = Ocvtf64f32;
      else if (kfrom == KF32) op = issigned(to) ? Ocvtf32s : Ocvtf32u;
      else if (kfrom == KF64) op = issigned(to) ? Ocvtf64s : Ocvtf64u;
      else assert(0);
   } else {
      if (to.t == TYBOOL) {
         if (from.t == TYBOOL) return ref;
         if (isboollike(fn, ref))
            return kfrom == KI32 ? ref : scalarcvt(fn, mktype(TYINT), from, ref);
         return irbinop(fn, Oneq, kfrom, ref, ZEROREF);
      }
      else if (kfrom == KI32 && issigned(from)) op = Oexts32;
      else if (kfrom == KI32) op = Oextu32;
      else if (kto == KI32 && isintcon(ref))
         return issigned(to) ? mkintcon(kto, (int)intconval(ref)) : mkintcon(kto, (uint)intconval(ref));
      else op = Ocopy;
   }
   return irunop(fn, op, kto, ref);
}

static Ref
narrow(Function *fn, enum irclass to, Type t, Ref ref, uint bitsiz)
{
   enum typetag tt = scalartypet(t);
   assert(isscalar(t));
   if (targ_primsizes[tt] < cls2siz[to]) {
      enum op op;
      if (isfltt(tt)) {
         assert(to == KF32 && tt >= TYDOUBLE);
         op = Ocvtf64f32;
      } else {
         static const enum op ext[5][2] = {
            [1] = {Oextu8, Oexts8}, [2] = {Oextu16, Oexts16}, [4] = {Oextu32, Oexts32}
         };
         op = ext[targ_primsizes[tt]][issignedt(tt)];
      }
      ref = irunop(fn, op, to, ref);
   }
   if (bitsiz) {
      assert(kisint(to) && isintt(tt) && bitsiz < 8*targ_primsizes[tt]);
      if (!issignedt(tt)) {
         ref = irbinop(fn, Oand, to, ref, mkintcon(to, (1ull<<bitsiz)-1));
      } else {
         uint sh = 8*cls2siz[to] - bitsiz;
         ref = irbinop(fn, Oshl, to, ref, mkref(RICON, sh));
         ref = irbinop(fn, Osar, to, ref, mkref(RICON, sh));
      }
   }
   return ref;
}

Ref
genptroff(Function *fn, enum op op, uint siz, Ref ptr,
          Type t, Ref idx)
{
   uint cls = type2cls[targ_sizetype];
   Ref off;
   assert(siz);

   idx = scalarcvt(fn, mktype(targ_sizetype), t, idx);
   if (siz == 1) off = idx;
   else if (idx.t == RICON) {
      if (op == Osub) op = Oadd, idx.i = -idx.i;
      off = mkintcon(cls, idx.i * (int)siz);
   } else {
      off = irbinop(fn, Omul, cls, idx, mkintcon(cls, siz));
   }
   assert(in_range(op, Oadd, Osub));
   return irbinop(fn, op, KPTR, ptr, off);
}

Ref
genptrdiff(Function *fn, uint siz, Ref a, Ref b)
{
   uint cls = type2cls[targ_ptrdifftype];
   assert(siz > 0);
   a = irbinop(fn, Osub, cls, a, b);
   if (siz == 1) return a;
   else if (ispo2(siz))
      return irbinop(fn, Osar, cls, a, mkintcon(cls, ilog2(siz)));
   else
      return irbinop(fn, Odiv, cls, a, mkintcon(cls, siz));
}

/* used to emit the jumps in an in if (), while (), etc condition */
static void
condjump(Function *fn, const Expr *ex, Block *tr, Block *fl)
{
   Block *next, *next2;
Recur:
   for (; ex->t == ESEQ; ex = &ex->sub[1])
      expreffects(fn, &ex->sub[0]);
   if (ex->t == ELOGAND) {
      next = newblk(fn);
      condjump(fn, &ex->sub[0], next, fl);
      useblk(fn, next);
      ex = &ex->sub[1];
      goto Recur;
   } else if (ex->t == ELOGIOR) {
      next = newblk(fn);
      condjump(fn, &ex->sub[0], tr, next);
      useblk(fn, next);
      ex = &ex->sub[1];
      goto Recur;
   } else if (ex->t == ECOND) {
      next = newblk(fn);
      next2 = newblk(fn);
      condjump(fn, &ex->sub[0], next, next2);
      useblk(fn, next);
      condjump(fn, &ex->sub[1], tr, fl);
      useblk(fn, next2);
      condjump(fn, &ex->sub[2], tr, fl);
   } else if (ex->t == ELOGNOT) {
   Negate:
      /* swap tr,fl */
      next = tr;
      tr = fl;
      fl = next;
      ex = &ex->sub[0];
      goto Recur;
   } else if (ex->t == EEQU && isnullpo(&ex->sub[1])) { /* == 0 */
      goto Negate;
   } else if (ex->t == ENEQ && isnullpo(&ex->sub[1])) { /* != 0 */
      ex = &ex->sub[0];
      goto Recur;
   } else {
      putcondbranch(fn, exprvalue(fn, ex), tr, fl);
   }
}

typedef struct {
   Type typ;
   vec_of(Ref) refs;
} CondPhi;

static void
condexprrec(Function *fn, const Expr *ex, CondPhi *phi, Block *end)
{
Recur:
   for (; ex->t == ESEQ; ex = &ex->sub[1])
      expreffects(fn, &ex->sub[0]);
   int prevpred = end->npred;
   if (ex->t == ELOGAND) {
      Block *tr = newblk(fn);
      condjump(fn, &ex->sub[0], tr, end);
      assert(prevpred <= end->npred);
      if (phi) for (int n = end->npred - prevpred; n > 0; --n) {
         vpush(&phi->refs, mkref(RICON, 0));
      }
      useblk(fn, tr);
      ex = &ex->sub[1];
      goto Recur;
   } else if (ex->t == ELOGIOR) {
      Block *fl = newblk(fn);
      condjump(fn, &ex->sub[0], end, fl);
      assert(prevpred <= end->npred);
      if (phi) for (int n = end->npred - prevpred; n > 0; --n) {
         vpush(&phi->refs, mkref(RICON, 1));
      }
      useblk(fn, fl);
      ex = &ex->sub[1];
      goto Recur;
   } else if (ex->t == ECOND) {
      Block *tr = newblk(fn), *fl = newblk(fn);
      condjump(fn, &ex->sub[0], tr, fl);
      useblk(fn, tr);
      condexprrec(fn, &ex->sub[1], phi, end);
      useblk(fn, fl);
      ex = &ex->sub[2];
      goto Recur;
   } else {
      if (!phi) {
         expreffects(fn, ex);
      } else {
         Ref val = exprvalue(fn, ex);
         if (isscalar(phi->typ))
            val = scalarcvt(fn, phi->typ, ex->ty, val);
         else assert(ex->ty.bits == phi->typ.bits);
         vpush(&phi->refs, val);
      }
      if (fn->curblk)
         putbranch(fn, end);
   }
}

/* the naive way to generate something like a ? b : c ? d : e, uses multiple phis,
 * this code reduces such nested conditional expressions into one phi */
static Ref
condexprvalue(Function *fn, const Expr *ex, bool discard)
{
   Ref refbuf[8];
   CondPhi phi = { ex->t == ECOND ? ex->ty : mktype(TYBOOL), VINIT(refbuf, countof(refbuf)) };
   Block *dst = newblk(fn);
   condexprrec(fn, ex, discard ? NULL : &phi, dst);
   useblk(fn, dst);
   if (discard) return NOREF;
   enum irclass k;
   if (isscalar(ex->ty)) {
      k = type2cls[scalartypet(ex->ty)];
      assert(k);
   } else {
      assert(isagg(ex->ty) || isptrcvt(ex->ty));
      k = KPTR;
   }
   Ref r = addphi(fn, k, phi.refs.p);
   vfree(&phi.refs);
   return r;
}

static Ref
compilecall(Function *fn, const Expr *ex)
{
   Instr ins = {0};
   Expr *sub = ex->sub;
   const TypeData *td = &typedata[sub[0].ty.dat];
   Instr insnsbuf[10];
   vec_of(Instr) insns = VINIT(insnsbuf, countof(insnsbuf));

   if (sub[0].t == ESYM && declsbuf.p[sub[0].decl].isbuiltin) {
      return declsbuf.p[sub[0].decl].builtin->comp(fn, (Expr *)ex, 0);
   }
   ins.op = Ocall;
   if (isagg(ex->ty)) {
      ins.cls = KPTR;
   } else {
      assert(isscalar(ex->ty) || ex->ty.t == TYVOID);
      ins.cls = type2cls[scalartypet(ex->ty)];
      assert(ins.cls || ex->ty.t == TYVOID);
   }
   ins.l = exprvalue(fn, &sub[0]);
   for (int i = 0; i < ex->narg; ++i) {
      Expr *arg = &sub[i+1];
      Type ty = i < td->nmemb ? td->param[i] : argpromote(arg->ty);
      Ref r = scalarcvt(fn, ty, typedecay(arg->ty), exprvalue(fn, arg));
      vpush(&insns, mkarginstr(mkirtype(ty), r));
   }
   for (int i = 0; i < insns.n; ++i)
      addinstr(fn, insns.p[i]);
   vfree(&insns);
   ins.r = mkcallarg(mkirtype(ex->ty), ex->narg, td->variadic ? td->nmemb : td->kandr ? 0 : -1);
   Ref r = addinstr(fn, ins);
   if (sub[0].t == ESYM && declsbuf.p[sub[0].decl].noret) /* trap if noreturn func returns */
      puttrap(fn);
   return r;
}

static Ref
genbitfload(Function *fn, Ref *tmpval, const Type ty, Ref *addr,
            const ExprGetFld *fld, bool volatyl)
{
   enum irclass k = type2cls[scalartypet(ty)];
   uint off = fld->off, bitsiz = fld->bitsiz, bitoff = fld->bitoff;
   Ref tmp;
   uvlong mask;

   assert(k);
   *addr = irbinop(fn, Oadd, KPTR, *addr, mkintcon(KI32, off));
   tmp = genload(fn, ty, *addr, volatyl);
   if (tmpval) *tmpval = tmp;
   if (!issigned(ty)) {
      /* shift right and mask */
      tmp = irbinop(fn, Oslr, k, tmp, mkref(RICON, bitoff));
      if (bitsiz < 8*typesize(ty)) {
         mask = bitsiz == 64 ? -1ull : (1ull << bitsiz) - 1;
         tmp = irbinop(fn, Oand, k, tmp, mkintcon(k, mask));
      }
   } else {
      /* shift left and shift right arithmetic to propagate sign bit */
      int sh = 8*cls2siz[k] - bitsiz - bitoff;
      tmp = irbinop(fn, Oshl, k, tmp, mkref(RICON, sh));
      sh += bitoff;
      tmp = irbinop(fn, Osar, k, tmp, mkref(RICON, sh));
   }
   return tmp;
}

static void
genbitfstore(Function *fn, const Type ty, Ref addr,
             const ExprGetFld *fld, Ref tmp, Ref val)
{
   enum irclass k = type2cls[scalartypet(ty)];
   uint off = fld->off, bitsiz = fld->bitsiz, bitoff = fld->bitoff;
   uint bittypesize = 8*typesize(ty);
   uvlong mask;

   assert(k);
   if (!tmp.bits) {
      addr = irbinop(fn, Oadd, KPTR, addr, mkintcon(KPTR, off));
      tmp = genload(fn, ty, addr, 0);
   }
   mask = (bitsiz == 64 ? -1ull : (1ull << bitsiz) - 1) << bitoff;

   /* mask out bits in existing container */
   tmp = irbinop(fn, Oand, k, tmp, mkintcon(k, ~mask));

   /* shift and mask source value */
   if (isintcon(val)) {
      val = mkintcon(k, ((uvlong)intconval(val) << bitoff) & mask);
   } else {
      val = irbinop(fn, Oshl, k, val, mkref(RICON, bitoff));
      if (bitsiz < bittypesize)
         val = irbinop(fn, Oand, k, val, mkintcon(k, mask));
   }
   /* combine and write */
   if (bitsiz < bittypesize)
      val = irbinop(fn, Oior, k, tmp, val);
   genstore(fn, ty, addr, val);
}

static bool
knowntruthy(bool *t, Expr *ex)
{
   if (!eval(ex, EVFOLD)) return 0;

   switch (ex->t) {
   default: assert(0 && "!scalar?");
   case ENUMLIT:
      *t = isflt(ex->ty) ? ex->f != 0.0 : ex->u != 0;
      break;
   case ESTRLIT: case ESSYMREF:
      /* string literals & symbol addresses are always truthy */
      *t = 1;
      break;
   }
   return 1;
}

Ref
compileexpr(Function *fn, const Expr *ex, bool discard)
{
   Type ty;
   Ref l, r, q, adr;
   uint bitsiz;
   enum op op;
   enum irclass cls = type2cls[scalartypet(ex->ty)];
   int swp = 0;
   Expr *sub;

   //eval((Expr *)ex, EVFOLD);
   sub = ex->sub;

   if (ex->ty.t != TYVOID && !isscalar(ex->ty)) {
      /* fn & array designators evaluate to their address;
       * so do aggregates for the purpose of code generation */
      if (isagg(ex->ty) && isincomplete(ex->ty))
         error(&ex->span, "use of incomplete type '%ty'", ex->ty);
      return expraddr(fn, ex);
   }
   switch (ex->t) {
   case ENUMLIT:
      if (discard) return NOREF;
      if (isflt(ex->ty))
         return mkfltcon(cls, ex->f);
      return mkintcon(cls, ex->i);
   case ESYM:
      if (discard && !(ex->qual & QVOLATILE)) return NOREF;
      return genload(fn, ex->ty, expraddr(fn, ex), ex->qual & QVOLATILE);
   case ESSYMREF:
      return expraddr(fn, ex);
   case EVAARG:
      return builtin_va_arg_comp(fn, ex, discard);
   case EGETF:
      if (discard && !(ex->qual & QVOLATILE)) return NOREF;
      if (ex->fld.bitsiz) {
         /* bit-field */
         r = expraddr(fn, ex->sub);
         return genbitfload(fn, NULL, ex->ty, &r, &ex->fld, ex->qual & QVOLATILE);
      }
      return genload(fn, ex->ty, expraddr(fn, ex), ex->qual & QVOLATILE);
   case ECAST:
      if (ex->ty.t == TYVOID) {
         expreffects(fn, sub);
         return NOREF;
      }
      /* fallthru */
   case EPLUS:
      r = compileexpr(fn, sub, discard);
      if (discard) return NOREF;
      r = scalarcvt(fn, ex->ty, sub->ty, r);
      if (isint(ex->ty) && (typesize(ex->ty) < typesize(sub->ty) || issigned(ex->ty) != issigned(sub->ty)))
         return narrow(fn, type2cls[scalartypet(ex->ty)], ex->ty, r, 0);
      return r;
   case ENEG:
      op = Oneg;
      goto Unary;
   case ECOMPL:
      op = Onot;
   Unary:
      l = compileexpr(fn, sub, discard);
      if (discard) return NOREF;
      l = scalarcvt(fn, ex->ty, sub->ty, l);
      return irunop(fn, op, cls, l);
   case ELOGNOT:
      for (; sub->t == ELOGNOT; ex = sub, sub = sub->sub)
         swp ^= 1;
      op = Oequ + swp;
      l = compileexpr(fn, sub, discard);
      if (discard) return NOREF;
      l = scalarcvt(fn, ex->ty, sub->ty, l);
      r = mkintcon(cls, 0);
      return irbinop(fn, op, cls, l, r);
   case EDEREF:
      discard &= (ex->qual & QVOLATILE) == 0;
      r = compileexpr(fn, sub, discard);
      if (discard) return NOREF;
      return genload(fn, ex->ty, r, ex->qual & QVOLATILE);
   case EADDROF:
      return expraddr(fn, sub);
   case EMUL:
      op = Omul;
      goto BinArith;
   case EDIV:
      op = isunsigned(ex->ty) ? Oudiv : Odiv;
      goto BinArith;
   case EREM:
      op = issigned(ex->ty) ? Orem : Ourem;
      goto BinArith;
   case EBAND:
      op = Oand;
      goto BinArith;
   case EXOR:
      op = Oxor;
      goto BinArith;
   case EBIOR:
      op = Oior;
      goto BinArith;
   case ESHL:
      op = Oshl;
      goto BinArith;
   case ESHR:
      op = issigned(ex->ty) ? Osar : Oslr;
      goto BinArith;
   case ESUB:
      op = Osub;
      goto BinArith;
   case EADD:
      op = Oadd;
   BinArith:
      l = compileexpr(fn, &sub[0], discard);
      r = compileexpr(fn, &sub[1], discard);
      if (discard) return NOREF;
      if (op == Osub && isptrcvt(sub[0].ty) && isptrcvt(sub[1].ty)) {
         /* ptr - ptr */
         return genptrdiff(fn, typesize(typechild(sub[0].ty)), l, r);
      } else if ((op != Oadd && op != Osub) || cls != KPTR) {
         /* num OP num */
         l = scalarcvt(fn, ex->ty, sub[0].ty, l);
         r = scalarcvt(fn, ex->ty, sub[1].ty, r);
      } else {
         assert(isptrcvt(sub[0].ty));
         /* ptr +/- num */
         return genptroff(fn, op, typesize(typechild(sub[0].ty)), l, sub[1].ty, r);
      }
      return irbinop(fn, op, cls, l, r);
   case EPOSTINC:
   case EPOSTDEC:
      op = ex->t == EPOSTINC ? Oadd : Osub;
      if (ex->ty.t == TYPTR)
         r = mkintcon(type2cls[targ_sizetype], typesize(typechild(ex->ty)));
      else
         r = isflt(ex->ty) ? mkfltcon(type2cls[ex->ty.t], 1.0) : mkref(RICON, 1);
      bitsiz = 0;
      if (sub[0].t == EGETF && (bitsiz = sub->fld.bitsiz)) {
         Ref tmp;
         adr = expraddr(fn, &sub[0].sub[0]);
         l = genbitfload(fn, &tmp, sub[0].ty, &adr, &sub[0].fld, sub[0].qual & QVOLATILE);
         q = irbinop(fn, op, cls, l, r);
         genbitfstore(fn, sub[0].ty, adr, &sub[0].fld, tmp, q);
      } else {
         adr = expraddr(fn, sub);
         l = genload(fn, sub->ty, adr, sub->qual & QVOLATILE);
         q = irbinop(fn, op, cls, l, r);
         genstore(fn, sub->ty, adr, q);
      }
      return discard ? NOREF : l;
   case EPREINC:
   case EPREDEC:
      op = ex->t == EPREINC ? Oadd : Osub;
      if (ex->ty.t == TYPTR)
         r = mkintcon(type2cls[targ_sizetype], typesize(typechild(ex->ty)));
      else
         r = isflt(ex->ty) ? mkfltcon(type2cls[ex->ty.t], 1.0) : mkref(RICON, 1);
      if (sub[0].t == EGETF && (bitsiz = sub->fld.bitsiz)) {
         ty = ex->ty;
         goto CompoundBitf;
      }
      adr = expraddr(fn, sub);
      l = genload(fn, sub->ty, adr, sub->qual & QVOLATILE);
      q = irbinop(fn, op, cls, l, r);
      genstore(fn, sub->ty, adr, q);
      if (discard) return NOREF;
      return narrow(fn, cls, ex->ty, q, 0);
   case EEQU:
      op = Oequ;
      goto Cmp;
   case ENEQ:
      op = Oneq;
      goto Cmp;
   case ELTH:
      op = Olth;
      goto Cmp;
   case ELTE:
      op = Olte;
      goto Cmp;
   case EGTH:
      op = Ogth;
      goto Cmp;
   case EGTE:
      op = Ogte;
   Cmp:
      ty = cvtarith(sub[0].ty, sub[1].ty);
      if (!ty.t) ty.t = TYPTR;
      if (isunsigned(ty) && in_range(op, Olth, Ogte))
         op += Oulth - Olth;
      l = compileexpr(fn, &sub[0], discard);
      r = compileexpr(fn, &sub[1], discard);
      if (discard) return NOREF;
      l = scalarcvt(fn, ty, sub[0].ty, l);
      r = scalarcvt(fn, ty, sub[1].ty, r);
      cls = type2cls[ty.t];
      return irbinop(fn, op, cls, l, r);
   case ESET:
      assert(isscalar(ex->ty));
      q = scalarcvt(fn, sub[0].ty, sub[1].ty, exprvalue(fn, &sub[1]));
      if (sub[0].t == EGETF && (bitsiz = sub[0].fld.bitsiz)) {
         /* bit-field */
         adr = expraddr(fn, &sub[0].sub[0]);
         genbitfstore(fn, ex->ty, adr, &sub[0].fld, NOREF, q);
      } else {
         bitsiz = 0;
         adr = expraddr(fn, &sub[0]);
         genstore(fn, ex->ty, adr, q);
      }
      if (discard) return NOREF;
      return bitsiz ? narrow(fn, cls, sub[0].ty, q, bitsiz) : q;
   case ESETMUL:
      op = Omul;
      goto Compound;
   case ESETDIV:
      op = isunsigned(ex->ty) ? Oudiv : Odiv;
      goto Compound;
   case ESETREM:
      op = issigned(ex->ty) ? Orem : Ourem;
      goto Compound;
   case ESETAND:
      op = Oand;
      goto Compound;
   case ESETXOR:
      op = Oxor;
      goto Compound;
   case ESETIOR:
      op = Oior;
      goto Compound;
   case ESETSHL:
      op = Oshl;
      goto Compound;
   case ESETSHR:
      op = issigned(ex->ty) ? Osar : Oslr;
      goto Compound;
   case ESETSUB:
      op = Osub;
      goto Compound;
   case ESETADD:
      op = Oadd;
   Compound:
      ty = in_range(ex->t, ESETSHL, ESETSHR) ? mktype(intpromote(ex->ty.t))
                                             : cvtarith(sub[0].ty, sub[1].ty);
      r = exprvalue(fn, &sub[1]);
      if (sub[0].t == EGETF && (bitsiz = sub[0].fld.bitsiz)) {
         /* bit-field */
         Ref tmp;
      CompoundBitf:
         adr = expraddr(fn, &sub[0].sub[0]);
         l = genbitfload(fn, &tmp, sub[0].ty, &adr, &sub[0].fld, sub[0].qual & QVOLATILE);
         q = irbinop(fn, op, cls, l, r);
         genbitfstore(fn, sub[0].ty, adr, &sub[0].fld, tmp, q);
      } else {
         bitsiz = 0;
         adr = expraddr(fn, &sub[0]);
         l = genload(fn, ex->ty, adr, ex->qual & QVOLATILE);
         if ((op != Oadd && op != Osub) || cls != KPTR) {
            l = scalarcvt(fn, ty, sub[0].ty, l);
            r = scalarcvt(fn, ty, sub[1].ty, r);
            q = irbinop(fn, op, type2cls[ty.t], l, r);
            q = scalarcvt(fn, ex->ty, ty, q);
         } else {
            q = genptroff(fn, op, typesize(typechild(ex->ty)), l, sub[1].ty, r);
         }
         genstore(fn, ex->ty, adr, q);
      }
      if (discard) return NOREF;
      return bitsiz ? narrow(fn, cls, ex->ty, q, bitsiz) : q;
   case ECALL:
      r = compilecall(fn, ex);
      if (isint(ex->ty))
         return narrow(fn, cls, ex->ty, r, 0);
      return r;
   case ECOND:
      for (bool c; knowntruthy(&c, &ex->sub[0]);) {
         r = compileexpr(fn, &ex->sub[2-c], discard);
         if (discard) return NOREF;
         return scalarcvt(fn, ex->ty, ex->sub[2-c].ty, r);
      }

      if (ex->ty.t == TYVOID || discard) {
         Block *tr, *fl, *end;
         condjump(fn, &sub[0], tr = newblk(fn), fl = newblk(fn));
         useblk(fn, tr);
         expreffects(fn, &sub[1]);
         end = newblk(fn);
         if (fn->curblk)
            putbranch(fn, end);
         useblk(fn, fl);
         expreffects(fn, &sub[2]);
         if (fn->curblk)
            putbranch(fn, end);
         useblk(fn, end);
         return NOREF;
      }
      return condexprvalue(fn, ex, discard);
   case ELOGAND:
   case ELOGIOR:
      for (bool c; knowntruthy(&c, &ex->sub[0]);) {
         c ^= ex->t == ELOGIOR;
         r = compileexpr(fn, &ex->sub[c], discard);
         if (discard) return NOREF;
         return scalarcvt(fn, mktype(TYBOOL), ex->sub[c].ty, r);
      }
      return condexprvalue(fn, ex, discard);
   case ESEQ:
      expreffects(fn, &sub[0]);
      return compileexpr(fn, &sub[1], discard);
   default: assert(!"nyi expr");
   }
}

/************************************/
/* Statements parsing & compilation */
/************************************/

static void
stmtterm(CComp *cm)
{
   expect(cm, ';', "to terminate previous statement");
}

static void block(CComp *cm, Function *fn);
static bool stmt(CComp *cm, Function *fn);
static void localdecl(CComp *cm, Function *fn, bool forinit);

typedef struct Label Label;
struct Label {
   Label *link;
   internstr name;
   Block *blk;
   Span usespan;
   /* if usespan.ex.len == 0, this label is resolved and blk is the block that
    * the label starts, otherwise the label is unresolved and blk is the head
    * of a linked list of relocations, the next list entry is in blk->s1, etc,
    * terminated by NULL */
};

static Label *
findlabel(CComp *cm, internstr name)
{
   for (Label *l = cm->labels; l; l = l->link)
      if (l->name == name) return l;
   return NULL;
}

static void
deflabel(CComp *cm, Function *fn, const Span *span, internstr name)
{
   Label *label = findlabel(cm, name);
   if (label && label->usespan.ex.len == 0) {
      error(span, "redefinition of label '%s'", name);
   } else if (label) {
      Block *new = NULL;
      if (!nerror) {
         new = newblk(fn);
         if (fn->curblk) putbranch(fn, new);
      }
      /* fix up relocations */
      for (Block *list = label->blk, *next; list; list = next) {
         next = list->s1;
         if (!nerror) {
            useblk(fn, list);
            putbranch(fn, new);
         }
      }
      label->usespan = (Span){0};
      label->blk = new;
      if (!nerror) useblk(fn, new);
   } else {
      Label l = { cm->labels, name };
      if (!nerror) {
         Block *new = newblk(fn);
         if (fn->curblk) putbranch(fn, new);
         useblk(fn, new);
      }
      l.blk = fn->curblk;
      cm->labels = alloccopy(fn->arena, &l, sizeof l, 0);
   }
}

static bool
loopbody(CComp *cm, Function *fn, Block *brk, Block *cont)
{
   Block *save[2];
   bool terminates = 0;

   save[0] = cm->breakto, save[1] = cm->loopcont;
   cm->breakto = brk, cm->loopcont = cont;
   ++cm->loopdepth;

   terminates = stmt(cm, fn);

   --cm->loopdepth;
   cm->breakto = save[0], cm->loopcont = save[1];

   return terminates;
}

#define EMITS if (doemit && !nerror)

typedef struct {
   vlong val;
   Block *blk;
   Span span;
} SwitchCase;
typedef struct SwitchStmt {
   Block *bdefault;
   Type condtype;
   vec_of(SwitchCase) cases;
} SwitchStmt;

static int
cmpswcase(const void *aa, const void *bb)
{
   const SwitchCase *a = aa, *b = bb;
   vlong v1 = a->val, v2 = b->val;
   if (v1 != v2) return v1 < v2 ? -1 : 1;
   return (a > b) - (a < b); /* preserve original order */
}

static void
swsortcases(SwitchCase *cs, uint n)
{
   void qsort(void *, size_t n, size_t size, int (*)(const void *, const void *));
   qsort(cs, n, sizeof *cs, cmpswcase);
}

static bool
genswitch(CComp *cm, Function *fn, const Expr *ex)
{
   Ref sel;
   bool doemit = fn->curblk;
   Block *begin = NULL, *end = NULL, *breaksave = cm->breakto;
   SwitchStmt *stsave = cm->switchstmt, st = {.condtype = ex->ty};
   enum irclass k = type2cls[scalartypet(ex->ty)];
   SwitchCase casebuf[8];
   vinit(&st.cases, casebuf, countof(casebuf));

   assert(k);
   end = newblk(fn);
   EMITS {
      sel = exprvalue(fn, ex);
      assert(isint(ex->ty));
   }
   cm->switchstmt = &st;
   cm->breakto = end;
   begin = fn->curblk;
   fn->curblk = NULL;
   ++cm->switchdepth;
   stmt(cm, fn);
   --cm->switchdepth;
   doemit = fn->curblk;
   cm->switchstmt = stsave;
   cm->breakto = breaksave;

   EMITS putbranch(fn, end);
   useblk(fn, begin);
   swsortcases(st.cases.p, st.cases.n);
   doemit = 1;
   if (!st.bdefault) st.bdefault = end;
   /* TODO: optimize instead of generating the equivalent of if == .. else if .. chain
    *    XX 1. sort by case values (also for easy duplicates checking)
    *       2. contiguous ranges (case a..b:  ->  x >= && x <= b)
    *       3. binary search
    *       4. jump tables? (harder, backend refactoring)
    */
   vlong prev;
   for (int i = 0; i < st.cases.n; ++i) {
      const SwitchCase *c = &st.cases.p[i];
      if (i > 0) {
         assert(c->val >= prev);
         if (c->val == prev) {
            error(&c->span, "duplicate case value");
            note(DGERROR, &c[-1].span, "previously defined here");
         }
      }
      EMITS {
         Block *next = i < st.cases.n - 1 ? newblk(fn) : st.bdefault;
         putcondbranch(fn, irbinop(fn, Oequ, k, sel, mkintcon(k, c->val)), c->blk, next);
         if (next != st.bdefault) useblk(fn, next);
      }
      prev = c->val;
   }
   vfree(&st.cases);
   if (fn->curblk != end) {
      if (fn->curblk) EMITS putbranch(fn, end);
      if (end->npred > 0) {
         useblk(fn, end);
      } else {
         fn->curblk = NULL;
         freeblk(fn, end);
      }
   }

   return fn->curblk == NULL;
}

static bool /* return 1 if stmt is terminating (ends with a jump) */
stmt(CComp *cm, Function *fn)
{
   Block *tr, *fl, *end, *begin;
   union {
      Arena a;
      char mem[sizeof(Arena) + sizeof(Expr)*4];
   } atmp = { .a.cap = sizeof(Expr)*4 };
   Arena *atmpp;
   Expr ex;
   Env e;
   Ref r;
   Token tk;
   bool terminates = 0;
   bool doemit = fn->curblk;

   while (match(cm, &tk, TKIDENT) || match(cm, &tk, TKWcase) || match(cm, &tk, TKWdefault)) {
      if (tk.t == TKWcase) {
         /* case <expr> ':' */
         if (!cm->switchstmt) error(&tk.span, "'case' outside of switch statement");
         ex = constantexpr(cm);
         if (!eval(&ex, EVINTCONST))
            error(&ex.span, "not an integer constant expression");
         else if (cm->switchstmt && ex.ty.bits != cm->switchstmt->condtype.bits) {
            Expr tmp = ex;
            ex = mkexpr(ECAST, ex.span, cm->switchstmt->condtype, .sub = &tmp);
            bool ok = eval(&ex, EVINTCONST);
            assert(ok && "cast const int?");
            if (ex.i != tmp.i)
               warn(&ex.span, "overflow converting case value to switch condition type");
         }
         expect(cm, ':', NULL);
         if (!fn->curblk || (fn->curblk->phi.n > 0 || fn->curblk->ins.n > 0)) {
            begin = newblk(fn);
            EMITS putbranch(fn, begin);
            useblk(fn, begin);
         }
         if (cm->switchstmt)
            vpush(&cm->switchstmt->cases, ((SwitchCase) {ex.i, fn->curblk, ex.span}));
      } else if (tk.t == TKWdefault) {
         /* default ':' */
         if (!cm->switchstmt) error(&tk.span, "'default' outside of switch statement");
         expect(cm, ':', NULL);
         if (!fn->curblk || (fn->curblk->phi.n > 0 || fn->curblk->ins.n > 0)) {
            begin = newblk(fn);
            EMITS putbranch(fn, begin);
            useblk(fn, begin);
         }
         if (cm->switchstmt) {
            if (cm->switchstmt->bdefault) error(&tk.span, "multiple 'default' labels in one switch");
            cm->switchstmt->bdefault = fn->curblk;
         }
      } else if (tk.t == TKIDENT && match(cm, NULL, ':')) {
         /* <label> ':' */
         deflabel(cm, fn, &tk.span, tk.name);
      } else {
         assert(tk.t == TKIDENT);
         /* kludge for no backtracking and no lookahead */
         ex = exprparse(cm, 1, &tk, EFROMSTMT);
         stmtterm(cm);
         EMITS expreffects(fn, &ex);
         return fn->curblk == NULL;
      }
      doemit = 1;
   }

   switch (peek(cm, NULL)) {
   case '{':
      lex(cm, NULL);
      envdown(cm, &e);
      block(cm, fn);
      envup(cm);
      break;
   case ';':
      lex(cm, NULL);
      break;
   case TKWif:
      lex(cm, NULL);
      expect(cm, '(', NULL);
      ex = commaexpr(cm);
      expect(cm, ')', NULL);
      if (!isscalar(ex.ty))
         error(&ex.span, "'if' condition is not a scalar '%ty'", ex.ty);
      tr = fl = end = NULL;
      EMITS {
         tr = newblk(fn);
         fl = newblk(fn);
         condjump(fn, &ex, tr, fl);
         useblk(fn, tr);
      }
      terminates = stmt(cm, fn);
      if (!match(cm, NULL, TKWelse)) {
         end = fl;
         EMITS if (!terminates) putbranch(fn, end);
         terminates = 0;
      } else {
         EMITS {
            end = newblk(fn);
            if (!terminates) putbranch(fn, end);
            useblk(fn, fl);
         }
         terminates &= stmt(cm, fn);
         EMITS {
            if (fn->curblk) putbranch(fn, end);
         }
      }
      EMITS if (!terminates) useblk(fn, end);
      break;
   case TKWelse:
      lex(cm, &tk);
      error(&tk.span, "'else' without matching 'if'");
      break;
   case TKWwhile: /* while ( <cond> ) <body> */
      lex(cm, NULL);
      expect(cm, '(', NULL);
      ex = commaexpr(cm);
      expect(cm, ')', NULL);
      if (!isscalar(ex.ty))
         error(&ex.span, "'while' condition is not a scalar '%ty'", ex.ty);
      tr = begin = end = NULL;
      /* @begin:
       *   <cond>
       *   b <cond>, @tr, @end
       * @tr:
       *   <body>
       *   b @begin
       * @end:
       *   <-
       */
      doemit = 1;
      EMITS {
         begin = newblk(fn);
         if (fn->curblk)
            putbranch(fn, begin);
         useblk(fn, begin);
         condjump(fn, &ex, tr = newblk(fn), end = newblk(fn));
         useblk(fn, tr);
      }
      terminates = loopbody(cm, fn, end, begin);
      EMITS {
         if (fn->curblk) putbranch(fn, begin);
         useblk(fn, end);
      }
      break;
   case TKWdo: /* do <body> while ( <cond> ) ; */
      lex(cm, NULL);
      begin = tr = end = NULL;
      /* @begin:
       *   <body>
       *   b @tr
       * @tr:  <- necessary for continue stmt
       *   <cond>
       *    b <cond>, @begin, @end
       * @end:
       *   <-
       */
      doemit = 1;
      EMITS {
         begin = newblk(fn);
         if (fn->curblk)
            putbranch(fn, begin);
         useblk(fn, begin);
         tr = newblk(fn);
         end = newblk(fn);
      }
      terminates = loopbody(cm, fn, end, tr);
      expect(cm, TKWwhile, NULL);
      expect(cm, '(', NULL);
      ex = commaexpr(cm);
      expect(cm, ')', NULL);
      if (!isscalar(ex.ty))
         error(&ex.span, "'while' condition is not a scalar '%ty'", ex.ty);
      stmtterm(cm);
      EMITS {
         if (!terminates) putbranch(fn, tr);
         useblk(fn, tr);
         condjump(fn, &ex, begin, end);
         useblk(fn, end);
      }
      break;
   case TKWfor: /* for ( <init>? ; <cond>? ; <iter>? ) <body> */
      lex(cm, NULL);
      begin = tr = end = fl = NULL;
      expect(cm, '(', NULL);
      /*   ->
       *   <init>
       *   b @begin
       * @begin:
       *   <cond>
       *   b <cond>, @tr, @fl
       * @tr:
       *   <body>
       *   b @end
       * @end:  <- necessary for continue stmt
       *   <iter>
       *   b @begin
       * @fl:
       *   <-
       *
       * if cond omitted, tr = begin
       * if iter omitted, end = begin
       */
      envdown(cm, &e);
      if (!match(cm, NULL, ';')) { /* init */
         if (isdecltok(cm)) {
            localdecl(cm, fn, 1);
         } else {
            ex = commaexpr(cm);
            EMITS expreffects(fn, &ex);
            expect(cm, ';', NULL);
         }
      }
      doemit = 1;
      EMITS {
         end = tr = begin = newblk(fn);
         if (fn->curblk)
            putbranch(fn, begin);
         useblk(fn, begin);
         fl = newblk(fn);
      }
      if (!match(cm, NULL, ';')) { /* cond */
         ex = commaexpr(cm);
         expect(cm, ';', NULL);
         if (!isscalar(ex.ty))
            error(&ex.span, "'for' condition is not a scalar type ('%ty')", ex.ty);
         EMITS {
            tr = newblk(fn);
            condjump(fn, &ex, tr, fl);
            useblk(fn, tr);
         }
      }
      atmpp = NULL;
      if (!match(cm, NULL, ')')) { /* iter */
         /* since exarena is free'd at the end of each stmt, create a new temporary
          * arena to parse this expression because loop body statements would free it
          * otherwise */
         Arena *tmp = cm->exarena;
         cm->exarena = &atmp.a;
         ex = commaexpr(cm);
         atmpp = cm->exarena;
         cm->exarena = tmp;

         end = newblk(fn);
         expect(cm, ')', NULL);
      }

      terminates = loopbody(cm, fn, fl, end);

      EMITS {
         if (end != begin) { /* have iter */
            if (!terminates) putbranch(fn, end);
            useblk(fn, end);
            expreffects(fn, &ex);
            putbranch(fn, begin);
         } else if (!terminates) putbranch(fn, begin);
         if (fl->npred > 0) {
            useblk(fn, fl);
         } else {
            freeblk(fn, fl);
            terminates = 1;
         }
      }
      if (atmpp && atmpp != cm->exarena) freearena(&atmpp);
      envup(cm);
      break;
   case TKWswitch:
      lex(cm, NULL);
      expect(cm, '(', NULL);
      ex = commaexpr(cm);
      expect(cm, ')', NULL);
      if (!isint(ex.ty))
         error(&ex.span, "'switch' value is not an integer: '%ty'", ex.ty);
      terminates = genswitch(cm, fn, &ex);
      break;
   case TKWbreak:
      lex(cm, &tk);
      if (!cm->loopdepth && !cm->switchdepth)
         error(&tk.span, "'break' outside of loop or switch statement");
      EMITS putbranch(fn, cm->breakto);
      stmtterm(cm);
      break;
   case TKWcontinue:
      lex(cm, &tk);
      if (!cm->loopdepth)
         error(&tk.span, "'continue' outside of loop");
      EMITS putbranch(fn, cm->loopcont);
      stmtterm(cm);
      break;
   case TKWgoto:
      lex(cm, &tk);
      peek(cm, &tk);
      if (expect(cm, TKIDENT, NULL)) {
         Label *label = findlabel(cm, tk.name);
         if (!label) {
            /* create reloc list */
            Label l = { cm->labels, tk.name, fn->curblk, tk.span };
            assert(l.usespan.ex.len);
            cm->labels = alloccopy(fn->arena, &l, sizeof l, 0);
            fn->curblk = NULL;
         } else if (label && label->usespan.ex.len != 0) {
            /* append to relocs list */
            Block *next = label->blk;
            label->blk = fn->curblk;
            EMITS {
               fn->curblk->s1 = next;
               fn->curblk = NULL;
            }
         } else {
            EMITS {
               assert(label->blk);
               putbranch(fn, label->blk);
            }
         }
      }
      stmtterm(cm);
      break;
   case TKWreturn:
      lex(cm, &tk);
      if (isexprtok(cm)) {
         ex = commaexpr(cm);
         if (fn->retty.t == TYVOID) {
            if (ex.ty.t != TYVOID) error(&ex.span, "void function should not return a value");
            else if (ccopt.pedant) warn(&ex.span, "returning void expression is an extension");
         } else if (!assigncheck(fn->retty, &ex)) {
            error(&ex.span,
                  "cannot return '%ty' value from function with return type '%ty'",
                  ex.ty, fn->retty);
         }
         EMITS {
            if (isscalar(fn->retty))
               r = scalarcvt(fn, fn->retty, ex.ty, exprvalue(fn, &ex));
            else if (fn->retty.t == TYVOID)
               r = (expreffects(fn, &ex), NOREF);
            else
               r = structreturn(fn, &ex);
            putreturn(fn, r, NOREF);
         }
      } else {
         if (fn->retty.t != TYVOID)
            error(&tk.span, "non-void function should return a value");
         EMITS putreturn(fn, NOREF, NOREF);
      }
      stmtterm(cm);
      break;
   default:
      ex = exprparse(cm, 1, NULL, EFROMSTMT);
      stmtterm(cm);
      EMITS expreffects(fn, &ex);
      break;
   }
   freearena(&cm->exarena);
   lexerfreetemps(cm->lx);
   return fn->curblk == NULL;
}

/* parse and compile a function-local declaration */
static void
localdecl(CComp *cm, Function *fn, bool forini)
{
   Expr ini;
   Token tk;
   const bool doemit = fn->curblk;
   DeclState st = { DFUNCVAR };

   if (!forini && match(cm, &tk, TKIDENT)) {
      if (match(cm, NULL, ':')) {
         /* <label> ':' */
         deflabel(cm, fn, &tk.span, tk.name);
         stmt(cm, fn);
         return;
      }
      /* finddecl() -> non null because localdecl() is called when isdecltok() */
      st.base = finddecl(cm, tk.name)->ty;
      st.base0 = 1;
   }
   do {
      Decl decl = pdecl(&st, cm);
      if (decl.name) {
         static int staticid;
         bool put = 0;
         bool dynarr = 0;

         switch (decl.scls) {
         case SCSTATIC:
            if (forini)
               error(&decl.span, "static declaration in 'for' loop initializer");
            if (!decl.sym)
               decl.sym = mkhiddensym(&fn->name->c, &decl.name->c, ++staticid);
            goto Initz;
         case SCNONE:
            if (decl.ty.t == TYFUNC) {
               decl.scls = SCEXTERN;
               if (!decl.sym) decl.sym = decl.name;
               break;
            }
            decl.scls = SCAUTO;
            /* fallthru */
         case SCAUTO:
         case SCREGISTER:
            if (decl.ty.t == TYFUNC) {
               error(&decl.span, "declaring variable '%s' with function type '%ty'", decl.name, decl.ty);
               goto Err;
            } else if (isincomplete(decl.ty) && !(dynarr = (decl.ty.t == TYARRAY && st.varini))) {
               error(&decl.span, "declaring variable '%s' with incomplete type '%ty'", decl.name, decl.ty);
               goto Err;
            }
            decl.id = -1;
            if (!nerror) {
               Instr alloc = mkalloca(typesize(decl.ty), typealign(decl.ty));
               if (fn->curblk) decl.id = addinstr(fn, alloc).i;
               else decl.id = insertinstr(fn->entry, fn->entry->ins.n, alloc).i;
            }
         Initz:
            if (st.varini) {
               int d = putdecl(cm, &decl);
               Type ty = decl.ty;
               bool statik = decl.scls & (SCSTATIC | SCEXTERN);
               ini = initializer(cm, &ty, statik ? EVSTATICINI : EVFOLD,
                                 /* globl? */ decl.scls == SCEXTERN, decl.qual, statik ? decl.sym : NULL);
               declsbuf.p[d].ty = ty;
               put = 1;
               pdecl(&st, cm);
               if (!statik) {
                  /* fix alloca for actual size, for implicitly sized arrays */
                  assert(!isincomplete(ty));
                  EMITS instrtab[decl.id] = mkalloca(typesize(ty), typealign(ty));

                  if (!initcheck(ty, &ini)) {
                     Span span = decl.span;
                     joinspan(&span.ex, ini.span.ex);
                     error(&span, "cannot initialize '%ty' variable with '%ty'",
                           ty, ini.ty);
                  }
                  EMITS {
                     if (ini.t == EINIT || (ty.t == TYARRAY && ini.t == ESTRLIT))
                        geninit(fn, ty, mkref(RTMP, decl.id), &ini);
                     else if (isagg(ty))
                        structcopy(fn, ty, mkref(RTMP, decl.id), exprvalue(fn, &ini));
                     else {
                        genstore(fn, ty, mkref(RTMP, decl.id),
                              scalarcvt(fn, ty, ini.ty, exprvalue(fn, &ini)));
                     }
                  }
               } else if (decl.scls == SCEXTERN) {
                  Span span = decl.span;
                  joinspan(&span.ex, ini.span.ex);
                  error(&span, "block local 'extern' variable cannot have an initializer");
               }
            } else if (decl.scls == SCSTATIC) {
               /* zero-initialized static */
               if (decl.ty.t == TYARRAY && isincomplete(decl.ty))
                  error(&decl.span, "definition of variable with array type needs size or initializer");
               else if (isincomplete(decl.ty))
                  error(&decl.span, "definition of static variable with incomplete type");
               else
                  objnewdat(decl.sym, Sbss, 0, typesize(decl.ty), typealign(decl.ty));
            }
            break;
         case SCTYPEDEF:
            if (forini)
               error(&decl.span, "typedef in 'for' loop initializer");
            break;
         case SCEXTERN:
            if (!decl.sym) decl.sym = decl.name;
            if (forini)
               error(&decl.span, "extern declaration in 'for' loop initializer");
            if (st.varini) goto Initz;
            break;
         default: assert(0);
         }
         if (st.funcdef) {
            Span span = decl.span;
            joinspan(&span.ex, (peek(cm, &tk), tk.span.ex));
            error(&span, "function definition not allowed here");
            int bal = 1;
            do switch (lex(cm, NULL)) {
               case TKEOF: break;
               case '{': ++bal; break;
               case '}': --bal; break;
            } while (bal);
         }
      Err:
         if (!put) putdecl(cm, &decl);
      } else if (forini) {
         error(&decl.span, "non-variable declaration in 'for' loop initializer");
      }
   } while (st.more);
}

static void
block(CComp *cm, Function *fn)
{
   Token tk;

   while (!match(cm, &tk, '}')) {
      if (isdecltok(cm))
         localdecl(cm, fn, 0);
      else
         stmt(cm, fn);
   }
   cm->fnblkspan = tk.span;
}

static void
function(CComp *cm, Function *fn, internstr *pnames, const Span *pspans, uchar *pquals)
{
   const TypeData *td = &typedata[fn->fnty.dat];
   const bool doemit = fn->curblk;
   Env e;
   Token tk;
   envdown(cm, &e);

   /* emit Oparam instructions */
   EMITS {
      for (int i = 0; i < td->nmemb; ++i) {
         IRType pty = mkirtype(td->param[i]);
         Ref r = addinstr(fn, mkinstr(Oparam, pty.isagg ? KPTR : pty.cls,
                                            mkref(RICON, i), mktyperef(pty)));
         assert(r.t == RTMP && r.i == i);
      }
   }
   /* add parameters to symbol table and create prologue (arguments) block */
   for (int i = 0; i < td->nmemb; ++i) {
      if (pnames[i]) {
         Decl arg = { .ty = td->param[i], .qual = pquals ? pquals[i] : 0,
                             .name = pnames[i], .scls = SCAUTO, .span = pspans[i] };
         EMITS {
            if (isscalar(arg.ty)) {
               arg.id = addinstr(fn, mkalloca(typesize(arg.ty), typealign(arg.ty))).i;
               genstore(fn, arg.ty, mkref(RTMP, arg.id), mkref(RTMP, i));
            } else {
               arg.id = addinstr(fn, mkinstr(Ocopy, KPTR, mkref(RTMP, i))).i;
            }
         }
         putdecl(cm, &arg);
      } else if (ccopt.cstd < STDC23) {
         warn(&pspans[i], "missing name of parameter #%d", i+1);
      }
   }

   /* put __func__, though its data is generated lazily the first time it is encountered */
   putdecl(cm, &(Decl) {
      .ty = mkarrtype(mktype(TYCHAR), QCONST, strlen(&fn->name->c) + 1), .qual = QCONST,
      .name = istr__func__, .scls = SCSTATIC, .span = (peek(cm, &tk), tk.span),
      .isbuiltin = 1, .sym = fn->name,
   });

   /* end prologue */
   EMITS {
      Block *blk;
      putbranch(fn, blk = newblk(fn));
      useblk(fn, blk);
   }
   cm->labels = NULL;
   block(cm, fn);
   envup(cm);
   for (Label *l = cm->labels; l; l = l->link) {
      if (l->usespan.ex.len) {
         error(&l->usespan, "label '%s' used but never defined", l->name);
      }
   }
   if (fn->curblk) {
      if (fn->retty.t == TYINT && fn->name == istr_main) {
         /* implicit return 0 for main function (ISO C standard behavior) */
         putreturn(fn, ZEROREF, NOREF);
      } else {
         if (fn->retty.t != TYVOID && !nerror) {
            /* it may not actually be reachable after constant-folding
             * peephole optimizations (from code like assert(0 && "x")) */
            if (blkreachable(fn, fn->curblk)) {
               warn(&cm->fnblkspan, "non-void function '%s' may not return a value", fn->name);
            }
         }
         putreturn(fn, NOREF, NOREF);
      }
   }
}

/* top-level declaration */
static void
tldecl(CComp *cm)
{
   DeclState st = { DTOPLEVEL };
   do {
      bool noscls = 0;
      int nerr = nerror;
      Decl decl = pdecl(&st, cm);

      if (nerror != nerr && st.varini) {
         (void)expr(cm);
         pdecl(&st, cm);
         continue;
      }
      if (st.empty) break;
      if (!decl.scls) {
         noscls = 1;
         decl.scls = SCEXTERN;
      }
      if (!decl.sym) decl.sym = decl.name;
      decl.isdef = st.varini;
      if (st.funcdef) {
         const TypeData *td = &typedata[decl.ty.dat];
         if (td->ret.t != TYVOID && isincomplete(td->ret))
            error(&decl.span, "function definition with incomplete return type '%ty'", td->ret);
         for (int i = 0; i < td->nmemb; ++i) {
            if (td->param[i].t != TYVOID && isincomplete(td->param[i]))
               error(&st.pspans[i], "parameter has incomplete type '%ty'", td->param[i]);
         }
         decl.isdef = 1;
         int idecl = putdecl(cm, &decl);
         Decl *d = &declsbuf.p[idecl];
         if (d->inlin && decl.scls != SCSTATIC) fatal(&d->span, "non-static inline is unimplemented");
         Function fn = { &cm->fnarena, .name = d->sym, .globl = d->scls != SCSTATIC, .fnty = decl.ty, .retty = td->ret, .inlin = d->inlin };
         irinit(&fn);
         function(cm, &fn, st.pnames, st.pspans, st.pqual);
         if (!nerror && ccopt.dbg.p)
            irdump(&fn);
         irfini(&fn);
      } else if (decl.name) {
         int idecl = putdecl(cm, &decl);
         Decl *d = &declsbuf.p[idecl];
         if (st.varini) {
            if (isagg(decl.ty) && isincomplete(decl.ty))
               error(&decl.span, "initialization of variable with incomplete type '%ty'", decl.ty);
            Expr ini = initializer(cm, &decl.ty, EVSTATICINI, d->scls != SCSTATIC, d->qual, d->sym);
            d = &declsbuf.p[idecl];
            d->ty = decl.ty;
            if (d->scls == SCEXTERN && !noscls) {
               Span span = decl.span;
               joinspan(&span.ex, ini.span.ex);
               warn(&span, "'extern' variable has initializer");
            }
            pdecl(&st, cm);
         } else if (d->ty.t != TYFUNC && d->scls != SCTYPEDEF && (d->scls != SCEXTERN || noscls)) {
            /* tentative definitions */
            if (!objhassym(d->sym, NULL)) {
               uint size = typesize(d->ty);
               if (isincomplete(d->ty)) {
                  if (d->ty.t == TYARRAY) {
                     warn(&d->span, "tentative array definition assumed to have one element");
                     size = typesize(typechild(d->ty));
                     assert(size != 0);
                  } else if (isagg(d->ty)) {
                     warn(&d->span, "tentative definition with incomplete type '%ty'", d->ty);
                     assert(size == 0);
                  } else assert(0);
               }
               if (size) objnewdat(d->sym, Sbss, d->scls == SCEXTERN, size, typealign(d->ty));
            }
         }
         if (ccopt.dbg.p) bfmt(ccopt.dbgout, "var %s : %tq\n", d->name, d->ty, d->qual);
      } else {
         if (ccopt.dbg.p && decl.ty.t) bfmt(ccopt.dbgout, "type %ty\n", decl.ty);
      }
      freearena(&cm->fnarena);
      freearena(&cm->exarena);
      lexerfreetemps(cm->lx);
   } while (st.more);
}

Type cvalistty;
void
docomp(CComp *cm)
{
   static Env toplevel;
   Token tk[1];

   istr__func__ = intern("__func__");
   istr_main = intern("main");
   istr_memset = intern("memset");
   if (!cm->env) {
      vinit(&declsbuf, NULL, 1<<10);
      pmap_init(&tldeclmap, 1<<8);
      cm->env = &toplevel;
   }
   if (!cvalistty.t) {
      TypeData td = {
         .t = TYSTRUCT, .siz = targ_valistsize, .align = targ_primalign[TYPTR], .nmemb = 1,
         .fld = &(NamedField){intern("-"), {mkarrtype(mktype(TYPTR), 0, 3)}}
      };
      cvalistty = mkarrtype(mktagtype(intern("__builtin_va_list"), &td), 0, 1);
   }
   peek(cm, tk);
   envadddecl(cm->env, &(Decl) { cvalistty, SCTYPEDEF, .span = tk->span, .name = intern("__builtin_va_list") });
   putbuiltins(cm->env);

   while (peek(cm, tk) != TKEOF) {
      if (tk->t == ';') {
         lex(cm, tk);
      } else if (!isdecltok(cm) && tk->t != TKIDENT) {
         error(&tk->span, "expected declaration");
         do lex(cm, tk); while (tk->t != TKEOF && !isdecltok(cm));
      } else {
         tldecl(cm);
      }
   }
}

static void
initcm(CComp *cm, const char *file)
{
   enum { N = 1<<12 };
   static union { char m[sizeof(Arena) + N]; Arena *_align; } amem[2];
   const char *err;
   switch (initlexer(cm->lx, &err, file)) {
   default: assert(0);
   case LXERR:
      fatal(NULL, "Cannot open %'s: %s", file, err);
   case LXOK:
      cm->fnarena = (void *)amem[0].m;
      cm->fnarena->cap = N;
      cm->exarena = (void *)amem[1].m;
      cm->exarena->cap = N;
   }
}

void
ccomp(const char *file)
{
   CComp cm = {&(Lexer){0}};
   initcm(&cm, file);
   docomp(&cm);
}

void
cpp(WriteBuf *out, const char *file)
{
   CComp cm = {&(Lexer){0}};
   initcm(&cm, file);
   lexerdump(cm.lx, out);
}

/* vim:set ts=3 sw=3 expandtab: */
