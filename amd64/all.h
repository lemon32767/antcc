#include "../ir.h"

#define LIST_REGS(_)                                               \
   _(RAX) _(RCX) _(RDX) _(RBX) _(RSP) _(RBP) _(RSI) _(RDI)         \
   _(R8) _(R9) _(R10) _(R11) _(R12) _(R13) _(R14) _(R15)           \
   _(XMM0) _(XMM1) _(XMM2) _(XMM3) _(XMM4) _(XMM5) _(XMM6) _(XMM7) \
   _(XMM8) _(XMM9) _(XMM10) _(XMM11) _(XMM12) _(XMM13) _(XMM14) _(XMM15)

enum reg {
#define R(r) r,
   LIST_REGS(R)
#undef R
};

static inline void
wr32le(uchar *p, uint x)
{
   p[0] = x >>  0; p[1] = x >>  8;
   p[2] = x >> 16; p[3] = x >> 24;
}

static inline void
wr64le(uchar *p, uvlong x)
{
   wr32le(p+0, x>>00);
   wr32le(p+4, x>>32);
}

void amd64_isel(struct function *);
void amd64_emit(struct function *);

/* vim:set ts=3 sw=3 expandtab: */
