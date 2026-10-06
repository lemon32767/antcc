#include "t_aarch64.h"

/* Ref: https://github.com/ARM-software/abi-aa/blob/2025Q4/aapcs64/aapcs64.rst */


static bool
hfa_scalar(enum typetag *hfa_t, Type t) {
   enum typetag tt;
   if (isflt(t)) tt = scalartypet(t);
   else if (iscomplex(t)) tt = t.t - TYCOMPLEXF + TYFLOAT;
   else return 0;
   if (!*hfa_t) *hfa_t = tt;
   else if (*hfa_t != tt) return 0;
   return 1;
}

static bool cls_hfa(enum typetag *, const TypeData *td);

static bool
hfa_arr(enum typetag *hfa_t, Type ty) {
   Type chld = typechild(ty);
   if (isagg(chld))
      return cls_hfa(hfa_t, &typedata[chld.dat]);
   if (chld.t == TYARRAY)
      return hfa_arr(hfa_t, chld);
   return hfa_scalar(hfa_t, chld);
}

static bool
cls_hfa(enum typetag *hfa_t, const TypeData *td) {
   assert(isaggt(td->t));
   for (int i = 0; i < td->nmemb; ++i) {
      FieldData *fld = &td->fld[i].f;
      if (fld->bitf && !fld->bitsiz) continue;
      if (isagg(fld->t)) {
         if (!cls_hfa(hfa_t, &typedata[fld->t.dat]))
            return 0;
      } else if (fld->t.t == TYARRAY) {
         if (isincomplete(fld->t)) continue;
         if (!hfa_arr(hfa_t, fld->t))
            return 0;
      } else {
         if (!hfa_scalar(hfa_t, fld->t))
            return 0;
      }
   }
   return 1;
}

static enum irclass
classify(const TypeData *td) {
   if (td->siz > 16) return 0;
   enum typetag hfa_t = 0;
   return cls_hfa(&hfa_t, td) ? type2cls[hfa_t] : (td->siz > 4 ? KI64 : KI32);
}

/* XXX types with alignment >= 16 */

static int
abiarg(short r[2], uchar cls[2], uchar *r2off, int *ni, int *nf, int *ns, IRType typ) {
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
abiret(short r[2], uchar cls[2], uchar *r2off, int *_ni, IRType typ) {
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

/* Layout of va_list:
 * struct {
 *    ( 0) void *stack;
 *    ( 8) void *gr_top;
 *    (16) void *vr_top;
 *    (24) int gr_offs;
 *    (28) int vr_offs;
 * }
 *
 */
enum {
   VL_STACK_OFF   = 0,
   VL_GR_TOP_OFF  = 8,
   VL_VR_TOP_OFF  = 16,
   VL_GR_OFFS_OFF = 24,
   VL_VR_OFFS_OFF = 28,
};

/* !!keep in sync with emit()'s xvaprologue */
static void
vastart(Function *fn, Block *blk, int *curi) {
   int named_gr = 0, named_vr = 0, named_stk = 0;
   Instr *ins = &instrtab[blk->ins.p[*curi]];
   Ref ap = ins->l, dst, src;
   assert(ins->op == Ovastart);
   for (int i = 0; i < fn->nabiarg; ++i) {
      ABIArg abi = fn->abiarg[i];
      if (!abi.isstk) {
         if (abi.reg >= V0) ++named_vr;
         else ++named_gr;
      } else {
         named_stk = abi.stk + 8;
      }
   }
   int grtop = (8-named_gr)*8;
   int vrtop = alignup(grtop, 16) + (8-named_vr)*16;
   Ref rsave;
   if (fn->entry->ins.n > 1 && instrtab[fn->entry->ins.p[1]].op == Oxvaprologue) {
      rsave = mkref(RTMP, fn->entry->ins.p[0]);
      assert(instrtab[rsave.i].op == Oalloca16);
   } else {
      rsave = insertinstr(fn->entry, 0, mkalloca(vrtop, 16));
      insertinstr(fn->entry, 1, (Instr){Oxvaprologue, 0, .keep=1, .l=rsave});
   }
   /* set ap->gr_top */
   *ins = mkinstr2(Oadd, KPTR, ap, mkref(RICON, VL_GR_TOP_OFF));
   dst = mkref(RTMP, ins - instrtab);
   int i = *curi + 1;
   src = insertinstr(blk, i++, mkinstr2(Oadd, KPTR, rsave, mkref(RICON, grtop)));
   insertinstr(blk, i++, mkinstr2(Ostorei64, 0, dst, src));
   /* set ap->vr_top */
   src = insertinstr(blk, i++, mkinstr2(Oadd, KPTR, rsave, mkref(RICON, vrtop)));
   dst = insertinstr(blk, i++, mkinstr2(Oadd, KPTR, ap, mkref(RICON, VL_VR_TOP_OFF)));
   insertinstr(blk, i++, mkinstr2(Ostorei64, 0, dst, src));
   /* set ap->stack */
   src = insertinstr(blk, i++, mkinstr1(Ocopy, KPTR, mkref(RSTACK, -named_stk-8)));
   dst = ap; /* VL_STACK_OFF = 0 */
   insertinstr(blk, i++, mkinstr2(Ostorei64, 0, dst, src));
   /* set ap->gr_offs */
   dst = insertinstr(blk, i++, mkinstr2(Oadd, KPTR, ap, mkref(RICON, VL_GR_OFFS_OFF)));
   insertinstr(blk, i++, mkinstr2(Ostorei32, 0, dst, mkref(RICON, -((8 - named_gr) * 8))));
   /* set ap->vr_offs */
   dst = insertinstr(blk, i++, mkinstr2(Oadd, KPTR, ap, mkref(RICON, VL_VR_OFFS_OFF)));
   insertinstr(blk, i++, mkinstr2(Ostorei32, 0, dst, mkref(RICON, -((8 - named_vr) * 16))));
   *curi = i-1;
}

static void
vaarg(Function *fn, Block *blk, int *curi) {
   short r[2];
   uchar cls[2];
   int ngr = 0, nvr = 0, nstk = 0;
   uchar r2off;
   int var = blk->ins.p[*curi];
   Ref ap = instrtab[var].l;
   IRType ty = ref2type(instrtab[var].r);
   uint typsiz = ty.isagg ? typedata[ty.dat].siz : 8;

   assert(instrtab[var].op == Ovaarg);
   blk->ins.p[*curi] = newinstr(blk, (Instr){Onop});
   int ret = abiarg(r, cls, &r2off, &ngr, &nvr, &nstk, ty);

   /* https://github.com/ARM-software/abi-aa/blob/2025Q4/aapcs64/aapcs64.rst#144the-va_arg-macro */
   int regwidth = nvr ? 16 : 8,
       xr_offs_off = nvr ? VL_VR_OFFS_OFF : VL_GR_OFFS_OFF,
       xr_top_off = nvr ? VL_VR_TOP_OFF : VL_GR_TOP_OFF;
   Ref poffs, tmp, offs, args[2];
   Block *end, *on_stack, *regsave1;
   if (ret > 0) { /* uses regs. not HFA */
      assert((ngr > 0) ^ (nvr > 0)); /* either all GRs or all VRs */
      int nreg = ngr | nvr;
      end = blksplitafter(fn, blk, *curi);
      blk->jmp.t = 0;
      end->npred = 0;
      useblk(fn, blk);
      poffs = irbinop(fn, Oadd, KPTR, ap, mkref(RICON, xr_offs_off));
      offs = addinstr(fn, mkinstr1(Oloads32, KI64, poffs)); /* offs = ap->Xr_offs */
      tmp = irbinop(fn, Olth, KI32, offs, ZEROREF); /* reg save area empty? */
      putcondbranch(fn, tmp, newblk(fn), on_stack = newblk(fn));
      useblk(fn, blk->s1);
      /* XXX alignof > 8 */
      tmp = irbinop(fn, Oadd, KI64, offs, mkref(RICON, nreg*regwidth)); /* offs + nreg*W */
      addinstr(fn, mkinstr2(Ostorei32, 0, poffs, tmp));
      tmp = irbinop(fn, Ogth, KI64, tmp, ZEROREF);
      putcondbranch(fn, tmp, on_stack, regsave1 = newblk(fn)); /* overflowed reg save area? */
      useblk(fn, regsave1);
      /* XXX big endian */
      tmp = irbinop(fn, Oadd, KPTR, ap, mkref(RICON, xr_top_off));
      tmp = addinstr(fn, mkinstr1(Oloadi64, KPTR, tmp));
      tmp = irbinop(fn, Oadd, KPTR, tmp, offs); /* ap->Xr_top + offs */
      args[0] = tmp;
      putbranch(fn, end);
   } else {
      assert(ty.isagg);
      Type typ = mktype(typedata[ty.dat].t, .dat = ty.dat);
      fatal(NULL, "NYI: va_arg for '%ty' (#gr=%d,#vr=%d,#stk=%d)", typ, ngr, nvr, nstk);
   }
   useblk(fn, on_stack);
   {
      Ref arg = addinstr(fn, mkinstr1(Oloadi64, KPTR, ap));
      /* XXX alignof > 8 */
      tmp = irbinop(fn, Oadd, KPTR, arg, mkintcon(KPTR, typsiz + 7));
      tmp = irbinop(fn, Oand, KPTR, tmp, mkintcon(KPTR, -8));
      addinstr(fn, mkinstr2(Ostorei64, 0, ap, tmp)); /* ap->stack = (arg + sizeof(type)+7) & -8 */
      args[1] = arg;
      putbranch(fn, end);
   }
   assert(end->npred == 2);
   vpush(&end->ins, 0);
   memmove(end->ins.p+1, end->ins.p, (end->ins.n-1)*sizeof *end->ins.p);
   end->ins.p[0] = var;
   Ref phi = insertphi(end, KPTR);
   memcpy(phiargs(phi.i), args, sizeof args);
   fn->prop &= ~FNUSE;
   if (!ty.isagg) {
      /* *(type *)arg */
      instrtab[var] = mkinstr1(cls2load[cls[0]], cls[0], phi);
   } else {
      instrtab[var] = mkinstr1(Ocopy, KPTR, phi);
   }
}

static const char aarch64_rnames[][6] = {
    "R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7", "R8", "R9","R10","R11","R12","R13","R14","R15",
   "R16","R17","R18","R19","R20","R21","R22","R23","R24","R25","R26","R27","R28", "FP", "LR", "SP",
    "V0", "V1", "V2", "V3", "V4", "V5", "V6", "V7", "V8", "V9","V10","V11","V12","V13","V14","V15",
   "V16","V17","V18","V19","V20","V21","V22","V23","V24","V25","V26","V27","V28","V29","V30","V31",
};

const MCTarg t_aarch64_aapcs = {
   .gpr0 = R0, .ngpr = 31,
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
