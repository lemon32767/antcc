antcc
=====

A small C compiler using its own independent backend. [Homepage and docs](https://386s.org/antcc/).

Supports [most of C11 and some C23 features](doc/cstd.md), as well as [some common extensions](doc/cext.md).

It can build real-world C codebases such as Lua, SQLite, git, curl, Tcl, DOOM...

Compilation speed is fast, and generated code efficiency falls somewhere in the
middle of `tcc` and `gcc -O1`.

`antcc` is inspired by other small C compilers like
[tcc](https://bellard.org/tcc/),
[cproc](https://git.sr.ht/~mcf/cproc),
[chibicc](https://github.com/rui314/chibicc),
and backends like [QBE](https://c9x.me/compile/) and [LLVM](https://llvm.org/).

Building/Installing
-------------------

Quick start:

```
git clone https://codeberg.org/lsof/antcc
cd antcc
./configure && make
sudo make install
```

`antcc` is written in C11 and can be built with any conforming compiler
toolchain that supports `#pragma once`. The driver uses POSIX APIs.

`./configure` creates `src/hostconfig.h` and `config.mk`. You may wish to customize these
for your system, or check `./configure --help`.

Other make targets:

```
make opt  #compiles with -O2
make dbg  #compiles with clang's UBSan and ASan
```

Supported targets
-----------------

x86-64, aarch64 (experimental); only POSIX targets (Sys-V ABI, ELF objects) are
supported at the moment. Support for other OSes, architectures is planned.

Tested and known to work:

 - `x86_64-linux-gnu`
 - `x86_64-linux-musl`
 - `x86_64-unknown-openbsd`
 - `x86_64-unknown-freebsd`

Semi-working (can self-host, but not passing the external test suites/exhaustively tested)

 - `aarch64-linux-gnu` (tested on QEMU)
 - `aarch64-unknown-linux-android24` (termux)


Usage
-----

The driver is still incomplete but it mimics that of compilers like gcc, see `--help`.
`antcc` compiles translation units to object files directly, and the driver
will invoke an external `ld` command to link to an executable if `-c` isn't passed.
For more information see the [manpage](https://386s.org/antcc/antcc.1.html).

Cross-compilation is supported in a best-effort basis, cross-compiling object files works
but an external cross-compiling toolchain for linking is required; the driver
will try to find one (invoking e.g. `aarch64-linux-gnu-gcc`, or falling back to
[`zig cc`](https://andrewkelley.me/post/zig-cc-powerful-drop-in-replacement-gcc-clang.html)),
and appropiate include paths may have to be manually provided. You can specify the
compiler target architecture with `-target <triple>`.

Contributing
------------

The easiest way to contribute is to try building C codebases with antcc and report any
issues you encounter.

For more information: [CONTRIBUTING.md](CONTRIBUTING.md).

Testing
-------

`bootstrap.sh` will bootstrap the compiler in 3 stages:

  - Stage 0 builds the compiler with the system's C compiler
  - Stage 1 builds the compiler with the stage 0 output
  - Stage 2 builds the compiler with the stage 1 output
  - Then stage 1 and 2 outputs are verified to be identical

There are tests in the `test` directory:

  - `test/c/run.sh`: local tests
  - `test/ir/run.sh`: IR unit tests
  - `test/c-testsuite.sh`: run [c-testsuite](https://github.com/c-testsuite/c-testsuite)
  - `test/c-smith.sh`: run [csmith](https://github.com/csmith-project/csmith) (fuzzing)
  - `test/external/lua.sh`: compile Lua 5.4.0 and run its testsuite
  - `test/external/sqlite.sh`: compile SQLite and optionally run its testsuite
  - `test/external/qbe.sh`: compile QBE and run its testsuite
  - `test/external/metalang99.sh`: compile and run [metalang99](https://github.com/hirrolot/metalang99) tests (preprocessor stress testing)

Internals & Design
------------------

See [doc/internals](doc/internals.md).
