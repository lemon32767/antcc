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
   uchar qual : 2;
   uchar isenum : 1;
   struct span span;
   const char *name;
   union {
      struct { ushort align; int id; };
      vlong value;
   };
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
deftagged(struct parser *pr, struct span *span, enum typetag tt, const char *name, union type ty)
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
   return envaddtagged(pr->env, ty.t ? ty : mktagtype(name, &td), span)->ty;
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

   if (callee->t == ESYM && !callee->ty.t) { /* implicit function decl.. */
      const char *name = (void *)callee->sym;
      struct decl decl = {
         ty = mkfntype(mktype(TYINT), 0, NULL, NULL, /* kandr */ 1, 0),
         .scls = SCEXTERN, .span = callee->span, .name = name
      };
      warn(&callee->span, "call to undeclared function '%s'", name);
      ((struct expr *)callee)->sym = putdecl(pr, &decl);
      td = &typedata[ty.dat];
   }

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
         if (lexpeek(pr, NULL) == '(') { /* implicit function decl? */
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
            if (ex.t == EGETF && ex.fld.bitsiz)
               error(&span, "cannot take address of bitfield");
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
         bool leftassoc = (bintab[tk.t].k & BCSET) == 0; /* only the assignment operators are right-associative */
         /* ex OP rhs */
         span.sl = tk.span.sl;
         span.ex = ex.span.ex;
         rhs = exprparse(pr, opprec + leftassoc);
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
   union ref args[2] = { dst, src };
   union irtype typs[2] = { mkirtype(ty) };
   typs[1] = typs[0];
   addinstr(fn, mkintrin(fn, INstructcopy, 0, 2, args, typs));
}

static union ref
structreturn(struct function *fn, const struct expr *src)
{
   return expraddr(fn, src);
}

static union ref compilecall(struct function *fn, const struct expr *ex);

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
         return mksymref(fn, decl->name);
      case SCSTATIC:
         assert(!"nyi");
         break;
      default:
         assert(0);
      }
      break;
   case ESTRLIT:
      return mkdatref(fn, ex->s.n+1, /*align*/ 1, ex->s.p, ex->s.n);
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
      if (ref.t == RICON) {
         assert(kisflt(kto) && kisint(kfrom));
         return mkfltcon(fn, kto, kto == KF4 ? (float)ref.i : (double)ref.i);
      }
      if (kisflt(kto) && kfrom == KI4) ins.op = issignedt(from) ? Ocvts4f : Ocvtu4f;
      else if (to == TYBOOL && kisflt(kfrom)) ins.op = Oneq, ins.r = mkfltcon(fn, kfrom, 0.0);
      else if (kisflt(kto) && kfrom == KI8) ins.op = issignedt(from) ? Ocvts8f : Ocvtu8f;
      else if (kto == KF8 && kfrom == KF4) ins.op = Ocvtf4f8;
      else if (kto == KF4 && kfrom == KF8) ins.op = Ocvtf8f4;
      else if (kfrom == KF4) ins.op = issignedt(to) ? Ocvtf4s : Ocvtf4u;
      else if (kfrom == KF8) ins.op = issignedt(to) ? Ocvtf8s : Ocvtf8u;
      else assert(0);
   } else {
      if (to == TYBOOL) {
         if (from == TYBOOL) return ref;
         if (ref.t == RTMP)
            /* these instrs already have output range of [0,1] */
            if (instrtab[ref.idx].op == Onot || oiscmp(instrtab[ref.idx].op))
               return ref;
         ins.op = Oneq, ins.r = mkzerocon();
      }
      else if (kfrom == KI4 && issignedt(from)) ins.op = Oexts4;
      else if (kfrom == KI4) ins.op = Oextu4;
      else ins.op = Ocopy;
   }
   return addinstr(fn, ins);
}

static union ref
narrow(struct function *fn, enum irclass to, enum typetag tt, union ref ref)
{
   struct instr ins = {0};
   assert(isscalart(tt));
   if (targ_primsizes[tt] >= cls2siz[to]) return ref;
   ins.cls = to;
   if (isfltt(tt)) {
      assert(to == KF4 && tt == TYDOUBLE);
      ins.op = Ocvtf8f4;
   } else {
      static const enum op ext[5][2] = {
         [1] = {Oextu1, Oexts1}, [2] = {Oextu2, Oexts2}, [4] = {Oextu4, Oexts4}
      };
      ins.op = ext[targ_primsizes[tt]][issignedt(tt)];
   }
   ins.l = ref;
   return addinstr(fn, ins);
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
   else if (ispo2(siz))
      off = addinstr(fn,
            mkinstr(Oshl, cls, .l = idx, .r = mkintcon(fn, cls, ilog2(siz))));
   else
      off = addinstr(fn,
            mkinstr(Omul, cls, .l = idx, .r = mkintcon(fn, cls, siz)));
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
      return addinstr(fn, mkinstr(Osar, cls, a, mkintcon(fn, cls, ilog2(siz))));
   else
      return addinstr(fn, mkinstr(Odiv, cls, a, mkintcon(fn, cls, siz)));
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
   vec_of(struct block *) blk;
   vec_of(union ref) ref;
};

static void
condexprrec(struct function *fn, const struct expr *ex, struct condphis *phis,
            int boolcon, struct block *end, struct block *zero)
{
   struct block *tr, *fl, *next;
   union ref r;
   while (ex->t == ESEQ) {
      expreffects(fn, &ex->sub[0]);
      ex = &ex->sub[1];
   }
   if (ex->t == ELOGAND) {
      next = newblk(fn);
      condexprrec(fn, &ex->sub[0], phis, 0, next, end);
      useblk(fn, next);
      condexprrec(fn, &ex->sub[1], phis, -2, end, zero);
   } else if (ex->t == ELOGIOR) {
      next = newblk(fn);
      condexprrec(fn, &ex->sub[0], phis, 1, end, next);
      useblk(fn, next);
      condexprrec(fn, &ex->sub[1], phis, -2, end, zero);
   } else if (ex->t == ECOND) {
      tr = newblk(fn);
      fl = newblk(fn);
      condjump(fn, &ex->sub[0], tr, fl);
      useblk(fn, tr);
      condexprrec(fn, &ex->sub[1], phis, -1, end, zero);
      useblk(fn, fl);
      condexprrec(fn, &ex->sub[2], phis, -1, end, zero);
   } else {
      r = exprvalue(fn, ex);
      vpush(&phis->blk, fn->curblk);
      if (boolcon == -2)
         r = cvt(fn, TYBOOL, ex->ty.t, r);
      if (boolcon >= 0)
         vpush(&phis->ref, mkintcon(fn, KI4, boolcon));
      else
         vpush(&phis->ref, r);
      if (zero) {
         putcondbranch(fn, r, end, zero);
      } else {
         assert(boolcon < 0);
         putbranch(fn, end);
      }
   }
}

static union ref
compilecall(struct function *fn, const struct expr *ex)
{
   struct instr ins = {0};
   struct expr *sub = ex->sub;
   const struct typedata *td = &typedata[sub[0].ty.dat];
   union ref argsbuf[10];
   union irtype typbuf[10];
   vec_of(union ref) args = VINIT(argsbuf, arraylength(argsbuf));
   vec_of(union irtype) typs = VINIT(typbuf, arraylength(typbuf));

   ins.op = Ocall;
   if (isagg(ex->ty)) {
      ins.cls = KPTR;
   } else {
      assert(isscalar(ex->ty) || ex->ty.t == TYVOID);
      ins.cls = type2cls[ex->ty.t];
   }
   ins.l = exprvalue(fn, &sub[0]);
   vresize(&args, ex->narg);
   vresize(&typs, ex->narg);
   /* backwards because of RTL call stack order */
   for (int i = ex->narg - 1; i >= 0; --i) {
      struct expr *arg = &sub[i+1];
      union type ty = i < td->nmemb ? td->param[i] : argpromote(arg->ty);
      args.p[i] = cvt(fn, ty.t, arg->ty.t, exprvalue(fn, arg));
      typs.p[i] = mkirtype(ty);
   }
   if (isagg(ex->ty))
      vpush(&typs, mkirtype(ex->ty));
   ins.r = mkcallarg(fn, isagg(ex->ty), ex->narg, td->variadic ? td->nmemb : td->kandr ? 0 : -1,
                     args.p, typs.p);
   vfree(&args);
   vfree(&typs);
   return addinstr(fn, ins);
}

/* the naive way to generate something like a ? b : c ? d : e, uses multiple phis,
 * this code reduces such nested conditional expressions into one phi */
static union ref
condexprvalue(struct function *fn, const struct expr *ex)
{
   struct block *blkbuf[8];
   union ref refbuf[8];
   struct condphis phis = { VINIT(blkbuf, arraylength(blkbuf)),
                            VINIT(refbuf, arraylength(refbuf))};
   struct block *dst = newblk(fn);
   union ref r;
   condexprrec(fn, ex, &phis, -1, dst, NULL);
   useblk(fn, dst);
   r = addphi(fn, type2cls[ex->ty.t], phis.blk.p, phis.ref.p, phis.blk.n);
   vfree(&phis.blk);
   vfree(&phis.ref);
   return r;
}

static union ref
compileexpr(struct function *fn, const struct expr *ex, bool discard)
{
   union type ty;
   union ref r, q;
   enum irclass cls = type2cls[ex->ty.t];
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
         return mkfltcon(fn, cls, ex->f);
      return mkintcon(fn, cls, ex->i);
   case ESYM:
      if (discard && !(ex->qual & QVOLATILE)) return NOREF;
      return genload(fn, ex->ty, expraddr(fn, ex));
   case EGETF:
      if (discard && !(ex->qual & QVOLATILE)) return NOREF;
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
      return cvt(fn, ex->ty.t, sub->ty.t, r);
   case ENEG:
      ins.op = Oneg;
      goto Unary;
   case ECOMPL:
      ins.op = Onot;
   Unary:
      ins.l = compileexpr(fn, sub, discard);
      if (discard) return NOREF;
      ins.l = cvt(fn, ex->ty.t, sub->ty.t, ins.l);
      ins.cls = cls;
      return addinstr(fn, ins);
   case ELOGNOT:
      for (; sub->t == ELOGNOT; ex = sub, sub = sub->sub)
         swp ^= 1;
      ins.op = Oequ + swp;
      ins.l = compileexpr(fn, sub, discard);
      if (discard) return NOREF;
      ins.l = cvt(fn, ex->ty.t, sub->ty.t, ins.l);
      ins.r = mkintcon(fn, cls, 0);
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
      ins.op = issigned(ex->ty) ? Osar : Oshr;
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
      if (discard) return NOREF;
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
      if (!ty.t) ty.t = TYPTR;
      if (isunsigned(ty) && in_range(ins.op, Olth, Olte))
         ins.op += Oulth - Olth;
      ins.l = compileexpr(fn, &sub[0^swp], discard);
      ins.r = compileexpr(fn, &sub[1^swp], discard);
      if (discard) return NOREF;
      ins.l = cvt(fn, ty.t, sub[0^swp].ty.t, ins.l);
      ins.r = cvt(fn, ty.t, sub[1^swp].ty.t, ins.r);
      ins.cls = cls;
      return addinstr(fn, ins);
   case ESET:
      assert(isscalar(ex->ty));
      r = expraddr(fn, &sub[0]);
      q = cvt(fn, sub[0].ty.t, sub[1].ty.t, exprvalue(fn, &sub[1]));
      genstore(fn, ex->ty, r, q);
      if (discard) return NOREF;
      return narrow(fn, cls, sub[0].ty.t, q);
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
      if (discard) return NOREF;
      return narrow(fn, cls, ex->ty.t, q);
   case ECALL:
      r = compilecall(fn, ex);
      if (isint(ex->ty))
         return narrow(fn, cls, ex->ty.t, r);
      return r;
   case ECOND:
      if (ex->ty.t == TYVOID) {
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
      return condexprvalue(fn, ex);
   case ESEQ:
      expreffects(fn, &sub[0]);
      return compileexpr(fn, &sub[1], discard);
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
         condjump(fn, &ex, tr, fl);
         useblk(fn, tr);
      }
      terminates = stmt(pr, fn);
      if (!match(pr, NULL, TKWelse)) {
         end = fl;
         EMITS if (!terminates) putbranch(fn, end);
         terminates = 0;
      } else {
         EMITS {
            if (!terminates) putbranch(fn, end = newblk(fn));
            useblk(fn, fl);
         }
         terminates &= stmt(pr, fn);
         EMITS {
            if (fn->curblk) putbranch(fn, end);
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
         putbranch(fn, begin);
         useblk(fn, begin);
         condjump(fn, &ex, tr = newblk(fn), end = newblk(fn));
         useblk(fn, tr);
      }
      terminates = stmt(pr, fn);
      EMITS {
         if (!terminates) putbranch(fn, begin);
         useblk(fn, end);
      }
      break;
   case TKWdo:
      lex(pr, NULL);
      begin = end = NULL;
      EMITS {
         putbranch(fn, begin = newblk(fn));
         useblk(fn, begin);
      }
      terminates = stmt(pr, fn);
      expect(pr, TKWwhile, NULL);
      expect(pr, '(', NULL);
      ex = commaexpr(pr);
      expect(pr, ')', NULL);
      if (!isscalar(ex.ty))
         error(&ex.span, "'while' condition is not a scalar (%ty)", ex.ty);
      stmtterm(pr);
      EMITS {
         end = newblk(fn);
         if (!terminates) condjump(fn, &ex, begin, end);
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
            if (isscalar(fn->retty))
               r = cvt(fn, fn->retty.t, ex.ty.t, exprvalue(fn, &ex));
            else
               r = structreturn(fn, &ex);
            putreturn(fn, r, NOREF);
         }
      } else {
         EMITS putreturn(fn, NOREF, NOREF);
      }
      stmtterm(pr);
      break;
   default:
      ex = commaexpr(pr);
      stmtterm(pr);
      EMITS expreffects(fn, &ex);
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
                  EMITS {
                     decl.id = addinstr(fn, mkalloca(typesize(decl.ty), typealign(decl.ty))).idx;
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
                     EMITS {
                        if (isagg(decl.ty))
                           structcopy(fn, decl.ty, mkref(RTMP, decl.id), expraddr(fn, &ini));
                        else
                           genstore(fn, decl.ty, mkref(RTMP, decl.id), exprvalue(fn, &ini));
                     }
                  }
                  break;
               case SCTYPEDEF: break;
               case SCEXTERN: break;
               default: assert(0);
               }
            Err:
               if (!put) putdecl(pr, &decl);
            }
         } while (st.more);
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
   /* add parameters to symbol table and create prologue (arguments) block */
   for (int i = 0; i < td->nmemb; ++i) {
      if (pnames[i]) {
         struct decl arg = { .ty = td->param[i], .qual = tdgetqual(td->quals, i),
                             .name = pnames[i], .scls = SCAUTO, .span = pspans[i] };
         EMITS {
            if (isscalar(arg.ty)) {
               arg.id = addinstr(fn, mkalloca(typesize(arg.ty), typealign(arg.ty))).idx;
               genstore(fn, arg.ty, mkref(RTMP, arg.id), mkref(RPARAM, i));
            } else {
               arg.id = addinstr(fn, mkinstr(Ocopy, KPTR, mkref(RPARAM, i))).idx;
            }
         }
         putdecl(pr, &arg);
      } else if (ccopt.cstd < STDC23) {
         warn(&pspans[i], "missing name of parameter #%d", i+1);
      }
   }
   /* end prologue */
   EMITS {
      struct block *blk;
      putbranch(fn, blk = newblk(fn));
      useblk(fn, blk);
   }
   block(pr, fn);
   envup(pr);
   if (fn->curblk) {
      if (fn->retty.t != TYVOID && !nerror) {
         warn(&pr->fnblkspan, "non-void function may not return a value");
      }
      putreturn(fn, NOREF, NOREF);
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
            if (!decl.name) {
               if (!isagg(decl.ty) || ttypenames[typedata[decl.ty.dat].id]) {
                  warn(&decl.span, "declaration does not declare anything");
                  continue;
               } else if (ccopt.cstd < STDC11 && ccopt.pedant) {
                  warn(&decl.span, "anonymous %s in %M is an extension",
                       decl.ty.t == TYUNION ? "union" : "struct");
               }
            }
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
   *min = isunsignedt(tt) ? 0 : -(1ll << (bits - 1));
   *max = isunsignedt(tt) ? ~0ull >> (64 - bits) : (1ll << (bits - 1)) - 1;
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
buildenum(struct parser *pr, const char *name, const struct span *span)
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
   while (!match(pr, &tk, '}')) {
      struct decl decl = {0};
      peek(pr, &tk);
      expect(pr, TKIDENT, NULL);
      if (match(pr, NULL, '=') || (peek(pr, NULL) == TKNUMLIT && !expect(pr, '=', NULL))) {
         struct expr ex = expr(pr);
         if (eval(&ex, EVINTCONST)) {
            iota = ex.i;
            if (ex.ty.t != ty.t)
               inttyminmax(&tymin, &tymax, ex.ty.t);
            ty = ex.ty;
         } else {
            error(&ex.span, "enum value is not an integer constant");
         }
      } else if (tk.t != TKIDENT) {
         lex(pr, NULL);
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
      putdecl(pr, &decl);
      if (!match(pr, &tk, ',')) {
         if (expect(pr, '}', "or `,'"))
            break;
         else lex(pr, NULL);
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
      warn(&maxvspan, "enumerators exceed range of enum's backing type (%ty)", mktype(td.backing));
   }
   if (td.backing >= TYVLONG && !somelonglong && ccopt.cstd == STDC89 && ccopt.pedant)
      warn(span, "enum backing type is '%ty' in %M", mktype(td.backing));

   ty = mktagtype(name, &td);
   ty.backing = td.backing;
   return ty;
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
      if (tt == TYENUM && !t.t) {
         error(&tk.span, "cannot forward-declare enum");
         return mktype(TYINT);
      }
   } else {
      if (tt != TYENUM) {
         if (tag) {
            t = deftagged(pr, &span, tt, tag, mktype(0));
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
         t = buildagg(pr, tt, tag, tag ? typedata[t.dat].id : -1);
      } else {
         t = buildenum(pr, tag, &span);
         if (tag) deftagged(pr, &span, TYENUM, tag, t);
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
      case TKW_BitInt: case TKW_Complex:
      case TKW_Decimal128: case TKW_Decimal32:
      case TKW_Decimal64: case TKW_Imaginary:
         error(&tk.span, "%'tk is unsupported", &tk);
         arith = arith ? arith : KINT;
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
      st->base = mktype(TYINT);
   } else if (!st->base.t && arith) {
      enum typetag t;
      ioflush(&bstderr);
      if (arith == KFLOAT)
         t = TYFLOAT;
      else if (arith == KDOUBLE)
         t = TYDOUBLE;
      else if (arith == (KLONG | KDOUBLE)) {
         t = TYLDOUBLE;
         error(&span, "`long double' is unsupported");
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
         if (st.funcdef) {
            const struct typedata *td =  &typedata[decl.ty.dat];
            struct function fn = { pr->fnarena, decl.name, .globl = decl.scls != SCSTATIC };
            fn.fnty = decl.ty;
            fn.retty = td->ret;
            putdecl(pr, &decl);
            irinit(&fn);
            function(pr, &fn, st.pnames, st.pspans);
            if (!nerror && ccopt.dbg.p)
               irdump(&fn, decl.name);
            irfini(&fn);
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
            if (ccopt.dbg.p) efmt("var %s : %tq\n", decl.name, decl.ty, decl.qual);
         } else {
            if (ccopt.dbg.p) efmt("type %ty\n", decl.ty);
         }
         freearena(pr->fnarena);
         freearena(pr->exarena);
      } while (st.more);
   }
}

/* vim:set ts=3 sw=3 expandtab: */
