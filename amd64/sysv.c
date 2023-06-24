#include "all.h"


static int classify(uchar cls[2], const struct typedata *td, uint off);
static int
classifyarr(uchar cls[2], union type ty, uint off)
{
   union type chld = typechild(ty);
   uint n = typearrlen(ty), siz = typesize(chld);
   assert(n > 0);
   for (uint i = 0; i < n; ++i) {
      uint offx = off + i * siz;
      if (isagg(chld)) {
         if (!classify(cls, &typedata[chld.dat], offx))
            return cls[0] = cls[1] = 0;
      } else if (chld.t == TYARRAY) {
         if (!classifyarr(cls, chld, offx))
            return cls[0] = cls[1] = 0;
      } else if (isflt(chld)) { /* SSE */
         if (!cls[offx/8])
            cls[offx/8] = KF8;
      } else { /* INTEGER */
         assert(isint(chld) || chld.t == TYPTR);
         cls[offx/8] = KI8;
      }
   }
   return !!cls[0] + !!cls[1];
}
static int
classify(uchar cls[2], const struct typedata *td, uint off)
{
   uint siz = alignup(td->siz, 8);
   if (siz > 16) /* MEMORY */
      return 0;
   for (int i = 0; i < td->nmemb; ++i) {
      struct fielddata *fld = &td->fld[i].f;
      uint align = typealign(fld->t);
      if (alignup(fld->off, align) != fld->off) /* unaligned field -> MEMORY */
         return cls[0] = cls[1] = 0;
      if (isagg(fld->t)) {
         if (!classify(cls, &typedata[fld->t.dat], off + fld->off))
            return cls[0] = cls[1] = 0;
      } else if (fld->t.t == TYARRAY) {
         if (isincomplete(fld->t)) continue;
         if (!classifyarr(cls, fld->t, off + fld->off))
            return cls[0] = cls[1] = 0;
      } else if (isflt(fld->t)) { /* SSE */
         if (!cls[(fld->off + off)/8])
            cls[(fld->off + off)/8] = KF8;
      } else { /* INTEGER */
         assert(isint(fld->t) || fld->t.t == TYPTR);
         cls[(fld->off +  off)/8] = KI8;
      }
   }
   return !!cls[0] + !!cls[1];
}

static int
abiarg(short r[2], uchar cls[2], int *ni, int *nf, int *ns, union irtype typ)
{
   static const uchar intregs[] = { RDI, RSI, RDX, RCX, R8, R9 };
   enum { NINT = arraylength(intregs), NFLT = 8 };
   int ret, ni_save, nf_save;

   if (!typ.isagg) {
      if (kisflt(cls[0] = typ.cls) && *nf < NFLT) {
         r[0] = XMM0 + (*nf)++;
      } else if (*ni < NINT) {
         r[0] = intregs[(*ni)++];
      } else {
         r[0] = -*ns - 16;
         *ns += 8;
         return 0; /* MEMORY */
      }
      return 1;
   }
   cls[0] = cls[1] = 0;
   ret = classify(cls, &typedata[typ.dat], 0);
   if (!ret) { /*MEMORY*/
      r[0] = -*ns - 16;
      *ns = alignup(*ns + typedata[typ.dat].siz, 8);
      return 0;
   }
   assert(ret <= 2);
   ni_save = *ni, nf_save = *nf;
   for (int i = 0; i < ret; ++i) {
      assert(cls[i]);
      if (cls[i] == KF8 && *nf < NFLT)
         r[i] = XMM0 + (*nf)++;
      else if (cls[i] == KI8 && *ni < NINT)
         r[i] = intregs[(*ni)++];
      else { /* MEMORY */
         *ni = ni_save, *nf = nf_save;
         r[0] = -*ns - 16;
         *ns = alignup(*ns + typedata[typ.dat].siz, 8);
         r[1] = -1;
         return cls[0] = cls[1] = 0;
      }
   }
   return ret;
}

static int
abiret(short r[2], uchar cls[2], int *ni, union irtype typ)
{
   int ret;

   if (!typ.isagg) {
      r[0] = kisflt(cls[0] = typ.cls) ? XMM0 : RAX;
      return 1;
   }

   cls[0] = cls[1] = 0;
   ret = classify(cls, &typedata[typ.dat], 0);
   if (!ret) { /* MEMORY */
      assert(*ni == 0);
      r[0] = RAX; /* on return should contain result location address */
      r[1] = RDI; /* register for caller-owned result location argument */
      ++*ni;
      return 0;
   }
   assert(ret <= 2);
   for (int i = 0, ni = 0, nf = 0; i < ret; ++i) {
      assert(cls[i]);
      if (cls[i] == KF8) /* SSE (XMM0, XMM1) */
         r[i] = XMM0 + nf++;
      else if (cls[i] == KI8) /* INTEGER (RAX, RDX) */
         r[i] = ni++ == 0 ? RAX : RDX;
      else assert(0);
   }
   return ret;
}

static const char amd64_rnames[][6] = {
#define R(r) #r,
   LIST_REGS(R)
#undef R
};

const struct mctarg t_amd64_sysv = {
   .gpr0 = RAX, .ngpr = R15 - RAX + 1,
   .bpr = RBP,
   .fpr0 = XMM0, .nfpr = XMM15 - XMM0 + 1,
   .rcallee = 1<<RBX | 1<<R12 | 1<<R13 | 1<<R14 | 1<<R15,
   .rglob = 1<<RSP | 1<<RBP,
   .rnames = amd64_rnames,
   .objkind = OBJELF,
   .isa = ISamd64,
   .abiret = abiret,
   .abiarg = abiarg,
   .isel = amd64_isel,
   .emit = amd64_emit
};

/* vim:set ts=3 sw=3 expandtab: */
