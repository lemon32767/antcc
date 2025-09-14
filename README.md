`antcc` is a C compiler using its own custom backend. Currently still in a
experimental stage.

# Supported targets

- For now just x86-64 POSIX (Sys-V + ELF). Only tested on linux so far.

# Building

```
make   # outputs ./antcc executable
# or
make opt #compile with optimizations
# or
make dbg #compile with UBsan and Asan
```


# Usage

The driver is still incomplete but it mimics that of compilers like gcc.
`antcc` compiles translation units to object files directly, but the driver
will invoke an external linker command to output an executable if `-c` isn't passed.
