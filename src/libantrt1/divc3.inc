/** Implementation from C11(N1570) Annex G **/

TYPE complex
FUNC(TYPE complex z, TYPE complex w) {
   // #pragma STDC FP_CONTRACT OFF // we don't do this
   TYPE a, b, c, d, logbw, denom, x, y;
   a = creal(z), b = cimag(z);
   c = creal(w), d = cimag(w);
   logbw = logb(fmax(fabs(c), fabs(d)));
   int ilogbw = 0;
   if (isfinite(logbw)) {
      ilogbw = (int)logbw;
      c = scalbn(c, -ilogbw);
      d = scalbn(d, -ilogbw);
   }
   denom = c * c + d * d;
   x = scalbn((a * c + b * d) / denom, -ilogbw);
   y = scalbn((b * c - a * d) / denom, -ilogbw);
   /* Recover infinities and zeros that computed as NaN+iNaN;
    * the only cases are nonzero/zero, infinite/finite, and finite/infinite, ...  */
   if (isnan(x) && isnan(y)) {
      if ((denom == 0.0) && (!isnan(a) || !isnan(b))) {
         x = copysign(INFINITY, c) * a;
         y = copysign(INFINITY, c) * b;
      } else if ((isinf(a) || isinf(b)) && isfinite(c) && isfinite(d)) {
         a = copysign(isinf(a) ? 1.0 : 0.0, a);
         b = copysign(isinf(b) ? 1.0 : 0.0, b);
         x = INFINITY * (a * c + b * d);
         y = INFINITY * (b * c - a * d);
      } else if ((logbw == INFINITY) && isfinite(a) && isfinite(b)) {
         c = copysign(isinf(c) ? 1.0 : 0.0, c);
         d = copysign(isinf(d) ? 1.0 : 0.0, d);
         x = 0.0 * (a * c + b * d);
         y = 0.0 * (b * c - a * d);
      }
   }
   return __builtin_complex(x, y);
}
