#include "c.h"
#include "lex.h"
#include "ir.h"
#include "endian.h"
#include "obj.h"

/** C compiler state **/
struct comp {
   struct lexer lx;
   struct env *env;
   struct arena *fnarena, *exarena;
   struct span fnblkspan;
   uint loopdepth, switchdepth;
   struct block *breakto, *loopcont;
   struct switchstmt *switchstmt;
   struct label *labels;
};

/** Parsing helper functions **/
#define peek(Cm,Tk) lexpeek(&(Cm)->lx,Tk)
static int
lexc(struct comp *cm, struct token *tk)
{
   struct token tk2;
   int t = lex(&cm->lx, tk);
   if (t == TKSTRLIT && peek(cm, &tk2) == TKSTRLIT && tk2.wide == tk->wide) {
      /* 5.1.1.2 Translation phase 6: concatenate adjacent string literal tokens */
      static char buf[200];
      vec_of(char) rest = VINIT(buf, sizeof buf);
      do {
         lex(&cm->lx, NULL);
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
         } else if (tk->wide && targ_primsizes[targ_wchartype] == 2) {
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
   return t;
}
#define lex(Cm,Tk) lexc(Cm,Tk)
static bool
match(struct comp *cm, struct token *tk, enum toktag t)
{
   if (peek(cm, NULL) == t) {
      lex(cm, tk);
      return 1;
   }
   return 0;
}
static bool
expect(struct comp *cm, enum toktag t, const char *s)
{
   struct token tk;
   if (!match(cm, &tk, t)) {
      peek(cm, &tk);
      if (aisprint(t)) tk.span.ex.len = tk.span.sl.len = 1;
      error(&tk.span, "expected %'tt%s%s", t, s?" ":"",s ? s : "");
      return 0;
   }
   return 1;
}
static struct token
expectdie(struct comp *cm, enum toktag t, const char *s)
{
   struct token tk;
   if (!match(cm, &tk, t))
      fatal(&tk.span, "expected %'tt%s%s", t, s?" ":"",s ? s : "");
   return tk;
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
struct declstate {
   enum declkind kind;
   union type base;
   enum storageclass scls;
   enum qualifier qual;
   uint align;
   bool more, /* caller should keep calling pdecl to get next decl */
        varini, /* caller should parse an initializer ('=' <ini>) and
                   call pdecl() to advance state before checking .more */
        funcdef, /* caller should parse an func definition ('{' <body> '}').
                    the declaration list is finished. */
        bitf, /* caller should parse a bitfield size and
                 call pdecl() to advance state before checking .more */
        tagdecl;
   const char **pnames; /* param names for function definition */
   struct span *pspans; /* param spans ditto */
};
static struct decl pdecl(struct declstate *st, struct comp *cm);

static struct decl *finddecl(struct comp *cm, const char *name);

/* next token starts a decl? */
static bool
isdecltok(struct comp *cm)
{
   struct decl *decl;
   struct token tk;
   switch (peek(cm, &tk)) {
   case TKWsigned: case TKWunsigned: case TKWshort: case TKWlong:
   case TKWint: case TKWchar: case TKW_Bool: case TKWauto:
   case TKWstruct: case TKWunion: case TKWenum: case TKWtypedef:
   case TKWextern: case TKWstatic: case TKWinline: case TKW_Noreturn:
   case TKWconst: case TKWvolatile: case TKWvoid: case TKWfloat:
   case TKWdouble: case TKWregister: case TKW_Static_assert:
   case TKW__typeof__: case TKWtypeof: case TKWtypeof_unqual:
      return 1;
   case TKIDENT:
      return (decl = finddecl(cm, tk.s)) && decl->scls == SCTYPEDEF;
   }
   return 0;
}


/**********************************/
/* Environment (scope) management */
/**********************************/

static struct decl envdeclsbuf[1<<10];
static vec_of(struct decl) envdecls = VINIT(envdeclsbuf, arraylength(envdeclsbuf));
struct tagged { /* a tagged type declaration */
   union type ty;
   struct span span;
};
static struct tagged envtaggedbuf[1<<10];
static vec_of(struct tagged) envtagged = VINIT(envtaggedbuf, arraylength(envtaggedbuf));
struct env {
   struct env *up;
   /* list of decls is implicitly envdecls[decl..ndecl] */
   ushort decl, ndecl;
   /* ditto for envtagged[] */
   ushort tagged, ntagged;
};
static struct env toplevel;

static void
envdown(struct comp *cm, struct env *e)
{
   assert(cm->env->decl + cm->env->ndecl == envdecls.n);
   assert(cm->env->tagged + cm->env->ntagged == envtagged.n);
   e->decl = envdecls.n;
   e->tagged = envtagged.n;
   e->ndecl = e->ntagged = 0;
   e->up = cm->env;
   cm->env = e;
}

static void
envup(struct comp *cm)
{
   struct env *env = cm->env;
   assert(env->decl + env->ndecl == envdecls.n);
   envdecls.n -= env->ndecl;
   envtagged.n -= env->ntagged;
   assert(env->up);
   cm->env = env->up;
}

static struct decl *
envadddecl(struct env *env, const struct decl *d)
{
   assert(env->decl + env->ndecl == envdecls.n);
   vpush(&envdecls, *d);
   ++env->ndecl;
   return &envdecls.p[envdecls.n - 1];
}

/* iters in reversed order of insertion (most to least recent) */
/* use like so: for (d = NULL; enviterdecl(&d, env);) ... */
static inline bool
enviterdecl(struct decl **d, struct env *env)
{
   if (!env->ndecl) return 0;
   if (!*d) *d = &envdecls.p[env->decl + env->ndecl - 1];
   else if (*d == &envdecls.p[env->decl]) return 0;
   else --*d;
   return 1;
}

static struct tagged *
envaddtagged(struct env *env, union type ty, const struct span *span)
{
   struct tagged tagged = { ty, *span };
   assert(env->tagged + env->ntagged == envtagged.n);
   vpush(&envtagged, tagged);
   ++env->ntagged;
   return &envtagged.p[envtagged.n - 1];
}

/* like enviterdecl */
static inline bool
envitertagged(struct tagged **l, struct env *env)
{
   if (!env->ntagged) return 0;
   if (!*l) *l = &envtagged.p[env->tagged + env->ntagged - 1];
   else if (*l == &envtagged.p[env->tagged]) return 0;
   else --*l;
   return 1;
}

static bool
redeclarationok(const struct decl *old, const struct decl *new)
{
   if (old->scls != new->scls) return 0;
   switch (old->scls) {
   case SCSTATIC:
      if (old->ty.t != TYFUNC)
         break;
      /*fallthru*/
   case SCEXTERN:
      if (old->ty.t == TYARRAY && new->ty.t == TYARRAY
       && typechild(old->ty).bits == typechild(new->ty).bits
       && isincomplete(old->ty))
      {
         return 1;
      }
      /*fallthru*/
   case SCTYPEDEF:
      return old->ty.bits == new->ty.bits;
   }
   return 0;
}

static struct decl *
putdecl(struct comp *cm, const struct decl *decl)
{
   struct decl *l;
   for (l = NULL; enviterdecl(&l, cm->env);) {
      if (decl->name == l->name) {
         if (l->isdef && decl->isdef) {
            error(&decl->span, "redefinition of '%s'", decl->name);
            note(&l->span, "previously defined here");
            break;
         } else if (!redeclarationok(l, decl)) {
            error(&decl->span, "incompatible redeclaration of '%s'", decl->name);
            note(&l->span, "previously declared here");
            break;
         }
      }
   }
   l = envadddecl(cm->env, decl);
   return l;
}

static struct decl *
finddecl(struct comp *cm, const char *name)
{
   struct env *e;
   struct decl *l;
   assert(name);
   for (e = cm->env; e; e = e->up) {
      for (l = NULL; enviterdecl(&l, e);) {
         if (name == l->name)
            return l;
      }
   }
   return NULL;
}

static union type
gettagged(struct comp *cm, struct span *span, enum typetag tt, const char *name, bool dodef)
{
   struct env *e;
   struct tagged *l;
   struct typedata td = {0};
   assert(name);
   for (e = cm->env; e; e = e->up) {
      for (l = NULL; envitertagged(&l, e);) {
         if (name == ttypenames[typedata[l->ty.dat].id]) {
            if (dodef && e != cm->env)
               goto Break2;
            *span = l->span;
            return l->ty;
         }
      }
   }
Break2:
   if (tt == TYENUM)
      return mktype(0);
   td.t = tt;
   return envaddtagged(cm->env, mktagtype(name, &td), span)->ty;
}

static union type
deftagged(struct comp *cm, struct span *span, enum typetag tt, const char *name, union type ty)
{
   struct tagged *l;
   struct typedata td = {0};
   assert(name);
   for (l = NULL; envitertagged(&l, cm->env);) {
      if (name == ttypenames[typedata[l->ty.dat].id]) {
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
islvalue(const struct expr *ex)
{
   if (ex->t == EGETF) return islvalue(ex->sub);
   return ex->t == ESYM || ex->t == EDEREF || ex->t == EINIT;
}

static union type /* 6.5.2.6 default argument promotions */
argpromote(union type t)
{
   if (isint(t)) t.t = intpromote(t.t);
   else if (t.t == TYFLOAT) t.t = TYDOUBLE;
   else if (t.t == TYARRAY) return mkptrtype(typechild(t), t.flag & TFCHLDQUAL);
   return t;
}

static bool
assigncheck(union type t, const struct expr *src)
{
   if (assigncompat(t, typedecay(src->ty))) return 1;
   if (t.t == TYPTR && iszero(*src)) return 1;
   return 0;
}

static bool
initcheck(union type t, const struct expr *src)
{
   if (assigncheck(t, src)) return 1;
   if (t.bits == src->ty.bits && (src->t == EINIT || src->t == ESTRLIT)) return 1;
   return 0;
}

static void
incdeccheck(enum toktag tt, const struct expr *ex, const struct span *span)
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
castcheck(union type to, const struct expr *ex)
{
   union type src = ex->ty;
   if (to.t == TYVOID) return 1;
   if (isagg(to)) return 0;
   if (to.bits == src.bits) return 1;
   if (isarith(to) && isarith(src)) return 1;
   if (isint(to) && isptrcvt(src)) return 1;
   if (to.t == TYPTR && isint(src)) return 1;
   if (to.t == TYPTR && isptrcvt(src)) return 1;
   return 0;
}

static union type /* 6.5.2.1 Array subscripting */
subscriptcheck(const struct expr *ex, const struct expr *rhs, const struct span *span)
{
   union type ty;
   if (ex->ty.t == TYPTR || ex->ty.t == TYARRAY) {
      if (isincomplete(ty = typechild(ex->ty))) {
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

static void /* 6.5.3.4 The sizeof operator */
sizeofcheck(const struct span *span, union type ty)
{
   if (isincomplete(ty))
      error(span, "cannot apply sizeof to incomplete type '%ty'", ty);
   else if (ty.t == TYFUNC)
      error(span, "cannot apply sizeof to function type '%ty'", ty);
}

static bool /* 6.5.8 Relational operators */
relationalcheck(const struct expr *a, const struct expr *b)
{
   union type t1 = a->ty, t2 = b->ty;
   if (isarith(t1) && isarith(t2)) return 1;
   if (isptrcvt(t1) && isptrcvt(t2)) {
      t1 = typedecay(t1);
      t2 = typedecay(t2);
      return t1.dat == t2.dat;
   }
   return 0;
}

static bool
isnullpo(const struct expr *ex) /* match '0' or '(void *) 0' */
{
   static const union type voidptr = {{ TYPTR, .flag = TFCHLDPRIM, .child = TYVOID }};
   if (ex->t == ECAST && ex->ty.bits == voidptr.bits)
      ex = ex->sub;
   return iszero(*ex);
}

static bool /* 6.5.9 Equality operators */
equalitycheck(const struct expr *a, const struct expr *b)
{
   union type t1 = a->ty, t2 = b->ty;
   if (isarith(t1) && isarith(t2)) return 1;
   if (isptrcvt(t1) && isptrcvt(t2)) {
      t1 = typedecay(t1);
      t2 = typedecay(t2);
      return t1.dat == t2.dat || typechild(t1).t == TYVOID || typechild(t2).t == TYVOID;
   }
   if (isptrcvt(t1) && isnullpo(b)) return 1;
   return isptrcvt(t2) && isnullpo(a);
}

static union type /* 6.5.15 Conditional operator */
condtype(const struct expr *a, const struct expr *b)
{
   union type t1 = typedecay(a->ty), t2 = typedecay(b->ty), s1, s2;
   if (isarith(t1) && isarith(t2)) return cvtarith(t1, t2);
   if (t1.bits == t2.bits) return t1;
   if (t1.t == TYPTR && t2.t == TYPTR) {
      s1 = typechild(t1);
      s2 = typechild(t2);
      if (s1.bits == s2.bits || s2.t == TYVOID || s1.t == TYVOID) {
         return mkptrtype(s1.t == TYVOID ? s1 : s2, (t1.flag | t2.flag) & TFCHLDQUAL);
      }
   }
   if (t1.t == TYPTR && isnullpo(b)) return t1;
   if (isnullpo(a) && t2.t == TYPTR) return t2;
   return mktype(0);
}

static void
bintypeerr(const struct span *span, enum toktag tt, union type lhs, union type rhs)
{
   error(span, "bad operands to %tt: '%ty', '%ty'", tt, lhs, rhs);
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

static union type
bintypecheck(const struct span *span, enum toktag tt, struct expr *lhs, struct expr *rhs)
{
   enum binopclass k = bintab[tt].k;
   union type ty = lhs->ty;

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
         const struct expr swaptmp = *lhs;
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
         union type pointee = typechild(ty);
         if (isincomplete(pointee))
            error(span, "arithmetic on pointer to incomplete type '%ty'", ty);
         else if (pointee.t == TYFUNC)
            error(span, "arithmetic on function pointer '%ty'", ty);
         ty = typedecay(ty);
      } else if (tt == '-' && isptrcvt(ty) && isptrcvt(rhs->ty)) {
         /* ptr - ptr */
         union type pointee1 = typechild(typedecay(ty)),
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
      if (!isint(ty) || !isint(rhs->ty))
         goto Error;
      ty.t = intpromote(ty.t);
      assert(ty.t);
      break;
   case BCEQL:
      if (!equalitycheck(lhs, rhs))
         goto Error;
      ty = mktype(TYINT);
      break;
   case BCCMP:
      if (!relationalcheck(lhs, rhs))
         goto Error;
      ty = mktype(TYINT);
      break;
   case BCLOG: /* 6.5.13-14 Logical AND/OR operator */
      if (!isscalar(typedecay(ty)) || !isscalar(typedecay(rhs->ty)))
         goto Error;
      ty = mktype(TYINT);
      break;
   }
   return (k & BCSET) || !ty.t ? lhs->ty : ty;
}

/****************/
/* Expr Parsing */
/****************/

#define mkexpr(t_,span_,ty_,...) ((struct expr){.t=(t_), .ty=(ty_), .span=(span_), __VA_ARGS__})

static struct expr *
exprdup(struct comp *cm, const struct expr *e)
{
   return alloccopy(&cm->exarena, e, sizeof *e, 0);
}
static struct expr *
exprdup2(struct comp *cm, const struct expr *e1, const struct expr *e2)
{
   struct expr *r = alloc(&cm->exarena, 2*sizeof *r, 0);
   r[0] = *e1, r[1] = *e2;
   return r;
}

static struct expr expr(struct comp *cm);
static struct expr commaexpr(struct comp *cm);

static struct expr /* 6.5.2.2 Function calls */
callexpr(struct comp *cm, const struct span *span_, const struct expr *callee)
{
   struct token tk;
   struct expr ex, arg;
   struct span span = callee->span;
   union type ty = callee->ty;
   const struct typedata *td = &typedata[ty.dat];
   struct expr argbuf[10];
   vec_of(struct expr) args = VINIT(argbuf, arraylength(argbuf));
   bool spanok = joinspan(&span.ex, span_->ex);
   bool printsig = 0;

   if (callee->t == ESYM && !callee->ty.t) { /* implicit function decl.. */
      const char *name = (void *)callee->sym;
      struct decl decl = {
         ty = mkfntype(mktype(TYINT), 0, NULL, NULL, /* kandr */ 1, 0),
         .scls = SCEXTERN, .span = callee->span, .name = name
      };
      warn(&callee->span, "call to undeclared function '%s'", name);
      ((struct expr *)callee)->ty = decl.ty;
      ((struct expr *)callee)->sym = putdecl(cm, &decl);
      td = &typedata[ty.dat];
   }

   if (ty.t == TYPTR) /* auto-deref when calling a function pointer */
      ty = typechild(ty);
   if (ty.t != TYFUNC) error(&callee->span, "calling a value of type '%ty'", callee->ty);
   if (!match(cm, &tk, ')')) for (;;) {
      arg = expr(cm);
      spanok = spanok && joinspan(&span.ex, callee->span.ex);
      if (ty.t == TYFUNC && args.n == td->nmemb && !td->variadic && !td->kandr) {
         error(&arg.span, "too many args to function taking %d params", td->nmemb);
         printsig = 1;
      }
      if (ty.t == TYFUNC && args.n < td->nmemb && !td->kandr) {
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

   if (!td->variadic && !td->kandr && args.n < td->nmemb) {
      error(&tk.span, "not enough args to function taking %d param%s",
            td->nmemb, td->nmemb != 1 ? "s" : "");
      printsig = 1;
   }
   if (printsig) note(&callee->span, "function signature is '%ty'", ty);

   ex = mkexpr(ECALL, span, ty.t == TYFUNC ? td->ret : ty, .narg = args.n,
               .sub = alloc(&cm->exarena, (args.n+1)*sizeof(struct expr), 0));
   ex.sub[0] = *callee;
   memcpy(ex.sub+1, args.p, args.n*sizeof(struct expr));
   vfree(&args);
   return ex;
}

static inline int
tkprec(int tt)
{
   return ((uint)tt < arraylength(bintab)) ?  bintab[tt].prec : 0;
}

static struct expr initializer(struct comp *cm, union type *ty, enum evalmode ev,
                               bool globl, enum qualifier qual, const char *name);

/* parse an expression with the given operator precedence */
/* param ident is a kludge to support block labels without backtracking or extra lookahead
 * see stmt() */
static struct expr
exprparse(struct comp *cm, int prec, const struct token *ident, bool fromstmt)
{
   struct token tk, tk2;
   struct span span;
   struct expr ex, rhs, tmp;
   struct decl *decl;
   union type ty;
   int opprec;
   enum exprkind ek;
   struct {
      struct span span;
      union {
         union type ty; /* cast type */
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
   case '+': case '-': case '~': case '!':
   case '*': case '&': case TKINC: case TKDEC:
   Unops:
      unops[nunop].span = tk.span;
      unops[nunop].t0 = 0;
      unops[nunop].tt = tk.t;
      if (++nunop >= arraylength(unops)) {
         ex = exprparse(cm, 999, NULL, 0);
         break;
      }
      goto Unary;

   /* base exprs */
   case TKNUMLIT:
   case TKCHRLIT:
      ex = mkexpr(ENUMLIT, tk.span, mktype(0), );
      if (!(ty.t = parsenumlit(&ex.u, &ex.f, &tk, 0)))
         error(&tk.span, "bad number literal %'tk", &tk);
      ex.ty.t = ty.t ? ty.t : TYINT;
      break;
   case TKSTRLIT:
      ty = mktype(((const char []){TYCHAR, TYSHORT, TYINT})[tk.wide]);
      ex = mkexpr(ESTRLIT, tk.span, mkarrtype(ty, 0, tk.len+1), .s = { (void *)tk.s, tk.len });
      break;
   case TKIDENT:
   Ident:
      decl = finddecl(cm, tk.s);
      if (!decl) {
         if (peek(cm, NULL) == '(') { /* implicit function decl? */
            ex = mkexpr(ESYM, tk.span, mktype(0), .sym = (void *)tk.s);
         } else {
            error(&tk.span, "undeclared identifier %'tk", &tk);
            ex = mkexpr(ESYM, tk.span, mktype(TYINT), .sym = NULL);
         }
      } else if (decl->scls == SCTYPEDEF) {
         error(&tk.span, "unexpected typename %'tk (expected expression)", &tk);
         ex = mkexpr(ESYM, tk.span, decl->ty, .sym = NULL);
      } else if (decl->isenum) {
         ex = mkexpr(ENUMLIT, tk.span, decl->ty, .i = decl->value);
      } else {
         ex = mkexpr(ESYM, tk.span, decl->ty, .qual = decl->qual, .sym = decl);
      }
      break;

   /* might be unary op or primary expr */
   case '(':
      if (!isdecltok(cm)) { /* (expr) */
         ex = commaexpr(cm);
         expect(cm, ')', NULL);
         break;
      } else { /* (type) expr */
         struct declstate st = { DCASTEXPR };
         struct decl decl = pdecl(&st, cm);
         expect(cm, ')', NULL);
         assert(decl.ty.t);
         if (peek(cm, NULL) == '{') {
            if (ccopt.cstd < STDC99)
               warn(&tk.span, "compound literals are a c99 feature");
            ex = initializer(cm, &decl.ty, (decl.scls & SCSTATIC) ? EVSTATICINI : EVFOLD,
                                 /* globl */ 0, decl.qual, NULL);
            break;
         }
         unops[nunop].span = tk.span;
         unops[nunop].ty = decl.ty;
         if (++nunop >= arraylength(unops)) {
            ex = exprparse(cm, 999, NULL, 0);
            break;
         }
         goto Unary;
      }
   case TKWsizeof:
      span = tk.span;
      if (!match(cm, NULL, '(')) /* sizeof expr */
         goto Unops;
      else if (isdecltok(cm)) { /* sizeof (type) */
         struct declstate st = { DCASTEXPR };
         ty = pdecl(&st, cm).ty;
      } else { /* sizeof (expr) */
         ty = commaexpr(cm).ty;
      }
      peek(cm, &tk);
      if (expect(cm, ')', NULL))
         joinspan(&span.ex, tk.span.ex);
      sizeofcheck(&span, ty);
      ex = mkexpr(ENUMLIT, span, mktype(targ_sizetype), .u = typesize(ty));
      break;
   default:
      fatal(&tk.span, "expected %s (near %'tk)", fromstmt ? "statement" : "expression", &tk);
   }

   /* postfix operators */
Postfix:
   switch (peek(cm, &tk)) {
   default: break;
   case TKINC:
   case TKDEC:
      lex(cm, &tk);
      span = ex.span;
      if (!joinspan(&span.ex, tk.span.ex)) span = tk.span;
      incdeccheck(tk.t, &ex, &span);
      ex = mkexpr(tk.t == TKINC ? EPOSTINC : EPOSTDEC, span, ex.ty, .sub = exprdup(cm, &ex));
      goto Postfix;
   case '[': /* a[subscript] */
      lex(cm, NULL);
      rhs = commaexpr(cm);
      span = ex.span;
      if (!joinspan(&span.ex, tk.span.ex) || !joinspan(&span.ex, ex.span.ex)
       || (peek(cm, &tk2), !joinspan(&span.ex, tk.span.ex)))
         span = tk.span;
      expect(cm, ']', NULL);

      if (isint(ex.ty) && isptrcvt(rhs.ty)) {
         /* swap idx[ptr] -> ptr[idx] */
         tmp = ex;
         ex = rhs;
         rhs = tmp;
      }

      ty = subscriptcheck(&ex, &rhs, &span);
      assert(ty.t);
      if (!iszero(rhs)) {
         tmp.sub = exprdup2(cm, &ex, &rhs);
         tmp.t = EADD;
         tmp.span = span;
         tmp.ty = typedecay(ex.ty);
      }
      tmp.sub = exprdup(cm, iszero(rhs) ? &ex : &tmp);
      tmp.span = span;
      tmp.t = EDEREF;
      tmp.qual = ex.ty.flag & TFCHLDQUAL;
      tmp.ty = ty;
      ex = tmp;
      goto Postfix;
   case '(': /* call(args) */
      lex(cm, &tk);
      span = ex.span;
      ex = callexpr(cm, &span, &ex);
      goto Postfix;
   case TKARROW:
      if (ex.ty.t != TYPTR && ex.ty.t != TYARRAY)
         error(&ex.span, "operand to -> is not a pointer: '%ty'", ex.ty);
      else
         ex = mkexpr(EDEREF, ex.span, typechild(ex.ty), .qual = ex.ty.flag & TFCHLDQUAL,
                     .sub = exprdup(cm, &ex));
      /* fallthru */
   case '.':
      lex(cm, &tk);
      span = ex.span;
      peek(cm, &tk2); /* field name */
      if (!expect(cm, TKIDENT, NULL)) tk2.s = "";
      if (!joinspan(&span.ex, tk.span.ex) || !joinspan(&span.ex, tk2.span.ex))
         span = tk.span;
      if (!isagg(ex.ty)) {
         error(&span, "member access operand is not an aggregate: '%ty'%s", ex.ty,
               ex.ty.t == TYPTR && isagg(typechild(ex.ty)) ? "; did you mean to use '->'?" : "");
      } else {
         struct fielddata fld = {.t = mktype(TYINT)};
         if (*tk2.s && !getfield(&fld, ex.ty, tk2.s))
            error(&span, "'%ty' has no such field: '%s'", ex.ty, tk2.s);
         if (ex.t == EGETF && ex.qual == fld.qual) { /* accumulate */
            ex.span = span;
            ex.ty = fld.t;
            ex.fld.off += fld.off;
            ex.fld.bitoff = fld.bitoff;
            ex.fld.bitsiz = fld.bitsiz;
         } else {
            ex = mkexpr(EGETF, span, fld.t, .qual = ex.qual | fld.qual, .sub = exprdup(cm, &ex),
                        .fld = { fld.off, fld.bitsiz, fld.bitoff });
         }
      }
      goto Postfix;
   }

   /* unary operators (process) */
   while (nunop-- > 0) {
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
               if (isincomplete(ty)) {
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
         case TKWsizeof:
            sizeofcheck(&span, ex.ty);
            ex = mkexpr(ENUMLIT, span, mktype(targ_sizetype), .u = typesize(ex.ty));
            break;
         default: assert(0);
         }
      } else { /* cast */
         ty = unops[nunop].ty;
         if (!castcheck(ty, &ex))
            error(&span, "cannot cast value of type '%ty' to '%ty'", ex.ty, ty);
         ex = mkexpr(ECAST, span, ty, .sub = exprdup(cm, &ex));
      }
   }

   /* binary operators */
   while ((opprec = tkprec(peek(cm, &tk))) >= prec) {
      lex(cm, &tk);
      ek = bintab[tk.t].t;
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
         /* ex ? tmp : rhs */
         struct expr *sub;
         span.sl = tk.span.sl;
         span.ex = ex.span.ex;
         if (!isscalar(ex.ty))
            error(&ex.span, "?: condition is not a scalar type: '%ty'", ex.ty);
         tmp = commaexpr(cm);
         joinspan(&tk.span.ex, tmp.span.ex);
         expect(cm, ':', NULL);
         rhs = expr(cm);
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

static struct expr
expr(struct comp *cm)
{
   return exprparse(cm, bintab['='].prec, NULL, 0); /* non-comma expr */
}

static struct expr
constantexpr(struct comp *cm)
{
   return exprparse(cm, bintab['?'].prec, NULL, 0); /* conditional-expr */
}

static struct expr
commaexpr(struct comp *cm)
{
   return exprparse(cm, 1, NULL, 0);
}

/****************/
/* Initializers */
/****************/

static uint
nmemb(union type ty)
{
   if (ty.t == TYARRAY)
      return typearrlen(ty) ? typearrlen(ty) : -1u;
   if (isagg(ty))
      return typedata[ty.dat].nmemb;
   return 1;
}

static bool
objectp(union type ty)
{
   return isagg(ty) || ty.t == TYARRAY;
}

static bool
chrarrayof(union type ty, union type chld)
{
   assert(isint(chld));
   return ty.t == TYARRAY && isint(typechild(ty)) && typesize(typechild(ty)) == typesize(chld);
}

static union type
membertype(uint *off, uint *bitsiz, uint *bitoff, union type ty, uint idx)
{
   *bitsiz = *bitoff = 0;
   if (!objectp(ty)) {
      *off = 0;
      return ty;
   } else if (ty.t == TYARRAY) {
      *off = typesize(typechild(ty)) * idx;
      return typechild(ty);
   } else if (idx < typedata[ty.dat].nmemb) {
      struct fielddata fld = typedata[ty.dat].fld[idx].f;
      *off = fld.off;
      *bitsiz = fld.bitsiz, *bitoff = fld.bitoff;
      return fld.t;
   }
   *off = ~0u;
   return mktype(0);
}

struct initparser {
   struct initcur {
      union type ty;
      uint idx;
      uint off;
      short prev;
   } buf[32], *cur, *sub;
   struct arena **arena;
   uint arrlen;
   enum evalmode ev;
   bool dyn; /* size is not known until parsing done (implicit array size) */
   union {
      struct init *init; /* for initializer with automatic storage */
      struct {  /* for static storage (dyn = 0) */
         enum section sec;
         uint off;
      };
      struct { /* for static storage (dyn = 1) */
         vec_of(uchar) ddat;
         struct dreloc {
            struct dreloc *link;
            const char *sym;
            vlong addend;
            uint off;
         } *drel;
      };
   };
};

static void
excesscheck(struct initparser *ip, const struct span *span)
{
   union type sub = ip->sub->ty;
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
dumpini(struct initparser *ip)
{
   efmt(">>>\n");
   for (struct initcur *s = ip->buf; s < ip->sub+1; ++s) {
      efmt("  ");
      efmt("%d. [%ty, %u]", s- ip->buf, s->ty, s->idx);
      if (s == ip->cur) efmt(" <-- cursor");
      ioputc(&bstderr, '\n');
   }
   efmt("<<<\n");
}
#endif

static union ref expraddr(struct function *, const struct expr *);
static bool
globsym(union ref *psym, const struct expr *ex)
{
   if (ex->t == EINIT || ex->t == ESTRLIT || (ex->t == ESYM && (ex->sym->scls & (SCSTATIC | SCEXTERN)))) {
      *psym = expraddr(NULL, ex);
      return 1;
   }
   return 0;

}

static void
expr2reloc(union ref *psym, vlong *paddend, const struct expr *ex)
{
   if (ex->t == EADDROF && globsym(psym, ex)) {
      *paddend = 0;
   } else if (ex->t == EADDROF && (ex->sub->t == EGETF && globsym(psym, ex->sub->sub))) {
      *paddend = ex->sub->fld.off;
   } else if (globsym(psym, ex) && in_range(ex->ty.t, TYARRAY, TYFUNC)) {
      *paddend = 0;
   } else if (ex->t == ESUB && globsym(psym, &ex->sub[0]) && isint(ex->sub[1].ty) && ex->sub[1].t == ENUMLIT) {
      *paddend = ex->sub[1].i * typesize(ex->sub[0].ty);
   } else if (ex->t == EADD) {
      for (int swp = 0; swp < 2; ++swp) {
         struct expr *a = &ex->sub[swp], *b = &ex->sub[swp ^ 1];
         if (globsym(psym, a) && isint(b->ty) && b->t == ENUMLIT) {
            *paddend = b->i * typesize(a->ty);
            return;
         }
      }
      goto Fail;
   } else Fail: assert(0 && "non static reloc");
}

static void
iniwrite(struct comp *cm, struct initparser *ip, uint off, union type ty, struct expr *ex)
{
   uchar *p;
   uint bitsiz, bitoff;
   if (ex->ty.t == TYSTRUCT) {
      assert(ty.bits == ex->ty.bits);
      for (uint i = 0, n = nmemb(ex->ty); i < n; ++i) {
         uint suboff;
         union type sub = membertype(&suboff, &bitsiz, &bitoff, ex->ty, i);
         assert(!bitsiz);
         iniwrite(cm, ip, off + suboff, sub, &mkexpr(EGETF, ex->span, sub, .sub = ex));
      }
   } else if (ip->ev == EVSTATICINI) {
      uint siz = typesize(ty);
      if (ip->dyn) {
         if (ip->ddat.n < off + siz) {
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
         struct expr *e = ex, tmp;
         if (ex->ty.bits != ty.bits && ty.t != TYPTR) {
            tmp = mkexpr(ECAST, ex->span, ty, .sub = ex);
            e = &tmp;
            eval(e, EVSTATICINI);
            assert(e->t == ENUMLIT);
         }
         // efmt("#%u' wr %lx at %u\n", ip->dyn?0:ip->off, e->u, off);
         // ioflush(&bstderr);
         switch (siz) {
         default: assert(0);
         case 1: *p = e->u; break;
         case 2: wr16targ(p, e->u); break;
         case 4: isint(ty) ? wr32targ(p, e->u) : wrf32targ(p, e->f); break;
         case 8: isint(ty) ? wr64targ(p, e->u) : wrf64targ(p, e->f); break;
         }
      } else if (ty.t == TYARRAY && ex->t == ESTRLIT) {
         uint n = ex->s.n * typesize(typechild(ty));
         if (siz < n) n = siz;
         /* XXX endian for wide strs */
         memcpy(p, ex->s.p, n);
      } else {
         union ref sym;
         vlong addend;
         //efmt("<<> %ty <- %ty\n", ty, ex->ty);
         expr2reloc(&sym, &addend, ex);
         assert(sym.t == RXCON);
         if (!ip->dyn) {
            objreloc(xcon2sym(sym.i), targ_64bit ? REL_ABS64 : REL_ABS32,
                     ip->sec, ip->off + off, addend);
         } else {
            struct dreloc *rel = alloc(ip->arena, sizeof *rel, 0);
            rel->link = ip->drel;
            rel->sym = xcon2sym(sym.i);
            rel->off = off;
            rel->addend = addend;
            ip->drel = rel;
         }
      }
   } else {
      struct init *init = ip->init;
      struct initval val = {
         .off = off,
         .ex = *ex
      }, *new = alloccopy(&cm->exarena, &val, sizeof val, 0);
      *init->tail = new;
      init->tail = &new->next;
      for (uint i = off, end = i + typesize(ex->ty); i < end; ++i) {
         if (BSSIZE(end) > arraylength(init->zero)) break;
         bsclr(init->zero, i);
      }
   }
}

static bool
iniwriterec(struct comp *cm, struct initparser *ip, uint off, struct expr *ex)
{
   for (struct initval *v = ex->init->vals; v; v = v->next) {
      if (v->ex.t == EINIT) iniwriterec(cm, ip, off + v->off, &v->ex);
      else if (ip->ev && !eval(&v->ex, ip->ev) && ip->ev != EVFOLD) return 0;
   }
   return 1;
}

static struct initcur *
iniadvance(struct initparser *ip, struct initcur *c, const struct span *span)
{
   if (c - ip->buf >= arraylength(ip->buf) - 1)
      fatal(span, "too many nested initializers");
   return c + 1;
}

/* set the initializer cursor object */
static void
inifocus(struct initparser *ip, struct comp *cm, const struct span *span, uint idx)
{
   uint off, bitsiz, bitoff;
   union type targ = membertype(&off, &bitsiz, &bitoff, ip->sub->ty, idx);
   struct initcur *next = iniadvance(ip, ip->cur, span);
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
inistrlit(struct comp *cm, struct expr *ex, union type *ty)
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
ininext(struct initparser *ip, struct comp *cm)
{
   uint off, bitsiz, bitoff;
   union type targ;
   struct expr ex = expr(cm);

Retry:
   targ = membertype(&off, &bitsiz, &bitoff, ip->sub->ty, ip->sub->idx);
   assert(!bitsiz);

   if (isagg(ip->sub->ty) && targ.t == TYARRAY && !typearrlen(targ)) {
      error(&ex.span, "cannot initialize flexible array member");
      ++ip->sub->idx;
      return;
   }
   if (ex.t == ESTRLIT && chrarrayof(targ, typechild(ex.ty))) {
      assert(!isincomplete(targ));
      inistrlit(cm, &ex, &targ);
      iniwrite(cm, ip, ip->sub->off + off, targ, &ex);
      ++ip->sub->idx;
      return;
   } else if (ex.t == ESTRLIT && ip->sub->idx == 0 && chrarrayof(ip->sub->ty, typechild(ex.ty))) {
      /* handle e.g. (char []){"foo"} */
      assert(off == 0);
      targ = ip->sub->ty;
      inistrlit(cm, &ex, &targ);
      iniwrite(cm, ip, ip->sub->off, targ, &ex);
      if (ip->sub == ip->buf && ip->arrlen < ex.s.n+1)
         ip->arrlen = ex.s.n+1;
      --ip->sub;
      return;
   } else if (ip->sub->idx >= nmemb(ip->sub->ty) && ip->sub != ip->cur) {
      --ip->sub;
      goto Retry;
   } else if (objectp(targ) && targ.bits != ex.ty.bits) {
      struct initcur *next = iniadvance(ip, ip->sub, &ex.span);
      if (ip->sub - ip->buf == arraylength(ip->buf) - 1)
         fatal(&ex.span, "too many nested initializers");
      ++ip->sub->idx;
      *next = (struct initcur) { targ, .off = ip->sub->off + off };
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
            struct expr *pex = &ex;
            if (ip->ev != EVSTATICINI) {
               if (ex.ty.bits != targ.bits)
                  ex = mkexpr(ECAST, ex.span, targ, .sub = exprdup(cm, &ex));
               pex = exprdup(cm, &ex);
            }
            iniwrite(cm, ip, ip->sub->off + off, targ, pex);
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
aggdesignator(struct initparser *ip, union type ty, const char *name, const struct span *span)
{
   const struct typedata *td = &typedata[ty.dat];
   for (int i = 0; i < td->nmemb; ++i) {
      struct namedfield *fld = &td->fld[i];
      if (fld->name == name) {
         return i;
      } else if (!fld->name) {
         int save, sub;
         struct initcur *next = iniadvance(ip, ip->sub, span);
         save = ip->sub->idx;
         ip->sub->idx = i+1;
         *next = (struct initcur) { fld->f.t, .off = ip->sub->off + fld->f.off };
         ip->sub = next;
         sub = aggdesignator(ip, fld->f.t, name, span);
         if (sub == -1) {
            --ip->sub;
            ip->sub->idx = save;
         }
         else return sub;
      }
   }
   return -1;
}

static bool
designators(struct initparser *ip, struct comp *cm)
{
   struct token tk;
   struct span span;
   bool some = 0;

   for (;;) {
            uint off, bitsiz, bitoff;
      uvlong idx = ~0ull;
      if (match(cm, &tk, '[')) {
         struct expr ex = commaexpr(cm);
         span = tk.span;
         joinspan(&span.ex, ex.span.ex);
         peek(cm, &tk);
         if (some) {
            union type ty = membertype(&off, &bitsiz, &bitoff, ip->sub->ty, ip->sub->idx++);
            struct initcur *next = iniadvance(ip, ip->sub, &tk.span);
            assert(!bitsiz);
            *next = (struct initcur) { ty, .off = ip->sub->off + off };
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
            union type ty = membertype(&off, &bitsiz, &bitoff, ip->sub->ty, ip->sub->idx++);
            struct initcur *next = iniadvance(ip, ip->sub, &tk.span);
            *next = (struct initcur) { ty, .off = ip->sub->off + off };
            ip->sub = next;
            dumpini(ip);
         }
         if (expect(cm, TKIDENT, NULL)) joinspan(&span.ex, tk.span.ex);
         if (!isagg(ip->sub->ty))
            error(&span, "member designator used with non-aggregate type '%ty'", ip->sub->ty);
         else if (tk.t == TKIDENT) {
            do {
               idx = aggdesignator(ip, ip->sub->ty, tk.s, &span);
               if (idx >= 0) break;
               if (ip->sub != ip->cur && !ttypenames[typedata[ip->sub->ty.dat].id]) {
                  /* if in anonymous aggregate, go up and look again */
                  --ip->sub;
                  continue;
               }
            } while (0);
            ip->sub->idx = idx;
            if (idx < 0)
               error(&span, "%ty has no such field: '%s'", ip->cur->ty, tk.s);
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

static struct expr
initializer(struct comp *cm, union type *ty, enum evalmode ev, bool globl,
            enum qualifier qual, const char *name)
{
   struct token tk;
   struct span span;
   struct init res = {0};
   struct initparser ip[1] = {0};

   ip->arena = &cm->exarena;
   ip->ev = ev;
   if (ev == EVSTATICINI) {
      if (ty->t == TYARRAY && !typearrlen(*ty)) {
         ip->dyn = 1;
      } else {
         ip->sec = qual & QCONST ? Srodata : Sdata;
         ip->off = objnewdat(name, ip->sec, globl, typesize(*ty), typealign(*ty));
      }
   } else {
      ip->init = &res;
      res.tail = &res.vals;
   }

   if (!match(cm, &tk, '{')) {
      struct expr ex = expr(cm);
      if (ex.t == ESTRLIT && chrarrayof(*ty, typechild(ex.ty))) {
         inistrlit(cm, &ex, ty);
         iniwrite(cm, ip, 0, *ty, &ex);
         if (ip->dyn)
            goto Dynfix;
      }
      if (!initcheck(*ty, &ex))
         error(&ex.span, "cannot initialize '%ty' with expression of type '%ty'", *ty, ex.ty);
      else {
         if (ev && !eval(&ex, ev) && ev != EVFOLD)
            error(&ex.span, "cannot evaluate expression statically");
         else
            iniwrite(cm, ip, 0, *ty, &ex);
      }
      return ex;
   }

   assert(arraylength(res.zero) == 1);
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
         struct span span = tk.span;
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
   }
   if (ip->dyn) {
      enum section sec;
      uint off, siz, align;
      uchar *p;
      uint len = ip->arrlen > ip->cur->idx ? ip->arrlen : ip->cur->idx;

      if (len == 0)
         error(&span, "array cannot have zero length");
      *ty = mkarrtype(typechild(*ty), ty->flag & TFCHLDQUAL, len);
   Dynfix:
      sec = qual & QCONST ? Srodata : Sdata;
      off = objnewdat(name, sec, globl, siz = typesize(*ty), align = typealign(*ty));
      p = sec == Srodata ? objout.rodata.p : objout.data.p;
      memcpy(p + off, ip->ddat.p, ip->ddat.n);
      memset(p + off + ip->ddat.n, 0, typesize(*ty) - ip->ddat.n);
      vpush(&dattab, ((struct irdat) {
         align, globl, sec, siz, off, name
      }));
      vfree(&ip->ddat);
      for (struct dreloc *rel = ip->drel; rel; rel = rel->link) {
         objreloc(rel->sym, targ_64bit ? REL_ABS64 : REL_ABS32, sec, off + rel->off, rel->addend);
      }
   }
   dumpini(ip);

   if (ev == EVSTATICINI) {
      return (struct expr){0};
   } else {
      uint siz;
      if (isincomplete(*ty)) {
         if (!ip->arrlen)
            error(&span, "initializer creates a zero-sized array");
         *ty = mkarrtype(typechild(*ty), ty->flag & TFCHLDQUAL, ip->arrlen > 0 ? ip->arrlen : 1);
      }

      assert(arraylength(res.zero) == 1);
      siz = typesize(*ty);
      if (siz && siz <= 64)
         res.zero->u &= ~0ull >> (64 - siz);

      return mkexpr(EINIT, span, *ty, .init = alloccopy(&cm->exarena, &res, sizeof res, 0));
   }
}

/*****************/
/* Decls Parsing */
/*****************/

static union type
buildagg(struct comp *cm, enum typetag tt, const char *name, int id)
{
   struct token tk;
   union type t;
   struct span flexspan;
   struct namedfield fbuf[32];
   vec_of(struct namedfield) fld = VINIT(fbuf, arraylength(fbuf));
   struct typedata td = {tt};
   bool isunion = tt == TYUNION;
   const char *tag = isunion ? "union" : "struct";
   uint bitsiz = 0, bitfbyteoff = 0,
        bitoff = 0, bitftypesiz = 0;

   while (!match(cm, &tk, '}')) {
      struct declstate st = { DFIELD };
      do {
         struct decl decl = pdecl(&st, cm);
         uint tysize = typesize(decl.ty);
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
            struct expr ex = constantexpr(cm);
            const char *name = decl.name ? decl.name : "<anonymous>";
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
                  if (bitftypesiz) {
                     bitfbyteoff += bitftypesiz;
                     bitfbyteoff = alignup(bitfbyteoff, typealign(decl.ty));
                  }
                  bitoff = 0;
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
            struct namedfield f = { decl.name, { decl.ty, off, bitsiz, bitoff, .qual = decl.qual }};
            if (bitftypesiz && siz != bitftypesiz) while (f.f.bitoff + f.f.bitsiz > 8*siz) {
               /* adjust bitfields narrower than container type */
               f.f.off += siz;
               f.f.bitoff -= 8*siz;
            }
            if (!decl.name && !bitftypesiz) {
               if (!isagg(decl.ty) || ttypenames[typedata[decl.ty.dat].id]) {
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
      struct namedfield dummy = { "", { mktype(TYCHAR), 0 }};
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
static union type
buildenum(struct comp *cm, const char *name, const struct span *span)
{
   struct token tk;
   vlong tymin, minv = 0;
   uvlong tymax, maxv = 0;
   struct typedata td = {TYENUM, .backing = TYINT};
   union type ty = mktype(td.backing);
   struct span maxvspan;
   vlong iota = 0;
   bool somelonglong = 0;

   inttyminmax(&tymin, &tymax, td.backing);
   while (!match(cm, &tk, '}')) {
      struct decl decl = {0};
      peek(cm, &tk);
      expect(cm, TKIDENT, NULL);
      if (match(cm, NULL, '=') || (peek(cm, NULL) == TKNUMLIT && !expect(cm, '=', NULL))) {
         struct expr ex = expr(cm);
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

      decl.name = tk.s;
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
   for (int t = TYINT; t <= TYUVLONG; ++t) {
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

   ty = mktagtype(name, &td);
   ty.backing = td.backing;
   return ty;
}

static union type
tagtype(struct comp *cm, enum toktag kind)
{
   struct token tk;
   union type t;
   struct span span;
   enum typetag tt = kind == TKWenum ? TYENUM : kind == TKWstruct ? TYSTRUCT : TYUNION;
   const char *tag = NULL;

   peek(cm, &tk);
   if (match(cm, &tk, TKIDENT))
      tag = tk.s;
   span = tk.span;
   if (!match(cm, NULL, '{')) {
      if (!tag) {
         error(&tk.span, "expected %tt name or '{'", kind);
         return mktype(0);
      }
      t = gettagged(cm, &span, tt, tag, /* def? */ peek(cm, NULL) == ';');
      if (tt == TYENUM && !t.t) {
         error(&tk.span, "cannot forward-declare enum");
         return mktype(TYINT);
      }
   } else {
      if (tt != TYENUM) {
         if (tag) {
            t = deftagged(cm, &span, tt, tag, mktype(0));
            if (t.t != tt || !isincomplete(t)) {
               if (t.t != tt)
                  error(&tk.span,
                        "defining tagged type %'tk as %tt clashes with previous definition",
                        &tk, kind);
               else
                  error(&tk.span, "redefinition of '%tt %s'", kind, tag, mktype(0));
               note(&span, "previous definition:");
            }
         }
         t = buildagg(cm, tt, tag, tag ? typedata[t.dat].id : -1);
      } else {
         t = buildenum(cm, tag, &span);
         if (tag) deftagged(cm, &span, TYENUM, tag, t);
      }
   }

   if (t.t != tt) {
      error(&tk.span, "declaring tagged type %'tk as %tt clashes with previous definition",
            &tk, kind);
      note(&span, "previous definition:");
   }
   return t;
}

static union type
ptypeof(struct comp *cm)
{
   union type ty;
   expect(cm, '(', NULL);
   if (isdecltok(cm)) { /* typeof (type) */
      struct declstate st = { DCASTEXPR };
      ty = pdecl(&st, cm).ty;
   } else { /* typeof (expr) */
      ty = commaexpr(cm).ty;
   }
   expect(cm, ')', NULL);
   return ty;
}

static void
declspec(struct declstate *st, struct comp *cm)
{
   struct token tk;
   struct decl *decl;
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
   } arith = 0;
   struct span span = {0};

   for (;;) {
      peek(cm, &tk);
      switch (tk.t) {
      case TKWconst:
         st->qual |= QCONST;
         break;
      case TKWrestrict:
         /* unimplemented */
         /*st->qual |= QRESTRICT;*/
         break;
      case TKWvolatile:
         st->qual |= QVOLATILE;
         break;
      case TKW_Noreturn:
         st->qual |= QNORETURN;
         break;
      case TKWinline:
         st->qual |= QINLINE;
         break;
      case TKWvoid:
         st->base = mktype(TYVOID);
         break;
      case TKWsigned:
         arith |= KSIGNED;
         break;
      case TKWunsigned:
         arith |= KUNSIGNED;
         break;
      case TKW_Bool:
      case TKWbool:
         if (arith & KBOOL) goto Dup;
         arith |= KBOOL;
         break;
      case TKWchar:
         if (arith & KCHAR) {
         Dup:
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
         if (arith & KINT) goto Dup;
         arith |= KINT;
         break;
      case TKWfloat:
         if (arith & KFLOAT) goto Dup;
         arith |= KFLOAT;
         break;
      case TKWdouble:
         if (arith & KDOUBLE) goto Dup;
         arith |= KDOUBLE;
         break;
      case TKWenum:
      case TKWstruct:
      case TKWunion:
         lex(cm, &tk);
         st->base = tagtype(cm, tk.t);
         st->tagdecl = 1;
         if (!span.ex.len) span.ex = tk.span.ex;
         joinspan(&span.ex, tk.span.ex);
         goto End;
      case TKW__typeof__: case TKWtypeof:
         lex(cm, &tk);
         st->base = ptypeof(cm);
         if (!span.ex.len) span.ex = tk.span.ex;
         joinspan(&span.ex, tk.span.ex);
         goto End;
      case TKIDENT:
         if (!st->base.t && !arith && (decl = finddecl(cm, tk.s))
             && decl->scls == SCTYPEDEF) {
            st->base = decl->ty;
            break;
         }
         /* fallthru */
      default:
         if (!span.ex.len) span.ex = tk.span.ex;
         goto End;
      case TKW_BitInt: case TKW_Complex:
      case TKW_Decimal128: case TKW_Decimal32:
      case TKW_Decimal64: case TKW_Imaginary:
         error(&tk.span, "%'tk is unsupported", &tk);
         arith = arith ? arith : KINT;
      }
      if (!span.ex.len) span.ex = tk.span.ex;
      joinspan(&span.ex, tk.span.ex);
      lex(cm, &tk);
      if (st->base.t) break;
   }
End:
   if (st->base.t && arith) {
      /* combining arith type specifiers and other types */
   Bad:
      error(&span, "invalid declaration specifier");
      st->base = mktype(TYINT);
   } else if (!st->base.t && arith) {
      enum typetag t;
      if (arith == KFLOAT)
         t = TYFLOAT;
      else if (arith == KDOUBLE)
         t = TYDOUBLE;
      else if (arith == (KLONG | KDOUBLE)) {
         t = TYLDOUBLE;
      } else if (arith == KBOOL)
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
      else
         goto Bad;
      st->base = mktype(t ? t : TYINT);
   } else if (!st->base.t && ccopt.cstd < STDC23) {
      warn(&span, "type implicitly declared as int");
      st->base = mktype(TYINT);
   } else if (!st->base.t)
      fatal(&span, "expected declaration type specifier");
}

/* circular doubly linked list used to parse declarators */
static struct decllist {
   struct decllist *prev, *next;
   uchar t; /* TYPTR, TYARRAY or TYFUNC */
   union {
      uchar qual; /* TYPTR */
      uint len; /* TYARRAY */
      struct { /* TYFUNC */
         union type *param;
         const char **pnames;
         struct span *pspans;
         uchar *pqual;
         short npar;
         bool kandr : 1, variadic : 1;
      };
   };
   struct span span;
} decltmp[64], *declfreelist;
static union type declparamtmp[16];
static const char *declpnamestmp[16];
static struct span declpspanstmp[16];
static uchar declpqualtmp[tdqualsiz(16)];

static void
declinsert(struct decllist *list, const struct decllist *node)
{
   struct decllist *pnode = declfreelist;
   if (!pnode) fatal(NULL, "too many nested declarators");
   declfreelist = declfreelist->next;
   *pnode = *node;
	pnode->next = list->next;
	pnode->prev = list;
	list->next->prev = pnode;
	list->next = pnode;
}

static int
sclass(struct comp *cm, struct span *span)
{
   struct token tk;
   int sc = 0, first = 1;
   for (;; lex(cm, &tk)) {
      switch (peek(cm, &tk)) {
      case TKWtypedef:  sc |= SCTYPEDEF; break;
      case TKWextern:   sc |= SCEXTERN; break;
      case TKWstatic:   sc |= SCSTATIC; break;
      case TKWauto:     sc |= SCAUTO; break;
      case TKWregister: sc |= SCREGISTER; break;
      case TKWthread_local:
      case TKW_Thread_local:
         sc |= SCTHREADLOCAL; break;
      default: return sc;
      }
      if (first) *span = tk.span;
      else joinspan(&span->ex, tk.span.ex);
      first = 0;
   }
}

static int
cvqual(struct comp *cm)
{
   struct token tk;
   int q = 0;
   while (match(cm, &tk, TKWconst) || match(cm, &tk, TKWvolatile) || match(cm, &tk, TKWrestrict))
      q |= tk.t == TKWconst ? QCONST : tk.t == TKWvolatile ? QVOLATILE : 0;
   return q;
}

static void
decltypes(struct comp *cm, struct decllist *list, const char **name, struct span *span) {
   struct token tk;
   struct decllist *ptr, node;

   while (match(cm, &tk, '*')) {
      node.t = TYPTR;
      node.qual = cvqual(cm);
      node.span = tk.span;
      declinsert(list, &node);
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
         node.variadic = 0;
         node.kandr = 1;
         node.npar = 0;
         declinsert(ptr->prev, &node);
         break;
      } else {
         decltypes(cm, list, name, span);
         expect(cm, ')', NULL);
      }
      break;
   case TKIDENT:
      if (!name)
         error(&tk.span, "unexpected identifier in type name");
      else {
         *name = tk.s;
         *span = tk.span;
      }
      lex(cm, &tk);
      break;
   default:
      *span = tk.span;
      if (name)
         *name = NULL;
   }
   for (;;) {
      if (match(cm, &tk, '[')) {
         node.span = tk.span;
         uint n = 0;
         if (!match(cm, &tk, ']')) {
            struct expr ex = expr(cm);
            if (!eval(&ex, EVINTCONST)) {
               error(&ex.span, "array length is not an integer constant");
            } else if (typesize(ex.ty) < 8 && ex.i < 0) {
               error(&ex.span, "array length is negative");
            } else if (ex.u > (1ull << (8*sizeof n)) - 1) {
               error(&ex.span, "array too long (%ul)", ex.u);
            } else if (ex.u == 0) {
               error(&ex.span, "array cannot have zero length");
            } else {
               n = ex.u;
            }
            peek(cm, &tk);
            joinspan(&node.span.ex, tk.span.ex);
            expect(cm, ']', NULL);
         }
         node.t = TYARRAY;
         node.len = n;
         declinsert(ptr->prev, &node);
      } else if (match(cm, &tk, '(')) Func: {
         static int depth = 0;
         vec_of(union type) params = {0};
         vec_of(uchar) qual = {0};
         vec_of(const char *) names = {0};
         vec_of(struct span) spans = {0};
         bool anyqual = 0;

         if (depth++ == 0) {
            vinit(&params, declparamtmp, arraylength(declparamtmp));
            vinit(&qual, declpqualtmp, arraylength(declpqualtmp));
            vinit(&names, declpnamestmp, arraylength(declpnamestmp));
            vinit(&spans, declpspanstmp, arraylength(declpspanstmp));
         }
         node.span = tk.span;
         node.kandr = 0;
         node.variadic = 0;

         while (!match(cm, &tk, ')')) {
            struct declstate st = { DFUNCPARAM };
            struct decl decl;
            if (match(cm, &tk, TKDOTS)) {
               node.variadic = 1;
               expect(cm, ')', NULL);
               break;
            }
            decl = pdecl(&st, cm);
            decl.ty = typedecay(decl.ty);
            vpush(&params, decl.ty);
            vpush(&names, decl.name);
            vpush(&spans, decl.span);
            if (decl.qual) {
               anyqual = 1;
               while (qual.n < tdqualsiz(params.n)) vpush(&qual, 0);
               tdsetqual(qual.p, params.n-1, decl.qual);
            }
            if (isincomplete(decl.ty)) {
               if (params.n > 1 || decl.ty.t != TYVOID || decl.qual || decl.name) {
                  error(&decl.span,
                        "function parameter #%d has incomplete type (%tq)",
                        params.n, decl.ty, tdgetqual(qual.p, params.n-1));
               }
            }
            joinspan(&node.span.ex, tk.span.ex);
            if (!match(cm, &tk, ',')) {
               expect(cm, ')', NULL);
               break;
            }
         }
         --depth;
         node.kandr = params.n == 0 && ccopt.cstd < STDC23;
         if (params.n == 1 && params.p[0].t == TYVOID && !qual.n && !names.p[0]) { /* (void) */
            vfree(&params);
            vfree(&names);
            vfree(&spans);
         } else if (params.n && params.p[0].t == TYVOID && !qual.n && !names.p[0]) {
            error(&node.span, "function parameter #1 has incomplete type (%tq)",
                  params.p[0], tdgetqual(qual.p, 0));
         }
         node.t = TYFUNC;
         node.param = params.n ? params.p : NULL;
         node.pqual = anyqual ? qual.p : NULL;
         node.pnames = params.n ? names.p : NULL;
         node.pspans = params.n ? spans.p : NULL;
         node.npar = params.n;
         declinsert(ptr->prev, &node);
      } else break;
   }
}

static struct decl
declarator(struct declstate *st, struct comp *cm) {
   struct decl decl = { st->base, st->scls, st->qual, st->align };
   struct decllist list = { &list, &list }, *l;
   static bool inidecltmp = 0;
   if (!inidecltmp) {
      inidecltmp = 1;
      for (int i = 0; i < arraylength(decltmp); ++i) {
         decltmp[i].next = declfreelist;
         declfreelist = &decltmp[i];
      }
   }

   decltypes(cm, &list, st->kind == DCASTEXPR ? NULL : &decl.name, &decl.span);
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
         decl.ty = mkarrtype(decl.ty, decl.qual, l->len);
         break;
      case TYFUNC:
         if (decl.ty.t == TYFUNC)
            error(&decl.span, "function cannot return function type '%ty'", decl.ty);
         else if (decl.ty.t == TYARRAY)
            error(&decl.span, "function cannot return array type", decl.ty);
         else if (decl.ty.t != TYVOID && isincomplete(decl.ty))
            error(&decl.span, "function cannot return incomplete type '%ty'", decl.ty);
         if (l->kandr && ccopt.cstd > STDC89)
            warn(&l->span, "function declaration without a prototype is deprecated");
         decl.ty = mkfntype(decl.ty, l->npar, l->param, l->pqual, l->kandr, l->variadic);
         if (l->param != declparamtmp) free(l->param);
         if (l->pqual != declpqualtmp) free(l->pqual);
         if (l->prev == &list && l->npar) { /* last */
            st->pnames = alloccopy(&cm->fnarena, l->pnames, l->npar * sizeof(char *), 0);
            st->pspans = alloccopy(&cm->fnarena, l->pspans, l->npar * sizeof(struct span), 0);
         }
         if (l->pnames != declpnamestmp) free(l->pnames);
         if (l->pspans != declpspanstmp) free(l->pspans);
         decl.qual = 0;
         break;
      }

      l->next = declfreelist;
      declfreelist = l;
   }

   return decl;
}

static void
pstaticassert(struct comp *cm, struct span *span)
{
   struct expr ex;
   struct token tk, msg = {0};

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

static struct decl
pdecl(struct declstate *st, struct comp *cm) {
   struct token tk;
   struct decl decl;
   bool iniallowed = st->kind != DFIELD && st->kind != DFUNCPARAM && st->kind != DCASTEXPR;
   bool staticassertok = iniallowed;
   bool first = 0;

   assert(!st->funcdef);

   if (st->varini || st->bitf) {
      memset(&decl, 0, sizeof decl);
      goto AfterIniBitf;
   }

   if (!st->base.t) {
      if (staticassertok && (match(cm, &tk, TKW_Static_assert) || match(cm, &tk, TKWstatic_assert))) {
         pstaticassert(cm, &tk.span);
         return decl = (struct decl){0};
      }
      first = 1;
      st->scls = sclass(cm, &tk.span);
      if (popcnt(st->scls) > 1)
         error(&tk.span, "invalid combination of storage class specifiers");
      else {
         int allowed;
         switch (st->kind) {
         case DTOPLEVEL: allowed = SCTYPEDEF | SCEXTERN | SCSTATIC | SCTHREADLOCAL; break;
         case DCASTEXPR: allowed = 0; break;
         case DFIELD: allowed = 0; break;
         case DFUNCPARAM: allowed = 0; break;
         case DFUNCVAR:
            allowed = SCTYPEDEF | SCREGISTER | SCAUTO | SCEXTERN | SCSTATIC | SCTHREADLOCAL;
            break;
         default: assert(0);
         }
         if ((st->scls & allowed) != st->scls)
            error(&tk.span, "this storage class is not allowed in this context");
         st->scls &= allowed;
      }
      declspec(st, cm);
   }
   if (st->scls == SCTYPEDEF) iniallowed = 0;

   if (first && st->tagdecl && match(cm, &tk, ';')) {
      decl = (struct decl) { st->base, st->scls, st->qual, st->align, 0, tk.span };
      return decl;
   } else if (st->kind == DFIELD && match(cm, &tk, ':')) {
      decl = (struct decl) { st->base, st->scls, st->qual, st->align, 0, tk.span };
      st->bitf = 1;
      return decl;
   }
   decl = declarator(st, cm);

   if (iniallowed && match(cm, &tk, '=')) {
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

static union ref expraddr(struct function *, const struct expr *);
static union ref compileexpr(struct function *, const struct expr *, bool discard);
static inline union ref
exprvalue(struct function *fn, const struct expr *ex)
{
   return compileexpr(fn, ex, /*discard*/ 0);
}
static inline void
expreffects(struct function *fn, const struct expr *ex)
{
   compileexpr(fn, ex, /*discard*/ 1);
}

static void
structcopy(struct function *fn, union type ty, union ref dst, union ref src)
{
   union irtype typ = mkirtype(ty);
   addinstr(fn, mkarginstr(typ, dst));
   addinstr(fn, mkarginstr(typ, src));
   addinstr(fn, mkintrin(INstructcopy, 0, 2));
}

static union ref
structreturn(struct function *fn, const struct expr *src)
{
   return expraddr(fn, src);
}

static union ref compilecall(struct function *fn, const struct expr *ex);

static const char *
mkhiddensym(const char *fnname, const char *name, int id)
{
   char buf[200];
   struct wbuf wbuf = MEMBUF(buf, sizeof buf);
   assert(id > 0);
   bfmt(&wbuf, "%s.%s.%d", fnname, name, id-1);
   ioputc(&wbuf, 0);
   assert(!wbuf.err);
   return intern(buf);
}

static void geninit(struct function *fn, union type t, union ref dst, const struct expr *src);

static union ref
expraddr(struct function *fn, const struct expr *ex)
{
   struct decl *decl;
   union ref r;
   struct instr ins = {0};

   switch (ex->t) {
   case ESYM:
      decl = ex->sym;
      assert(decl != NULL);
      switch (decl->scls) {
      case SCAUTO: case SCREGISTER:
         return mkref(RTMP, decl->id);
      case SCEXTERN: case SCNONE:
         return mksymref(decl->name);
      case SCSTATIC:
         if (!decl->id)
            return mksymref(decl->name);
         else
            return mksymref(mkhiddensym(fn->name, decl->name, decl->id));
      default:
         assert(0);
      }
      break;
   case ESTRLIT:
      /* XXX endian for wide strs */
      return mkdatref(NULL, typesize(ex->ty), typealign(ex->ty), ex->s.p, ex->s.n * typesize(typechild(ex->ty)), /*deref*/0);
   case EDEREF:
      return exprvalue(fn, ex->sub);
   case EGETF:
      r = expraddr(fn, ex->sub);
      assert(ex->fld.bitsiz == 0);
      if (ex->fld.off == 0) return r;
      ins.cls = KPTR;
      ins.op = Oadd;
      ins.l = r;
      ins.r = mkintcon(KI4, ex->fld.off);
      return addinstr(fn, ins);
   case ESET:
      assert(isagg(ex->ty));
      r = expraddr(fn, &ex->sub[1]);
      structcopy(fn, ex->ty, expraddr(fn, &ex->sub[0]), r);
      return r;
   case ESEQ:
      expreffects(fn, &ex->sub[0]);
      return expraddr(fn, &ex->sub[1]);
   case ECALL:
      assert(isagg(ex->ty));
      return compilecall(fn, ex);
   case EINIT:
      if (fn) {
         /* compound literal, allocate temp */
         r = addinstr(fn, mkalloca(typesize(ex->ty), typealign(ex->ty)));
         geninit(fn, ex->ty, r, ex);
         return r;
      } else {
      }
   default:
      assert(!"lvalue?>");
   }

}

static union ref
genload(struct function *fn, union type t, union ref ref)
{
   struct instr ins = {0};

   assert(isscalar(t));
   ins.cls = type2cls[scalartypet(t)];
   assert(ins.cls);
   switch (typesize(t)) {
   case 1: ins.op = issigned(t) ? Oloads1 : Oloadu1; break;
   case 2: ins.op = issigned(t) ? Oloads2 : Oloadu2; break;
   case 4: ins.op = isflt(t) ? Oloadf4 : issigned(t) ? Oloads4 : Oloadu4; break;
   case 8: ins.op = isflt(t) ? Oloadf8 : Oloadi8; break;
   default: assert(0);
   }
   ins.l = ref;
   return addinstr(fn, ins);
}

static union ref
genstore(struct function *fn, union type t, union ref ptr, union ref val)
{
   struct instr ins = {0};

   assert(isscalar(t));
   switch (typesize(t)) {
   case 1: ins.op = Ostore1; break;
   case 2: ins.op = Ostore2; break;
   case 4: ins.op = Ostore4; break;
   case 8: ins.op = Ostore8; break;
   default: assert(0);
   }
   ins.l = ptr;
   ins.r = val;
   return addinstr(fn, ins);
}

static void
geninit(struct function *fn, union type t, union ref dst, const struct expr *src)
{
   union ref adr;
   if (src->t == EINIT) {
      struct init *ini = src->init;
      uint siz = typesize(t);
      if (BSSIZE(siz) <= arraylength(ini->zero) && bscount(ini->zero, arraylength(ini->zero)) < 32) {
         /* write individual zeros at non initialized gaps */
         for (uint i = 0; bsiter(&i, ini->zero, arraylength(ini->zero)) && i < siz; ++i) {
            /* TODO coalesce into multibyte zero writes */
            adr = i == 0 ? dst : addinstr(fn, mkinstr(Oadd, KPTR, dst, mkref(RICON, i)));
            genstore(fn, mktype(TYCHAR), adr, ZEROREF);
         }
      } else {
         /* memset(dst,0,siz) */
         /* TODO make it into an intrinsic */
         struct instr call = { Ocall, KPTR };
         addinstr(fn, mkarginstr(cls2type(KPTR), dst));
         addinstr(fn, mkarginstr(cls2type(KI4), ZEROREF));
         addinstr(fn, mkarginstr(cls2type(type2cls[targ_sizetype]), mkintcon(type2cls[targ_sizetype], siz)));
         call.l = mksymref("memset");
         call.r = mkcallarg(cls2type(KPTR), 3, -1);
         addinstr(fn, call);
      }
      for (struct initval *val = ini->vals; val; val = val->next) {
         uint off = val->off;
         struct expr *ex = &val->ex;
         adr = off == 0 ? dst : addinstr(fn, mkinstr(Oadd, KPTR, dst, mkref(RICON, off)));
         genstore(fn, ex->ty, adr, exprvalue(fn, ex));
      }
   } else if (src->t == ESTRLIT) {
      adr = dst;
      for (uint i = 0; i < src->s.n; ++i) {
         genstore(fn, mktype(TYCHAR), adr, mkref(RICON, src->s.p[i]));
         adr = addinstr(fn, mkinstr(Oadd, KPTR, dst, mkref(RICON, i+1)));
      }
      genstore(fn, mktype(TYCHAR), adr, ZEROREF); /* null term */
   } else assert(0);
}

static union ref
cvt(struct function *fn, union type to, union type from, union ref ref)
{
   enum irclass kto = type2cls[scalartypet(to)], kfrom = type2cls[scalartypet(from)];
   struct instr ins = {0};
   assert(kto && kfrom);
   if (kto == kfrom && to.t != TYBOOL) return ref;
   if (ref.t == RICON && kto < KF4) return ref;

   ins.cls = kto;
   ins.l = ref;
   if (kisflt(kto) || kisflt(kfrom)) {
      if (ref.t == RICON) {
         assert(kisflt(kto) && kisint(kfrom));
         return mkfltcon(kto, kto == KF4 ? (float)ref.i : (double)ref.i);
      }
      if (kisflt(kto) && kfrom == KI4) ins.op = issigned(from) ? Ocvts4f : Ocvtu4f;
      else if (to.t == TYBOOL && kisflt(kfrom)) ins.op = Oneq, ins.r = mkfltcon(kfrom, 0.0);
      else if (kisflt(kto) && kfrom == KI8) ins.op = issigned(from) ? Ocvts8f : Ocvtu8f;
      else if (kto == KF8 && kfrom == KF4) ins.op = Ocvtf4f8;
      else if (kto == KF4 && kfrom == KF8) ins.op = Ocvtf8f4;
      else if (kfrom == KF4) ins.op = issigned(to) ? Ocvtf4s : Ocvtf4u;
      else if (kfrom == KF8) ins.op = issigned(to) ? Ocvtf8s : Ocvtf8u;
      else assert(0);
   } else {
      if (to.t == TYBOOL) {
         if (from.t == TYBOOL) return ref;
         if (ref.t == RTMP)
            /* these instrs already have output range of [0,1] */
            if (oiscmp(instrtab[ref.i].op))
               return ref;
         ins.op = Oneq, ins.r = ZEROREF;
      }
      else if (kfrom == KI4 && issigned(from)) ins.op = Oexts4;
      else if (kfrom == KI4) ins.op = Oextu4;
      else if (kto == KI4 && isintcon(ref))
         return issigned(to) ? mkintcon(kto, (int)intconval(ref)) : mkintcon(kto, (uint)intconval(ref));
      else ins.op = Ocopy;
   }
   return addinstr(fn, ins);
}

static union ref
narrow(struct function *fn, enum irclass to, union type t, union ref ref, uint bitsiz)
{
   struct instr ins = {0};
   enum typetag tt = scalartypet(t);
   assert(isscalar(t));
   if (targ_primsizes[tt] < cls2siz[to]) {
      ins.cls = to;
      if (isfltt(tt)) {
         assert(to == KF4 && tt >= TYDOUBLE);
         ins.op = Ocvtf8f4;
      } else {
         static const enum op ext[5][2] = {
            [1] = {Oextu1, Oexts1}, [2] = {Oextu2, Oexts2}, [4] = {Oextu4, Oexts4}
         };
         ins.op = ext[targ_primsizes[tt]][issignedt(tt)];
      }
      ins.l = ref;
      ref = addinstr(fn, ins);
   }
   if (bitsiz) {
      assert(kisint(to) && isintt(tt) && bitsiz < 8*targ_primsizes[tt]);
      if (!issignedt(tt)) {
         ref = addinstr(fn, mkinstr(Oand, to, .l = ref, .r = mkintcon(to, (1ull<<bitsiz)-1)));
      } else {
         uint sh = 8*cls2siz[to] - bitsiz;
         ref = addinstr(fn, mkinstr(Oshl, to, .l = ref, .r = mkref(RICON, sh)));
         ref = addinstr(fn, mkinstr(Osar, to, .l = ref, .r = mkref(RICON, sh)));
      }
   }
   return ref;
}

union ref
genptroff(struct function *fn, enum op op, uint siz, union ref ptr,
          union type t, union ref idx)
{
   uint cls = type2cls[targ_sizetype];
   union ref off;
   assert(siz);

   idx = cvt(fn, mktype(targ_sizetype), t, idx);
   if (siz == 1) off = idx;
   else if (idx.t == RICON) {
      if (op == Osub) op = Oadd, idx.i = -idx.i;
      off = mkintcon(cls, idx.i * (int)siz);
   } else if (ispo2(siz))
      off = addinstr(fn,
            mkinstr(Oshl, cls, .l = idx, .r = mkintcon(cls, ilog2(siz))));
   else
      off = addinstr(fn,
            mkinstr(Omul, cls, .l = idx, .r = mkintcon(cls, siz)));
   assert(in_range(op, Oadd, Osub));
   return addinstr(fn, mkinstr(op, KPTR, .l = ptr, .r = off));
}

union ref
genptrdiff(struct function *fn, uint siz, union ref a, union ref b)
{
   uint cls = type2cls[targ_ptrdifftype];
   assert(siz > 0);
   a = addinstr(fn, mkinstr(Osub, cls, .l = a, .r = b));
   if (siz == 1) return a;
   else if ((siz & (siz-1)) == 0) /* is power of 2 */
      return addinstr(fn, mkinstr(Osar, cls, a, mkintcon(cls, ilog2(siz))));
   else
      return addinstr(fn, mkinstr(Odiv, cls, a, mkintcon(cls, siz)));
}

/* used to emit the jumps in an in if (), while (), etc condition */
static void
condjump(struct function *fn, const struct expr *ex, struct block *tr, struct block *fl)
{
   struct block *next, *next2;
Loop:
   while (ex->t == ESEQ) {
      expreffects(fn, &ex->sub[0]);
      ex = &ex->sub[1];
   }
   if (ex->t == ELOGAND) {
      next = newblk(fn);
      condjump(fn, &ex->sub[0], next, fl);
      useblk(fn, next);
      ex = &ex->sub[1];
      goto Loop;
   } else if (ex->t == ELOGIOR) {
      next = newblk(fn);
      condjump(fn, &ex->sub[0], tr, next);
      useblk(fn, next);
      ex = &ex->sub[1];
      goto Loop;
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
      goto Loop;
   } else if (ex->t == EEQU && isnullpo(&ex->sub[1])) { /* == 0 */
      goto Negate;
   } else if (ex->t == ENEQ && isnullpo(&ex->sub[1])) { /* != 0 */
      ex = &ex->sub[0];
      goto Loop;
   } else {
      putcondbranch(fn, exprvalue(fn, ex), tr, fl);
   }
}

struct condphis {
   vec_of(union ref) ref;
};

static void
condexprrec(struct function *fn, const struct expr *ex, struct condphis *phis,
            int boolcon, struct block *const next, struct block *end)
{
   struct block *tr, *fl;
   while (ex->t == ESEQ) {
      expreffects(fn, &ex->sub[0]);
      ex = &ex->sub[1];
   }
   if (ex->t == ELOGAND) {
      tr = newblk(fn);
      condexprrec(fn, &ex->sub[0], phis, 0, tr, end);
      useblk(fn, tr);
      condexprrec(fn, &ex->sub[1], phis, 0, next, end);
   } else if (ex->t == ELOGIOR) {
      fl = newblk(fn);
      condexprrec(fn, &ex->sub[0], phis, 1, end, fl);
      useblk(fn, fl);
      condexprrec(fn, &ex->sub[1], phis, 1, end, next ? next : end);
   } else if (ex->t == ECOND) {
      tr = newblk(fn);
      fl = newblk(fn);
      condjump(fn, &ex->sub[0], tr, fl);
      useblk(fn, tr);
      condexprrec(fn, &ex->sub[1], phis, -1, end, end);
      useblk(fn, fl);
      condexprrec(fn, &ex->sub[2], phis, -1, end, end);
   } else {
      union ref r, val;
      if (!phis && (!next || next == end)) {
         expreffects(fn, ex);
      } else {
         val = r = exprvalue(fn, ex);
         if (boolcon >= 0) {
            if (!next || next == end) {
               boolcon = -1;
               val = cvt(fn, mktype(TYBOOL), ex->ty, r);
            } else {
               val = mkref(RICON, boolcon);
            }
         }
      }
      if (phis)
         vpush(&phis->ref, val);
      if (next && next != end) {
         putcondbranch(fn, r, next, end);
      } else {
         assert(boolcon < 0);
         putbranch(fn, end);
      }
   }
}

/* the naive way to generate something like a ? b : c ? d : e, uses multiple phis,
 * this code reduces such nested conditional expressions into one phi */
static union ref
condexprvalue(struct function *fn, const struct expr *ex, bool discard)
{
   union ref refbuf[8];
   struct condphis phis = { VINIT(refbuf, arraylength(refbuf)) };
   struct block *dst = newblk(fn);
   union ref r;
   enum irclass k;
   condexprrec(fn, ex, discard ? NULL : &phis, -1, NULL, dst);
   useblk(fn, dst);
   if (discard) return NOREF;
   k = type2cls[scalartypet(ex->ty)];
   assert(k);
   r = addphi(fn, k, phis.ref.p);
   vfree(&phis.ref);
   return r;
}

static union ref
compilecall(struct function *fn, const struct expr *ex)
{
   struct instr ins = {0};
   struct expr *sub = ex->sub;
   const struct typedata *td = &typedata[sub[0].ty.dat];
   struct instr insnsbuf[10];
   vec_of(struct instr) insns = VINIT(insnsbuf, arraylength(insnsbuf));

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
      struct expr *arg = &sub[i+1];
      union type ty = i < td->nmemb ? td->param[i] : argpromote(arg->ty);
      union ref r = cvt(fn, ty, arg->ty, exprvalue(fn, arg));
      vpush(&insns, mkarginstr(mkirtype(ty), r));
   }
   for (int i = 0; i < insns.n; ++i)
      addinstr(fn, insns.p[i]);
   vfree(&insns);
   ins.r = mkcallarg(mkirtype(ex->ty), ex->narg, td->variadic ? td->nmemb : td->kandr ? 0 : -1);
   return addinstr(fn, ins);
}

static union ref
genbitfload(struct function *fn, const union type ty, union ref *addr, const struct exgetfld *fld)
{
   enum irclass k = type2cls[scalartypet(ty)];
   uint off = fld->off, bitsiz = fld->bitsiz, bitoff = fld->bitoff;
   union ref tmp;
   uvlong mask;

   assert(k);
   if (off > 0)
      *addr = addinstr(fn, mkinstr(Oadd, KPTR, .l = *addr, .r = mkintcon(KI4, off)));
   tmp = genload(fn, ty, *addr);
   if (!issigned(ty)) {
      /* shift right and mask */
      if (bitoff > 0)
         tmp = addinstr(fn, mkinstr(Oslr, k, .l = tmp, .r = mkref(RICON, bitoff)));
      if (bitsiz < 8*typesize(ty)) {
         mask = bitsiz == 64 ? -1ull : (1ull << bitsiz) - 1;
         tmp = addinstr(fn, mkinstr(Oand, k, .l = tmp, .r = mkintcon(k, mask)));
      }
   } else {
      /* shift left and shift right arithmetic to propagate sign bit */
      int sh = 8*cls2siz[k] - bitsiz - bitoff;
      if (sh)
         tmp = addinstr(fn, mkinstr(Oshl, k, .l = tmp, .r = mkref(RICON, sh)));
      sh += bitoff;
      if (sh)
         tmp = addinstr(fn, mkinstr(Osar, k, .l = tmp, .r = mkref(RICON, sh)));
   }
   return tmp;
}

static void
genbitfstore(struct function *fn, const union type ty, union ref addr,
             const struct exgetfld *fld, union ref tmp, union ref val)
{
   enum irclass k = type2cls[scalartypet(ty)];
   uint off = fld->off, bitsiz = fld->bitsiz, bitoff = fld->bitoff;
   uint bittypesize = 8*typesize(ty);
   uvlong mask;

   assert(k);
   if (!tmp.bits) {
      if (off > 0)
         addr = addinstr(fn, mkinstr(Oadd, KPTR, .l = addr, .r = mkintcon(KI4, off)));
      tmp = genload(fn, ty, addr);
   }
   mask = (bitsiz == 64 ? -1ull : (1ull << bitsiz) - 1) << bitoff;

   /* mask out bits in existing container */
   tmp = addinstr(fn, mkinstr(Oand, k, .l = tmp, .r = mkintcon(k, ~mask)));

   /* shift and mask source value */
   if (isintcon(val)) {
      val = mkintcon(k, ((uvlong)intconval(val) << bitoff) & mask);
   } else {
      if (bitoff)
         val = addinstr(fn, mkinstr(Oshl, k, .l = val, .r = mkref(RICON, bitoff)));
      if (bitsiz < bittypesize)
         val = addinstr(fn, mkinstr(Oand, k, .l = val, .r = mkintcon(k, mask)));
   }
   /* combine and write */
   if (bitsiz < bittypesize)
      val = addinstr(fn, mkinstr(Oior, k, .l = tmp, .r = val));
   genstore(fn, ty, addr, val);
}

static union ref
compileexpr(struct function *fn, const struct expr *ex, bool discard)
{
   union type ty;
   union ref r, q;
   uint bitsiz;
   enum irclass cls = type2cls[scalartypet(ex->ty)];
   struct instr ins = {0};
   int swp = 0;
   struct expr *sub;

   eval((struct expr *)ex, EVFOLD);
   sub = ex->sub;

   if (ex->ty.t != TYVOID && !isscalar(ex->ty))
      /* fn & array designators evaluate to their address;
       * so do aggregates for the purpose of code generation */
      return expraddr(fn, ex);
   switch (ex->t) {
   case ENUMLIT:
      if (discard) return NOREF;
      if (isflt(ex->ty))
         return mkfltcon(cls, ex->f);
      return mkintcon(cls, ex->i);
   case ESYM:
      if (discard && !(ex->qual & QVOLATILE)) return NOREF;
      return genload(fn, ex->ty, expraddr(fn, ex));
   case EGETF:
      if (discard && !(ex->qual & QVOLATILE)) return NOREF;
      if (ex->fld.bitsiz) {
         /* bit-field */
         r = expraddr(fn, ex->sub);
         return genbitfload(fn, ex->ty, &r, &ex->fld);
      }
      return genload(fn, ex->ty, expraddr(fn, ex));
   case ECAST:
      if (ex->ty.t == TYVOID) {
         expreffects(fn, sub);
         return NOREF;
      }
      /* fallthru */
   case EPLUS:
      r = compileexpr(fn, sub, discard);
      if (discard) return NOREF;
      return cvt(fn, ex->ty, sub->ty, r);
   case ENEG:
      ins.op = Oneg;
      goto Unary;
   case ECOMPL:
      ins.op = Onot;
   Unary:
      ins.l = compileexpr(fn, sub, discard);
      if (discard) return NOREF;
      ins.l = cvt(fn, ex->ty, sub->ty, ins.l);
      ins.cls = cls;
      return addinstr(fn, ins);
   case ELOGNOT:
      for (; sub->t == ELOGNOT; ex = sub, sub = sub->sub)
         swp ^= 1;
      ins.op = Oequ + swp;
      ins.l = compileexpr(fn, sub, discard);
      if (discard) return NOREF;
      ins.l = cvt(fn, ex->ty, sub->ty, ins.l);
      ins.r = mkintcon(cls, 0);
      ins.cls = cls;
      return addinstr(fn, ins);
   case EDEREF:
      discard &= (ex->qual & QVOLATILE) == 0;
      r = compileexpr(fn, sub, discard);
      if (discard) return NOREF;
      return genload(fn, ex->ty, r);
   case EADDROF:
      return expraddr(fn, sub);
   case EMUL:
      ins.op = isunsigned(ex->ty) ? Oumul : Omul;
      goto BinArith;
   case EDIV:
      ins.op = isunsigned(ex->ty) ? Oudiv : Odiv;
      goto BinArith;
   case EREM:
      ins.op = issigned(ex->ty) ? Orem : Ourem;
      goto BinArith;
   case EBAND:
      ins.op = Oand;
      goto BinArith;
   case EXOR:
      ins.op = Oxor;
      goto BinArith;
   case EBIOR:
      ins.op = Oior;
      goto BinArith;
   case ESHL:
      ins.op = Oshl;
      goto BinArith;
   case ESHR:
      ins.op = issigned(ex->ty) ? Osar : Oslr;
      goto BinArith;
   case ESUB:
      ins.op = Osub;
      goto BinArith;
   case EADD:
      ins.op = Oadd;
   BinArith:
      ins.l = compileexpr(fn, &sub[0], discard);
      ins.r = compileexpr(fn, &sub[1], discard);
      if (discard) return NOREF;
      if (ins.op == Osub && isptrcvt(sub[0].ty) && isptrcvt(sub[1].ty)) {
         /* ptr - ptr */
         return genptrdiff(fn, typesize(typechild(sub[0].ty)), ins.l, ins.r);
      } else if ((ins.op != Oadd && ins.op != Osub) || cls != KPTR) {
         /* num OP num */
         ins.l = cvt(fn, ex->ty, sub[0].ty, ins.l);
         ins.r = cvt(fn, ex->ty, sub[1].ty, ins.r);
      } else {
         assert(isptrcvt(sub[0].ty));
         /* ptr +/- num */
         return genptroff(fn, ins.op, typesize(typechild(sub[0].ty)), ins.l, sub[1].ty, ins.r);
      }
      ins.cls = cls;
      return addinstr(fn, ins);
   case EPOSTINC:
   case EPOSTDEC:
      ins.op = ex->t == EPOSTINC ? Oadd : Osub;
      ins.cls = cls;
      r = expraddr(fn, sub);
      ins.l = genload(fn, sub->ty, r);
      if (ex->ty.t == TYPTR)
         ins.r = mkintcon(KI4, typesize(typechild(ex->ty)));
      else
         ins.r = mkref(RICON, 1);
      genstore(fn, sub->ty, r, addinstr(fn, ins));
      return ins.l;
   case EPREINC:
   case EPREDEC:
      ins.op = ex->t == EPREINC ? Oadd : Osub;
      ins.cls = cls;
      r = expraddr(fn, sub);
      ins.l = genload(fn, sub->ty, r);
      if (ex->ty.t == TYPTR)
         ins.r = mkintcon(KI4, typesize(typechild(ex->ty)));
      else
         ins.r = mkref(RICON, 1);
      q = addinstr(fn, ins);
      genstore(fn, sub->ty, r, q);
      if (discard) return NOREF;
      return narrow(fn, cls, ex->ty, q, 0);
   case EEQU:
      ins.op = Oequ;
      goto Cmp;
   case ENEQ:
      ins.op = Oneq;
      goto Cmp;
   case ELTH:
      ins.op = Olth;
      goto Cmp;
   case ELTE:
      ins.op = Olte;
      goto Cmp;
   case EGTH:
      ins.op = Ogth;
      goto Cmp;
   case EGTE:
      ins.op = Ogte;
   Cmp:
      ty = cvtarith(sub[0].ty, sub[1].ty);
      if (!ty.t) ty.t = TYPTR;
      if (isunsigned(ty) && in_range(ins.op, Olth, Ogte))
         ins.op += Oulth - Olth;
      ins.l = compileexpr(fn, &sub[0], discard);
      ins.r = compileexpr(fn, &sub[1], discard);
      if (discard) return NOREF;
      ins.l = cvt(fn, ty, sub[0].ty, ins.l);
      ins.r = cvt(fn, ty, sub[1].ty, ins.r);
      ins.cls = type2cls[ty.t];
      return addinstr(fn, ins);
   case ESET:
      assert(isscalar(ex->ty));
      q = cvt(fn, sub[0].ty, sub[1].ty, exprvalue(fn, &sub[1]));
      if (sub[0].t == EGETF && (bitsiz = sub[0].fld.bitsiz)) {
         /* bit-field */
         r = expraddr(fn, &sub[0].sub[0]);
         genbitfstore(fn, ex->ty, r, &sub[0].fld, NOREF, q);
      } else {
         bitsiz = 0;
         r = expraddr(fn, &sub[0]);
         genstore(fn, ex->ty, r, q);
      }
      if (discard) return NOREF;
      return bitsiz ? narrow(fn, cls, sub[0].ty, q, bitsiz) : q;
   case ESETMUL:
      ins.op = isunsigned(ex->ty) ? Oumul : Omul;
      goto Compound;
   case ESETDIV:
      ins.op = isunsigned(ex->ty) ? Oudiv : Odiv;
      goto Compound;
   case ESETREM:
      ins.op = issigned(ex->ty) ? Orem : Ourem;
      goto Compound;
   case ESETAND:
      ins.op = Oand;
      goto Compound;
   case ESETXOR:
      ins.op = Oxor;
      goto Compound;
   case ESETIOR:
      ins.op = Oior;
      goto Compound;
   case ESETSHL:
      ins.op = Oshl;
      goto Compound;
   case ESETSHR:
      ins.op = issigned(ex->ty) ? Osar : Oslr;
      goto Compound;
   case ESETSUB:
      ins.op = Osub;
      goto Compound;
   case ESETADD:
      ins.op = Oadd;
   Compound:
      ty = in_range(ex->t, ESETSHL, ESETSHR) ? mktype(intpromote(ex->ty.t))
                                             : cvtarith(sub[0].ty, sub[1].ty);
      ins.cls = cls;
      ins.r = exprvalue(fn, &sub[1]);
      if (sub[0].t == EGETF && (bitsiz = sub[0].fld.bitsiz)) {
         /* bit-field */
         r = expraddr(fn, &sub[0].sub[0]);
         ins.l = genbitfload(fn, sub[0].ty, &r, &sub[0].fld);
         q = addinstr(fn, ins);
         genbitfstore(fn, sub[0].ty, r, &sub[0].fld, ins.l, q);
      } else {
         bitsiz = 0;
         r = expraddr(fn, &sub[0]);
         ins.l = genload(fn, ex->ty, r);
         if ((ins.op != Oadd && ins.op != Osub) || cls != KPTR) {
            ins.l = cvt(fn, ty, sub[0].ty, ins.l);
            ins.r = cvt(fn, ex->ty, sub[1].ty, ins.r);
            q = addinstr(fn, ins);
         } else {
            q = genptroff(fn, ins.op, typesize(typechild(ex->ty)), ins.l, sub[1].ty, ins.r);
         }
         genstore(fn, ex->ty, r, q);
      }
      if (discard) return NOREF;
      return bitsiz ? narrow(fn, cls, ex->ty, q, bitsiz) : q;
   case ECALL:
      r = compilecall(fn, ex);
      if (isint(ex->ty))
         return narrow(fn, cls, ex->ty, r, 0);
      return r;
   case ECOND:
      if (ex->ty.t == TYVOID || discard) {
         struct block *tr, *fl, *end;
         condjump(fn, &sub[0], tr = newblk(fn), fl = newblk(fn));
         useblk(fn, tr);
         expreffects(fn, &sub[1]);
         end = newblk(fn);
         putbranch(fn, end);
         useblk(fn, fl);
         expreffects(fn, &sub[2]);
         putbranch(fn, end);
         useblk(fn, end);
         return NOREF;
      }
      /* fallthru */
   case ELOGAND:
   case ELOGIOR:
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
stmtterm(struct comp *cm)
{
   expect(cm, ';', "to terminate previous statement");
}

static void block(struct comp *cm, struct function *fn);
static bool stmt(struct comp *cm, struct function *fn);
static void localdecl(struct comp *cm, struct function *fn, bool forinit);

struct label {
   struct label *link;
   const char *name;
   struct block *blk;
   struct span usespan;
   /* if usespan.ex.len == 0, this label is resolved and blk is the block that
    * the label starts, otherwise the label is unresolved and blk is the head
    * of a linked list of relocations, the next list entry is in blk->s1, etc,
    * terminated by NULL */
};

static struct label *
findlabel(struct comp *cm, const char *name)
{
   for (struct label *l = cm->labels; l; l = l->link)
      if (l->name == name) return l;
   return NULL;
}

static void
deflabel(struct comp *cm, struct function *fn, const struct span *span, const char *name)
{
   struct label *label = findlabel(cm, name);
   if (label && label->usespan.ex.len == 0) {
      error(span, "redefinition of label '%s'", name);
   } else if (label) {
      struct block *new = NULL;
      if (!nerror) {
         new = newblk(fn);
         if (fn->curblk) putbranch(fn, new);
      }
      /* fix up relocations */
      for (struct block *list = label->blk, *next; list; list = next) {
         next = list->s1;
         if (!nerror) {
            useblk(fn, list);
            putbranch(fn, new);
         }
      }
      label->usespan = (struct span){0};
      label->blk = fn->curblk;
      if (!nerror) useblk(fn, new);
   } else {
      struct label l = { cm->labels, name };
      if (!nerror) {
         struct block *new = newblk(fn);
         if (fn->curblk) putbranch(fn, new);
         useblk(fn, new);
      }
      l.blk = fn->curblk;
      cm->labels = alloccopy(fn->arena, &l, sizeof l, 0);
   }
}

static bool
loopbody(struct comp *cm, struct function *fn, struct block *brk, struct block *cont)
{
   struct block *save[2];
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

struct swcase {
   vlong val;
   struct block *blk;
};
struct switchstmt {
   struct block *bdefault;
   vec_of(struct swcase) cases;
};

static void
genswitch(struct comp *cm, struct function *fn, const struct expr *ex)
{
   union ref sel;
   bool doemit = fn->curblk;
   struct block *begin = NULL, *end = NULL, *breaksave = cm->breakto;
   struct switchstmt *stsave = cm->switchstmt, st = {0};
   enum irclass k = type2cls[scalartypet(ex->ty)];
   struct swcase casebuf[8];
   vinit(&st.cases, casebuf, arraylength(casebuf));

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
   doemit = 1;
   if (!st.bdefault) st.bdefault = end;
   /* TODO: optimize instead of generating the equivalent of if == .. else if .. chain
    *       1. sort by case values (also for easy duplicates checking)
    *       2. contiguous ranges (case a..b:  ->  x >= && x <= b)
    *       3. binary search
    *       4. jump tables? (harder)
    */
   for (int i = 0; i < st.cases.n; ++i) {
      struct swcase c = st.cases.p[i];
      EMITS {
         struct block *next = i < st.cases.n - 1 ? newblk(fn) : st.bdefault;
         putcondbranch(fn, addinstr(fn, mkinstr(Oequ, k, .l = sel, .r = mkintcon(k, c.val))), c.blk, next);
         if (next != st.bdefault) useblk(fn, next);
      }
   }
   vfree(&st.cases);
   if (fn->curblk != end) {
      if (fn->curblk) EMITS putbranch(fn, end);
      useblk(fn, end);
   }
}

static bool /* return 1 if stmt is terminating (ends with a jump) */
stmt(struct comp *cm, struct function *fn)
{
   struct block *tr, *fl, *end, *begin;
   union {
      struct arena a;
      char mem[sizeof(struct arena) + sizeof(struct expr)*4];
   } atmp = { .a.cap = sizeof(struct expr)*4 };
   struct arena *atmpp;
   struct expr ex;
   struct env e;
   union ref r;
   struct token tk;
   bool terminates = 0;
   bool doemit = fn->curblk;

   while (match(cm, &tk, TKIDENT) || match(cm, &tk, TKWcase) || match(cm, &tk, TKWdefault)) {
      if (tk.t == TKWcase) {
         /* case <expr> ':' */
         if (!cm->switchstmt) error(&tk.span, "'case' outside of switch statement");
         ex = constantexpr(cm);
         if (!eval(&ex, EVINTCONST)) error(&ex.span, "not an integer constant expression");
         expect(cm, ':', NULL);
         begin = newblk(fn);
         EMITS putbranch(fn, begin);
         useblk(fn, begin);
         if (cm->switchstmt)
            vpush(&cm->switchstmt->cases, ((struct swcase) {ex.i, fn->curblk}));
      } else if (tk.t == TKWdefault) {
         /* default ':' */
         if (!cm->switchstmt) error(&tk.span, "'default' outside of switch statement");
         expect(cm, ':', NULL);
         begin = newblk(fn);
         EMITS putbranch(fn, begin);
         useblk(fn, begin);
         if (cm->switchstmt) {
            if (cm->switchstmt->bdefault) error(&tk.span, "multiple 'default' labels in one switch");
            cm->switchstmt->bdefault = begin;
         }
      } else if (tk.t == TKIDENT && match(cm, NULL, ':')) {
         /* <label> ':' */
         deflabel(cm, fn, &tk.span, tk.s);
      } else {
         assert(tk.t == TKIDENT);
         /* kludge for no backtracking and no lookahead */
         ex = exprparse(cm, 1, &tk, 1);
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
      EMITS {
         putbranch(fn, begin = newblk(fn));
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
      EMITS {
         putbranch(fn, begin = newblk(fn));
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
      EMITS {
         putbranch(fn, end = tr = begin = newblk(fn));
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
      if (!match(cm, NULL, ')')) { /* iter */
         /* since exarena is free'd at the end of each stmt, create a new temporary
          * arena to parse this expression because loop body statements would free it
          * otherwise */
         struct arena *tmp = cm->exarena;
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
            freearena(&atmpp);
         } else if (!terminates) putbranch(fn, begin);
         useblk(fn, fl);
      }
      envup(cm);
      break;
   case TKWswitch:
      lex(cm, NULL);
      expect(cm, '(', NULL);
      ex = commaexpr(cm);
      expect(cm, ')', NULL);
      if (!isint(ex.ty))
         error(&ex.span, "'switch' value is not an integer: '%ty'", ex.ty);
      genswitch(cm, fn, &ex);
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
         struct label *label = findlabel(cm, tk.s);
         if (!label) {
            /* create reloc list */
            struct label l = { cm->labels, tk.s, fn->curblk, tk.span };
            cm->labels = alloccopy(fn->arena, &l, sizeof l, 0);
            fn->curblk = NULL;
         } else if (label && label->usespan.ex.len != 0) {
            /* append to relocs list */
            struct block *next = label->blk;
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
      lex(cm, NULL);
      if (fn->retty.t != TYVOID) {
         ex = commaexpr(cm);
         if (!assigncheck(fn->retty, &ex)) {
            error(&ex.span,
                  "cannot return '%ty' value from function with return type '%ty'",
                  ex.ty, fn->retty);
         }
         EMITS {
            if (isscalar(fn->retty))
               r = cvt(fn, fn->retty, ex.ty, exprvalue(fn, &ex));
            else
               r = structreturn(fn, &ex);
            putreturn(fn, r, NOREF);
         }
      } else {
         EMITS putreturn(fn, NOREF, NOREF);
      }
      stmtterm(cm);
      break;
   default:
      ex = exprparse(cm, 1, NULL, 1);
      stmtterm(cm);
      EMITS expreffects(fn, &ex);
      break;
   }
   freearena(&cm->exarena);
   lexerfreetemps(&cm->lx);
   return fn->curblk == NULL;
}

/* parse and compile a function-local declaration */
static void
localdecl(struct comp *cm, struct function *fn, bool forini)
{
   struct expr ini;
   struct token tk;
   const bool doemit = fn->curblk;
   struct declstate st = { DFUNCVAR };

   if (!forini && match(cm, &tk, TKIDENT)) {
      if (match(cm, NULL, ':')) {
         /* <label> ':' */
         deflabel(cm, fn, &tk.span, tk.s);
         stmt(cm, fn);
         return;
      }
      st.base = finddecl(cm, tk.s)->ty;
   }
   do {
      struct decl decl = pdecl(&st, cm);
      if (decl.name) {
         static int staticid;
         bool put = 0;
         bool dynarr = 0;

         switch (decl.scls) {
         case SCSTATIC:
            if (forini)
               error(&decl.span, "static declaration in 'for' loop initializer");
            decl.id = ++staticid;
            goto Initz;
         case SCNONE:
            if (decl.ty.t == TYFUNC) {
               decl.scls = SCEXTERN;
               break;
            }
            decl.scls = SCAUTO;
            /* fallthru */
         case SCAUTO:
         case SCREGISTER:
            if (decl.ty.t == TYFUNC) {
               error(&decl.span, "declaring variable '%s' with function type '%ty'", decl.name, decl.ty);
            } else if (isincomplete(decl.ty) && !(dynarr = (decl.ty.t == TYARRAY && st.varini))) {
               error(&decl.span, "declaring variable '%s' with incomplete type '%ty'", decl.name, decl.ty);
               goto Err;
            }
            EMITS {
               decl.id = addinstr(fn, mkalloca(typesize(decl.ty), typealign(decl.ty))).i;
            }
         Initz:
            if (st.varini) {
               struct decl *d = putdecl(cm, &decl);
               bool statik = st.scls & (SCSTATIC | SCEXTERN);
               const char *name = decl.name;
               put = 1;
               if (decl.scls == SCSTATIC)
                  name = mkhiddensym(fn->name, name, decl.id);
               ini = initializer(cm, &d->ty, statik ? EVSTATICINI : EVFOLD,
                                 /* globl? */ decl.scls == SCEXTERN, decl.qual, name);
               pdecl(&st, cm);
               if (!statik) {
                  /* fix alloca for actual size, for implicitly sized arrays */
                  assert(!isincomplete(d->ty));
                  instrtab[decl.id] = mkalloca(typesize(d->ty), typealign(d->ty));

                  if (!initcheck(d->ty, &ini)) {
                     struct span span = decl.span;
                     joinspan(&span.ex, ini.span.ex);
                     error(&span, "cannot initialize '%ty' variable with '%ty'",
                           d->ty, ini.ty);
                  }
                  EMITS {
                     if (ini.t == EINIT || (d->ty.t == TYARRAY && ini.t == ESTRLIT))
                        geninit(fn, d->ty, mkref(RTMP, decl.id), &ini);
                     else if (isagg(d->ty))
                        structcopy(fn, d->ty, mkref(RTMP, decl.id), exprvalue(fn, &ini));
                     else {
                        genstore(fn, d->ty, mkref(RTMP, decl.id),
                              cvt(fn, d->ty, ini.ty, exprvalue(fn, &ini)));
                     }
                  }
               } else if (decl.scls == SCEXTERN) {
                  struct span span = decl.span;
                  joinspan(&span.ex, ini.span.ex);
                  error(&span,
                        "declaration of block local with extern linkage cannot have an initializer");
               }
            } else if (decl.scls == SCSTATIC) {
               /* zero-initialized static */
               const char *sym = mkhiddensym(fn->name, decl.name, decl.id);
               if (decl.ty.t == TYARRAY && isincomplete(decl.ty))
                  error(&decl.span, "definition of variable with array type needs size or initializer");
               else
                  objnewdat(sym, Sbss, 0, typesize(decl.ty), typealign(decl.ty));
            }
            break;
         case SCTYPEDEF:
            if (forini)
               error(&decl.span, "typedef in 'for' loop initializer");
            break;
         case SCEXTERN:
            if (forini)
               error(&decl.span, "extern declaration in 'for' loop initializer");
            if (st.varini) goto Initz;
            break;
         default: assert(0);
         }
      Err:
         if (!put) putdecl(cm, &decl);
      } else if (forini)
         error(&decl.span, "non-variable declaration in 'for' loop initializer");
   } while (st.more);
}

static void
block(struct comp *cm, struct function *fn)
{
   struct token tk;

   while (!match(cm, &tk, '}')) {
      if (isdecltok(cm))
         localdecl(cm, fn, 0);
      else
         stmt(cm, fn);
   }
   cm->fnblkspan = tk.span;
}

static void
function(struct comp *cm, struct function *fn, const char **pnames, const struct span *pspans)
{
   const struct typedata *td = &typedata[fn->fnty.dat];
   const bool doemit = fn->curblk;
   struct env e;
   struct token tk;
   envdown(cm, &e);

   /* emit Oparam instructions */
   EMITS {
      for (int i = 0; i < td->nmemb; ++i) {
         union irtype pty = mkirtype(td->param[i]);
         union ref r = addinstr(fn, mkinstr(Oparam, pty.isagg ? KPTR : pty.cls,
                                            mkref(RICON, i), mktyperef(pty)));
         assert(r.t == RTMP && r.i == i);
      }
   }
   /* add parameters to symbol table and create prologue (arguments) block */
   for (int i = 0; i < td->nmemb; ++i) {
      if (pnames[i]) {
         struct decl arg = { .ty = td->param[i], .qual = tdgetqual(td->quals, i),
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

   /* __func__ */
   {
      static const char *ifunc;
      if (!ifunc) ifunc = intern("__func__");
      union type ty = mkarrtype(mktype(TYCHAR), QCONST, strlen(fn->name) + 1);
      const char *sym = mkhiddensym(fn->name, ifunc, 1);
      uint off = objnewdat(sym, Srodata, 0, typesize(ty), typealign(ty));
      uchar *p = objout.rodata.p + off;
      memcpy(p, fn->name, typearrlen(ty)-1);
      putdecl(cm, &(struct decl) {
         .ty = ty, .qual = QCONST,
         .name = ifunc, .scls = SCSTATIC, .span = (peek(cm, &tk), tk.span),
         .id = 1,
      });
   }
   /* end prologue */
   EMITS {
      struct block *blk;
      putbranch(fn, blk = newblk(fn));
      useblk(fn, blk);
   }
   cm->labels = NULL;
   block(cm, fn);
   envup(cm);
   for (struct label *l = cm->labels; l; l = l->link) {
      if (l->usespan.ex.len) {
         error(&l->usespan, "label '%s' used but never defined", l->name);
      }
   }
   if (fn->curblk) {
      if (!strcmp(fn->name, "main") && fn->retty.t == TYINT) {
         /* implicit return 0 for main function */
         putreturn(fn, ZEROREF, NOREF);
      } else {
         if (fn->retty.t != TYVOID && !nerror) {
            warn(&cm->fnblkspan, "non-void function may not return a value");
         }
         putreturn(fn, NOREF, NOREF);
      }
   }
}

void
docomp(struct comp *cm)
{
   struct token tk[1];

   if (!cm->env) cm->env = &toplevel;

   putdecl(cm, &(struct decl) { mktype(TYVALIST), SCTYPEDEF, .name = intern("__builtin_va_list") });

   while (peek(cm, tk) != TKEOF) {
      struct declstate st = { DTOPLEVEL };
      do {
         bool noscls = 0;
         int nerr = nerror;
         struct decl decl = pdecl(&st, cm);

         if (nerror != nerr) {
            if (st.varini) {
               (void)expr(cm);
               pdecl(&st, cm);
            }
            continue;
         }
         if (!decl.scls) {
            noscls = 1;
            decl.scls = SCEXTERN;
         }
         decl.isdef = st.varini;
         if (st.funcdef) {
            const struct typedata *td =  &typedata[decl.ty.dat];
            struct function fn = { &cm->fnarena, decl.name, .globl = decl.scls != SCSTATIC };
            fn.fnty = decl.ty;
            fn.retty = td->ret;
            decl.isdef = 1;
            putdecl(cm, &decl);
            irinit(&fn);
            function(cm, &fn, st.pnames, st.pspans);
            if (!nerror && ccopt.dbg.p)
               irdump(&fn);
            irfini(&fn);
         } else if (decl.name) {
            struct decl *d = putdecl(cm, &decl);
            if (st.varini) {
               (void) initializer(cm, &d->ty, EVSTATICINI, decl.scls != SCSTATIC, decl.qual, decl.name);
               pdecl(&st, cm);
            } else if (decl.ty.t != TYFUNC && decl.scls != SCTYPEDEF && (decl.scls != SCEXTERN || noscls)) {
               objnewdat(d->name, Sbss, decl.scls == SCEXTERN, typesize(d->ty), typealign(d->ty));
            }
            if (ccopt.dbg.p) efmt("var %s : %tq\n", d->name, d->ty, d->qual);
         } else {
            if (ccopt.dbg.p && decl.ty.t) efmt("type %ty\n", decl.ty);
         }
         freearena(&cm->fnarena);
         freearena(&cm->exarena);
         lexerfreetemps(&cm->lx);
      } while (st.more);
   }
}

void
ccomp(const char *file)
{
   enum { N = 1<<12 };
   static union { char m[sizeof(struct arena) + N]; struct arena *_align; } amem[2];
   struct comp cm = {0};
   const char *err = initlexer(&cm.lx, NULL, file);
   if (err)
      fatal(NULL, "Cannot open %'s: %s", file, err);
   cm.fnarena = (void *)amem[0].m;
   cm.fnarena->cap = N;
   cm.exarena = (void *)amem[1].m;
   cm.exarena->cap = N;
   docomp(&cm);
}

/* vim:set ts=3 sw=3 expandtab: */
