#include "c.h"
#include "ir.h"

static const Type ANYTYPE = { .bits = -1u };

static bool
callcheck(const Span *span, int nparam, const Type *param, int narg, Expr *args)
{
   bool ok = 1;
   for (int i = 0, n = narg < nparam ? narg : nparam; i < n; ++i) {
      if (param[i].bits != ANYTYPE.bits && !assigncheck(typedecay(param[i]), &args[i])) {
         ok = 0;
         error(&args[i].span, "arg #%d of type '%ty' is incompatible with '%ty'",
               i, args[i].ty, param[i]);
      }
   }

   if (narg > nparam) {
      error(&args[nparam].span, "too many args to builtin function taking %d params", nparam);
      ok = 0;
   } else if (narg < nparam) {
      error(span, "not enough args to builtin function taking %d param%s", nparam,
            nparam != 1 ? "s" : "");
      ok = 0;
   }
   return ok;
}

#define DEF_FNLIKE_SEMA(name, retty, ...)                                      \
   static bool                                                                 \
   name##_sema(CComp *cm, Expr *ex) {                                          \
      Type par[] = { {{0}}, __VA_ARGS__ };                                     \
      ex->ty = retty;                                                          \
      return callcheck(&ex->span, countof(par)-1, par+1, ex->narg, ex->sub+1); \
   }

/* __builtin_va_start */
static bool
va_start_sema(CComp *cm, Expr *ex)
{
   ex->ty = mktype(TYVOID);
   return callcheck(&ex->span, 1, &cvalistty, ex->narg, ex->sub+1);
}
static Ref
va_start_comp(Function *fn, Expr *ex, bool discard)
{
   assert(ex->t == ECALL && ex->narg == 1);
   assert(typedecay(ex->sub[1].ty).bits == typedecay(cvalistty).bits);
   if (!typedata[fn->fnty.dat].variadic)
      error(&ex->span, "va_start used in non-variadic function");
   addinstr(fn, mkinstr1(Ovastart, 0, compileexpr(fn, &ex->sub[1], 0)));
   return NOREF;
}

/* __builtin_va_end */
static bool
va_end_sema(CComp *cm, Expr *ex)
{
   ex->ty = mktype(TYVOID);
   return callcheck(&ex->span, 1, &cvalistty, ex->narg, ex->sub+1);
}

static Ref
va_end_comp(Function *fn, Expr *ex, bool discard)
{
   return NOREF;
}

/* __builtin_va_copy */
DEF_FNLIKE_SEMA(va_copy, mktype(TYVOID), cvalistty, cvalistty)
static Ref
va_copy_comp(Function *fn, Expr *ex, bool discard)
{
   IRType typ = mkirtype(cvalistty.t == TYARRAY ? typechild(cvalistty) : cvalistty);
   for (int i = 1; i <= 2; ++i)
      assert(typedecay(ex->sub[i].ty).bits == typedecay(cvalistty).bits);
   Ref dst = compileexpr(fn, &ex->sub[1], 0), src = compileexpr(fn, &ex->sub[2], 0);
   addinstr(fn, mkarginstr(typ, dst));
   addinstr(fn, mkarginstr(typ, src));
   addinstr(fn, mkintrin(INstructcopy, 0, 2));
   return NOREF;
}

/* __builtin_trap */
DEF_FNLIKE_SEMA(trap, mktype(TYVOID), )
static Ref
trap_comp(Function *fn, Expr *ex, bool discard)
{
   puttrap(fn);
   useblk(fn, newblk(fn)); /* unreachable block, but simplifies expr codegen */
   return NOREF;
}

/* __builtin_unreachable */
DEF_FNLIKE_SEMA(unreachable, mktype(TYVOID), )
static Ref
unreachable_comp(Function *fn, Expr *ex, bool discard)
{
   /* just compile to a trap, don't have poison values/control flow */
   return trap_comp(fn, ex, discard);
}

/* __builtin_bswap16 */
DEF_FNLIKE_SEMA(bswap16, mktype(TYUSHORT), mktype(TYUSHORT))
static Ref
bswap16_comp(Function *fn, Expr *ex, bool discard)
{
   assert(isint(ex->ty));
   return irunop(fn, Obswap16, KI32, scalarcvt(fn, ex->ty, ex->sub[1].ty,
            compileexpr(fn, &ex->sub[1], 0)));
}
/* __builtin_bswap32 */
DEF_FNLIKE_SEMA(bswap32, mktype(TYUINT), mktype(TYUINT))
static Ref
bswap32_comp(Function *fn, Expr *ex, bool discard)
{
   assert(isint(ex->ty));
   return irunop(fn, Obswap32, KI32, scalarcvt(fn, ex->ty, ex->sub[1].ty,
            compileexpr(fn, &ex->sub[1], 0)));
}
/* __builtin_bswap64 */
DEF_FNLIKE_SEMA(bswap64, mktype(TYUVLONG), mktype(TYUVLONG))
static Ref
bswap64_comp(Function *fn, Expr *ex, bool discard)
{
   assert(isint(ex->ty));
   return irunop(fn, Obswap64, KI64, scalarcvt(fn, ex->ty, ex->sub[1].ty,
            compileexpr(fn, &ex->sub[1], 0)));
}

/* __builtin_constant_p */
DEF_FNLIKE_SEMA(constant_p, mktype(TYINT), ANYTYPE)
static Ref
constant_p_comp(Function *fn, Expr *ex, bool discard)
{
   if (discard) return NOREF;
   ex = &ex->sub[1];
   if (eval(ex, EVFOLD) && isarith(ex->ty)) return mkref(RICON, 1);
   /* XXX: this only performs peephole optimizations, not backend level,
    *      misses things that might be actually constant after -O2. */
   Block *next = newblk(fn);
   putbranch(fn, next);
   useblk(fn, newblk(fn));
   Ref r = compileexpr(fn, ex, 0);
   puttrap(fn);
   useblk(fn, next);
   return mkref(RICON, isnumcon(r));
}

/* __builtin_expect (stub) */
DEF_FNLIKE_SEMA(expect, mktype(TYLONG), mktype(TYLONG), mktype(TYLONG))
static Ref
expect_comp(Function *fn, Expr *ex, bool discard)
{
   return compileexpr(fn, &ex->sub[1], discard);
}

#define LIST_BUILTINS(_) \
   _(va_start) _(va_copy) _(va_end) \
   _(trap) _(bswap16) _(bswap32) _(bswap64) \
   _(unreachable) _(constant_p) _(expect) \

static const struct {
   const char *name;
   Builtin b;
} tab[] = {
#define FNS(x) { "__builtin_" #x, { x##_sema, x##_comp } },
   LIST_BUILTINS(FNS)
#undef FNS
};

void
putbuiltins(Env *env)
{
   for (int i = 0; i < countof(tab); ++i) {
      envadddecl(env, &(Decl) {
         .name = intern(tab[i].name),
         .isbuiltin = 1,
         .builtin = &tab[i].b,
      });
   }
}

/* this is separate because it's a keyword */
Ref
builtin_va_arg_comp(Function *fn, const Expr *ex, bool discard)
{
   assert(ex->t == EVAARG && ex->ty.t);
   enum irclass k = (isagg(ex->ty) || iscomplex(ex->ty)) ? KPTR : type2cls[scalartypet(ex->ty)];
   return addinstr(fn, mkinstr2(Ovaarg, k, compileexpr(fn, ex->sub, 0), mktyperef(mkirtype(ex->ty))));
}

bool
hasbuiltin(const char *name, uint len)
{
   for (int i = 0; i < countof(tab); ++i)
      if (!strncmp(name, tab[i].name, len))
         return 1;
   if (!strcmp(name, "__builtin_va_arg")) return 1;
   return 0;
}

/* vim:set ts=3 sw=3 expandtab: */
