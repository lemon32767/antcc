#include "c.h"
#include "ir.h"

static bool
callcheck(const struct span *span, int nparam, const union type *param, int narg, struct expr *args)
{
   bool ok = 1;
   for (int i = 0, n = narg < nparam ? narg : nparam; i < n; ++i) {
      if (!assigncheck(typedecay(param[i]), &args[i])) {
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
   name##_sema(struct comp *cm, struct expr *ex) {                             \
      union type par[] = { {{0}}, __VA_ARGS__ };                               \
      ex->ty = retty;                                                          \
      return callcheck(&ex->span, countof(par)-1, par+1, ex->narg, ex->sub+1); \
   }

/* __builtin_va_start */
static bool
va_start_sema(struct comp *cm, struct expr *ex)
{
   ex->ty = mktype(TYVOID);
   return callcheck(&ex->span, 1, &cvalistty, ex->narg, ex->sub+1);
}
static union ref
va_start_comp(struct function *fn, struct expr *ex, bool discard)
{
   assert(ex->t == ECALL && ex->narg == 1);
   assert(typedecay(ex->sub[1].ty).bits == typedecay(cvalistty).bits);
   if (!typedata[fn->fnty.dat].variadic)
      error(&ex->span, "va_start used in non-variadic function");
   addinstr(fn, mkinstr(Ovastart, 0, compileexpr(fn, &ex->sub[1], 0)));
   return NOREF;
}

/* __builtin_va_end */
static bool
va_end_sema(struct comp *cm, struct expr *ex)
{
   ex->ty = mktype(TYVOID);
   return callcheck(&ex->span, 1, &cvalistty, ex->narg, ex->sub+1);
}

static union ref
va_end_comp(struct function *fn, struct expr *ex, bool discard)
{
   return NOREF;
}

/* __builtin_va_copy */
DEF_FNLIKE_SEMA(va_copy, mktype(TYVOID), cvalistty, cvalistty)
static union ref
va_copy_comp(struct function *fn, struct expr *ex, bool discard)
{
   union irtype typ = mkirtype(cvalistty.t == TYARRAY ? typechild(cvalistty) : cvalistty);
   for (int i = 1; i <= 2; ++i)
      assert(typedecay(ex->sub[i].ty).bits == typedecay(cvalistty).bits);
   union ref dst = compileexpr(fn, &ex->sub[1], 0), src = compileexpr(fn, &ex->sub[2], 0);
   addinstr(fn, mkarginstr(typ, dst));
   addinstr(fn, mkarginstr(typ, src));
   addinstr(fn, mkintrin(INstructcopy, 0, 2));
   return NOREF;
}

/* __builtin_trap */
DEF_FNLIKE_SEMA(trap, mktype(TYVOID), )
static union ref
trap_comp(struct function *fn, struct expr *ex, bool discard)
{
   puttrap(fn);
   useblk(fn, newblk(fn)); /* unreachable block, but simplifies expr codegen */
   return NOREF;
}

static inline union ref
cvtintref(struct function *fn, enum irclass dst, union ref src)
{
   if (src.t == RTMP) {
      if (insrescls(instrtab[src.i]) != dst)
         return addinstr(fn, mkinstr(Ocopy, dst, src));
      return src;
   } else if (isintcon(src)) {
      vlong x = intconval(src);
      return mkintcon(dst, cls2siz[dst] == 4 ? (int)x : x);
   }
   assert(!"int ref?");
}

/* __builtin_bswap16 */
DEF_FNLIKE_SEMA(bswap16, mktype(TYUSHORT), mktype(TYUSHORT))
static union ref
bswap16_comp(struct function *fn, struct expr *ex, bool discard)
{
   assert(isint(ex->ty));
   return irunop(fn, Obswap16, KI32, scalarcvt(fn, ex->ty, ex->sub[1].ty,
            compileexpr(fn, &ex->sub[1], 0)));
}
/* __builtin_bswap32 */
DEF_FNLIKE_SEMA(bswap32, mktype(TYUINT), mktype(TYUINT))
static union ref
bswap32_comp(struct function *fn, struct expr *ex, bool discard)
{
   assert(isint(ex->ty));
   return irunop(fn, Obswap32, KI32, scalarcvt(fn, ex->ty, ex->sub[1].ty,
            compileexpr(fn, &ex->sub[1], 0)));
}
/* __builtin_bswap64 */
DEF_FNLIKE_SEMA(bswap64, mktype(TYUVLONG), mktype(TYUVLONG))
static union ref
bswap64_comp(struct function *fn, struct expr *ex, bool discard)
{
   assert(isint(ex->ty));
   return irunop(fn, Obswap64, KI64, scalarcvt(fn, ex->ty, ex->sub[1].ty,
            compileexpr(fn, &ex->sub[1], 0)));
}

#define LIST_BUILTINS(_) \
   _(va_start) _(va_copy) _(va_end) \
   _(trap) _(bswap16) _(bswap32) _(bswap64)

static const struct {
   const char *name;
   struct builtin b;
} tab[] = {
#define FNS(x) { "__builtin_" #x, { x##_sema, x##_comp } },
   LIST_BUILTINS(FNS)
#undef FNS
};

void
putbuiltins(struct env *env)
{
   for (int i = 0; i < countof(tab); ++i) {
      envadddecl(env, &(struct decl) {
         .name = intern(tab[i].name),
         .isbuiltin = 1,
         .builtin = &tab[i].b,
      });
   }
}

/* this is separate because it's a keyword */
union ref
builtin_va_arg_comp(struct function *fn, const struct expr *ex, bool discard)
{
   assert(ex->t == EVAARG && ex->ty.t);
   enum irclass k = isagg(ex->ty) ? KPTR : type2cls[scalartypet(ex->ty)];
   return addinstr(fn, mkinstr(Ovaarg, k, compileexpr(fn, ex->sub, 0), mktyperef(mkirtype(ex->ty))));
}

bool
hasbuiltin(const char *name, uint len)
{
   for (int i = 0; i < countof(tab); ++i)
      if (!strncmp(name, tab[i].name, len))
         return 1;
   return 0;
}

/* vim:set ts=3 sw=3 expandtab: */
