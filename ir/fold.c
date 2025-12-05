#include "ir.h"

static union ref
foldint(enum op op, enum irclass k, union ref lr, union ref rr)
{
   vlong x;
   union {
      vlong s;
      uvlong u;
   } l = {.s = intconval(lr)}, r = {.s = intconval(rr)};
   bool w = cls2siz[k] == 8;
   if (in_range(op, Odiv, Ourem)) assert(r.u != 0);
   switch (op) {
   case Ocopy: x = l.s; break;
   case Oneg:  x = -l.s; break;
   case Onot:  x = ~l.s; break;
   case Ocvtf32s: x = (int)(float)fltconval(lr); break;
   case Ocvtf32u: x = (uint)(float)fltconval(lr); break;
   case Ocvtf64s: x = (vlong)fltconval(lr); break;
   case Ocvtf64u: x = (uvlong)fltconval(lr); break;
   case Oexts8:  x = (schar)l.s; break;
   case Oextu8:  x = (uchar)l.s; break;
   case Oexts16: x = (short)l.s; break;
   case Oextu16: x = (ushort)l.s; break;
   case Oexts32: x = (int)l.s; break;
   case Oextu32: x = (uint)l.s; break;
   case Oadd:  x = l.u + r.u; break;
   case Osub:  x = l.u - r.u; break;
   case Omul:  x = l.u * r.u; break;
   case Odiv:  x = w ? l.s / r.s : (int)l.s / (int)r.s; break;
   case Oudiv: x = w ? l.u / r.u : (uint)l.u / (uint)r.u; break;
   case Orem:  x = w ? l.s % r.s : (int)l.s % (int)r.s; break;
   case Ourem: x = w ? l.u % r.u : (uint)l.u % (uint)r.u; break;
   case Oand:  x = l.u & r.u; break;
   case Oior:  x = l.u | r.u; break;
   case Oxor:  x = l.u ^ r.u; break;
   case Oshl:  x = l.u << (r.u & (w ? 63 : 31)); break;
   case Oslr:  x = w ? l.u >> (r.u&63) : (int)l.s >> (r.u&31); break;
   case Osar:  x = w ? l.s >> (r.u&63) : (int)l.s >> (r.u&31); break;
   case Oequ:  x = l.s == r.s; break;
   case Oneq:  x = l.s != r.s; break;
   case Olth:  x = l.s < r.s; break;
   case Ogth:  x = l.s > r.s; break;
   case Olte:  x = l.s <= r.s; break;
   case Ogte:  x = l.s >= r.s; break;
   case Oulth: x = l.u < r.u; break;
   case Ougth: x = l.u > r.u; break;
   case Oulte: x = l.u <= r.u; break;
   case Ougte: x = l.u >= r.u; break;
   default: assert(0);
   }
   return mkintcon(k, x);
}

static union ref
foldflt(enum op op, enum irclass k, union ref lr, union ref rr)
{
   int xi;
   double x, l = fltconval(lr), r = fltconval(rr);
   bool w = k == KF64;
   switch (op) {
   case Ocopy: x = l; break;
   case Oneg:  x = -l; break;
   case Ocvtf32f64: x = (float)l; break;
   case Ocvtf64f32: x = (float)l; break;
   case Ocvts32f:   x = (int)intconval(lr); break;
   case Ocvtu32f:   x = (int)intconval(lr); break;
   case Ocvts64f:   x = (vlong)intconval(lr); break;
   case Ocvtu64f:   x = (uvlong)intconval(lr); break;
   case Oadd: x = l + r; break;
   case Osub: x = l - r; break;
   case Omul: x = l * r; break;
   case Odiv: x = l / r; break;
   case Oequ: xi = l == r; break;
   case Oneq: xi = l != r; break;
   case Olth: xi = l < r; break;
   case Ogth: xi = l > r; break;
   case Olte: xi = l <= r; break;
   case Ogte: xi = l >= r; break;
   default: assert(0);
   }
   if (oiscmp(op)) return mkref(RICON, xi);
   if (!w) x = (float)x;
   return mkfltcon(k, x);
}

bool
foldbinop(union ref *to, enum op op, enum irclass k, union ref l, union ref r)
{
   if (!oisarith(op))
      return 0;
   if (!isnumcon(l) || !isnumcon(r)) return 0;
   if (in_range(op, Odiv, Ourem) && kisint(k) && intconval(r) == 0)
      return 0;
   if (kisint(k))
      *to = foldint(op, k, l, r);
   else
      *to = foldflt(op, k, l, r);
   return 1;
}

bool
foldunop(union ref *to, enum op op, enum irclass k, union ref a)
{
   if (!isnumcon(a)) return 0;
   if (op != Ocopy && !oisarith(op))
      return 0;
   if (kisint(k))
      *to = foldint(op, k, a, ZEROREF);
   else
      *to = foldflt(op, k, a, ZEROREF);
   return 1;
}

/* vim:set ts=3 sw=3 expandtab: */
