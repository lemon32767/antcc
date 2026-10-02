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
      DEF_SVEC(char, 200, rest);
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
   DFUNCPARAMOLD,
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
   uchar scls; /* enum storageclass bitset */
   uchar qual;
   bool fninline : 1;
   bool base0  : 1, /* caller set initial base type, but there may be declspecs to parse */
        more   : 1, /* caller should keep calling pdecl to get next decl */
        varini : 1, /* caller should parse an initializer ('=' <ini>) and
                       call pdecl() to advance state before checking .more */
        funcdef : 1, /* caller should parse an func definition ('{' <body> '}').
                        the declaration list is finished. */
        bitf    : 1, /* caller should parse a bitfield size and
                        call pdecl() to advance state before checking .more */
        tagdecl : 1, /* declarator is a tagged type */
        empty   : 1; /* nothing decl (';') */
   internstr *pnames; /* param names for function definition */
   Span *pspans; /* param spans ditto */
   uchar *pqual; /* param quals ditto */
   Attrs attr;
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
         kw(int), kw(char), kw(_Bool), kw(bool), kw(__int128), kw(__uint128_t),
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
         tk('('), tk(TKNUMLIT), tk(TKCHRLIT), tk(TKSTRLIT), tk(TKW_Generic),
         tk(TKW__builtin_va_arg),
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
redeclarationok(const Decl *old, Decl *new)
{
   bool takeoldscls = 0;
   if (old->scls != new->scls) {
      if (old->scls == SCSTATIC && (new->scls &~ SCEXTERN) == SCNONE)
         takeoldscls = 1;
      else
         return 0;
   }
   switch (old->scls) {
   case SCSTATIC:
   case SCEXTERN:
      if (typescompat(&new->ty, old->ty, new->ty)) {
         if (takeoldscls) ((Decl *)new)->scls = old->scls;
         return 1;
      }
      return 0;
   case SCTYPEDEF:
      return old->ty.bits == new->ty.bits;
   }
   return 0;
}


static void
mergeattr(Attrs *to, const Attrs *src)
{
   bsunion(to->set, src->set, countof(to->set));
   if (src->align > to->align) to->align = src->align;
}

static int
putdecl(CComp *cm, Decl *decl)
{
   for (Env *env = cm->env; env; env = env->up) {
      Decl *old;
      if (!env->up) {
         ushort *pi = pmap_get(&tldeclmap, decl->name);
         if (pi) {
            old = &declsbuf.p[*pi];
            goto Match;
         }
      } else for (old = NULL; enviterdecl(&old, env);) {
         if (decl->name == old->name) {
         Match:
            if ((cm->env->up != NULL && decl->scls == SCSTATIC) || (old->isdef && decl->isdef)) {
               error(&decl->span, "redefinition of '%s'", decl->name);
               note(&old->span, "previously defined here");
               break;
            } else if (!redeclarationok(old, decl)) {
               error(&decl->span, "incompatible redeclaration of '%s'", decl->name);
               note(&old->span, "previously declared here");
               break;
            }
            if (old->sym) {
               assert(!!decl->sym);
               if (old->sym != decl->sym) {
                  bool oldisrename = old->sym != decl->name,
                       newisrename = decl->sym != decl->name;
                  if (oldisrename && newisrename) {
                     /* conflicting __asm__ names */
                     if (error(&decl->span, "'asm' declaration conflicts with previous rename")) {
                        note(&old->span, "previously defined here");
                     }
                     decl->sym = old->sym;
                  } else if (oldisrename) {
                     decl->sym = old->sym;
                  } else if (newisrename) {
                     old->sym = decl->sym;
                  }
               }
            } else {
               assert(!decl->sym);
            }
            if (old->isdef && !decl->isdef) {
               if (cm->env->up == NULL && old->inlin && !decl->inlin) {
                  /* inline def followed by non-inline decl, must emit */
                  markfnneeded(old->sym);
               }
               return old - declsbuf.p;
            } else {
               mergeattr(&decl->attr, &old->attr);
               /* C99/C11 6.7.4p6: the function definition is inline only if
                * all file scope declarations are inline */
               decl->inlin &= old->inlin;
            }
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
gettagged(CComp *cm, Span *span, enum typetag tt, internstr name, bool dodef, enum typetag enumbasety)
{
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
   return envaddtagged(cm->env, mktagtype(name, &(TypeData){tt, TFUNKNOWN, .backing = enumbasety}), span)->ty;
}

static Type
deftagged(CComp *cm, Span *span, enum typetag tt, internstr name, Type ty, enum typetag enumbasety)
{
   assert(name);
   for (Tagged *l = NULL; envitertagged(&l, cm->env);) {
      if (name == tagtypetags[typedata[l->ty.dat].id]) {
         *span = l->span;
         return l->ty;
      }
   }
   return envaddtagged(cm->env, ty.t ? ty : mktagtype(name, &(TypeData){tt, TFUNKNOWN, .backing = enumbasety}), span)->ty;
}

/*********************/
/* Expr Typechecking */
/*********************/

static const Type voidptr = {{ TYPTR, .flag = TFCHLDPRIM, .child = TYVOID }};
#define iszero(ex) \
   ((ex).t == ENUMLIT && (ex).u == 0 && (isint((ex).ty) || ((ex).ty.bits == voidptr.bits)))

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
   else if (t.t == TYARRAY || t.t == TYFUNC) t = typedecay(t);
   return t;
}

bool
assigncheck(Type t, const Expr *src)
{
   Type srcty = typedecay(src->ty);
   if (assigncompat(t, srcty)) {
      if (t.t == TYPTR && srcty.t == TYPTR
       && (t.flag & TFCHLDQUAL & srcty.flag & TFCHLDQUAL) != (srcty.flag & TFCHLDQUAL)) {
         warn(&src->span, "usage of '%ty' discards pointer qualifiers", src->ty);
      }
      return 1;
   } else if ((t.t == TYPTR && srcty.t == TYPTR)
           && !typescompat(NULL, typechild(t), typechild(srcty))) {
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

/* folding where a constant expression is required as an extension, with a warning */
static bool
eval2xintcon(Expr *ex)
{
   if (!isint(ex->ty)) return 0;
   if (eval(ex, EVINTCONST)) return 1;
   if (eval(ex, EVFOLD)) {
      warn(&ex->span, "expression is not a integer constant expression; "
                      "folding it here is an extension");
      return 1;
   }
   return 0;
}

static bool
isnullpo(const Expr *ex) /* match '0' or '(void *) 0' */
{
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
      int qual = (t1.flag | t2.flag) & TFCHLDQUAL;
      s1 = typechild(t1), s2 = typechild(t2);
      if (s1.bits == s2.bits || s2.t == TYVOID || s1.t == TYVOID) {
         return mkptrtype(s1.t == TYVOID ? s1 : s2, qual);
      }
      Span span = a->span;
      joinspan(&span.ex, b->span.ex);
      warn(&span, "pointer type mismatch in conditional expression ('%ty' and '%ty')",
            a->ty, b->ty);
      return mkptrtype(mktype(TYVOID), qual);
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
         else if (!typescompat(NULL, pointee1, pointee2)) {
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
   DEF_SVEC(Expr, 10, args);
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
   if (printsig) note(&callee->span, "function signature is '%ty'", ty);

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

static Expr initializer(CComp *, Type *ty, uint align, enum evalmode ev,
                        bool globl, enum qualifier qual, internstr name);
static void block(CComp *, Ref *stmtexprval, Type *stmtexprty);

static internstr istr__func__, istr_main, istr_memset;
static internstr mkhiddensym(const char *fnname, const char *name, int id);

static uint declalign(const Decl *d);

static Expr
compoundliteral(CComp *cm, Decl *decl, const Span *span)
{
   static bool warned = 0;
   if (ccopt.cstd < STDC99 && !warned)
      warn(span, "compound literals are a c99 feature"), warned = 1;
   return initializer(cm, &decl->ty, declalign(decl),
                     (decl->scls & SCSTATIC) ? EVSTATICINI : EVFOLD,
                     /*globl*/ 0, decl->qual, /*name*/ NULL);
}

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
      if (match(cm, &tk, '{')) { /* ({ GNU statement expression }) */
         Span span = tk.span;
         Ref val;
         ex.t = EIRVALUE;
         if (!cm->fn) {
            error(&span, "statement expression not allowed outside function");
            int bal = 1;
            do {
               switch (lex(cm, &tk)) {
               case '}': --bal; break;
               case '{': ++bal; break;
               case TKEOF: bal = 0; break;
               }
            } while (bal != 0);
            ex.ty = mktype(TYINT);
         } else {
            block(cm, &val, &ex.ty);
            ex.irref.bits = val.bits;
         }
         if (expect(cm, ')', NULL)) joinspan(&span.ex, tk.span.ex);
         ex.span = span;
      } else if (!isdecltok(cm)) { /* (expr) */
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
            ex = compoundliteral(cm, &decl, &tk.span);
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
            /* XXX really need a special ERROR type.. */
            ex = mkexpr(ENUMLIT, tk.span, mktype(TYINT), .i = 0);
         }
      } else if (decl->scls == SCTYPEDEF) {
         error(&tk.span, "unexpected typename %'tk (expected expression)", &tk);
         ex = mkexpr(ESYM, tk.span, decl->ty, .implicitsym = NULL);
      } else if (decl->isenumconst) {
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
      Expr tmp;
      span = tk.span;
      if (!match(cm, NULL, '(')) /* sizeof/alignof expr */
         goto Unops;
      else if (isdecltok(cm)) { /* sizeof/alignof (type) */
         DeclState st = { DCASTEXPR };
         Decl decl = pdecl(&st, cm);
         peek(cm, &tk);
         if (expect(cm, ')', NULL))
            joinspan(&span.ex, tk.span.ex);
         if (peek(cm, &tk) == '{') { /* actually sizeof (type){..} -> compound literal expr */
            joinspan(&span.ex, tk.span.ex);
            tmp = compoundliteral(cm, &decl, &span);
            joinspan(&span.ex, tmp.span.ex);
            goto SizeofExpr;
         }
         res = sizeofalignofcheck(&span, tt, decl.ty, NULL);
      } else { /* sizeof/alignof (expr) */
         tmp = commaexpr(cm);
         peek(cm, &tk);
         if (expect(cm, ')', NULL))
            joinspan(&span.ex, tk.span.ex);
      SizeofExpr:
         ppostfixopers(cm, &tmp);
         res = sizeofalignofcheck(&span, tt, tmp.ty, &tmp);
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
            } else if (ex.ty.t == TYFUNC) { /* function->pointer->function decay, no-op */
               break;
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
         if (!isptrcvt(ex.ty) && !isscalar(ex.ty))
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
   case TYARRAY: return isincomplete(ty) ? -1u : typearrlen(ty);
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
            s64int addend;
            uint off;
            uchar flag;
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

static s64int /* -> returns addend */
expr2reloc(internstr *psym, enum symflags *sf, const Expr *ex)
{
   if (ex->t == ESSYMREF) {
      *psym = ex->ssym.sym;
      *sf = (SLOCAL &- ex->ssym.local) | (SFUNC &- ex->ssym.func);
      return ex->ssym.off;
   } else if (ex->t == ESTRLIT || ex->t == EINIT) {
      if (ex->t == ESTRLIT) assert(ex->ty.t == TYARRAY);
      Ref r = expraddr(NULL, ex);
      *psym = xcon2sym(r.i);
      *sf = contab.p[r.i].flag;
      return 0;
   }
   fatal(&ex->span, "internal bug: non static reloc?");
}

static bool
rodatarelocok(void)
{
   return !(ccopt.pie | ccopt.pic);
}

static bool iniwriterec(CComp *cm, InitParser *ip, uint off, Expr *ex);

static void
iniwrite(CComp *cm, InitParser *ip, uint off, uint bitsiz, uint bitoff, Type ty, Expr *ex)
{
   if (isagg(ex->ty) && ip->ev == EVSTATICINI) {
      assert(ty.bits == ex->ty.bits);
      assert(ex->t != EINIT);
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
            u64int mask = (bitsiz == 64 ? -1ull : (1ull << bitsiz) - 1) << bitoff;
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
         enum symflags sf;
         s64int addend = expr2reloc(&sym, &sf, ex);
         if (!ip->dyn) {
            assert(ip->sec != Srodata || rodatarelocok());
            objreloc(sym, sf, targ_64bit ? REL_ABS64 : REL_ABS32,
                     ip->sec, ip->off + off, addend);
         } else {
            InitReloc *rel = alloc(ip->arena, sizeof *rel, 0);
            rel->link = ip->drel;
            rel->sym = sym;
            rel->off = off;
            rel->addend = addend;
            rel->flag = sf;
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
      assert(ex->ty.bits == ty.bits);
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

   if (isagg(ip->sub->ty) && targ.t == TYARRAY && isincomplete(targ)) {
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

   ip->sub = ip->cur;
   for (;;) {
      uint off, bitsiz, bitoff;
      u64int idx = ~0ull;
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
         if (!eval2xintcon(&ex))
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
initializer(CComp *cm, Type *ty, uint align, enum evalmode ev, bool globl,
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
            ip->off = objnewdat(sym, SLOCAL &- !globl, ip->sec, typesize(*ty), align);
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
      } else if (ex.t == EINIT) {
         if (!iniwriterec(cm, ip, 0, &ex))
            goto CannotEval;
      } else {
         if (ev && !eval(&ex, ev) && ev != EVFOLD) CannotEval:
            error(&ex.span, "cannot evaluate expression statically");
         else if (ev == EVSTATICINI || !isscalar(*ty))
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
      uint off, siz;
      uchar *p;

      if (isincomplete(*ty)) {
         uint len = ip->arrlen > ip->cur->idx ? ip->arrlen : ip->cur->idx;
         if (len == 0 && ccopt.pedant)
            warn(&span, "zero size array is an extension");
         *ty = mkarrtype(typechild(*ty), ty->flag & TFCHLDQUAL, len);
      }
   Dynfix:
      if (qual & QCONST && (ip->drel == NULL || rodatarelocok()))
         sec = Srodata;
      else
         sec = Sdata;
      assert(align >= typealign(*ty));
      if (!nerror) {
         off = objnewdat(sym, SLOCAL &- !globl, sec, siz = typesize(*ty), align);
         if (siz > 0) {
            p = sec == Srodata ? objout.rodata.p : objout.data.p;
            assert(ip->ddat.n <= siz);
            memcpy(p + off, ip->ddat.p, ip->ddat.n);
            memset(p + off + ip->ddat.n, 0, siz - ip->ddat.n);
            for (InitReloc *rel = ip->drel; rel; rel = rel->link) {
               objreloc(rel->sym, rel->flag, targ_64bit ? REL_ABS64 : REL_ABS32,
                        sec, off + rel->off, rel->addend);
            }
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
         if (!len && ccopt.pedant)
            warn(&span, "initializer creates a zero size array");
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

static const struct {
   const char *s;
   uint slen;
   uchar f:1, v:1, t:1;
} cattrs[] = {
#define DEF_ATTR(a,...) [ATTR##a] = {#a, sizeof#a - 1, .f=__VA_ARGS__},
   LIST_ATTRS(DEF_ATTR)
#undef DEF_ATTR
};

bool
hasattribute(const char *s, uint len)
{
   for (int i = 0; i < countof(cattrs); ++i) {
      if (cattrs[i].slen == len && !memcmp(cattrs[i].s, s, len))
         return 1;
   }
   return 0;
}

static bool
parse1attr(CComp *cm, Attrs *attr, Token *tk)
{ /* attribute ::= name | name ( params ) */
   Span span = tk->span;
   assert(tk->t == TKIDENT || in_range(tk->t, TKWBEGIN_, TKWEND_));
   enum attr a = ATTRxxx;
   const char *aname = NULL;
   for (int i = 0; i < countof(cattrs); ++i) {
      const char *s = tk->s;
      uint n = tk->len;

      /* strip __xyz__ -> xyz */
      if (n > 5 && s[0] == '_' && s[1] == '_' && s[n-2] == '_' && s[n-1] == '_') {
         s += 2;
         n -= 4;
      }
      if (n == cattrs[i].slen && !memcmp(s, aname = cattrs[i].s, n)) {
         setattr(attr, a = i);
         break;
      }
   }

   if (a == ATTRxxx)
      warn(&tk->span, "unknown/unsupported attribute ignored (%'tk)", tk);

   Expr params[4];
   int nparam = 0;
   if (match(cm, tk, '(')) {
      /* params ::= (pseudo-expr1, ...) */
      while (!match(cm, tk, ')')) {
         if (nparam == countof(params)) return 0;
         Expr ex = exprparse(cm, bintab['='].prec, NULL, EATTRARG);
         if (nparam < countof(params)) params[nparam] = ex;
         ++nparam;
         if (!match(cm, NULL, ',')) {
            peek(cm, tk);
            if (expect(cm, ')', NULL)) break;
            else return 0;
         }
      }
      joinspan(&span.ex, tk->span.ex);
   }

   int nminparam = 0, nmaxparam = 0;
   switch (a) {
   case ATTRxxx: break;
   case ATTRused: case ATTRunused: case ATTRwarn_unused_result: case ATTRdeprecated:
      nmaxparam = 1;
      goto Arity;
   case ATTRformat: nmaxparam = 3; goto Arity;
   case ATTRnonnull: /*variadic*/ break;

   /* these take no args */
   case ATTRnoreturn: case ATTRreturns_twice: case ATTRnoinline:
   case ATTRalways_inline: case ATTRflatten: case ATTRpure:
   case ATTRconst: case ATTRnothrow: case ATTRmalloc:
   case ATTRmay_alias: case ATTRexternally_visible:
   case ATTRpacked: case ATTRweak:

   Arity:
      if (nparam > nmaxparam || nparam < nminparam)
      BadArgs:
         warn(&span, "wrong number of arguments for attribute '%s'", aname);
      break;
   case ATTRaligned:
      if (nparam > 1) goto BadArgs;
      else if (nparam == 1) {
         if (isint(params[0].ty) && eval2xintcon(&params[0])) {
            if (!ispo2(params[0].u))
               error(&params[0].span, "alignment is not a power of 2");
            else
               attr->align = params[0].i;
         } else {
            error(&params[0].span, "'aligned' requires integer constant");
         }
      }
      break;
   /* these can't be safely ignored */
   //case ATTRmode:
   case ATTRsection:
   case ATTRconstructor:
   case ATTRdestructor:
   case ATTRalias:
   case ATTRweakref:
   case ATTRcommon: case ATTRnocommon:
   case ATTRtransparent_union:
   //Stub:
      stub(&span, "attribute '%s'", aname);
      break;
   }
   return 1;
}

static bool
attrspec(CComp *cm, Attrs *attr)
{ /* __attribute__ (( attribute-list )) */
   if (!match(cm, NULL, TKW__attribute__)) return 0;
   do {
      if (!expect(cm, '(', "after __attribute__")
       || !expect(cm, '(', "after __attribute__(")) {
      Bad:
         fatal(NULL, NULL);
      }
      while (!match(cm, NULL, ')')) {
         Token tk;
         lex(cm, &tk);
         if (tk.t != TKIDENT && !in_range(tk.t, TKWBEGIN_, TKWEND_)) {
            fatal(&tk.span, "expected attribute name");
         }
         if (!parse1attr(cm, attr, &tk)) goto Bad;
         if (!match(cm, NULL, ',')) {
            if (expect(cm, ')', NULL)) break;
            else goto Bad;
         }
      }
      if (!expect(cm, ')', NULL))
         goto Bad;
   } while (match(cm, NULL, TKW__attribute__));
   return 1;
}

static void
attrcheckctx(const Span *span, const Attrs *attr, char c /*f/v/t*/)
{
   bs_each(a, attr->set, countof(attr->set)) {
      if ((c == 'f' && !cattrs[a].f)
       || (c == 'v' && !cattrs[a].v)
       || (c == 't' && !cattrs[a].t))
      {
         warn(span, "'%s' attribute has no effect here", cattrs[a].s);
      }
   }
}

static uint
declalign(const Decl *d)
{
   int talign = typealign(d->ty);
   return d->attr.align > talign ? d->attr.align : talign;
}

static Type
buildagg(CComp *cm, enum typetag tt, internstr name, int id, const Attrs *tyattr)
{
   Token tk;
   Type t;
   Span flexspan;
   DEF_SVEC(NamedField, 32, fld);
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
         if (!decl.name && !decl.ty.t) { /* static assert */
            continue;
         }
         tysize = typesize(decl.ty);
         if (fld.n && td.flexi) {
            td.flexi = 0;
            error(&flexspan, "flexible array member is not at end of struct");
         }
         if (!isunion && decl.ty.t == TYARRAY && typearrlen(decl.ty) == 0) {
            td.flexi = 1;
            flexspan = decl.span;
         } else if (isincomplete(decl.ty)) {
            error(&decl.span, "field has incomplete type '%ty'", decl.ty);
         } else if (decl.ty.t == TYFUNC)  {
            error(&decl.span, "field has function type '%ty'", decl.ty);
         }
         bitsiz = 0;
         if (st.bitf) { /* handle bit-field */
            Expr ex = constantexpr(cm);
            const char *name = decl.name ? &decl.name->c : "<anonymous>";
            if (!isint(decl.ty)) {
               error(&decl.span, "bit-field '%s' has non-integer type '%ty'", name, decl.ty);
            } else if (!isint(ex.ty)) {
               error(&ex.span, "integer constant expression has non-integer type '%ty'", decl.ty);
            } else if (!eval2xintcon(&ex)) {
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
                  // XXX what to do with packed here
                  bitfbyteoff = alignup(td.siz, typealign(decl.ty));
               } else if (bitoff + bitsiz > 8*bitftypesiz) {
                  /* no straddling boundaries */
                  bitoff = 0;
                  bitfbyteoff += bitftypesiz;
               }
               if (tysize > bitftypesiz) bitftypesiz = tysize;
            }
            pdecl(&st, cm);
         } else { /* reset bit-field */
            bitftypesiz = bitoff = bitsiz = 0;
         }
         if (decl.ty.t) {
            uint align;
            if ((hasattr(tyattr, ATTRpacked) || hasattr(&decl.attr, ATTRpacked))
             && !hasattr(&decl.attr, ATTRaligned))
               align = 1;
            else if (cm->pragma->pack.stk[cm->pragma->pack.top])
               align = cm->pragma->pack.stk[cm->pragma->pack.top];
            else
               align = declalign(&decl);
            assert(ispo2(align));
            uint siz = tysize;
            uint off = isunion ? 0
                     : bitftypesiz ? bitfbyteoff
                     : alignup(td.siz, align);
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
               if (typedata[decl.ty.dat].flexi && !isunion) {
                  td.flexi = 1;
                  flexspan = decl.span;
               }
            }
            if (isunion) {
               td.siz = td.siz < siz ? siz : td.siz;
               bitsiz = bitfbyteoff = bitoff = bitftypesiz = 0;
            } else {
               bitoff += bitsiz;
               td.siz = off + siz;
            }
            td.align = td.align < align ? align : td.align;
         }
      } while (st.more);
   }
   if (td.flexi && fld.n == 1)
      warn(&flexspan, "flexible array member in otherwise empty aggregate");
   if (td.flexi && ccopt.cstd < STDC99 && ccopt.pedant)
      warn(&flexspan, "flexible array member in %M is an extension");
   if (fld.n == 0) {
      NamedField dummy = { intern(""), { mktype(TYCHAR), 0 }};
      if (ccopt.pedant)
         warn(&tk.span, "%s with zero members is an extension", tag);
      vpush(&fld, dummy);
      td.siz = 0;
      td.align = 1;
   }
   if (tyattr->align && tyattr->align > td.align)
      td.align = tyattr->align;
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
inttyminmax(s64int *min, u64int *max, enum typetag tt)
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
buildenum(CComp *cm, internstr name, const Span *span, int id, enum typetag basety, const Attrs *tyattr)
{
   Token tk;
   s64int tymin, minv = 0;
   u64int tymax, maxv = 0;
   TypeData td = {TYENUM, .backing = basety ? basety : TYINT};
   Type ty = mktype(td.backing);
   Span maxvspan;
   s64int iota = 0;
   bool somelonglong = 0;

   inttyminmax(&tymin, &tymax, td.backing);
   while (!match(cm, &tk, '}')) {
      Decl decl = {0};
      peek(cm, &tk);
      expect(cm, TKIDENT, NULL);
      if (match(cm, NULL, '=') || (peek(cm, NULL) == TKNUMLIT && !expect(cm, '=', NULL))) {
         Expr ex = expr(cm);
         if (eval2xintcon(&ex)) {
            iota = ex.i;
            if (!basety) {
               if (ex.ty.t != ty.t)
                  inttyminmax(&tymin, &tymax, ex.ty.t);
               ty = ex.ty;
            }
         } else {
            error(&ex.span, "enum value is not an integer constant");
         }
      } else if (tk.t != TKIDENT) {
         lex(cm, NULL);
         continue;
      }
      if (!basety) {
         while (issigned(ty) ? (iota > (s64int)tymax || iota < tymin) : iota > tymax)
            inttyminmax(&tymin, &tymax, ++ty.t);
         somelonglong |= ty.t >= TYVLONG;
         if ((isunsigned(ty) || iota > 0) && iota > maxv)
            maxv = iota, maxvspan = tk.span;
         else if (issigned(ty) && iota < minv)
            minv = iota;
      } else {
         if (iota != intcast(basety, iota))
            error(&tk.span, "enum value outside of '%ty' range", ty);
      }
      decl.ty = ty;
      decl.name = tk.name;
      decl.isenumconst = 1;
      decl.value = iota++;
      putdecl(cm, &decl);
      if (!match(cm, &tk, ',')) {
         if (expect(cm, '}', "or `,'"))
            break;
         else lex(cm, NULL);
      }
   }

   if (!basety) {
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
   return ty;
}

static Type
tagtype(CComp *cm, enum toktag kind)
{
   Token tk;
   Type t;
   enum typetag enumbasety = 0;
   Span span, fullspan;
   enum typetag tt = kind == TKWenum ? TYENUM
                   : kind == TKWstruct ? TYSTRUCT
                   : TYUNION;
   internstr tag = NULL;

   Attrs attr = {0};
   attrspec(cm, &attr);
   peek(cm, &tk);
   if (match(cm, &tk, TKIDENT))
      tag = tk.name;
   span = tk.span;
   attrcheckctx(&span, &attr, 't');
   fullspan = span;
   if (tt == TYENUM && match(cm, NULL, ':')) {
      /* enum : base_type */
      Decl decl = pdecl(&(DeclState){DCASTEXPR}, cm);
      if (!isint(decl.ty)) {
         error(&decl.span, "invalid enum underlying type '%ty'", decl.ty);
      } else {
         joinspan(&fullspan.ex, decl.span.ex);
         enumbasety = decl.ty.t;
      }
   }
   if (!match(cm, NULL, '{')) {
      if (!tag) {
         error(&tk.span, "expected %tt name or '{'", kind);
         return mktype(0);
      }
      bool dodef = peek(cm, NULL) == ';';
      if (!dodef && enumbasety) {
         error(&fullspan, "cannot specify enum backing type here");
      }
      t = gettagged(cm, &span, tt, tag, dodef, enumbasety);
   } else {
      if (tag) {
         t = deftagged(cm, &span, tt, tag, mktype(0), enumbasety);
         bool err = 0;
         if ((err = t.t != tt)) {
               error(&tk.span,
                     "defining tagged type %'tk as %tt clashes with previous definition",
                     &tk, kind);
         } else if ((err = t.t == TYENUM && typedata[t.dat].backing != enumbasety)) {
            error(&fullspan, "enum '%s' underlying type mismatch", tag);
         } else if ((err = !isincomplete(t))) {
            error(&fullspan, "redefinition of '%tt %s'", kind, tag);
         }
         if (err) note(&span, "previously:");
      }
      if (tt == TYENUM)
         t = buildenum(cm, tag, &span, tag ? typedata[t.dat].id : -1, enumbasety, &attr);
      else
         t = buildagg(cm, tt, tag, tag ? typedata[t.dat].id : -1, &attr);
   }
   peek(cm, &tk);
   if (attrspec(cm, &attr)) {
      error(&tk.span, "NYI: type attribute at the end of its definition");
      attrcheckctx(&span, &attr, 't');
   }

   if (t.t != tt) {
      error(&tk.span, "declaring tagged type %'s as %tt clashes with previous definition",
            tag, kind);
      note(&span, "previous definition:");
   }
   return t;
}

static void
declcheckattr(const Decl *decl)
{
   char c = decl->scls == SCTYPEDEF ? 't'
          : decl->ty.t == TYFUNC ? 'f'
          : 'v';
   attrcheckctx(&decl->span, &decl->attr, c);
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
      KI128     = 1<<11,
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
         setattr(&st->attr, ATTRnoreturn);
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
      case TKW__int128:
         if (arith & KI128) goto DupArith;
         arith |= KI128;
         break;
      case TKW__uint128_t:
         if (arith & (KUNSIGNED | KI128)) goto DupArith;
         arith |= KUNSIGNED | KI128;
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
            mergeattr(&st->attr, &decl->attr);
            continue;
         }
         /* fallthru */
      default:
         if (attrspec(cm, &st->attr))
            continue;
         goto End;
      case TKW_BitInt:
      case TKW_Decimal128: case TKW_Decimal32:
      case TKW_Decimal64: case TKW_Imaginary:
         error(&tk.span, "%'tk is unsupported", &tk);
         arith = arith ? arith : KINT;
      }

      bool paramp = st->kind == DFUNCPARAM || st->kind == DFUNCPARAMOLD;
      if ((!properdecl && scls && !(paramp && scls == SCREGISTER))
       || (scls == SCAUTO && st->kind == DTOPLEVEL)) {
         error(&tk.span, "storage class specifier %'tk is not allowed here", &tk);
      } else {
         st->scls |= scls;
      }

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
      else if ((arith & ~KSIGNED) == KI128)
         t = TYINT128;
      else if (arith == (KUNSIGNED | KI128))
         t = TYUINT128;
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
      if (!name) break;
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
declarator(DeclState *st, CComp *cm, Span span0, Attrs attr0) {
   Decl decl = { st->base, st->scls, .qual = st->qual, .span = span0, .attr = attr0 };
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
            decl.ty = mkunszarrtype(decl.ty, decl.qual);
         else {
            uint n = 0;
            Expr *ex = &l->count;
            if (!ex->t) { /* ['*'] */
               if (l->prev != &list) error(&l->span, "[*] array declarator is not allowed here");
            } else if (!eval2xintcon(ex)) {
               error(&ex->span, "array length is not an integer constant");
            } else if (issigned(ex->ty) && ex->i < 0) {
               error(&ex->span, "array length is negative");
            } else if (ex->u > (1ull << (8*sizeof n)) - 1) {
               error(&ex->span, "array too long (%ul)", ex->u);
            } else if (ex->u == 0) {
               if (ccopt.pedant)
                  warn(&ex->span, "zero size array is an extension");
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
         if (l->prev == &list) { /* root declaration node */
            if (l->npar) {
               st->pnames = alloccopy(&cm->fnarena, l->pnames, l->npar * sizeof(char *), 0);
               st->pspans = alloccopy(&cm->fnarena, l->pspans, l->npar * sizeof(Span), 0);
               st->pqual = l->pqual ? alloccopy(&cm->fnarena, l->pqual, l->npar, 1) : NULL;
            }
            st->funcdef = 1;
            decl.inlin = st->fninline;
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

/* declaration-list for K&R style def. 'f(a) int a; { ... }' */
static void
poldstyleparams(CComp *cm, Decl *fndecl, internstr *pnames, Span *pspans)
{
   assert(fndecl->ty.t == TYFUNC);
   const TypeData *td = &typedata[fndecl->ty.dat];
   int nparam = td->nmemb;
   Type *params = alloccopy(&cm->fnarena, td->param, nparam * sizeof *params, 0);

   /* scope setup */
   Env env = {0};
   envdown(cm, &env);
   for (int i = 0; i < nparam; ++i)
      envadddecl(&env, &(Decl){params[i], .span = pspans[i], .name = pnames[i],
                               .id = i});

   do {
      DeclState st = { DFUNCPARAMOLD };
      do {
         Decl decl = pdecl(&st, cm),
              *par = NULL;
         while (enviterdecl(&par, &env))
            if (decl.name == par->name)
               break;
         if (par) {
            if (par->id >= 0) {
               (void)decl.qual; /* it's "old style" C, who cares about const */
               params[par->id] = decl.ty = typedecay(decl.ty);
               pspans[par->id] = decl.span;
               *par = decl;
               par->id = -1; /* mark defined */
            } else if (!typescompat(NULL, decl.ty, par->ty)) {
               error(&decl.span, "redefinition of parameter '%s'", decl.name);
            }
         } else {
            error(&decl.span, "declaration for '%s' but no such parameter", decl.name);
         }
      } while (st.more);
   } while (isdecltok(cm));
   envup(cm);
   fndecl->ty = mkfntype(td->ret, nparam, params, /*kandr*/1, 0);
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
   if (!eval2xintcon(&ex)) {
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

   if (st->base0 || st->kind == DCASTEXPR) goto DeclSpec;
   if (!st->base.t) {
      if (match(cm, &tk, TKW_Static_assert) || match(cm, &tk, TKWstatic_assert)) {
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
      attrspec(cm, &st->attr);
      declspec(st, cm, &decl.span);
   } else {
      peek(cm, &tk);
      decl.span = tk.span;
   }
   if (st->scls == SCTYPEDEF) properdecl = 0;

   if (first && st->tagdecl && match(cm, &tk, ';')) {
      /* struct/union/enum ... ; */
      attrcheckctx(&decl.span, &st->attr, 't');
      decl = (Decl) { st->base, st->scls, st->qual, .span = decl.span, .attr = st->attr };
      return decl;
   } else if (st->kind == DFIELD && match(cm, &tk, ':')) {
      /* indicate bitfield */
      decl = (Decl) { st->base, st->scls, st->qual, .span = decl.span, .attr = st->attr };
      st->bitf = 1;
      return decl;
   }
   decl.attr = st->attr;
   attrspec(cm, &decl.attr);
   decl = declarator(st, cm, decl.span, decl.attr);
   bool funcdefok = st->funcdef && first;
   st->funcdef = 0;
   if (attrspec(cm, &decl.attr)) funcdefok = 0;
   if (decl.ty.t != TYFUNC && st->fninline)
      error(&decl.span, "`inline' used on non-function declaration");
   /*if (decl.ty.t != TYFUNC && st->fnnoreturn)
      error(&decl.span, "`_Noreturn' used on non-function declaration");*/
   if (st->kind == DTOPLEVEL || st->kind == DFUNCVAR) {
      if (match(cm, NULL, TKW__asm__) && expect(cm, '(', NULL)) {
         if (peek(cm, NULL) == TKSTRLIT) {
            lex(cm, &tk);
            decl.sym = intern_(tk.s, tk.len);
         } else expect(cm, TKSTRLIT, "asm symbol name");
         expect(cm, ')', NULL);
      }
      attrspec(cm, &decl.attr);
   }

   declcheckattr(&decl);
   if (properdecl && match(cm, &tk, '=')) {
      st->varini = 1;
      return decl;
   } else if (funcdefok) {
      if (match(cm, &tk, '{')) {
         st->funcdef = 1;
         return decl;
      } else if (typedata[decl.ty.dat].kandr && isdecltok(cm)) {
         poldstyleparams(cm, &decl, st->pnames, st->pspans);
         if (match(cm, &tk, '{')) {
            st->funcdef = 1;
         } else {
            peek(cm, &tk);
            error(&tk.span, "expected '{' for function body after parameter list");
         }
         return decl;
      }
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

static inline enum symflags
declsymflags(const Decl *decl)
{
   enum symflags sf = 0;
   if (decl->scls == SCSTATIC) sf |= SLOCAL;
   if (decl->ty.t == TYFUNC) sf |= SFUNC;
   if (hasattr(&decl->attr, ATTRweak)) sf |= SWEAK;
   if (decl->inlin) sf |= SC99INLFN;
   return sf;
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
   assert(isagg(src->ty));
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

static void geninit(Function *fn, Type t, Ref dst, const Expr *src, bool volatyl);
static Ref condexprvalue(Function *fn, const Expr *ex, bool discard);

Ref
expraddr(Function *fn, const Expr *ex)
{
   Decl *decl;
   Ref r;
   enum symflags sf;

   switch (ex->t) {
   case ESYM:
      decl = &declsbuf.p[ex->decl];
      assert(decl != NULL);
      switch (decl->scls) {
      case SCAUTO: case SCREGISTER:
         assert(decl->id >= 0);
         return mkref(RTMP, decl->id);
      case SCEXTERN: case SCNONE: case SCSTATIC:
         sf = declsymflags(decl);
         if (decl->isdef && !decl->inlin) sf |= SLOCAL;
         return mksymref(decl->sym, sf);
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
         geninit(fn, ex->ty, r, ex, ex->qual & QVOLATILE);
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
         ip->off = objnewdat(sym, SLOCAL, ip->sec, typesize(ty), typealign(ty));
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
genstore(Function *fn, Type t, Ref ptr, Ref val, bool volatyl)
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
   ins.keep = volatyl;
   return addinstr(fn, ins);
}

static void genbitfstore(Function *fn, const Type ty, Ref addr,
                         const ExprGetFld *fld, Ref tmp, Ref val, bool volatyl);

typedef struct { Ref a, b; } Ref2;
static Ref complex2addr_cvt(Function *, Type, const Expr *);
static Ref complex2scalar(Function *fn, Type to, const Expr *ex);
static Ref2 cvt2complex(Function *fn, Type base, const Expr *ex);
Ref2 compcomplexex(Function *, const Expr *, bool discard);
static void complexstore(Function *fn, Type base, Ref, Ref2, bool volatyl);

static void
geninit(Function *fn, Type t, Ref dst, const Expr *src, bool volatyl)
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
               addinstr(fn, mkinstr2(Ostorei8 + ilog2(align), 0, adr, ZEROREF));
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
            geninit(fn, ex->ty, adr, ex, volatyl);
         } else if (isagg(ex->ty)) {
            structcopy(fn, ex->ty, adr, expraddr(fn, ex));
         } else if (iscomplex(ex->ty)) {
            complexstore(fn, typechild(ex->ty), adr, compcomplexex(fn, ex, 0), volatyl);
         } else if (!val->bitsiz) {
            genstore(fn, ex->ty, adr, exprvalue(fn, ex), volatyl);
         } else {
            Ref q = exprvalue(fn, ex);
            genbitfstore(fn, ex->ty, adr, &(ExprGetFld){0, val->bitsiz, val->bitoff}, NOREF, q, volatyl);
         }
      }
   } else if (src->t == ESTRLIT) {
      Type ctyp = typechild(src->ty);
      uint csiz = typesize(ctyp);
      adr = dst;
      bool volatyl = 0;
      for (uint i = 0; i < src->s.n; ++i) {
         if (csiz == 1)
            genstore(fn, ctyp, adr, mkref(RICON, src->s.p[i]), volatyl);
         else if (csiz == 2)
            genstore(fn, ctyp, adr, mkref(RICON, src->s.w16[i]), volatyl);
         else
            genstore(fn, ctyp, adr, mkintcon(KI32, src->s.w32[i]), volatyl);
         adr = irbinop(fn, Oadd, KPTR, dst, mkref(RICON, (i+1)*csiz));
      }
      genstore(fn, ctyp, adr, ZEROREF, volatyl); /* null term */
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
      Ref r;
      if (!iscomplex(ex->ty)) r = exprvalue(fn, ex);
      else r = complex2scalar(fn, mktype(TYBOOL), ex);
      putcondbranch(fn, r, tr, fl);
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
         Ref val;
         if (!iscomplex(ex->ty)) {
            val = exprvalue(fn, ex);
            if (isscalar(phi->typ))
               val = scalarcvt(fn, phi->typ, ex->ty, val);
            else assert(ex->ty.bits == phi->typ.bits);
         } else {
            assert(isint(phi->typ));
            val = complex2scalar(fn, phi->typ, ex);
         }
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
   if (isagg(ex->ty) || iscomplex(ex->ty)) {
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
      Ref r;
      if (!iscomplex(ty))
         r = scalarcvt(fn, ty, typedecay(arg->ty), exprvalue(fn, arg));
      else
         r = complex2addr_cvt(fn, ty, arg);
      vpush(&insns, mkarginstr(mkirtype(ty), r));
   }
   for (int i = 0; i < insns.n; ++i)
      addinstr(fn, insns.p[i]);
   vfree(&insns);
   ins.r = mkcallarg(mkirtype(ex->ty), ex->narg, td->variadic ? td->nmemb : td->kandr ? 0 : -1);
   Ref r = addinstr(fn, ins);
   if (sub[0].t == ESYM && hasattr(&declsbuf.p[sub[0].decl].attr, ATTRnoreturn))
      /* trap if noreturn func returns */
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
   u64int mask;

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
             const ExprGetFld *fld, Ref tmp, Ref val, bool volatyl)
{
   enum irclass k = type2cls[scalartypet(ty)];
   uint off = fld->off, bitsiz = fld->bitsiz, bitoff = fld->bitoff;
   uint bittypesize = 8*typesize(ty);
   u64int mask;

   assert(k);
   if (!tmp.bits) {
      addr = irbinop(fn, Oadd, KPTR, addr, mkintcon(KPTR, off));
      tmp = genload(fn, ty, addr, volatyl);
   }
   mask = (bitsiz == 64 ? -1ull : (1ull << bitsiz) - 1) << bitoff;

   /* mask out bits in existing container */
   tmp = irbinop(fn, Oand, k, tmp, mkintcon(k, ~mask));

   /* shift and mask source value */
   if (isintcon(val)) {
      val = mkintcon(k, ((u64int)intconval(val) << bitoff) & mask);
   } else {
      val = irbinop(fn, Oshl, k, val, mkref(RICON, bitoff));
      if (bitsiz < bittypesize)
         val = irbinop(fn, Oand, k, val, mkintcon(k, mask));
   }
   /* combine and write */
   if (bitsiz < bittypesize)
      val = irbinop(fn, Oior, k, tmp, val);
   genstore(fn, ty, addr, val, volatyl);
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
   if (ex->ty.t != TYVOID && !isscalar(ex->ty)) {
      /* fn & array designators evaluate to their address;
       * so do aggregates for the purpose of code generation */
      if (isagg(ex->ty) && isincomplete(ex->ty))
         error(&ex->span, "use of incomplete type '%ty'", ex->ty);
      return expraddr(fn, ex);
   } else if (iscomplex(ex->ty)) {
      assert(discard);
      (void)compcomplexex(fn, ex, discard);
      return NOREF;
   } else if (in_range(ex->ty.t, TYINT128, TYUINT128) && !discard) {
      error(&ex->span, "%ty codegen is not yet supported", &ex->ty);
      return NOREF;
   }

   Type ty;
   Ref l, r, q, adr;
   uint bitsiz;
   enum op op;
   int swp;
   enum irclass cls = type2cls[scalartypet(ex->ty)];
   Expr *sub = ex->sub;

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
      if (!iscomplex(sub->ty) || discard) {
         r = compileexpr(fn, sub, discard);
         if (discard) return NOREF;
         r = scalarcvt(fn, ex->ty, sub->ty, r);
      } else {
         r = complex2scalar(fn, ex->ty, sub);
      }
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
      for (swp = 0; sub->t == ELOGNOT; ex = sub, sub = sub->sub)
         swp ^= 1;
      op = Oequ + swp;
      if (!iscomplex(sub->ty) || discard) {
         l = compileexpr(fn, sub, discard);
         if (discard) return NOREF;
         l = scalarcvt(fn, ex->ty, sub->ty, l);
      } else {
         l = complex2scalar(fn, mktype(TYBOOL), sub);
      }
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
      if (sub->t == EGETF && (bitsiz = sub->fld.bitsiz)) {
         Ref tmp;
         adr = expraddr(fn, &sub->sub[0]);
         l = genbitfload(fn, &tmp, sub->ty, &adr, &sub->fld, sub->qual & QVOLATILE);
         q = irbinop(fn, op, cls, l, r);
         genbitfstore(fn, sub->ty, adr, &sub->fld, tmp, q, sub->qual & QVOLATILE);
      } else {
         adr = expraddr(fn, sub);
         l = genload(fn, sub->ty, adr, sub->qual & QVOLATILE);
         q = irbinop(fn, op, cls, l, r);
         genstore(fn, sub->ty, adr, q, sub->qual & QVOLATILE);
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
      genstore(fn, sub->ty, adr, q, sub[0].qual & QVOLATILE);
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
      if (!iscomplex(ty) || discard) {
         if (isint(ty) && isunsigned(ty) && in_range(op, Olth, Ogte))
            op += Oulth - Olth;
         l = compileexpr(fn, &sub[0], discard);
         r = compileexpr(fn, &sub[1], discard);
         if (discard) return NOREF;
         l = scalarcvt(fn, ty, sub[0].ty, l);
         r = scalarcvt(fn, ty, sub[1].ty, r);
         cls = type2cls[ty.t];
         return irbinop(fn, op, cls, l, r);
      } else {
         assert(op == Oequ || op == Oneq);
         Type base = typechild(ty);
         Ref2 z1 = cvt2complex(fn, base, &sub[0]),
              z2 = cvt2complex(fn, base, &sub[1]);
         /* meh */
         l = irbinop(fn, op, type2cls[base.t], z1.a, z2.a);
         r = irbinop(fn, op, type2cls[base.t], z1.b, z2.b);
         return irbinop(fn, op == Oequ ? Oand : Oior, KI32, l, r);
      }
   case ESET:
      assert(isscalar(ex->ty));
      q = scalarcvt(fn, sub[0].ty, sub[1].ty, exprvalue(fn, &sub[1]));
      if (sub[0].t == EGETF && (bitsiz = sub[0].fld.bitsiz)) {
         /* bit-field */
         adr = expraddr(fn, &sub[0].sub[0]);
         genbitfstore(fn, ex->ty, adr, &sub[0].fld, NOREF, q, sub[0].qual & QVOLATILE);
      } else {
         bitsiz = 0;
         adr = expraddr(fn, &sub[0]);
         genstore(fn, ex->ty, adr, q, sub[0].qual & QVOLATILE);
      }
      if (discard) return NOREF;
      return narrow(fn, cls, sub[0].ty, q, bitsiz);
   case ESETMUL:
      op = Omul;
      goto Compound;
   case ESETDIV:
      op = Odiv;
      goto Compound;
   case ESETREM:
      op = Orem;
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
      if (isunsigned(ty) && in_range(op, Odiv, Orem)) {
         op = op - Odiv + Oudiv;
      }
      r = exprvalue(fn, &sub[1]);
      if (sub[0].t == EGETF && (bitsiz = sub[0].fld.bitsiz)) {
         /* bit-field */
         Ref tmp;
      CompoundBitf:
         adr = expraddr(fn, &sub[0].sub[0]);
         l = genbitfload(fn, &tmp, sub[0].ty, &adr, &sub[0].fld, sub[0].qual & QVOLATILE);
         q = irbinop(fn, op, cls, l, r);
         genbitfstore(fn, sub[0].ty, adr, &sub[0].fld, tmp, q, sub[0].qual & QVOLATILE);
      } else {
         bitsiz = 0;
         adr = expraddr(fn, &sub[0]);
         l = genload(fn, ex->ty, adr, sub[0].qual & QVOLATILE);
         if ((op != Oadd && op != Osub) || cls != KPTR) {
            l = scalarcvt(fn, ty, sub[0].ty, l);
            r = scalarcvt(fn, ty, sub[1].ty, r);
            q = irbinop(fn, op, type2cls[ty.t], l, r);
            q = scalarcvt(fn, ex->ty, ty, q);
         } else {
            q = genptroff(fn, op, typesize(typechild(ex->ty)), l, sub[1].ty, r);
         }
         genstore(fn, ex->ty, adr, q, sub[0].qual & QVOLATILE);
      }
      if (discard) return NOREF;
      return narrow(fn, cls, ex->ty, q, bitsiz);
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
   case EIRVALUE:
      return (Ref){.bits = ex->irref.bits};
   default: assert(!"nyi expr");
   }
}

static Ref
complex2scalar(Function *fn, Type to, const Expr *ex)
{
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

static Ref2
cvt2complex(Function *fn, Type base, const Expr *ex)
{
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
   return (Ref2){scalarcvt(fn, base, ex->ty, exprvalue(fn, ex)), mkfltcon(type2cls[base.t], 0)};
}

static Ref2
complexload(Function *fn, Type base, Ref adr, bool volatyl)
{
   assert(isscalar(base) && isflt(base));
   return (Ref2){
      genload(fn, base, adr, volatyl),
      genload(fn, base, irbinop(fn, Oadd, KPTR, adr, mkref(RICON, targ_primsizes[base.t])), volatyl)
   };
}

static void
complexstore(Function *fn, Type base, Ref adr, Ref2 c, bool volatyl)
{
   assert(isscalar(base) && isflt(base));
   genstore(fn, base, adr, c.a, volatyl);
   genstore(fn, base, irbinop(fn, Oadd, KPTR, adr, mkref(RICON, targ_primsizes[base.t])), c.b, volatyl);
}

static Ref
complex2addr_cvt(Function *fn, Type to, const Expr *ex)
{
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
compcomplexex(Function *fn, const Expr *ex, bool discard)
{
   static const Ref2 NIL = { 0 };
   assert(iscomplex(ex->ty));
   Type sty = typechild(ex->ty);
   assert(isflt(sty));
   enum irclass cls = type2cls[sty.t];
   Ref adr, r;
   Ref2 q, w, z;
   const Expr *sub = ex->sub;
   Type ty;
   enum op op;
   switch (ex->t) {
   case ENUMLIT: /* imaginary literal e.g. 1.0iF */
      if (discard) return NIL;
      return (Ref2){mkfltcon(cls, 0.0), mkfltcon(cls, ex->f)};
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
      w.a = irbinop(fn, ex->t < EPREDEC ? Oadd : Osub, cls, q.a, mkfltcon(cls, 1.0));
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
         r = compileexpr(fn, &ex->sub[2-c], discard);
         if (discard) return NIL;
         assert(ex->sub[2-c].ty.bits == ex->ty.bits);
         return compcomplexex(fn, &ex->sub[2-c], 0);
      }

      Block *tr, *fl, *end;
      condjump(fn, &sub[0], tr = newblk(fn), fl = newblk(fn));
      useblk(fn, tr);
      q = compcomplexex(fn, &sub[1], discard);
      end = newblk(fn);
      if (fn->curblk)
         putbranch(fn, end);
      useblk(fn, fl);
      w = compcomplexex(fn, &sub[2], discard);
      if (fn->curblk)
         putbranch(fn, end);
      useblk(fn, end);
      if (discard) return NIL;
      z.a = addphi(fn, cls, (Ref[2]){q.a, w.a});
      z.b = addphi(fn, cls, (Ref[2]){q.b, w.b});
      return z;
   case ESEQ:
      expreffects(fn, &sub[0]);
      return compcomplexex(fn, &sub[1], discard);
   default: assert(!"nyi cmplx expr");
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

static bool stmt(CComp *, Ref *stmtexprval, Type *stmtexprty);
static void localdecl(CComp *, bool forinit);

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
deflabel(CComp *cm, const Span *span, internstr name)
{
   Function *fn = cm->fn;
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
loopbody(CComp *cm, Block *brk, Block *cont)
{
   Block *save[2];
   bool terminates = 0;

   save[0] = cm->breakto, save[1] = cm->loopcont;
   cm->breakto = brk, cm->loopcont = cont;
   ++cm->loopdepth;

   terminates = stmt(cm, NULL, NULL);

   --cm->loopdepth;
   cm->breakto = save[0], cm->loopcont = save[1];

   return terminates;
}

#define EMITS if (doemit && !nerror)

typedef struct {
   s64int val;
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
   s64int v1 = a->val, v2 = b->val;
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
genswitch(CComp *cm, const Expr *ex)
{
   Ref sel;
   Function *fn = cm->fn;
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
   stmt(cm, NULL, NULL);
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
   s64int prev;
   for (int i = 0; i < st.cases.n; ++i) {
      const SwitchCase *c = &st.cases.p[i];
      if (i > 0) {
         assert(c->val >= prev);
         if (c->val == prev) {
            error(&c->span, "duplicate case value");
            note(&c[-1].span, "previously defined here");
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
stmt(CComp *cm, Ref *stmtexprval, Type *stmtexprty)
{
   Function *fn = cm->fn;
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
         if (!eval2xintcon(&ex))
            error(&ex.span, "not an integer constant expression");
         else if (cm->switchstmt && ex.ty.bits != cm->switchstmt->condtype.bits) {
            Expr tmp = ex;
            ex = mkexpr(ECAST, ex.span, cm->switchstmt->condtype, .sub = &tmp);
            bool ok = eval2xintcon(&ex);
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
         deflabel(cm, &tk.span, tk.name);
      } else {
         assert(tk.t == TKIDENT);
         /* kludge for no backtracking and no lookahead */
         ex = exprparse(cm, 1, &tk, EFROMSTMT);
         goto ExprStmt;
      }
      doemit = 1;
   }

   switch (peek(cm, NULL)) {
   case '{':
      lex(cm, NULL);
      envdown(cm, &e);
      block(cm, NULL, NULL);
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
      if (!isptrcvt(ex.ty) && !isscalar(ex.ty))
         error(&ex.span, "'if' condition is not a scalar '%ty'", ex.ty);
      tr = fl = end = NULL;
      EMITS {
         tr = newblk(fn);
         fl = newblk(fn);
         condjump(fn, &ex, tr, fl);
         useblk(fn, tr);
      }
      terminates = stmt(cm, NULL, NULL);
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
         terminates &= stmt(cm, NULL, NULL);
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
      terminates = loopbody(cm, end, begin);
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
      terminates = loopbody(cm, end, tr);
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
            localdecl(cm, 1);
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

      terminates = loopbody(cm, fl, end);

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
      terminates = genswitch(cm, &ex);
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
            if (isscalar(fn->retty) && !iscomplex(fn->retty)) {
               if (iscomplex(ex.ty)) r = complex2scalar(fn, fn->retty, &ex);
               else r = scalarcvt(fn, fn->retty, ex.ty, exprvalue(fn, &ex));
            } else if (iscomplex(fn->retty)) {
               r = complex2addr_cvt(fn, fn->retty, &ex);
            } else if (fn->retty.t == TYVOID) {
               r = (expreffects(fn, &ex), NOREF);
            } else {
               r = structreturn(fn, &ex);
            }
            if (fn->curblk) /* ret expr could e.g. call a no return function */
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
   ExprStmt:
      stmtterm(cm);
      if (stmtexprty) *stmtexprty = ex.ty;
      EMITS r = compileexpr(fn, &ex, stmtexprval == NULL);
      if (stmtexprval) *stmtexprval = r;
      break;
   }
   freearena(&cm->exarena);
   lexerfreetemps(cm->lx);
   return fn->curblk == NULL;
}

/* parse and compile a function-local declaration */
static void
localdecl(CComp *cm, bool forini)
{
   Expr ini;
   Token tk;
   Function *fn = cm->fn;
   const bool doemit = fn->curblk;
   DeclState st = { DFUNCVAR };

   if (!forini && match(cm, &tk, TKIDENT)) {
      if (match(cm, NULL, ':')) {
         /* <label> ':' */
         deflabel(cm, &tk.span, tk.name);
         stmt(cm, NULL, NULL);
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
               Instr alloc = mkalloca(typesize(decl.ty), declalign(&decl));
               if (fn->curblk) decl.id = addinstr(fn, alloc).i;
               else decl.id = insertinstr(fn->entry, fn->entry->ins.n, alloc).i;
            }
         Initz:
            if (st.varini) {
               int d = putdecl(cm, &decl);
               Type ty = decl.ty;
               bool statik = decl.scls & (SCSTATIC | SCEXTERN);
               ini = initializer(cm, &ty, declalign(&decl), statik ? EVSTATICINI : EVFOLD,
                                 /* globl? */ decl.scls == SCEXTERN, decl.qual, statik ? decl.sym : NULL);
               declsbuf.p[d].ty = ty;
               put = 1;
               pdecl(&st, cm);
               if (!statik) {
                  /* fix alloca for actual size, for implicitly sized arrays */
                  assert(!isincomplete(ty));
                  EMITS instrtab[decl.id] = mkalloca(typesize(ty), declalign(&decl));

                  if (!initcheck(ty, &ini)) {
                     Span span = decl.span;
                     joinspan(&span.ex, ini.span.ex);
                     error(&span, "cannot initialize '%ty' variable with '%ty'",
                           ty, ini.ty);
                  }
                  EMITS {
                     if (ini.t == EINIT || (ty.t == TYARRAY && ini.t == ESTRLIT))
                        geninit(fn, ty, mkref(RTMP, decl.id), &ini, decl.qual & QVOLATILE);
                     else if (isagg(ty))
                        structcopy(fn, ty, mkref(RTMP, decl.id), exprvalue(fn, &ini));
                     else if (iscomplex(ty)) {
                        Type base = typechild(ty);
                        complexstore(fn, base, mkref(RTMP, decl.id),
                              cvt2complex(fn, base, &ini), decl.qual & QVOLATILE);
                     } else {
                        genstore(fn, ty, mkref(RTMP, decl.id),
                              scalarcvt(fn, ty, ini.ty, exprvalue(fn, &ini)), decl.qual & QVOLATILE);
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
                  objnewdat(decl.sym, SLOCAL, Sbss, typesize(decl.ty), declalign(&decl));
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
block(CComp *cm, Ref *stexval, Type *stexty)
{
   Token tk;

   if (stexty) *stexty = mktype(TYVOID);
   while (!match(cm, &tk, '}')) {
      if (stexty) *stexty = mktype(TYVOID);
      if (isdecltok(cm))
         localdecl(cm, 0);
      else
         stmt(cm, stexval, stexty);
   }
   cm->fnblkspan = tk.span;
}

static void
functionbody(CComp *cm, Function *fn, internstr *pnames, const Span *pspans, uchar *pquals)
{
   const TypeData *td = &typedata[fn->fnty.dat];
   const bool doemit = fn->curblk;
   Function *prevfn = cm->fn;
   cm->fn = fn;
   Env e;
   Token tk;
   envdown(cm, &e);

   /* emit Oparam instructions */
   EMITS {
      for (int i = 0; i < td->nmemb; ++i) {
         IRType pty = mkirtype(td->param[i]);
         Ref r = addinstr(fn, mkinstr2(Oparam, pty.isagg ? KPTR : pty.cls,
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
            if (isscalar(arg.ty) && !iscomplex(arg.ty)) {
               arg.id = addinstr(fn, mkalloca(typesize(arg.ty), typealign(arg.ty))).i;
               genstore(fn, arg.ty, mkref(RTMP, arg.id), mkref(RTMP, i), /*volatile*/0);
            } else {
               arg.id = addinstr(fn, mkinstr1(Ocopy, KPTR, mkref(RTMP, i))).i;
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
   block(cm, NULL, NULL);
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
   cm->fn = prevfn;
}

/* top-level declaration */
static void
tldecl(CComp *cm)
{
   DeclState st = { DTOPLEVEL };
   do {
      bool noscls = 0;
      int nerr = nerror;
      Decl decl0 = pdecl(&st, cm),
          *decl = &decl0;

      if (nerror != nerr && st.varini) {
         (void)expr(cm);
         pdecl(&st, cm);
         continue;
      }
      if (st.empty) break;
      if (!decl->scls) {
         noscls = 1;
         decl->scls = SCEXTERN;
      }
      if (!decl->sym) decl->sym = decl->name;
      decl->isdef = st.varini;
      if (st.funcdef) {
         const TypeData *td = &typedata[decl->ty.dat];
         if (td->ret.t != TYVOID && isincomplete(td->ret))
            error(&decl->span, "function definition with incomplete return type '%ty'", td->ret);
         for (int i = 0; i < td->nmemb; ++i) {
            if (td->param[i].t != TYVOID && isincomplete(td->param[i]))
               error(&st.pspans[i], "parameter has incomplete type '%ty'", td->param[i]);
         }
         decl->isdef = 1;
         int idecl = -1;
         if (decl->name) {
            idecl = putdecl(cm, decl);
            decl = &declsbuf.p[idecl];
         } else {
            /* had e.g. 'int () {...}' */
            extern int nerror;
            assert(nerror > 0);
            decl->name = decl->sym = intern("?");
         }
         Function fn = { &cm->fnarena, .name = decl->sym, .symflags = declsymflags(decl),
                         .fnty = decl->ty, .retty = td->ret };
         if (decl->inlin) fn.inlhint |= FNINLC;
         if (hasattr(&decl->attr, ATTRalways_inline)) fn.inlhint |= FNINLALWAYS;
         if (hasattr(&decl->attr, ATTRnoinline)) {
            if (fn.inlhint & FNINLALWAYS)
               error(&decl->span, "'noinline' and 'always_inline' in one function");
            fn.inlhint |= FNINLNEVER;
         }
         irinit(&fn);
         functionbody(cm, &fn, st.pnames, st.pspans, st.pqual);
         if (idecl >= 0)
            decl = &declsbuf.p[idecl];
         if (!nerror && ccopt.dbg.dumpparsed && dumpfilt(&fn.name->c))
            irdump(&fn);
         irfini(&fn);
      } else if (decl->name) {
         int idecl = putdecl(cm, decl);
         decl = &declsbuf.p[idecl];
         if (st.varini) {
            if (isagg(decl->ty) && isincomplete(decl->ty))
               error(&decl->span, "initialization of variable with incomplete type '%ty'", decl->ty);
            Expr ini = initializer(cm, &decl->ty, declalign(decl), EVSTATICINI,
                                   decl->scls != SCSTATIC, decl->qual, decl->sym);
            decl = &declsbuf.p[idecl];
            decl->ty = decl->ty;
            if (decl->scls == SCEXTERN && !noscls) {
               Span span = decl->span;
               joinspan(&span.ex, ini.span.ex);
               warn(&span, "'extern' variable has initializer");
            }
            pdecl(&st, cm);
         } else if (decl->ty.t != TYFUNC && decl->scls != SCTYPEDEF && (decl->scls != SCEXTERN || noscls)) {
            /* tentative definitions */
            if (!objhassym(decl->sym, NULL)) {
               uint size = typesize(decl->ty);
               if (isincomplete(decl->ty)) {
                  if (decl->ty.t == TYARRAY) {
                     warn(&decl->span, "tentative array definition assumed to have one element");
                     size = typesize(typechild(decl->ty));
                     assert(size != 0);
                  } else if (isagg(decl->ty)) {
                     warn(&decl->span, "tentative definition with incomplete type '%ty'", decl->ty);
                     assert(size == 0);
                  } else assert(0);
               }
               if (size) {
                  objnewdat(decl->sym, declsymflags(decl), Sbss,
                            size, declalign(decl));
               }
            }
         }
         decl = &declsbuf.p[idecl];
         if (ccopt.dbg.dumpparsed) bfmt(ccopt.dbg.out, "decl %s : %tq\n", decl->name, decl->ty, decl->qual);
      } else {
         if (ccopt.dbg.dumpparsed && decl->ty.t) bfmt(ccopt.dbg.out, "type %ty\n", decl->ty);
      }
      if (decl->inlin && (decl->scls & SCEXTERN) && !noscls) {
         /* explicit 'extern inline' instructs definition to be emitted in this TU */
         assert(decl->ty.t == TYFUNC);
         markfnneeded(decl->sym);
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
      /* XXX hardcoded va_list impl */
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
      cm->lx->pragma = cm->pragma;
      cm->fnarena = (void *)amem[0].m;
      cm->fnarena->cap = N;
      cm->exarena = (void *)amem[1].m;
      cm->exarena->cap = N;
   }
}

void
ccomp(const char *file)
{
   CComp cm = {&(Lexer){0}, &(PragmaState){0}};
   initcm(&cm, file);
   docomp(&cm);
}

void
cpp(WriteBuf *out, const char *file)
{
   CComp cm = {&(Lexer){0}, &(PragmaState){0}};
   initcm(&cm, file);
   lexerdump(cm.lx, out);
}

/* vim:set ts=3 sw=3 expandtab: */
