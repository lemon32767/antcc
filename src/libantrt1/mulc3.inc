/** Implementation from C11(N1570) Annex G **/

// #pragma STDC FP_CONTRACT OFF // we don't do this

TYPE complex
FUNC(TYPE complex z, TYPE complex w) {
   TYPE a, b, c, d, ac, bd, ad, bc, x, y;
   a = creal(z), b = cimag(z);
   c = creal(w), d = cimag(w);
   ac = a * c, bd = b * d;
   ad = a * d, bc = b * c;
   x = ac - bd, y = ad + bc;
   if (isnan(x) && isnan(y)) {
      /* Recover infinities that computed as NaN+iNaN ... */
      int recalc = 0;
      if (isinf(a) || isinf(b)) { // z is infinite
         /* "Box" the infinity and change NaNs in the other factor to 0 */
         a = copysign(isinf(a) ? 1.0 : 0.0, a);
         b = copysign(isinf(b) ? 1.0 : 0.0, b);
         if (isnan(c))
            c = copysign(0.0, c);
         if (isnan(d))
            d = copysign(0.0, d);
         recalc = 1;
      }
      if (isinf(c) || isinf(d)) { // w is infinite
         /* "Box" the infinity and change NaNs in the other factor to 0 */
         c = copysign(isinf(c) ? 1.0 : 0.0, c);
         d = copysign(isinf(d) ? 1.0 : 0.0, d);
         if (isnan(a))
            a = copysign(0.0, a);
         if (isnan(b))
            b = copysign(0.0, b);
         recalc = 1;
      }
      if (!recalc && (isinf(ac) || isinf(bd) || isinf(ad) || isinf(bc))) {
         /* Recover infinities from overflow by changing NaNs to 0 ... */
         if (isnan(a))
            a = copysign(0.0, a);
         if (isnan(b))
            b = copysign(0.0, b);
         if (isnan(c))
            c = copysign(0.0, c);
         if (isnan(d))
            d = copysign(0.0, d);
         recalc = 1;
      }
      if (recalc) {
         x = INFINITY * (a * c - b * d);
         y = INFINITY * (a * d + b * c);
      }
   }
   return __builtin_complex(x, y);
}
