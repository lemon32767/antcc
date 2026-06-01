`__attribute__`s
----------------
  
GNU-like attribute [syntax](https://gcc.gnu.org/onlinedocs/gcc-4.2.1/gcc/Attribute-Syntax.html#Attribute-Syntax). Implemented:

  - `aligned(alignment)`, `packed`
  - `noreturn` (treated as C11 `_Noreturn`)

Other attributes for correctness hints/diagnostics are recognized (`format`,
`nonnull`, etc) but discarded.

`__builtin` functions
---------------------

  - variadic arguments: `__builtin_va_start/copy/end/arg`, and `__builtin_va_list` type
  - `__builtin_trap`
  - `__builtin_bswap16/32/64`

Other GNU C language extensions
-------------------------------

  - Statement expressions `({ ... })`
  - `__typeof__`, `__alignof__`
  - `__asm__` for symbol renaming
  - zero-sized arrays, empty structs/unions
  - `return`ing void expression in void function
  - forward-declared enums
  - `__FUNCTION__`, `__PRETTY_FUNCTION__` as synonyms for `__func__`
  - applying `_Alignof` to an expression
  - some constant-foldable expressions in contexts where integer constant expressions are required
  - `'\e'` escape sequence
  - Various alternate keywords: `__const`, `__inline`, [etc](/src/keywords.def)

Preprocessor extensions
-----------------------

  - `#include_next`
  - `#pragma once`, `#warning`
  - `__has_include`/`__has_include_next`
  - `__has_attribute`/`__has_builtin`
  - `__COUNTER__`, `__VA_OPT__`, `##__VA_ARGS__` comma swallowing
  - named variadic macro arguments `#define mac(args...)`
  - alternate `#line` directives: `# `*line-number* *filename*
  - embedding preprocessor directives within macro arguments
