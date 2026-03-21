#include "antcc.h"
#include "c_type.h"

extern const struct MCTarg t_x86_64_sysv, t_aarch64_aapcs;
static const struct Targ {
   struct { enum mcarch arch; uint oss, abis; };
   struct { uchar longsize, vlongsize, ptrsize, valistsize; };
   struct { uchar longalign, vlongalign, doublealign, ptralign; };
   bool charsigned;
   uchar sizetype, ptrdifftype, wchartype;
   const struct MCTarg *mctarg;
} targs[] = {
   { {ISx86_64,  -1, -1}, {8,8,8,24}, {8,8,8,8}, 1, TYULONG, TYLONG, TYINT, &t_x86_64_sysv },
   { {ISaarch64, -1, -1}, {8,8,8,32}, {8,8,8,8}, 0, TYULONG, TYLONG, TYUINT, &t_aarch64_aapcs },
};

TargTriple target;
uchar targ_primsizes[TYPTR+1];
uchar targ_primalign[TYPTR+1];
uint targ_valistsize;
enum typetag targ_sizetype, targ_ptrdifftype, targ_wchartype;
bool targ_charsigned, targ_bigendian, targ_64bit;
enum mcarch targ_arch;
const struct MCTarg *mctarg;

static bool
matchstr(const char **s, const char *pat)
{
   const char *p;
   for (p = *s; *pat; ++p, ++pat) {
      if (*pat == '$') { if (*p) return 0; else break; }
      else if (*p != *pat) return 0;
   }
   *s = p;
   return 1;
}

static bool
parsetriple(TargTriple *trg, const char *str)
{
   if (matchstr(&str, "x86_64-") || matchstr(&str, "amd64-"))
      trg->arch = ISx86_64;
   else if (matchstr(&str, "aarch64-") || matchstr(&str, "arm64-"))
      trg->arch = ISaarch64;
   else return 0;

   if (matchstr(&str, "unknown-") || matchstr(&str, "pc-")) {}

   if (matchstr(&str, "linux$")) {
      trg->os = OSlinux;
      trg->abi = ABIgnu;
      return 1;
   } else if (matchstr(&str, "linux")) {
      trg->os = OSlinux;
   } else if (matchstr(&str, "openbsd")) {
      trg->os = OSopenbsd;
   } else return 0;

   if (trg->os == OSlinux && matchstr(&str, "-gnu")) {
      trg->abi = ABIgnu;
   } else if (trg->os == OSlinux && matchstr(&str, "-musl")) {
      trg->abi = ABImusl;
   } else if (matchstr(&str, "-")) {
      return 0;
   }

   return 1;
}

static const char *const ospredefs[] = {
   [OSlinux] = "__linux\0__linux__\0linux\0unix\0__unix\0__unix__\0__ELF__\0",
   [OSopenbsd] = "__OpenBSD__\0unix\0__unix\0__unix__\0__ELF__\0"
}, *archpredefs[] = {
   [ISx86_64] = "__x86_64__\0__x86_64\0__amd64__\0__amd64\0",
   [ISaarch64] = "__aarch64__\0__aarch64\0",
};

/* https://gcc.gnu.org/onlinedocs/cpp/Common-Predefined-Macros.html */
static void
putpredefmacros(void)
{
   for (const char *s = ospredefs[target.os]; s && *s; s = s + strlen(s)+1)
      cpp0define(s, NULL);
   for (const char *s = archpredefs[target.arch]; s && *s; s = s + strlen(s)+1)
      cpp0define(s, NULL);
   if (ccopt.pie)
      cpp0define("__pie__", NULL), cpp0define("__PIE__", NULL);
   if (ccopt.pic)
      cpp0define("__pic__", NULL), cpp0define("__PIC__", NULL);

   if (targ_primsizes[TYPTR] == 4)
      cpp0define("_ILP32", NULL), cpp0define("__ILP32__", NULL);
   else if (targ_primsizes[TYLONG] == 4)
      cpp0define("_LLP64", NULL), cpp0define("__LLP64__", NULL);
   else {
      assert(targ_primsizes[TYINT] == 4);
      cpp0define("_LP64", NULL), cpp0define("__LP64__", NULL);
   }

   if (!targ_charsigned) cpp0define("__CHAR_UNSIGNED__", NULL);
   /* __SIZE_TYPE__ & co. */
   int i64t = targ_primsizes[TYLONG] == 8 ? TYLONG : TYVLONG;
#define DEFTYP(T,tt) cpp0define("__"T"_TYPE__", primtypenames[tt]);
   DEFTYP("SIZE", targ_sizetype);
   DEFTYP("PTRDIFF", targ_ptrdifftype);
   DEFTYP("WCHAR", targ_wchartype);
   DEFTYP("WINT", targ_wchartype - isunsignedt(targ_wchartype));
   DEFTYP("INTMAX", i64t);
   DEFTYP("UINTMAX", i64t+1);
   DEFTYP("SIG_ATOMIC", TYINT);
   DEFTYP("INT8", TYSCHAR);
   DEFTYP("INT16", TYSHORT);
   DEFTYP("INT32", TYINT);
   DEFTYP("INT64", i64t);
   DEFTYP("UINT8", TYUCHAR);
   DEFTYP("UINT16", TYUSHORT);
   DEFTYP("UINT32", TYUINT);
   DEFTYP("UINT64", i64t+1);
   DEFTYP("INT_LEAST8", TYSCHAR);
   DEFTYP("INT_LEAST16", TYSHORT);
   DEFTYP("INT_LEAST32", TYINT);
   DEFTYP("INT_LEAST64", i64t);
   DEFTYP("UINT_LEAST8", TYUCHAR);
   DEFTYP("UINT_LEAST16", TYUSHORT);
   DEFTYP("UINT_LEAST32", TYUINT);
   DEFTYP("UINT_LEAST64", i64t+1);
   DEFTYP("INTPTR", targ_64bit ? i64t : TYINT);
   DEFTYP("UINTPTR", targ_64bit ? i64t : TYINT);
#undef DEFTYP
   static const char atoi[][3] = {[1]="1",[2]="2",[4]="4",[8]="8",[16]="16"};
#define TYPSIZ(T,n) cpp0define("__SIZEOF_"T"__", atoi[targ_primsizes[n]])
   TYPSIZ("INT", TYINT);
   TYPSIZ("LONG", TYLONG);
   TYPSIZ("LONG_LONG", TYVLONG);
   TYPSIZ("SHORT", TYSHORT);
   TYPSIZ("POINTER", TYPTR);
   TYPSIZ("FLOAT", TYFLOAT);
   TYPSIZ("DOUBLE", TYDOUBLE);
   TYPSIZ("LONG_DOUBLE", TYLDOUBLE);
   TYPSIZ("SIZE_T", targ_sizetype);
   TYPSIZ("WCHAR_T", targ_wchartype);
   TYPSIZ("WINT_T", targ_wchartype);
   TYPSIZ("PTRDIFF_T", targ_ptrdifftype);

   cpp0define("__ORDER_LITTLE_ENDIAN__", "1234");
   cpp0define("__ORDER_BIG_ENDIAN__", "4321");
   cpp0define("__BYTE_ORDER__",
         targ_bigendian ? "__ORDER_BIG_ENDIAN__" : "__ORDER_LITTLE_ENDIAN__");
}

bool
targ_init(const char *starg, const TargTriple *dfault)
{
   const struct Targ *t = NULL;
   uchar *sizes = targ_primsizes, *align = targ_primalign;

   if (!starg) {
      target = *dfault;
   } else if (!parsetriple(&target, starg)) {
      fatal(NULL, "unrecognized target: %s", starg);
   }

   for (size_t i = 0; i < countof(targs); ++i) {
      if (targs[i].arch == target.arch)
      if (targs[i].oss & (1 << target.os))
      if (targs[i].abis & (1 << target.abi)) {
         t = &targs[i];
         break;
      }
   }
   if (!t) return 0;

   sizes[TYBOOL] = sizes[TYCHAR] = sizes[TYSCHAR] = sizes[TYUCHAR] = 1;
   sizes[TYSHORT] = sizes[TYUSHORT] = 2;
   sizes[TYUINT] = sizes[TYINT] = 4;
   sizes[TYFLOAT] = 4;
   sizes[TYDOUBLE] = 8;
   sizes[TYLDOUBLE] = 8;
   memcpy(align, sizes, sizeof targ_primalign);
   sizes[TYULONG] = sizes[TYLONG] = t->longsize;
   sizes[TYUVLONG] = sizes[TYVLONG] = t->vlongsize;
   sizes[TYPTR] = t->ptrsize;
   align[TYULONG] = align[TYLONG] = t->longalign;
   align[TYUVLONG] = align[TYVLONG] = t->vlongalign;
   align[TYDOUBLE] = t->doublealign;
   align[TYLDOUBLE] = t->doublealign;
   align[TYPTR] = t->ptralign;
   sizes[TYCOMPLEXF] = sizes[TYFLOAT]*2;
   sizes[TYCOMPLEX] = sizes[TYDOUBLE]*2;
   sizes[TYCOMPLEXL] = sizes[TYLDOUBLE]*2;
   align[TYCOMPLEXF] = align[TYFLOAT];
   align[TYCOMPLEX] = align[TYDOUBLE];
   align[TYCOMPLEXL] = align[TYLDOUBLE];
   targ_valistsize = t->valistsize;
   targ_sizetype = t->sizetype;
   targ_ptrdifftype = t->ptrdifftype;
   targ_wchartype = t->wchartype;
   targ_charsigned = t->charsigned;
   targ_bigendian = 0;
   targ_64bit = t->ptrsize == 8;
   mctarg = t->mctarg;
   targ_arch = ISx86_64;

   putpredefmacros();

   return 1;
}
