#include "parse.h"
#include "common.h"
#include "ir.h"

static struct arena *tlarena;

#define peek(Pr,Tk) lexpeek(Pr,Tk)

void
initparser(struct parser *pr, const char *file)
{
   const char *error;
   struct memfile *f;

   memset(pr, 0, sizeof *pr);
   pr->fileid = openfile(&error, &f, file);
   if (pr->fileid < 0)
      fatal(NULL, "Cannot open %'s: %s", file, error);
   pr->dat = f->p;
   pr->ndat = f->n;
}

static bool
match(struct parser *pr, struct token *tk, enum toktag t)
{
   if (peek(pr, NULL) == t) {
      lex(pr, tk);
      return 1;
   }
   return 0;
}

static bool
expect(struct parser *pr, enum toktag t, const char *s)
{
   struct token tk;
   if (!match(pr, &tk, t)) {
      peek(pr, &tk);
      if (aisprint(t)) tk.span.ex.len = tk.span.sl.len = 1;
      error(&tk.span, "expected %'tt%s%s", t, s?" ":"",s ? s : "");
      return 0;
   }
   return 1;
}

static struct token
expectdie(struct parser *pr, enum toktag t, const char *s)
{
   struct token tk;
   if (!match(pr, &tk, t))
      fatal(&tk.span, "expected %'tt%s%s", t, s?" ":"",s ? s : "");
   return tk;
}

enum declkind {
   DTOPLEVEL,
   DFUNCPARAM,
   DFUNCVAR,
   DFIELD,
   DCASTEXPR,
};

struct decl {
   union type ty;
   uchar scls;
   uchar qual;
   ushort align;
   struct span span;
   const char *name;
   int id;
};

struct declstate {
   enum declkind kind;
   union type base;
   enum storageclass scls;
   enum qualifier qual;
   uint align;
   bool more, varini, funcdef, tagdecl;
   const char **pnames;
   struct span *pspans;
};

static struct decl pdecl(struct declstate *st, struct parser *pr);
static struct decl *finddecl(struct parser *pr, const char *name);

static bool
isdecltok(struct parser *pr)
{
   struct decl *decl;
   struct token tk;
   switch (peek(pr, &tk)) {
   case TKWsigned: case TKWunsigned: case TKWshort: case TKWlong:
   case TKWint: case TKWchar: case TKW_Bool: case TKWauto:
   case TKWstruct: case TKWunion: case TKWenum: case TKWtypedef:
   case TKWextern: case TKWstatic: case TKWinline: case TKW_Noreturn:
   case TKWconst: case TKWvolatile: case TKWvoid: case TKWfloat:
   case TKWdouble: case TKWregister:
      return 1;
   case TKIDENT:
      return (decl = finddecl(pr, tk.s)) && decl->scls == SCTYPEDEF;
   }
   return 0;
}


/*******/
/* ENV */
/*******/

static struct decl envdeclsbuf[1<<10];
static vec_of(struct decl) envdecls = VINIT(envdeclsbuf, arraylength(envdeclsbuf));
struct tagged {
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
envdown(struct parser *pr, struct env *e)
{
   assert(pr->env->decl + pr->env->ndecl == envdecls.n);
   assert(pr->env->tagged + pr->env->ntagged == envtagged.n);
   e->decl = envdecls.n;
   e->tagged = envtagged.n;
   e->ndecl = e->ntagged = 0;
   e->up = pr->env;
   pr->env = e;
}

static void
envup(struct parser *pr)
{
   struct env *env = pr->env;
   assert(env->decl + env->ndecl == envdecls.n);
   envdecls.n -= env->ndecl;
   envtagged.n -= env->ntagged;
   assert(env->up);
   pr->env = env->up;
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
   case SCTYPEDEF:
      return old->ty.bits == new->ty.bits;
   }
   return 0;
}

static struct decl *
putdecl(struct parser *pr, const struct decl *decl)
{
   struct decl *l;
   for (l = NULL; enviterdecl(&l, pr->env);) {
      if (decl->name == l->name && !redeclarationok(l, decl)) {
         error(&decl->span, "incompatible redeclaration of '%s'", decl->name);
         note(&l->span, "previously declared here");
      }
   }
   l = envadddecl(pr->env, decl);
   return l;
}

static struct decl *
finddecl(struct parser *pr, const char *name)
{
   struct env *e;
   struct decl *l;
   assert(name);
   for (e = pr->env; e; e = e->up) {
      for (l = NULL; enviterdecl(&l, e);) {
         if (name == l->name)
            return l;
      }
   }
   return NULL;
}

static union type
gettagged(struct parser *pr, struct span *span, enum typetag tt, const char *name, bool dodef)
{
   struct env *e;
   struct tagged *l;
   struct typedata td = {0};
   assert(name);
   for (e = pr->env; e; e = e->up) {
      for (l = NULL; envitertagged(&l, e);) {
         if (name == ttypenames[typedata[l->ty.dat].id]) {
            if (dodef && e != pr->env)
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
   return envaddtagged(pr->env, mktagtype(name, &td), span)->ty;
}

static union type
deftagged(struct parser *pr, struct span *span, enum typetag tt, const char *name)
{
   struct tagged *l;
   struct typedata td = {0};
   assert(name);
   for (l = NULL; envitertagged(&l, pr->env);) {
      if (name == ttypenames[typedata[l->ty.dat].id]) {
         *span = l->span;
         return l->ty;
      }
   }
   td.t = tt;
   return envaddtagged(pr->env, mktagtype(name, &td), span)->ty;
}

/*******************/
/*** EXPRESSIONS ***/
/*******************/

/**********************/
/* EXPR TYPE CHECKING */
/**********************/

#define iszero(ex) ((ex).t == ENUMLIT && (ex).u == 0)

static bool
islvalue(const struct expr *ex)
{
   if (ex->t == EGETF) return islvalue(ex->sub);
   return ex->t == ESYM || ex->t == EDEREF;
}

static union type /* 6.5.2.6 default argument promotions */
argpromote(union type t)
{
   if (isint(t)) t.t = intpromote(t.t);
   else if (t.t == TYFLOAT) t.t = TYDOUBLE;
   return t;
}

static bool
assigncheck(union type t, const struct expr *src)
{
   if (assigncompat(t, typedecay(src->ty))) return 1;
   if (t.t == TYPTR && iszero(*src)) return 1;
   return 0;
}

static void
incdeccheck(enum toktag tt, const struct expr *ex, const struct span *span)
{
   if (!isscalar(ex->ty))
      error(&ex->span, "invalid operand to %tt (%ty)", tt, ex->ty);
   else if (!islvalue(ex))
      error(&ex->span, "operand to %tt is not an lvalue", tt);
   else if (ex->ty.t == TYPTR && isincomplete(typechild(ex->ty)))
      error(span, "arithmetic on pointer to incomplete type (%ty)", ex->ty);
   else if (ex->ty.t == TYPTR && typechild(ex->ty).t == TYFUNC)
      error(span, "arithmetic on function pointer (%ty)", ex->ty);
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
         error(span, "cannot dereference pointer to incomplete type (%ty)", ty);
         ty = mktype(TYINT);
      } else if (ty.t == TYFUNC) {
         error(span, "subscripted value is pointer to function");
         ty = mktype(TYINT);
      }
   } else {
      error(&ex->span, "subscripted value is not pointer-convertible (%ty)", ex->ty);
      ty = mktype(TYINT);
   }
   if (!isint(rhs->ty))
      error(&rhs->span, "array subscript is not integer (%ty)", rhs->ty);
   return ty;
}

static void /* 6.5.3.4 The sizeof operator */
sizeofcheck(const struct span *span, union type ty)
{
   if (isincomplete(ty))
      error(span, "cannot apply sizeof to incomplete type (%ty)", ty);
   else if (ty.t == TYFUNC)
      error(span, "cannot apply sizeof to function type (%ty)", ty);
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
   error(span, "bad operands to %tt (%ty, %ty)", tt, lhs, rhs);
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
         error(&lhs->span, "cannot assign to incomplete type (%ty)", ty);
      else if (ty.t == TYARRAY)
         error(&lhs->span, "cannot assign to array type (%ty)", ty);
      else if (ty.t == TYFUNC)
         error(&lhs->span, "cannot assign to function designator (%ty)", lhs->ty);
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
      } else if ((ty.t == TYPTR || ty.t == TYARRAY) && rhs->ty.t == TYINT) {
         /* ptr +/- int */
         union type pointee = typechild(ty);
         if (isincomplete(pointee))
            error(span, "arithmetic on pointer to incomplete type (%ty)", ty);
         else if (pointee.t == TYFUNC)
            error(span, "arithmetic on function pointer (%ty)", ty);
         ty = typedecay(ty);
      } else if (tt == '-' && isptrcvt(ty) && isptrcvt(rhs->ty)) {
         /* ptr - ptr */
         union type pointee1 = typechild(typedecay(ty)),
                    pointee2 = typechild(typedecay(rhs->ty));
         if (isincomplete(pointee1))
            error(span, "arithmetic on pointer to incomplete type (%ty)", ty);
         else if (pointee1.t == TYFUNC)
            error(span, "arithmetic on function pointer (%ty)", lhs->ty);
         else if (pointee1.bits != pointee2.bits) {
            error(span, "arithmetic on incompatible pointer types (%ty, %ty)",
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
      if (!isscalar(ty) || !isscalar(rhs->ty))
         goto Error;
      ty = mktype(TYINT);
      break;
   }
   return (k & BCSET) || !ty.t ? lhs->ty : ty;
}

/****************/
/* EXPR PARSING */
/****************/

#define mkexpr(t_,span_,ty_,...) ((struct expr){.t=(t_), .ty=(ty_), .span=(span_), __VA_ARGS__})

static struct expr *
exprdup(struct parser *pr, const struct expr *e)
{
   return memcpy(alloc(&pr->exarena, sizeof *e, 0), e, sizeof *e);
}
static struct expr *
exprdup2(struct parser *pr, const struct expr *e1, const struct expr *e2)
{
   struct expr *r = alloc(&pr->exarena, 2*sizeof *r, 0);
   r[0] = *e1, r[1] = *e2;
   return r;
}

static struct expr expr(struct parser *pr);
static struct expr commaexpr(struct parser *pr);

static struct expr /* 6.5.2.2 Function calls */
callexpr(struct parser *pr, const struct span *span_, const struct expr *callee)
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

   if (ty.t == TYPTR) /* auto-deref when calling a function pointer */
      ty = typechild(ty);
   if (ty.t != TYFUNC) error(&callee->span, "calling a value of type '%ty'", callee->ty);
   if (!match(pr, &tk, ')')) for (;;) {
      arg = expr(pr);
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
      peek(pr, &tk);
      if (match(pr, &tk, ',')) {
         spanok = spanok && joinspan(&span.ex, tk.span.ex);
      } else if (expect(pr, ')', "or ',' after arg")) {
         break;
      }
   }
   if (!spanok || !joinspan(&span.ex, tk.span.ex)) span = *span_;

   if (!td->variadic && !td->kandr && args.n < td->nmemb) {
      error(&tk.span, "not enough args to function taking %d param%s",
            td->nmemb, td->nmemb != 1 ? "s" : "");
      printsig = 1;
   }
   if (printsig) note(&callee->span, "function signature is %ty", ty);

   ex = mkexpr(ECALL, span, ty.t == TYFUNC ? td->ret : ty, .narg = args.n,
               .sub = alloc(&pr->exarena, (args.n+1)*sizeof(struct expr), 0));
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

static struct expr
exprparse(struct parser *pr, int prec)
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

Unary:
   switch (lex(pr, &tk)) {
   /* unary operators (gather) */
   case '+': case '-': case '~': case '!':
   case '*': case '&': case TKINC: case TKDEC:
   Unops:
      unops[nunop].span = tk.span;
      unops[nunop].t0 = 0;
      unops[nunop].tt = tk.t;
      if (++nunop >= arraylength(unops)) {
         ex = exprparse(pr, 999);
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
      ex = mkexpr(ESTRLIT, tk.span,
                  mkarrtype(mktype(TYCHAR), 0, tk.len+1), .s = { (uchar *)tk.s, tk.len });
      break;
   case TKIDENT:
      decl = finddecl(pr, tk.s);
      if (!decl) {
         error(&tk.span, "undeclared identifier %'tk", &tk);
         ex = mkexpr(ESYM, tk.span, mktype(TYINT), .sym = NULL);
      } else if (decl->scls == SCTYPEDEF) {
         error(&tk.span, "unexpected typename %'tk (expected expression)", &tk);
         ex = mkexpr(ESYM, tk.span, decl->ty, .sym = NULL);
      } else {
         ex = mkexpr(ESYM, tk.span, decl->ty, .qual = decl->qual, .sym = decl);
      }
      break;

   /* might be unary op or primary expr */
   case '(':
      if (!isdecltok(pr)) { /* (expr) */
         ex = commaexpr(pr);
         expect(pr, ')', NULL);
         break;
      } else { /* (type) expr */
         struct declstate st = { DCASTEXPR };
         struct decl decl = pdecl(&st, pr);
         expect(pr, ')', NULL);
         assert(decl.ty.t);
         unops[nunop].span = tk.span;
         unops[nunop].ty = decl.ty;
         if (++nunop >= arraylength(unops)) {
            ex = exprparse(pr, 999);
            break;
         }
         goto Unary;
      }
   case TKWsizeof:
      span = tk.span;
      if (!match(pr, NULL, '(')) /* sizeof expr */
         goto Unops;
      else if (isdecltok(pr)) { /* sizeof (type) */
         struct declstate st = { DCASTEXPR };
         ty = pdecl(&st, pr).ty;
      } else { /* sizeof (expr) */
         ty = commaexpr(pr).ty;
      }
      peek(pr, &tk);
      if (expect(pr, ')', NULL))
         joinspan(&span.ex, tk.span.ex);
      sizeofcheck(&span, ty);
      ex = mkexpr(ENUMLIT, span, mktype(targ_sizetype), .u = typesize(ty));
      break;
   default:
      fatal(&tk.span, "expected expression (near %'tk)", &tk);
   }

   /* postfix operators */
Postfix:
   switch (peek(pr, &tk)) {
   default: break;
   case TKINC:
   case TKDEC:
      lex(pr, &tk);
      span = ex.span;
      if (!joinspan(&span.ex, tk.span.ex)) span = tk.span;
      incdeccheck(tk.t, &ex, &span);
      ex = mkexpr(tk.t == TKINC ? EPOSTINC : EPOSTDEC, span, ex.ty, .sub = exprdup(pr, &ex));
      goto Postfix;
   case '[': /* a[subscript] */
      lex(pr, NULL);
      rhs = commaexpr(pr);
      span = ex.span;
      if (!joinspan(&span.ex, tk.span.ex) || !joinspan(&span.ex, ex.span.ex)
       || (peek(pr, &tk2), !joinspan(&span.ex, tk.span.ex)))
         span = tk.span;
      expect(pr, ']', NULL);

      if (isint(ex.ty) && isptrcvt(rhs.ty)) {
         /* swap idx[ptr] -> ptr[idx] */
         tmp = ex;
         ex = rhs;
         rhs = tmp;
      }

      ty = subscriptcheck(&ex, &rhs, &span);
      assert(ty.t);
      if (!iszero(rhs)) {
         tmp.sub = exprdup2(pr, &ex, &rhs);
         tmp.t = EADD;
         tmp.span = span;
         tmp.ty = typedecay(ex.ty);
      }
      tmp.sub = exprdup(pr, iszero(rhs) ? &ex : &tmp);
      tmp.span = span;
      tmp.t = EDEREF;
      tmp.qual = ex.ty.flag & TFCHLDQUAL;
      tmp.ty = ty;
      ex = tmp;
      goto Postfix;
   case '(': /* call(args) */
      lex(pr, &tk);
      span = ex.span;
      ex = callexpr(pr, &span, &ex);
      goto Postfix;
   case TKARROW:
      if (ex.ty.t != TYPTR && ex.ty.t != TYARRAY)
         error(&ex.span, "operand to -> is not a pointer (%ty)", ex.ty);
      else
         ex = mkexpr(EDEREF, ex.span, typechild(ex.ty), .qual = ex.ty.flag & TFCHLDQUAL,
                     .sub = exprdup(pr, &ex));
      /* fallthru */
   case '.':
      lex(pr, &tk);
      span = ex.span;
      peek(pr, &tk2); /* field name */
      if (!expect(pr, TKIDENT, NULL)) tk2.s = "";
      if (!joinspan(&span.ex, tk.span.ex) || !joinspan(&span.ex, tk2.span.ex))
         span = tk.span;
      if (!isagg(ex.ty)) {
         error(&span, "member access operand is not an aggregate (%ty)%s", ex.ty,
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
            ex = mkexpr(EGETF, span, fld.t, .qual = ex.qual | fld.qual, .sub = exprdup(pr, &ex),
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
               error(&tk.span, "invalid operand to %'tk (%ty)", &tk, ex.ty);
               ty = mktype(TYINT);
            }
            ex = mkexpr(ek, span, ty, .sub = exprdup(pr, &ex));
            break;
         case TKINC: case TKDEC:
            ty = ex.ty;
            incdeccheck(tk.t, &ex, &span);
            ex = mkexpr(unops[nunop].tt == TKINC ? EPREINC : EPREDEC, span, ty,
                        .sub = exprdup(pr, &ex));
            break;
         case '*':
            if (ex.ty.t == TYPTR || ex.ty.t == TYARRAY) {
               ty = typechild(ex.ty);
               if (isincomplete(ty)) {
                  error(&span, "cannot dereference pointer to incomplete type (%ty)", ty);
                  ty = mktype(TYINT);
               }
            } else {
               error(&span, "invalid operand to unary * (%ty)", ex.ty);
               ty = mktype(TYINT);
            }
            ex = mkexpr(EDEREF, span, ty, .qual = ex.ty.flag & TFCHLDQUAL,
                        .sub = exprdup(pr, &ex));
            break;
         case '&':
            if (!islvalue(&ex))
               error(&span, "operand to unary & is not an lvalue");
            ex = mkexpr(EADDROF, span, mkptrtype(ex.ty, ex.qual), .sub = exprdup(pr, &ex));
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
         ex = mkexpr(ECAST, span, ty, .sub = exprdup(pr, &ex));
      }
   }

   /* binary operators */
   while ((opprec = tkprec(peek(pr, &tk))) >= prec) {
      lex(pr, &tk);
      ek = bintab[tk.t].t;
      if (ek != ECOND) {
         /* ex OP rhs */
         span.sl = tk.span.sl;
         span.ex = ex.span.ex;
         rhs = exprparse(pr, opprec + 1);
         if (!joinspan(&span.ex, tk.span.ex) || !joinspan(&span.ex, rhs.span.ex))
            span.ex = tk.span.ex;
         ty = bintypecheck(&span, tk.t, &ex, &rhs);
         assert(ty.t);
         ex = mkexpr(ek, span, ty, .sub = exprdup2(pr, &ex, &rhs));
      } else {
         /* ex ? tmp : rhs */
         struct expr *sub;
         span.sl = tk.span.sl;
         span.ex = ex.span.ex;
         if (!isscalar(ex.ty))
            error(&ex.span, "?: condition is not a scalar type (%ty)", ex.ty);
         tmp = commaexpr(pr);
         joinspan(&tk.span.ex, tmp.span.ex);
         expect(pr, ':', NULL);
         rhs = expr(pr);
         if (!joinspan(&span.ex, tk.span.ex) || !joinspan(&span.ex, tmp.span.ex)
           || !joinspan(&span.ex, rhs.span.ex))
            span.ex = tk.span.ex;
         ty = condtype(&tmp, &rhs);
         if (!ty.t) {
            error(&span, "bad operands to conditional expression (%ty, %ty)", tmp.ty, rhs.ty);
            ty = tmp.ty;
         }
         sub = alloc(&pr->exarena, 3 * sizeof*sub, 0);
         sub[0] = ex, sub[1] = tmp, sub[2] = rhs;
         ex = mkexpr(ECOND, span, ty, .sub = sub);
      }
   }

   return ex;
}

static struct expr
expr(struct parser *pr)
{
   return exprparse(pr, 2); /* non-comma expr */
}

static struct expr
commaexpr(struct parser *pr)
{
   return exprparse(pr, 1);
}

/*********/
/* -> IR */
/*********/

static union ref exprvalue(struct function *, const struct expr *);

static union ref
expraddr(struct function *fn, const struct expr *ex)
{
   struct decl *decl;
   union ref r;
   struct instr ins;

   switch (ex->t) {
   case ESYM:
      decl = ex->sym;
      assert(decl != NULL);
      switch (decl->scls) {
      case SCAUTO: case SCREGISTER:
         return mkref(RTMP, decl->id);
      case SCEXTERN: case SCNONE:
         return mksymref(fn, decl->name);
      case SCSTATIC:
         assert(!"nyi");
         break;
      default:
         assert(0);
      }
      break;
   case EDEREF:
      return exprvalue(fn, ex->sub);
   case EGETF:
      r = expraddr(fn, ex->sub);
      assert(ex->fld.bitsiz == 0);
      if (ex->fld.off == 0) return r;
      ins.cls = KPTR;
      ins.op = Oadd;
      ins.l = r;
      ins.r = mkintcon(fn, KI4, ex->fld.off);
      return addinstr(fn, ins);
      break;
   default:
      assert(!"lvalue?>");
   }

}

static union ref
genload(struct function *fn, union type t, union ref ref)
{
   struct instr ins = {0};

   assert(isscalar(t));
   ins.cls = type2cls[t.t];
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

static union ref
cvt(struct function *fn, enum typetag to, enum typetag from, union ref ref)
{
   enum irclass kto = type2cls[to], kfrom = type2cls[from];
   struct instr ins = {0};
   if (kto == kfrom && to != TYBOOL) return ref;
   if (ref.t == RICON && kto < KF4) return ref;

   ins.cls = kto;
   ins.l = ref;
   if (kisflt(kto) || kisflt(kfrom)) {
      if (kto == KPTR) kto = siz2intcls[cls2siz[kto]];
      if (kfrom == KPTR) kfrom = siz2intcls[cls2siz[kfrom]];
      if (kisflt(kto) && kfrom == KI4) ins.op = issignedt(from) ? Ocvts4f : Ocvtu4f;
      else if (to == TYBOOL && kisflt(kfrom)) ins.op = Oneq, ins.r = mkfltcon(fn, kfrom, 0.0);
      else if (kisflt(kto) && kfrom == KI8) ins.op = issignedt(from) ? Ocvts8f : Ocvtu8f;
      else if (kto == KF8 && kfrom == KF4) ins.op = Ocvtf4f8;
      else if (kto == KF4 && kfrom == KF8) ins.op = Ocvtf8f4;
      else if (kfrom == KF4) ins.op = issignedt(to) ? Ocvtf4s : Ocvtf4u;
      else if (kfrom == KF8) ins.op = issignedt(to) ? Ocvtf8s : Ocvtf8u;
      else assert(0);
   } else {
      if (to == TYBOOL) ins.op = Oneq, ins.r = mkref(RICON, 0);
      else if (kfrom == KI4 && issignedt(from)) ins.op = Oexts4;
      else if (kfrom == KI4) ins.op = Oextu4;
      else ins.op = Ocopy;
   }
   return addinstr(fn, ins);
}

static union ref
narrow(struct function *fn, enum irclass to, enum typetag tt, union ref ref)
{
   struct instr ins;
   assert(isintt(tt) || tt == TYPTR);
   if (targ_primsizes[tt] >= cls2siz[to]) return ref;
   ins.cls = to;
   switch (targ_primsizes[tt]) {
   case 1: ins.op = issignedt(tt) ? Oexts1 : Oextu1; break;
   case 2: ins.op = issignedt(tt) ? Oexts2 : Oextu2; break;
   case 4: ins.op = issignedt(tt) ? Oexts4 : Oextu4; break;
   default: assert(0);
   }
   ins.l = ref;
   return addinstr(fn, ins);
}

static inline uint
ilog2(uint x) { /* assumes x is a power of 2 */
#ifdef __GNUC__
   return __builtin_ctz(x);
#else
   uint n = 0;
   while (x >>= 1) ++n;
   return n;
#endif
}

union ref
genptroff(struct function *fn, enum op op, uint siz, union ref ptr,
          enum typetag tt, union ref idx)
{
   uint cls = type2cls[targ_sizetype];
   union ref off;
   assert(siz);

   idx = cvt(fn, targ_sizetype, tt, idx);
   if (siz ==  1) off = idx;
   else if (idx.t == RICON)
      off = mkintcon(fn, cls, idx.i * siz);
   else if ((siz & siz-1) == 0) /* is power of 2 */
      off = addinstr(fn,
            (struct instr) { Oshl, cls, .l = idx, .r = mkintcon(fn, cls, ilog2(siz)) });
   else
      off = addinstr(fn,
            (struct instr) { Omul, cls, .l = idx, .r = mkintcon(fn, cls, siz) });
   assert(in_range(op, Oadd, Osub));
   return addinstr(fn, (struct instr) { op, KPTR, .l = ptr, .r = off });
}

union ref
genptrdiff(struct function *fn, uint siz, union ref a, union ref b)
{
   uint cls = type2cls[targ_ptrdifftype];
   assert(siz > 0);
   a = addinstr(fn, (struct instr) { Osub, cls, .l = a, .r = b });
   if (siz == 1) return a;
   else if ((siz & siz-1) == 0) /* is power of 2 */
      return addinstr(fn, (struct instr) { Osar, cls, a, mkintcon(fn, cls, ilog2(siz)) });
   else
      return addinstr(fn, (struct instr) { Odiv, cls, a, mkintcon(fn, cls, siz) });
}

static union ref
exprvalue(struct function *fn, const struct expr *ex)
{
   union type ty;
   union ref r, q;
   struct block *tr, *fl, *end;
   enum irclass cls = type2cls[ex->ty.t];
   struct instr ins = {0};
   int swp = 0;
   struct expr *sub;

   eval((struct expr *)ex, EVFOLD);
   sub = ex->sub;

   if (ex->ty.t == TYARRAY || ex->ty.t == TYFUNC)
      return expraddr(fn, ex);
   switch (ex->t) {
   case ENUMLIT:
      if (isflt(ex->ty))
         return mkfltcon(fn, cls, ex->f);
      return mkintcon(fn, cls, ex->i);
   case ESYM:
      return genload(fn, ex->ty, expraddr(fn, ex));
   case EGETF:
      return genload(fn, ex->ty, expraddr(fn, ex));
   case ECAST:
      if (ex->ty.t == TYVOID) return exprvalue(fn, sub);
   case EPLUS:
      return cvt(fn, ex->ty.t, sub->ty.t, exprvalue(fn, sub));
   case ENEG:
      ins.op = Oneg;
      goto Unary;
   case ECOMPL:
      ins.op = Onot;
   Unary:
      ins.l = exprvalue(fn, sub);
      ins.l = cvt(fn, ex->ty.t, sub->ty.t, ins.l);
      ins.cls = cls;
      return addinstr(fn, ins);
   case ELOGNOT:
      ins.op = Oequ;
      ins.l = exprvalue(fn, sub);
      ins.l = cvt(fn, ex->ty.t, sub->ty.t, ins.l);
      ins.r = mkintcon(fn, cls, 0);
      ins.cls = cls;
      return addinstr(fn, ins);
   case EDEREF:
      return genload(fn, ex->ty, exprvalue(fn, sub));
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
      ins.op = issigned(ex->ty) ? Osar : Oshr;
      goto BinArith;
   case ESUB:
      ins.op = Osub;
      goto BinArith;
   case EADD:
      ins.op = Oadd;
   BinArith:
      ins.l = exprvalue(fn, &sub[0]);
      ins.r = exprvalue(fn, &sub[1]);
      if (ins.op == Osub && isptrcvt(sub[0].ty) && isptrcvt(sub[1].ty)) {
         /* ptr - ptr */
         return genptrdiff(fn, typesize(typechild(sub[0].ty)), ins.l, ins.r);
      } else if ((ins.op != Oadd && ins.op != Osub) || cls != KPTR) {
         /* num OP num */
         ins.l = cvt(fn, ex->ty.t, sub[0].ty.t, ins.l);
         ins.r = cvt(fn, ex->ty.t, sub[1].ty.t, ins.r);
      } else {
         assert(isptrcvt(sub[0].ty));
         /* ptr +/- num */
         return genptroff(fn, ins.op, typesize(typechild(sub[0].ty)), ins.l, sub[1].ty.t, ins.r);
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
         ins.r = mkintcon(fn, KI4, typesize(typechild(ex->ty)));
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
         ins.r = mkintcon(fn, KI4, typesize(typechild(ex->ty)));
      else
         ins.r = mkref(RICON, 1);
      q = addinstr(fn, ins);
      genstore(fn, sub->ty, r, q);
      return narrow(fn, cls, ex->ty.t, q);
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
      ins.op = Olth;
      swp = 1;
      goto Cmp;
   case EGTE:
      ins.op = Olte;
      swp = 1;
   Cmp:
      ty = cvtarith(sub[0].ty, sub[1].ty);
      if (isunsigned(ty) && in_range(ins.op, Olth, Olte))
         ins.op += Oulth - Olth;
      ins.l = exprvalue(fn, &sub[0^swp]);
      ins.l = cvt(fn, ex->ty.t, ty.t, ins.l);
      ins.r = exprvalue(fn, &sub[1^swp]);
      ins.r = cvt(fn, ex->ty.t, ty.t, ins.r);
      ins.cls = cls;
      return addinstr(fn, ins);
      break;
   case ESET:
      assert(isscalar(ex->ty));
      return genstore(fn, ex->ty, expraddr(fn, &sub[0]), exprvalue(fn, &sub[1]));
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
      ins.op = issigned(ex->ty) ? Osar : Oshr;
      goto Compound;
   case ESETSUB:
      ins.op = Osub;
      goto Compound;
   case ESETADD:
      ins.op = Oadd;
   Compound:
      r = expraddr(fn, &sub[0]);
      ty = in_range(ex->t, ESETSHL, ESETSHR) ? mktype(intpromote(ex->ty.t))
                                             : cvtarith(sub[0].ty, sub[1].ty);
      ins.cls = cls;
      ins.l = genload(fn, ex->ty, r);
      ins.r = exprvalue(fn, &sub[1]);
      if ((ins.op != Oadd && ins.op != Osub) || cls != KPTR) {
         ins.l = cvt(fn, ty.t, sub[0].ty.t, ins.l);
         ins.r = cvt(fn, ex->ty.t, sub[1].ty.t, ins.r);
         q = addinstr(fn, ins);
      } else {
         q = genptroff(fn, ins.op, typesize(typechild(ex->ty)), ins.l, sub[1].ty.t, ins.r);
      }
      genstore(fn, ex->ty, r, q);
      return narrow(fn, cls, ex->ty.t, q);
   case ECALL:
      {
         const struct typedata *td = &typedata[sub[0].ty.dat];
         union ref argsbuf[10];
         union irtype typbuf[10];
         vec_of(union ref) args = VINIT(argsbuf, arraylength(argsbuf));
         vec_of(union irtype) typs = VINIT(typbuf, arraylength(typbuf));
         ins.op = Ocall;
         assert(isscalar(ex->ty) || ex->ty.t == TYVOID);
         ins.cls = type2cls[ex->ty.t];
         ins.l = exprvalue(fn, &sub[0]);
         for (int i = 0; i < ex->narg; ++i) {
            struct expr *arg = &sub[i+1];
            union type ty = i < td->nmemb ? td->param[i] : argpromote(arg->ty);
            vpush(&args, cvt(fn, ty.t, arg->ty.t, exprvalue(fn, arg)));
            vpush(&typs, mkirtype(ty));
         }
         ins.r = mkcall(fn, sub[0].ty, ex->narg, args.p, typs.p);
         vfree(&args);
         vfree(&typs);
         return addinstr(fn, ins);
      }
   case ECOND:
      r = exprvalue(fn, &sub[0]);
      tr = newblk(fn);
      fl = newblk(fn);
      end = newblk(fn);
      putjump(fn, Jbcnd, r, tr, fl);
      useblk(fn, tr);
      r = cvt(fn, ex->ty.t, sub[1].ty.t, exprvalue(fn, &sub[1]));
      putjump(fn, Jb, NOREF, end, NULL);
      useblk(fn, fl);
      q = cvt(fn, ex->ty.t, sub[2].ty.t, exprvalue(fn, &sub[2]));
      putjump(fn, Jb, NOREF, end, NULL);
      useblk(fn, end);
      return addphi2(fn, type2cls[ex->ty.t], tr, r, fl, q);
   case ELOGAND:
      r = exprvalue(fn, &sub[0]);
      fl = fn->curblk;
      tr = newblk(fn);
      end = newblk(fn);
      putjump(fn, Jbcnd, r, tr, end);
      useblk(fn, tr);
      q = cvt(fn, TYBOOL, sub[1].ty.t, exprvalue(fn, &sub[1]));
      putjump(fn, Jb, NOREF, end, NULL);
      useblk(fn, end);
      return addphi2(fn, KI4, fl, mkref(RICON, 0), tr, q);
   case ELOGIOR:
      r = exprvalue(fn, &sub[0]);
      tr = fn->curblk;
      fl = newblk(fn);
      end = newblk(fn);
      putjump(fn, Jbcnd, r, end, fl);
      useblk(fn, fl);
      q = cvt(fn, TYBOOL, sub[1].ty.t, exprvalue(fn, &sub[1]));
      putjump(fn, Jb, NOREF, end, NULL);
      useblk(fn, end);
      return addphi2(fn, KI4, tr, mkref(RICON, 1), fl, q);
   case ESEQ:
      (void)exprvalue(fn, &sub[0]);
      return exprvalue(fn, &sub[1]);
   default: assert(!"nyi expr");
   }
}

static void
stmtterm(struct parser *pr)
{
   expect(pr, ';', "to terminate previous statement");
}

static void block(struct parser *pr, struct function *fn);

static bool /* return 1 if stmt is terminating (all codepaths return) */
stmt(struct parser *pr, struct function *fn)
{
   struct block *tr, *fl, *end, *begin;
   struct expr ex;
   union ref r;
   bool terminates = 0;
   const bool doemit = fn->curblk;

#define EMITS if (doemit && !nerror)

   switch (peek(pr, NULL)) {
   case '{':
      lex(pr, NULL);
      {
         struct env e;
         envdown(pr, &e);
         block(pr, fn);
         envup(pr);
      }
      break;
   case ';':
      lex(pr, NULL);
      break;
   case TKWif:
      lex(pr, NULL);
      expect(pr, '(', NULL);
      ex = commaexpr(pr);
      expect(pr, ')', NULL);
      if (!isscalar(ex.ty))
         error(&ex.span, "'if' condition is not a scalar (%ty)", ex.ty);
      tr = fl = end = NULL;
      EMITS {
         tr = newblk(fn);
         fl = newblk(fn);
         r = exprvalue(fn, &ex);
         EMITS {
            putjump(fn, Jbcnd, r, tr, fl);
            useblk(fn, tr);
         }
      }
      terminates = stmt(pr, fn);
      if (!match(pr, NULL, TKWelse)) {
         end = fl;
         EMITS if (!terminates) putjump(fn, Jb, NOREF, end, NULL);
         terminates = 0;
      } else {
         EMITS {
            if (!terminates) putjump(fn, Jb, NOREF, end = newblk(fn), NULL);
            useblk(fn, fl);
         }
         terminates &= stmt(pr, fn);
         EMITS {
            if (fn->curblk) putjump(fn, Jb, NOREF, end, NULL);
         }
      }
      EMITS if (!terminates) useblk(fn, end);
      break;
   case TKWwhile:
      lex(pr, NULL);
      expect(pr, '(', NULL);
      ex = commaexpr(pr);
      expect(pr, ')', NULL);
      if (!isscalar(ex.ty))
         error(&ex.span, "'while' condition is not a scalar (%ty)", ex.ty);
      tr = begin = end = NULL;
      EMITS {
         begin = newblk(fn);
         putjump(fn, Jb, NOREF, begin, NULL);
         useblk(fn, begin);
         r = exprvalue(fn, &ex);
         EMITS {
            putjump(fn, Jbcnd, r, tr = newblk(fn), end = newblk(fn));
            useblk(fn, tr);
         }
      }
      terminates = stmt(pr, fn);
      EMITS {
         if (!terminates) putjump(fn, Jb, NOREF, begin, NULL);
         useblk(fn, end);
      }
      break;
   case TKWreturn:
      lex(pr, NULL);
      if (fn->retty.t != TYVOID) {
         ex = commaexpr(pr);
         if (!assigncheck(fn->retty, &ex)) {
            error(&ex.span,
                  "cannot return '%ty' value from function with return type '%ty'",
                  ex.ty, fn->retty);
         }
         EMITS {
            r = cvt(fn, fn->retty.t, ex.ty.t, exprvalue(fn, &ex));
            putjump(fn, Jrets, r, NULL, NULL);
         }
      } else {
         EMITS putjump(fn, Jret, NOREF, NULL, NULL);
      }
      stmtterm(pr);
      break;
   default:
      ex = commaexpr(pr);
      stmtterm(pr);
      EMITS exprvalue(fn, &ex);
      break;
   }
   freearena(pr->exarena);
   return fn->curblk == NULL;
}

static void
block(struct parser *pr, struct function *fn)
{
   struct token tk;
   const bool doemit = fn->curblk;

   while (!match(pr, &tk, '}')) {
      if (isdecltok(pr)) { /* decl */
         struct expr ini;
         struct declstate st = { DFUNCVAR };
         do {
            struct decl decl = pdecl(&st, pr);
            enum op op;
            uint siz, align, nalloc;
            if (decl.name) {
               static int staticid;
               bool put = 0;

               switch (decl.scls) {
               case SCSTATIC:
                  decl.id = ++staticid;
                  break;
               case SCNONE:
                  decl.scls = SCAUTO;
               case SCAUTO:
               case SCREGISTER:
                  if (isincomplete(decl.ty) || decl.ty.t == TYFUNC) {
                     error(&decl.span,
                           "declaring variable '%s' with %s type '%ty'",
                           decl.name, decl.ty.t == TYFUNC ? "function" : "incomplete",
                           decl.ty);
                     goto Err;
                  }
                  switch (align = typealign(decl.ty)) {
                  case 1:  op = Oalloca1; break;
                  case 2:  op = Oalloca2; break;
                  case 4:  op = Oalloca4; break;
                  case 8:  op = Oalloca8; break;
                  case 16: op = Oalloca16; break;
                  default: assert(!"align");
                  }
                  siz = typesize(decl.ty);
                  nalloc = siz/align + ((siz&(align-1)) != 0);
                  EMITS {
                     decl.id = addinstr(fn,
                                 (struct instr) { op, KPTR, mkintcon(fn, KI4, nalloc) }).idx;
                  }
                  if (st.varini) {
                     putdecl(pr, &decl);
                     put = 1;
                     ini = expr(pr);
                     pdecl(&st, pr);
                     if (!assigncheck(decl.ty, &ini)) {
                        struct span span = decl.span;
                        joinspan(&span.ex, ini.span.ex);
                        error(&span, "cannot initialize '%ty' variable with '%ty'",
                              decl.ty, ini.ty);
                     }
                     EMITS genstore(fn, decl.ty, mkref(RTMP, decl.id), exprvalue(fn, &ini));
                  }
                  break;
               case SCTYPEDEF: break;
               default: assert(0);
               }
            Err:
               if (!put) putdecl(pr, &decl);
            }
         } while (0);
      } else {
         stmt(pr, fn);
      }
   }
   pr->fnblkspan = tk.span;
}

static void
function(struct parser *pr, struct function *fn, const char **pnames, const struct span *pspans)
{
   const struct typedata *td = &typedata[fn->fnty.dat];
   const bool doemit = fn->curblk;
   struct env e;
   envdown(pr, &e);
   for (int i = 0; i < td->nmemb; ++i) {
      if (pnames[i]) {
         uint siz, align, nalloc;
         struct decl arg = { .ty = td->param[i], .qual = tdgetqual(td->quals, i),
                             .name = pnames[i], .scls = SCAUTO, .span = pspans[i] };
         enum op op;
         switch (align = typealign(arg.ty)) {
         case 1:  op = Oalloca1; break;
         case 2:  op = Oalloca2; break;
         case 4:  op = Oalloca4; break;
         case 8:  op = Oalloca8; break;
         case 16: op = Oalloca16; break;
         default: assert(!"align");
         }
         siz = typesize(arg.ty);
         nalloc = siz/align + ((siz&(align-1)) != 0);
         EMITS {
            struct instr alloca = { op, KPTR, mkintcon(fn, KI4, nalloc) };
            arg.id = addinstr(fn, alloca).idx;
            genstore(fn, arg.ty, mkref(RTMP, arg.id), mkref(RARG, i));
         }
         putdecl(pr, &arg);
      } else {
         warn(&pspans[i], "missing name of parameter #%d", i+1);
      }
   }
   block(pr, fn);
   envup(pr);
   if (fn->curblk) {
      if (fn->retty.t != TYVOID && !nerror) {
         warn(&pr->fnblkspan, "non-void function may not return a value");
      }
      putjump(fn, Jret, NOREF, NULL, NULL);
   }
}

/********/
/* DECL */
/********/

static union type
buildagg(struct parser *pr, enum typetag tt, const char *name, int id)
{
   struct token tk;
   union type t;
   struct span flexspan;
   struct namedfield fbuf[32];
   vec_of(struct namedfield) fld = VINIT(fbuf, arraylength(fbuf));
   struct typedata td = {tt};
   bool isunion = tt == TYUNION;
   const char *tag = isunion ? "union" : "struct";

   while (!match(pr, &tk, '}')) {
      struct declstate st = { DFIELD };
      do {
         struct decl decl = pdecl(&st, pr);
         if (fld.n && td.flexi) {
            td.flexi = 0;
            error(&flexspan, "flexible array member is not at end of struct");
         }
         if (!isunion && decl.ty.t == TYARRAY && !typearrlen(decl.ty)) {
            td.flexi = 1;
            flexspan = decl.span;
         } else if (isincomplete(decl.ty)) {
            error(&decl.span, "field has incomplete type (%ty)", decl.ty);
         } else if (decl.ty.t == TYFUNC)  {
            error(&decl.span, "field has function type (%ty)", decl.ty);
         }
         if (decl.ty.t) {
            uint align = typealign(decl.ty);
            uint siz = typesize(decl.ty);
            uint off = isunion ? 0 : alignup(td.siz, align);
            struct namedfield f = { decl.name, { decl.ty, off, .qual = decl.qual }};
            vpush(&fld, f);
            td.anyconst |= decl.qual & QCONST;
            if (isagg(decl.ty)) {
               td.anyconst |= typedata[decl.ty.dat].anyconst;
               if (typedata[decl.ty.dat].flexi)
                  error(&decl.span, "nested aggregate has flexible array member");
            }
            if (isunion)
               td.siz = td.siz < siz ? siz : td.siz;
            else
               td.siz = off + siz;
            td.align = td.align < align ? align : td.align;
         }
      } while (st.more);
   }
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

static union type
buildenum(struct parser *pr, const char *name)
{
   union type t;
   struct typedata td = {TYENUM};
   enum typetag backing = TYINT;

   t = mktagtype(name, &td);
   t.backing = backing;
   return t;
}

static union type
tagtype(struct parser *pr, enum toktag kind)
{
   struct token tk;
   union type t;
   struct span span;
   enum typetag tt = kind == TKWenum ? TYENUM : kind == TKWstruct ? TYSTRUCT : TYUNION;
   const char *tag = NULL;

   peek(pr, &tk);
   if (match(pr, &tk, TKIDENT))
      tag = tk.s;
   span = tk.span;
   if (!match(pr, NULL, '{')) {
      if (!tag) {
         error(&tk.span, "expected %tt name or '{'", kind);
         return mktype(0);
      }
      t = gettagged(pr, &span, tt, tag, /* def? */ peek(pr, NULL) == ';');
   } else {
      if (tt != TYENUM) {
         if (tag) {
            t = deftagged(pr, &span, tt, tag);
            if (t.t != tt || !isincomplete(t)) {
               if (t.t != tt)
                  error(&tk.span,
                        "defining tagged type %'tk as %tt clashes with previous definition",
                        &tk, kind);
               else
                  error(&tk.span, "redefinition of '%tt %s'", kind, tag);
               note(&span, "previous definition:");
            }
         }
         t = buildagg(pr, tt, tag, tag ? typedata[t.dat].id : -1);
      } else {
         t = buildenum(pr, tag);
      }
   }

   if (t.t != tt) {
      error(&tk.span, "declaring tagged type %'tk as %tt clashes with previous definition",
            &tk, kind);
      note(&span, "previous definition:");
   }
   return t;
}

static void
declspec(struct declstate *st, struct parser *pr)
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
      peek(pr, &tk);
      switch (tk.t) {
      case TKWconst:
         st->qual |= QCONST;
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
         lex(pr, &tk);
         st->base = tagtype(pr, tk.t);
         st->tagdecl = 1;
         if (!span.ex.len) span.ex = tk.span.ex;
         joinspan(&span.ex, tk.span.ex);
         goto End;
      case TKIDENT:
         if (!st->base.t && !arith && (decl = finddecl(pr, tk.s))
             && decl->scls == SCTYPEDEF) {
            st->base = decl->ty;
            break;
         }
         /* fallthru */
      default:
         if (!span.ex.len) span.ex = tk.span.ex;
         goto End;
      }
      if (!span.ex.len) span.ex = tk.span.ex;
      joinspan(&span.ex, tk.span.ex);
      lex(pr, &tk);
      if (st->base.t) break;
   }
End:
   if (st->base.t && arith) {
      /* combining arith type specifiers and other types */
   Bad:
      error(&span, "invalid declaration specifier");
   } else if (!st->base.t && arith) {
      enum typetag t;
      ioflush(&bstderr);
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
      else
         goto Bad;
      st->base = mktype(t);
   } else if (!st->base.t && ccopt.cstd < STDC99) {
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
sclass(struct parser *pr, struct span *span)
{
   struct token tk;
   int sc = 0, first = 1;
   for (;; lex(pr, &tk)) {
      switch (peek(pr, &tk)) {
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
cvqual(struct parser *pr)
{
   struct token tk;
   int q = 0;
   while (match(pr, &tk, TKWconst) || match(pr, &tk, TKWvolatile))
      q |= tk.t == TKWconst ? QCONST : QVOLATILE;
   return q;
}

static void
decltypes(struct parser *pr, struct decllist *list, const char **name, struct span *span) {
   struct token tk;
   struct decllist *ptr, node;

   while (match(pr, &tk, '*')) {
      node.t = TYPTR;
      node.qual = cvqual(pr);
      node.span = tk.span;
      declinsert(list, &node);
   }
   ptr = list->next;
   switch (peek(pr, &tk)) {
   case '(':
      lex(pr, &tk);
      if (isdecltok(pr)) {
         goto Func;
      } else if (match(pr, &tk, ')')) {
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
         decltypes(pr, list, name, span);
         expect(pr, ')', NULL);
      }
      break;
   case TKIDENT:
      if (!name)
         error(&tk.span, "unexpected identifier in type name");
      else {
         *name = tk.s;
         *span = tk.span;
      }
      lex(pr, &tk);
      break;
   default:
      *span = tk.span;
      if (name)
         *name = NULL;
   }
   for (;;) {
      if (match(pr, &tk, '[')) {
         node.span = tk.span;
         uint n = 0;
         if (!match(pr, &tk, ']')) {
            struct expr ex = expr(pr);
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
            peek(pr, &tk);
            joinspan(&node.span.ex, tk.span.ex);
            expect(pr, ']', NULL);
         }
         node.t = TYARRAY;
         node.len = n;
         declinsert(ptr->prev, &node);
      } else if (match(pr, &tk, '(')) Func: {
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

         while (!match(pr, &tk, ')')) {
            struct declstate st = { DFUNCPARAM };
            struct decl decl;
            if (match(pr, &tk, TKDOTS)) {
               node.variadic = 1;
               expect(pr, ')', NULL);
               break;
            }
            decl = pdecl(&st, pr);
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
            if (!match(pr, &tk, ',')) {
               expect(pr, ')', NULL);
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
declarator(struct declstate *st, struct parser *pr) {
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

   decltypes(pr, &list, st->kind == DCASTEXPR ? NULL : &decl.name, &decl.span);
   if (!decl.name && st->kind != DCASTEXPR && st->kind != DFUNCPARAM) {
      if (list.prev == &list) lex(pr, NULL);
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
            error(&l->span, "array has incomplete element type (%ty)", decl.ty);
         else if (decl.ty.t == TYFUNC)
            error(&l->span, "array has element has function type (%ty)", decl.ty);
         decl.ty = mkarrtype(decl.ty, decl.qual, l->len);
         decl.qual = 0;
         break;
      case TYFUNC:
         if (decl.ty.t == TYFUNC)
            error(&decl.span, "function cannot return function type (%ty)", decl.ty);
         else if (decl.ty.t == TYARRAY)
            error(&decl.span, "function cannot return array type", decl.ty);
         else if (decl.ty.t != TYVOID && isincomplete(decl.ty))
            error(&decl.span, "function cannot return incomplete type (%ty)", decl.ty);
         decl.ty = mkfntype(decl.ty, l->npar, l->param, l->pqual, l->kandr, l->variadic);
         if (l->param != declparamtmp) free(l->param);
         if (l->pqual != declpqualtmp) free(l->pqual);
         if (l->prev == &list && l->npar) { /* last */
            st->pnames = alloc(&pr->fnarena, l->npar * sizeof(char *), 0);
            st->pspans = alloc(&pr->fnarena, l->npar * sizeof(struct span), 0);
            memcpy(st->pnames, l->pnames, l->npar * sizeof(char *));
            memcpy(st->pspans, l->pspans, l->npar * sizeof(struct span));
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

static struct decl
pdecl(struct declstate *st, struct parser *pr) {
   struct token tk;
   struct decl decl;
   bool iniallowed = st->kind != DFIELD && st->kind != DFUNCPARAM && st->kind != DCASTEXPR;
   bool first = 0;

   if (st->varini) {
      memset(&decl, 0, sizeof decl);
      goto AfterVarIni;
   }

   if (!st->base.t) {
      first = 1;
      st->scls = sclass(pr, &tk.span);
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
      declspec(st, pr);
   }

   if (first && st->tagdecl && match(pr, &tk, ';')) {
      decl = (struct decl) { st->base, st->scls, st->qual, st->align, tk.span };
      return decl;
   }
   decl = declarator(st, pr);

   if (iniallowed && match(pr, &tk, '=')) {
      st->varini = 1;
      return decl;
   } else if (first && decl.ty.t == TYFUNC && match(pr, &tk, '{')) {
      st->funcdef = 1;
      return decl;
   }

AfterVarIni:
   st->varini = 0;
   st->more = 0;
   if (st->kind != DCASTEXPR && st->kind != DFUNCPARAM) {
      if (match(pr, &tk, ','))
         st->more = 1;
      else expect(pr, st->kind == DFUNCPARAM ? ')' : ';', "or `,'");
   }

   return decl;
}

void
parse(struct parser *pr)
{
   struct token tk[1];

   if (!pr->env) pr->env = &toplevel;
   if (!tlarena) {
      enum { N = 1<<12 };
      static union { char m[sizeof(struct arena) + N]; struct arena *_align; } amem[3];

      tlarena = (void *)amem[0].m;
      tlarena->cap = N;
      pr->fnarena = (void *)amem[1].m;
      pr->fnarena->cap = N;
      pr->exarena = (void *)amem[2].m;
      pr->exarena->cap = N;
   }

   putdecl(pr, &(struct decl) { mktype(TYVALIST), SCTYPEDEF, .name = intern("__builtin_va_list") });

   while (peek(pr, tk) != TKEOF) {
      struct expr ini;
      struct declstate st = { DTOPLEVEL, };
      do {
         int nerr = nerror;
         struct decl decl = pdecl(&st, pr);

         if (nerror != nerr) {
            if (st.varini) {
               (void)expr(pr);
               pdecl(&st, pr);
            }
            continue;
         }
         if (decl.name) efmt("%s : %tq\n", decl.name, decl.ty, decl.qual);
         if (st.funcdef) {
            const struct typedata *td =  &typedata[decl.ty.dat];
            struct function fn = { pr->fnarena, decl.name, .globl = decl.scls != SCSTATIC };
            fn.fnty = decl.ty;
            fn.retty = td->ret;
            putdecl(pr, &decl);
            irinit(&fn);
            function(pr, &fn, st.pnames, st.pspans);
            if (!nerror) irdump(&fn, decl.name);
         } else if (decl.name) {
            putdecl(pr, &decl);
            if (st.varini) {
               ini = expr(pr);
               pdecl(&st, pr);
               if (!assigncheck(decl.ty, &ini))
                  error(&ini.span, "cannot initialize %ty with %ty", decl.ty, ini.ty);
               if (!eval(&ini, EVSTATICINI))
                  error(&ini.span, "cannot evaluate expression statically");
            }
         }
         freearena(pr->fnarena);
         freearena(pr->exarena);
      } while (st.more);
   }
}

/* vim:set ts=3 sw=3 expandtab: */
