#include "t_aarch64.h"

/* Ref: https://github.com/ARM-software/abi-aa/blob/2025Q4/aapcs64/aapcs64.rst */

static bool
hfa_scalar(enum typetag *hfa_t, enum typetag t)
{
   if (!isfltt(t)) return 0;
   if (!*hfa_t) *hfa_t = t;
   else if (*hfa_t != t) return 0;
   return 1;
}

static bool cls_hfa(enum typetag *, const TypeData *td);

static bool
hfa_arr(enum typetag *hfa_t, Type ty)
{
   Type chld = typechild(ty);
   if (isagg(chld))
      return cls_hfa(hfa_t, &typedata[chld.dat]);
   if (chld.t == TYARRAY)
      return hfa_arr(hfa_t, chld);
   return hfa_scalar(hfa_t, scalartypet(chld));
}

static bool
cls_hfa(enum typetag *hfa_t, const TypeData *td)
{
   assert(isaggt(td->t));
   for (int i = 0; i < td->nmemb; ++i) {
      FieldData *fld = &td->fld[i].f;
      if (isagg(fld->t)) {
         if (!cls_hfa(hfa_t, &typedata[fld->t.dat]))
            return 0;
      } else if (fld->t.t == TYARRAY) {
         if (isincomplete(fld->t)) continue;
         if (!hfa_arr(hfa_t, fld->t))
            return 0;
      } else {
         if (!hfa_scalar(hfa_t, scalartypet(fld->t)))
            return 0;
      }
   }
   return 1;
}

static enum irclass
classify(const TypeData *td)
{
   if (td->siz > 16) return 0;
   enum typetag hfa_t = 0;
   return cls_hfa(&hfa_t, td) ? type2cls[hfa_t] : (td->siz > 4 ? KI64 : KI32);
}

/* XXX types with alignment >= 16 */

static int
abiarg(short r[2], uchar cls[2], uchar *r2off, int *ni, int *nf, int *ns, IRType typ)
{
   enum { NINT = 8, NFLT = 8 };
   if (!typ.isagg) {
      if (kisflt(cls[0] = typ.cls) && *nf < 8) {
         r[0] = V(0) + (*nf)++;
      } else if (kisint(cls[0]) && *ni < NINT) {
         r[0] = R0 + (*ni)++;
      } else { /* passed on the stack */
         r[0] = *ns;
         *ns += 8;
         return 0;
      }
      return 1;
   }
   cls[0] = cls[1] = 0;
   enum irclass k = classify(&typedata[typ.dat]);
   if (!k) { /* copied to caller memory and passed as a pointer */
      cls[0] = KPTR;
      if (*ni < NINT) { /* in a gpr */
         r[0] = R0 + (*ni)++;
         return 1;
      } else { /* that pointer, passed on the stack */
         r[0] = *ns;
         *ns += 8;
         return -1;
      }
   }
   *r2off = cls2siz[k];
   int n;
   uint tsiz = typedata[typ.dat].siz;
   if (kisflt(k)) { /* HFAA ([1..4]f32 or [1..2]f64) */
      n = tsiz / cls2siz[k];
      assert(n <= 2 && "oops");
      if (n <= NFLT - *nf) {
         for (int i = 0; i < n; ++i) {
            r[i] = V(0) + *nf + i;
            cls[i] = k;
         }
         *nf += n;
      } else { /* stack */
         *nf = NFLT;
      Stack:
         r[0] = *ns;
         *ns = alignup(*ns + tsiz, 8);
         r[1] = -1;
         return cls[0] = cls[1] = 0;
      }
   } else { /* Composite Type <= 16 bytes */
      n = 1 + (tsiz > 8);
      if (n <= NINT - *ni) {
         r[0] = R0 + *ni;
         if (n > 1) r[1] = r[0] + 1;
         *ni += n;
         cls[0] = tsiz > 4 ? KI64 : KI32;
         if (n > 1) cls[1] = KI64;
      } else {
         *ni = NINT;
         goto Stack;
      }
   }
   return n;
}

static int
abiret(short r[2], uchar cls[2], uchar *r2off, int *_ni, IRType typ)
{
   if (!typ.isagg) {
      r[0] = kisflt(cls[0] = typ.cls) ? V(0) : R0;
      return 1;
   }
   int ni = 0, nf = 0, ns = 0;
   int ret = abiarg(r, cls, r2off, &ni, &nf, &ns, typ);
   if (ret && cls[0] != KPTR) /* in regs */
      return ret;
   /* caller-allocated result address in x8 */
   r[0] = -1;
   r[1] = R(8);
   return 0;
}

static void
vastart(Function *fn, Block *blk, int *curi)
{
   assert(!"nyi");
}

static void
vaarg(Function *fn, Block *blk, int *curi)
{
   assert(!"nyi");
}

static const char aarch64_rnames[][6] = {
    "R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7", "R8", "R9","R10","R11","R12","R13","R14","R15",
   "R16","R17","R18","R19","R20","R21","R22","R23","R24","R25","R26","R27","R28", "FP", "LR", "SP",
    "V0", "V1", "V2", "V3", "V4", "V5", "V6", "V7", "V8", "V9","V10","V11","V12","V13","V14","V15",
   "V16","V17","V18","V19","V20","V21","V22","V23","V24","V25","V26","V27","V28","V29","V30","V31",
};

const MCTarg t_aarch64_aapcs = {
   .gpr0 = R0, .ngpr = 31,
   .bpr = FP,
   .gprscratch = R(16), .fprscratch = V(31),
   .fpr0 = V0, .nfpr = 32,
   .rcallee = BIT(R(19)) | BIT(R(20)) | BIT(R(21)) | BIT(R(22)) | BIT(R(23))
            | BIT(R(24)) | BIT(R(25)) | BIT(R(26)) | BIT(R(27)) | BIT(R(28))
            | BIT( V(8)) | BIT( V(9)) | BIT(V(10)) | BIT(V(11)) | BIT(V(12))
            | BIT(V(13)) | BIT(V(14)) | BIT(V(15)),
   .rglob = BIT(FP) | BIT(LR) | BIT(SP),
   .rnames = aarch64_rnames,
   .objkind = OBJELF,
   .abiret = abiret,
   .abiarg = abiarg,
   .vastart = vastart,
   .vaarg = vaarg,
   .isel = aarch64_isel,
   .emit = aarch64_emit,
};

/* vim:set ts=3 sw=3 expandtab: */
