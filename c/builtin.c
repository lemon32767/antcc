#include "c.h"
#include "../ir/ir.h"

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

#define DEF_FNLIKE_SEMA(name, retty, ...)                                        \
   static bool                                                                   \
   name##_sema(struct comp *cm, struct expr *ex) {                               \
      static union type par[] = { {{0}}, __VA_ARGS__ };                          \
      ex->ty = retty;                                                            \
      return callcheck(&ex->span, arraylength(par)-1, par+1, ex->narg, ex->sub+1); \
   }

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
   assert(ex->sub[1].ty.bits == cvalistty.bits);
   if (!typedata[fn->fnty.dat].variadic)
      error(&ex->span, "va_start used in non-variadic function");
   addinstr(fn, mkinstr(Ovastart, 0, compileexpr(fn, &ex->sub[1], 0)));
   return NOREF;
}

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

DEF_FNLIKE_SEMA(trap, mktype(TYVOID), )

static union ref
trap_comp(struct function *fn, struct expr *ex, bool discard)
{
   puttrap(fn);
   return NOREF;
}

union ref
builtin_va_arg_comp(struct function *fn, const struct expr *ex, bool discard)
{
   assert(ex->t == EVAARG && ex->ty.t);
   enum irclass k = isagg(ex->ty) ? KPTR : type2cls[scalartypet(ex->ty)];
   return addinstr(fn, mkinstr(Ovaarg, k, compileexpr(fn, ex->sub, 0), mktyperef(mkirtype(ex->ty))));
}

#define LIST_BUILTINS(_) \
   _(va_start) \
   _(va_end) \
   _(trap)

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
   for (int i = 0; i < arraylength(tab); ++i) {
      const char *intern(const char *);
      envadddecl(env, &(struct decl) {
         .name = intern(tab[i].name),
         .isbuiltin = 1,
         .builtin = &tab[i].b,
      });
   }
}

bool
hasbuiltin(const char *name, uint len)
{
   for (int i = 0; i < arraylength(tab); ++i)
      if (!strncmp(name, tab[i].name, len))
         return 1;
   return 0;
}

/* vim:set ts=3 sw=3 expandtab: */
