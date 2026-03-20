`antcc` is a small C compiler using its own independent backend.

Supports [most of C11 and some C23 features](doc/cstd.md), as well as some GNU extensions.

Currently still in a experimental stage, but can successfully build some
real-world C codebases such as Lua, SQLite,
[oksh](https://github.com/ibara/oksh), [tin](http://www.tin.org/) and itself.

`antcc` is inspired by other small C compilers like
[tcc](https://bellard.org/tcc/),
[cproc](https://git.sr.ht/~mcf/cproc),
[chibicc](https://github.com/rui314/chibicc),
and backends like [QBE](https://c9x.me/compile/) and [LLVM](https://llvm.org/).

## Requirements

`antcc` is written in standard C11 and can be built with any conforming
compiler toolchain.  The `Makefile` requires GNU Make.

## Building

Run `./configure` to create `hostconfig.h` and `config.mk` for your system.

Build with

```
make
#or
make opt #compile with -O2
#or
make dbg #compile with UBsan and Asan
```

Install with `(sudo) make install`.

## Supported targets

For now just x86-64 POSIX (Sys-V + ELF). aarch64 backend is in the works.  Tested and known to work:

 - `x86_64-linux-gnu`
 - `x86_64-linux-musl`

## Usage

The driver is still incomplete but it mimics that of compilers like gcc, see `--help`.
`antcc` compiles translation units to object files directly, but the driver
will invoke an external command to link to an executable if `-c` isn't passed.

Cross-compilation is partially supported: cross-compiling object files works
but an external cross-compiling toolchain for linking is required; the driver
will try to find one (invoking e.g. `aarch64-linux-gnu-gcc`, or falling back
to [`zig cc`](https://andrewkelley.me/post/zig-cc-powerful-drop-in-replacement-gcc-clang.html)),
and appropiate include paths must be manually specified. You can specify the compiler target architecture with `-target <triple>`.

## Testing

`bootstrap.sh` will bootstrap the compiler in 3 stages:
  - Stage 0 builds the compiler with the system's C compiler
  - Stage 1 builds the compiler with the stage 0 output
  - Stage 2 builds the compiler with the stage 1 output
  - Then stage 1 and 2 outputs are verified to be identical

There are tests in the `test` directory:
  - `test/run.sh`: local tests
  - `test/lua.sh`: compile Lua 5.4.0 and run its testsuite
  - `test/c-testsuite.sh`: run [c-testsuite](https://github.com/c-testsuite/c-testsuite)
  - `test/sqlite.sh`: compile SQLite and run its testsuite (must pull in external sqlite submodule: `git submodule update --init --recursive`)
  - `test/metalang99.sh`: compile and run [metalang99](https://github.com/hirrolot/metalang99) tests (preprocessor stress testing)

## Issues and contributing

You can report issues on the [issue tracker](https://codeberg.org/lsof/antcc/issues).

Contributions are welcome as long as they aren't low-effort AI slop, send as pull requests [on Codeberg](https://codeberg.org/lsof/antcc/pulls).

## Internals & Design

C type representation (`c_type.h` & `c_type.c`) is shared by the frontend and
backend because the backend is responsible for ABI-specific lowering of calling
conventions.

The C frontend is structured like so:

  - Compiler driver (`a_main.c`), which parses command line options, inputs and
    outputs and calls out to the core compiler to build individual object
    files and possibly invoke an external command to link them together.

  - Tokenizer & preprocessor (`c_lex.c`): The input file is scanned on-demand,
    initially reading characters into an internal buffer after performing
    backslash-newline delition (and possibly trigraph substitution), then producing
    one token at a time when the parser requests the next one. Preprocessing
    (directives & macro expansion) is also done on the fly.

  - Parser & IR generation (`c.c`): The handwritten parser reads declarations
    and keeps them in a symbol table/environment. Static data is written to
    buffers that correspond to the .rodata/.data sections of the final object
    file, emitting relocations to the object file interface too. Function
    bodies are parsed and transformed into the IR in one pass.  Expressions are
    parsed into expression trees before being emitted or compile-time evaluated
    (`c_eval.c`), but there is no whole-program AST.  When the end of a
    function definition is reached, the backend is called to perform all of the
    passes that will finally transform it into machine code written to the
    .text section.

The backend (`ir_*`) uses an IR in Static Single Assignment (SSA) form.
Instructions have a return type and up to two operands. Because of SSA form,
temporaries are simply referenced by the instruction that provides their
definition, so an explicit output operand is not required. The list of
instructions is defined in `ir_op.def`. Each basic block in the control flow
graph consists of 0 or more phi functions, followed by 0 or more instructions,
terminated by a jump (unconditional/conditional branch, return, or trap).

The builder API (`ir_builder.c`) used by the frontend performs peephole
optimizations on the fly, mainly constant folding.

Object file interface routines are in `obj.[c/h]` ELF implementation in
`o_elf.[c/h]`. Support for other object formats like PE and Mach-O is planned.
Debug information in the form of DWARF is also planned, but it is a sizeable
undertaking.

The `-d...` compiler flag can be used to print the output of different stages
of the backend for debugging.

The backend performs the following main passes:

  - ABI lowering (`ir_abi0.c`, `t_x86-64_sysv.c`): implements target calling
    convention details, such as lowering structures being passed/returned by
    value in registers or the stack.
  - Intrinsics lowering (`ir_intrin.c`): lowers some intrinsics emitted by the
    frontend (currently just structcopy)
  - mem2reg (`ir_mem2reg.c`): lower stack slots into SSA temporaries. This is
    an important pass because the frontend puts every C variable into a stack
    slot, and this pass transforms those into temporaries and phi instructions
    in SSA form instructions when possible (most of the time, unless they are
    aggregates or their address is taken), which is also how clang/LLVM does
    it. Can be disabled with -O0.
  - With -O1+ optimizations enabled
      + inlining (`ir_inliner.c`)
      + common-subexpression elimination (`ir_cse.c`),
      + general arithmetic simplifications, branch simplification
          (`ir_simpl.c`)

  - Stack lowering (`ir_stack.c`): `alloca` instructions are deleted and
    corresponding stack slots replaced with calculated stack offsets.
  - Instruction selection (`t_x86-64_isel.c`): architecture-specific
    instruction selection, addressing mode utilization, introduction of
    register constraints.
  - Register allocation (`ir_regalloc.c`): performs linear scan register
    allocation. A scratch register is reserved for operations with spilled
    temporaries.
  - Code emission (`t_x86-64_emit.c`): binary code for the target architecture is
    emitted directly (not textual assembly). Relocations are deferred to the
    object file interface too.

[ ... ]
