A list of **missing** standard C features:

## C89
  - trigraphs (deliberately left out)

## C99
  - Proper `long double` support in platforms with extended floats (currently equivalent to `double`)
  - `<tgmath.h>` header
  - digraphs
  - Universal character names (`\uXXXX`, `\UXXXXXXXX`)
  - IEEE 754 float support Annex F IEC 60559 (`FLT_EVAL_METHOD`, `FENV_ACCESS` pragma) (not even GCC or Clang care about this)
Stack-allocated VLAs are a stub and actually compile to a fixed size array at the moment (but noone should use them anyway)

## C11
  - `_Alignas`
  - Multithreading support (`_Thread_local`, atomics)
  - `u8".."` string literals

## C23
  - Decimal floating-point types (`_Decimal32`, `_Decimal64`, and `_Decimal128`)
  - Bit-precise integers (`_BitInt`)
  - `nullptr`, `nullptr_t`
  - Binary integer constants
  - `char8_t`
  - Digit separator '
  - Attributes (`[[...]]` syntax)
  - Labels followed by declarations and }
  - `auto` for type inference, `typeof_unqual`, `constexpr`
  - `unreachable` macro in `<stddef.h>`
  - checked int arithmetic (`<stdckdint.h>`)
  - tagged type compatibility [N3037](https://www.open-std.org/jtc1/sc22/wg14/www/docs/n3037.pdf)
  - storage class specifier for compound literals
  - preprocessor `#embed` directive, `__has_include`, `__has_c_attribute`
  - Pragmas for float rounding direction (`STDC FENV_ROUND`, `STDC FENV_DEC_ROUND`)
  - IEEE 754 decimal floats, interchange and extended types. various `<float.h>` macros,...
